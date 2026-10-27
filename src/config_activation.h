// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_ACTIVATION_H
#define LCS_CONFIG_ACTIVATION_H

#include "config_application.h"

#define LCS_CONFIG_ACTIVATION_MAX (LCS_RESOURCE_DOCUMENT_MAX * 2u + 16384u)

typedef enum
{
    LCS_CONFIG_ACTIVATION_INITIALIZING = 1,
    LCS_CONFIG_ACTIVATION_PREPARED = 2,
} lcs_config_activation_phase_t;

/* Local initialization intent, NOT a cluster activation decision. Its complete
 * revision-zero baseline freezes the predecessor before any journal is created.
 * Synchronous I/O: use before listeners open or in the serialized storage worker.
 * Configuration votes require PREPARED plus the separate distributed barrier. */
typedef struct
{
    lcs_config_store_t *store;
    const lcs_config_t *base; /* Borrowed immutable frozen_base output of open. */
    lcs_config_value_t baseline;
    char member[LCS_NAME_MAX + 1];
    char membership[LCS_CONFIG_DIGEST_SIZE];
    lcs_config_activation_phase_t phase;
    bool poisoned;
} lcs_config_activation_t;

/* Never authorize first initialization from mere peer/version advertisements.
 * allow_initialize requires a coordinated, serialized initialization intent.
 * An existing record supplies resources even if INI definitions have changed.
 * cfg and frozen_base may alias; outputs must be fresh/unopened objects. */
int lcs_config_activation_open(lcs_config_activation_t *activation,
                                lcs_config_store_t *store, const lcs_config_t *cfg,
                                lcs_config_t *frozen_base, bool allow_initialize,
                                char *err, size_t err_len);
/* Resume initialization in journal -> application -> prepared-record order.
 * Missing records may be created ONLY while INITIALIZING and before any promise
 * or accepted value exists. PREPARED state never recreates missing records.
 * Success returns recovered handles; failure closes them and requires reopen. */
int lcs_config_activation_resume(lcs_config_activation_t *activation,
                                  lcs_config_agreement_t *agreement,
                                  lcs_config_application_t *application,
                                  char *err, size_t err_len);
void lcs_config_activation_close(lcs_config_activation_t *activation);

#endif
