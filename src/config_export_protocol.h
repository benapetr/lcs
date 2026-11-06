// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_EXPORT_PROTOCOL_H
#define LCS_CONFIG_EXPORT_PROTOCOL_H
#include "protocol.h"
#include "resource_config.h"

#define LCS_CONFIG_EXPORT_FORMAT 1
#define LCS_CONFIG_EXPORT_HEADER_SIZE 20u
enum { LCS_CONFIG_EXPORT_COMMITTED = 1, LCS_CONFIG_EXPORT_APPLIED = 2 };
typedef struct
{
    uint8_t authority, selection; /* authority: 0 INI, 1 JSON */
    uint64_t revision;
    uint32_t total, offset;
} lcs_config_export_chunk_t;

int lcs_config_export_request_encode(uint8_t selection, void *payload, size_t cap, size_t *length);
int lcs_config_export_request_decode(const void *payload, size_t length, uint8_t *selection);
int lcs_config_export_chunk_encode(const lcs_config_export_chunk_t *chunk, const void *data, size_t size,
                                    void *payload, size_t cap, size_t *length);
int lcs_config_export_chunk_decode(const void *payload, size_t length, lcs_config_export_chunk_t *chunk,
                                    const void **data, size_t *size);
#endif
