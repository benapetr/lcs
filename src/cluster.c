// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cluster.h"

#include "log.h"
#include "util.h"
#include "vip.h"

#include <stdio.h>
#include <string.h>

bool node_is_online(const daemon_state_t *st, size_t node_idx)
{
    if ((int)node_idx == st->self_index)
        return true;
    if (node_idx >= st->cfg.node_count || !st->peers[node_idx].online)
        return false;
    return lcs_now_ms() - st->peers[node_idx].last_seen_ms <= st->cfg.peer_timeout_ms;
}

int first_online_full_member(const daemon_state_t *st)
{
    int best = -1;
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if (st->cfg.nodes[i].role != LCS_NODE_FULL || !node_is_online(st, i))
            continue;
        if (best < 0 || strcmp(st->cfg.nodes[i].name, st->cfg.nodes[best].name) < 0)
            best = (int)i;
    }
    return best;
}

const char *node_name_or_none(const daemon_state_t *st, int node_idx)
{
    if (node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return "-";
    return st->cfg.nodes[node_idx].name;
}

int has_quorum(const daemon_state_t *st)
{
    return st->votes_seen >= st->quorum_needed;
}

void recompute_votes(daemon_state_t *st)
{
    uint32_t votes = 1;
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        if (st->peers[i].online && now - st->peers[i].last_seen_ms <= st->cfg.peer_timeout_ms)
        {
            votes++;
        } else
        {
            if (st->peers[i].online)
                lcs_log_info("peer %s offline", st->cfg.nodes[i].name);
            st->peers[i].online = false;
        }
    }
    st->votes_seen = votes;
}

uint32_t peer_handshake_timeout_ms(const daemon_state_t *st)
{
    return st->cfg.peer_timeout_ms < LCS_DEFAULT_HANDSHAKE_TIMEOUT_MS ?
           st->cfg.peer_timeout_ms : LCS_DEFAULT_HANDSHAKE_TIMEOUT_MS;
}

int encode_state(const daemon_state_t *st, unsigned char *payload, size_t cap, size_t *len)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u64(&w, st->instance_id) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)st->cfg.vip_count) != 0)
        return -1;

    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        const resource_runtime_t *res = &st->resources[i];
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
            lcs_buf_put_fixed_string(&w, res->conflict_reason, LCS_REASON_MAX + 1) != 0)
            return -1;
    }
    *len = w.len;
    return 0;
}

int apply_state(daemon_state_t *st, const void *payload, size_t len, int source_node_idx)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint64_t sender_instance_id;
    uint16_t count;
    if (lcs_buf_get_u64(&r, &sender_instance_id) != 0 ||
        lcs_buf_get_u16(&r, &count) != 0 ||
        count != st->cfg.vip_count)
        return -1;

    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= st->cfg.node_count ||
         sender_instance_id != st->peers[source_node_idx].instance_id))
        return -1;

    for (uint16_t n = 0; n < count; n++)
    {
        uint16_t id, owner;
        uint8_t state;
        uint64_t owner_instance_id, epoch, lease_id, remaining_ms, failover_count;
        char reason[LCS_REASON_MAX + 1];
        if (lcs_buf_get_u16(&r, &id) != 0 ||
            lcs_buf_get_u16(&r, &owner) != 0 ||
            lcs_buf_get_u64(&r, &owner_instance_id) != 0 ||
            lcs_buf_get_u8(&r, &state) != 0 ||
            lcs_buf_get_u64(&r, &epoch) != 0 ||
            lcs_buf_get_u64(&r, &lease_id) != 0 ||
            lcs_buf_get_u64(&r, &remaining_ms) != 0 ||
            lcs_buf_get_u64(&r, &failover_count) != 0 ||
            lcs_buf_get_fixed_string(&r, reason, sizeof(reason), LCS_REASON_MAX + 1) != 0 ||
            id >= st->cfg.vip_count)
            return -1;

        resource_runtime_t *res = &st->resources[id];
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
                                         owner == (uint16_t)st->self_index &&
                                         owner_instance_id == st->instance_id &&
                                         (res->state == LCS_RES_STARTING ||
                                          res->state == LCS_RES_STOPPING);
        bool conflict_update = incoming_conflict && epoch >= res->epoch;
        if (local_conflict && !incoming_conflict && epoch <= res->epoch)
            continue;
        if (newer_epoch || same_lease || conflict_update)
        {
            if (res->owner_node == st->self_index &&
                res->owner_instance_id == st->instance_id &&
                owner != (uint16_t)st->self_index &&
                res->state == LCS_RES_ACTIVE)
                lcs_vip_del(&st->cfg.vips[id]);
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
                               st->cfg.vips[id].name, (unsigned long long)epoch);
            } else
                res->lease_deadline_ms = incoming_deadline_ms;
            snprintf(res->conflict_reason, sizeof(res->conflict_reason), "%s",
                     incoming_conflict ? reason : "");
        }
    }
    return 0;
}
