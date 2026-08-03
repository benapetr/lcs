// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "cli_server.h"

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

static uint32_t cli_server_epoll_id(int slot_idx)
{
    return LCS_EPOLL_CLI_SERVER_BASE + (uint32_t)slot_idx;
}

int cli_server_index_from_epoll_id(uint32_t id)
{
    if (id < LCS_EPOLL_CLI_SERVER_BASE ||
        id >= LCS_EPOLL_CLI_SERVER_BASE + LCS_CLI_SERVER_MAX)
        return -1;
    return (int)(id - LCS_EPOLL_CLI_SERVER_BASE);
}

static int cli_server_update_epoll(int epoll_fd, int slot_idx, const cli_server_runtime_t *client)
{
    if (!client->active || client->fd < 0)
        return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (client->out_len > client->out_off)
        ev.events |= EPOLLOUT;
    ev.data.u32 = cli_server_epoll_id(slot_idx);
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, client->fd, &ev);
}

static void cli_server_free_buffers(cli_server_runtime_t *client)
{
    free(client->inbuf);
    free(client->outbuf);
    client->inbuf = NULL;
    client->outbuf = NULL;
}

static int cli_server_alloc_buffers(cli_server_runtime_t *client)
{
    client->inbuf = malloc(LCS_CLI_SERVER_INBUF_SIZE);
    if (!client->inbuf)
        return -1;
    client->outbuf = malloc(LCS_CLI_SERVER_OUTBUF_SIZE);
    if (!client->outbuf)
    {
        cli_server_free_buffers(client);
        return -1;
    }
    return 0;
}

static void cli_server_close_slot(int epoll_fd, int slot_idx, const char *reason)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active)
        return;
    uint64_t cli_server_id = client->id;
    lcs_log_debug3("closing CLI connection slot=%d fd=%d reason=%s in=%zu out=%zu",
                   slot_idx, client->fd, reason ? reason : "-",
                   client->in_len,
                   client->out_len > client->out_off ?
                   client->out_len - client->out_off : 0);
    if (client->fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, client->fd, NULL);
        close(client->fd);
    }
    cli_server_free_buffers(client);
    memset(client, 0, sizeof(*client));
    client->fd = -1;
    move_cancel_cli_server(slot_idx, cli_server_id);
}

static int cli_server_queue_frame(int epoll_fd, int slot_idx, uint16_t type, uint32_t seq, const void *payload, uint32_t length)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active || !client->outbuf || length > LCS_MAX_FRAME)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = client->out_len - client->out_off;
    if (frame_len > LCS_CLI_SERVER_OUTBUF_SIZE - queued)
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
    return cli_server_update_epoll(epoll_fd, slot_idx, client);
}

static void cli_server_queue_error(int epoll_fd, int slot_idx, uint32_t seq, const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, -1, msg) == 0)
        cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_ERROR, seq, payload, (uint32_t)len);
}

static void cli_server_queue_status(int epoll_fd, int slot_idx, uint32_t seq)
{
    unsigned char payload[LCS_MAX_FRAME];
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, sizeof(payload));
    uint64_t now = lcs_now_ms();
    uint64_t membership_seconds = g_state.membership_since_ms && now >= g_state.membership_since_ms ?
                                  (now - g_state.membership_since_ms) / 1000u : 0;
    if (lcs_encode_status_header(&w, (uint16_t)g_state.cfg.node_count,
                                 (uint16_t)g_state.cfg.resource_count,
                                 (uint16_t)g_state.self_index,
                                 (uint16_t)g_state.quorum_needed,
                                 (uint16_t)g_state.votes_seen,
                                 cluster_has_quorum() ? 1 : 0,
                                 membership_seconds) != 0)
    {
        cli_server_queue_error(epoll_fd, slot_idx, seq, "failed to encode status header");
        return;
    }
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if (lcs_encode_status_node(&w, (uint16_t)i,
                                   (uint16_t)g_state.cfg.nodes[i].role,
                                   (uint8_t)cluster_node_state(i),
                                   i == (size_t)g_state.self_index ? 1 : 0,
                                   g_state.cfg.nodes[i].name) != 0)
        {
            cli_server_queue_error(epoll_fd, slot_idx, seq,  "failed to encode status node");
            return;
        }
    }
    for (size_t i = 0; i < g_state.cfg.resource_count; i++)
    {
        uint16_t owner = g_state.resources[i].owner_node < 0 ?
                         UINT16_MAX : (uint16_t)g_state.resources[i].owner_node;
        const char *group = g_state.cfg.resources[i].group_idx >= 0 ?
                            g_state.cfg.groups[g_state.cfg.resources[i].group_idx].name : "";
        const char *home_node = g_state.cfg.resources[i].home_node_idx >= 0 ?
                                g_state.cfg.nodes[g_state.cfg.resources[i].home_node_idx].name : "";
        const char *type = lcs_resource_type_name(g_state.cfg.resources[i].type);
        if (lcs_encode_status_resource(&w, (uint16_t)i, owner,
                                  g_state.resources[i].epoch,
                                  g_state.resources[i].lease_id,
                                  (uint8_t)g_state.resources[i].state,
                                  g_state.cfg.resources[i].name,
                                  g_state.cfg.resources[i].address,
                                  g_state.cfg.resources[i].interface,
                                  group,
                                  g_state.cfg.resources[i].priority,
                                  home_node,
                                  type,
                                  g_state.cfg.resources[i].systemd_unit,
                                  g_state.resources[i].home_blocked ? 1 : 0,
                                  g_state.resources[i].disabled ? 1 : 0,
                                  g_state.resources[i].conflict_reason) != 0)
        {
            cli_server_queue_error(epoll_fd, slot_idx, seq, "failed to encode status resource");
            return;
        }
    }
    cli_server_queue_frame(epoll_fd, slot_idx, LCS_MSG_STATUS_RESP, seq, payload, (uint32_t)w.len);
}

static void cli_server_queue_simple_response(int epoll_fd, int slot_idx, uint16_t type, uint32_t seq, int32_t status, const char *message)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, status, message) == 0)
        cli_server_queue_frame(epoll_fd, slot_idx, type, seq, payload, (uint32_t)len);
}

void cli_server_complete_move(int epoll_fd, int slot_idx, uint64_t cli_server_id, uint32_t seq, int32_t status, const char *message)
{
    if (slot_idx < 0 || slot_idx >= LCS_CLI_SERVER_MAX)
        return;
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active || client->id != cli_server_id)
        return;
    cli_server_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_MOVE_RESP, seq, status, message);
    client->close_after_flush = true;
}

static void cli_server_queue_clear_conflict(int epoll_fd, int slot_idx, uint32_t seq, const void *payload, uint32_t len)
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
        int resource_idx = lcs_config_resource_index(&g_state.cfg, vip_name);
        if (resource_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown VIP");
        } else
        {
            resource_runtime_t *res = &g_state.resources[resource_idx];
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
                lcs_log_info("admin cleared conflict for resource %s epoch=%llu", g_state.cfg.resources[resource_idx].name, (unsigned long long)res->epoch);
            }
        }
    }
    cli_server_queue_simple_response(epoll_fd, slot_idx, LCS_MSG_CLEAR_CONFLICT_RESP, seq, status, message);
}

static void cli_server_queue_resource_control(int epoll_fd,
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
        int resource_idx = lcs_config_resource_index(&g_state.cfg, resource_name);
        if (resource_idx < 0)
        {
            snprintf(message, sizeof(message), "unknown resource");
        } else if (resources_set_disabled(resource_idx, disabled, epoll_fd,
                                          message, sizeof(message)) == 0)
        {
            status = 0;
        }
    }
    cli_server_queue_simple_response(epoll_fd, slot_idx, resp_type, seq, status, message);
}

static int cli_server_process_frame(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr, const unsigned char *payload)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    switch (hdr->type)
    {
        case LCS_MSG_STATUS_REQ:
            cli_server_queue_status(epoll_fd, slot_idx, hdr->seq);
            break;
        case LCS_MSG_MOVE_REQ:
            move_start_cli_server(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            return 0;
        case LCS_MSG_CLEAR_CONFLICT_REQ:
            cli_server_queue_clear_conflict(epoll_fd, slot_idx, hdr->seq, payload, hdr->length);
            break;
        case LCS_MSG_RESOURCE_START_REQ:
            cli_server_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_START_RESP, hdr->seq, payload, hdr->length, false);
            break;
        case LCS_MSG_RESOURCE_STOP_REQ:
            cli_server_queue_resource_control(epoll_fd, slot_idx, LCS_MSG_RESOURCE_STOP_RESP, hdr->seq, payload, hdr->length, true);
            break;
        default:
            cli_server_queue_error(epoll_fd, slot_idx, hdr->seq, "unsupported local CLI message");
            break;
    }
    client->close_after_flush = true;
    return 0;
}

static int cli_server_parse_frames(int epoll_fd, int slot_idx)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
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

        if (cli_server_process_frame(epoll_fd, slot_idx, &hdr, client->inbuf + off + LCS_FRAME_HEADER_SIZE) != 0)
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

static int cli_server_flush_output(int epoll_fd, int slot_idx)
{
    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    while (client->out_off < client->out_len)
    {
        ssize_t n = write(client->fd, client->outbuf + client->out_off, client->out_len - client->out_off);
        if (n > 0)
        {
            client->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return cli_server_update_epoll(epoll_fd, slot_idx, client);
        return -1;
    }
    client->out_off = 0;
    client->out_len = 0;
    if (client->close_after_flush)
        cli_server_close_slot(epoll_fd, slot_idx, "response sent");
    else if (client->active)
        return cli_server_update_epoll(epoll_fd, slot_idx, client);
    return 0;
}

void cli_server_accept(int epoll_fd, int listen_fd)
{
    for (;;)
    {
        int fd = accept4(listen_fd, NULL, NULL, SOCK_CLOEXEC | SOCK_NONBLOCK);
        if (fd < 0)
        {
            if (errno != EAGAIN && errno != EWOULDBLOCK)
                lcs_log_debug("CLI connection accept failed: %s", strerror(errno));
            return;
        }
        int slot_idx = -1;
        for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
        {
            if (!g_state.cli_servers[i].active)
            {
                slot_idx = (int)i;
                break;
            }
        }
        if (slot_idx < 0)
        {
            lcs_log_warn("rejecting CLI connection: connection table full");
            close(fd);
            continue;
        }
        cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
        memset(client, 0, sizeof(*client));
        client->fd = fd;
        client->id = ++g_state.next_cli_server_id;
        if (client->id == 0)
            client->id = ++g_state.next_cli_server_id;
        client->deadline_ms = lcs_now_ms() + g_state.cfg.peer_timeout_ms;
        if (cli_server_alloc_buffers(client) != 0)
        {
            lcs_log_warn("rejecting CLI connection: failed to allocate buffers");
            close(fd);
            memset(client, 0, sizeof(*client));
            client->fd = -1;
            continue;
        }
        client->active = true;
        if (lcs_add_epoll_fd_events(epoll_fd, fd, cli_server_epoll_id(slot_idx), EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "epoll add failed");
            continue;
        }
        lcs_log_debug3("accepted CLI connection slot=%d fd=%d", slot_idx, fd);
    }
}

void cli_server_pump_epoll_event(int epoll_fd, const struct epoll_event *ev)
{
    int slot_idx = cli_server_index_from_epoll_id(ev->data.u32);
    if (slot_idx < 0)
        return;

    cli_server_runtime_t *client = &g_state.cli_servers[slot_idx];
    if (!client->active)
        return;

    if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        cli_server_close_slot(epoll_fd, slot_idx, "connection closed");
        return;
    }
    if ((ev->events & EPOLLOUT) && cli_server_flush_output(epoll_fd, slot_idx) != 0)
    {
        cli_server_close_slot(epoll_fd, slot_idx, "write failed");
        return;
    }
    if (!(ev->events & EPOLLIN) || !client->active)
        return;
        
    for (;;)
    {
        if (client->in_len == LCS_CLI_SERVER_INBUF_SIZE)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "input buffer full");
            return;
        }
        ssize_t n = read(client->fd, client->inbuf + client->in_len, LCS_CLI_SERVER_INBUF_SIZE - client->in_len);
        if (n > 0)
        {
            client->in_len += (size_t)n;
            if (cli_server_parse_frames(epoll_fd, slot_idx) != 0)
            {
                cli_server_close_slot(epoll_fd, slot_idx, "invalid frame");
                return;
            }
            if (!client->active || client->out_len > client->out_off)
                return;
            continue;
        }
        if (n == 0)
        {
            cli_server_close_slot(epoll_fd, slot_idx, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        cli_server_close_slot(epoll_fd, slot_idx, "read failed");
        return;
    }
}

void cli_server_expire(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
    {
        cli_server_runtime_t *client = &g_state.cli_servers[i];
        if (client->active && client->deadline_ms && now >= client->deadline_ms)
            cli_server_close_slot(epoll_fd, (int)i, "CLI connection timeout");
    }
}

void cli_server_close_all(int epoll_fd)
{
    for (size_t i = 0; i < LCS_CLI_SERVER_MAX; i++)
    {
        if (g_state.cli_servers[i].active)
            cli_server_close_slot(epoll_fd, (int)i, "shutdown");
    }
}
