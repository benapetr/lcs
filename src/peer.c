// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "peer.h"

#include "cluster.h"
#include "epoll_util.h"
#include "lease.h"
#include "local_client.h"
#include "log.h"
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
static int update_peer_epoll(daemon_state_t *st, int epoll_fd, int node_idx);
static int queue_peer_frame(daemon_state_t *st, int epoll_fd, int node_idx,
                            uint16_t type, uint32_t seq, const void *payload,
                            uint32_t length);
static int flush_peer_output(daemon_state_t *st, int epoll_fd, int node_idx);
static int queue_outbound_hello(daemon_state_t *st, int epoll_fd, int node_idx);
static void handle_persistent_peer_event(daemon_state_t *st, int epoll_fd, int node_idx);
static int register_peer_connection(daemon_state_t *st, int epoll_fd, int node_idx,
                                    int fd, bool outbound);
static void mark_peer_seen(daemon_state_t *st, int node_idx, uint64_t instance_id);
static int validate_peer_instance_for_hello(const daemon_state_t *st, int node_idx,
                                            uint64_t instance_id);
static int update_handshake_epoll(daemon_state_t *st, int epoll_fd, int slot_idx);
static int flush_handshake_output(daemon_state_t *st, int epoll_fd, int slot_idx);
static void mark_peer_sync_failed(daemon_state_t *st, int node_idx);
static int send_persistent_state_sync(daemon_state_t *st, int epoll_fd, int node_idx);
static void pump_peer_events_until_pending(daemon_state_t *st, int epoll_fd,
                                           int node_idx, uint64_t deadline_ms);
static void free_peer_buffers(peer_runtime_t *peer);
static void free_handshake_buffers(inbound_handshake_t *hs);

static int ensure_peer_buffers(peer_runtime_t *peer)
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
            free_peer_buffers(peer);
            return -1;
        }
    }
    return 0;
}

static void free_peer_buffers(peer_runtime_t *peer)
{
    free(peer->inbuf);
    free(peer->outbuf);
    peer->inbuf = NULL;
    peer->outbuf = NULL;
}

static int ensure_handshake_buffers(inbound_handshake_t *hs)
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
            free_handshake_buffers(hs);
            return -1;
        }
    }
    return 0;
}

static void free_handshake_buffers(inbound_handshake_t *hs)
{
    free(hs->inbuf);
    free(hs->outbuf);
    hs->inbuf = NULL;
    hs->outbuf = NULL;
}

static bool is_peer_request_type(uint16_t type)
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

static int update_peer_epoll(daemon_state_t *st, int epoll_fd, int node_idx)
{
    if (epoll_fd < 0)
        return 0;
    if (node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return -1;
    peer_runtime_t *peer = &st->peers[node_idx];
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

static int encode_hello(const daemon_state_t *st, unsigned char *payload, size_t cap,
                        size_t *len, uint8_t mode)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u16(&w, LCS_PROTO_VERSION) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)st->self_index) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)st->cfg.node_count) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)st->cfg.vip_count) != 0 ||
        lcs_buf_put_u16(&w, (uint16_t)st->cfg.nodes[st->self_index].role) != 0 ||
        lcs_buf_put_u8(&w, mode) != 0 ||
        lcs_buf_put_u64(&w, st->instance_id) != 0 ||
        lcs_buf_put_fixed_string(&w, st->cfg.nodes[st->self_index].name, LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_put_fixed_string(&w, st->cfg.cluster_name, LCS_NAME_MAX + 1) != 0 ||
        lcs_buf_put_fixed_string(&w, st->cfg.secret, LCS_NAME_MAX + 1) != 0)
        return -1;
    *len = w.len;
    return 0;
}

static int decode_hello(const daemon_state_t *st, const void *payload, size_t len,
                        int *node_idx, uint64_t *instance_id, uint8_t *mode)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint16_t proto_version, remote_idx, node_count, vip_count, role;
    char name[LCS_NAME_MAX + 1];
    char cluster_name[LCS_NAME_MAX + 1];
    char secret[LCS_NAME_MAX + 1];
    if (lcs_buf_get_u16(&r, &proto_version) != 0 ||
        lcs_buf_get_u16(&r, &remote_idx) != 0 ||
        lcs_buf_get_u16(&r, &node_count) != 0 ||
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
        lcs_log_debug("rejecting HELLO with protocol version %u, expected %u",
                      proto_version, LCS_PROTO_VERSION);
        return -1;
    }
    int idx = lcs_config_node_index(&st->cfg, name);
    if (idx < 0 || idx != (int)remote_idx ||
        node_count != st->cfg.node_count || vip_count != st->cfg.vip_count ||
        role != (uint16_t)st->cfg.nodes[idx].role)
        return -1;
    if (strcmp(cluster_name, st->cfg.cluster_name) != 0)
        return -1;
    if (*st->cfg.secret && strcmp(secret, st->cfg.secret) != 0)
        return -1;
    if (*mode != LCS_HELLO_MODE_PERSISTENT)
        return -1;
    *node_idx = idx;
    return 0;
}

static int queue_raw_frame(unsigned char *outbuf, size_t outbuf_cap,
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

static int queue_peer_frame(daemon_state_t *st, int epoll_fd, int node_idx,
                            uint16_t type, uint32_t seq, const void *payload,
                            uint32_t length)
{
    if (node_idx < 0 || (size_t)node_idx >= st->cfg.node_count || length > LCS_MAX_FRAME)
        return -1;
    peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->fd < 0 || !peer->outbuf)
        return -1;
    size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)length;
    size_t queued = peer->out_len - peer->out_off;
    if (frame_len > LCS_PEER_OUTBUF_SIZE - queued)
    {
        lcs_log_debug3("peer %s output queue full while queuing type=%u seq=%u queued=%zu need=%zu",
                       st->cfg.nodes[node_idx].name, type, seq, queued, frame_len);
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
                   st->cfg.nodes[node_idx].name, type, seq, length,
                   peer->out_len - peer->out_off);
    return update_peer_epoll(st, epoll_fd, node_idx);
}

static int queue_simple_peer_resp(daemon_state_t *st, int epoll_fd, int node_idx,
                                  uint32_t seq, uint16_t type, int32_t status,
                                  const char *msg)
{
    unsigned char payload[256];
    size_t len = 0;
    if (lcs_encode_simple_resp(payload, sizeof(payload), &len, status, msg) != 0)
        return -1;
    return queue_peer_frame(st, epoll_fd, node_idx, type, seq, payload, (uint32_t)len);
}

static int flush_peer_output(daemon_state_t *st, int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &st->peers[node_idx];
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
            return update_peer_epoll(st, epoll_fd, node_idx);
        lcs_log_debug3("peer %s write failed: %s", st->cfg.nodes[node_idx].name,
                       n == 0 ? "short write" : strerror(errno));
        return -1;
    }
    peer->out_off = 0;
    peer->out_len = 0;
    return update_peer_epoll(st, epoll_fd, node_idx);
}

static int handle_peer_connect_ready(daemon_state_t *st, int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &st->peers[node_idx];
    int so_error = 0;
    socklen_t len = sizeof(so_error);
    if (getsockopt(peer->fd, SOL_SOCKET, SO_ERROR, &so_error, &len) != 0 || so_error != 0)
    {
        errno = so_error ? so_error : errno;
        lcs_log_debug3("peer %s outbound connect failed: %s",
                       st->cfg.nodes[node_idx].name, strerror(errno));
        return -1;
    }
    lcs_log_debug3("peer %s outbound connect completed", st->cfg.nodes[node_idx].name);
    return queue_outbound_hello(st, epoll_fd, node_idx);
}

static int update_handshake_epoll(daemon_state_t *st, int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
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

void close_handshake(daemon_state_t *st, int epoll_fd, int slot_idx, const char *reason)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
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
    free_handshake_buffers(hs);
    memset(hs, 0, sizeof(*hs));
    hs->fd = -1;
    hs->node_idx = -1;
}

static int promote_handshake(daemon_state_t *st, int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
    int fd = hs->fd;
    int node_idx = hs->node_idx;
    uint64_t instance_id = hs->instance_id;
    if (fd < 0 || node_idx < 0)
        return -1;
    epoll_ctl(epoll_fd, EPOLL_CTL_DEL, fd, NULL);
    hs->active = false;
    hs->fd = -1;
    mark_peer_seen(st, node_idx, instance_id);
    free_handshake_buffers(hs);
    hs->in_len = 0;
    hs->out_off = 0;
    hs->out_len = 0;
    if (register_peer_connection(st, epoll_fd, node_idx, fd, false) != 0)
    {
        close(fd);
        return -1;
    }
    return 0;
}

static int flush_handshake_output(daemon_state_t *st, int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
    while (hs->out_off < hs->out_len)
    {
        ssize_t n = write(hs->fd, hs->outbuf + hs->out_off, hs->out_len - hs->out_off);
        if (n > 0)
        {
            hs->out_off += (size_t)n;
            continue;
        }
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return update_handshake_epoll(st, epoll_fd, slot_idx);
        return -1;
    }
    hs->out_off = 0;
    hs->out_len = 0;
    return promote_handshake(st, epoll_fd, slot_idx);
}

static int process_handshake_frame(daemon_state_t *st, int epoll_fd, int slot_idx,
                                   const lcs_frame_header_t *hdr,
                                   const unsigned char *payload)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
    if (hdr->type != LCS_MSG_HELLO)
    {
        lcs_log_warn("peer client sent message type %u before HELLO", hdr->type);
        return -1;
    }
    int node_idx = -1;
    uint64_t instance_id = 0;
    uint8_t mode = 0;
    if (decode_hello(st, payload, hdr->length, &node_idx, &instance_id, &mode) != 0)
    {
        lcs_log_debug("invalid inbound peer HELLO");
        return -1;
    }
    if (node_idx == st->self_index)
    {
        lcs_log_debug("rejecting persistent connection from self");
        return -1;
    }
    if (node_idx > st->self_index)
    {
        lcs_log_debug("rejecting inbound persistent connection from %s; lower node owns outbound side",
                      st->cfg.nodes[node_idx].name);
        return -1;
    }
    if (validate_peer_instance_for_hello(st, node_idx, instance_id) != 0)
    {
        lcs_log_debug("rejecting stale inbound HELLO from %s", st->cfg.nodes[node_idx].name);
        return -1;
    }
    unsigned char resp[LCS_MAX_FRAME];
    size_t resp_len = 0;
    if (encode_hello(st, resp, sizeof(resp), &resp_len, mode) != 0 ||
        queue_raw_frame(hs->outbuf, LCS_PEER_INBUF_SIZE, &hs->out_off, &hs->out_len,
                        LCS_MSG_HELLO_ACK, hdr->seq, resp, (uint32_t)resp_len) != 0)
        return -1;
    hs->node_idx = node_idx;
    hs->instance_id = instance_id;
    lcs_log_debug3("inbound HELLO accepted from %s slot=%d seq=%u",
                   st->cfg.nodes[node_idx].name, slot_idx, hdr->seq);
    return flush_handshake_output(st, epoll_fd, slot_idx);
}

static int parse_handshake_frames(daemon_state_t *st, int epoll_fd, int slot_idx)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
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
    return process_handshake_frame(st, epoll_fd, slot_idx, &hdr,
                                   hs->inbuf + LCS_FRAME_HEADER_SIZE);
}

static void handle_handshake_event(daemon_state_t *st, int epoll_fd, int slot_idx,
                                   uint32_t events)
{
    inbound_handshake_t *hs = &st->handshakes[slot_idx];
    if (!hs->active)
        return;
    if (events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
    {
        close_handshake(st, epoll_fd, slot_idx, "connection closed");
        return;
    }
    if ((events & EPOLLOUT) && flush_handshake_output(st, epoll_fd, slot_idx) != 0)
    {
        close_handshake(st, epoll_fd, slot_idx, "write failed");
        return;
    }
    if (!(events & EPOLLIN) || !hs->active)
        return;
    for (;;)
    {
        if (hs->in_len == LCS_PEER_INBUF_SIZE)
        {
            close_handshake(st, epoll_fd, slot_idx, "input buffer full");
            return;
        }
        ssize_t n = read(hs->fd, hs->inbuf + hs->in_len, LCS_PEER_INBUF_SIZE - hs->in_len);
        if (n > 0)
        {
            hs->in_len += (size_t)n;
            if (parse_handshake_frames(st, epoll_fd, slot_idx) != 0)
            {
                close_handshake(st, epoll_fd, slot_idx, "invalid handshake");
                return;
            }
            if (!hs->active || hs->out_len > hs->out_off)
                return;
            continue;
        }
        if (n == 0)
        {
            close_handshake(st, epoll_fd, slot_idx, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        close_handshake(st, epoll_fd, slot_idx, "read failed");
        return;
    }
}

static const char *peer_error_message(const void *payload, size_t len, char *buf,
                                      size_t buf_len)
{
    int32_t status = -1;
    if (lcs_decode_simple_resp(payload, len, &status, buf, buf_len) == 0 && *buf)
        return buf;
    snprintf(buf, buf_len, "undecodable error payload length=%zu", len);
    return buf;
}

static void reset_peer_sequence_cache(peer_runtime_t *peer)
{
    memset(peer->seen_request_seqs, 0, sizeof(peer->seen_request_seqs));
    peer->seen_request_pos = 0;
}

static int remember_peer_request_sequence(daemon_state_t *st, int source_node_idx,
                                          const lcs_frame_header_t *hdr)
{
    if (source_node_idx < 0 || (size_t)source_node_idx >= st->cfg.node_count ||
        !is_peer_request_type(hdr->type))
        return 0;
    peer_runtime_t *peer = &st->peers[source_node_idx];
    for (size_t i = 0; i < LCS_SEQ_CACHE_SIZE; i++)
    {
        if (peer->seen_request_seqs[i] == hdr->seq)
        {
            lcs_log_debug("rejecting duplicate peer request from %s seq=%u type=%u",
                          st->cfg.nodes[source_node_idx].name, hdr->seq, hdr->type);
            return -1;
        }
    }
    peer->seen_request_seqs[peer->seen_request_pos] = hdr->seq;
    peer->seen_request_pos = (peer->seen_request_pos + 1) % LCS_SEQ_CACHE_SIZE;
    return 0;
}

static int handle_peer_request_frame(daemon_state_t *st, int epoll_fd, int source_node_idx,
                                     const lcs_frame_header_t *hdr,
                                     unsigned char *payload)
{
    size_t len = 0;
    if (hdr->seq == 0)
    {
        queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR,
                               -1, "invalid sequence");
        return -1;
    }
    if (remember_peer_request_sequence(st, source_node_idx, hdr) != 0)
    {
        queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR,
                               -1, "duplicate request sequence");
        return -1;
    }
    switch (hdr->type)
    {
        case LCS_MSG_STATE_SYNC_REQ:
            if (hdr->length && apply_state(st, payload, hdr->length, source_node_idx) != 0)
            {
                queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR,
                                       -1, "failed to apply state");
                return -1;
            }
            if (encode_state(st, payload, LCS_MAX_FRAME, &len) != 0)
            {
                queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR,
                                       -1, "failed to encode state");
                return -1;
            }
            return queue_peer_frame(st, epoll_fd, source_node_idx, LCS_MSG_STATE_SYNC_RESP,
                                    hdr->seq, payload, (uint32_t)len);
        case LCS_MSG_STATE_SYNC_RESP:
            return apply_state(st, payload, hdr->length, source_node_idx);
        case LCS_MSG_LEASE_REQ:
        case LCS_MSG_LEASE_RENEW:
        case LCS_MSG_LEASE_RELEASE:
            if (accept_lease_message(st, hdr->type, payload, hdr->length, source_node_idx) == 0)
                return queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq,
                                              LCS_MSG_LEASE_ACK, 0, "ok");
            return queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq,
                                          LCS_MSG_LEASE_ACK, -1, "lease rejected");
        case LCS_MSG_OWNER_RELEASE_REQ:
            if (handle_owner_release_request(st, payload, hdr->length, source_node_idx) == 0)
                return queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq,
                                              LCS_MSG_OWNER_RELEASE_RESP, 0, "ok");
            return queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq,
                                          LCS_MSG_OWNER_RELEASE_RESP, -1, "owner release rejected");
        case LCS_MSG_MOVE_REQ:
        {
            int32_t status = -1;
            char message[128] = "";
            if (compute_move_response(st, payload, hdr->length, epoll_fd, &status,
                                      message, sizeof(message)) != 0)
            {
                status = -1;
                snprintf(message, sizeof(message), "move request failed");
            }
            return queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq,
                                          LCS_MSG_MOVE_RESP, status, message);
        }
        default:
            queue_simple_peer_resp(st, epoll_fd, source_node_idx, hdr->seq, LCS_MSG_ERROR,
                                   -1, "unsupported peer message");
            return -1;
    }
}

static void mark_peer_seen(daemon_state_t *st, int node_idx, uint64_t instance_id)
{
    if (node_idx == st->self_index || node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return;
    bool was_online = st->peers[node_idx].online;
    if (st->peers[node_idx].instance_id != 0 &&
        st->peers[node_idx].instance_id != instance_id)
        reset_peer_sequence_cache(&st->peers[node_idx]);
    st->peers[node_idx].online = true;
    st->peers[node_idx].instance_id = instance_id;
    st->peers[node_idx].last_seen_ms = lcs_now_ms();
    st->peers[node_idx].next_sync_ms = st->peers[node_idx].last_seen_ms + 1000;
    st->peers[node_idx].backoff_ms = 0;
    if (!was_online)
        lcs_log_info("peer %s online instance=%llu", st->cfg.nodes[node_idx].name,
                     (unsigned long long)instance_id);
}

static int validate_peer_instance_for_hello(const daemon_state_t *st, int node_idx,
                                            uint64_t instance_id)
{
    if (node_idx == st->self_index || node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return -1;
    const peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->conn_state == LCS_PEER_ESTABLISHED &&
        peer->instance_id != 0 &&
        peer->instance_id != instance_id)
        return -1;
    return 0;
}

static void mark_peer_sync_failed(daemon_state_t *st, int node_idx)
{
    if (node_idx == st->self_index || node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return;
    peer_runtime_t *peer = &st->peers[node_idx];
    uint64_t now = lcs_now_ms();
    if (peer->online && now - peer->last_seen_ms > st->cfg.peer_timeout_ms)
    {
        lcs_log_info("peer %s offline", st->cfg.nodes[node_idx].name);
        peer->online = false;
    }
    uint32_t next_backoff = peer->backoff_ms ? peer->backoff_ms * 2u : 1000u;
    if (next_backoff > st->cfg.peer_timeout_ms)
        next_backoff = st->cfg.peer_timeout_ms;
    peer->backoff_ms = next_backoff;
    peer->next_sync_ms = now + next_backoff;
}

void close_peer_connection(daemon_state_t *st, int epoll_fd, int node_idx,
                           bool mark_offline, const char *reason)
{
    if (node_idx == st->self_index || node_idx < 0 || (size_t)node_idx >= st->cfg.node_count)
        return;
    peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->fd >= 0)
    {
        lcs_log_debug3("closing peer %s fd=%d mark_offline=%s reason=%s pending=%s out_bytes=%zu in_bytes=%zu",
                       st->cfg.nodes[node_idx].name, peer->fd,
                       mark_offline ? "true" : "false", reason ? reason : "-",
                       peer->pending_active ? "true" : "false",
                       peer->out_len > peer->out_off ? peer->out_len - peer->out_off : 0,
                       peer->in_len);
        epoll_ctl(epoll_fd, EPOLL_CTL_DEL, peer->fd, NULL);
        close(peer->fd);
        peer->fd = -1;
    }
    free_peer_buffers(peer);
    if (peer->pending_active)
    {
        peer->pending_status = -1;
        peer->pending_done = true;
        peer->pending_active = false;
    }
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
        lcs_log_info("peer %s offline%s%s", st->cfg.nodes[node_idx].name,
                     reason ? ": " : "", reason ? reason : "");
    }
    if (mark_offline)
        mark_peer_sync_failed(st, node_idx);
}

static int register_peer_connection(daemon_state_t *st, int epoll_fd, int node_idx,
                                    int fd, bool outbound)
{
    peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->fd >= 0)
        close_peer_connection(st, epoll_fd, node_idx, false, "duplicate session replaced");
    if (ensure_peer_buffers(peer) != 0 ||
        set_fd_nonblocking(fd) != 0 ||
        add_epoll_fd(epoll_fd, fd, peer_epoll_id(node_idx)) != 0)
    {
        free_peer_buffers(peer);
        return -1;
    }
    peer->fd = fd;
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->outbound = outbound;
    peer->pending_active = false;
    peer->pending_done = false;
    peer->pending_status = 0;
    peer->in_len = 0;
    peer->out_off = 0;
    peer->out_len = 0;
    peer->connect_deadline_ms = 0;
    peer->hello_seq = 0;
    peer->next_sync_ms = lcs_now_ms();
    lcs_log_debug("peer %s persistent connection established direction=%s",
                  st->cfg.nodes[node_idx].name, outbound ? "outbound" : "inbound");
    return 0;
}

static int queue_outbound_hello(daemon_state_t *st, int epoll_fd, int node_idx)
{
    unsigned char hello[LCS_MAX_FRAME];
    size_t hello_len = 0;
    peer_runtime_t *peer = &st->peers[node_idx];
    peer->hello_seq = lcs_next_seq();
    if (encode_hello(st, hello, sizeof(hello), &hello_len, LCS_HELLO_MODE_PERSISTENT) != 0 ||
        queue_peer_frame(st, epoll_fd, node_idx, LCS_MSG_HELLO, peer->hello_seq,
                         hello, (uint32_t)hello_len) != 0)
        return -1;
    peer->conn_state = LCS_PEER_HELLO_SENT;
    lcs_log_debug3("peer %s HELLO queued seq=%u", st->cfg.nodes[node_idx].name, peer->hello_seq);
    return 0;
}

static int complete_outbound_hello(daemon_state_t *st, int epoll_fd, int node_idx,
                                   const lcs_frame_header_t *hdr,
                                   const unsigned char *payload)
{
    peer_runtime_t *peer = &st->peers[node_idx];
    if (hdr->type != LCS_MSG_HELLO_ACK || hdr->seq != peer->hello_seq)
    {
        if (hdr->type == LCS_MSG_ERROR)
        {
            char msg[128];
            lcs_log_warn("persistent peer %s rejected HELLO: %s",
                         st->cfg.nodes[node_idx].name,
                         peer_error_message(payload, hdr->length, msg, sizeof(msg)));
        } else
        {
            lcs_log_debug("persistent peer %s invalid HELLO_ACK: type=%u seq=%u expected_type=%u expected_seq=%u",
                          st->cfg.nodes[node_idx].name, hdr->type, hdr->seq,
                          LCS_MSG_HELLO_ACK, peer->hello_seq);
        }
        return -1;
    }
    int remote_idx = -1;
    uint64_t instance_id = 0;
    uint8_t mode = 0;
    if (decode_hello(st, payload, hdr->length, &remote_idx, &instance_id, &mode) != 0 ||
        validate_peer_instance_for_hello(st, remote_idx, instance_id) != 0 ||
        mode != LCS_HELLO_MODE_PERSISTENT ||
        remote_idx != node_idx)
    {
        lcs_log_debug("persistent peer %s invalid HELLO_ACK payload",
                      st->cfg.nodes[node_idx].name);
        return -1;
    }
    peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->hello_seq = 0;
    peer->connect_deadline_ms = 0;
    mark_peer_seen(st, node_idx, instance_id);
    if (update_peer_epoll(st, epoll_fd, node_idx) != 0)
        return -1;
    lcs_log_debug("peer %s persistent connection established direction=outbound",
                  st->cfg.nodes[node_idx].name);
    return 0;
}

static void clear_pending_request(peer_runtime_t *peer)
{
    peer->pending_active = false;
    peer->pending_done = false;
    peer->pending_status = 0;
    peer->pending_seq = 0;
    peer->pending_expected_type = 0;
    peer->pending_resp_payload = NULL;
    peer->pending_resp_cap = 0;
    peer->pending_resp_len = NULL;
}

static void pump_peer_events_until_pending(daemon_state_t *st, int epoll_fd,
                                           int node_idx, uint64_t deadline_ms)
{
    peer_runtime_t *peer = &st->peers[node_idx];
    while (!peer->pending_done && !g_stop)
    {
        uint64_t now = lcs_now_ms();
        if (now >= deadline_ms)
        {
            peer->pending_status = -1;
            peer->pending_done = true;
            break;
        }
        int timeout_ms = (int)(deadline_ms - now);
        if (timeout_ms > 100)
            timeout_ms = 100;
        struct epoll_event events[32];
        int rc = epoll_wait(epoll_fd, events, 32, timeout_ms);
        if (rc < 0)
        {
            if (errno == EINTR)
                continue;
            peer->pending_status = -1;
            peer->pending_done = true;
            break;
        }
        for (int i = 0; i < rc && !peer->pending_done; i++)
            pump_peer_epoll_event(st, epoll_fd, &events[i]);
        poll_peers(st, epoll_fd);
        recompute_votes(st);
    }
}

static int peer_persistent_request(daemon_state_t *st, int epoll_fd, int node_idx,
                                   uint16_t req_type, const void *req_payload,
                                   uint32_t req_len, uint16_t expected_type,
                                   unsigned char *resp_payload, size_t resp_cap,
                                   uint32_t *resp_len, uint32_t timeout_ms)
{
    peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->fd < 0 || peer->conn_state != LCS_PEER_ESTABLISHED || peer->pending_active)
    {
        lcs_log_debug("peer %s persistent request type=%u unavailable: fd=%d state=%d pending=%s",
                      st->cfg.nodes[node_idx].name, req_type, peer->fd, peer->conn_state,
                      peer->pending_active ? "true" : "false");
        return -1;
    }
    clear_pending_request(peer);
    peer->pending_active = true;
    peer->pending_seq = lcs_next_seq();
    peer->pending_expected_type = expected_type;
    peer->pending_resp_payload = resp_payload;
    peer->pending_resp_cap = resp_cap;
    peer->pending_resp_len = resp_len;
    if (resp_len)
        *resp_len = 0;
    if (queue_peer_frame(st, epoll_fd, node_idx, req_type, peer->pending_seq,
                         req_payload, req_len) != 0 ||
        flush_peer_output(st, epoll_fd, node_idx) != 0)
    {
        lcs_log_debug("peer %s persistent request type=%u seq=%u queue/write failed",
                      st->cfg.nodes[node_idx].name, req_type, peer->pending_seq);
        close_peer_connection(st, epoll_fd, node_idx, true, "request queue/write failed");
        clear_pending_request(peer);
        return -1;
    }
    uint64_t deadline_ms = lcs_now_ms() + timeout_ms;
    pump_peer_events_until_pending(st, epoll_fd, node_idx, deadline_ms);
    int rc = peer->pending_status == 0 ? 0 : -1;
    if (rc != 0)
    {
        lcs_log_debug("peer %s persistent request type=%u seq=%u failed status=%d done=%s",
                      st->cfg.nodes[node_idx].name, req_type, peer->pending_seq,
                      peer->pending_status, peer->pending_done ? "true" : "false");
    }
    clear_pending_request(peer);
    return rc;
}

int peer_rpc(daemon_state_t *st, int epoll_fd, int node_idx, uint16_t req_type,
             const void *req_payload, uint32_t req_len,
             uint16_t expected_type, unsigned char *resp_payload,
             size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms)
{
    return peer_persistent_request(st, epoll_fd, node_idx, req_type, req_payload,
                                   req_len, expected_type, resp_payload, resp_cap,
                                   resp_len, timeout_ms);
}

static int send_persistent_state_sync(daemon_state_t *st, int epoll_fd, int node_idx)
{
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = 0;
    if (encode_state(st, payload, sizeof(payload), &len) != 0)
        return -1;
    return queue_peer_frame(st, epoll_fd, node_idx, LCS_MSG_STATE_SYNC_REQ,
                            lcs_next_seq(), payload, (uint32_t)len);
}

static int process_persistent_peer_frame(daemon_state_t *st, int epoll_fd, int node_idx,
                                         const lcs_frame_header_t *hdr,
                                         unsigned char *payload)
{
    if (node_idx < 0 || (size_t)node_idx >= st->cfg.node_count ||
        st->peers[node_idx].fd < 0)
        return -1;
    lcs_log_debug2("peer %s frame type=%u seq=%u length=%u",
                   st->cfg.nodes[node_idx].name, hdr->type, hdr->seq, hdr->length);
    peer_runtime_t *peer = &st->peers[node_idx];
    if (peer->conn_state == LCS_PEER_HELLO_SENT)
        return complete_outbound_hello(st, epoll_fd, node_idx, hdr, payload);
    if (peer->conn_state != LCS_PEER_ESTABLISHED)
    {
        lcs_log_debug3("peer %s frame ignored in connection state=%d",
                       st->cfg.nodes[node_idx].name, peer->conn_state);
        return -1;
    }
    mark_peer_seen(st, node_idx, st->peers[node_idx].instance_id);
    if (peer->pending_active && hdr->seq == peer->pending_seq)
    {
        if (hdr->type != peer->pending_expected_type || hdr->length > peer->pending_resp_cap)
        {
            peer->pending_status = -1;
            peer->pending_done = true;
            lcs_log_warn("peer %s invalid pending response: type=%u seq=%u length=%u expected_type=%u expected_seq=%u buffer=%zu",
                         st->cfg.nodes[node_idx].name, hdr->type, hdr->seq, hdr->length,
                         peer->pending_expected_type, peer->pending_seq,
                         peer->pending_resp_cap);
            return -1;
        }
        if (hdr->length)
            memcpy(peer->pending_resp_payload, payload, hdr->length);
        if (peer->pending_resp_len)
            *peer->pending_resp_len = hdr->length;
        peer->pending_status = 0;
        peer->pending_done = true;
        return 0;
    }
    return handle_peer_request_frame(st, epoll_fd, node_idx, hdr, payload);
}

static int parse_peer_frames(daemon_state_t *st, int epoll_fd, int node_idx)
{
    peer_runtime_t *peer = &st->peers[node_idx];
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
                           st->cfg.nodes[node_idx].name, hdr.magic, hdr.flags, hdr.length);
            return -1;
        }
        size_t frame_len = LCS_FRAME_HEADER_SIZE + (size_t)hdr.length;
        if (peer->in_len - off < frame_len)
            break;
        if (process_persistent_peer_frame(st, epoll_fd, node_idx, &hdr,
                                          peer->inbuf + off + LCS_FRAME_HEADER_SIZE) != 0)
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

static void handle_persistent_peer_event(daemon_state_t *st, int epoll_fd, int node_idx)
{
    if (node_idx < 0 || (size_t)node_idx >= st->cfg.node_count ||
        st->peers[node_idx].fd < 0)
        return;
    peer_runtime_t *peer = &st->peers[node_idx];
    for (;;)
    {
        if (peer->in_len == LCS_PEER_INBUF_SIZE)
        {
            close_peer_connection(st, epoll_fd, node_idx, true, "input buffer full");
            return;
        }
        ssize_t n = read(peer->fd, peer->inbuf + peer->in_len,
                         LCS_PEER_INBUF_SIZE - peer->in_len);
        if (n > 0)
        {
            peer->in_len += (size_t)n;
            lcs_log_debug3("peer %s read %zd bytes buffered=%zu",
                           st->cfg.nodes[node_idx].name, n, peer->in_len);
            if (parse_peer_frames(st, epoll_fd, node_idx) != 0)
            {
                close_peer_connection(st, epoll_fd, node_idx, true, "protocol error");
                return;
            }
            continue;
        }
        if (n == 0)
        {
            close_peer_connection(st, epoll_fd, node_idx, true, "connection closed");
            return;
        }
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            return;
        if (errno == EINTR)
            continue;
        char reason[128];
        snprintf(reason, sizeof(reason), "connection read failed: %s", strerror(errno));
        close_peer_connection(st, epoll_fd, node_idx, true, reason);
        return;
    }
}

void pump_peer_epoll_event(daemon_state_t *st, int epoll_fd, const struct epoll_event *ev)
{
    uint32_t event_id = ev->data.u32;
    int handshake_idx = handshake_index_from_epoll_id(event_id);
    if (handshake_idx >= 0)
    {
        handle_handshake_event(st, epoll_fd, handshake_idx, ev->events);
        return;
    }
    int peer_idx = peer_index_from_epoll_id(event_id);
    if (peer_idx >= 0)
    {
        peer_runtime_t *peer = &st->peers[peer_idx];
        if (peer->conn_state == LCS_PEER_CONNECTING && (ev->events & EPOLLOUT))
        {
            if (handle_peer_connect_ready(st, epoll_fd, peer_idx) != 0)
                close_peer_connection(st, epoll_fd, peer_idx, true, "connect failed");
            return;
        }
        if (ev->events & (EPOLLHUP | EPOLLRDHUP | EPOLLERR))
        {
            lcs_log_debug("peer %s epoll close event mask=0x%x",
                          st->cfg.nodes[peer_idx].name, ev->events);
            close_peer_connection(st, epoll_fd, peer_idx, true, "connection closed");
        } else
        {
            if ((ev->events & EPOLLOUT) &&
                flush_peer_output(st, epoll_fd, peer_idx) != 0)
            {
                close_peer_connection(st, epoll_fd, peer_idx, true,
                                      "connection write failed");
                return;
            }
            if (ev->events & EPOLLIN)
                handle_persistent_peer_event(st, epoll_fd, peer_idx);
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
            if (set_fd_nonblocking(peer_fd) != 0)
            {
                close(peer_fd);
                continue;
            }
            int slot_idx = -1;
            for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
            {
                if (!st->handshakes[i].active)
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
            inbound_handshake_t *hs = &st->handshakes[slot_idx];
            memset(hs, 0, sizeof(*hs));
            if (ensure_handshake_buffers(hs) != 0)
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
            hs->deadline_ms = lcs_now_ms() + peer_handshake_timeout_ms(st);
            if (add_epoll_fd_events(epoll_fd, peer_fd, handshake_epoll_id(slot_idx),
                                    EPOLLIN | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
            {
                close_handshake(st, epoll_fd, slot_idx, "epoll add failed");
                continue;
            }
            lcs_log_debug3("accepted inbound peer handshake slot=%d fd=%d deadline_ms=%llu",
                           slot_idx, peer_fd, (unsigned long long)hs->deadline_ms);
            recompute_votes(st);
        }
    }
}

static int connect_persistent_peer(daemon_state_t *st, int epoll_fd, int node_idx)
{
    if (st->self_index >= node_idx || st->peers[node_idx].fd >= 0)
        return 0;
    char port_buf[16];
    snprintf(port_buf, sizeof(port_buf), "%u", st->cfg.nodes[node_idx].port);
    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(st->cfg.nodes[node_idx].address, port_buf, &hints, &res) != 0)
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
        if (set_fd_nonblocking(fd) != 0)
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
    peer_runtime_t *peer = &st->peers[node_idx];
    if (ensure_peer_buffers(peer) != 0)
    {
        close(fd);
        errno = ENOMEM;
        return -1;
    }
    peer->fd = fd;
    peer->conn_state = LCS_PEER_CONNECTING;
    peer->outbound = true;
    peer->connect_deadline_ms = lcs_now_ms() + peer_handshake_timeout_ms(st);
    peer->in_len = 0;
    peer->out_off = 0;
    peer->out_len = 0;
    if (add_epoll_fd_events(epoll_fd, fd, peer_epoll_id(node_idx),
                            EPOLLIN | EPOLLOUT | EPOLLRDHUP | EPOLLERR | EPOLLHUP) != 0)
    {
        close(fd);
        peer->fd = -1;
        free_peer_buffers(peer);
        peer->conn_state = LCS_PEER_DISCONNECTED;
        return -1;
    }
    lcs_log_debug3("peer %s outbound connect started fd=%d deadline_ms=%llu",
                   st->cfg.nodes[node_idx].name, fd,
                   (unsigned long long)peer->connect_deadline_ms);
    return 0;
}

void broadcast_state_sync(daemon_state_t *st, int epoll_fd)
{
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index || st->peers[i].fd < 0)
            continue;
        if (send_persistent_state_sync(st, epoll_fd, (int)i) != 0)
            lcs_log_debug("state sync broadcast to %s failed", st->cfg.nodes[i].name);
    }
}

void poll_peers(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < st->cfg.node_count; i++)
    {
        if ((int)i == st->self_index)
            continue;
        if (st->peers[i].fd < 0 && st->self_index < (int)i &&
            (!st->peers[i].next_sync_ms || now >= st->peers[i].next_sync_ms))
        {
            if (connect_persistent_peer(st, epoll_fd, (int)i) != 0)
            {
                mark_peer_sync_failed(st, (int)i);
                continue;
            }
        }
        if (st->peers[i].fd < 0)
            continue;
        if (st->peers[i].conn_state != LCS_PEER_ESTABLISHED)
        {
            if (st->peers[i].connect_deadline_ms && now >= st->peers[i].connect_deadline_ms)
            {
                lcs_log_debug3("peer %s setup timed out state=%d",
                               st->cfg.nodes[i].name, st->peers[i].conn_state);
                close_peer_connection(st, epoll_fd, (int)i, true, "connect/hello timeout");
            }
            continue;
        }
        if (st->peers[i].next_sync_ms && now < st->peers[i].next_sync_ms)
            continue;
        if (send_persistent_state_sync(st, epoll_fd, (int)i) != 0)
            close_peer_connection(st, epoll_fd, (int)i, true, "state sync failed");
        else
            st->peers[i].next_sync_ms = now + 1000;
    }
    recompute_votes(st);
}

void expire_handshakes(daemon_state_t *st, int epoll_fd)
{
    uint64_t now = lcs_now_ms();
    for (size_t i = 0; i < LCS_HANDSHAKE_MAX; i++)
    {
        if (st->handshakes[i].active &&
            st->handshakes[i].deadline_ms &&
            now >= st->handshakes[i].deadline_ms)
            close_handshake(st, epoll_fd, (int)i, "handshake timeout");
    }
}
