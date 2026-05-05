// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "move.h"

#include "cluster.h"
#include "lease.h"
#include "local_client.h"
#include "log.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "util.h"

#include <stdio.h>
#include <string.h>

static void move_clear(move_runtime_t *move)
{
    memset(move, 0, sizeof(*move));
    move->local_client_slot = -1;
    move->source_node_idx = -1;
    move->vip_idx = -1;
    move->target_idx = -1;
    move->old_owner_idx = -1;
}

static move_runtime_t *move_alloc(daemon_state_t *st)
{
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++)
    {
        if (!st->moves[i].active)
        {
            move_clear(&st->moves[i]);
            st->moves[i].active = true;
            st->moves[i].id = ++st->next_move_id;
            if (!st->moves[i].id)
                st->moves[i].id = ++st->next_move_id;
            return &st->moves[i];
        }
    }
    return NULL;
}

static void move_complete(daemon_state_t *st, int epoll_fd, move_runtime_t *move)
{
    if (move->origin == LCS_MOVE_ORIGIN_LOCAL_CLIENT)
    {
        client_complete_move(st, epoll_fd, move->local_client_slot,
                             move->local_client_id, move->client_seq,
                             move->final_status, move->final_message);
    } else if (move->origin == LCS_MOVE_ORIGIN_PEER)
    {
        peer_send_simple_response(st, epoll_fd, move->source_node_idx,
                                  move->peer_seq, LCS_MSG_MOVE_RESP,
                                  move->final_status, move->final_message);
    }
    move_clear(move);
}

static void move_peer_callback(void *ctx, int status,
                               const unsigned char *payload, uint32_t len)
{
    move_rpc_context_t *rpc_ctx = ctx;
    move_runtime_t *move = rpc_ctx->move;
    if (!move || !move->active || move->id != rpc_ctx->move_id)
        return;
    move->peer_done = true;
    move->peer_status = status;
    move->peer_resp_len = 0;
    if (status == 0 && payload && len <= sizeof(move->peer_resp))
    {
        memcpy(move->peer_resp, payload, len);
        move->peer_resp_len = len;
    }
}

static void move_rpc_callback(void *ctx, int status,
                              const unsigned char *payload, uint32_t len)
{
    move_rpc_context_t *rpc_ctx = ctx;
    move_runtime_t *move = rpc_ctx->move;
    int node_idx = rpc_ctx->node_idx;
    if (!move || !move->active || move->id != rpc_ctx->move_id ||
        node_idx < 0 || node_idx >= LCS_MAX_NODES)
        return;
    move->rpc_done[node_idx] = true;
    move->rpc_status[node_idx] = status;
    move->rpc_resp_len[node_idx] = 0;
    if (status == 0 && payload && len <= sizeof(move->rpc_resp[node_idx]))
    {
        memcpy(move->rpc_resp[node_idx], payload, len);
        move->rpc_resp_len[node_idx] = len;
    }
    if (move->pending_rpcs > 0)
        move->pending_rpcs--;
}

static void move_fail_immediate_local(daemon_state_t *st, int epoll_fd,
                                      int local_slot, uint64_t local_client_id,
                                      uint32_t client_seq, const char *message)
{
    client_complete_move(st, epoll_fd, local_slot, local_client_id, client_seq,
                         -1, message);
}

static int move_validate_request(daemon_state_t *st, const void *payload,
                                 uint32_t len, int *vip_idx, int *target_idx,
                                 char *message, size_t message_len)
{
    char vip_name[LCS_NAME_MAX + 1];
    char target_name[LCS_NAME_MAX + 1];
    if (lcs_decode_move_req(payload, len, vip_name, sizeof(vip_name),
                            target_name, sizeof(target_name)) != 0)
    {
        snprintf(message, message_len, "invalid move request length");
        return -1;
    }
    *vip_idx = lcs_config_vip_index(&st->cfg, vip_name);
    *target_idx = lcs_config_node_index(&st->cfg, target_name);
    if (*vip_idx < 0 || *target_idx < 0)
    {
        snprintf(message, message_len, "unknown VIP or target node");
        return -1;
    }
    if (!has_quorum(st))
    {
        snprintf(message, message_len, "majority quorum is not available");
        return -1;
    }
    if (st->cfg.nodes[*target_idx].role != LCS_NODE_FULL)
    {
        snprintf(message, message_len, "target node is not a full-member");
        return -1;
    }
    return 0;
}

static void move_set_failed(move_runtime_t *move, const char *message)
{
    move->final_status = -1;
    snprintf(move->final_message, sizeof(move->final_message), "%s", message);
}

static void move_start_owner_release(daemon_state_t *st, int epoll_fd,
                                     move_runtime_t *move)
{
    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    move->rpc_ctx[move->old_owner_idx].move = move;
    move->rpc_ctx[move->old_owner_idx].move_id = move->id;
    move->rpc_ctx[move->old_owner_idx].node_idx = move->old_owner_idx;
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)move->vip_idx,
                         (uint16_t)move->old_owner_idx, move->old_epoch,
                         move->old_lease_id, 0, st->instance_id) != 0 ||
        peer_rpc_async(st, epoll_fd, move->old_owner_idx,
                       LCS_MSG_OWNER_RELEASE_REQ, req, (uint32_t)req_len,
                       LCS_MSG_OWNER_RELEASE_RESP,
                       move->rpc_resp[move->old_owner_idx],
                       sizeof(move->rpc_resp[move->old_owner_idx]),
                       &move->rpc_resp_len[move->old_owner_idx],
                       st->cfg.peer_timeout_ms, move_rpc_callback,
                       &move->rpc_ctx[move->old_owner_idx]) != 0)
    {
        move->phase = LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY;
        move->wait_until_ms = st->resources[move->vip_idx].lease_deadline_ms;
        lcs_log_info("old owner %s did not accept release request for VIP %s; waiting for lease expiry",
                     node_name_or_none(st, move->old_owner_idx),
                     st->cfg.vips[move->vip_idx].name);
        return;
    }
    move->pending_rpcs = 1;
    move->phase = LCS_MOVE_PHASE_WAIT_OWNER_RELEASE;
}

static void move_apply_local_lease(daemon_state_t *st, move_runtime_t *move)
{
    resource_runtime_t *res = &st->resources[move->vip_idx];
    uint64_t now = lcs_now_ms();
    res->epoch = move->epoch;
    res->lease_id = move->lease_id;
    res->owner_node = st->self_index;
    res->owner_instance_id = st->instance_id;
    res->state = LCS_RES_ACTIVE;
    res->lease_deadline_ms = now + st->cfg.lease_ms;
    res->renew_after_ms = now + st->cfg.renew_ms;
    res->conflict_reason[0] = '\0';
}

static void move_start_failed_release(daemon_state_t *st, int epoll_fd,
                                      move_runtime_t *move)
{
    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    move->pending_rpcs = 0;
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)move->vip_idx,
                         (uint16_t)st->self_index, move->epoch, move->lease_id,
                         st->cfg.lease_ms, st->instance_id) != 0)
    {
        move_complete(st, epoll_fd, move);
        return;
    }
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index || !move->lease_acked[i])
            continue;
        move->rpc_done[i] = false;
        move->rpc_status[i] = -1;
        move->rpc_resp_len[i] = 0;
        move->rpc_ctx[i].move = move;
        move->rpc_ctx[i].move_id = move->id;
        move->rpc_ctx[i].node_idx = (int)i;
        if (peer_rpc_async(st, epoll_fd, (int)i, LCS_MSG_LEASE_RELEASE,
                           req, (uint32_t)req_len, LCS_MSG_LEASE_ACK,
                           move->rpc_resp[i], sizeof(move->rpc_resp[i]),
                           &move->rpc_resp_len[i], st->cfg.peer_timeout_ms,
                           move_rpc_callback, &move->rpc_ctx[i]) == 0)
            move->pending_rpcs++;
    }
    if (move->pending_rpcs == 0)
        move_complete(st, epoll_fd, move);
    else
        move->phase = LCS_MOVE_PHASE_RELEASE_FAILED_LEASE;
}

static void move_start_lease_acquire(daemon_state_t *st, int epoll_fd,
                                     move_runtime_t *move)
{
    unsigned char req[LCS_MAX_FRAME];
    size_t req_len = 0;
    resource_runtime_t *res = &st->resources[move->vip_idx];
    if (res->state == LCS_RES_CONFLICT)
    {
        move_set_failed(move, "VIP is in conflict state");
        move_complete(st, epoll_fd, move);
        return;
    }
    move->epoch = res->epoch + 1;
    move->lease_id = lcs_random_u64();
    move->votes = 1;
    move->pending_rpcs = 0;
    memset(move->lease_acked, 0, sizeof(move->lease_acked));
    if (lease_encode_msg(req, sizeof(req), &req_len, (uint16_t)move->vip_idx,
                         (uint16_t)st->self_index, move->epoch, move->lease_id,
                         st->cfg.lease_ms, st->instance_id) != 0)
    {
        move_set_failed(move, "failed to encode lease request");
        move_complete(st, epoll_fd, move);
        return;
    }
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        move->rpc_done[i] = false;
        move->rpc_status[i] = -1;
        move->rpc_resp_len[i] = 0;
        move->rpc_ctx[i].move = move;
        move->rpc_ctx[i].move_id = move->id;
        move->rpc_ctx[i].node_idx = (int)i;
        if (peer_rpc_async(st, epoll_fd, (int)i, LCS_MSG_LEASE_REQ,
                           req, (uint32_t)req_len, LCS_MSG_LEASE_ACK,
                           move->rpc_resp[i], sizeof(move->rpc_resp[i]),
                           &move->rpc_resp_len[i], st->cfg.peer_timeout_ms,
                           move_rpc_callback, &move->rpc_ctx[i]) == 0)
            move->pending_rpcs++;
    }
    move->phase = LCS_MOVE_PHASE_WAIT_LEASE;
}

static void move_finish_activation(daemon_state_t *st, int epoll_fd,
                                   move_runtime_t *move)
{
    move_apply_local_lease(st, move);
    if (resources_activate_acquired_local(st, move->vip_idx, move->epoch,
                                          move->lease_id, epoll_fd) == 0)
    {
        move->final_status = 0;
        snprintf(move->final_message, sizeof(move->final_message), "%s",
                 st->resources[move->vip_idx].state == LCS_RES_STARTING ?
                 "move accepted, activation hook running" : "move completed");
    } else
    {
        move_set_failed(move, "failed to activate VIP on target");
    }
    move_complete(st, epoll_fd, move);
}

static void move_start_target(daemon_state_t *st, int epoll_fd,
                              move_runtime_t *move)
{
    resource_runtime_t *res = &st->resources[move->vip_idx];
    if (res->owner_node == st->self_index &&
        res->owner_instance_id == st->instance_id &&
        res->state == LCS_RES_ACTIVE)
    {
        move->final_status = 0;
        snprintf(move->final_message, sizeof(move->final_message),
                 "already active on target");
        move_complete(st, epoll_fd, move);
        return;
    }
    move->old_owner_idx = res->owner_node;
    move->old_epoch = res->epoch;
    move->old_lease_id = res->lease_id;
    if (move->old_owner_idx >= 0 && move->old_owner_idx != st->self_index)
    {
        if (node_is_online(st, (size_t)move->old_owner_idx))
        {
            move_start_owner_release(st, epoll_fd, move);
            return;
        }
        move->wait_until_ms = res->lease_deadline_ms;
        if (!move->wait_until_ms)
        {
            move_set_failed(move, "old owner did not release and lease expiry is unknown");
            move_complete(st, epoll_fd, move);
            return;
        }
        lcs_log_info("old owner %s is offline for VIP %s; waiting for lease expiry",
                     node_name_or_none(st, move->old_owner_idx),
                     st->cfg.vips[move->vip_idx].name);
        move->phase = LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY;
        return;
    }
    move_start_lease_acquire(st, epoll_fd, move);
}

static int move_start_remote_target(daemon_state_t *st, int epoll_fd,
                                    int local_slot, uint32_t client_seq,
                                    const void *payload, uint32_t len,
                                    int vip_idx, int target_idx)
{
    uint64_t local_client_id = st->local_clients[local_slot].id;
    move_runtime_t *move = move_alloc(st);
    if (!move)
    {
        move_fail_immediate_local(st, epoll_fd, local_slot, local_client_id,
                                  client_seq, "move table is full");
        return -1;
    }
    move->origin = LCS_MOVE_ORIGIN_LOCAL_CLIENT;
    move->phase = LCS_MOVE_PHASE_WAIT_TARGET;
    move->local_client_slot = local_slot;
    move->local_client_id = local_client_id;
    move->client_seq = client_seq;
    move->vip_idx = vip_idx;
    move->target_idx = target_idx;
    move->deadline_ms = lcs_now_ms() + (st->cfg.peer_timeout_ms * 2u) + 1000u;
    st->local_clients[local_slot].deadline_ms = move->deadline_ms + 1000u;
    move_set_failed(move, "target node move request failed");
    move->rpc_ctx[0].move = move;
    move->rpc_ctx[0].move_id = move->id;
    move->rpc_ctx[0].node_idx = target_idx;

    uint32_t timeout_ms = (st->cfg.peer_timeout_ms * 2u) + 1000u;
    if (peer_rpc_async(st, epoll_fd, target_idx, LCS_MSG_MOVE_REQ, payload, len,
                       LCS_MSG_MOVE_RESP, move->peer_resp,
                       sizeof(move->peer_resp), &move->peer_resp_len,
                       timeout_ms, move_peer_callback, &move->rpc_ctx[0]) != 0)
    {
        if (!move->active)
            return -1;
        if (st->resources[vip_idx].owner_node == target_idx &&
            st->resources[vip_idx].state == LCS_RES_ACTIVE)
        {
            move->final_status = 0;
            snprintf(move->final_message, sizeof(move->final_message),
                     "move completed");
        }
        move_complete(st, epoll_fd, move);
        return -1;
    }
    lcs_log_debug3("move operation forwarded VIP=%s target=%s local_slot=%d",
                   st->cfg.vips[vip_idx].name, st->cfg.nodes[target_idx].name,
                   local_slot);
    return 0;
}

int move_start_local_client(daemon_state_t *st, int epoll_fd, int local_slot,
                            uint32_t client_seq, const void *payload,
                            uint32_t len)
{
    int vip_idx = -1;
    int target_idx = -1;
    char message[128] = "";
    uint64_t local_client_id = st->local_clients[local_slot].id;
    if (move_validate_request(st, payload, len, &vip_idx, &target_idx,
                              message, sizeof(message)) != 0)
    {
        move_fail_immediate_local(st, epoll_fd, local_slot, local_client_id,
                                  client_seq, message);
        return -1;
    }
    if (target_idx != st->self_index)
        return move_start_remote_target(st, epoll_fd, local_slot, client_seq,
                                        payload, len, vip_idx, target_idx);

    move_runtime_t *move = move_alloc(st);
    if (!move)
    {
        move_fail_immediate_local(st, epoll_fd, local_slot, local_client_id,
                                  client_seq, "move table is full");
        return -1;
    }
    move->origin = LCS_MOVE_ORIGIN_LOCAL_CLIENT;
    move->phase = LCS_MOVE_PHASE_PREPARE_TARGET;
    move->local_client_slot = local_slot;
    move->local_client_id = local_client_id;
    move->client_seq = client_seq;
    move->vip_idx = vip_idx;
    move->target_idx = target_idx;
    move->deadline_ms = lcs_now_ms() + (st->cfg.peer_timeout_ms * 3u) + st->cfg.lease_ms + 1000u;
    st->local_clients[local_slot].deadline_ms = move->deadline_ms + 1000u;
    move_start_target(st, epoll_fd, move);
    return 0;
}

int move_start_peer_request(daemon_state_t *st, int epoll_fd, int source_node_idx,
                            uint32_t peer_seq, const void *payload,
                            uint32_t len)
{
    int vip_idx = -1;
    int target_idx = -1;
    char message[128] = "";
    if (move_validate_request(st, payload, len, &vip_idx, &target_idx,
                              message, sizeof(message)) != 0)
    {
        peer_send_simple_response(st, epoll_fd, source_node_idx, peer_seq,
                                  LCS_MSG_MOVE_RESP, -1, message);
        return -1;
    }
    if (target_idx != st->self_index)
    {
        peer_send_simple_response(st, epoll_fd, source_node_idx, peer_seq,
                                  LCS_MSG_MOVE_RESP, -1,
                                  "move request reached non-target node");
        return -1;
    }
    move_runtime_t *move = move_alloc(st);
    if (!move)
    {
        peer_send_simple_response(st, epoll_fd, source_node_idx, peer_seq,
                                  LCS_MSG_MOVE_RESP, -1, "move table is full");
        return -1;
    }
    move->origin = LCS_MOVE_ORIGIN_PEER;
    move->phase = LCS_MOVE_PHASE_PREPARE_TARGET;
    move->source_node_idx = source_node_idx;
    move->peer_seq = peer_seq;
    move->vip_idx = vip_idx;
    move->target_idx = target_idx;
    move->deadline_ms = lcs_now_ms() + (st->cfg.peer_timeout_ms * 3u) + st->cfg.lease_ms + 1000u;
    move_start_target(st, epoll_fd, move);
    return 0;
}

static void move_process_target(move_runtime_t *move, daemon_state_t *st,
                                int epoll_fd, uint64_t now)
{
    if (move->phase == LCS_MOVE_PHASE_WAIT_OWNER_RELEASE)
    {
        if (!move->rpc_done[move->old_owner_idx])
            return;
        int32_t status = -1;
        char msg[128];
        if (move->rpc_status[move->old_owner_idx] == 0 &&
            lcs_decode_simple_resp(move->rpc_resp[move->old_owner_idx],
                                   move->rpc_resp_len[move->old_owner_idx],
                                   &status, msg, sizeof(msg)) == 0 &&
            status == 0)
        {
            lcs_log_info("old owner %s confirmed release of VIP %s",
                         st->cfg.nodes[move->old_owner_idx].name,
                         st->cfg.vips[move->vip_idx].name);
            move_start_lease_acquire(st, epoll_fd, move);
            return;
        }
        move->wait_until_ms = st->resources[move->vip_idx].lease_deadline_ms;
        if (!move->wait_until_ms)
        {
            move_set_failed(move, "old owner did not release and lease expiry is unknown");
            move_complete(st, epoll_fd, move);
            return;
        }
        lcs_log_info("old owner %s did not confirm release of VIP %s; waiting for lease expiry",
                     node_name_or_none(st, move->old_owner_idx),
                     st->cfg.vips[move->vip_idx].name);
        move->phase = LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY;
    }
    if (move->phase == LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY)
    {
        if (move->wait_until_ms && now < move->wait_until_ms)
            return;
        move_start_lease_acquire(st, epoll_fd, move);
    }
}

static void move_process_lease(move_runtime_t *move, daemon_state_t *st,
                               int epoll_fd)
{
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index || !move->rpc_done[i] ||
            move->lease_acked[i])
            continue;
        int32_t status = -1;
        char msg[128];
        if (move->rpc_status[i] == 0 &&
            lcs_decode_simple_resp(move->rpc_resp[i], move->rpc_resp_len[i],
                                   &status, msg, sizeof(msg)) == 0 &&
            status == 0)
        {
            move->lease_acked[i] = true;
            move->votes++;
            lcs_log_debug("lease request for VIP %s epoch=%llu acked by %s",
                          st->cfg.vips[move->vip_idx].name,
                          (unsigned long long)move->epoch,
                          st->cfg.nodes[i].name);
        }
    }
    if ((uint32_t)move->votes >= st->quorum_needed)
    {
        lcs_log_debug("lease acquired for VIP %s epoch=%llu votes=%d need=%u",
                      st->cfg.vips[move->vip_idx].name,
                      (unsigned long long)move->epoch, move->votes,
                      st->quorum_needed);
        move_finish_activation(st, epoll_fd, move);
        return;
    }
    if (move->pending_rpcs == 0)
    {
        lcs_log_debug("lease acquire failed for VIP %s epoch=%llu votes=%d need=%u",
                      st->cfg.vips[move->vip_idx].name,
                      (unsigned long long)move->epoch, move->votes,
                      st->quorum_needed);
        move_set_failed(move, "could not acquire majority lease");
        move_start_failed_release(st, epoll_fd, move);
    }
}

void move_process(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++)
    {
        move_runtime_t *move = &st->moves[i];
        if (!move->active)
            continue;
        if (move->deadline_ms && now >= move->deadline_ms)
        {
            move_set_failed(move, "move timed out");
            if (move->phase == LCS_MOVE_PHASE_WAIT_LEASE)
                move_start_failed_release(st, epoll_fd, move);
            else
                move_complete(st, epoll_fd, move);
            continue;
        }
        if (move->phase == LCS_MOVE_PHASE_WAIT_TARGET)
        {
            if (!move->peer_done)
                continue;
            if (move->peer_status == 0 &&
                lcs_decode_simple_resp(move->peer_resp, move->peer_resp_len,
                                       &move->final_status,
                                       move->final_message,
                                       sizeof(move->final_message)) == 0)
            {
                /* decoded final status */
            } else if (move->vip_idx >= 0 &&
                       st->resources[move->vip_idx].owner_node == move->target_idx &&
                       st->resources[move->vip_idx].state == LCS_RES_ACTIVE)
            {
                move->final_status = 0;
                snprintf(move->final_message, sizeof(move->final_message),
                         "move completed");
            } else
                move_set_failed(move, "target node move request failed");
            move_complete(st, epoll_fd, move);
        } else if (move->phase == LCS_MOVE_PHASE_WAIT_OWNER_RELEASE ||
                   move->phase == LCS_MOVE_PHASE_WAIT_OLD_LEASE_EXPIRY)
            move_process_target(move, st, epoll_fd, now);
        else if (move->phase == LCS_MOVE_PHASE_WAIT_LEASE)
            move_process_lease(move, st, epoll_fd);
        else if (move->phase == LCS_MOVE_PHASE_RELEASE_FAILED_LEASE &&
                 move->pending_rpcs == 0)
            move_complete(st, epoll_fd, move);
    }
}

void move_cancel_local_client(daemon_state_t *st, int local_slot,
                              uint64_t local_client_id)
{
    for (size_t i = 0; i < LCS_MOVE_OP_MAX; i++)
    {
        move_runtime_t *move = &st->moves[i];
        if (move->active &&
            move->origin == LCS_MOVE_ORIGIN_LOCAL_CLIENT &&
            move->local_client_slot == local_slot &&
            move->local_client_id == local_client_id)
        {
            move->origin = LCS_MOVE_ORIGIN_NONE;
            move->local_client_slot = -1;
            move->local_client_id = 0;
            move->client_seq = 0;
        }
    }
}
