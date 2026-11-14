// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_export_protocol.h"
#include <string.h>

static bool selection_valid(uint8_t selection)
{ return selection == LCS_CONFIG_EXPORT_COMMITTED || selection == LCS_CONFIG_EXPORT_APPLIED; }

int lcs_config_export_request_encode(uint8_t selection, void *payload, size_t cap, size_t *length)
{
    lcs_buf_writer_t writer; lcs_buf_writer_init(&writer, payload, cap);
    if (!selection_valid(selection) || lcs_buf_put_u16(&writer, LCS_CONFIG_EXPORT_FORMAT) ||
        lcs_buf_put_u8(&writer, selection)) return -1;
    *length = writer.len; return 0;
}
int lcs_config_export_request_decode(const void *payload, size_t length, uint8_t *selection)
{
    lcs_buf_reader_t reader; lcs_buf_reader_init(&reader, payload, length);
    uint16_t format; uint8_t value;
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_EXPORT_FORMAT ||
        lcs_buf_get_u8(&reader, &value) || !selection_valid(value) || reader.off != reader.len) return -1;
    *selection = value; return 0;
}
static bool chunk_valid(const lcs_config_export_chunk_t *chunk, size_t size)
{
    return chunk->authority <= 1 && selection_valid(chunk->selection) &&
        ((chunk->authority == 0 && chunk->revision == 0) ||
         (chunk->authority == 1 && chunk->revision && chunk->revision <= INT64_MAX)) &&
        chunk->total && chunk->total <= LCS_RESOURCE_DOCUMENT_MAX && chunk->offset < chunk->total &&
        size && size <= LCS_MAX_FRAME - LCS_CONFIG_EXPORT_HEADER_SIZE && size <= chunk->total - chunk->offset;
}
int lcs_config_export_chunk_encode(const lcs_config_export_chunk_t *chunk, const void *data, size_t size,
                                    void *payload, size_t cap, size_t *length)
{
    if (!chunk_valid(chunk, size) || !data || cap < LCS_CONFIG_EXPORT_HEADER_SIZE || size > cap - LCS_CONFIG_EXPORT_HEADER_SIZE) return -1;
    lcs_buf_writer_t writer; lcs_buf_writer_init(&writer, payload, cap);
    if (lcs_buf_put_u16(&writer, LCS_CONFIG_EXPORT_FORMAT) || lcs_buf_put_u8(&writer, chunk->authority) ||
        lcs_buf_put_u8(&writer, chunk->selection) || lcs_buf_put_u64(&writer, chunk->revision) ||
        lcs_buf_put_u32(&writer, chunk->total) || lcs_buf_put_u32(&writer, chunk->offset)) return -1;
    memcpy(writer.data + writer.len, data, size); *length = writer.len + size; return 0;
}
int lcs_config_export_chunk_decode(const void *payload, size_t length, lcs_config_export_chunk_t *chunk,
                                    const void **data, size_t *size)
{
    lcs_buf_reader_t reader; lcs_buf_reader_init(&reader, payload, length);
    uint16_t format; lcs_config_export_chunk_t value = {0};
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_EXPORT_FORMAT ||
        lcs_buf_get_u8(&reader, &value.authority) || lcs_buf_get_u8(&reader, &value.selection) ||
        lcs_buf_get_u64(&reader, &value.revision) || lcs_buf_get_u32(&reader, &value.total) ||
        lcs_buf_get_u32(&reader, &value.offset) || !chunk_valid(&value, length - reader.off)) return -1;
    *chunk = value; *data = reader.data + reader.off; *size = length - reader.off; return 0;
}
