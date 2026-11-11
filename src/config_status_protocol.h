// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_STATUS_PROTOCOL_H
#define LCS_CONFIG_STATUS_PROTOCOL_H

#include "protocol.h"

#define LCS_CONFIG_STATUS_DIGEST_SIZE 65
#define LCS_CONFIG_STATUS_FORMAT 1
#define LCS_CONFIG_STATUS_VIEW_FORMAT 2
#define LCS_CONFIG_STATUS_COMMANDS_FORMAT 3
#define LCS_CONFIG_STATUS_FROZEN_FORMAT 4

enum { LCS_CONFIG_AUTHORITY_INI = 0, LCS_CONFIG_AUTHORITY_JSON = 1 };
enum
{
    LCS_CONFIG_DIAG_LEGACY = 0,
    LCS_CONFIG_DIAG_RECOVERING,
    LCS_CONFIG_DIAG_BLOCKED,
    LCS_CONFIG_DIAG_ADMITTING,
    LCS_CONFIG_DIAG_ACTIVE,
    LCS_CONFIG_DIAG_FAILED,
    LCS_CONFIG_DIAG_STOPPING,
    LCS_CONFIG_DIAG_FROZEN,
};

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
    /* Local diagnostics only, never peer readiness/agreement evidence. Legacy
     * INI uses format 1; JSON uses 2/3; frozen INI uses 4 with no admission,
     * revisions or mutation capability. Older clients reject new formats. */
    uint8_t authority, admitted, dynamic_commands;
    uint16_t phase;
    uint64_t committed_revision, applied_revision, pending_revision;
    char committed_digest[LCS_CONFIG_STATUS_DIGEST_SIZE];
    char applied_digest[LCS_CONFIG_STATUS_DIGEST_SIZE];
    char pending_digest[LCS_CONFIG_STATUS_DIGEST_SIZE];
    char error[256];
} lcs_config_status_view_t;

const char *lcs_config_status_phase_name(uint16_t phase);

int lcs_config_status_encode(const lcs_config_status_t *status, void *payload, size_t cap, size_t *length);
int lcs_config_status_decode(const void *payload, size_t length, lcs_config_status_t *status);
bool lcs_config_status_equal(const lcs_config_status_t *a, const lcs_config_status_t *b);
int lcs_config_status_view_encode(const lcs_config_status_view_t *view, void *payload, size_t cap, size_t *length);
int lcs_config_status_view_decode(const void *payload, size_t length, lcs_config_status_view_t *view);
bool lcs_config_status_node_matches(const lcs_config_status_view_t *view, size_t node);

#endif
