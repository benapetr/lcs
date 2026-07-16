// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cluster.h"

#include "log.h"
#include "systemd_service.h"
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

static void cluster_stop_local_resource(size_t id)
{
    const lcs_vip_config_t *res = &g_state.cfg.vips[id];
    if (res->type == LCS_RESOURCE_SERVICE)
        lcs_systemd_service_stop(res);
    else
        lcs_vip_del(res);
}

int cluster_has_quorum(void)
{
    return g_state.votes_seen >= g_state.quorum_needed;
}

void cluster_recompute_votes(void)
{
    uint32_t votes = 0;
    uint64_t membership_mask = 0;
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        bool online = false;
        if ((int)i == g_state.self_index)
        {
            online = true;
        } else if (g_state.peers[i].online && now - g_state.peers[i].last_seen_ms <= g_state.cfg.peer_timeout_ms)
        {
            online = true;
        } else
        {
            if (g_state.peers[i].online)
                lcs_log_info("peer %s offline", g_state.cfg.nodes[i].name);
            g_state.peers[i].online = false;
        }
        if (online)
        {
            votes++;
            membership_mask |= 1ull << i;
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
    if (lcs_buf_put_u64(&w, g_state.instance_id) != 0 || lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.vip_count) != 0)
        return -1;

    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
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

int cluster_apply_state(const void *payload, size_t len, int source_node_idx)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint64_t sender_instance_id;
    uint16_t count;
    if (lcs_buf_get_u64(&r, &sender_instance_id) != 0 ||
        lcs_buf_get_u16(&r, &count) != 0 ||
        count != g_state.cfg.vip_count)
        return -1;

    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= g_state.cfg.node_count ||
         sender_instance_id != g_state.peers[source_node_idx].instance_id))
        return -1;

    for (uint16_t n = 0; n < count; n++)
    {
        uint16_t id, owner;
        uint8_t state, home_blocked, disabled;
        uint64_t owner_instance_id, epoch, lease_id, remaining_ms, failover_count, home_generation, disabled_generation;
        char reason[LCS_REASON_MAX + 1];
        if (lcs_buf_get_u16(&r, &id) != 0 ||
            lcs_buf_get_u16(&r, &owner) != 0 ||
            lcs_buf_get_u64(&r, &owner_instance_id) != 0 ||
            lcs_buf_get_u8(&r, &state) != 0 ||
            lcs_buf_get_u64(&r, &epoch) != 0 ||
            lcs_buf_get_u64(&r, &lease_id) != 0 ||
            lcs_buf_get_u64(&r, &remaining_ms) != 0 ||
            lcs_buf_get_u64(&r, &failover_count) != 0 ||
            lcs_buf_get_u64(&r, &home_generation) != 0 ||
            lcs_buf_get_u8(&r, &home_blocked) != 0 ||
            lcs_buf_get_u64(&r, &disabled_generation) != 0 ||
            lcs_buf_get_u8(&r, &disabled) != 0 ||
            lcs_buf_get_fixed_string(&r, reason, sizeof(reason), LCS_REASON_MAX + 1) != 0 ||
            id >= g_state.cfg.vip_count)
            return -1;

        resource_runtime_t *res = &g_state.resources[id];
        if (home_generation > res->home_generation)
        {
            res->home_generation = home_generation;
            res->home_blocked = home_blocked != 0;
        }
        if (disabled_generation > res->disabled_generation)
        {
            res->disabled_generation = disabled_generation;
            res->disabled = disabled != 0;
        }
        if (failover_count > res->failover_count)
            res->failover_count = failover_count;
        bool incoming_conflict = state == LCS_RES_CONFLICT;
        bool local_conflict = res->state == LCS_RES_CONFLICT;
        bool newer_epoch = epoch > res->epoch;
        bool same_lease = epoch == res->epoch &&
                          lease_id != 0 &&
                          lease_id == res->lease_id &&
                          owner_instance_id == res->owner_instance_id &&
                          (owner == UINT16_MAX ? res->owner_node < 0 : res->owner_node == (int)owner);
        bool preserve_local_transition = same_lease &&
                                         owner == (uint16_t)g_state.self_index &&
                                         owner_instance_id == g_state.instance_id &&
                                         (res->state == LCS_RES_STARTING ||
                                          res->state == LCS_RES_STOPPING);
        bool conflict_update = incoming_conflict && epoch >= res->epoch;
        if (local_conflict && !incoming_conflict && epoch <= res->epoch)
            continue;
        if (newer_epoch || same_lease || conflict_update)
        {
            if (res->owner_node == g_state.self_index &&
                res->owner_instance_id == g_state.instance_id &&
                owner != (uint16_t)g_state.self_index &&
                res->state == LCS_RES_ACTIVE)
                cluster_stop_local_resource(id);
            res->epoch = epoch;
            res->lease_id = lease_id;
            res->owner_node = owner == UINT16_MAX ? -1 : (int)owner;
            res->owner_instance_id = owner == UINT16_MAX ? 0 : owner_instance_id;
            if (!preserve_local_transition)
                res->state = (lcs_resource_state_t)state;
            uint64_t incoming_deadline_ms = remaining_ms ? lcs_now_ms() + remaining_ms : 0;
            if (same_lease && incoming_deadline_ms && res->lease_deadline_ms &&
                incoming_deadline_ms < res->lease_deadline_ms)
            {
                lcs_log_debug3("state sync for VIP %s kept later local deadline for same lease epoch=%llu",
                               g_state.cfg.vips[id].name, (unsigned long long)epoch);
            } else
                res->lease_deadline_ms = incoming_deadline_ms;
            snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s",
                     incoming_conflict ? reason : "");
        }
    }
    return 0;
}
