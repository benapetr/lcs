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
#endif
