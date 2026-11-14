// SPDX-License-Identifier: GPL-3.0-or-later
#include "resource_frame.h"
#include <string.h>

static bool digest_valid(const char *value)
{
    for (size_t i = 0; i < 64; i++)
        if (!((value[i] >= '0' && value[i] <= '9') || (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return !value[64];
}

bool lcs_resource_frame_type(uint16_t type)
{
    switch (type)
    {
        case LCS_MSG_STATE_SYNC_REQ: case LCS_MSG_STATE_SYNC_RESP:
        case LCS_MSG_LEASE_REQ: case LCS_MSG_LEASE_ACK: case LCS_MSG_LEASE_RENEW:
        case LCS_MSG_LEASE_RELEASE: case LCS_MSG_LEASE_COMMIT:
        case LCS_MSG_OWNER_RELEASE_REQ: case LCS_MSG_OWNER_RELEASE_RESP:
        case LCS_MSG_MOVE_REQ: case LCS_MSG_MOVE_RESP:
        case LCS_MSG_RESOURCE_START_REQ: case LCS_MSG_RESOURCE_START_RESP:
        case LCS_MSG_RESOURCE_STOP_REQ: case LCS_MSG_RESOURCE_STOP_RESP:
        case LCS_MSG_CLEAR_CONFLICT_REQ: case LCS_MSG_CLEAR_CONFLICT_RESP:
        case LCS_MSG_HEARTBEAT: case LCS_MSG_ERROR:
            return true;
        default: return false;
    }
}

bool lcs_resource_generation_equal(const lcs_resource_generation_t *a, const lcs_resource_generation_t *b)
{
    return a->revision == b->revision && !memcmp(a->membership, b->membership, sizeof(a->membership)) &&
        !memcmp(a->digest, b->digest, sizeof(a->digest));
}

int lcs_resource_frame_prefix(const lcs_resource_generation_t *generation,
                                uint64_t sender, uint64_t target, uint16_t type, size_t body_length,
                                void *payload, size_t cap, size_t *length)
{
    if (!generation || !payload || !length || !sender || !target || !generation->revision || generation->revision > INT64_MAX ||
        !digest_valid(generation->membership) || !digest_valid(generation->digest) ||
        !lcs_resource_frame_type(type) || body_length > LCS_MAX_FRAME - LCS_RESOURCE_FRAME_PREFIX)
        return -1;
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, cap);
    if (lcs_buf_put_u16(&writer, LCS_RESOURCE_FRAME_FORMAT) || lcs_buf_put_u16(&writer, type) ||
        lcs_buf_put_u64(&writer, sender) || lcs_buf_put_u64(&writer, target) ||
        lcs_buf_put_u64(&writer, generation->revision) ||
        lcs_buf_put_fixed_string(&writer, generation->membership, sizeof(generation->membership)) ||
        lcs_buf_put_fixed_string(&writer, generation->digest, sizeof(generation->digest)))
        return -1;
    *length = writer.len;
    return 0;
}

int lcs_resource_frame_decode(const void *payload, size_t length, uint64_t sender, uint64_t target,
                                lcs_resource_generation_t *generation, uint16_t *type,
                                const void **body, size_t *body_length)
{
    if (!payload || !generation || !type || !body || !body_length || length < LCS_RESOURCE_FRAME_PREFIX || length > LCS_MAX_FRAME || !sender || !target)
        return -1;
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, payload, length);
    uint16_t format, kind;
    uint64_t from, to;
    lcs_resource_generation_t next = {0};
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_RESOURCE_FRAME_FORMAT ||
        lcs_buf_get_u16(&reader, &kind) || !lcs_resource_frame_type(kind) ||
        lcs_buf_get_u64(&reader, &from) || from != sender || lcs_buf_get_u64(&reader, &to) || to != target ||
        lcs_buf_get_u64(&reader, &next.revision) || !next.revision || next.revision > INT64_MAX)
        return -1;
    /* Fixed canonical 64-hex-plus-NUL fields; no alternate padding encodings. */
    memcpy(next.membership, reader.data + reader.off, sizeof(next.membership)); reader.off += sizeof(next.membership);
    memcpy(next.digest, reader.data + reader.off, sizeof(next.digest)); reader.off += sizeof(next.digest);
    if (!digest_valid(next.membership) || !digest_valid(next.digest))
        return -1;
    *generation = next; *type = kind;
    *body = reader.data + reader.off; *body_length = reader.len - reader.off;
    return 0;
}
