// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cluster.h"

#include "log.h"
#include "resources.h"
#include "util.h"
#include "vip.h"

#include <stdio.h>
#include <string.h>

bool cluster_node_is_online(size_t node_idx)
{
    if ((int)node_idx == g_state.self_index)
        return true;
    if (node_idx >= g_state.cfg.node_count || !g_state.peers[node_idx].online)
        return false;
    return lcs_now_ms() - g_state.peers[node_idx].last_seen_ms <= g_state.cfg.peer_timeout_ms;
}

lcs_node_state_t cluster_node_state(size_t node_idx)
{
    if (!cluster_node_is_online(node_idx))
        return LCS_NODE_OFFLINE;
    if ((int)node_idx == g_state.self_index)
        return g_state.voting_ready ? LCS_NODE_ONLINE : LCS_NODE_RECOVERING;
    return g_state.peers[node_idx].voting_ready ? LCS_NODE_ONLINE : LCS_NODE_RECOVERING;
}

int cluster_first_online_full_member(void)
{
    int best = -1;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (g_state.cfg.nodes[i].role != LCS_NODE_FULL || !cluster_node_is_online(i))
            continue;
        if (best < 0 || strcmp(g_state.cfg.nodes[i].name, g_state.cfg.nodes[best].name) < 0)
            best = (int)i;
    }
    return best;
}

const char *cluster_node_name_or_none(int node_idx)
{
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return "-";
    return g_state.cfg.nodes[node_idx].name;
}

int cluster_has_quorum(void)
{
    return g_state.votes_seen >= g_state.quorum_needed;
}

bool cluster_local_voting_ready(void)
{
    return g_state.voting_ready;
}

void cluster_update_recovery_state(void)
{
    if (g_state.voting_ready || lcs_now_ms() < g_state.voting_not_before_ms)
        return;
    if (!resources_startup_cleanup_complete())
        return;

    uint32_t synchronized_votes = 1;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;
        if (g_state.peers[i].initial_sync_complete && cluster_node_is_online(i))
            synchronized_votes++;
    }
    if (synchronized_votes < g_state.quorum_needed)
        return;

    g_state.voting_ready = true;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i != g_state.self_index && g_state.peers[i].fd >= 0)
            g_state.peers[i].next_heartbeat_ms = 0;
    }
    lcs_log_info("recovery complete; node is now eligible to vote after synchronizing %u/%u votes", synchronized_votes, g_state.quorum_needed);
}

void cluster_recompute_votes(void)
{
    uint32_t votes = 0;
    uint64_t membership_mask = 0;
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        bool online = false;
        bool voting_ready = false;
        if ((int)i == g_state.self_index)
        {
            online = true;
            voting_ready = g_state.voting_ready;
        } else if (g_state.peers[i].online && now - g_state.peers[i].last_seen_ms <= g_state.cfg.peer_timeout_ms)
        {
            online = true;
            voting_ready = g_state.peers[i].voting_ready;
        } else
        {
            if (g_state.peers[i].online)
                lcs_log_info("peer %s offline", g_state.cfg.nodes[i].name);
            g_state.peers[i].online = false;
        }
        if (online)
        {
            membership_mask |= 1ull << i;
            if (voting_ready)
                votes++;
        }
    }
    if (!g_state.membership_since_ms || membership_mask != g_state.membership_mask)
    {
        g_state.membership_mask = membership_mask;
        g_state.membership_since_ms = now;
    }
    g_state.votes_seen = votes;
}

int cluster_encode_state(unsigned char *payload, size_t cap, size_t *len)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u64(&w, g_state.instance_id) != 0 || lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.resource_count) != 0)
        return -1;

    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        const resource_runtime_t *res = &g_state.resources[i];
        uint16_t owner = res->owner_node < 0 ? UINT16_MAX : (uint16_t)res->owner_node;
        if (lcs_buf_put_u16(&w, (uint16_t)i) != 0 ||
            lcs_buf_put_u16(&w, owner) != 0 ||
            lcs_buf_put_u64(&w, res->owner_instance_id) != 0 ||
            lcs_buf_put_u8(&w, (uint8_t)res->state) != 0 ||
            lcs_buf_put_u64(&w, res->epoch) != 0 ||
            lcs_buf_put_u64(&w, res->lease_id) != 0 ||
            lcs_buf_put_u64(&w, res->lease_deadline_ms > lcs_now_ms() ?
                            res->lease_deadline_ms - lcs_now_ms() : 0) != 0 ||
            lcs_buf_put_u64(&w, res->failover_count) != 0 ||
            lcs_buf_put_u64(&w, res->home_generation) != 0 ||
            lcs_buf_put_u8(&w, res->home_blocked ? 1 : 0) != 0 ||
            lcs_buf_put_u64(&w, res->disabled_generation) != 0 ||
            lcs_buf_put_u8(&w, res->disabled ? 1 : 0) != 0 ||
            lcs_buf_put_fixed_string(&w, res->conflict_reason, LCS_REASON_MAX + 1) != 0)
            return -1;
    }
    *len = w.len;
    return 0;
}

typedef struct
{
    uint16_t id;
    uint16_t owner;
    uint64_t owner_instance_id;
    uint8_t state;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t remaining_ms;
    uint64_t failover_count;
    uint64_t home_generation;
    uint8_t home_blocked;
    uint64_t disabled_generation;
    uint8_t disabled;
    char reason[LCS_REASON_MAX + 1];
} cluster_state_entry_t;

static int cluster_reject_state(int source_node_idx, int entry_idx,
                                const char *reason)
{
    const char *source = source_node_idx >= 0 ?
                         cluster_node_name_or_none(source_node_idx) : "internal";
    if (entry_idx >= 0)
        lcs_log_warn("rejecting state snapshot from %s entry=%d: %s",
                     source, entry_idx, reason);
    else
        lcs_log_warn("rejecting state snapshot from %s: %s", source, reason);
    return -1;
}

static const char *cluster_validate_state_entry(const cluster_state_entry_t *entry)
{
    if (entry->id >= g_state.cfg.resource_count)
        return "resource ID is out of range";
    if (entry->home_blocked > 1 || entry->disabled > 1)
        return "boolean field is not 0 or 1";
    if (entry->remaining_ms > g_state.cfg.lease_ms)
        return "remaining lease exceeds configured lease_ms";

    bool has_owner = entry->owner != UINT16_MAX;
    if (has_owner &&
        (entry->owner >= g_state.cfg.node_count ||
         g_state.cfg.nodes[entry->owner].role != LCS_NODE_FULL))
        return "owner is not a valid full-member";
    if (!has_owner && entry->owner_instance_id != 0)
        return "owner instance is set without an owner";
    if (has_owner && entry->owner_instance_id == 0)
        return "owner instance is missing";

    switch ((lcs_resource_state_t)entry->state)
    {
        case LCS_RES_ACTIVE:
        case LCS_RES_STARTING:
        case LCS_RES_STOPPING:
            if (!has_owner)
                return "active or transitioning resource has no owner";
            if (entry->lease_id == 0)
                return "active or transitioning resource has no lease ID";
            break;
        case LCS_RES_STOPPED:
        case LCS_RES_CONFLICT:
            if (has_owner || entry->owner_instance_id != 0 ||
                entry->lease_id != 0 || entry->remaining_ms != 0)
                return "stopped or conflicted resource carries ownership or lease state";
            break;
        case LCS_RES_STOP_FAILED:
            if (entry->remaining_ms != 0)
                return "stop_failed resource carries a live lease duration";
            if (!has_owner && entry->lease_id != 0)
                return "ownerless stop_failed resource carries a lease ID";
            break;
        default:
            return "resource state is invalid";
    }
    return NULL;
}

int cluster_apply_state(const void *payload, size_t len, int source_node_idx)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint64_t sender_instance_id;
    uint16_t count;
    if (lcs_buf_get_u64(&r, &sender_instance_id) != 0 ||
        lcs_buf_get_u16(&r, &count) != 0)
        return cluster_reject_state(source_node_idx, -1, "truncated header");
    if (count != g_state.cfg.resource_count)
        return cluster_reject_state(source_node_idx, -1, "resource count does not match configuration");

    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= g_state.cfg.node_count ||
         sender_instance_id != g_state.peers[source_node_idx].instance_id))
        return cluster_reject_state(source_node_idx, -1, "sender instance does not match the connected peer");

    cluster_state_entry_t incoming[LCS_MAX_RESOURCES];
    bool seen[LCS_MAX_RESOURCES] = { false };
    for (uint16_t n = 0; n < count; n++)
    {
        cluster_state_entry_t entry;
        memset(&entry, 0, sizeof(entry));
        if (lcs_buf_get_u16(&r, &entry.id) != 0 ||
            lcs_buf_get_u16(&r, &entry.owner) != 0 ||
            lcs_buf_get_u64(&r, &entry.owner_instance_id) != 0 ||
            lcs_buf_get_u8(&r, &entry.state) != 0 ||
            lcs_buf_get_u64(&r, &entry.epoch) != 0 ||
            lcs_buf_get_u64(&r, &entry.lease_id) != 0 ||
            lcs_buf_get_u64(&r, &entry.remaining_ms) != 0 ||
            lcs_buf_get_u64(&r, &entry.failover_count) != 0 ||
            lcs_buf_get_u64(&r, &entry.home_generation) != 0 ||
            lcs_buf_get_u8(&r, &entry.home_blocked) != 0 ||
            lcs_buf_get_u64(&r, &entry.disabled_generation) != 0 ||
            lcs_buf_get_u8(&r, &entry.disabled) != 0 ||
            lcs_buf_get_fixed_string(&r, entry.reason, sizeof(entry.reason),
                                     LCS_REASON_MAX + 1) != 0)
            return cluster_reject_state(source_node_idx, n, "truncated or malformed entry");

        const char *validation_error = cluster_validate_state_entry(&entry);
        if (validation_error)
            return cluster_reject_state(source_node_idx, n, validation_error);
        if (seen[entry.id])
            return cluster_reject_state(source_node_idx, n, "duplicate resource ID");
        seen[entry.id] = true;
        incoming[entry.id] = entry;
    }
    if (r.off != r.len)
        return cluster_reject_state(source_node_idx, -1, "trailing data");
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        if (!seen[i])
            return cluster_reject_state(source_node_idx, -1, "snapshot is missing a resource ID");
    }

    for (size_t id = 0; id < g_state.cfg.resource_count; id++)
    {
        const cluster_state_entry_t *entry = &incoming[id];
        resource_runtime_t *res = &g_state.resources[id];
        if (entry->home_generation > res->home_generation)
        {
            res->home_generation = entry->home_generation;
            res->home_blocked = entry->home_blocked != 0;
        }
        if (entry->disabled_generation > res->disabled_generation)
        {
            res->disabled_generation = entry->disabled_generation;
            res->disabled = entry->disabled != 0;
        }
        if (entry->failover_count > res->failover_count)
            res->failover_count = entry->failover_count;
        if (resources_preserve_startup_cleanup_failure((int)id, entry->epoch))
            continue;
        bool incoming_conflict = entry->state == LCS_RES_CONFLICT;
        bool incoming_stop_failed = entry->state == LCS_RES_STOP_FAILED;
        bool local_unsafe = res->state == LCS_RES_CONFLICT ||
                            res->state == LCS_RES_STOP_FAILED;
        bool newer_epoch = entry->epoch > res->epoch;
        bool same_lease = entry->epoch == res->epoch &&
                          entry->lease_id != 0 &&
                          entry->lease_id == res->lease_id &&
                          entry->owner_instance_id == res->owner_instance_id &&
                          (entry->owner == UINT16_MAX ? res->owner_node < 0 : res->owner_node == (int)entry->owner);
        bool preserve_local_transition = same_lease &&
                                         entry->owner == (uint16_t)g_state.self_index &&
                                         entry->owner_instance_id == g_state.instance_id &&
                                         (res->state == LCS_RES_STARTING ||
                                          res->state == LCS_RES_STOPPING);
        bool unsafe_update = (incoming_conflict || incoming_stop_failed) && entry->epoch >= res->epoch;
        if (local_unsafe && !incoming_conflict && !incoming_stop_failed && entry->epoch <= res->epoch)
            continue;
        bool local_owner = res->owner_node == g_state.self_index &&
                           res->owner_instance_id == g_state.instance_id &&
                           res->state != LCS_RES_STOPPED;
        if (local_owner && !same_lease && !unsafe_update)
        {
            lcs_log_debug3("ignoring state sync that would replace locally owned resource %s epoch=%llu", g_state.cfg.resources[id].name, (unsigned long long)res->epoch);
            continue;
        }
        if (newer_epoch || same_lease || unsafe_update)
        {
            if (res->owner_node == g_state.self_index &&
                res->owner_instance_id == g_state.instance_id &&
                entry->owner != (uint16_t)g_state.self_index &&
                res->state == LCS_RES_ACTIVE)
            {
                if (resources_stop_local_backend(&g_state.cfg.resources[id]) != 0)
                {
                    resources_enter_stop_failed_state((int)id, entry->epoch + 1, "local resource stop failed while applying state sync", -1);
                    continue;
                }
            }
            res->epoch = entry->epoch;
            res->lease_id = entry->lease_id;
            res->owner_node = entry->owner == UINT16_MAX ? -1 : (int)entry->owner;
            res->owner_instance_id = entry->owner == UINT16_MAX ? 0 : entry->owner_instance_id;
            if (!preserve_local_transition)
                res->state = (lcs_resource_state_t)entry->state;
            if (!same_lease)
                res->lease_deadline_ms = entry->remaining_ms ?
                                         lcs_now_ms() + entry->remaining_ms : 0;
            snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s",
                     (incoming_conflict || incoming_stop_failed) ? entry->reason : "");
        }
    }
    return 0;
}
