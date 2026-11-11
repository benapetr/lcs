// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_MUTATION_PROTOCOL_H
#define LCS_CONFIG_MUTATION_PROTOCOL_H
#include "config_request_status_protocol.h"
#define LCS_CONFIG_MUTATION_FORMAT 1
#define LCS_CONFIG_MUTATION_ARG_MAX 64
#define LCS_CONFIG_MUTATION_RESPONSE_SIZE (3u + LCS_CONFIG_REQUEST_STATUS_SIZE + 256u)
enum { LCS_CONFIG_MUTATION_APPLIED = 0, LCS_CONFIG_MUTATION_REJECTED = 1,
       LCS_CONFIG_MUTATION_PENDING = 2, LCS_CONFIG_MUTATION_UNKNOWN = 3 };
typedef struct
{
    char id[LCS_CONFIG_REQUEST_MAX + 1];
    bool revision_set, digest_set;
    uint64_t revision;
    char digest[LCS_CONFIG_DIGEST_SIZE];
    int argc;
    const char *argv[LCS_CONFIG_MUTATION_ARG_MAX];
    char arguments[LCS_MAX_FRAME]; /* Decoder-owned strings; do not copy argv pointers. */
} lcs_config_mutation_t;
typedef struct
{
    uint8_t result;
    lcs_config_request_view_t request;
    char reason[256];
} lcs_config_mutation_result_t;
int lcs_config_mutation_encode(const lcs_config_mutation_t *request, void *payload, size_t cap, size_t *length);
int lcs_config_mutation_decode(const void *payload, size_t length, lcs_config_mutation_t *request);
int lcs_config_mutation_result_encode(const lcs_config_mutation_result_t *result, void *payload, size_t cap, size_t *length);
int lcs_config_mutation_result_decode(const void *payload, size_t length, lcs_config_mutation_result_t *result);

#define LCS_CONFIG_IMPORT_FORMAT 1
#define LCS_CONFIG_IMPORT_BEGIN_MAX 256u
#define LCS_CONFIG_IMPORT_CHUNK_HEADER 6u
/* BEGIN wraps a fixed config/import/json mutation descriptor and total length.
 * CHUNK carries an offset and nonempty bytes. No commit before complete upload. */
int lcs_config_import_begin_encode(const lcs_config_mutation_t *request, uint32_t total,
                                     void *payload, size_t cap, size_t *length);
int lcs_config_import_begin_decode(const void *payload, size_t length,
                                     lcs_config_mutation_t *request, uint32_t *total);
int lcs_config_import_chunk_encode(uint32_t offset, const void *data, size_t size,
                                     void *payload, size_t cap, size_t *length);
int lcs_config_import_chunk_decode(const void *payload, size_t length,
                                     uint32_t *offset, const void **data, size_t *size);
#endif
