// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "local_client.h"

#include "cluster.h"
#include "lease.h"
#include "log.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"

#include <stdio.h>
#include <string.h>

void send_error(int fd, uint32_t seq, const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, -1, msg) == 0)
        lcs_write_frame(fd, LCS_MSG_ERROR, seq, payload, (uint32_t)len);
}

static void handle_status(int fd, uint32_t seq, const daemon_state_t *st)
{
    unsigned char payload[LCS_MAX_FRAME];
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, sizeof(payload));
    if (lcs_encode_status_header(&w, (uint16_t)st->cfg.node_count,
                                 (uint16_t)st->cfg.vip_count,
                                 (uint16_t)st->self_index,
                                 (uint16_t)st->quorum_needed,
                                 (uint16_t)st->votes_seen,
                                 has_quorum(st) ? 1 : 0) != 0)
    {
        send_error(fd, seq, "failed to encode status header");
        return;
    }
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if (lcs_encode_status_node(&w, (uint16_t)i,
                                   (uint16_t)st->cfg.nodes[i].role,
                                   node_is_online(st, i) ? 1 : 0,
                                   i == (size_t)st->self_index ? 1 : 0,
                                   st->cfg.nodes[i].name) != 0)
        {
            send_error(fd, seq, "failed to encode status node");
            return;
        }
    }
    for (size_t i = 0; i < st->cfg.vip_count; i++)
    {
        uint16_t owner = st->resources[i].owner_node < 0 ?
                         UINT16_MAX : (uint16_t)st->resources[i].owner_node;
        if (lcs_encode_status_vip(&w, (uint16_t)i, owner,
                                  st->resources[i].epoch,
                                  st->resources[i].lease_id,
                                  (uint8_t)st->resources[i].state,
                                  st->cfg.vips[i].name,
                                  st->cfg.vips[i].address,
                                  st->cfg.vips[i].interface,
                                  st->resources[i].conflict_reason) != 0)
        {
            send_error(fd, seq, "failed to encode status vip");
            return;
        }
    }
    lcs_write_frame(fd, LCS_MSG_STATUS_RESP, seq, payload, (uint32_t)w.len);
}

int compute_move_response(daemon_state_t *st, const void *payload, uint32_t len,
                                 int epoll_fd, int32_t *status, char *message,
                                 size_t message_len)
{
    char vip_name[LCS_NAME_MAX + 1];
    char target_name[LCS_NAME_MAX + 1];
    if (lcs_decode_move_req(payload, len, vip_name, sizeof(vip_name),
                            target_name, sizeof(target_name)) != 0)
    {
        *status = -1;
        snprintf(message, message_len, "invalid move request length");
        return -1;
    }
    *status = -1;
    message[0] = '\0';
    int vip_idx = lcs_config_vip_index(&st->cfg, vip_name);
    int target_idx = lcs_config_node_index(&st->cfg, target_name);
    if (vip_idx < 0 || target_idx < 0)
    {
        *status = -1;
        snprintf(message, message_len, "unknown VIP or target node");
    } else if (!has_quorum(st))
    {
        *status = -1;
        snprintf(message, message_len, "majority quorum is not available");
    } else if (st->cfg.nodes[target_idx].role != LCS_NODE_FULL)
    {
        *status = -1;
        snprintf(message, message_len, "target node is not a full-member");
    } else if (target_idx != st->self_index)
    {
        unsigned char peer_resp[LCS_MAX_FRAME];
        uint32_t peer_resp_len = 0;
        uint32_t move_timeout_ms = (st->cfg.peer_timeout_ms * 2u) + 1000u;
        if (peer_rpc(st, epoll_fd, target_idx, LCS_MSG_MOVE_REQ, payload, len,
                     LCS_MSG_MOVE_RESP, peer_resp, sizeof(peer_resp), &peer_resp_len,
                     move_timeout_ms) == 0 &&
            lcs_decode_simple_resp(peer_resp, peer_resp_len, status, message, message_len) == 0)
        {
            /* status and message already decoded */
        } else
        {
            if (st->resources[vip_idx].owner_node == target_idx &&
                st->resources[vip_idx].state == LCS_RES_ACTIVE)
            {
                *status = 0;
                snprintf(message, message_len, "move completed");
            } else
            {
                *status = -1;
                snprintf(message, message_len, "target node move request failed");
            }
        }
    } else
    {
        resource_runtime_t *res = &st->resources[vip_idx];
        if (res->owner_node == st->self_index &&
            res->owner_instance_id == st->instance_id &&
            res->state == LCS_RES_ACTIVE)
        {
            *status = 0;
            snprintf(message, message_len, "already active on target");
        } else if (prepare_controlled_handoff(st, vip_idx, epoll_fd) != 0)
        {
            *status = -1;
            snprintf(message, message_len,
                     "old owner did not release and lease expiry is unknown");
        } else if (activate_local_resource(st, vip_idx, res->epoch + 1, epoll_fd) == 0)
        {
            *status = 0;
            snprintf(message, message_len, "%s",
                     res->state == LCS_RES_STARTING ?
                     "move accepted, activation hook running" : "move completed");
        } else
        {
            *status = -1;
            snprintf(message, message_len, "failed to activate VIP on target");
        }
    }
    return *status == 0 ? 0 : -1;
}

static void handle_move(int fd, uint32_t seq, daemon_state_t *st, const void *payload,
                        uint32_t len, int epoll_fd)
{
    int32_t status = -1;
    char message[128] = "";
    compute_move_response(st, payload, len, epoll_fd, &status, message, sizeof(message));
    unsigned char resp_payload[256];
    size_t resp_len = 0;
    if (lcs_encode_simple_resp(resp_payload, sizeof(resp_payload), &resp_len,
                               status, message) == 0)
        lcs_write_frame(fd, LCS_MSG_MOVE_RESP, seq, resp_payload, (uint32_t)resp_len);
}

static void handle_clear_conflict(int fd, uint32_t seq, daemon_state_t *st,
                                  const void *payload, uint32_t len, int epoll_fd)
{
    char vip_name[LCS_NAME_MAX + 1];
    int32_t status = -1;
    char message[128] = "";
    if (lcs_decode_clear_conflict_req(payload, len, vip_name, sizeof(vip_name)) != 0)
    {
        snprintf(message, sizeof(message), "invalid clear-conflict request");
    } else if (!has_quorum(st))
    {
        snprintf(message, sizeof(message), "majority quorum is not available");
    } else
    {
        int vip_idx = lcs_config_vip_index(&st->cfg, vip_name);
        if (vip_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown VIP");
        } else
        {
            resource_runtime_t *res = &st->resources[vip_idx];
            if (res->state != LCS_RES_CONFLICT)
            {
                status = 0;
                snprintf(message, sizeof(message), "VIP is not in conflict state");
            } else
            {
                res->epoch++;
                res->owner_node = -1;
                res->owner_instance_id = 0;
                res->state = LCS_RES_STOPPED;
                res->lease_id = 0;
                res->lease_deadline_ms = 0;
                res->renew_after_ms = 0;
                res->next_activation_attempt_ms = 0;
                res->conflict_reason[0] = '\0';
                broadcast_state_sync(st, epoll_fd);
                status = 0;
                snprintf(message, sizeof(message), "conflict cleared");
                lcs_log_info("admin cleared conflict for VIP %s epoch=%llu",
                             st->cfg.vips[vip_idx].name, (unsigned long long)res->epoch);
            }
        }
    }
    unsigned char resp_payload[256];
    size_t resp_len = 0;
    if (lcs_encode_simple_resp(resp_payload, sizeof(resp_payload), &resp_len,
                               status, message) == 0)
    {
        lcs_write_frame(fd, LCS_MSG_CLEAR_CONFLICT_RESP, seq, resp_payload,
                        (uint32_t)resp_len);
    }
}

void handle_client(int fd, daemon_state_t *st, int epoll_fd)
{
    unsigned char payload[LCS_MAX_FRAME];
    lcs_frame_header_t hdr;
    int rc = lcs_read_frame(fd, &hdr, payload, sizeof(payload));
    if (rc <= 0)
    {
        lcs_log_debug("local client frame read failed: %s", lcs_protocol_error());
        return;
    }
    switch (hdr.type)
    {
        case LCS_MSG_STATUS_REQ:
            handle_status(fd, hdr.seq, st);
            break;
        case LCS_MSG_MOVE_REQ:
            handle_move(fd, hdr.seq, st, payload, hdr.length, epoll_fd);
            break;
        case LCS_MSG_CLEAR_CONFLICT_REQ:
            handle_clear_conflict(fd, hdr.seq, st, payload, hdr.length, epoll_fd);
            break;
        default:
            send_error(fd, hdr.seq, "unsupported local CLI message");
            break;
    }
}
