// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_reload.h"

#include "cluster.h"
#include "config_candidate.h"
#include "lease.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static uint32_t node_bit(size_t node_idx)
{
    return UINT32_C(1) << node_idx;
}

static unsigned int participant_count(uint32_t mask)
{
    unsigned int count = 0;
    while (mask)
    {
        count += mask & 1u;
        mask >>= 1;
    }
    return count;
}

static int participant_coordinator(uint32_t mask)
{
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (mask & node_bit(i))
            return (int)i;
    }
    return -1;
}

static bool participant_mask_valid(uint32_t mask)
{
    uint32_t valid_mask = g_state.cfg.node_count == LCS_MAX_NODES ?
        UINT32_MAX : (node_bit(g_state.cfg.node_count) - 1u);
    return mask && !(mask & ~valid_mask);
}

static bool participant_set_has_full_member(uint32_t mask)
{
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((mask & node_bit(i)) &&
            g_state.cfg.nodes[i].role == LCS_NODE_FULL)
            return true;
    }
    return false;
}

static uint32_t capture_online_participants(void)
{
    uint32_t mask = node_bit((size_t)g_state.self_index);
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i != g_state.self_index &&
            g_state.peers[i].conn_state == LCS_PEER_ESTABLISHED)
            mask |= node_bit(i);
    }
    return mask;
}

static bool participants_support_reload(uint32_t participant_mask)
{
    if (LCS_PEER_PROTO_VERSION < LCS_PROTO_FEATURE_RESOURCE_RELOAD)
        return false;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index ||
            !(participant_mask & node_bit(i)))
            continue;
        if (g_state.peers[i].conn_state != LCS_PEER_ESTABLISHED ||
            g_state.peers[i].protocol_version <
            LCS_PROTO_FEATURE_RESOURCE_RELOAD)
            return false;
    }
    return true;
}

static bool resource_requires_drain(size_t old_idx)
{
    const lcs_resource_config_t *old_resource = &g_state.cfg.resources[old_idx];
    int new_idx = lcs_config_resource_index(&g_state.config_reload.candidate, old_resource->name);
    return new_idx < 0 || !lcs_config_backend_equal(old_resource, &g_state.config_reload.candidate.resources[new_idx]);
}

static bool local_drain_complete(int epoll_fd)
{
    bool complete = true;
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        if (!resource_requires_drain(i))
            continue;
        resource_runtime_t *runtime = &g_state.resources[i];
        bool locally_owned = runtime->owner_node == g_state.self_index && runtime->owner_instance_id == g_state.instance_id;
        if (locally_owned && runtime->state != LCS_RES_STOPPED)
        {
            resources_release_local((int)i, epoll_fd);
            complete = false;
        }
        if (locally_owned || runtime->hook.pid > 0 ||
            runtime->vip_probe.pid > 0 || runtime->vip.pid > 0 ||
            runtime->service.pid > 0 || runtime->handoff_pending)
            complete = false;
    }
    return complete;
}

static void clear_peer_candidates(void)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    memset(reload->peer_loaded, 0, sizeof(reload->peer_loaded));
    memset(reload->peer_agreed, 0, sizeof(reload->peer_agreed));
    memset(reload->peer_ready, 0, sizeof(reload->peer_ready));
    memset(reload->peer_participant_mask, 0,
           sizeof(reload->peer_participant_mask));
    memset(reload->peer_voting_fingerprint, 0, sizeof(reload->peer_voting_fingerprint));
    memset(reload->peer_full_fingerprint, 0, sizeof(reload->peer_full_fingerprint));
}

static void reset_candidate_state(config_reload_runtime_t *reload)
{
    reload->loaded = false;
    reload->agreed = false;
    reload->drain_started = false;
    reload->ready = false;
    reload->commit_requested = false;
    reload->commit_broadcast = false;
    reload->catchup = false;
    reload->participant_mask = 0;
    reload->pending_participant_mask = 0;
    reload->commit_not_before_ms = 0;
    reload->excluded_lease_not_before_ms = 0;
    reload->all_ready_since_ms = 0;
    clear_peer_candidates();
}

static bool excluded_full_member_needs_lease_wait(uint32_t participant_mask)
{
    bool backend_change = false;
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        if (resource_requires_drain(i))
        {
            backend_change = true;
            break;
        }
    }
    if (!backend_change)
        return false;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (!(participant_mask & node_bit(i)) &&
            g_state.cfg.nodes[i].role == LCS_NODE_FULL)
            return true;
    }
    return false;
}

static void load_candidate(int epoll_fd)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    lcs_config_t candidate;
    char error[512] = {0};
    reload->request_pending = false;
    if (lcs_config_load(reload->config_path, &candidate, error, sizeof(error)) != 0)
    {
        reset_candidate_state(reload);
        reload->auto_load_blocked = true;
        lcs_log_error("configuration reload rejected: %s", error);
        peer_broadcast_config_reload(epoll_fd);
        return;
    }
    if (lcs_config_candidate_prepare(&g_state.cfg, &candidate, &candidate, error, sizeof(error)) != 0)
    {
        reset_candidate_state(reload);
        reload->auto_load_blocked = true;
        lcs_log_error("configuration reload rejected: %s", error);
        peer_broadcast_config_reload(epoll_fd);
        return;
    }

    uint64_t voting_fingerprint = lcs_config_voting_fingerprint(&candidate);
    uint64_t full_fingerprint = lcs_config_full_fingerprint(&candidate);
    if (lcs_config_resources_equal(&candidate, &g_state.cfg))
    {
        reset_candidate_state(reload);
        lcs_log_info("configuration reload ignored: resource configuration is unchanged");
        peer_broadcast_config_reload(epoll_fd);
        return;
    }

    bool catchup = reload->catchup_proof &&
        voting_fingerprint == reload->catchup_voting_fingerprint &&
        (!reload->catchup_require_full_fingerprint ||
         full_fingerprint == reload->catchup_full_fingerprint);
    uint32_t participant_mask = catchup ?
        node_bit((size_t)g_state.self_index) :
        reload->pending_participant_mask;
    if (!catchup && !participant_mask)
    {
        if (!cluster_has_quorum())
        {
            reset_candidate_state(reload);
            reload->auto_load_blocked = true;
            lcs_log_error("configuration reload rejected: the cluster does not currently have quorum");
            peer_broadcast_config_reload(epoll_fd);
            return;
        }
        participant_mask = capture_online_participants();
    }
    reload->pending_participant_mask = 0;
    if (!catchup &&
        (!(participant_mask & node_bit((size_t)g_state.self_index)) ||
        participant_count(participant_mask) < g_state.quorum_needed ||
        !participant_set_has_full_member(participant_mask) ||
        !participants_support_reload(participant_mask)))
    {
        reset_candidate_state(reload);
        reload->auto_load_blocked = true;
        lcs_log_error("configuration reload rejected: online participant set lacks quorum, a full member, or protocol support");
        peer_broadcast_config_reload(epoll_fd);
        return;
    }

    reload->candidate = candidate;
    reload->loaded = true;
    reload->auto_load_blocked = false;
    reload->agreed = false;
    reload->drain_started = false;
    reload->ready = false;
    reload->commit_requested = false;
    reload->commit_broadcast = false;
    reload->catchup = catchup;
    reload->commit_not_before_ms = 0;
    reload->participant_mask = participant_mask;
    reload->excluded_lease_not_before_ms =
        !catchup && excluded_full_member_needs_lease_wait(participant_mask) ?
        lcs_now_ms() + g_state.cfg.lease_ms : 0;
    reload->voting_fingerprint = voting_fingerprint;
    reload->full_fingerprint = full_fingerprint;
    reload->next_announce_ms = 0;
    reload->all_ready_since_ms = 0;
    reload->mismatch_logged = false;
    clear_peer_candidates();
    move_cancel_all(epoll_fd, "cluster configuration reload is in progress");
    if (catchup)
        lcs_log_info("configuration reload entering stale-node catch-up for committed voting_fingerprint=%016llx full_fingerprint=%016llx",
                     (unsigned long long)voting_fingerprint,
                     (unsigned long long)full_fingerprint);
    lcs_log_info("configuration reload candidate loaded generation=%llu resources=%zu participants=%u/%zu mask=%08x voting_fingerprint=%016llx full_fingerprint=%016llx",
                 (unsigned long long)(reload->active_generation + 1),
                 candidate.resource_count,
                 participant_count(participant_mask),
                 g_state.cfg.node_count,
                 participant_mask,
                 (unsigned long long)reload->voting_fingerprint,
                 (unsigned long long)reload->full_fingerprint);
}

void config_reload_init(const char *config_path)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    memset(reload, 0, sizeof(*reload));
    snprintf(reload->config_path, sizeof(reload->config_path), "%s", config_path ? config_path : "");
    reload->active_generation = 1;
}

void config_reload_request(void)
{
    if (g_state.config_reload.agreed)
    {
        lcs_log_warn("configuration reload request ignored: the candidate is already agreement-locked");
        return;
    }
    g_state.config_reload.auto_load_blocked = false;
    g_state.config_reload.request_pending = true;
}

bool config_reload_in_progress(void)
{
    return g_state.config_reload.loaded || g_state.config_reload.request_pending;
}

bool config_reload_supported_by_online_quorum(void)
{
    if (!cluster_has_quorum())
        return false;
    uint32_t participants = capture_online_participants();
    return participant_count(participants) >= g_state.quorum_needed &&
           participant_set_has_full_member(participants) &&
           participants_support_reload(participants);
}

bool config_reload_catchup_available(void)
{
    return g_state.config_reload.catchup_proof;
}

int config_reload_encode_announcement(void *payload, size_t cap, size_t *len)
{
    const config_reload_runtime_t *reload = &g_state.config_reload;
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, cap);
    if (lcs_buf_put_u8(&writer, reload->loaded ? 1 : 0) != 0 ||
        lcs_buf_put_u8(&writer, reload->agreed ? 1 : 0) != 0 ||
        lcs_buf_put_u8(&writer, reload->ready ? 1 : 0) != 0)
        return -1;
    if (lcs_buf_put_u64(&writer, reload->loaded ?
                        reload->voting_fingerprint : 0) != 0 ||
        lcs_buf_put_u64(&writer, reload->loaded ?
                        reload->full_fingerprint : 0) != 0 ||
        lcs_buf_put_u32(&writer, reload->loaded ?
                        reload->participant_mask : 0) != 0)
        return -1;
    *len = writer.len;
    return 0;
}

int config_reload_handle_announcement(const void *payload, size_t len, int source_node_idx)
{
    if (source_node_idx < 0 ||
        (size_t)source_node_idx >= g_state.cfg.node_count)
        return -1;
    lcs_buf_reader_t reader;
    uint64_t voting_fingerprint, full_fingerprint;
    uint32_t participant_mask;
    uint8_t loaded, agreed, ready;
    lcs_buf_reader_init(&reader, payload, len);
    if (lcs_buf_get_u8(&reader, &loaded) != 0 ||
        lcs_buf_get_u8(&reader, &agreed) != 0 ||
        lcs_buf_get_u8(&reader, &ready) != 0 ||
        lcs_buf_get_u64(&reader, &voting_fingerprint) != 0 ||
        lcs_buf_get_u64(&reader, &full_fingerprint) != 0 ||
        lcs_buf_get_u32(&reader, &participant_mask) != 0 ||
        loaded > 1 || agreed > 1 || ready > 1 || reader.off != reader.len ||
        (ready && !agreed) ||
        (!loaded && (agreed || ready || voting_fingerprint ||
                     full_fingerprint || participant_mask)) ||
        (loaded && (!participant_mask_valid(participant_mask) ||
                    !(participant_mask & node_bit((size_t)source_node_idx)) ||
                    participant_count(participant_mask) < g_state.quorum_needed)))
        return -1;

    config_reload_runtime_t *reload = &g_state.config_reload;
    reload->peer_loaded[source_node_idx] = loaded != 0;
    reload->peer_agreed[source_node_idx] = agreed != 0;
    reload->peer_ready[source_node_idx] = ready != 0;
    reload->peer_participant_mask[source_node_idx] = participant_mask;
    reload->peer_voting_fingerprint[source_node_idx] = voting_fingerprint;
    reload->peer_full_fingerprint[source_node_idx] = full_fingerprint;
    if (loaded && reload->loaded && !reload->agreed &&
        voting_fingerprint == reload->voting_fingerprint &&
        full_fingerprint == reload->full_fingerprint &&
        participant_mask != reload->participant_mask &&
        (participant_mask & node_bit((size_t)g_state.self_index)) &&
        source_node_idx == participant_coordinator(participant_mask))
    {
        lcs_log_info("configuration reload adopting participant set mask=%08x from coordinator %s",
                     participant_mask, g_state.cfg.nodes[source_node_idx].name);
        reload->participant_mask = participant_mask;
        reload->excluded_lease_not_before_ms =
            excluded_full_member_needs_lease_wait(participant_mask) ?
            lcs_now_ms() + g_state.cfg.lease_ms : 0;
        reload->next_announce_ms = 0;
        clear_peer_candidates();
        reload->peer_loaded[source_node_idx] = true;
        reload->peer_agreed[source_node_idx] = agreed != 0;
        reload->peer_ready[source_node_idx] = ready != 0;
        reload->peer_participant_mask[source_node_idx] = participant_mask;
        reload->peer_voting_fingerprint[source_node_idx] = voting_fingerprint;
        reload->peer_full_fingerprint[source_node_idx] = full_fingerprint;
    }
    if (loaded && !reload->loaded && !reload->request_pending &&
        !reload->auto_load_blocked)
    {
        bool candidate_is_committed_active = reload->committed_transition &&
            agreed && ready &&
            voting_fingerprint == lcs_config_voting_fingerprint(&g_state.cfg) &&
            full_fingerprint == lcs_config_full_fingerprint(&g_state.cfg);
        if (candidate_is_committed_active)
            return 0;
        if (!(participant_mask & node_bit((size_t)g_state.self_index)))
            return 0;
        lcs_log_info("peer %s requested configuration reload; loading local candidate",
                     g_state.cfg.nodes[source_node_idx].name);
        reload->pending_participant_mask = participant_mask;
        reload->request_pending = true;
    }
    return 0;
}

int config_reload_handle_commit(const void *payload, size_t len, int source_node_idx)
{
    lcs_buf_reader_t reader;
    uint64_t voting_fingerprint, full_fingerprint;
    uint32_t participant_mask;
    lcs_buf_reader_init(&reader, payload, len);
    if (lcs_buf_get_u64(&reader, &voting_fingerprint) != 0 ||
        lcs_buf_get_u64(&reader, &full_fingerprint) != 0 ||
        lcs_buf_get_u32(&reader, &participant_mask) != 0 ||
        reader.off != reader.len)
        return -1;
    bool local_full = g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL;
    if (!g_state.config_reload.loaded ||
        source_node_idx != participant_coordinator(participant_mask) ||
        participant_mask != g_state.config_reload.participant_mask ||
        voting_fingerprint != g_state.config_reload.voting_fingerprint ||
        (local_full && full_fingerprint !=
                       g_state.config_reload.full_fingerprint) ||
        !g_state.config_reload.drain_started ||
        !g_state.config_reload.ready)
        return -1;
    g_state.config_reload.commit_requested = true;
    g_state.config_reload.commit_not_before_ms = lcs_now_ms() + 250u;
    return 0;
}

static bool participant_set_intact(void)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    if (!(reload->participant_mask & node_bit((size_t)g_state.self_index)) ||
        participant_count(reload->participant_mask) < g_state.quorum_needed)
        return false;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;
        bool participant = (reload->participant_mask & node_bit(i)) != 0;
        bool established = g_state.peers[i].conn_state == LCS_PEER_ESTABLISHED;
        if (participant != established)
        {
            lcs_log_warn("configuration reload participant set changed: node %s is now %s but was %s at proposal start",
                         g_state.cfg.nodes[i].name,
                         established ? "online" : "offline",
                         participant ? "online" : "offline");
            return false;
        }
    }
    return true;
}

static bool candidate_barrier_complete(bool require_agreed, bool require_ready, uint64_t *agreed_full_fingerprint)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    if (!reload->loaded ||
        (require_agreed && !reload->agreed) ||
        (require_ready && !reload->ready) ||
        !participants_support_reload(reload->participant_mask))
        return false;
    uint64_t full_fingerprint = 0;
    if (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL)
        full_fingerprint = reload->full_fingerprint;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index ||
            !(reload->participant_mask & node_bit(i)))
            continue;
        if (g_state.peers[i].conn_state != LCS_PEER_ESTABLISHED ||
            !reload->peer_loaded[i] ||
            (require_agreed && !reload->peer_agreed[i]) ||
            (require_ready && !reload->peer_ready[i]) ||
            reload->peer_participant_mask[i] != reload->participant_mask ||
            reload->peer_voting_fingerprint[i] !=
            reload->voting_fingerprint)
        {
            if (reload->peer_loaded[i] && reload->peer_voting_fingerprint[i] != reload->voting_fingerprint && !reload->mismatch_logged)
            {
                lcs_log_warn("configuration reload candidate mismatch with node %s: voting fingerprint local=%016llx remote=%016llx",
                             g_state.cfg.nodes[i].name,
                             (unsigned long long)reload->voting_fingerprint,
                             (unsigned long long)reload->peer_voting_fingerprint[i]);
                reload->mismatch_logged = true;
            }
            return false;
        }
        if (g_state.cfg.nodes[i].role == LCS_NODE_FULL)
        {
            if (!full_fingerprint)
                full_fingerprint = reload->peer_full_fingerprint[i];
            else if (reload->peer_full_fingerprint[i] != full_fingerprint)
            {
                if (!reload->mismatch_logged)
                {
                    lcs_log_warn("configuration reload candidate mismatch with full-member %s: full fingerprint expected=%016llx remote=%016llx",
                                 g_state.cfg.nodes[i].name,
                                 (unsigned long long)full_fingerprint,
                                 (unsigned long long)reload->peer_full_fingerprint[i]);
                    reload->mismatch_logged = true;
                }
                return false;
            }
        }
    }
    reload->mismatch_logged = false;
    if (agreed_full_fingerprint)
        *agreed_full_fingerprint = full_fingerprint;
    return true;
}

static uint64_t next_epoch(uint64_t value)
{
    return value == UINT64_MAX ? UINT64_MAX : value + 1;
}

static void apply_candidate(int epoll_fd)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    lcs_config_t old_config = g_state.cfg;
    resource_runtime_t old_resources[LCS_MAX_RESOURCES];
    lease_grant_t old_grants[LCS_MAX_RESOURCES];
    memcpy(old_resources, g_state.resources, sizeof(old_resources));
    memcpy(old_grants, g_state.lease_grants, sizeof(old_grants));

    move_cancel_all(epoll_fd, "cluster configuration reload committed");
    lease_cancel_all_operations();
    peer_close_all_for_config_reload(epoll_fd);
    memset(g_state.resources, 0, sizeof(g_state.resources));
    memset(g_state.lease_grants, 0, sizeof(g_state.lease_grants));

    for (size_t new_idx = 0; new_idx < reload->candidate.resource_count; new_idx++)
    {
        const lcs_resource_config_t *new_resource = &reload->candidate.resources[new_idx];
        int old_idx = lcs_config_resource_index(&old_config, new_resource->name);
        resource_runtime_t *new_runtime = &g_state.resources[new_idx];
        lease_grant_t *new_grant = &g_state.lease_grants[new_idx];
        if (old_idx >= 0 && lcs_config_backend_equal(&old_config.resources[old_idx], new_resource))
        {
            *new_runtime = old_resources[old_idx];
            *new_grant = old_grants[old_idx];
            continue;
        }
        new_runtime->owner_node = -1;
        new_runtime->state = LCS_RES_STOPPED;
        new_grant->owner_node = -1;
        if (old_idx >= 0)
        {
            uint64_t epoch = old_resources[old_idx].epoch;
            if (old_grants[old_idx].promised_epoch > epoch)
                epoch = old_grants[old_idx].promised_epoch;
            new_runtime->epoch = next_epoch(epoch);
            new_grant->promised_epoch = new_runtime->epoch;
        }
    }

    g_state.cfg = reload->candidate;
    if (g_state.config_bootstrap_initialized)
        lcs_config_bootstrap_request(&g_state.config_bootstrap);
    g_state.self_index = lcs_config_self_index(&g_state.cfg);
    g_state.quorum_needed = lcs_config_quorum(&g_state.cfg);
    g_state.next_placement_ms = lcs_now_ms() + 500u;
    reload->active_generation++;
    reload->committed_transition = true;
    reload->loaded = false;
    reload->request_pending = false;
    reload->agreed = false;
    reload->drain_started = false;
    reload->ready = false;
    reload->commit_requested = false;
    reload->commit_broadcast = false;
    reload->catchup = false;
    reload->catchup_proof = false;
    reload->catchup_require_full_fingerprint = false;
    reload->participant_mask = 0;
    reload->pending_participant_mask = 0;
    reload->next_announce_ms = 0;
    reload->all_ready_since_ms = 0;
    reload->commit_not_before_ms = 0;
    reload->excluded_lease_not_before_ms = 0;
    reload->catchup_voting_fingerprint = 0;
    reload->catchup_full_fingerprint = 0;
    reload->mismatch_logged = false;
    clear_peer_candidates();
    lcs_log_info("configuration reload committed generation=%llu resources=%zu",
                 (unsigned long long)reload->active_generation,
                 g_state.cfg.resource_count);
}

void config_reload_process(int epoll_fd)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    if (reload->request_pending)
        load_candidate(epoll_fd);
    if (!reload->loaded)
        return;

    if (reload->catchup)
    {
        bool ready = local_drain_complete(epoll_fd);
        if (ready != reload->ready)
        {
            reload->ready = ready;
            lcs_log_info("configuration reload stale-node catch-up is %s",
                         ready ? "ready to apply" :
                         "waiting for local resource drain");
        }
        uint64_t catchup_now = lcs_now_ms();
        if (ready && !reload->commit_requested)
        {
            reload->commit_requested = true;
            reload->commit_not_before_ms = catchup_now + 250u;
        }
        if (reload->commit_requested && ready &&
            catchup_now >= reload->commit_not_before_ms)
            apply_candidate(epoll_fd);
        return;
    }

    if (!reload->commit_requested && !participant_set_intact())
    {
        reset_candidate_state(reload);
        reload->auto_load_blocked = false;
        lcs_log_warn("configuration reload aborted because the fixed online participant set changed; retry reload with the current online quorum");
        peer_broadcast_config_reload(epoll_fd);
        return;
    }

    uint64_t agreed_full_fingerprint = 0;
    if (!reload->agreed && candidate_barrier_complete(false, false, &agreed_full_fingerprint))
    {
        reload->agreed = true;
        reload->next_announce_ms = 0;
        lcs_log_info("configuration reload candidate agreement reached locally; waiting for peer acknowledgements");
    }
    if (!reload->drain_started && reload->agreed && candidate_barrier_complete(true, false, &agreed_full_fingerprint))
    {
        reload->drain_started = true;
        reload->next_announce_ms = 0;
        lcs_log_info("configuration reload candidate agreed by every participant; starting resource drain");
    }

    bool ready = reload->drain_started && local_drain_complete(epoll_fd);
    if (ready != reload->ready)
    {
        reload->ready = ready;
        reload->next_announce_ms = 0;
        lcs_log_info("configuration reload candidate is %s",
                     ready ? "ready to commit" : "waiting for local resource drain");
    }

    uint64_t now = lcs_now_ms();
    if (!reload->next_announce_ms || now >= reload->next_announce_ms)
    {
        peer_broadcast_config_reload(epoll_fd);
        reload->next_announce_ms = now + 250u;
    }

    if (candidate_barrier_complete(true, true, &agreed_full_fingerprint))
    {
        if ((!reload->excluded_lease_not_before_ms ||
             now >= reload->excluded_lease_not_before_ms) &&
            !reload->all_ready_since_ms)
            reload->all_ready_since_ms = now;
    } else
    {
        reload->all_ready_since_ms = 0;
    }

    uint64_t settle_ms = g_state.cfg.peer_timeout_ms / 4u;
    if (settle_ms < 250u)
        settle_ms = 250u;
    if (settle_ms > 2000u)
        settle_ms = 2000u;
    if (g_state.self_index == participant_coordinator(reload->participant_mask) &&
        reload->all_ready_since_ms &&
        now - reload->all_ready_since_ms >= settle_ms &&
        !reload->commit_broadcast)
    {
        unsigned char payload[20];
        lcs_buf_writer_t writer;
        lcs_buf_writer_init(&writer, payload, sizeof(payload));
        if (lcs_buf_put_u64(&writer, reload->voting_fingerprint) == 0 &&
            lcs_buf_put_u64(&writer, agreed_full_fingerprint) == 0 &&
            lcs_buf_put_u32(&writer, reload->participant_mask) == 0)
        {
            peer_broadcast_config_reload_commit(epoll_fd, payload, (uint32_t)writer.len);
            reload->commit_broadcast = true;
            reload->commit_requested = true;
            /* Let nonblocking peer queues deliver the decision before apply_candidate()
             * closes every connection to remap the resource-indexed runtime state. */
            reload->commit_not_before_ms = now + 250u;
        }
    }

    if (reload->commit_requested && reload->ready &&
        (!reload->commit_not_before_ms || now >= reload->commit_not_before_ms) &&
        (!reload->excluded_lease_not_before_ms ||
         now >= reload->excluded_lease_not_before_ms))
        apply_candidate(epoll_fd);
}
