// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_export_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

int main(void)
{
    unsigned char payload[LCS_MAX_FRAME + 1], saved[LCS_MAX_FRAME]; size_t length;
    uint8_t selection = 99;
    assert(!lcs_config_export_request_encode(LCS_CONFIG_EXPORT_COMMITTED, payload, sizeof(payload), &length));
    assert(length == 3 && !lcs_config_export_request_decode(payload, length, &selection) && selection == 1);
    for (size_t i = 0; i < length; i++) assert(lcs_config_export_request_decode(payload, i, &selection) < 0);
    payload[3] = 0; assert(lcs_config_export_request_decode(payload, 4, &selection) < 0);
    payload[1] = 2; assert(lcs_config_export_request_decode(payload, 3, &selection) < 0);
    payload[1] = 1; payload[2] = 0; assert(lcs_config_export_request_decode(payload, 3, &selection) < 0);
    assert(lcs_config_export_request_encode(3, payload, sizeof(payload), &length) < 0);
    assert(lcs_config_export_request_encode(1, payload, 2, &length) < 0);

    char *document = malloc(LCS_RESOURCE_DOCUMENT_MAX), *copy = malloc(LCS_RESOURCE_DOCUMENT_MAX);
    assert(document && copy); memset(document, 'x', LCS_RESOURCE_DOCUMENT_MAX);
    lcs_config_export_chunk_t chunk = {.authority = 1, .selection = 2, .revision = 123,
        .total = LCS_RESOURCE_DOCUMENT_MAX}, decoded;
    unsigned frames = 0;
    while (chunk.offset < chunk.total)
    {
        size_t size = chunk.total - chunk.offset, got; const void *data;
        if (size > LCS_MAX_FRAME - LCS_CONFIG_EXPORT_HEADER_SIZE) size = LCS_MAX_FRAME - LCS_CONFIG_EXPORT_HEADER_SIZE;
        assert(!lcs_config_export_chunk_encode(&chunk, document + chunk.offset, size, payload, sizeof(payload), &length));
        assert(length == size + LCS_CONFIG_EXPORT_HEADER_SIZE);
        assert(!lcs_config_export_chunk_decode(payload, length, &decoded, &data, &got));
        assert(decoded.offset == chunk.offset && decoded.total == chunk.total && decoded.revision == 123 && got == size);
        memcpy(copy + decoded.offset, data, got); chunk.offset += (uint32_t)got; frames++;
    }
    assert(frames > 1 && !memcmp(document, copy, LCS_RESOURCE_DOCUMENT_MAX));
    chunk.offset = 0;
    assert(!lcs_config_export_chunk_encode(&chunk, document, 10, payload, sizeof(payload), &length));
    memcpy(saved, payload, length);
    for (size_t i = 0; i <= LCS_CONFIG_EXPORT_HEADER_SIZE; i++)
    { const void *data; size_t size; assert(lcs_config_export_chunk_decode(payload, i, &decoded, &data, &size) < 0); }
    length = LCS_CONFIG_EXPORT_HEADER_SIZE + 10;
    for (unsigned fault = 0; fault < 6; fault++)
    {
        memcpy(payload, saved, length);
        if (fault == 0) payload[1] = 2;
        if (fault == 1) payload[2] = 2;
        if (fault == 2) payload[3] = 0;
        if (fault == 3) memset(payload + 4, 0, 8); /* JSON authority requires nonzero revision. */
        if (fault == 4) memset(payload + 12, 0, 4);
        if (fault == 5) memset(payload + 16, 255, 4);
        const void *data; size_t size;
        assert(lcs_config_export_chunk_decode(payload, length, &decoded, &data, &size) < 0);
    }
    chunk.authority = 0; assert(lcs_config_export_chunk_encode(&chunk, document, 10, payload, sizeof(payload), &length) < 0);
    chunk.revision = 0; assert(!lcs_config_export_chunk_encode(&chunk, document, 10, payload, sizeof(payload), &length));
    assert(lcs_config_export_chunk_encode(&chunk, document, 0, payload, sizeof(payload), &length) < 0);
    chunk.total++; assert(lcs_config_export_chunk_encode(&chunk, document, 10, payload, sizeof(payload), &length) < 0);
    free(document); free(copy);
    puts("bounded configuration export codec unit tests passed"); return 0;
}
