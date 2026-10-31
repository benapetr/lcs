// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_BARRIER_H
#define LCS_CONFIG_BARRIER_H

#include "config_activation.h"

#define LCS_CONFIG_BARRIER_MAX 2048u

/* Explicit durable-readiness receipts, NOT advisory config status. Transport
 * must admit each receipt under its configured peer identity and capability.
 * Readiness means frozen initialization is durable and INI reload is excluded.
 * This is the same non-Byzantine evidence model as agreement voter masks. */
typedef struct
{
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char baseline[LCS_CONFIG_DIGEST_SIZE];
    uint32_t required;
    uint32_t ready;
    bool sealed;
    bool failed;
} lcs_config_barrier_round_t;

typedef struct
{
    lcs_config_store_t *store;
    const lcs_config_t *base;
    char member[LCS_NAME_MAX + 1];
    char membership[LCS_CONFIG_DIGEST_SIZE];
    char baseline[LCS_CONFIG_DIGEST_SIZE];
    uint32_t ready;
    bool poisoned;
} lcs_config_barrier_t;

bool lcs_config_barrier_local_ready(const lcs_config_activation_t *activation,
                                      const lcs_config_agreement_t *agreement,
                                      const lcs_config_application_t *application);
int lcs_config_barrier_begin(lcs_config_barrier_round_t *round,
                              const lcs_config_activation_t *activation,
                              const lcs_config_agreement_t *agreement,
                              const lcs_config_application_t *application,
                              char *err, size_t err_len);
int lcs_config_barrier_ready(lcs_config_barrier_round_t *round, size_t node,
                              const char *membership, const char *baseline,
                              char *err, size_t err_len);
int lcs_config_barrier_seal(lcs_config_barrier_round_t *round, char *err, size_t err_len);
/* Startup/serialized storage worker ONLY. NULL authorization reads existing
 * state, returning 1 when absent. Creating a missing record requires a sealed
 * all-member round and pristine local initialization: never reconstruct lost
 * readiness after promises/accepted/committed successors exist. A write failure
 * poisons the handle and requires disk recovery. Outputs must be fresh. */
int lcs_config_barrier_open(lcs_config_barrier_t *barrier,
                             const lcs_config_activation_t *activation,
                             const lcs_config_agreement_t *agreement,
                             const lcs_config_application_t *application,
                             const lcs_config_barrier_round_t *authorization,
                             char *err, size_t err_len);
void lcs_config_barrier_close(lcs_config_barrier_t *barrier);
/* Configuration votes only. Does NOT switch authority, activate resources, or
 * replace lease/recovery gates. Application must match the learned revision;
 * an unfinished application blocks votes on the following configuration. */
bool lcs_config_barrier_can_vote(const lcs_config_barrier_t *barrier,
                                   const lcs_config_activation_t *activation,
                                   const lcs_config_agreement_t *agreement,
                                   const lcs_config_application_t *application);
#endif
