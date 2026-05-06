// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "peer.h"

#include "cluster.h"
#include "epoll_util.h"
#include "lease.h"
#include "log.h"
#include "move.h"
#include "protocol.h"
#include "util.h"

#include <arpa/inet.h>
#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <unistd.h>

/* Forward declarations */
static int peer_update_epoll(int epoll_fd, int node_idx);
static int peer_queue_frame(int epoll_fd, int node_idx, uint16_t type, uint32_t seq, const void *payload, uint32_t length);
static int peer_flush_output(int epoll_fd, int node_idx);
static int peer_queue_outbound_hello(int epoll_fd, int node_idx);
static void peer_handle_event(int epoll_fd, int node_idx);
static int peer_register_connection(int epoll_fd, int node_idx, int fd, bool outbound);
static void peer_mark_seen(int node_idx, uint64_t instance_id);
static int peer_validate_instance_for_hello(int node_idx, uint64_t instance_id);
static int handshake_update_epoll(int epoll_fd, int slot_idx);
static int handshake_flush_output(int epoll_fd, int slot_idx);
static void peer_mark_sync_failed(int node_idx);
static int peer_send_state_sync(int epoll_fd, int node_idx);
static void peer_free_buffers(peer_runtime_t *peer);
static void handshake_free_buffers(inbound_handshake_t *hs);

static int peer_ensure_buffers(peer_runtime_t *peer)
{
    if (!peer->inbuf)
    {
        peer->inbuf = malloc(LCS_PEER_INBUF_SIZE);
        if (!peer->inbuf)
            return -1;
    }
    if (!peer->outbuf)
    {
        peer->outbuf = malloc(LCS_PEER_OUTBUF_SIZE);
        if (!peer->outbuf)
        {
            peer_free_buffers(peer);
            return -1;
        }
    }
    return 0;
}

static void peer_free_buffers(peer_runtime_t *peer)
{
    free(peer->inbuf);
    free(peer->outbuf);
    peer->inbuf = NULL;
    peer->outbuf = NULL;
}

static int handshake_ensure_buffers(inbound_handshake_t *hs)
{
    if (!hs->inbuf)
    {
        hs->inbuf = malloc(LCS_PEER_INBUF_SIZE);
        if (!hs->inbuf)
            return -1;
    }
    if (!hs->outbuf)
    {
        hs->outbuf = malloc(LCS_PEER_INBUF_SIZE);
        if (!hs->outbuf)
        {
            handshake_free_buffers(hs);
            return -1;
        }
    }
    return 0;
}

static void handshake_free_buffers(inbound_handshake_t *hs)
{
    free(hs->inbuf);
    free(hs->outbuf);
    hs->inbuf = NULL;
    hs->outbuf = NULL;
}

static bool peer_is_request_type(uint16_t type)
{
    switch (type)
    {
        case LCS_MSG_STATE_SYNC_REQ:
        case LCS_MSG_LEASE_REQ:
        case LCS_MSG_LEASE_RENEW:
        case LCS_MSG_LEASE_RELEASE:
        case LCS_MSG_OWNER_RELEASE_REQ:
        case LCS_MSG_MOVE_REQ:
            return true;
        default:
            return false;
    }
}

static uint32_t peer_epoll_id(int node_idx)
{
    return LCS_EPOLL_PEER_CONN_BASE + (uint32_t)node_idx;
}

static int peer_index_from_epoll_id(uint32_t id)
{
    if (id < LCS_EPOLL_PEER_CONN_BASE)
        return -1;
    if (id >= LCS_EPOLL_HANDSHAKE_BASE)
        return -1;
    return (int)(id - LCS_EPOLL_PEER_CONN_BASE);
}

static uint32_t handshake_epoll_id(int slot_idx)
{
    return LCS_EPOLL_HANDSHAKE_BASE + (uint32_t)slot_idx;
}

static int handshake_index_from_epoll_id(uint32_t id)
{
    if (id < LCS_EPOLL_HANDSHAKE_BASE || id >= LCS_EPOLL_HANDSHAKE_BASE + LCS_HANDSHAKE_MAX)
        return -1;
    return (int)(id - LCS_EPOLL_HANDSHAKE_BASE);
}

static int peer_update_epoll(int epoll_fd, int node_idx)
{
    if (epoll_fd < 0)
        return 0;
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return -1;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->fd < 0)
        return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (peer->conn_state == LCS_PEER_CONNECTING || peer->out_len > peer->out_off)
        ev.events |= EPOLLOUT;
    ev.data.u32 = peer_epoll_id(node_idx);
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, peer->fd, &ev);
}

static int peer_encode_hello(unsigned char *payload, size_t cap, size_t *len, uint8_t mode)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u16(&w, LCS_PROTO_VERSION) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)g_state.self_index) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.node_count) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.group_count) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.vip_count) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)g_state.cfg.nodes[g_state.self_index].role) != 0 ||
        lcs_buf_put_u8(&w, mode) != 0 ||
        lcs_buf_put_u64(&w, g_state.instance_id) != 0 ||
        lcs_buf_put_fixed_string(&w, g_state.cfg.nodes[g_state.self_index].name, LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_put_fixed_string(&w, g_state.cfg.cluster_name, LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_put_fixed_string(&w, g_state.cfg.secret, LCS_NAME_MAX + 1) != 0)
        return -1;
    *len = w.len;
    return 0;
}

static int peer_decode_hello(const void *payload, size_t len,
                        int *node_idx, uint64_t *instance_id, uint8_t *mode)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint16_t proto_version, remote_idx, node_count, group_count, vip_count, role;
    char name[LCS_NAME_MAX + 1];
    char cluster_name[LCS_NAME_MAX + 1];
    char secret[LCS_NAME_MAX + 1];
    if (lcs_buf_get_u16(&r, &proto_version) != 0 ||
        lcs_buf_get_u16(&r, &remote_idx) != 0 ||
        lcs_buf_get_u16(&r, &node_count) != 0 ||
        lcs_buf_get_u16(&r, &group_count) != 0 ||
        lcs_buf_get_u16(&r, &vip_count) != 0 ||
        lcs_buf_get_u16(&r, &role) != 0 ||
        lcs_buf_get_u8(&r, mode) != 0 ||
        lcs_buf_get_u64(&r, instance_id) != 0 ||
        lcs_buf_get_fixed_string(&r, name, sizeof(name), LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_get_fixed_string(&r, cluster_name, sizeof(cluster_name), LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_get_fixed_string(&r, secret, sizeof(secret), LCS_NAME_MAX + 1) != 0)
        return -1;
    if (proto_version != LCS_PROTO_VERSION)
    {
        lcs_log_debug("rejecting HELLO with protocol version %u, expected %u", proto_version, LCS_PROTO_VERSION);
        return -1;
    }
    int idx = lcs_config_node_index(&g_state.cfg, name);
    if (idx < 0 || idx != (int)remote_idx ||
        node_count != g_state.cfg.node_count ||
        group_count != g_state.cfg.group_count ||
        vip_count != g_state.cfg.vip_count ||
        role != (uint16_t)g_state.cfg.nodes[idx].role)
        return -1;
    if (strcmp(cluster_name, g_state.cfg.cluster_name) != 0)
        return -1;
    if (*g_state.cfg.secret && strcmp(secret, g_state.cfg.secret) != 0)
        return -1;
    if (*mode != LCS_HELLO_MODE_PERSISTENT)
        return -1;
    *node_idx = idx;
    return 0;
}

static int peer_queue_raw_frame(unsigned char *outbuf, size_t outbuf_cap,
                           size_t *out_off, size_t *out_len,
                           uint16_t type, uint32_t seq, const void *payload,
                           uint32_t length)
{
    if (length > LCS_MAX_FRAME)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = *out_len - *out_off;
    if (frame_len > outbuf_cap - queued)
        return -1;
    if (*out_off && queued)
        memmove(outbuf, outbuf + *out_off, queued);
    *out_off = 0;
    *out_len = queued;
    lcs_frame_header_t wire;
    wire.magic = htonl(LCS_PROTO_MAGIC);
    wire.type = htons(type);
    wire.flags = 0;
    wire.length = htonl(length);
    wire.seq = htonl(seq);
    memcpy(outbuf + *out_len, &wire, sizeof(wire));
    *out_len += sizeof(wire);
    if (length)
    {
        memcpy(outbuf + *out_len, payload, length);
        *out_len += length;
    }
    return 0;
}

static int peer_queue_frame(int epoll_fd, int node_idx,
                            uint16_t type, uint32_t seq, const void *payload,
                            uint32_t length)
{
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count || length > LCS_MAX_FRAME)
        return -1;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->fd < 0 || !peer->outbuf)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = peer->out_len - peer->out_off;
    if (frame_len > LCS_PEER_OUTBUF_SIZE - queued)
    {
        lcs_log_debug3("peer %s output queue full while queuing type=%u seq=%u queued=%zu need=%zu",
                       g_state.cfg.nodes[node_idx].name, type, seq, queued, frame_len);
        return -1;
    }
    if (peer->out_off && queued)
        memmove(peer->outbuf, peer->outbuf + peer->out_off, queued);
    peer->out_off = 0;
    peer->out_len = queued;
    lcs_frame_header_t wire;
    wire.magic = htonl(LCS_PROTO_MAGIC);
    wire.type = htons(type);
    wire.flags = 0;
    wire.length = htonl(length);
    wire.seq = htonl(seq);
    memcpy(peer->outbuf + peer->out_len, &wire, sizeof(wire));
    peer->out_len += sizeof(wire);
    if (length)
    {
        memcpy(peer->outbuf + peer->out_len, payload, length);
        peer->out_len += length;
    }
    lcs_log_debug3("queued peer %s frame type=%u seq=%u length=%u pending_bytes=%zu",
                   g_state.cfg.nodes[node_idx].name, type, seq, length,
                   peer->out_len - peer->out_off);
    return peer_update_epoll(epoll_fd, node_idx);
}

int peer_queue_simple_resp(int epoll_fd, int node_idx,
                           uint32_t seq, uint16_t type, int32_t status,
                           const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, status, msg) != 0)
        return -1;
        
    return peer_queue_frame(epoll_fd, node_idx, type, seq, payload, (uint32_t)len);
}

static int peer_flush_output(int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    while (peer->out_off < peer->out_len)
    {
        ssize_t n = write(peer->fd, peer->outbuf + peer->out_off,
                          peer->out_len - peer->out_off);
        if (n > 0)
        {
            peer->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return peer_update_epoll(epoll_fd, node_idx);
        lcs_log_debug3("peer %s write failed: %s", g_state.cfg.nodes[node_idx].name,
                       n == 0 ? "short write" : strerror(errno));
        return -1;
    }
    peer->out_off = 0;
    peer->out_len = 0;
    return peer_update_epoll(epoll_fd, node_idx);
}

static int peer_handle_connect_ready(int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(peer->fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0)
    {
        errno = so_error ? so_error : errno;
        lcs_log_debug3("peer %s outbound connect failed: %s",
                       g_state.cfg.nodes[node_idx].name, strerror(errno));
        return -1;
    }
    lcs_log_debug3("peer %s outbound connect completed", g_state.cfg.nodes[node_idx].name);
    return peer_queue_outbound_hello(epoll_fd, node_idx);
}

static int handshake_update_epoll(int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    if (!hs->active || hs->fd < 0)
        return -1;
    struct epoll_event ev;
    memset(&ev, 0, sizeof(ev));
    ev.events = EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP;
    if (hs->out_len > hs->out_off)
        ev.events |= EPOLLOUT;
    ev.data.u32 = handshake_epoll_id(slot_idx);
    return epoll_ctl(epoll_fd, EPOLL_CTL_MOD, hs->fd, &ev);
}

void handshake_close(int epoll_fd, int slot_idx, const char *reason)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    if (!hs->active)
        return;
    lcs_log_debug3("closing inbound handshake slot=%d fd=%d reason=%s in=%zu out=%zu",
                   slot_idx, hs->fd, reason ? reason : "-",
                   hs->in_len, hs->out_len > hs->out_off ? hs->out_len - hs->out_off : 0);
    if (hs->fd >= 0)
    {
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, hs->fd, NULL);
        close(hs->fd);
    }
    handshake_free_buffers(hs);
    memset(hs, 0, sizeof(*hs));
    hs->fd = -1;
    hs->node_idx = -1;
}

static int handshake_promote(int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    int fd = hs->fd;
    int node_idx = hs->node_idx;
    uint64_t instance_id = hs->instance_id;
    if (fd < 0 || node_idx < 0)
        return -1;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    hs->active = false;
    hs->fd = -1;
    peer_mark_seen(node_idx, instance_id);
    handshake_free_buffers(hs);
    hs->in_len = 0;
    hs->out_off = 0;
    hs->out_len = 0;
    if (peer_register_connection(epoll_fd, node_idx, fd, false) != 0)
    {
        close(fd);
        return -1;
    }
    return 0;
}

static int handshake_flush_output(int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    while (hs->out_off < hs->out_len)
    {
        ssize_t n = write(hs->fd, hs->outbuf + hs->out_off, hs->out_len - hs->out_off);
        if (n > 0)
        {
            hs->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return handshake_update_epoll(epoll_fd, slot_idx);
        return -1;
    }
    hs->out_off = 0;
    hs->out_len = 0;
    return handshake_promote(epoll_fd, slot_idx);
}

static int handshake_process_frame(int epoll_fd, int slot_idx, const lcs_frame_header_t *hdr, const unsigned char *payload)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    if (hdr->type != LCS_MSG_HELLO)
    {
        lcs_log_warn("peer client sent message type %u before HELLO", hdr->type);
        return -1;
    }
    int node_idx = -1;
    uint64_t instance_id = 0;
    uint8_t mode = 0;
    if (peer_decode_hello(payload, hdr->length, &node_idx, &instance_id, &mode) != 0)
    {
        lcs_log_debug("invalid inbound peer HELLO");
        return -1;
    }
    if (node_idx == g_state.self_index)
    {
        lcs_log_debug("rejecting persistent connection from self");
        return -1;
    }
    if (node_idx > g_state.self_index)
    {
        lcs_log_debug("rejecting inbound persistent connection from %s; lower node owns outbound side",
                      g_state.cfg.nodes[node_idx].name);
        return -1;
    }
    if (peer_validate_instance_for_hello(node_idx, instance_id) != 0)
    {
        lcs_log_debug("rejecting stale inbound HELLO from %s", g_state.cfg.nodes[node_idx].name);
        return -1;
    }
    unsigned char resp[LCS_MAX_FRAME];
    size_t resp_len = 0;
    if (peer_encode_hello(resp, sizeof(resp), &resp_len, mode) != 0 ||
        peer_queue_raw_frame(hs->outbuf, LCS_PEER_INBUF_SIZE, &hs->out_off, &hs->out_len,
                        LCS_MSG_HELLO_ACK, hdr->seq, resp, (uint32_t)resp_len) != 0)
        return -1;
    hs->node_idx = node_idx;
    hs->instance_id = instance_id;
    lcs_log_debug3("inbound HELLO accepted from %s slot=%d seq=%u",
                   g_state.cfg.nodes[node_idx].name, slot_idx, hdr->seq);
    return handshake_flush_output(epoll_fd, slot_idx);
}

static int handshake_parse_frames(int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    if (hs->in_len < LCS_FRAME_HEADER_SIZE)
        return 0;
    lcs_frame_header_t wire;
    memcpy(&wire, hs->inbuf, sizeof(wire));
    lcs_frame_header_t hdr;
    hdr.magic = ntohl(wire.magic);
    hdr.type = ntohs(wire.type);
    hdr.flags = ntohs(wire.flags);
    hdr.length = ntohl(wire.length);
    hdr.seq = ntohl(wire.seq);
    if (hdr.magic != LCS_PROTO_MAGIC || hdr.flags != 0 || hdr.length > LCS_MAX_FRAME)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)hdr.length;
    if (hs->in_len < frame_len)
        return 0;
    return handshake_process_frame(epoll_fd, slot_idx, &hdr,
                                   hs->inbuf + LCS_FRAME_HEADER_SIZE);
}

static void handshake_handle_event(int epoll_fd, int slot_idx, uint32_t events)
{
    inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
    if (!hs->active)
        return;
    if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        handshake_close(epoll_fd, slot_idx, "connection closed");
        return;
    }
    if ((events & EPOLLOUT) && handshake_flush_output(epoll_fd, slot_idx) != 0)
    {
        handshake_close(epoll_fd, slot_idx, "write failed");
        return;
    }
    if (!(events & EPOLLIN) || !hs->active)
        return;
    for (;;)
    {
        if (hs->in_len == LCS_PEER_INBUF_SIZE)
        {
            handshake_close(epoll_fd, slot_idx, "input buffer full");
            return;
        }
        ssize_t n = read(hs->fd, hs->inbuf + hs->in_len, LCS_PEER_INBUF_SIZE - hs->in_len);
        if (n > 0)
        {
            hs->in_len += (size_t)n;
            if (handshake_parse_frames(epoll_fd, slot_idx) != 0)
            {
                handshake_close(epoll_fd, slot_idx, "invalid handshake");
                return;
            }
            if (!hs->active || hs->out_len > hs->out_off)
                return;
            continue;
        }
        if (n == 0)
        {
            handshake_close(epoll_fd, slot_idx, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        handshake_close(epoll_fd, slot_idx, "read failed");
        return;
    }
}

static const char *peer_error_message(const void *payload, size_t len, char *buf, size_t buf_len)
{
    int32_t status = -1;
    if (lcs_decode_simple_resp(payload, len, &status, buf, buf_len) == 0 && *buf)
        return buf;
    snprintf(buf, buf_len, "undecodable error payload length=%zu", len);
    return buf;
}

static size_t peer_inflight_count(const peer_runtime_t *peer)
{
    size_t count = 0;
    for (size_t i = 0; i < LCS_MAX_PEER_RPC_INFLIGHT; i++)
    {
        if (peer->in_flight[i].active)
            count++;
    }
    return count;
}

static void peer_clear_rpc(peer_rpc_runtime_t *rpc)
{
    memset(rpc, 0, sizeof(*rpc));
}

static void peer_complete_rpc(peer_rpc_runtime_t *rpc, int status)
{
    rpc->status = status;
    rpc->done = true;
    if (rpc->callback)
    {
        uint32_t len = rpc->resp_len ? *rpc->resp_len : 0;
        rpc->callback(rpc->callback_ctx, status, rpc->resp_payload, len);
        peer_clear_rpc(rpc);
    }
}

static peer_rpc_runtime_t *peer_find_rpc_by_seq(peer_runtime_t *peer, uint32_t seq)
{
    for (size_t i = 0; i < LCS_MAX_PEER_RPC_INFLIGHT; i++)
    {
        if (peer->in_flight[i].active && peer->in_flight[i].seq == seq)
            return &peer->in_flight[i];
    }
    return NULL;
}

static peer_rpc_runtime_t *peer_find_rpc(peer_runtime_t *peer, uint32_t seq,
                                         uint16_t expected_type)
{
    peer_rpc_runtime_t *rpc = peer_find_rpc_by_seq(peer, seq);
    if (!rpc || rpc->expected_type != expected_type)
        return NULL;
    return rpc;
}

static peer_rpc_runtime_t *peer_register_rpc(peer_runtime_t *peer, int peer_idx,
                                             uint16_t req_type,
                                             uint16_t expected_type,
                                             uint64_t deadline_ms,
                                             unsigned char *resp_payload,
                                             size_t resp_cap,
                                             uint32_t *resp_len,
                                             peer_rpc_callback_t callback,
                                             void *callback_ctx)
{
    for (size_t i = 0; i < LCS_MAX_PEER_RPC_INFLIGHT; i++)
    {
        peer_rpc_runtime_t *rpc = &peer->in_flight[i];
        if (rpc->active)
            continue;
        peer_clear_rpc(rpc);
        rpc->active = true;
        rpc->peer_idx = peer_idx;
        rpc->seq = lcs_next_seq();
        rpc->req_type = req_type;
        rpc->expected_type = expected_type;
        rpc->deadline_ms = deadline_ms;
        rpc->resp_payload = resp_payload;
        rpc->resp_cap = resp_cap;
        rpc->resp_len = resp_len;
        rpc->callback = callback;
        rpc->callback_ctx = callback_ctx;
        if (resp_len)
            *resp_len = 0;
        return rpc;
    }
    return NULL;
}

static void peer_fail_inflight_rpcs(peer_runtime_t *peer)
{
    for (size_t i = 0; i < LCS_MAX_PEER_RPC_INFLIGHT; i++)
    {
        peer_rpc_runtime_t *rpc = &peer->in_flight[i];
        if (rpc->active && !rpc->done)
            peer_complete_rpc(rpc, -1);
    }
}

static void peer_reset_sequence_cache(peer_runtime_t *peer)
{
    memset(peer->seen_request_seqs, 0, sizeof(peer->seen_request_seqs));
    peer->seen_request_pos = 0;
}

static int peer_remember_request_sequence(int source_node_idx, const lcs_frame_header_t *hdr)
{
    if (source_node_idx < 0 || (size_t)source_node_idx >= g_state.cfg.node_count ||
        !peer_is_request_type(hdr->type))
        return 0;
    peer_runtime_t *peer = &g_state.peers[source_node_idx];
    for (size_t i = 0; i < LCS_SEQ_CACHE_SIZE; i++)
    {
        if (peer->seen_request_seqs[i] == hdr->seq)
        {
            lcs_log_debug("rejecting duplicate peer request from %s seq=%u type=%u",
                          g_state.cfg.nodes[source_node_idx].name, hdr->seq, hdr->type);
            return -1;
        }
    }
    peer->seen_request_seqs[peer->seen_request_pos] = hdr->seq;
    peer->seen_request_pos = (peer->seen_request_pos + 1) % LCS_SEQ_CACHE_SIZE;
    return 0;
}

static int peer_handle_request_frame(int epoll_fd, int source_node_idx,
                                     const lcs_frame_header_t *hdr,
                                     unsigned char *payload)
{
    size_t len = 0;
    if (hdr->seq == 0)
    {
        peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR, -1, "invalid sequence");
        return -1;
    }
    if (peer_remember_request_sequence(source_node_idx, hdr) != 0)
    {
        peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR, -1, "duplicate request sequence");
        return -1;
    }
    switch (hdr->type)
    {
        case LCS_MSG_STATE_SYNC_REQ:
            if (hdr->length && cluster_apply_state(payload, hdr->length, source_node_idx) != 0)
            {
                peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR, -1, "failed to apply state");
                return -1;
            }
            if (cluster_encode_state(payload, LCS_MAX_FRAME, &len) != 0)
            {
                peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR, -1, "failed to encode state");
                return -1;
            }
            return peer_queue_frame(epoll_fd, source_node_idx, LCS_MSG_STATE_SYNC_RESP, hdr->seq, payload, (uint32_t)len);
        case LCS_MSG_LEASE_REQ:
        case LCS_MSG_LEASE_RENEW:
        case LCS_MSG_LEASE_RELEASE:
            if (lease_accept_message(hdr->type, payload, hdr->length, source_node_idx) == 0)
                return peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_LEASE_ACK, 0, "ok");

            return peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_LEASE_ACK, -1, "lease rejected");
        case LCS_MSG_OWNER_RELEASE_REQ:
            if (lease_handle_owner_release_request(payload, hdr->length, source_node_idx) == 0)
                return peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_OWNER_RELEASE_RESP, 0, "ok");

            return peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_OWNER_RELEASE_RESP, -1, "owner release rejected");
        case LCS_MSG_MOVE_REQ:
            move_start_peer_request(epoll_fd, source_node_idx, hdr->seq, payload, hdr->length);
            return 0;
        default:
            peer_queue_simple_resp(epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR, -1, "unsupported peer message");
            return -1;
    }
}

static void peer_mark_seen(int node_idx, uint64_t instance_id)
{
    if (node_idx == g_state.self_index || node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return;

    bool was_online = g_state.peers[node_idx].online;
    if (g_state.peers[node_idx].instance_id != 0 && g_state.peers[node_idx].instance_id != instance_id)
        peer_reset_sequence_cache(&g_state.peers[node_idx]);

    g_state.peers[node_idx].online = true;
    g_state.peers[node_idx].instance_id = instance_id;
    g_state.peers[node_idx].last_seen_ms = lcs_now_ms();
    g_state.peers[node_idx].next_sync_ms = g_state.peers[node_idx].last_seen_ms + 1000;
    g_state.peers[node_idx].backoff_ms = 0;
    if (!was_online)
        lcs_log_info("peer %s online instance=%llu", g_state.cfg.nodes[node_idx].name, (unsigned long long)instance_id);
}

static int peer_validate_instance_for_hello(int node_idx, uint64_t instance_id)
{
    if (node_idx == g_state.self_index || node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return -1;
    const peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->conn_state == LCS_PEER_ESTABLISHED &&
        peer->instance_id != 0 &&
        peer->instance_id != instance_id)
        return -1;
    return 0;
}

static void peer_mark_sync_failed(int node_idx)
{
    if (node_idx == g_state.self_index || node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    uint64_t now = lcs_now_ms();
    if (peer->online && now - peer->last_seen_ms > g_state.cfg.peer_timeout_ms)
    {
        lcs_log_info("peer %s offline", g_state.cfg.nodes[node_idx].name);
        peer->online = false;
    }
    uint32_t next_backoff = peer->backoff_ms ? peer->backoff_ms * 2u : 1000u;
    if (next_backoff > g_state.cfg.peer_timeout_ms)
        next_backoff = g_state.cfg.peer_timeout_ms;
    peer->backoff_ms = next_backoff;
    peer->next_sync_ms = now + next_backoff;
}

void peer_close_connection(int epoll_fd, int node_idx, bool mark_offline, const char *reason)
{
    if (node_idx == g_state.self_index || node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->fd >= 0)
    {
        lcs_log_debug3("closing peer %s fd=%d mark_offline=%s reason=%s inflight=%zu out_bytes=%zu in_bytes=%zu",
                       g_state.cfg.nodes[node_idx].name, peer->fd,
                       mark_offline ? "true" : "false", reason ? reason : "-",
                       peer_inflight_count(peer),
                       peer->out_len > peer->out_off ? peer->out_len - peer->out_off : 0,
                       peer->in_len);
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, peer->fd, NULL);
        close(peer->fd);
        peer->fd = -1;
    }
    peer_free_buffers(peer);
    peer_fail_inflight_rpcs(peer);
    peer->outbound = false;
    peer->conn_state = LCS_PEER_DISCONNECTED;
    peer->connect_deadline_ms = 0;
    peer->hello_seq = 0;
    peer->in_len = 0;
    peer->out_off = 0;
    peer->out_len = 0;
    if (mark_offline && peer->online)
    {
        peer->online = false;
        lcs_log_info("peer %s offline%s%s", g_state.cfg.nodes[node_idx].name, reason ? ": " : "", reason ? reason : "");
    }

    if (mark_offline)
        peer_mark_sync_failed(node_idx);
}

static int peer_register_connection(int epoll_fd, int node_idx, int fd, bool outbound)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->fd >= 0)
        peer_close_connection(epoll_fd, node_idx, false, "duplicate session replaced");
    if (peer_ensure_buffers(peer) != 0 || lcs_set_fd_nonblocking(fd) != 0 || lcs_add_epoll_fd(epoll_fd, fd, peer_epoll_id(node_idx)) != 0)
    {
        peer_free_buffers(peer);
        return -1;
    }
    peer->fd = fd;
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->outbound = outbound;
    peer->in_len = 0;
    peer->out_off = 0;
    peer->out_len = 0;
    peer->connect_deadline_ms = 0;
    peer->hello_seq = 0;
    peer->next_sync_ms = lcs_now_ms();
    lcs_log_debug("peer %s persistent connection established direction=%s", g_state.cfg.nodes[node_idx].name, outbound ? "outbound" : "inbound");
    return 0;
}

static int peer_queue_outbound_hello(int epoll_fd, int node_idx)
{
    unsigned char hello[LCS_MAX_FRAME];
    size_t hello_len = 0;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    peer->hello_seq = lcs_next_seq();
    if (peer_encode_hello(hello, sizeof(hello), &hello_len, LCS_HELLO_MODE_PERSISTENT) != 0 ||
        peer_queue_frame(epoll_fd, node_idx, LCS_MSG_HELLO, peer->hello_seq,
                         hello, (uint32_t)hello_len) != 0)
        return -1;
    peer->conn_state = LCS_PEER_HELLO_SENT;
    lcs_log_debug3("peer %s HELLO queued seq=%u", g_state.cfg.nodes[node_idx].name, peer->hello_seq);
    return 0;
}

static int peer_complete_outbound_hello(int epoll_fd, int node_idx,
                                   const lcs_frame_header_t *hdr,
                                   const unsigned char *payload)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (hdr->type != LCS_MSG_HELLO_ACK || hdr->seq != peer->hello_seq)
    {
        if (hdr->type == LCS_MSG_ERROR)
        {
            char msg[128];
            lcs_log_warn("persistent peer %s rejected HELLO: %s",
                         g_state.cfg.nodes[node_idx].name,
                         peer_error_message(payload, hdr->length, msg, sizeof(msg)));
        } else
        {
            lcs_log_debug("persistent peer %s invalid HELLO_ACK: type=%u seq=%u expected_type=%u expected_seq=%u",
                          g_state.cfg.nodes[node_idx].name, hdr->type, hdr->seq,
                          LCS_MSG_HELLO_ACK, peer->hello_seq);
        }
        return -1;
    }
    int remote_idx = -1;
    uint64_t instance_id = 0;
    uint8_t mode = 0;
    if (peer_decode_hello(payload, hdr->length, &remote_idx, &instance_id, &mode) != 0 ||
        peer_validate_instance_for_hello(remote_idx, instance_id) != 0 ||
        mode != LCS_HELLO_MODE_PERSISTENT ||
        remote_idx != node_idx)
    {
        lcs_log_debug("persistent peer %s invalid HELLO_ACK payload", g_state.cfg.nodes[node_idx].name);
        return -1;
    }
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->hello_seq = 0;
    peer->connect_deadline_ms = 0;
    peer_mark_seen(node_idx, instance_id);
    if (peer_update_epoll(epoll_fd, node_idx) != 0)
        return -1;

    lcs_log_debug("peer %s persistent connection established direction=outbound", g_state.cfg.nodes[node_idx].name);
    return 0;
}

int peer_rpc_async(int epoll_fd, int node_idx, uint16_t req_type,
                   const void *req_payload, uint32_t req_len,
                   uint16_t expected_type, unsigned char *resp_payload,
                   size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms,
                   peer_rpc_callback_t callback, void *callback_ctx)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED)
    {
        lcs_log_debug("peer %s persistent request type=%u unavailable: fd=%d state=%d inflight=%zu",
                      g_state.cfg.nodes[node_idx].name, req_type, peer->fd, peer->conn_state,
                      peer_inflight_count(peer));
        return -1;
    }
    uint64_t deadline_ms = lcs_now_ms() + timeout_ms;
    peer_rpc_runtime_t *rpc = peer_register_rpc(peer, node_idx, req_type, expected_type,
                                                deadline_ms, resp_payload, resp_cap,
                                                resp_len, callback, callback_ctx);
    if (!rpc)
    {
        lcs_log_debug("peer %s persistent request type=%u unavailable: in-flight table full", g_state.cfg.nodes[node_idx].name, req_type);
        return -1;
    }

    lcs_log_debug3("peer %s registered RPC request type=%u expected=%u seq=%u deadline_ms=%llu",
                   g_state.cfg.nodes[node_idx].name, req_type, expected_type, rpc->seq,
                   (unsigned long long)deadline_ms);
    if (peer_queue_frame(epoll_fd, node_idx, req_type, rpc->seq, req_payload, req_len) != 0 || peer_flush_output(epoll_fd, node_idx) != 0)
    {
        lcs_log_debug("peer %s persistent request type=%u seq=%u queue/write failed", g_state.cfg.nodes[node_idx].name, req_type, rpc->seq);
        peer_close_connection(epoll_fd, node_idx, true, "request queue/write failed");
        peer_clear_rpc(rpc);
        return -1;
    }
    return 0;
}

static int peer_send_state_sync(int epoll_fd, int node_idx)
{
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = 0;
    if (cluster_encode_state(payload, sizeof(payload), &len) != 0)
        return -1;
    return peer_queue_frame(epoll_fd, node_idx, LCS_MSG_STATE_SYNC_REQ, lcs_next_seq(), payload, (uint32_t)len);
}

static int peer_process_frame(int epoll_fd, int node_idx, const lcs_frame_header_t *hdr, unsigned char *payload)
{
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count || g_state.peers[node_idx].fd < 0)
        return -1;

    lcs_log_debug2("peer %s frame type=%u seq=%u length=%u", g_state.cfg.nodes[node_idx].name, hdr->type, hdr->seq, hdr->length);
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer->conn_state == LCS_PEER_HELLO_SENT)
        return peer_complete_outbound_hello(epoll_fd, node_idx, hdr, payload);
    if (peer->conn_state != LCS_PEER_ESTABLISHED)
    {
        lcs_log_debug3("peer %s frame ignored in connection state=%d", g_state.cfg.nodes[node_idx].name, peer->conn_state);
        return -1;
    }
    peer_mark_seen(node_idx, g_state.peers[node_idx].instance_id);
    if (peer_is_request_type(hdr->type))
        return peer_handle_request_frame(epoll_fd, node_idx, hdr, payload);

    peer_rpc_runtime_t *rpc = peer_find_rpc(peer, hdr->seq, hdr->type);
    if (rpc)
    {
        uint32_t seq = hdr->seq;
        uint16_t type = hdr->type;
        size_t resp_cap = rpc->resp_cap;
        if (hdr->length > resp_cap)
        {
            peer_complete_rpc(rpc, -1);
            lcs_log_warn("peer %s invalid RPC response: type=%u seq=%u length=%u buffer=%zu",
                         g_state.cfg.nodes[node_idx].name, hdr->type, hdr->seq, hdr->length,
                         resp_cap);
            return -1;
        }
        if (hdr->length)
            memcpy(rpc->resp_payload, payload, hdr->length);
        if (rpc->resp_len)
            *rpc->resp_len = hdr->length;
        peer_complete_rpc(rpc, 0);
        lcs_log_debug3("peer %s completed RPC seq=%u type=%u", g_state.cfg.nodes[node_idx].name, seq, type);
        return 0;
    }
    peer_rpc_runtime_t *same_seq_rpc = peer_find_rpc_by_seq(peer, hdr->seq);
    if (same_seq_rpc)
    {
        uint16_t expected_type = same_seq_rpc->expected_type;
        peer_complete_rpc(same_seq_rpc, -1);
        lcs_log_warn("peer %s invalid RPC response: type=%u seq=%u expected_type=%u",
                     g_state.cfg.nodes[node_idx].name, hdr->type, hdr->seq,
                     expected_type);
        return -1;
    }
    if (hdr->type == LCS_MSG_STATE_SYNC_RESP)
        return cluster_apply_state(payload, hdr->length, node_idx);

    lcs_log_warn("peer %s unexpected response frame: type=%u seq=%u length=%u inflight=%zu",
                 g_state.cfg.nodes[node_idx].name, hdr->type, hdr->seq, hdr->length,
                 peer_inflight_count(peer));
    return -1;
}

static int peer_parse_frames(int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &g_state.peers[node_idx];
    size_t off = 0;
    while (peer->in_len - off >= LCS_FRAME_HEADER_SIZE)
    {
        lcs_frame_header_t wire;
        memcpy(&wire, peer->inbuf + off, sizeof(wire));
        lcs_frame_header_t hdr;
        hdr.magic = ntohl(wire.magic);
        hdr.type = ntohs(wire.type);
        hdr.flags = ntohs(wire.flags);
        hdr.length = ntohl(wire.length);
        hdr.seq = ntohl(wire.seq);
        if (hdr.magic != LCS_PROTO_MAGIC || hdr.flags != 0 || hdr.length > LCS_MAX_FRAME)
        {
            lcs_log_debug3("peer %s invalid frame header magic=0x%08x flags=0x%04x length=%u",
                           g_state.cfg.nodes[node_idx].name, hdr.magic, hdr.flags, hdr.length);
            return -1;
        }
        size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)hdr.length;
        if (peer->in_len - off < frame_len)
            break;
        if (peer_process_frame(epoll_fd, node_idx, &hdr, peer->inbuf + off + LCS_FRAME_HEADER_SIZE) != 0)
            return -1;
        off += frame_len;
    }
    if (off)
    {
        size_t remaining = peer->in_len - off;
        if (remaining)
            memmove(peer->inbuf, peer->inbuf + off, remaining);
        peer->in_len = remaining;
    }
    return 0;
}

static void peer_handle_event(int epoll_fd, int node_idx)
{
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count ||
        g_state.peers[node_idx].fd < 0)
        return;
    peer_runtime_t *peer = &g_state.peers[node_idx];
    for (;;)
    {
        if (peer->in_len == LCS_PEER_INBUF_SIZE)
        {
            peer_close_connection(epoll_fd, node_idx, true, "input buffer full");
            return;
        }
        ssize_t n = read(peer->fd, peer->inbuf + peer->in_len, LCS_PEER_INBUF_SIZE - peer->in_len);
        if (n > 0)
        {
            peer->in_len += (size_t)n;
            lcs_log_debug3("peer %s read %zd bytes buffered=%zu", g_state.cfg.nodes[node_idx].name, n, peer->in_len);
            if (peer_parse_frames(epoll_fd, node_idx) != 0)
            {
                peer_close_connection(epoll_fd, node_idx, true, "protocol error");
                return;
            }
            continue;
        }
        if (n == 0)
        {
            peer_close_connection(epoll_fd, node_idx, true, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        char reason[128];
        snprintf(reason, sizeof(reason), "connection read failed: %s", strerror(errno));
        peer_close_connection(epoll_fd, node_idx, true, reason);
        return;
    }
}

static uint32_t peer_handshake_timeout_ms(void)
{
    return g_state.cfg.peer_timeout_ms < LCS_DEFAULT_HANDSHAKE_TIMEOUT_MS ?
           g_state.cfg.peer_timeout_ms : LCS_DEFAULT_HANDSHAKE_TIMEOUT_MS;
}

void peer_pump_epoll_event(int epoll_fd, const struct epoll_event *ev)
{
    uint32_t event_id = ev->data.u32;
    int handshake_idx = handshake_index_from_epoll_id(event_id);
    if (handshake_idx >= 0)
    {
        handshake_handle_event(epoll_fd, handshake_idx, ev->events);
        return;
    }
    int peer_idx = peer_index_from_epoll_id(event_id);
    if (peer_idx >= 0)
    {
        peer_runtime_t *peer = &g_state.peers[peer_idx];
        if (peer->conn_state == LCS_PEER_CONNECTING && (ev->events & EPOLLOUT))
        {
            if (peer_handle_connect_ready(epoll_fd, peer_idx) != 0)
                peer_close_connection(epoll_fd, peer_idx, true, "connect failed");
            return;
        }
        if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
        {
            lcs_log_debug("peer %s epoll close event mask=0x%x", g_state.cfg.nodes[peer_idx].name, ev->events);
            peer_close_connection(epoll_fd, peer_idx, true, "connection closed");
        } else
        {
            if ((ev->events & EPOLLOUT) && peer_flush_output(epoll_fd, peer_idx) != 0)
            {
                peer_close_connection(epoll_fd, peer_idx, true, "connection write failed");
                return;
            }
            if (ev->events & EPOLLIN)
                peer_handle_event(epoll_fd, peer_idx);
        }
        return;
    }
    if (event_id == LCS_EPOLL_PEER && (ev->events & EPOLLIN) && g_peer_listener_fd >= 0)
    {
        for (;;)
        {
            int peer_fd = accept4(g_peer_listener_fd, NULL, NULL, SOCK_CLOEXEC);
            if (peer_fd < 0)
            {
                if (errno != EAGAIN && errno != EWOULDBLOCK)
                    lcs_log_debug("peer accept failed: %s", strerror(errno));
                break;
            }
            if (lcs_set_fd_nonblocking(peer_fd) != 0)
            {
                close(peer_fd);
                continue;
            }
            int slot_idx = -1;
            for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
            {
                if (!g_state.handshakes[i].active)
                {
                    slot_idx = (int)i;
                    break;
                }
            }
            if (slot_idx < 0)
            {
                lcs_log_warn("rejecting inbound peer connection: handshake table full");
                close(peer_fd);
                continue;
            }
            inbound_handshake_t *hs = &g_state.handshakes[slot_idx];
            memset(hs, 0, sizeof(*hs));
            if (handshake_ensure_buffers(hs) != 0)
            {
                lcs_log_warn("rejecting inbound peer connection: failed to allocate handshake buffers");
                close(peer_fd);
                memset(hs, 0, sizeof(*hs));
                hs->fd = -1;
                hs->node_idx = -1;
                continue;
            }
            hs->active = true;
            hs->fd = peer_fd;
            hs->node_idx = -1;
            hs->deadline_ms = lcs_now_ms() + peer_handshake_timeout_ms();
            if (lcs_add_epoll_fd_events(epoll_fd, peer_fd, handshake_epoll_id(slot_idx), EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
            {
                handshake_close(epoll_fd, slot_idx, "epoll add failed");
                continue;
            }
            lcs_log_debug3("accepted inbound peer handshake slot=%d fd=%d deadline_ms=%llu", slot_idx, peer_fd, (unsigned long long)hs->deadline_ms);
            cluster_recompute_votes();
        }
    }
}

static int peer_connect(int epoll_fd, int node_idx)
{
    if (g_state.self_index >= node_idx || g_state.peers[node_idx].fd >= 0)
        return 0;
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%u", g_state.cfg.nodes[node_idx].port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(g_state.cfg.nodes[node_idx].address, port_buf, &hints, &res) != 0)
        return -1;
    int fd = -1;
    int saved_errno = ECONNREFUSED;
    for (struct addrinfo *ai = res; ai; ai = ai->ai_next)
    {
        fd = socket(ai->ai_family, ai->ai_socktype | SOCK_CLOEXEC, ai->ai_protocol);
        if (fd < 0)
        {
            saved_errno = errno;
            continue;
        }
        if (lcs_set_fd_nonblocking(fd) != 0)
        {
            saved_errno = errno;
            close(fd);
            fd = -1;
            continue;
        }
        int rc = connect(fd, ai->ai_addr, ai->ai_addrlen);
        if (rc == 0 || errno == EINPROGRESS)
            break;
        saved_errno = errno;
        close(fd);
        fd = -1;
    }
    freeaddrinfo(res);
    if (fd < 0)
    {
        errno = saved_errno;
        return -1;
    }
    peer_runtime_t *peer = &g_state.peers[node_idx];
    if (peer_ensure_buffers(peer) != 0)
    {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    peer->fd = fd;
    peer->conn_state = LCS_PEER_CONNECTING;
    peer->outbound = true;
    peer->connect_deadline_ms = lcs_now_ms() + peer_handshake_timeout_ms();
    peer->in_len = 0;
    peer->out_off = 0;
    peer->out_len = 0;
    if (lcs_add_epoll_fd_events(epoll_fd, fd, peer_epoll_id(node_idx), EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
    {
        close(fd);
        peer->fd = -1;
        peer_free_buffers(peer);
        peer->conn_state = LCS_PEER_DISCONNECTED;
        return -1;
    }
    lcs_log_debug3("peer %s outbound connect started fd=%d deadline_ms=%llu",
                   g_state.cfg.nodes[node_idx].name, fd,
                   (unsigned long long)peer->connect_deadline_ms);
    return 0;
}

void peer_broadcast_state_sync(int epoll_fd)
{
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index || g_state.peers[i].fd < 0)
            continue;

        if (peer_send_state_sync(epoll_fd, (int)i) != 0)
            lcs_log_debug("state sync broadcast to %s failed", g_state.cfg.nodes[i].name);
    }
}

void peer_poll(int epoll_fd)
{
    uint64_t now = lcs_now_ms();

    // Check state of each peer defined in config and sync state via timer
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        if ((int)i == g_state.self_index)
            continue;

        // If the peer is disconnected, try to reconnect, but only if we are supposed to initiate the connection
        // to prevent double sided connections, only peer with index < target starts it
        if (g_state.peers[i].fd < 0 && g_state.self_index < (int)i && (!g_state.peers[i].next_sync_ms || now >= g_state.peers[i].next_sync_ms))
        {
            if (peer_connect(epoll_fd, (int)i) != 0)
            {
                peer_mark_sync_failed((int)i);
                continue;
            }
        }

        // Skip disconnected peers
        if (g_state.peers[i].fd < 0)
            continue;

        if (g_state.peers[i].conn_state != LCS_PEER_ESTABLISHED)
        {
            // Connection is open, but handshake didn't finish, enforce timeout
            if (g_state.peers[i].connect_deadline_ms && now >= g_state.peers[i].connect_deadline_ms)
            {
                lcs_log_debug3("peer %s setup timed out state=%d", g_state.cfg.nodes[i].name, g_state.peers[i].conn_state);
                peer_close_connection(epoll_fd, (int)i, true, "connect/hello timeout");
            }
            continue;
        }

        for (size_t j = 0; j < LCS_MAX_PEER_RPC_INFLIGHT; j++)
        {
            // Enforce timeout on enqued RPC calls
            peer_rpc_runtime_t *rpc = &g_state.peers[i].in_flight[j];
            if (rpc->active && !rpc->done && rpc->deadline_ms && now >= rpc->deadline_ms)
            {
                lcs_log_debug3("peer %s RPC seq=%u type=%u timed out waiting for type=%u",
                               g_state.cfg.nodes[i].name, rpc->seq, rpc->req_type,
                               rpc->expected_type);
                peer_complete_rpc(rpc, -1);
            }
        }

        // Periodic state sync frames
        if (g_state.peers[i].next_sync_ms && now < g_state.peers[i].next_sync_ms)
            continue;

        if (peer_send_state_sync(epoll_fd, (int)i) != 0)
            peer_close_connection(epoll_fd, (int)i, true, "state sync failed");
        else
            g_state.peers[i].next_sync_ms = now + 1000;
    }
    cluster_recompute_votes();
}

void handshake_expire(int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
    {
        if (g_state.handshakes[i].active && g_state.handshakes[i].deadline_ms && now >= g_state.handshakes[i].deadline_ms)
            handshake_close(epoll_fd, (int)i, "handshake timeout");
    }
}
