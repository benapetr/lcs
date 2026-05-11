// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#ifndef LCS_PROTOCOL_H
#define LCS_PROTOCOL_H

#include "common.h"

#include <stddef.h>
#include <stdint.h>

#define LCS_PROTO_MAGIC 0x4c435331u
#define LCS_PEER_PROTO_VERSION 3
#define LCS_LOCAL_PROTO_VERSION 1000
#define LCS_MAX_FRAME (64u * 1024u)

typedef enum
{
    LCS_MSG_STATUS_REQ = 1,
    LCS_MSG_STATUS_RESP = 2,
    LCS_MSG_MOVE_REQ = 3,
    LCS_MSG_MOVE_RESP = 4,
    LCS_MSG_ERROR = 5,
    LCS_MSG_CLEAR_CONFLICT_REQ = 6,
    LCS_MSG_CLEAR_CONFLICT_RESP = 7,
    LCS_MSG_HELLO = 16,
    LCS_MSG_HELLO_ACK = 17,
    LCS_MSG_STATE_SYNC_REQ = 18,
    LCS_MSG_STATE_SYNC_RESP = 19,
    LCS_MSG_LEASE_REQ = 20,
    LCS_MSG_LEASE_ACK = 21,
    LCS_MSG_LEASE_RENEW = 22,
    LCS_MSG_LEASE_RELEASE = 23,
    LCS_MSG_OWNER_RELEASE_REQ = 24,
    LCS_MSG_OWNER_RELEASE_RESP = 25,
    LCS_MSG_HEARTBEAT_REQ = 26,
    LCS_MSG_HEARTBEAT_RESP = 27,
} lcs_msg_type_t;

typedef struct
{
    uint32_t magic;
    uint16_t type;
    uint16_t flags;
    uint32_t length;
    uint32_t seq;
} lcs_frame_header_t;

typedef struct
{
    unsigned char *data;
    size_t len;
    size_t cap;
} lcs_buf_writer_t;

typedef struct
{
    const unsigned char *data;
    size_t len;
    size_t off;
} lcs_buf_reader_t;

void lcs_buf_writer_init(lcs_buf_writer_t *w, void *data, size_t cap);
int lcs_buf_put_u8(lcs_buf_writer_t *w, uint8_t v);
int lcs_buf_put_u16(lcs_buf_writer_t *w, uint16_t v);
int lcs_buf_put_u32(lcs_buf_writer_t *w, uint32_t v);
int lcs_buf_put_u64(lcs_buf_writer_t *w, uint64_t v);
int lcs_buf_put_fixed_string(lcs_buf_writer_t *w, const char *s, size_t width);

void lcs_buf_reader_init(lcs_buf_reader_t *r, const void *data, size_t len);
int lcs_buf_get_u8(lcs_buf_reader_t *r, uint8_t *v);
int lcs_buf_get_u16(lcs_buf_reader_t *r, uint16_t *v);
int lcs_buf_get_u32(lcs_buf_reader_t *r, uint32_t *v);
int lcs_buf_get_u64(lcs_buf_reader_t *r, uint64_t *v);
int lcs_buf_get_fixed_string(lcs_buf_reader_t *r, char *dst, size_t dst_len, size_t width);

int lcs_read_frame(int fd, lcs_frame_header_t *hdr, void *payload, size_t payload_cap);
int lcs_write_frame(int fd, uint16_t type, uint32_t seq, const void *payload, uint32_t length);
uint32_t lcs_next_seq(void);
const char *lcs_protocol_error(void);

int lcs_encode_move_req(void *payload, size_t cap, size_t *len,  const char *vip, const char *target_node);
int lcs_decode_move_req(const void *payload, size_t len, char *vip, size_t vip_len, char *target_node, size_t target_node_len);
int lcs_encode_clear_conflict_req(void *payload, size_t cap, size_t *len, const char *vip);
int lcs_decode_clear_conflict_req(const void *payload, size_t len, char *vip, size_t vip_len);
int lcs_encode_simple_resp(void *payload, size_t cap, size_t *len, int32_t status, const char *message);
int lcs_decode_simple_resp(const void *payload, size_t len, int32_t *status, char *message, size_t message_len);

int lcs_encode_status_header(lcs_buf_writer_t *w, uint16_t node_count,
                             uint16_t vip_count, uint16_t self_node,
                             uint16_t quorum_needed, uint16_t votes_seen,
                             uint8_t has_quorum,
                             uint64_t membership_seconds);
int lcs_decode_status_header(lcs_buf_reader_t *r, uint16_t *node_count,
                             uint16_t *vip_count, uint16_t *self_node,
                             uint16_t *quorum_needed, uint16_t *votes_seen,
                             uint8_t *has_quorum,
                             uint64_t *membership_seconds);
int lcs_encode_status_node(lcs_buf_writer_t *w, uint16_t id, uint16_t role,
                           uint8_t online, uint8_t self, const char *name);
int lcs_decode_status_node(lcs_buf_reader_t *r, uint16_t *id, uint16_t *role,
                           uint8_t *online, uint8_t *self,
                           char *name, size_t name_len);
int lcs_encode_status_vip(lcs_buf_writer_t *w, uint16_t id, uint16_t owner_node,
                          uint64_t epoch, uint64_t lease_id, uint8_t state,
                          const char *name, const char *address,
                          const char *interface, const char *group,
                          uint32_t priority, const char *reason);
int lcs_decode_status_vip(lcs_buf_reader_t *r, uint16_t *id, uint16_t *owner_node,
                          uint64_t *epoch, uint64_t *lease_id, uint8_t *state,
                          char *name, size_t name_len,
                          char *address, size_t address_len,
                          char *interface, size_t interface_len,
                          char *group, size_t group_len,
                          uint32_t *priority,
                          char *reason, size_t reason_len);

#endif
