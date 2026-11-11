// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_freeze_protocol.h"
#include "protocol.h"

static bool digest_valid(const char digest[LCS_CONFIG_DIGEST_SIZE])
{
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f'))) return false;
    return !digest[64];
}

static bool valid(const lcs_config_freeze_message_t *message)
{
    if (!message) return false;
    const lcs_config_initialization_receipt_t *r = &message->receipt;
    return r->requester_instance && r->attempt && r->responder_instance &&
        digest_valid(r->membership) && digest_valid(r->baseline) &&
        ((message->kind == LCS_FREEZE_QUERY && !r->status) ||
         (message->kind == LCS_FREEZE_REPLY && r->status >= LCS_INITIALIZATION_FROZEN &&
          r->status <= LCS_INITIALIZATION_MISMATCH));
}

int lcs_config_freeze_encode(const lcs_config_freeze_message_t *message,
    void *payload, size_t capacity, size_t *length)
{
    if (!valid(message) || !payload || !length) return -1;
    const lcs_config_initialization_receipt_t *r = &message->receipt;
    lcs_buf_writer_t w; lcs_buf_writer_init(&w, payload, capacity);
    if (lcs_buf_put_u16(&w, LCS_CONFIG_FREEZE_FORMAT) || lcs_buf_put_u8(&w, message->kind) ||
        lcs_buf_put_u64(&w, r->requester_instance) || lcs_buf_put_u64(&w, r->attempt) ||
        lcs_buf_put_u64(&w, r->responder_instance) || lcs_buf_put_u8(&w, r->status) ||
        lcs_buf_put_fixed_string(&w, r->membership, sizeof(r->membership)) ||
        lcs_buf_put_fixed_string(&w, r->baseline, sizeof(r->baseline))) return -1;
    *length = w.len;
    return 0;
}

int lcs_config_freeze_decode(const void *payload, size_t length, lcs_config_freeze_message_t *message)
{
    if (!payload || !message || length != LCS_CONFIG_FREEZE_SIZE) return -1;
    lcs_config_freeze_message_t next = {0};
    lcs_config_initialization_receipt_t *r = &next.receipt;
    lcs_buf_reader_t reader; lcs_buf_reader_init(&reader, payload, length);
    uint16_t format;
    const unsigned char *bytes = payload;
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_FREEZE_FORMAT ||
        lcs_buf_get_u8(&reader, &next.kind) || lcs_buf_get_u64(&reader, &r->requester_instance) ||
        lcs_buf_get_u64(&reader, &r->attempt) || lcs_buf_get_u64(&reader, &r->responder_instance) ||
        lcs_buf_get_u8(&reader, &r->status) ||
        lcs_buf_get_fixed_string(&reader, r->membership, sizeof(r->membership), sizeof(r->membership)) ||
        lcs_buf_get_fixed_string(&reader, r->baseline, sizeof(r->baseline), sizeof(r->baseline)) ||
        reader.off != reader.len || bytes[92] || bytes[157] || !valid(&next)) return -1;
    *message = next;
    return 0;
}
