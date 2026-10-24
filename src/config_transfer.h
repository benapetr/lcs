// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_TRANSFER_H
#define LCS_CONFIG_TRANSFER_H

#include "config_agreement.h"

#define LCS_CONFIG_TRANSFER_CHUNK_MAX (16u * 1024u)
#define LCS_CONFIG_TRANSFER_TIMEOUT_MS 10000u

typedef enum
{
    LCS_CONFIG_TRANSFER_ACCEPT = 1,
    LCS_CONFIG_TRANSFER_PROMISE = 2,
    LCS_CONFIG_TRANSFER_LEARN = 3,
    LCS_CONFIG_TRANSFER_SYNC = 4,
} lcs_config_transfer_kind_t;

typedef struct
{
    lcs_config_transfer_kind_t kind;
    uint64_t id;
    uint64_t revision;
    uint64_t ballot;
    uint64_t accepted_ballot;
    uint32_t voters;
    uint32_t length;
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char parent_digest[LCS_CONFIG_DIGEST_SIZE];
    char value_digest[LCS_CONFIG_DIGEST_SIZE];
    char request_id[LCS_CONFIG_REQUEST_MAX + 1];
} lcs_config_transfer_metadata_t;

/* Transport-independent codecs for future negotiated dynamic-config messages.
 * They are not registered in peer.c and do not advertise a protocol feature. */
int lcs_config_transfer_encode_begin(const lcs_config_transfer_metadata_t *metadata,
                                     void *payload, size_t cap, size_t *length);
int lcs_config_transfer_decode_begin(const void *payload, size_t length,
                                     lcs_config_transfer_metadata_t *metadata);
int lcs_config_transfer_encode_chunk(uint64_t id, uint32_t offset,
                                     const void *data, size_t count,
                                     void *payload, size_t cap, size_t *length);

typedef struct
{
    lcs_config_transfer_metadata_t metadata;
    char *document;
    size_t used;
    uint64_t deadline_ms;
} lcs_config_transfer_t;

void lcs_config_transfer_clear(lcs_config_transfer_t *transfer);
/* One receiver per authenticated peer. Returns 1 if busy with a different
 * transfer; identical BEGIN retries preserve progress and the original deadline. */
int lcs_config_transfer_begin(lcs_config_transfer_t *transfer,
                              const lcs_config_transfer_metadata_t *metadata,
                              const char *membership, uint64_t now_ms);
int lcs_config_transfer_chunk(lcs_config_transfer_t *transfer,
                              const void *payload, size_t length, uint64_t now_ms);
bool lcs_config_transfer_expire(lcs_config_transfer_t *transfer, uint64_t now_ms);
/* A completed transfer constructs a validated value, never votes or applies it.
 * On finish success or validation failure, the receive buffer is released. */
int lcs_config_transfer_finish(lcs_config_transfer_t *transfer, uint64_t id,
                               const lcs_config_t *base, uint64_t now_ms,
                               lcs_config_value_t *value, char *err, size_t err_len);

#endif
