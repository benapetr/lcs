// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_RUNTIME_H
#define LCS_CONFIG_RUNTIME_H

#include "config_barrier.h"

/* Daemon-owned recovery boundary. The base copy remains immutable while durable
 * handles are open, even when the live INI resource arrays are reloaded. Opening
 * may finish an existing initialization intent but never discards history or
 * authorizes first initialization. Existing distributed state must not
 * be replaced by revision-zero INI bootstrap, even if resources.json is stale. */
typedef struct
{
    lcs_config_t *base;
    lcs_config_agreement_t agreement;
    lcs_config_application_t application;
    lcs_config_activation_t activation;
    lcs_config_barrier_t barrier;
    bool requires_recovery;
} lcs_config_runtime_t;

/* Call after locking storage, before bootstrap refresh or opening listeners.
 * Missing both records means legacy mode. Partial/corrupt/foreign records fail.
 * Valid records are retained for activation-aware recovery; the current legacy-mode
 * daemon refuses to run them until that protocol/application path is integrated. */
int lcs_config_runtime_open(lcs_config_runtime_t *runtime,
                             lcs_config_store_t *store, const lcs_config_t *cfg,
                             char *err, size_t err_len);
void lcs_config_runtime_close(lcs_config_runtime_t *runtime);

#endif
