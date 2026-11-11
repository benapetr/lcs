// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_mutation_protocol.h"
#include "util.h"
#include <string.h>

static bool digest_valid(const char *digest)
{
    if (strnlen(digest, 65) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f'))) return false;
    return true;
}
static int fixed(lcs_buf_reader_t *reader, char *out, size_t width)
{
    if (reader->off > reader->len || width > reader->len - reader->off) return -1;
    const unsigned char *start = reader->data + reader->off, *nul = memchr(start, 0, width);
    if (!nul) return -1;
    for (const unsigned char *p = nul; p < start + width; p++) if (*p) return -1;
    memcpy(out, start, width); reader->off += width; return 0;
}
static bool valid(const lcs_config_mutation_t *request)
{
    return memchr(request->id, 0, sizeof(request->id)) && lcs_valid_name(request->id) &&
        (request->revision_set ? request->revision < LCS_RESOURCE_REVISION_MAX : !request->revision) &&
        (request->digest_set ? digest_valid(request->digest) : !memcmp(request->digest, (char[65]){0}, 65)) &&
        request->argc >= 3 && request->argc <= LCS_CONFIG_MUTATION_ARG_MAX;
}
int lcs_config_mutation_encode(const lcs_config_mutation_t *request, void *payload, size_t cap, size_t *length)
{
    if (!request || !valid(request)) return -1;
    lcs_buf_writer_t w; lcs_buf_writer_init(&w, payload, cap < LCS_MAX_FRAME ? cap : LCS_MAX_FRAME);
    if (lcs_buf_put_u16(&w, LCS_CONFIG_MUTATION_FORMAT) || lcs_buf_put_fixed_string(&w, request->id, sizeof(request->id)) ||
        lcs_buf_put_u8(&w, (request->revision_set ? 1 : 0) | (request->digest_set ? 2 : 0)) ||
        lcs_buf_put_u64(&w, request->revision) || lcs_buf_put_fixed_string(&w, request->digest, sizeof(request->digest)) ||
        lcs_buf_put_u16(&w, (uint16_t)request->argc)) return -1;
    for (int i = 0; i < request->argc; i++)
    {
        if (!request->argv[i]) return -1;
        size_t n = strnlen(request->argv[i], LCS_PATH_MAX + 33u);
        if (n > LCS_PATH_MAX + 32u || n > UINT16_MAX || lcs_buf_put_u16(&w, (uint16_t)n) || n > w.cap - w.len) return -1;
        memcpy(w.data + w.len, request->argv[i], n); w.len += n;
    }
    *length = w.len; return 0;
}
int lcs_config_mutation_decode(const void *payload, size_t length, lcs_config_mutation_t *request)
{
    if (!request || length > LCS_MAX_FRAME) return -1;
    lcs_buf_reader_t r; lcs_buf_reader_init(&r, payload, length);
    uint16_t format, argc; uint8_t flags;
    memset(request, 0, sizeof(*request));
    if (lcs_buf_get_u16(&r, &format) || format != LCS_CONFIG_MUTATION_FORMAT || fixed(&r, request->id, sizeof(request->id)) ||
        lcs_buf_get_u8(&r, &flags) || flags > 3 || lcs_buf_get_u64(&r, &request->revision) ||
        fixed(&r, request->digest, sizeof(request->digest)) || lcs_buf_get_u16(&r, &argc)) return -1;
    request->revision_set = flags & 1; request->digest_set = flags & 2; request->argc = argc;
    if (!valid(request)) return -1;
    size_t used = 0;
    for (int i = 0; i < request->argc; i++)
    {
        uint16_t n;
        if (lcs_buf_get_u16(&r, &n) || n > LCS_PATH_MAX + 32u || n > r.len - r.off ||
            (size_t)n + 1 > sizeof(request->arguments) - used || memchr(r.data + r.off, 0, n)) return -1;
        request->argv[i] = request->arguments + used;
        memcpy(request->arguments + used, r.data + r.off, n); used += n + 1; r.off += n;
    }
    return r.off == r.len ? 0 : -1;
}
static bool result_valid(const lcs_config_mutation_result_t *result)
{
    return result->result <= LCS_CONFIG_MUTATION_UNKNOWN && result->reason[0] && memchr(result->reason, 0, sizeof(result->reason)) &&
        (result->result != LCS_CONFIG_MUTATION_APPLIED || result->request.state == LCS_CONFIG_REQUEST_APPLIED) &&
        (result->result != LCS_CONFIG_MUTATION_PENDING || result->request.state == LCS_CONFIG_REQUEST_COMMITTED) &&
        (result->result != LCS_CONFIG_MUTATION_REJECTED || result->request.state == LCS_CONFIG_REQUEST_UNKNOWN);
}
int lcs_config_mutation_result_encode(const lcs_config_mutation_result_t *result, void *payload, size_t cap, size_t *length)
{
    if (!result_valid(result)) return -1;
    lcs_buf_writer_t w; lcs_buf_writer_init(&w, payload, cap); size_t n;
    if (lcs_buf_put_u16(&w, LCS_CONFIG_MUTATION_FORMAT) || lcs_buf_put_u8(&w, result->result) ||
        lcs_config_request_view_encode(&result->request, w.data + w.len, w.cap - w.len, &n)) return -1;
    w.len += n;
    if (lcs_buf_put_fixed_string(&w, result->reason, sizeof(result->reason))) return -1;
    *length = w.len; return 0;
}
int lcs_config_mutation_result_decode(const void *payload, size_t length, lcs_config_mutation_result_t *result)
{
    if (length != LCS_CONFIG_MUTATION_RESPONSE_SIZE) return -1;
    lcs_buf_reader_t r; lcs_buf_reader_init(&r, payload, length);
    uint16_t format; lcs_config_mutation_result_t next = {0};
    if (lcs_buf_get_u16(&r, &format) || format != LCS_CONFIG_MUTATION_FORMAT || lcs_buf_get_u8(&r, &next.result) ||
        lcs_config_request_view_decode(r.data + r.off, LCS_CONFIG_REQUEST_STATUS_SIZE, &next.request)) return -1;
    r.off += LCS_CONFIG_REQUEST_STATUS_SIZE;
    if (fixed(&r, next.reason, sizeof(next.reason)) || !result_valid(&next)) return -1;
    *result = next; return 0;
}
