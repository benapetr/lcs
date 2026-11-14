// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_ACTIVATION_H
#define LCS_CONFIG_ACTIVATION_H

#include "config_application.h"

#define LCS_CONFIG_ACTIVATION_MAX (LCS_RESOURCE_DOCUMENT_MAX * 2u + 16384u)

typedef enum
{
    LCS_CONFIG_ACTIVATION_INITIALIZING = 1,
    LCS_CONFIG_ACTIVATION_PREPARED = 2,
    LCS_CONFIG_ACTIVATION_FROZEN = 3, /* Format 3: reload excluded, initialization NOT authorized. */
} lcs_config_activation_phase_t;

/* Local freeze/initialization intent, NOT a cluster activation decision. Its complete
 * revision-zero baseline freezes the predecessor before any journal is created.
 * Synchronous I/O: use before listeners open or in the serialized storage worker.
 * FROZEN excludes reload only; initialization needs separate authorization.
 * Configuration votes require PREPARED plus the separate distributed barrier. */
typedef struct
{
    lcs_config_store_t *store;
    const lcs_config_t *base; /* Borrowed immutable frozen_base output of open. */
    lcs_config_value_t baseline;
    char member[LCS_NAME_MAX + 1];
    char membership[LCS_CONFIG_DIGEST_SIZE];
    lcs_config_activation_phase_t phase;
    bool requests_required; /* Formats 2/3: required once initialized, not created by FROZEN alone. */
    bool poisoned;
} lcs_config_activation_t;

/* Never authorize first initialization from mere peer/version advertisements.
 * allow_initialize requires a coordinated, serialized initialization intent.
 * An existing record supplies resources even if INI definitions have changed.
 * cfg and frozen_base may alias; outputs must be fresh/unopened objects.
 * This compatibility primitive retains format-1 creation. Automatic upgrade
 * initialization must use open_requests below, never this legacy creation path. */
int lcs_config_activation_open(lcs_config_activation_t *activation,
                                lcs_config_store_t *store, const lcs_config_t *cfg,
                                lcs_config_t *frozen_base, bool allow_initialize,
                                char *err, size_t err_len);
/* Coordinated first initialization for automatic upgrade: write format 2,
 * requiring request history before PREPARED. Existing format-1 intents are never
 * silently upgraded or used to authorize a new empty request journal. The caller
 * must already exclude reload and coordinate initialization across members. */
int lcs_config_activation_open_requests(lcs_config_activation_t *activation,
                                         lcs_config_store_t *store, const lcs_config_t *cfg,
                                         lcs_config_t *frozen_base, bool allow_initialize,
                                         char *err, size_t err_len);
/* Persist only the reload freeze and exact baseline, before acknowledging it.
 * Requires serialized local reload exclusion. Creates NO agreement/application/
 * request history. Existing freeze is idempotent only for the exact baseline.
 * A coordinated open_requests(..., true) promotes it to INITIALIZING; read-only
 * recovery never promotes it. Runs before listeners or in the shared writer. */
int lcs_config_activation_freeze(lcs_config_activation_t *activation,
    lcs_config_store_t *store, const lcs_config_t *cfg, lcs_config_t *frozen_base,
    char *err, size_t err_len);
/* Resume initialization in agreement -> application -> required request history
 * -> prepared-record order. Format-1 intents retain their legacy semantics.
 * Missing records may be created ONLY while INITIALIZING and before any promise
 * or accepted value exists. PREPARED state never recreates missing records.
 * Success returns recovered handles; failure closes them and requires reopen. */
int lcs_config_activation_resume(lcs_config_activation_t *activation,
                                  lcs_config_agreement_t *agreement,
                                  lcs_config_application_t *application,
                                  char *err, size_t err_len);
void lcs_config_activation_close(lcs_config_activation_t *activation);

#endif
