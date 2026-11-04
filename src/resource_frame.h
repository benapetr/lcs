// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_RESOURCE_FRAME_H
#define LCS_RESOURCE_FRAME_H

#include "protocol.h"

#define LCS_RESOURCE_FRAME_FORMAT 1
#define LCS_RESOURCE_FRAME_PREFIX 158u

typedef struct
{
    uint64_t revision;
    char membership[65], digest[65];
} lcs_resource_generation_t;

/* Only resource-indexed traffic and resource voting/liveness belong inside.
 * HELLO and configuration agreement/readiness/checkpoints remain outside. */
bool lcs_resource_frame_type(uint16_t type);
bool lcs_resource_generation_equal(const lcs_resource_generation_t *a, const lcs_resource_generation_t *b);
/* Writes only the fixed prefix; caller appends the unchanged inner body. The
 * complete payload must fit LCS_MAX_FRAME. The outer sequence remains unchanged. */
int lcs_resource_frame_prefix(const lcs_resource_generation_t *generation,
                                uint64_t sender, uint64_t target, uint16_t type, size_t body_length,
                                void *payload, size_t cap, size_t *length);
int lcs_resource_frame_decode(const void *payload, size_t length, uint64_t sender, uint64_t target,
                                lcs_resource_generation_t *generation, uint16_t *type,
                                const void **body, size_t *body_length);
#endif
