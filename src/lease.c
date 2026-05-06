// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "lease.h"

#include "cluster.h"
#include "log.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
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
        *resource_id >= g_state.cfg.vip_count ||
        *owner_node >= g_state.cfg.node_count ||
        g_state.cfg.nodes[*owner_node].role != LCS_NODE_FULL)
        return -1;
    return 0;
}

int lease_accept_message(uint16_t type, const void *payload, size_t len, int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (lease_decode_msg(payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0)
    {
        lcs_log_debug("rejecting lease message type=%u from %s: invalid payload length=%zu",
                      type, cluster_node_name_or_none(source_node_idx), len);
        return -1;
    }
    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= g_state.cfg.node_count ||
         sender_instance_id != g_state.peers[source_node_idx].instance_id))
    {
        lcs_log_debug("rejecting lease message type=%u from %s: instance mismatch sender=%llu expected=%llu",
                      type, cluster_node_name_or_none(source_node_idx),
                      (unsigned long long)sender_instance_id,
                      source_node_idx >= 0 && (size_t)source_node_idx < g_state.cfg.node_count ?
                      (unsigned long long)g_state.peers[source_node_idx].instance_id : 0ull);
        return -1;
    }
    resource_runtime_t *res = &g_state.resources[resource_id];
    if (res->state == LCS_RES_CONFLICT)
    {
        lcs_log_debug("rejecting lease message type=%u for VIP %s from %s: local conflict state",
                      type, g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx));
        return -1;
    }
    if (type == LCS_MSG_LEASE_RELEASE)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->lease_id != 0 && lease_id != res->lease_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: lease_id mismatch got=%llu local=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          (unsigned long long)lease_id, (unsigned long long)res->lease_id);
            return -1;
        }
        if (epoch == res->epoch && res->owner_instance_id != 0 &&
            sender_instance_id != res->owner_instance_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: owner instance mismatch got=%llu local=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
        if (res->owner_node == (int)owner_node || epoch > res->epoch)
        {
            if (res->owner_node == g_state.self_index &&
                res->owner_instance_id == g_state.instance_id &&
                res->state == LCS_RES_ACTIVE)
                lcs_vip_del(&g_state.cfg.vips[resource_id]);
            res->epoch = epoch;
            res->lease_id = 0;
            res->owner_node = -1;
            res->owner_instance_id = 0;
            res->state = LCS_RES_STOPPED;
            res->lease_deadline_ms = 0;
            res->renew_after_ms = 0;
            res->conflict_reason[0] = '\0';
        }
        return 0;
    }
    if (type == LCS_MSG_LEASE_RENEW)
    {
        if (epoch != res->epoch || lease_id != res->lease_id ||
            res->owner_node != (int)owner_node ||
            sender_instance_id != res->owner_instance_id)
        {
            lcs_log_debug("rejecting lease renew for VIP %s from %s: got epoch=%llu lease=%llu owner=%s instance=%llu local epoch=%llu lease=%llu owner=%s instance=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)lease_id,
                          cluster_node_name_or_none(owner_node),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->epoch, (unsigned long long)res->lease_id,
                          cluster_node_name_or_none(res->owner_node),
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else if (type == LCS_MSG_LEASE_REQ)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->owner_node >= 0 &&
            (res->owner_node != (int)owner_node || res->lease_id != lease_id ||
             res->owner_instance_id != sender_instance_id))
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: epoch collision got owner=%s lease=%llu instance=%llu local owner=%s lease=%llu instance=%llu",
                          g_state.cfg.vips[resource_id].name, cluster_node_name_or_none(source_node_idx),
                          cluster_node_name_or_none(owner_node), (unsigned long long)lease_id,
                          (unsigned long long)sender_instance_id,
                          cluster_node_name_or_none(res->owner_node), (unsigned long long)res->lease_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else
        return -1;
    if (res->owner_node == g_state.self_index &&
        res->owner_instance_id == g_state.instance_id &&
        owner_node != (uint16_t)g_state.self_index &&
        res->state == LCS_RES_ACTIVE)
        lcs_vip_del(&g_state.cfg.vips[resource_id]);
    uint64_t now = lcs_now_ms();
    res->epoch = epoch;
    res->lease_id = lease_id;
    res->owner_node = owner_node;
    res->owner_instance_id = sender_instance_id;
    if (type != LCS_MSG_LEASE_RENEW ||
        (res->state != LCS_RES_STARTING && res->state != LCS_RES_STOPPING))
        res->state = LCS_RES_ACTIVE;
    res->lease_deadline_ms = now + lease_ms;
    res->renew_after_ms = now + (lease_ms / 2u);
    res->conflict_reason[0] = '\0';
    return 0;
}

static void lease_op_clear(lease_runtime_t *op)
{
    memset(op, 0, sizeof(*op));
    op->vip_idx = -1;
    op->owner_idx = -1;
}

static lease_runtime_t *lease_op_alloc(int vip_idx, lease_op_type_t type)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active)
        {
            if (g_state.lease_ops[i].vip_idx == vip_idx)
                return NULL;
            continue;
        }
        lease_op_clear(&g_state.lease_ops[i]);
        g_state.lease_ops[i].active = true;
        g_state.lease_ops[i].id = ++g_state.next_lease_op_id;
        if (!g_state.lease_ops[i].id)
            g_state.lease_ops[i].id = ++g_state.next_lease_op_id;
        g_state.lease_ops[i].vip_idx = vip_idx;
        g_state.lease_ops[i].type = type;
        return &g_state.lease_ops[i];
    }
    return NULL;
}

bool lease_operation_active(int vip_idx)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active && g_state.lease_ops[i].vip_idx == vip_idx)
            return true;
    }
    return false;
}

void lease_cancel_operations(int vip_idx)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active && g_state.lease_ops[i].vip_idx == vip_idx)
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
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)op->vip_idx,
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

static int lease_start_operation(int epoll_fd, lease_op_type_t type, int vip_idx, int owner_idx, uint64_t epoch, uint64_t lease_id)
{
    lease_runtime_t *op = lease_op_alloc(vip_idx, type);
    if (!op)
        return -1;
    op->owner_idx = owner_idx;
    op->epoch = epoch;
    op->lease_id = lease_id;
    op->votes = type == LCS_LEASE_OP_RELEASE ? 0 : 1;
    op->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;
        (void)lease_op_send_to_peer(epoll_fd, op, (int)i);
    }
    if (op->pending_rpcs == 0 && type == LCS_LEASE_OP_RELEASE)
        lease_op_clear(op);
    return 0;
}

int lease_start_acquire(int vip_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    if (!cluster_has_quorum())
        return -1;

    return lease_start_operation(epoll_fd, LCS_LEASE_OP_ACQUIRE, vip_idx, owner_idx, epoch, lease_id);
}

int lease_start_renew(int vip_idx, int epoll_fd)
{
    resource_runtime_t *res = &g_state.resources[vip_idx];
    return lease_start_operation(epoll_fd, LCS_LEASE_OP_RENEW, vip_idx, g_state.self_index, res->epoch, res->lease_id);
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
            if (op->type != LCS_LEASE_OP_RELEASE)
                op->votes++;
        }
    }
}

static void lease_finish_acquire(int epoll_fd, lease_runtime_t *op)
{
    resource_runtime_t *res = &g_state.resources[op->vip_idx];
    if (!cluster_has_quorum())
    {
        lcs_log_warn("discarding lease acquire for VIP %s epoch=%llu because quorum is lost",
                     g_state.cfg.vips[op->vip_idx].name,
                     (unsigned long long)op->epoch);
        lease_op_send_release_to_acked(epoll_fd, op);
        res->next_activation_attempt_ms = lcs_now_ms() + g_state.cfg.renew_ms;
        if (op->pending_rpcs > 0)
            return;
        lease_op_clear(op);
        return;
    }
    if (res->state == LCS_RES_CONFLICT ||
        res->epoch > op->epoch ||
        (res->owner_node >= 0 &&
         (res->owner_node != op->owner_idx ||
          res->owner_instance_id != g_state.instance_id ||
          res->lease_id != op->lease_id ||
          res->epoch > op->epoch)))
    {
        lcs_log_warn("discarding stale lease acquire for VIP %s epoch=%llu state=%u owner=%s local_epoch=%llu",
                     g_state.cfg.vips[op->vip_idx].name,
                     (unsigned long long)op->epoch, (unsigned)res->state,
                     cluster_node_name_or_none(res->owner_node),
                     (unsigned long long)res->epoch);
        lease_op_send_release_to_acked(epoll_fd, op);
        if (op->pending_rpcs > 0)
            return;
        lease_op_clear(op);
        return;
    }
    if ((uint32_t)op->votes >= g_state.quorum_needed)
    {
        uint64_t now = lcs_now_ms();
        res->epoch = op->epoch;
        res->lease_id = op->lease_id;
        res->owner_node = op->owner_idx;
        res->owner_instance_id = g_state.instance_id;
        res->state = LCS_RES_ACTIVE;
        res->lease_deadline_ms = now + g_state.cfg.lease_ms;
        res->renew_after_ms = now + g_state.cfg.renew_ms;
        res->conflict_reason[0] = '\0';
        lcs_log_debug("lease acquired for VIP %s epoch=%llu votes=%d need=%u",
                      g_state.cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      g_state.quorum_needed);
        if (resources_activate_acquired_local(op->vip_idx, op->epoch, op->lease_id, epoll_fd) != 0)
            res->next_activation_attempt_ms = now + g_state.cfg.lease_ms;
        if (op->type == LCS_LEASE_OP_RELEASE)
            return;
    } else
    {
        lcs_log_debug("lease acquire failed for VIP %s epoch=%llu votes=%d need=%u",
                      g_state.cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      g_state.quorum_needed);
        lease_op_send_release_to_acked(epoll_fd, op);
        res->next_activation_attempt_ms = lcs_now_ms() + g_state.cfg.renew_ms;
        if (op->pending_rpcs > 0)
            return;
    }
    lease_op_clear(op);
}

static void lease_finish_renew(int epoll_fd, lease_runtime_t *op)
{
    resource_runtime_t *res = &g_state.resources[op->vip_idx];
    uint64_t now = lcs_now_ms();
    if (res->owner_node != g_state.self_index ||
        res->owner_instance_id != g_state.instance_id ||
        res->epoch != op->epoch ||
        res->lease_id != op->lease_id)
    {
        lcs_log_debug("discarding stale lease renew result for VIP %s epoch=%llu lease=%llu",
                      g_state.cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch,
                      (unsigned long long)op->lease_id);
        lease_op_clear(op);
        return;
    }
    if (!cluster_has_quorum())
    {
        lcs_log_warn("dropping VIP %s because quorum is lost", g_state.cfg.vips[op->vip_idx].name);
        resources_drop_local(op->vip_idx, epoll_fd);
        lease_op_clear(op);
        return;
    }
    if ((uint32_t)op->votes >= g_state.quorum_needed)
    {
        res->lease_deadline_ms = now + g_state.cfg.lease_ms;
        res->renew_after_ms = now + g_state.cfg.renew_ms;
        lcs_log_debug("renewed VIP %s lease epoch=%llu votes=%d",
                      g_state.cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes);
    } else if (now + g_state.cfg.renew_ms >= res->lease_deadline_ms)
    {
        lcs_log_warn("dropping VIP %s because lease renewal failed", g_state.cfg.vips[op->vip_idx].name);
        resources_drop_local(op->vip_idx, epoll_fd);
    } else
    {
        res->renew_after_ms = now + g_state.cfg.renew_ms;
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
        if (op->pending_rpcs > 0 && op->deadline_ms && now < op->deadline_ms)
            continue;
        if (op->type == LCS_LEASE_OP_ACQUIRE)
            lease_finish_acquire(epoll_fd, op);
        else if (op->type == LCS_LEASE_OP_RENEW)
            lease_finish_renew(epoll_fd, op);
        else
            lease_op_clear(op);
    }
}

void lease_release_majority(int vip_idx, int owner_idx, uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        lease_runtime_t *op = &g_state.lease_ops[i];
        if (!op->active || op->vip_idx != vip_idx)
            continue;
        op->type = LCS_LEASE_OP_RELEASE;
        op->owner_idx = owner_idx;
        op->epoch = epoch;
        op->lease_id = lease_id;
        op->pending_rpcs = 0;
        op->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
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
    (void)lease_start_operation(epoll_fd, LCS_LEASE_OP_RELEASE, vip_idx, owner_idx, epoch, lease_id);
}

int lease_handle_owner_release_request(const void *payload, size_t len, int source_node_idx)
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
    if (res->owner_node == -1 && res->state == LCS_RES_STOPPED && res->epoch >= epoch)
        return 0;

    if (res->owner_node != g_state.self_index ||
        res->owner_instance_id != g_state.instance_id ||
        res->state != LCS_RES_ACTIVE ||
        res->epoch != epoch ||
        res->lease_id != lease_id)
        return -1;

    if (lcs_vip_del(&g_state.cfg.vips[resource_id]) != 0)
        return -1;

    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    res->next_activation_attempt_ms = lcs_now_ms() + g_state.cfg.lease_ms;
    lease_cancel_operations((int)resource_id);
    lcs_log_info("released VIP %s for controlled handoff at epoch=%llu", g_state.cfg.vips[resource_id].name, (unsigned long long)epoch);
    return 0;
}

void lease_expire_remote(void)
{
    uint64_t now = lcs_now_ms();
    uint64_t grace_ms = g_state.cfg.renew_ms ? g_state.cfg.renew_ms : 1000u;
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
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
        lcs_log_warn("clearing expired remote lease for VIP %s owner=%s epoch=%llu expired_ms=%llu",
                     g_state.cfg.vips[i].name,
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
