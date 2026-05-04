// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "protocol.h"

#include <arpa/inet.h>
#include <errno.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <unistd.h>

static uint32_t g_seq = 1;
static char g_protocol_error[192] = "no protocol error";

static void set_protocol_error(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(g_protocol_error, sizeof(g_protocol_error), fmt, ap);
    va_end(ap);
}

const char *lcs_protocol_error(void)
{
    return g_protocol_error;
}

static uint64_t htonll_u64(uint64_t v)
{
#if __BYTE_ORDER__ == __ORDER_LITTLE_ENDIAN__
    return ((uint64_t)htonl((uint32_t)(v & 0xffffffffu)) << 32) | htonl((uint32_t)(v >> 32));
#else
    return v;
#endif
}

static uint64_t ntohll_u64(uint64_t v)
{
    return htonll_u64(v);
}

static ssize_t read_full(int fd, void *buf, size_t len)
{
    char *p = buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = read(fd, p + done, len - done);
        if (n == 0)
            return 0;

        if (n < 0)
            return -1;

        done += (size_t)n;
    }
    return (ssize_t)done;
}

static ssize_t write_full(int fd, const void *buf, size_t len)
{
    const char *p = buf;
    size_t done = 0;
    while (done < len)
    {
        ssize_t n = write(fd, p + done, len - done);
        if (n < 0)
            return -1;

        done += (size_t)n;
    }
    return (ssize_t)done;
}

int lcs_read_frame(int fd, lcs_frame_header_t *hdr, void *payload, size_t payload_cap)
{
    set_protocol_error("no protocol error");
    lcs_frame_header_t wire;
    ssize_t n = read_full(fd, &wire, sizeof(wire));
    if (n <= 0)
    {
        if (n == 0)
        {
            set_protocol_error("connection closed while reading frame header");
        } else
        {
            set_protocol_error("failed to read frame header: %s", strerror(errno));
        }
        return (int)n;
    }
    hdr->magic = ntohl(wire.magic);
    hdr->type = ntohs(wire.type);
    hdr->flags = ntohs(wire.flags);
    hdr->length = ntohl(wire.length);
    hdr->seq = ntohl(wire.seq);
    if (hdr->magic != LCS_PROTO_MAGIC)
    {
        set_protocol_error("bad frame magic 0x%08x, expected 0x%08x", hdr->magic, LCS_PROTO_MAGIC);
        return -1;
    }
    if (hdr->flags != 0)
    {
        set_protocol_error("unsupported frame flags: 0x%04x", hdr->flags);
        return -1;
    }
    if (hdr->length > LCS_MAX_FRAME)
    {
        set_protocol_error("frame payload too large: %u bytes, max %u", hdr->length, LCS_MAX_FRAME);
        return -1;
    }
    if (hdr->length > payload_cap)
    {
        set_protocol_error("frame payload exceeds receive buffer: %u bytes, buffer %zu", hdr->length, payload_cap);
        return -1;
    }
    if (hdr->length == 0)
        return 1;

    n = read_full(fd, payload, hdr->length);
    if (n <= 0)
    {
        if (n == 0)
        {
            set_protocol_error("connection closed while reading %u-byte frame payload", hdr->length);
        } else
        {
            set_protocol_error("failed to read %u-byte frame payload: %s",  hdr->length, strerror(errno));
        }
        return (int)n;
    }
    return 1;
}

int lcs_write_frame(int fd, uint16_t type, uint32_t seq, const void *payload, uint32_t length)
{
    if (length > LCS_MAX_FRAME)
        return -1;
    lcs_frame_header_t wire;
    wire.magic = htonl(LCS_PROTO_MAGIC);
    wire.type = htons(type);
    wire.flags = 0;
    wire.length = htonl(length);
    wire.seq = htonl(seq);
    if (write_full(fd, &wire, sizeof(wire)) != (ssize_t)sizeof(wire))
        return -1;

    if (length && write_full(fd, payload, length) != (ssize_t)length)
        return -1;

    return 0;
}

uint32_t lcs_next_seq(void)
{
    return g_seq++;
}

int lcs_encode_move_req(void *payload, size_t cap, size_t *len, const char *vip, const char *target_node)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_fixed_string(&w, vip, LCS_NAME_MAX + 1) != 0 || lcs_buf_put_fixed_string(&w, target_node, LCS_NAME_MAX + 1) != 0)
        return -1;

    *len = w.len;
    return 0;
}

int lcs_decode_move_req(const void *payload, size_t len,
                        char *vip, size_t vip_len,
                        char *target_node, size_t target_node_len)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    if (lcs_buf_get_fixed_string(&r, vip, vip_len, LCS_NAME_MAX + 1) != 0 || lcs_buf_get_fixed_string(&r, target_node, target_node_len, LCS_NAME_MAX + 1) != 0)
        return -1;

    return r.off == r.len ? 0 : -1;
}

int lcs_encode_clear_conflict_req(void *payload, size_t cap, size_t *len, const char *vip)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_fixed_string(&w, vip, LCS_NAME_MAX + 1) != 0)
        return -1;
    *len = w.len;
    return 0;
}

int lcs_decode_clear_conflict_req(const void *payload, size_t len, char *vip, size_t vip_len)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    if (lcs_buf_get_fixed_string(&r, vip, vip_len, LCS_NAME_MAX + 1) != 0)
        return -1;
    return r.off == r.len ? 0 : -1;
}

int lcs_encode_simple_resp(void *payload, size_t cap, size_t *len, int32_t status, const char *message)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u32(&w, status < 0 ? UINT32_MAX : (uint32_t)status) != 0 || lcs_buf_put_fixed_string(&w, message ? message : "", 128) != 0)
        return -1;

    *len = w.len;
    return 0;
}

int lcs_decode_simple_resp(const void *payload, size_t len, int32_t *status, char *message, size_t message_len)
{
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, len);
    uint32_t wire_status;
    if (lcs_buf_get_u32(&r, &wire_status) != 0 || lcs_buf_get_fixed_string(&r, message, message_len, 128) != 0)
        return -1;

    *status = wire_status == UINT32_MAX ? -1 : (int32_t)wire_status;
    return r.off == r.len ? 0 : -1;
}

int lcs_encode_status_header(lcs_buf_writer_t *w, uint16_t node_count,
                             uint16_t vip_count, uint16_t self_node,
                             uint16_t quorum_needed, uint16_t votes_seen,
                             uint8_t has_quorum)
{
    return lcs_buf_put_u16(w, node_count) ||
           lcs_buf_put_u16(w, vip_count) ||
           lcs_buf_put_u16(w, self_node) ||
           lcs_buf_put_u16(w, quorum_needed) ||
           lcs_buf_put_u16(w, votes_seen) ||
           lcs_buf_put_u8(w, has_quorum) ? -1 : 0;
}

int lcs_decode_status_header(lcs_buf_reader_t *r, uint16_t *node_count,
                             uint16_t *vip_count, uint16_t *self_node,
                             uint16_t *quorum_needed, uint16_t *votes_seen,
                             uint8_t *has_quorum)
{
    return lcs_buf_get_u16(r, node_count) ||
           lcs_buf_get_u16(r, vip_count) ||
           lcs_buf_get_u16(r, self_node) ||
           lcs_buf_get_u16(r, quorum_needed) ||
           lcs_buf_get_u16(r, votes_seen) ||
           lcs_buf_get_u8(r, has_quorum) ? -1 : 0;
}

int lcs_encode_status_node(lcs_buf_writer_t *w, uint16_t id, uint16_t role,
                           uint8_t online, uint8_t self, const char *name)
{
    return lcs_buf_put_u16(w, id) ||
           lcs_buf_put_u16(w, role) ||
           lcs_buf_put_u8(w, online) ||
           lcs_buf_put_u8(w, self) ||
           lcs_buf_put_fixed_string(w, name, LCS_NAME_MAX + 1) ? -1 : 0;
}

int lcs_decode_status_node(lcs_buf_reader_t *r, uint16_t *id, uint16_t *role,
                           uint8_t *online, uint8_t *self,
                           char *name, size_t name_len)
{
    return lcs_buf_get_u16(r, id) ||
           lcs_buf_get_u16(r, role) ||
           lcs_buf_get_u8(r, online) ||
           lcs_buf_get_u8(r, self) ||
           lcs_buf_get_fixed_string(r, name, name_len, LCS_NAME_MAX + 1) ? -1 : 0;
}

int lcs_encode_status_vip(lcs_buf_writer_t *w, uint16_t id, uint16_t owner_node,
                          uint64_t epoch, uint64_t lease_id, uint8_t state,
                          const char *name, const char *address,
                          const char *interface, const char *reason)
{
    return lcs_buf_put_u16(w, id) ||
           lcs_buf_put_u16(w, owner_node) ||
           lcs_buf_put_u64(w, epoch) ||
           lcs_buf_put_u64(w, lease_id) ||
           lcs_buf_put_u8(w, state) ||
           lcs_buf_put_fixed_string(w, name, LCS_NAME_MAX + 1) ||
           lcs_buf_put_fixed_string(w, address, LCS_ADDR_MAX + 1) ||
           lcs_buf_put_fixed_string(w, interface, LCS_NAME_MAX + 1) ||
           lcs_buf_put_fixed_string(w, reason, LCS_REASON_MAX + 1) ? -1 : 0;
}

int lcs_decode_status_vip(lcs_buf_reader_t *r, uint16_t *id, uint16_t *owner_node,
                          uint64_t *epoch, uint64_t *lease_id, uint8_t *state,
                          char *name, size_t name_len,
                          char *address, size_t address_len,
                          char *interface, size_t interface_len,
                          char *reason, size_t reason_len)
{
    return lcs_buf_get_u16(r, id) ||
           lcs_buf_get_u16(r, owner_node) ||
           lcs_buf_get_u64(r, epoch) ||
           lcs_buf_get_u64(r, lease_id) ||
           lcs_buf_get_u8(r, state) ||
           lcs_buf_get_fixed_string(r, name, name_len, LCS_NAME_MAX + 1) ||
           lcs_buf_get_fixed_string(r, address, address_len, LCS_ADDR_MAX + 1) ||
           lcs_buf_get_fixed_string(r, interface, interface_len, LCS_NAME_MAX + 1) ||
           lcs_buf_get_fixed_string(r, reason, reason_len, LCS_REASON_MAX + 1) ? -1 : 0;
}

void lcs_buf_writer_init(lcs_buf_writer_t *w, void *data, size_t cap)
{
    w->data = data;
    w->len = 0;
    w->cap = cap;
}

static int put_bytes(lcs_buf_writer_t *w, const void *data, size_t len)
{
    if (w->len + len > w->cap)
        return -1;

    memcpy(w->data + w->len, data, len);
    w->len += len;
    return 0;
}

int lcs_buf_put_u8(lcs_buf_writer_t *w, uint8_t v)
{
    return put_bytes(w, &v, sizeof(v));
}

int lcs_buf_put_u16(lcs_buf_writer_t *w, uint16_t v)
{
    uint16_t n = htons(v);
    return put_bytes(w, &n, sizeof(n));
}

int lcs_buf_put_u32(lcs_buf_writer_t *w, uint32_t v)
{
    uint32_t n = htonl(v);
    return put_bytes(w, &n, sizeof(n));
}

int lcs_buf_put_u64(lcs_buf_writer_t *w, uint64_t v)
{
    uint64_t n = htonll_u64(v);
    return put_bytes(w, &n, sizeof(n));
}

int lcs_buf_put_fixed_string(lcs_buf_writer_t *w, const char *s, size_t width)
{
    if (w->len + width > w->cap)
        return -1;

    memset(w->data + w->len, 0, width);
    if (s)
    {
        size_t n = strlen(s);
        if (n >= width)
            n = width - 1;
        memcpy(w->data + w->len, s, n);
    }
    w->len += width;
    return 0;
}

void lcs_buf_reader_init(lcs_buf_reader_t *r, const void *data, size_t len)
{
    r->data = data;
    r->len = len;
    r->off = 0;
}

static int get_bytes(lcs_buf_reader_t *r, void *data, size_t len)
{
    if (r->off + len > r->len)
        return -1;

    memcpy(data, r->data + r->off, len);
    r->off += len;
    return 0;
}

int lcs_buf_get_u8(lcs_buf_reader_t *r, uint8_t *v)
{
    return get_bytes(r, v, sizeof(*v));
}

int lcs_buf_get_u16(lcs_buf_reader_t *r, uint16_t *v)
{
    uint16_t n;
    if (get_bytes(r, &n, sizeof(n)) != 0)
        return -1;

    *v = ntohs(n);
    return 0;
}

int lcs_buf_get_u32(lcs_buf_reader_t *r, uint32_t *v)
{
    uint32_t n;
    if (get_bytes(r, &n, sizeof(n)) != 0)
        return -1;

    *v = ntohl(n);
    return 0;
}

int lcs_buf_get_u64(lcs_buf_reader_t *r, uint64_t *v)
{
    uint64_t n;
    if (get_bytes(r, &n, sizeof(n)) != 0)
        return -1;

    *v = ntohll_u64(n);
    return 0;
}

int lcs_buf_get_fixed_string(lcs_buf_reader_t *r, char *dst, size_t dst_len, size_t width)
{
    if (dst_len == 0 || r->off + width > r->len)
        return -1;
    
    size_t n = width < dst_len - 1 ? width : dst_len - 1;
    memcpy(dst, r->data + r->off, n);
    dst[n] = '\0';
    r->off += width;
    return 0;
}
