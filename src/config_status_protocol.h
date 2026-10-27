// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_STATUS_PROTOCOL_H
#define LCS_CONFIG_STATUS_PROTOCOL_H

#include "protocol.h"

#define LCS_CONFIG_STATUS_DIGEST_SIZE 65
#define LCS_CONFIG_STATUS_FORMAT 1

/* Advisory bootstrap status, bound to the established peer session. It is not
 * a promise, acceptance, activation-readiness acknowledgement, or authority. */
typedef struct
{
    uint64_t serial;
    uint8_t prepared;
    char membership[LCS_CONFIG_STATUS_DIGEST_SIZE];
    char baseline[LCS_CONFIG_STATUS_DIGEST_SIZE];
} lcs_config_status_t;

typedef struct
{
    char name[LCS_NAME_MAX + 1];
    uint16_t role;
    uint16_t protocol;
    uint8_t connected;
    uint8_t known;
    lcs_config_status_t status;
} lcs_config_status_node_t;

typedef struct
{
    uint16_t count;
    uint16_t self;
    lcs_config_status_node_t nodes[LCS_MAX_NODES];
} lcs_config_status_view_t;

int lcs_config_status_encode(const lcs_config_status_t *status, void *payload, size_t cap, size_t *length);
int lcs_config_status_decode(const void *payload, size_t length, lcs_config_status_t *status);
bool lcs_config_status_equal(const lcs_config_status_t *a, const lcs_config_status_t *b);
int lcs_config_status_view_encode(const lcs_config_status_view_t *view, void *payload, size_t cap, size_t *length);
int lcs_config_status_view_decode(const void *payload, size_t length, lcs_config_status_view_t *view);
bool lcs_config_status_node_matches(const lcs_config_status_view_t *view, size_t node);

#endif
