// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "lease.h"

#include "cluster.h"
#include "log.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "systemd_service.h"
#include "util.h"
#include "vip.h"

#include <errno.h>
#include <string.h>

int lease_encode_msg(unsigned char *payload, size_t cap, size_t *len,
                     uint16_t resource_id, uint16_t owner_node,
                     uint64_t epoch, uint64_t lease_id, uint32_t lease_ms,
                     uint64_t sender_instance_id)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u16(&w, resource_id) != 0 ||
        lcs_buf_put_u16(&w, owner_node) != 0 ||
        lcs_buf_put_u64(&w, epoch) != 0 ||
        lcs_buf_put_u64(&w, lease_id) != 0 ||
        lcs_buf_put_u32(&w, lease_ms) != 0 ||
        lcs_buf_put_u64(&w, sender_instance_id) != 0)
        return -1;
    *len = w.len;
    return 0;
}

int lease_decode_msg(const void *payload, size_t len,
                     uint16_t *resource_id, uint16_t *owner_node,
                     uint64_t *epoch, uint64_t *lease_id, uint32_t *lease_ms,
                     uint64_t *sender_instance_id)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    if (lcs_buf_get_u16(&r, resource_id) != 0 ||
        lcs_buf_get_u16(&r, owner_node) != 0 ||
        lcs_buf_get_u64(&r, epoch) != 0 ||
        lcs_buf_get_u64(&r, lease_id) != 0 ||
        lcs_buf_get_u32(&r, lease_ms) != 0 ||
        lcs_buf_get_u64(&r, sender_instance_id) != 0 ||
        *resource_id >= g_state.cfg.resource_count ||
        *owner_node >= g_state.cfg.node_count ||
        g_state.cfg.nodes[*owner_node].role != LCS_NODE_FULL)
        return -1;
    return 0;
}

static bool lease_identity_matches(const lease_grant_t *grant, int owner_node,
                                   uint64_t owner_instance_id, uint64_t epoch,
                                   uint64_t lease_id)
{
    return grant->active && grant->owner_node == owner_node &&
           grant->owner_instance_id == owner_instance_id &&
           grant->epoch == epoch && grant->lease_id == lease_id;
}

static void lease_expire_grant(lease_grant_t *grant, uint64_t now)
{
    if (grant->active && grant->deadline_ms && now >= grant->deadline_ms)
    {
        grant->active = false;
        grant->owner_node = -1;
        grant->owner_instance_id = 0;
        grant->lease_id = 0;
        grant->deadline_ms = 0;
    }
}

static int lease_grant_acquire(int resource_idx, int owner_idx,
                               uint64_t owner_instance_id, uint64_t epoch,
                               uint64_t lease_id, uint64_t deadline_ms)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    lease_grant_t *grant = &g_state.lease_grants[resource_idx];
    uint64_t now = lcs_now_ms();
    lease_expire_grant(grant, now);

    if (epoch < grant->promised_epoch || epoch < res->epoch)
        return -1;
    if (grant->active)
        return lease_identity_matches(grant, owner_idx, owner_instance_id, epoch, lease_id) ? 0 : -1;
    if (epoch == res->epoch && res->owner_node >= 0 && (res->owner_node != owner_idx || res->owner_instance_id != owner_instance_id || res->lease_id != lease_id))
        return -1;
    if (res->owner_node == g_state.self_index && res->owner_instance_id == g_state.instance_id && res->state != LCS_RES_STOPPED && owner_idx != g_state.self_index)
        return -1;

    grant->active = true;
    grant->owner_node = owner_idx;
    grant->owner_instance_id = owner_instance_id;
    grant->epoch = epoch;
    grant->lease_id = lease_id;
    grant->deadline_ms = deadline_ms;
    if (epoch > grant->promised_epoch)
        grant->promised_epoch = epoch;
    return 0;
}

int lease_grant_local_acquire(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, uint64_t deadline_ms)
{
    if (resource_idx < 0 || (size_t)resource_idx >= g_state.cfg.resource_count || owner_idx != g_state.self_index || !cluster_local_voting_ready())
        return -1;
    return lease_grant_acquire(resource_idx, owner_idx, g_state.instance_id, epoch, lease_id, deadline_ms);
}

void lease_grant_local_release(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id)
{
    if (resource_idx < 0 || (size_t)resource_idx >= g_state.cfg.resource_count)
        return;

    lease_grant_t *grant = &g_state.lease_grants[resource_idx];
    if (lease_identity_matches(grant, owner_idx, g_state.instance_id, epoch, lease_id))
    {
        grant->active = false;
        grant->owner_node = -1;
        grant->owner_instance_id = 0;
        grant->lease_id = 0;
        grant->deadline_ms = 0;
        if (epoch != UINT64_MAX && epoch + 1 > grant->promised_epoch)
            grant->promised_epoch = epoch + 1;
    }
}

int lease_accept_message(uint16_t type, const void *payload, size_t len, int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (lease_decode_msg(payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0 ||
        source_node_idx < 0 || owner_node != (uint16_t)source_node_idx ||
        (size_t)source_node_idx >= g_state.cfg.node_count ||
        sender_instance_id != g_state.peers[source_node_idx].instance_id)
        return -1;

    resource_runtime_t *res = &g_state.resources[resource_id];
    lease_grant_t *grant = &g_state.lease_grants[resource_id];
    uint64_t now = lcs_now_ms();
    lease_expire_grant(grant, now);

    if (res->state == LCS_RES_CONFLICT || res->state == LCS_RES_STOP_FAILED)
        return -1;

    if (type == LCS_MSG_LEASE_RELEASE)
    {
        if (lease_identity_matches(grant, owner_node, sender_instance_id, epoch, lease_id))
        {
            grant->active = false;
            grant->owner_node = -1;
            grant->owner_instance_id = 0;
            grant->lease_id = 0;
            grant->deadline_ms = 0;
            if (epoch != UINT64_MAX && epoch + 1 > grant->promised_epoch)
                grant->promised_epoch = epoch + 1;
        }
        if (res->owner_node == (int)owner_node &&
            res->owner_instance_id == sender_instance_id &&
            res->epoch == epoch && res->lease_id == lease_id)
        {
            res->owner_node = -1;
            res->owner_instance_id = 0;
            res->state = LCS_RES_STOPPED;
            res->lease_id = 0;
            res->lease_deadline_ms = 0;
            res->renew_after_ms = 0;
        }
        return 0;
    }

    if (lease_ms != g_state.cfg.lease_ms)
        return -1;

    if (type == LCS_MSG_LEASE_REQ)
    {
        if (!cluster_local_voting_ready())
            return -1;
        return lease_grant_acquire((int)resource_id, owner_node,
                                   sender_instance_id, epoch, lease_id,
                                   now + lease_ms);
    }

    if (type == LCS_MSG_LEASE_RENEW)
    {
        bool matching_observation = res->owner_node == (int)owner_node &&
                                    res->owner_instance_id == sender_instance_id &&
                                    res->epoch == epoch && res->lease_id == lease_id &&
                                    res->state != LCS_RES_STOPPED;
        if (!lease_identity_matches(grant, owner_node, sender_instance_id,
                                    epoch, lease_id))
        {
            if (grant->active || !matching_observation ||
                lease_grant_acquire((int)resource_id, owner_node,
                                    sender_instance_id, epoch, lease_id,
                                    now + lease_ms) != 0)
                return -1;
        }
        grant->deadline_ms = now + lease_ms;
        return 0;
    }
    return -1;
}

int lease_apply_commit(const void *payload, size_t len, int source_node_idx, int epoll_fd)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t remaining_ms;
    if (lease_decode_msg(payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &remaining_ms, &sender_instance_id) != 0 ||
        source_node_idx < 0 || owner_node != (uint16_t)source_node_idx ||
        (size_t)source_node_idx >= g_state.cfg.node_count ||
        sender_instance_id != g_state.peers[source_node_idx].instance_id ||
        remaining_ms == 0 || remaining_ms > g_state.cfg.lease_ms)
        return -1;

    resource_runtime_t *res = &g_state.resources[resource_id];
    lease_grant_t *grant = &g_state.lease_grants[resource_id];
    lease_expire_grant(grant, lcs_now_ms());
    if (grant->active && !lease_identity_matches(grant, owner_node,
                                                  sender_instance_id,
                                                  epoch, lease_id))
        return -1;
    if (res->state == LCS_RES_CONFLICT || res->state == LCS_RES_STOP_FAILED ||
        epoch < res->epoch)
        return -1;

    bool same_lease = res->owner_node == (int)owner_node &&
                      res->owner_instance_id == sender_instance_id &&
                      res->epoch == epoch && res->lease_id == lease_id;
    if (epoch == res->epoch && res->owner_node >= 0 && !same_lease)
        return -1;
    if (res->owner_node == g_state.self_index &&
        res->owner_instance_id == g_state.instance_id && !same_lease &&
        res->state != LCS_RES_STOPPED)
    {
        int replacement_rc = resources_begin_state_replacement(
            (int)resource_id, owner_node, sender_instance_id,
            LCS_RES_ACTIVE, epoch, lease_id,
            lcs_now_ms() + remaining_ms, "", epoll_fd);
        if (replacement_rc > 0)
            return 0;
        if (replacement_rc < 0)
            return -1;
        if (resources_stop_local_backend(&g_state.cfg.resources[resource_id]) != 0)
        {
            resources_enter_stop_failed_state((int)resource_id, epoch + 1, "local resource stop failed while applying lease commit", epoll_fd);
            return -1;
        }
    }

    res->epoch = epoch;
    res->lease_id = lease_id;
    res->owner_node = owner_node;
    res->owner_instance_id = sender_instance_id;
    res->state = LCS_RES_ACTIVE;
    res->lease_deadline_ms = lcs_now_ms() + remaining_ms;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    return 0;
}

void lease_broadcast_commit(int epoll_fd, int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, uint64_t deadline_ms)
{
    uint64_t now = lcs_now_ms();
    if (deadline_ms <= now)
        return;
    uint64_t remaining = deadline_ms - now;
    if (remaining > g_state.cfg.lease_ms)
        remaining = g_state.cfg.lease_ms;
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = 0;
    if (lease_encode_msg(payload, sizeof(payload), &len, (uint16_t)resource_idx,
                         (uint16_t)owner_idx, epoch, lease_id,
                         (uint32_t)remaining, g_state.instance_id) == 0)
        peer_broadcast_lease_commit(epoll_fd, payload, (uint32_t)len);
}

static void lease_op_clear(lease_runtime_t *op)
{
    for (size_t i = 0; i < LCS_MAX_NODES; i++)
        peer_detach_rpcs_by_context(&op->rpc_ctx[i]);
    memset(op, 0, sizeof(*op));
    op->resource_idx = -1;
    op->owner_idx = -1;
    op->release_response_node = -1;
}

static lease_runtime_t *lease_op_alloc(int resource_idx, lease_op_type_t type)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active)
        {
            if (g_state.lease_ops[i].resource_idx == resource_idx)
                return NULL;
            continue;
        }
        lease_op_clear(&g_state.lease_ops[i]);
        g_state.lease_ops[i].active = true;
        g_state.lease_ops[i].id = ++g_state.next_lease_op_id;
        if (!g_state.lease_ops[i].id)
            g_state.lease_ops[i].id = ++g_state.next_lease_op_id;
        g_state.lease_ops[i].resource_idx = resource_idx;
        g_state.lease_ops[i].type = type;
        return &g_state.lease_ops[i];
    }
    return NULL;
}

bool lease_operation_active(int resource_idx)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active && g_state.lease_ops[i].resource_idx == resource_idx)
            return true;
    }
    return false;
}

void lease_cancel_operations(int resource_idx)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active && g_state.lease_ops[i].resource_idx == resource_idx)
            lease_op_clear(&g_state.lease_ops[i]);
    }
}

void lease_cancel_all_operations(void)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active)
            lease_op_clear(&g_state.lease_ops[i]);
    }
}

static void lease_rpc_callback(void *ctx, int status, const unsigned char *payload, uint32_t len)
{
    lease_rpc_context_t *rpc_ctx = ctx;
    lease_runtime_t *op = rpc_ctx->op;
    int node_idx = rpc_ctx->node_idx;
    if (!op || !op->active || op->id != rpc_ctx->op_id || node_idx < 0 || node_idx >= LCS_MAX_NODES)
        return;

    op->rpc_done[node_idx] = true;
    op->rpc_status[node_idx] = status;
    op->rpc_resp_len[node_idx] = 0;

    if (status == 0 && payload && len <= sizeof(op->rpc_resp[node_idx]))
    {
        memcpy(op->rpc_resp[node_idx], payload, len);
        op->rpc_resp_len[node_idx] = len;
    }

    if (op->pending_rpcs > 0)
        op->pending_rpcs--;
}

static uint16_t lease_op_message_type(lease_op_type_t type)
{
    switch (type)
    {
        case LCS_LEASE_OP_ACQUIRE:
            return LCS_MSG_LEASE_REQ;
        case LCS_LEASE_OP_RENEW:
            return LCS_MSG_LEASE_RENEW;
        case LCS_LEASE_OP_RELEASE:
            return LCS_MSG_LEASE_RELEASE;
        default:
            return LCS_MSG_ERROR;
    }
}

static int lease_op_send_to_peer(int epoll_fd, lease_runtime_t *op, int node_idx)
{
    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)op->resource_idx,
                         (uint16_t)op->owner_idx, op->epoch, op->lease_id,
                         g_state.cfg.lease_ms, g_state.instance_id) != 0)
        return -1;
    op->rpc_done[node_idx] = false;
    op->rpc_status[node_idx] = -1;
    op->rpc_resp_len[node_idx] = 0;
    op->rpc_ctx[node_idx].op = op;
    op->rpc_ctx[node_idx].op_id = op->id;
    op->rpc_ctx[node_idx].node_idx = node_idx;
    if (peer_rpc_async(epoll_fd, node_idx, lease_op_message_type(op->type),
                       req, (uint32_t)req_len, LCS_MSG_LEASE_ACK,
                       op->rpc_resp[node_idx], sizeof(op->rpc_resp[node_idx]),
                       &op->rpc_resp_len[node_idx], g_state.cfg.peer_timeout_ms,
                       lease_rpc_callback, &op->rpc_ctx[node_idx]) != 0)
        return -1;
    op->pending_rpcs++;
    return 0;
}

static void lease_op_send_release_to_acked(int epoll_fd, lease_runtime_t *op)
{
    lease_grant_local_release(op->resource_idx, op->owner_idx, op->epoch, op->lease_id);
    op->type = LCS_LEASE_OP_RELEASE;
    op->pending_rpcs = 0;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index || !op->acked[i])
            continue;
        op->rpc_done[i] = false;
        op->rpc_status[i] = -1;
        op->rpc_resp_len[i] = 0;
        (void)lease_op_send_to_peer(epoll_fd, op, (int)i);
    }
}

static int lease_start_operation(int epoll_fd, lease_op_type_t type, int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id)
{
    if (type != LCS_LEASE_OP_RELEASE && !cluster_local_voting_ready())
        return -1;

    lease_runtime_t *op = lease_op_alloc(resource_idx, type);
    
    if (!op)
        return -1;

    op->owner_idx = owner_idx;
    op->epoch = epoch;
    op->lease_id = lease_id;
    op->votes = 1;
    uint64_t now = lcs_now_ms();
    op->grant_deadline_ms = type == LCS_LEASE_OP_RELEASE ? 0 : now + g_state.cfg.lease_ms;
    op->deadline_ms = now + g_state.cfg.peer_timeout_ms;
    if (type == LCS_LEASE_OP_RELEASE)
    {
        lease_grant_local_release(resource_idx, owner_idx, epoch, lease_id);
        if (g_state.resources[resource_idx].shutdown_release_required)
            g_state.resources[resource_idx].shutdown_release_confirmed = false;
    } else if (lease_grant_local_acquire(resource_idx, owner_idx, epoch, lease_id,
                                       op->grant_deadline_ms) != 0)
    {
        lease_op_clear(op);
        return -1;
    } else if (type == LCS_LEASE_OP_RENEW)
        g_state.lease_grants[resource_idx].deadline_ms = op->grant_deadline_ms;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;
        (void)lease_op_send_to_peer(epoll_fd, op, (int)i);
    }
    return 0;
}

int lease_start_acquire(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    if (!cluster_has_quorum())
        return -1;

    return lease_start_operation(epoll_fd, LCS_LEASE_OP_ACQUIRE, resource_idx, owner_idx, epoch, lease_id);
}

int lease_start_renew(int resource_idx, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[resource_idx];
    return lease_start_operation(epoll_fd, LCS_LEASE_OP_RENEW, resource_idx, g_state.self_index, res->epoch, res->lease_id);
}

static void lease_process_result(lease_runtime_t *op)
{
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index || !op->rpc_done[i] || op->acked[i])
            continue;
        int32_t status = -1;
        char msg[128];
        if (op->rpc_status[i] == 0 && lcs_decode_simple_resp(op->rpc_resp[i], op->rpc_resp_len[i], &status, msg, sizeof(msg)) == 0 && status == 0)
        {
            op->acked[i] = true;
            op->votes++;
        }
    }
}

static void lease_finish_acquire(int epoll_fd, lease_runtime_t *op)
{
    resource_runtime_t *res = &g_state.resources[op->resource_idx];
    if (!cluster_has_quorum())
    {
        lcs_log_warn("discarding lease acquire for resource %s epoch=%llu because quorum is lost", g_state.cfg.resources[op->resource_idx].name, (unsigned long long)op->epoch);
        if (op->epoch > res->epoch)
            res->epoch = op->epoch;
        lease_op_send_release_to_acked(epoll_fd, op);
        res->next_activation_attempt_ms = lcs_now_ms() + lcs_jittered_delay_ms(g_state.cfg.renew_ms);
        if (op->pending_rpcs > 0)
            return;
        lease_op_clear(op);
        return;
    }
    if (res->state == LCS_RES_CONFLICT ||
        res->state == LCS_RES_STOP_FAILED ||
        res->epoch > op->epoch ||
        (res->owner_node >= 0 &&
         (res->owner_node != op->owner_idx ||
          res->owner_instance_id != g_state.instance_id ||
          res->lease_id != op->lease_id ||
          res->epoch > op->epoch)))
    {
        lcs_log_warn("discarding stale lease acquire for resource %s epoch=%llu state=%u owner=%s local_epoch=%llu",
                     g_state.cfg.resources[op->resource_idx].name,
                     (unsigned long long)op->epoch, (unsigned)res->state,
                     cluster_node_name_or_none(res->owner_node),
                     (unsigned long long)res->epoch);
        if (op->epoch > res->epoch)
            res->epoch = op->epoch;
        lease_op_send_release_to_acked(epoll_fd, op);
        if (op->pending_rpcs > 0)
            return;
        lease_op_clear(op);
        return;
    }
    if ((uint32_t)op->votes >= g_state.quorum_needed)
    {
        uint64_t now = lcs_now_ms();
        if (!op->grant_deadline_ms || now >= op->grant_deadline_ms)
        {
            lcs_log_warn("discarding expired lease acquire result for resource %s epoch=%llu",
                         g_state.cfg.resources[op->resource_idx].name,
                         (unsigned long long)op->epoch);
            if (op->epoch > res->epoch)
                res->epoch = op->epoch;
            lease_op_send_release_to_acked(epoll_fd, op);
            res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.renew_ms);
            if (op->pending_rpcs > 0)
                return;
            lease_op_clear(op);
            return;
        }
        res->epoch = op->epoch;
        res->lease_id = op->lease_id;
        res->owner_node = op->owner_idx;
        res->owner_instance_id = g_state.instance_id;
        res->state = LCS_RES_ACTIVE;
        res->lease_deadline_ms = op->grant_deadline_ms;
        res->renew_after_ms = now + g_state.cfg.renew_ms;
        res->conflict_reason[0] = '\0';
        lcs_log_debug("lease acquired for resource %s epoch=%llu votes=%d need=%u",
                      g_state.cfg.resources[op->resource_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      g_state.quorum_needed);
        lease_broadcast_commit(epoll_fd, op->resource_idx, op->owner_idx,
                               op->epoch, op->lease_id, op->grant_deadline_ms);
        if (resources_activate_acquired_local(op->resource_idx, op->epoch, op->lease_id, epoll_fd) != 0)
            res->next_activation_attempt_ms = now + lcs_jittered_delay_ms(g_state.cfg.lease_ms);
        if (op->type == LCS_LEASE_OP_RELEASE)
            return;
    } else
    {
        lcs_log_debug("lease acquire failed for resource %s epoch=%llu votes=%d need=%u",
                      g_state.cfg.resources[op->resource_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      g_state.quorum_needed);
        if (op->epoch > res->epoch)
            res->epoch = op->epoch;
        lease_op_send_release_to_acked(epoll_fd, op);
        res->next_activation_attempt_ms = lcs_now_ms() + lcs_jittered_delay_ms(g_state.cfg.renew_ms);
        if (op->pending_rpcs > 0)
            return;
    }
    lease_op_clear(op);
}

static void lease_finish_renew(int epoll_fd, lease_runtime_t *op)
{
    resource_runtime_t *res = &g_state.resources[op->resource_idx];
    uint64_t now = lcs_now_ms();
    if (res->owner_node != g_state.self_index ||
        res->owner_instance_id != g_state.instance_id ||
        res->epoch != op->epoch ||
        res->lease_id != op->lease_id)
    {
        lcs_log_debug("discarding stale lease renew result for resource %s epoch=%llu lease=%llu",
                      g_state.cfg.resources[op->resource_idx].name,
                      (unsigned long long)op->epoch,
                      (unsigned long long)op->lease_id);
        lease_op_clear(op);
        return;
    }
    if (!cluster_has_quorum())
    {
        lcs_log_warn("dropping resource %s because quorum is lost", g_state.cfg.resources[op->resource_idx].name);
        resources_drop_local(op->resource_idx, epoll_fd);
        lease_op_clear(op);
        return;
    }
    if ((uint32_t)op->votes >= g_state.quorum_needed)
    {
        if (op->grant_deadline_ms && now < op->grant_deadline_ms)
        {
            res->lease_deadline_ms = op->grant_deadline_ms;
            res->renew_after_ms = now + g_state.cfg.renew_ms;
            lcs_log_debug("renewed resource %s lease epoch=%llu votes=%d",
                          g_state.cfg.resources[op->resource_idx].name,
                          (unsigned long long)op->epoch, op->votes);
            lease_broadcast_commit(epoll_fd, op->resource_idx, op->owner_idx, op->epoch, op->lease_id, op->grant_deadline_ms);
        } else
        {
            lcs_log_warn("dropping resource %s because renewed grants expired before completion", g_state.cfg.resources[op->resource_idx].name);
            resources_drop_local(op->resource_idx, epoll_fd);
        }
    } else if (now + g_state.cfg.renew_ms >= res->lease_deadline_ms)
    {
        lcs_log_warn("dropping resource %s because lease renewal failed", g_state.cfg.resources[op->resource_idx].name);
        resources_drop_local(op->resource_idx, epoll_fd);
    } else
    {
        res->renew_after_ms = now + g_state.cfg.renew_ms;
    }
    lease_op_clear(op);
}

static void lease_finish_release(int epoll_fd, lease_runtime_t *op,
                                 bool operation_finished)
{
    resource_runtime_t *res = &g_state.resources[op->resource_idx];
    if ((uint32_t)op->votes >= g_state.quorum_needed &&
        res->shutdown_release_required &&
        !res->shutdown_release_confirmed)
    {
        res->shutdown_release_confirmed = true;
        lcs_log_info("shutdown release quorum confirmed for resource %s epoch=%llu votes=%d need=%u",
                     g_state.cfg.resources[op->resource_idx].name,
                     (unsigned long long)op->epoch, op->votes,
                     g_state.quorum_needed);
    }
    if (op->release_notify && !op->release_notified &&
        (uint32_t)op->votes >= g_state.quorum_needed)
    {
        if (peer_queue_simple_resp(epoll_fd, op->release_response_node,
                                   op->release_response_seq,
                                   LCS_MSG_OWNER_RELEASE_RESP, 0,
                                   "release quorum confirmed") == 0)
            lcs_log_info("release quorum confirmed for resource %s epoch=%llu votes=%d need=%u",
                         g_state.cfg.resources[op->resource_idx].name,
                         (unsigned long long)op->epoch, op->votes,
                         g_state.quorum_needed);
        op->release_notified = true;
    }

    if (!operation_finished)
        return;

    if (op->release_notify && !op->release_notified)
    {
        (void)peer_queue_simple_resp(epoll_fd, op->release_response_node,
                                     op->release_response_seq,
                                     LCS_MSG_OWNER_RELEASE_RESP, -1,
                                     "release quorum not reached");
        lcs_log_warn("release quorum failed for resource %s epoch=%llu votes=%d need=%u",
                     g_state.cfg.resources[op->resource_idx].name,
                     (unsigned long long)op->epoch, op->votes,
                     g_state.quorum_needed);
    }
    lease_op_clear(op);
}

void lease_process_operations(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        lease_runtime_t *op = &g_state.lease_ops[i];
        if (!op->active)
            continue;
        lease_process_result(op);

        if (op->type == LCS_LEASE_OP_RELEASE && op->release_notify &&
            !op->release_notified &&
            (uint32_t)op->votes >= g_state.quorum_needed)
            lease_finish_release(epoll_fd, op, false);

        // if there are still waiting for RPC results from other cluster members and we haven't reached the deadline, wait longer
        if (op->pending_rpcs > 0 && op->deadline_ms && now < op->deadline_ms)
            continue;
        
        // if we have reached the deadline or got all RPC results, process the result
        if (op->type == LCS_LEASE_OP_ACQUIRE)
            lease_finish_acquire(epoll_fd, op);
        else if (op->type == LCS_LEASE_OP_RENEW)
            lease_finish_renew(epoll_fd, op);
        else
            lease_finish_release(epoll_fd, op, true);
    }
}

void lease_release_majority(int resource_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    lease_grant_local_release(resource_idx, owner_idx, epoch, lease_id);
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        lease_runtime_t *op = &g_state.lease_ops[i];
        if (!op->active || op->resource_idx != resource_idx)
            continue;
        for (size_t n = 0; n < LCS_MAX_NODES; n++)
            peer_detach_rpcs_by_context(&op->rpc_ctx[n]);
        op->type = LCS_LEASE_OP_RELEASE;
        op->id = ++g_state.next_lease_op_id;
        if (!op->id)
            op->id = ++g_state.next_lease_op_id;
        op->owner_idx = owner_idx;
        op->epoch = epoch;
        op->lease_id = lease_id;
        op->votes = 1;
        op->pending_rpcs = 0;
        op->release_notify = false;
        op->release_notified = false;
        op->release_response_node = -1;
        op->release_response_seq = 0;
        memset(op->rpc_done, 0, sizeof(op->rpc_done));
        memset(op->rpc_status, 0, sizeof(op->rpc_status));
        memset(op->acked, 0, sizeof(op->acked));
        memset(op->rpc_resp_len, 0, sizeof(op->rpc_resp_len));
        op->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
        if (g_state.resources[resource_idx].shutdown_release_required)
            g_state.resources[resource_idx].shutdown_release_confirmed = false;
        for (size_t n = 0; n < g_state.cfg.node_count; n++)
        {
            if ((int)n == g_state.self_index)
                continue;
            op->rpc_done[n] = false;
            op->rpc_status[n] = -1;
            op->rpc_resp_len[n] = 0;
            (void)lease_op_send_to_peer(epoll_fd, op, (int)n);
        }
        if (op->pending_rpcs == 0)
            lease_op_clear(op);
        return;
    }
    (void)lease_start_operation(epoll_fd, LCS_LEASE_OP_RELEASE, resource_idx, owner_idx, epoch, lease_id);
}

int lease_handle_owner_release_request(const void *payload, size_t len,
                                       int source_node_idx, uint32_t response_seq,
                                       int epoll_fd)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (lease_decode_msg(payload, len, &resource_id, &owner_node, &epoch, &lease_id, &lease_ms, &sender_instance_id) != 0)
        return -1;

    (void)lease_ms;
    if (source_node_idx >= 0 && ((size_t)source_node_idx >= g_state.cfg.node_count || sender_instance_id != g_state.peers[source_node_idx].instance_id))
        return -1;

    if (owner_node != (uint16_t)g_state.self_index)
        return -1;

    resource_runtime_t *res = &g_state.resources[resource_id];
    if (res->owner_node != g_state.self_index ||
        res->owner_instance_id != g_state.instance_id ||
        res->state != LCS_RES_ACTIVE ||
        res->epoch != epoch ||
        res->lease_id != lease_id)
        return -1;

    int stop_rc = resources_release_for_handoff((int)resource_id, epoch,
                                                lease_id, source_node_idx,
                                                response_seq, epoll_fd);
    if (stop_rc < 0)
        return -1;
    if (stop_rc > 0)
    {
        lcs_log_info("asynchronous stop started for controlled handoff of resource %s epoch=%llu",
                     g_state.cfg.resources[resource_id].name,
                     (unsigned long long)epoch);
        return 1;
    }
    return lease_complete_owner_release((int)resource_id, g_state.self_index,
                                        epoch, lease_id, source_node_idx,
                                        response_seq, epoll_fd);
}

int lease_complete_owner_release(int resource_idx, int owner_idx,
                                 uint64_t epoch, uint64_t lease_id,
                                 int source_node_idx, uint32_t response_seq,
                                 int epoll_fd)
{
    if (lease_start_operation(epoll_fd, LCS_LEASE_OP_RELEASE,
                              resource_idx, owner_idx, epoch, lease_id) != 0)
        return -1;
    lease_runtime_t *release = NULL;
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active &&
            g_state.lease_ops[i].type == LCS_LEASE_OP_RELEASE &&
            g_state.lease_ops[i].resource_idx == resource_idx)
        {
            release = &g_state.lease_ops[i];
            break;
        }
    }
    if (!release)
        return -1;
    release->release_notify = true;
    release->release_response_node = source_node_idx;
    release->release_response_seq = response_seq;
    lcs_log_info("resource %s stopped for controlled handoff; waiting for release quorum at epoch=%llu",
                 g_state.cfg.resources[resource_idx].name,
                 (unsigned long long)epoch);
    return 1;
}

void lease_expire_remote(void)
{
    uint64_t now = lcs_now_ms();
    uint64_t grace_ms = g_state.cfg.renew_ms ? g_state.cfg.renew_ms : 1000u;
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        lease_expire_grant(&g_state.lease_grants[i], now);
        resource_runtime_t *res = &g_state.resources[i];
        if ((res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING) ||
            res->owner_node < 0 ||
            (res->owner_node == g_state.self_index &&
             res->owner_instance_id == g_state.instance_id))
            continue;
        if (res->lease_deadline_ms && now < res->lease_deadline_ms + grace_ms)
            continue;
        lcs_log_warn("clearing expired remote lease for resource %s owner=%s epoch=%llu expired_ms=%llu",
                     g_state.cfg.resources[i].name,
                     cluster_node_name_or_none(res->owner_node),
                     (unsigned long long)res->epoch,
                     res->lease_deadline_ms && now > res->lease_deadline_ms ?
                     (unsigned long long)(now - res->lease_deadline_ms) : 0ull);
        res->owner_node = -1;
        res->owner_instance_id = 0;
        res->state = LCS_RES_STOPPED;
        res->lease_id = 0;
        res->lease_deadline_ms = 0;
        res->renew_after_ms = 0;
        res->failover_pending = true;
        res->conflict_reason[0] = '\0';
    }
}
