// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "local_client.h"

#include "cluster.h"
#include "log.h"
#include "move.h"
#include "peer.h"
#include "protocol.h"
#include "resources.h"
#include "epoll_util.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <unistd.h>

static uint32_t client_epoll_id(int slot_idx)
{
    return LCS_EPOLL_LOCAL_CLIENT_BASE + (uint32_t)slot_idx;
}

int client_index_from_epoll_id(uint32_t id)
{
    if (id < LCS_EPOLL_LOCAL_CLIENT_BASE ||
        id >= LCS_EPOLL_LOCAL_CLIENT_BASE + LCS_LOCAL_CLIENT_MAX)
        return -1;
    return (int)(id - LCS_EPOLL_LOCAL_CLIENT_BASE);
}

static int client_update_epoll(int epoll_fd, int slot_idx, const local_client_runtime_t *client)
{
    if (!client->active || client->fd < 0)
        return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (client->out_len > client->out_off)
        ev.events |= EPOLLOUT;
    ev.data.u32 = client_epoll_id(slot_idx);
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
}

static void client_free_buffers(local_client_runtime_t *client)
{
    free(client->inbuf);
    free(client->outbuf);
    client->inbuf = NULL;
    client->outbuf = NULL;
}

static int client_alloc_buffers(local_client_runtime_t *client)
{
    client->inbuf = malloc(LCS_LOCAL_CLIENT_INBUF_SIZE);
    if (!client->inbuf)
        return -1;
    client->outbuf = malloc(LCS_LOCAL_CLIENT_OUTBUF_SIZE);
    if (!client->outbuf)
    {
        client_free_buffers(client);
        return -1;
    }
    return 0;
}

static void client_close_slot(int epoll_fd, int slot_idx, const char *reason)
{
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    if (!client->active)
        return;
    uint64_t client_id = client->id;
    lcs_log_debug3("closing local client slot=%d fd=%d reason=%s in=%zu out=%zu",
                   slot_idx, client->fd, reason ? reason : "-",
                   client->in_len,
                   client->out_len > client->out_off ?
                   client->out_len - client->out_off : 0);
    if (client->fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
    }
    client_free_buffers(client);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    move_cancel_local_client(slot_idx, client_id);
}

static int client_queue_frame(int epoll_fd, int slot_idx, uint16_t type, uint32_t seq, const void *payload, uint32_t length)
{
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    if (!client->active || !client->outbuf || length > LCS_MAX_FRAME)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = client->out_len - client->out_off;
    if (frame_len > LCS_LOCAL_CLIENT_OUTBUF_SIZE - queued)
        return -1;
    if (client->out_off && queued)
        memmove(client->outbuf, client->outbuf + client->out_off, queued);
    client->out_off = 0;
    client->out_len = queued;

    lcs_frame_header_t wire;
    wire.magic = htonl(LCS_PROTO_MAGIC);
    wire.type = htons(type);
    wire.flags = 0;
    wire.length = htonl(length);
    wire.seq = htonl(seq);
    memcpy(client->outbuf + client->out_len, &wire, sizeof(wire));
    client->out_len += sizeof(wire);
    if (length)
    {
        memcpy(client->outbuf + client->out_len, payload, length);
        client->out_len += length;
    }
    return client_update_epoll(epoll_fd, slot_idx, client);
}

static void client_queue_error(int epoll_fd, int slot_idx, uint32_t seq, const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, -1, msg) == 0)
        client_queue_frame(epoll_fd, slot_idx, LCS_MSG_ERROR, seq, payload, (uint32_t)len);
}

static void client_queue_status(int epoll_fd, int slot_idx, uint32_t seq)
{
    unsigned char payload[LCS_MAX_FRAME];
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, sizeof(payload));
    uint64_t now = lcs_now_ms();
    uint64_t membership_seconds = g_state.membership_since_ms && now >= g_state.membership_since_ms ?
                                  (now - g_state.membership_since_ms) / 1000u : 0;
    if (lcs_encode_status_header(&w, (uint16_t)g_state.cfg.node_count,
                                 (uint16_t)g_state.cfg.vip_count,
                                 (uint16_t)g_state.self_index,
                                 (uint16_t)g_state.quorum_needed,
                                 (uint16_t)g_state.votes_seen,
                                 cluster_has_quorum() ? 1 : 0,
                                 membership_seconds) != 0)
    {
        client_queue_error(epoll_fd, slot_idx, seq, "failed to encode status header");
        return;
    }
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (lcs_encode_status_node(&w, (uint16_t)i,
                                   (uint16_t)g_state.cfg.nodes[i].role,
                                   cluster_node_is_online(i) ? 1 : 0,
                                   i == (size_t)g_state.self_index ? 1 : 0,
                                   g_state.cfg.nodes[i].name) != 0)
        {
            client_queue_error(epoll_fd, slot_idx, seq,  "failed to encode status node");
            return;
        }
    }
    for (size_t i = 0; i < g_state.cfg.vip_count; i++)
    {
        uint16_t owner = g_state.resources[i].owner_node < 0 ?
                         UINT16_MAX : (uint16_t)g_state.resources[i].owner_node;
        const char *group = g_state.cfg.vips[i].group_idx >= 0 ?
                            g_state.cfg.groups[g_state.cfg.vips[i].group_idx].name : "";
        const char *home_node = g_state.cfg.vips[i].home_node_idx >= 0 ?
                                g_state.cfg.nodes[g_state.cfg.vips[i].home_node_idx].name : "";
        if (lcs_encode_status_vip(&w, (uint16_t)i, owner,
                                  g_state.resources[i].epoch,
                                  g_state.resources[i].lease_id,
                                  (uint8_t)g_state.resources[i].state,
                                  g_state.cfg.vips[i].name,
                                  g_state.cfg.vips[i].address,
                                  g_state.cfg.vips[i].interface,
                                  group,
                                  g_state.cfg.vips[i].priority,
                                  home_node,
                                  g_state.resources[i].home_blocked ? 1 : 0,
                                  g_state.resources[i].disabled ? 1 : 0,
                                  g_state.resources[i].conflict_reason) != 0)
        {
            client_queue_error(epoll_fd, slot_idx, seq, "failed to encode status vip");
            return;
        }
    }
    client_queue_frame(epoll_fd, slot_idx, LCS_MSG_STATUS_RESP, seq, payload, (uint32_t)w.len);
}

static void client_queue_simple_response(int epoll_fd,
                                         int slot_idx, uint16_t type,
                                         uint32_t seq, int32_t status,
                                         const char *message)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, status, message) == 0)
        client_queue_frame(epoll_fd, slot_idx, type, seq, payload, (uint32_t)len);
}

void client_complete_move(int epoll_fd, int slot_idx,
                          uint64_t client_id, uint32_t seq, int32_t status,
                          const char *message)
{
    if (slot_idx < 0 || slot_idx >= LCS_LOCAL_CLIENT_MAX)
        return;
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    if (!client->active || client->id != client_id)
        return;
    client_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_MOVE_RESP, seq, status, message);
    client->close_after_flush = true;
}

static void client_queue_clear_conflict(int epoll_fd,
                                        int slot_idx, uint32_t seq,
                                        const void *payload, uint32_t len)
{
    char vip_name[LCS_NAME_MAX + 1];
    int32_t status = -1;
    char message[128] = "";
    if (lcs_decode_clear_conflict_req(payload, len, vip_name, sizeof(vip_name)) != 0)
    {
        snprintf(message, sizeof(message), "invalid clear-conflict request");
    } else if (!cluster_has_quorum())
    {
        snprintf(message, sizeof(message), "majority quorum is not available");
    } else
    {
        int vip_idx = lcs_config_vip_index(&g_state.cfg, vip_name);
        if (vip_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown VIP");
        } else
        {
            resource_runtime_t *res = &g_state.resources[vip_idx];
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
                peer_broadcast_state_sync(epoll_fd);
                status = 0;
                snprintf(message, sizeof(message), "conflict cleared");
                lcs_log_info("admin cleared conflict for VIP %s epoch=%llu", g_state.cfg.vips[vip_idx].name, (unsigned long long)res->epoch);
            }
        }
    }
    client_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_CLEAR_CONFLICT_RESP, seq, status, message);
}

static void client_queue_resource_control(int epoll_fd,
                                          int slot_idx, uint16_t resp_type,
                                          uint32_t seq,
                                          const void *payload, uint32_t len,
                                          bool disabled)
{
    char resource_name[LCS_NAME_MAX + 1];
    int32_t status = -1;
    char message[128] = "";
    if (lcs_decode_resource_req(payload, len, resource_name, sizeof(resource_name)) != 0)
    {
        snprintf(message, sizeof(message), "invalid resource request");
    } else if (!cluster_has_quorum())
    {
        snprintf(message, sizeof(message), "majority quorum is not available");
    } else
    {
        int vip_idx = lcs_config_vip_index(&g_state.cfg, resource_name);
        if (vip_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown resource");
        } else if (resources_set_disabled(vip_idx, disabled, epoll_fd,
                                          message, sizeof(message)) == 0)
        {
            status = 0;
        }
    }
    client_queue_simple_response(epoll_fd, slot_idx, resp_type, seq, status, message);
}

static int client_process_frame(int epoll_fd, int slot_idx,
                                const lcs_frame_header_t *hdr,
                                const unsigned char *payload)
{
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    switch (hdr->type)
    {
        case LCS_MSG_STATUS_REQ:
            client_queue_status(epoll_fd, slot_idx, hdr->seq);
            break;
        case LCS_MSG_MOVE_REQ:
            move_start_local_client(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            return 0;
        case LCS_MSG_CLEAR_CONFLICT_REQ:
            client_queue_clear_conflict(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            break;
        case LCS_MSG_RESOURCE_START_REQ:
            client_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_START_RESP,
                                          hdr->seq, payload, hdr->length, false);
            break;
        case LCS_MSG_RESOURCE_STOP_REQ:
            client_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_STOP_RESP,
                                          hdr->seq, payload, hdr->length, true);
            break;
        default:
            client_queue_error(epoll_fd, slot_idx, hdr->seq, "unsupported local CLI message");
            break;
    }
    client->close_after_flush = true;
    return 0;
}

static int client_parse_frames(int epoll_fd, int slot_idx)
{
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    size_t off = 0;
    while (client->in_len - off >= LCS_FRAME_HEADER_SIZE)
    {
        lcs_frame_header_t wire;
        memcpy(&wire, client->inbuf + off, sizeof(wire));
        lcs_frame_header_t hdr;
        hdr.magic = ntohl(wire.magic);
        hdr.type = ntohs(wire.type);
        hdr.flags = ntohs(wire.flags);
        hdr.length = ntohl(wire.length);
        hdr.seq = ntohl(wire.seq);
        if (hdr.magic != LCS_PROTO_MAGIC || hdr.flags != 0 || hdr.length > LCS_MAX_FRAME)
            return -1;

        size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)hdr.length;
        if (client->in_len - off < frame_len)
            break;

        if (client_process_frame(epoll_fd, slot_idx, &hdr, client->inbuf + off + LCS_FRAME_HEADER_SIZE) != 0)
            return -1;
        off += frame_len;
        if (client->close_after_flush)
            break;
    }
    if (off)
    {
        size_t remaining = client->in_len - off;
        if (remaining)
            memmove(client->inbuf, client->inbuf + off, remaining);
            
        client->in_len = remaining;
    }
    return 0;
}

static int client_flush_output(int epoll_fd, int slot_idx)
{
    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    while (client->out_off < client->out_len)
    {
        ssize_t n = write(client->fd, client->outbuf + client->out_off, client->out_len - client->out_off);
        if (n > 0)
        {
            client->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return client_update_epoll(epoll_fd, slot_idx, client);
        return -1;
    }
    client->out_off = 0;
    client->out_len = 0;
    if (client->close_after_flush)
        client_close_slot(epoll_fd, slot_idx, "response sent");
    else if (client->active)
        return client_update_epoll(epoll_fd, slot_idx, client);
    return 0;
}

void client_accept(int epoll_fd, int listen_fd)
{
    for (;;)
    {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                lcs_log_debug("local client accept failed: %s", strerror(errno));
            return;
        }
        int slot_idx = -1;
        for (size_t i = 0; i < LCS_LOCAL_CLIENT_MAX; i++)
        {
            if (!g_state.local_clients[i].active)
            {
                slot_idx = (int)i;
                break;
            }
        }
        if (slot_idx < 0)
        {
            lcs_log_warn("rejecting local client: client table full");
            close(fd);
            continue;
        }
        local_client_runtime_t *client = &g_state.local_clients[slot_idx];
        memset(client, 0, sizeof(*client));
        client->fd = fd;
        client->id = ++g_state.next_local_client_id;
        if (client->id == 0)
            client->id = ++g_state.next_local_client_id;
        client->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
        if (client_alloc_buffers(client) != 0)
        {
            lcs_log_warn("rejecting local client: failed to allocate buffers");
            close(fd);
            memset(client, 0, sizeof(*client));
            client->fd = -1;
            continue;
        }
        client->active = true;
        if (lcs_add_epoll_fd_events(epoll_fd, fd, client_epoll_id(slot_idx), EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
        {
            client_close_slot(epoll_fd, slot_idx, "epoll add failed");
            continue;
        }
        lcs_log_debug3("accepted local client slot=%d fd=%d", slot_idx, fd);
    }
}

void client_pump_epoll_event(int epoll_fd, const struct epoll_event *ev)
{
    int slot_idx = client_index_from_epoll_id(ev->data.u32);
    if (slot_idx < 0)
        return;

    local_client_runtime_t *client = &g_state.local_clients[slot_idx];
    if (!client->active)
        return;

    if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        client_close_slot(epoll_fd, slot_idx, "connection closed");
        return;
    }
    if ((ev->events & EPOLLOUT) && client_flush_output(epoll_fd, slot_idx) != 0)
    {
        client_close_slot(epoll_fd, slot_idx, "write failed");
        return;
    }
    if (!(ev->events & EPOLLIN) || !client->active)
        return;
        
    for (;;)
    {
        if (client->in_len == LCS_LOCAL_CLIENT_INBUF_SIZE)
        {
            client_close_slot(epoll_fd, slot_idx, "input buffer full");
            return;
        }
        ssize_t n = read(client->fd, client->inbuf + client->in_len, LCS_LOCAL_CLIENT_INBUF_SIZE - client->in_len);
        if (n > 0)
        {
            client->in_len += (size_t)n;
            if (client_parse_frames(epoll_fd, slot_idx) != 0)
            {
                client_close_slot(epoll_fd, slot_idx, "invalid frame");
                return;
            }
            if (!client->active || client->out_len > client->out_off)
                return;
            continue;
        }
        if (n == 0)
        {
            client_close_slot(epoll_fd, slot_idx, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        client_close_slot(epoll_fd, slot_idx, "read failed");
        return;
    }
}

void client_expire(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_LOCAL_CLIENT_MAX; i++)
    {
        local_client_runtime_t *client = &g_state.local_clients[i];
        if (client->active && client->deadline_ms && now >= client->deadline_ms)
            client_close_slot(epoll_fd, (int)i, "client timeout");
    }
}

void client_close_all(int epoll_fd)
{
    for (size_t i = 0; i < LCS_LOCAL_CLIENT_MAX; i++)
    {
        if (g_state.local_clients[i].active)
            client_close_slot(epoll_fd, (int)i, "shutdown");
    }
}
