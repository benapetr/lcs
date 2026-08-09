// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_reload.h"

#include "lease.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static bool same_string(const char *a, const char *b)
{
    return strcmp(a, b) == 0;
}

static bool resource_backend_identity_equal(const lcs_resource_config_t *a, const lcs_resource_config_t *b)
{
    if (a->type != b->type)
        return false;
    if (a->type == LCS_RESOURCE_VIP)
        return same_string(a->address, b->address) &&
               same_string(a->interface, b->interface);
    if (a->type == LCS_RESOURCE_SERVICE)
        return same_string(a->systemd_unit, b->systemd_unit);
    return false;
}

static bool resource_config_equal(const lcs_resource_config_t *a, const lcs_resource_config_t *b)
{
    if (a->type != b->type || !same_string(a->name, b->name) ||
        !same_string(a->group_name, b->group_name) ||
        !same_string(a->home_node_name, b->home_node_name) ||
        !same_string(a->systemd_unit, b->systemd_unit) ||
        !same_string(a->address, b->address) ||
        !same_string(a->interface, b->interface) ||
        !same_string(a->pre_start, b->pre_start) ||
        !same_string(a->post_start, b->post_start) ||
        !same_string(a->pre_stop, b->pre_stop) ||
        !same_string(a->post_stop, b->post_stop) ||
        a->priority != b->priority ||
        a->depends_on_count != b->depends_on_count)
        return false;
    for (size_t i = 0; i < a->depends_on_count; i++)
    {
        if (!same_string(a->depends_on_names[i], b->depends_on_names[i]))
            return false;
    }
    return true;
}

static bool resource_configuration_equal(const lcs_config_t *a, const lcs_config_t *b)
{
    if (a->group_count != b->group_count || a->resource_count != b->resource_count)
        return false;
    for (size_t i = 0; i < a->group_count; i++)
    {
        if (!same_string(a->groups[i].name, b->groups[i].name) || a->groups[i].type != b->groups[i].type || a->groups[i].mode != b->groups[i].mode)
            return false;
    }
    for (size_t i = 0; i < a->resource_count; i++)
    {
        if (!resource_config_equal(&a->resources[i], &b->resources[i]))
            return false;
    }
    return true;
}

static int validate_resource_only_change(const lcs_config_t *candidate, char *error, size_t error_len)
{
    const lcs_config_t *active = &g_state.cfg;
    if (!same_string(candidate->cluster_name, active->cluster_name) || !same_string(candidate->self_name, active->self_name))
    {
        snprintf(error, error_len, "cluster name and local node identity cannot be reloaded");
        return -1;
    }
    if (candidate->node_count != active->node_count)
    {
        snprintf(error, error_len, "node membership changes require the future joint-consensus protocol");
        return -1;
    }
    for (size_t i = 0; i < active->node_count; i++)
    {
        const lcs_node_config_t *old_node = &active->nodes[i];
        const lcs_node_config_t *new_node = &candidate->nodes[i];
        if (!same_string(old_node->name, new_node->name) ||
            old_node->role != new_node->role ||
            !same_string(old_node->address, new_node->address) ||
            old_node->port != new_node->port)
        {
            snprintf(error, error_len, "node membership, roles, addresses, and ports cannot be reloaded");
            return -1;
        }
    }

#define REQUIRE_UNCHANGED(field) \
    do { if (candidate->field != active->field) { \
        snprintf(error, error_len, #field " cannot be changed by resource reload"); \
        return -1; \
    } } while (0)
#define REQUIRE_STRING_UNCHANGED(field) \
    do { if (!same_string(candidate->field, active->field)) { \
        snprintf(error, error_len, #field " cannot be changed by resource reload"); \
        return -1; \
    } } while (0)

    REQUIRE_STRING_UNCHANGED(bind_address);
    REQUIRE_STRING_UNCHANGED(metrics_bind_address);
    REQUIRE_STRING_UNCHANGED(socket_path);
    REQUIRE_STRING_UNCHANGED(pidfile_path);
    REQUIRE_STRING_UNCHANGED(secret);
    REQUIRE_UNCHANGED(port);
    REQUIRE_UNCHANGED(metrics_port);
    REQUIRE_UNCHANGED(lease_ms);
    REQUIRE_UNCHANGED(renew_ms);
    REQUIRE_UNCHANGED(peer_timeout_ms);
    REQUIRE_UNCHANGED(probe_count);
    REQUIRE_UNCHANGED(probe_timeout_ms);
    REQUIRE_UNCHANGED(hook_timeout_ms);
    REQUIRE_UNCHANGED(syslog_enabled);
    REQUIRE_UNCHANGED(metrics_enabled);
    REQUIRE_UNCHANGED(vip_backend);
#undef REQUIRE_STRING_UNCHANGED
#undef REQUIRE_UNCHANGED
    return 0;
}

static bool resource_requires_drain(size_t old_idx)
{
    const lcs_resource_config_t *old_resource = &g_state.cfg.resources[old_idx];
    int new_idx = lcs_config_resource_index(&g_state.config_reload.candidate, old_resource->name);
    return new_idx < 0 || !resource_backend_identity_equal(old_resource, &g_state.config_reload.candidate.resources[new_idx]);
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
    memset(reload->peer_voting_fingerprint, 0, sizeof(reload->peer_voting_fingerprint));
    memset(reload->peer_full_fingerprint, 0, sizeof(reload->peer_full_fingerprint));
}

static void load_candidate(int epoll_fd)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    lcs_config_t candidate;
    char error[512] = {0};
    reload->request_pending = false;
    if (lcs_config_load(reload->config_path, &candidate, error, sizeof(error)) != 0)
    {
        reload->loaded = false;
        reload->agreed = false;
        reload->drain_started = false;
        reload->ready = false;
        reload->commit_requested = false;
        reload->commit_broadcast = false;
        reload->commit_not_before_ms = 0;
        reload->auto_load_blocked = true;
        clear_peer_candidates();
        lcs_log_error("configuration reload rejected: %s", error);
        peer_broadcast_config_reload(epoll_fd);
        return;
    }
    if (validate_resource_only_change(&candidate, error, sizeof(error)) != 0)
    {
        reload->loaded = false;
        reload->agreed = false;
        reload->drain_started = false;
        reload->ready = false;
        reload->commit_requested = false;
        reload->commit_broadcast = false;
        reload->commit_not_before_ms = 0;
        reload->auto_load_blocked = true;
        clear_peer_candidates();
        lcs_log_error("configuration reload rejected: %s", error);
        peer_broadcast_config_reload(epoll_fd);
        return;
    }

    uint64_t voting_fingerprint = lcs_config_voting_fingerprint(&candidate);
    uint64_t full_fingerprint = lcs_config_full_fingerprint(&candidate);
    if (resource_configuration_equal(&candidate, &g_state.cfg))
    {
        reload->loaded = false;
        reload->agreed = false;
        reload->drain_started = false;
        reload->ready = false;
        reload->commit_requested = false;
        reload->commit_broadcast = false;
        reload->commit_not_before_ms = 0;
        clear_peer_candidates();
        lcs_log_info("configuration reload ignored: resource configuration is unchanged");
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
    reload->commit_not_before_ms = 0;
    reload->voting_fingerprint = voting_fingerprint;
    reload->full_fingerprint = full_fingerprint;
    reload->next_announce_ms = 0;
    reload->all_ready_since_ms = 0;
    reload->mismatch_logged = false;
    clear_peer_candidates();
    move_cancel_all(epoll_fd, "cluster configuration reload is in progress");
    lcs_log_info("configuration reload candidate loaded generation=%llu resources=%zu voting_fingerprint=%016llx full_fingerprint=%016llx",
                 (unsigned long long)(reload->active_generation + 1),
                 candidate.resource_count,
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
                        reload->full_fingerprint : 0) != 0)
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
    uint8_t loaded, agreed, ready;
    lcs_buf_reader_init(&reader, payload, len);
    if (lcs_buf_get_u8(&reader, &loaded) != 0 ||
        lcs_buf_get_u8(&reader, &agreed) != 0 ||
        lcs_buf_get_u8(&reader, &ready) != 0 ||
        lcs_buf_get_u64(&reader, &voting_fingerprint) != 0 ||
        lcs_buf_get_u64(&reader, &full_fingerprint) != 0 ||
        loaded > 1 || agreed > 1 || ready > 1 || reader.off != reader.len ||
        (ready && !agreed) ||
        (!loaded && (agreed || ready || voting_fingerprint || full_fingerprint)))
        return -1;

    config_reload_runtime_t *reload = &g_state.config_reload;
    reload->peer_loaded[source_node_idx] = loaded != 0;
    reload->peer_agreed[source_node_idx] = agreed != 0;
    reload->peer_ready[source_node_idx] = ready != 0;
    reload->peer_voting_fingerprint[source_node_idx] = voting_fingerprint;
    reload->peer_full_fingerprint[source_node_idx] = full_fingerprint;
    if (loaded && !reload->loaded && !reload->request_pending &&
        !reload->auto_load_blocked)
    {
        bool candidate_is_committed_active = reload->committed_transition &&
            agreed && ready &&
            voting_fingerprint == lcs_config_voting_fingerprint(&g_state.cfg) &&
            full_fingerprint == lcs_config_full_fingerprint(&g_state.cfg);
        if (candidate_is_committed_active)
            return 0;
        lcs_log_info("peer %s requested configuration reload; loading local candidate",
                     g_state.cfg.nodes[source_node_idx].name);
        reload->request_pending = true;
    }
    return 0;
}

int config_reload_handle_commit(const void *payload, size_t len, int source_node_idx)
{
    if (source_node_idx != 0)
        return -1;
    lcs_buf_reader_t reader;
    uint64_t voting_fingerprint, full_fingerprint;
    lcs_buf_reader_init(&reader, payload, len);
    if (lcs_buf_get_u64(&reader, &voting_fingerprint) != 0 ||
        lcs_buf_get_u64(&reader, &full_fingerprint) != 0 ||
        reader.off != reader.len)
        return -1;
    bool local_full = g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL;
    if (!g_state.config_reload.loaded ||
        voting_fingerprint != g_state.config_reload.voting_fingerprint ||
        (local_full && full_fingerprint !=
                       g_state.config_reload.full_fingerprint) ||
        !g_state.config_reload.drain_started ||
        !g_state.config_reload.ready)
        return -1;
    g_state.config_reload.commit_requested = true;
    return 0;
}

static bool candidate_barrier_complete(bool require_agreed, bool require_ready, uint64_t *agreed_full_fingerprint)
{
    config_reload_runtime_t *reload = &g_state.config_reload;
    if (!reload->loaded ||
        (require_agreed && !reload->agreed) ||
        (require_ready && !reload->ready) ||
        g_state.effective_protocol_version < LCS_PROTO_FEATURE_RESOURCE_RELOAD)
        return false;
    uint64_t full_fingerprint = 0;
    if (g_state.cfg.nodes[g_state.self_index].role == LCS_NODE_FULL)
        full_fingerprint = reload->full_fingerprint;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;
        if (g_state.peers[i].conn_state != LCS_PEER_ESTABLISHED ||
            !reload->peer_loaded[i] ||
            (require_agreed && !reload->peer_agreed[i]) ||
            (require_ready && !reload->peer_ready[i]) ||
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
        if (old_idx >= 0 && resource_backend_identity_equal(&old_config.resources[old_idx], new_resource))
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
    reload->next_announce_ms = 0;
    reload->all_ready_since_ms = 0;
    reload->commit_not_before_ms = 0;
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
        lcs_log_info("configuration reload candidate agreed by every node; starting resource drain");
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
        if (!reload->all_ready_since_ms)
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
    if (g_state.self_index == 0 && reload->all_ready_since_ms && now - reload->all_ready_since_ms >= settle_ms && !reload->commit_broadcast)
    {
        unsigned char payload[16];
        lcs_buf_writer_t writer;
        lcs_buf_writer_init(&writer, payload, sizeof(payload));
        if (lcs_buf_put_u64(&writer, reload->voting_fingerprint) == 0 && lcs_buf_put_u64(&writer, agreed_full_fingerprint) == 0)
        {
            peer_broadcast_config_reload_commit(epoll_fd, payload, (uint32_t)writer.len);
            reload->commit_broadcast = true;
            reload->commit_requested = true;
            /* Let nonblocking peer queues deliver the decision before apply_candidate()
             * closes every connection to remap the resource-indexed runtime state. */
            reload->commit_not_before_ms = now + 250u;
        }
    }

    if (reload->commit_requested && reload->ready && (!reload->commit_not_before_ms || now >= reload->commit_not_before_ms))
        apply_candidate(epoll_fd);
}
