// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_request_status_protocol.h"
#include "util.h"
#include <string.h>

static bool name_valid(const char *id)
{ return memchr(id, 0, LCS_CONFIG_REQUEST_MAX + 1) && lcs_valid_name(id); }
static bool digest_valid(const char *digest)
{
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f'))) return false;
    return !digest[64];
}
static int fixed(lcs_buf_reader_t *reader, char *out, size_t width)
{
    if (reader->off > reader->len || width > reader->len - reader->off) return -1;
    const unsigned char *start = reader->data + reader->off, *end = start + width, *nul = memchr(start, 0, width);
    if (!nul) return -1;
    for (const unsigned char *p = nul; p < end; p++) if (*p) return -1;
    memcpy(out, start, width); reader->off += width; return 0;
}
static bool valid(const lcs_config_request_view_t *view)
{
    if (!name_valid(view->id) || view->history > LCS_CONFIG_HISTORY_RECOVERY || view->state > LCS_CONFIG_REQUEST_APPLIED) return false;
    if (view->state == LCS_CONFIG_REQUEST_UNKNOWN)
        return !view->base_revision && !memcmp(view->base_digest, (char[65]){0}, 65) &&
            !memcmp(view->value_digest, (char[65]){0}, 65);
    return view->history == LCS_CONFIG_HISTORY_AVAILABLE && view->base_revision < LCS_RESOURCE_REVISION_MAX &&
        digest_valid(view->base_digest) && digest_valid(view->value_digest);
}
int lcs_config_request_id_encode(const char *id, void *payload, size_t cap, size_t *length)
{
    if (!id || strnlen(id, LCS_CONFIG_REQUEST_MAX + 1) > LCS_CONFIG_REQUEST_MAX || !lcs_valid_name(id)) return -1;
    lcs_buf_writer_t writer; lcs_buf_writer_init(&writer, payload, cap);
    if (lcs_buf_put_u16(&writer, LCS_CONFIG_REQUEST_STATUS_FORMAT) ||
        lcs_buf_put_fixed_string(&writer, id, LCS_CONFIG_REQUEST_MAX + 1)) return -1;
    *length = writer.len; return 0;
}
int lcs_config_request_id_decode(const void *payload, size_t length, char id[LCS_CONFIG_REQUEST_MAX + 1])
{
    lcs_buf_reader_t reader; lcs_buf_reader_init(&reader, payload, length);
    uint16_t format; char value[LCS_CONFIG_REQUEST_MAX + 1];
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_REQUEST_STATUS_FORMAT ||
        fixed(&reader, value, sizeof(value)) || reader.off != reader.len || !name_valid(value)) return -1;
    memcpy(id, value, sizeof(value)); return 0;
}
int lcs_config_request_view_encode(const lcs_config_request_view_t *view, void *payload, size_t cap, size_t *length)
{
    if (!valid(view)) return -1;
    lcs_buf_writer_t writer; lcs_buf_writer_init(&writer, payload, cap);
    if (lcs_buf_put_u16(&writer, view->state == LCS_CONFIG_REQUEST_APPLIED ? LCS_CONFIG_REQUEST_STATUS_APPLIED_FORMAT : LCS_CONFIG_REQUEST_STATUS_FORMAT) || lcs_buf_put_u8(&writer, view->history) ||
        lcs_buf_put_u8(&writer, view->state) || lcs_buf_put_fixed_string(&writer, view->id, sizeof(view->id)) ||
        lcs_buf_put_u64(&writer, view->base_revision) ||
        lcs_buf_put_fixed_string(&writer, view->base_digest, sizeof(view->base_digest)) ||
        lcs_buf_put_fixed_string(&writer, view->value_digest, sizeof(view->value_digest))) return -1;
    *length = writer.len; return 0;
}
int lcs_config_request_view_decode(const void *payload, size_t length, lcs_config_request_view_t *view)
{
    lcs_buf_reader_t reader; lcs_buf_reader_init(&reader, payload, length);
    uint16_t format; lcs_config_request_view_t value = {0};
    if (lcs_buf_get_u16(&reader, &format) || (format != LCS_CONFIG_REQUEST_STATUS_FORMAT && format != LCS_CONFIG_REQUEST_STATUS_APPLIED_FORMAT) ||
        lcs_buf_get_u8(&reader, &value.history) || lcs_buf_get_u8(&reader, &value.state) ||
        fixed(&reader, value.id, sizeof(value.id)) || lcs_buf_get_u64(&reader, &value.base_revision) ||
        fixed(&reader, value.base_digest, sizeof(value.base_digest)) ||
        fixed(&reader, value.value_digest, sizeof(value.value_digest)) || reader.off != reader.len || !valid(&value) || (value.state == LCS_CONFIG_REQUEST_APPLIED && format != LCS_CONFIG_REQUEST_STATUS_APPLIED_FORMAT)) return -1;
    *view = value; return 0;
}
