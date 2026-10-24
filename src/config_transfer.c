// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_transfer.h"
#include "protocol.h"
#include "util.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define TRANSFER_FORMAT 1

static bool digest_valid(const char *digest)
{
    if (!digest || strnlen(digest, LCS_CONFIG_DIGEST_SIZE) != 64)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') ||
              (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    return true;
}

static bool valid(const lcs_config_transfer_metadata_t *metadata)
{
    return metadata && metadata->kind >= LCS_CONFIG_TRANSFER_ACCEPT &&
        metadata->kind <= LCS_CONFIG_TRANSFER_SYNC && metadata->id &&
        metadata->revision <= LCS_RESOURCE_REVISION_MAX && metadata->length &&
        metadata->length <= LCS_RESOURCE_DOCUMENT_MAX &&
        strnlen(metadata->request_id, sizeof(metadata->request_id)) < sizeof(metadata->request_id) &&
        lcs_valid_name(metadata->request_id) && digest_valid(metadata->membership) &&
        digest_valid(metadata->parent_digest) && digest_valid(metadata->value_digest);
}

int lcs_config_transfer_encode_begin(const lcs_config_transfer_metadata_t *metadata,
                                     void *payload, size_t cap, size_t *length)
{
    if (!valid(metadata))
        return -1;
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u8(&w, TRANSFER_FORMAT) || lcs_buf_put_u8(&w, (uint8_t)metadata->kind) ||
        lcs_buf_put_u16(&w, 0) || lcs_buf_put_u64(&w, metadata->id) ||
        lcs_buf_put_u64(&w, metadata->revision) || lcs_buf_put_u64(&w, metadata->ballot) ||
        lcs_buf_put_u64(&w, metadata->accepted_ballot) || lcs_buf_put_u32(&w, metadata->voters) ||
        lcs_buf_put_u32(&w, metadata->length) ||
        lcs_buf_put_fixed_string(&w, metadata->membership, LCS_CONFIG_DIGEST_SIZE) ||
        lcs_buf_put_fixed_string(&w, metadata->parent_digest, LCS_CONFIG_DIGEST_SIZE) ||
        lcs_buf_put_fixed_string(&w, metadata->value_digest, LCS_CONFIG_DIGEST_SIZE) ||
        lcs_buf_put_fixed_string(&w, metadata->request_id, LCS_CONFIG_REQUEST_MAX + 1))
        return -1;
    *length = w.len;
    return 0;
}

static int get_string(lcs_buf_reader_t *reader, char *value, size_t width)
{
    if (width > reader->len - reader->off)
        return -1;
    const unsigned char *start = reader->data + reader->off;
    const unsigned char *end = memchr(start, 0, width);
    if (!end)
        return -1;
    for (const unsigned char *p = end; p < start + width; p++)
        if (*p)
            return -1;
    memcpy(value, start, width);
    reader->off += width;
    return 0;
}

int lcs_config_transfer_decode_begin(const void *payload, size_t length,
                                     lcs_config_transfer_metadata_t *metadata)
{
    lcs_config_transfer_metadata_t next = {0};
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, length);
    uint8_t format, kind;
    uint16_t reserved;
    if (lcs_buf_get_u8(&r, &format) || format != TRANSFER_FORMAT ||
        lcs_buf_get_u8(&r, &kind) || lcs_buf_get_u16(&r, &reserved) || reserved ||
        lcs_buf_get_u64(&r, &next.id) || lcs_buf_get_u64(&r, &next.revision) ||
        lcs_buf_get_u64(&r, &next.ballot) || lcs_buf_get_u64(&r, &next.accepted_ballot) ||
        lcs_buf_get_u32(&r, &next.voters) || lcs_buf_get_u32(&r, &next.length) ||
        get_string(&r, next.membership, sizeof(next.membership)) ||
        get_string(&r, next.parent_digest, sizeof(next.parent_digest)) ||
        get_string(&r, next.value_digest, sizeof(next.value_digest)) ||
        get_string(&r, next.request_id, sizeof(next.request_id)) || r.off != r.len)
        return -1;
    next.kind = (lcs_config_transfer_kind_t)kind;
    if (!valid(&next))
        return -1;
    *metadata = next;
    return 0;
}

int lcs_config_transfer_encode_chunk(uint64_t id, uint32_t offset,
                                     const void *data, size_t count,
                                     void *payload, size_t cap, size_t *length)
{
    if (!id || !data || !count || count > LCS_CONFIG_TRANSFER_CHUNK_MAX ||
        offset > LCS_RESOURCE_DOCUMENT_MAX || count > LCS_RESOURCE_DOCUMENT_MAX - offset)
        return -1;
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, cap);
    if (lcs_buf_put_u8(&w, TRANSFER_FORMAT) || lcs_buf_put_u64(&w, id) ||
        lcs_buf_put_u32(&w, offset) || count > w.cap - w.len)
        return -1;
    memcpy(w.data + w.len, data, count);
    *length = w.len + count;
    return 0;
}

void lcs_config_transfer_clear(lcs_config_transfer_t *transfer)
{
    free(transfer->document);
    memset(transfer, 0, sizeof(*transfer));
}

bool lcs_config_transfer_expire(lcs_config_transfer_t *transfer, uint64_t now_ms)
{
    if (!transfer->document || now_ms < transfer->deadline_ms)
        return false;
    lcs_config_transfer_clear(transfer);
    return true;
}

int lcs_config_transfer_begin(lcs_config_transfer_t *transfer,
                              const lcs_config_transfer_metadata_t *metadata,
                              const char *membership, uint64_t now_ms)
{
    if (!valid(metadata) || !digest_valid(membership) || strcmp(membership, metadata->membership) ||
        now_ms > UINT64_MAX - LCS_CONFIG_TRANSFER_TIMEOUT_MS)
        return -1;
    lcs_config_transfer_expire(transfer, now_ms);
    if (transfer->document)
    {
        unsigned char old[512], incoming[512];
        size_t old_len, incoming_len;
        if (lcs_config_transfer_encode_begin(&transfer->metadata, old, sizeof(old), &old_len) ||
            lcs_config_transfer_encode_begin(metadata, incoming, sizeof(incoming), &incoming_len))
            return -1;
        return old_len == incoming_len && !memcmp(old, incoming, old_len) ? 0 : 1;
    }
    char *document = malloc((size_t)metadata->length + 1);
    if (!document)
        return -1;
    transfer->document = document;
    transfer->metadata = *metadata;
    transfer->used = 0;
    transfer->deadline_ms = now_ms + LCS_CONFIG_TRANSFER_TIMEOUT_MS;
    return 0;
}

int lcs_config_transfer_chunk(lcs_config_transfer_t *transfer,
                              const void *payload, size_t length, uint64_t now_ms)
{
    if (lcs_config_transfer_expire(transfer, now_ms) || !transfer->document)
        return -1;
    lcs_buf_reader_t r;
    lcs_buf_reader_init(&r, payload, length);
    uint8_t format;
    uint64_t id;
    uint32_t offset;
    if (lcs_buf_get_u8(&r, &format) || format != TRANSFER_FORMAT ||
        lcs_buf_get_u64(&r, &id) || id != transfer->metadata.id ||
        lcs_buf_get_u32(&r, &offset))
        return -1;
    size_t count = r.len - r.off;
    if (!count || count > LCS_CONFIG_TRANSFER_CHUNK_MAX ||
        offset > transfer->used || offset > transfer->metadata.length ||
        count > transfer->metadata.length - offset)
        return -1;
    if (offset < transfer->used)
        return count <= transfer->used - offset &&
               !memcmp(transfer->document + offset, r.data + r.off, count) ? 0 : -1;
    memcpy(transfer->document + offset, r.data + r.off, count);
    transfer->used += count;
    return 0;
}

int lcs_config_transfer_finish(lcs_config_transfer_t *transfer, uint64_t id,
                               const lcs_config_t *base, uint64_t now_ms,
                               lcs_config_value_t *value, char *err, size_t err_len)
{
    if (lcs_config_transfer_expire(transfer, now_ms) || !transfer->document ||
        id != transfer->metadata.id || transfer->used != transfer->metadata.length)
    {
        if (err && err_len)
            snprintf(err, err_len, "configuration transfer is incomplete, expired, or has a different identifier");
        return -1;
    }
    lcs_config_value_t next = {0};
    int rc = lcs_config_value_init(&next, base, transfer->metadata.request_id,
        transfer->document, transfer->used, err, err_len);
    if (!rc && (next.revision != transfer->metadata.revision ||
                 strcmp(next.digest, transfer->metadata.value_digest)))
    {
        lcs_config_value_clear(&next);
        if (err && err_len)
            snprintf(err, err_len, "configuration transfer revision or content identity mismatch");
        rc = -1;
    }
    lcs_config_transfer_clear(transfer);
    if (!rc)
        *value = next;
    return rc;
}
