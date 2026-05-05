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

int lease_decode_msg(const daemon_state_t *st, const void *payload, size_t len,
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
        *resource_id >= st->cfg.vip_count ||
        *owner_node >= st->cfg.node_count ||
        st->cfg.nodes[*owner_node].role != LCS_NODE_FULL)
        return -1;
    return 0;
}

int lease_accept_message(daemon_state_t *st, uint16_t type, const void *payload,
                         size_t len, int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (lease_decode_msg(st, payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0)
    {
        lcs_log_debug("rejecting lease message type=%u from %s: invalid payload length=%zu",
                      type, node_name_or_none(st, source_node_idx), len);
        return -1;
    }
    if (source_node_idx >= 0 &&
        ((size_t)source_node_idx >= st->cfg.node_count ||
         sender_instance_id != st->peers[source_node_idx].instance_id))
    {
        lcs_log_debug("rejecting lease message type=%u from %s: instance mismatch sender=%llu expected=%llu",
                      type, node_name_or_none(st, source_node_idx),
                      (unsigned long long)sender_instance_id,
                      source_node_idx >= 0 && (size_t)source_node_idx < st->cfg.node_count ?
                      (unsigned long long)st->peers[source_node_idx].instance_id : 0ull);
        return -1;
    }
    resource_runtime_t *res = &st->resources[resource_id];
    if (res->state == LCS_RES_CONFLICT)
    {
        lcs_log_debug("rejecting lease message type=%u for VIP %s from %s: local conflict state",
                      type, st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx));
        return -1;
    }
    if (type == LCS_MSG_LEASE_RELEASE)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->lease_id != 0 && lease_id != res->lease_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: lease_id mismatch got=%llu local=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)lease_id, (unsigned long long)res->lease_id);
            return -1;
        }
        if (epoch == res->epoch && res->owner_instance_id != 0 &&
            sender_instance_id != res->owner_instance_id)
        {
            lcs_log_debug("rejecting lease release for VIP %s from %s: owner instance mismatch got=%llu local=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
        if (res->owner_node == (int)owner_node || epoch > res->epoch)
        {
            if (res->owner_node == st->self_index &&
                res->owner_instance_id == st->instance_id &&
                res->state == LCS_RES_ACTIVE)
                lcs_vip_del(&st->cfg.vips[resource_id]);
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
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)lease_id,
                          node_name_or_none(st, owner_node),
                          (unsigned long long)sender_instance_id,
                          (unsigned long long)res->epoch, (unsigned long long)res->lease_id,
                          node_name_or_none(st, res->owner_node),
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else if (type == LCS_MSG_LEASE_REQ)
    {
        if (epoch < res->epoch)
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: stale epoch=%llu local_epoch=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          (unsigned long long)epoch, (unsigned long long)res->epoch);
            return -1;
        }
        if (epoch == res->epoch && res->owner_node >= 0 &&
            (res->owner_node != (int)owner_node || res->lease_id != lease_id ||
             res->owner_instance_id != sender_instance_id))
        {
            lcs_log_debug("rejecting lease request for VIP %s from %s: epoch collision got owner=%s lease=%llu instance=%llu local owner=%s lease=%llu instance=%llu",
                          st->cfg.vips[resource_id].name, node_name_or_none(st, source_node_idx),
                          node_name_or_none(st, owner_node), (unsigned long long)lease_id,
                          (unsigned long long)sender_instance_id,
                          node_name_or_none(st, res->owner_node), (unsigned long long)res->lease_id,
                          (unsigned long long)res->owner_instance_id);
            return -1;
        }
    } else
        return -1;
    if (res->owner_node == st->self_index &&
        res->owner_instance_id == st->instance_id &&
        owner_node != (uint16_t)st->self_index &&
        res->state == LCS_RES_ACTIVE)
        lcs_vip_del(&st->cfg.vips[resource_id]);
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

static lease_runtime_t *lease_op_alloc(daemon_state_t *st, int vip_idx,
                                       lease_op_type_t type)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (st->lease_ops[i].active)
        {
            if (st->lease_ops[i].vip_idx == vip_idx)
                return NULL;
            continue;
        }
        lease_op_clear(&st->lease_ops[i]);
        st->lease_ops[i].active = true;
        st->lease_ops[i].id = ++st->next_lease_op_id;
        if (!st->lease_ops[i].id)
            st->lease_ops[i].id = ++st->next_lease_op_id;
        st->lease_ops[i].vip_idx = vip_idx;
        st->lease_ops[i].type = type;
        return &st->lease_ops[i];
    }
    return NULL;
}

bool lease_operation_active(const daemon_state_t *st, int vip_idx)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (st->lease_ops[i].active && st->lease_ops[i].vip_idx == vip_idx)
            return true;
    }
    return false;
}

static void lease_rpc_callback(void *ctx, int status,
                               const unsigned char *payload, uint32_t len)
{
    lease_rpc_context_t *rpc_ctx = ctx;
    lease_runtime_t *op = rpc_ctx->op;
    int node_idx = rpc_ctx->node_idx;
    if (!op || !op->active || op->id != rpc_ctx->op_id ||
        node_idx < 0 || node_idx >= LCS_MAX_NODES)
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

static int lease_op_send_to_peer(daemon_state_t *st, int epoll_fd,
                                 lease_runtime_t *op, int node_idx)
{
    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)op->vip_idx,
                         (uint16_t)op->owner_idx, op->epoch, op->lease_id,
                         st->cfg.lease_ms, st->instance_id) != 0)
        return -1;
    op->rpc_done[node_idx] = false;
    op->rpc_status[node_idx] = -1;
    op->rpc_resp_len[node_idx] = 0;
    op->rpc_ctx[node_idx].op = op;
    op->rpc_ctx[node_idx].op_id = op->id;
    op->rpc_ctx[node_idx].node_idx = node_idx;
    if (peer_rpc_async(st, epoll_fd, node_idx, lease_op_message_type(op->type),
                       req, (uint32_t)req_len, LCS_MSG_LEASE_ACK,
                       op->rpc_resp[node_idx], sizeof(op->rpc_resp[node_idx]),
                       &op->rpc_resp_len[node_idx], st->cfg.peer_timeout_ms,
                       lease_rpc_callback, &op->rpc_ctx[node_idx]) != 0)
        return -1;
    op->pending_rpcs++;
    return 0;
}

static void lease_op_send_release_to_acked(daemon_state_t *st, int epoll_fd,
                                           lease_runtime_t *op)
{
    op->type = LCS_LEASE_OP_RELEASE;
    op->pending_rpcs = 0;
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index || !op->acked[i])
            continue;
        op->rpc_done[i] = false;
        op->rpc_status[i] = -1;
        op->rpc_resp_len[i] = 0;
        (void)lease_op_send_to_peer(st, epoll_fd, op, (int)i);
    }
}

static int lease_start_operation(daemon_state_t *st, int epoll_fd,
                                 lease_op_type_t type, int vip_idx,
                                 int owner_idx, uint64_t epoch,
                                 uint64_t lease_id)
{
    lease_runtime_t *op = lease_op_alloc(st, vip_idx, type);
    if (!op)
        return -1;
    op->owner_idx = owner_idx;
    op->epoch = epoch;
    op->lease_id = lease_id;
    op->votes = type == LCS_LEASE_OP_RELEASE ? 0 : 1;
    op->deadline_ms = lcs_now_ms() + st->cfg.peer_timeout_ms;
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        (void)lease_op_send_to_peer(st, epoll_fd, op, (int)i);
    }
    if (op->pending_rpcs == 0 && type == LCS_LEASE_OP_RELEASE)
        lease_op_clear(op);
    return 0;
}

int lease_start_acquire(daemon_state_t *st, int vip_idx, int owner_idx,
                        uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    if (!has_quorum(st))
        return -1;
    return lease_start_operation(st, epoll_fd, LCS_LEASE_OP_ACQUIRE, vip_idx,
                                 owner_idx, epoch, lease_id);
}

int lease_start_renew(daemon_state_t *st, int vip_idx, int epoll_fd)
{
    resource_runtime_t *res = &st->resources[vip_idx];
    return lease_start_operation(st, epoll_fd, LCS_LEASE_OP_RENEW, vip_idx,
                                 st->self_index, res->epoch, res->lease_id);
}

static void lease_process_result(daemon_state_t *st, lease_runtime_t *op)
{
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index || !op->rpc_done[i] || op->acked[i])
            continue;
        int32_t status = -1;
        char msg[128];
        if (op->rpc_status[i] == 0 &&
            lcs_decode_simple_resp(op->rpc_resp[i], op->rpc_resp_len[i],
                                   &status, msg, sizeof(msg)) == 0 &&
            status == 0)
        {
            op->acked[i] = true;
            if (op->type != LCS_LEASE_OP_RELEASE)
                op->votes++;
        }
    }
}

static void lease_finish_acquire(daemon_state_t *st, int epoll_fd,
                                 lease_runtime_t *op)
{
    resource_runtime_t *res = &st->resources[op->vip_idx];
    if ((uint32_t)op->votes >= st->quorum_needed)
    {
        uint64_t now = lcs_now_ms();
        res->epoch = op->epoch;
        res->lease_id = op->lease_id;
        res->owner_node = op->owner_idx;
        res->owner_instance_id = st->instance_id;
        res->state = LCS_RES_ACTIVE;
        res->lease_deadline_ms = now + st->cfg.lease_ms;
        res->renew_after_ms = now + st->cfg.renew_ms;
        res->conflict_reason[0] = '\0';
        lcs_log_debug("lease acquired for VIP %s epoch=%llu votes=%d need=%u",
                      st->cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      st->quorum_needed);
        if (resources_activate_acquired_local(st, op->vip_idx, op->epoch,
                                              op->lease_id, epoll_fd) != 0)
            res->next_activation_attempt_ms = now + st->cfg.lease_ms;
        if (op->type == LCS_LEASE_OP_RELEASE)
            return;
    } else
    {
        lcs_log_debug("lease acquire failed for VIP %s epoch=%llu votes=%d need=%u",
                      st->cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes,
                      st->quorum_needed);
        lease_op_send_release_to_acked(st, epoll_fd, op);
        res->next_activation_attempt_ms = lcs_now_ms() + st->cfg.renew_ms;
        if (op->pending_rpcs > 0)
            return;
    }
    lease_op_clear(op);
}

static void lease_finish_renew(daemon_state_t *st, int epoll_fd,
                               lease_runtime_t *op)
{
    resource_runtime_t *res = &st->resources[op->vip_idx];
    uint64_t now = lcs_now_ms();
    if ((uint32_t)op->votes >= st->quorum_needed)
    {
        res->lease_deadline_ms = now + st->cfg.lease_ms;
        res->renew_after_ms = now + st->cfg.renew_ms;
        lcs_log_debug("renewed VIP %s lease epoch=%llu votes=%d",
                      st->cfg.vips[op->vip_idx].name,
                      (unsigned long long)op->epoch, op->votes);
    } else if (now + st->cfg.renew_ms >= res->lease_deadline_ms)
    {
        lcs_log_warn("dropping VIP %s because lease renewal failed",
                     st->cfg.vips[op->vip_idx].name);
        resources_drop_local(st, op->vip_idx, epoll_fd);
    } else
    {
        res->renew_after_ms = now + st->cfg.renew_ms;
    }
    lease_op_clear(op);
}

void lease_process_operations(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        lease_runtime_t *op = &st->lease_ops[i];
        if (!op->active)
            continue;
        lease_process_result(st, op);
        if (op->pending_rpcs > 0 && op->deadline_ms && now < op->deadline_ms)
            continue;
        if (op->type == LCS_LEASE_OP_ACQUIRE)
            lease_finish_acquire(st, epoll_fd, op);
        else if (op->type == LCS_LEASE_OP_RENEW)
            lease_finish_renew(st, epoll_fd, op);
        else
            lease_op_clear(op);
    }
}

void lease_release_majority(daemon_state_t *st, int vip_idx, int owner_idx,
                             uint64_t epoch, uint64_t lease_id, int epoll_fd)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        lease_runtime_t *op = &st->lease_ops[i];
        if (!op->active || op->vip_idx != vip_idx)
            continue;
        op->type = LCS_LEASE_OP_RELEASE;
        op->owner_idx = owner_idx;
        op->epoch = epoch;
        op->lease_id = lease_id;
        op->pending_rpcs = 0;
        op->deadline_ms = lcs_now_ms() + st->cfg.peer_timeout_ms;
        for (size_t n = 0; n < st->cfg.node_count; n++)
        {
            if ((int)n == st->self_index)
                continue;
            op->rpc_done[n] = false;
            op->rpc_status[n] = -1;
            op->rpc_resp_len[n] = 0;
            (void)lease_op_send_to_peer(st, epoll_fd, op, (int)n);
        }
        if (op->pending_rpcs == 0)
            lease_op_clear(op);
        return;
    }
    (void)lease_start_operation(st, epoll_fd, LCS_LEASE_OP_RELEASE, vip_idx,
                                owner_idx, epoch, lease_id);
}

int lease_handle_owner_release_request(daemon_state_t *st, const void *payload, size_t len,
                                 int source_node_idx)
{
    uint16_t resource_id, owner_node;
    uint64_t epoch, lease_id, sender_instance_id;
    uint32_t lease_ms;
    if (lease_decode_msg(st, payload, len, &resource_id, &owner_node, &epoch,
                         &lease_id, &lease_ms, &sender_instance_id) != 0)
        return -1;

    (void)lease_ms;
    if (source_node_idx >= 0 && ((size_t)source_node_idx >= st->cfg.node_count ||
                                  sender_instance_id != st->peers[source_node_idx].instance_id))
        return -1;

    if (owner_node != (uint16_t)st->self_index)
        return -1;

    resource_runtime_t *res = &st->resources[resource_id];
    if (res->owner_node == -1 && res->state == LCS_RES_STOPPED && res->epoch >= epoch)
        return 0;

    if (res->owner_node != st->self_index ||
        res->owner_instance_id != st->instance_id ||
        res->state != LCS_RES_ACTIVE ||
        res->epoch != epoch ||
        res->lease_id != lease_id)
        return -1;

    if (lcs_vip_del(&st->cfg.vips[resource_id]) != 0)
        return -1;

    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
    res->conflict_reason[0] = '\0';
    res->next_activation_attempt_ms = lcs_now_ms() + st->cfg.lease_ms;
    lcs_log_info("released VIP %s for controlled handoff at epoch=%llu",
                 st->cfg.vips[resource_id].name, (unsigned long long)epoch);
    return 0;
}

void lease_expire_remote(daemon_state_t *st)
{
    uint64_t now = lcs_now_ms();
    uint64_t grace_ms = st->cfg.renew_ms ? st->cfg.renew_ms : 1000u;
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        resource_runtime_t *res = &st->resources[i];
        if ((res->state != LCS_RES_ACTIVE &&
             res->state != LCS_RES_STARTING &&
             res->state != LCS_RES_STOPPING) ||
            res->owner_node < 0 ||
            (res->owner_node == st->self_index &&
             res->owner_instance_id == st->instance_id))
            continue;
        if (res->lease_deadline_ms && now < res->lease_deadline_ms + grace_ms)
            continue;
        lcs_log_warn("clearing expired remote lease for VIP %s owner=%s epoch=%llu expired_ms=%llu",
                     st->cfg.vips[i].name,
                     node_name_or_none(st, res->owner_node),
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
