// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_APPLICATION_H
#define LCS_CONFIG_APPLICATION_H

#include "config_agreement.h"

#define LCS_CONFIG_APPLICATION_HISTORY 2u
#define LCS_CONFIG_APPLICATION_MAX (LCS_RESOURCE_DOCUMENT_MAX * 8u + 32768u)

/* Local application catalog, not a consensus/ownership authority. All writes
 * are synchronous and must run outside the daemon lease event loop. The live
 * daemon validates existing catalogs at startup; the activated daemon manager
 * sequences live backend application through the shared nonblocking writer. */
typedef struct
{
    lcs_config_store_t *store;
    const lcs_config_t *base;
    char member[LCS_NAME_MAX + 1];
    char membership[LCS_CONFIG_DIGEST_SIZE];
    lcs_config_value_t applied;
    lcs_config_value_t pending;
    lcs_config_value_t history[LCS_CONFIG_APPLICATION_HISTORY]; /* Newest predecessor first. */
    bool poisoned;
} lcs_config_application_t;

typedef struct
{
    int new_to_old[LCS_MAX_RESOURCES]; /* Matching incarnation/backend, or -1. */
    uint32_t cleanup; /* Old backend definitions which must be removed. */
    uint32_t drain; /* cleanup plus dependency edits and transitive dependents. */
} lcs_config_application_plan_t;

/* A missing catalog may only be initialized explicitly at revision-zero
 * bootstrap. Existing/corrupt/missing active state is never silently reset. */
int lcs_config_application_open(lcs_config_application_t *application,
                                 const lcs_config_agreement_t *agreement,
                                 bool allow_initialize, char *err, size_t err_len);
void lcs_config_application_close(lcs_config_application_t *application);
/* Same-host worker handoff only, never peer decision evidence. Import performs
 * no I/O and admits only monotonic begin/finish transitions of the local catalog. */
int lcs_config_application_export(const lcs_config_application_t *application,
                                    char **document, size_t *length,
                                    char *err, size_t err_len);
int lcs_config_application_import(lcs_config_application_t *application,
                                    const lcs_config_agreement_t *agreement,
                                    const char *document, size_t length,
                                    char *err, size_t err_len);
/* Persist the latest locally learned decision as pending BEFORE touching any
 * backends. Returns 1 if a different pending application must finish first.
 * Intermediate revisions may be skipped; local cleanup obligations may not. */
int lcs_config_application_begin(lcs_config_application_t *application,
                                  const lcs_config_agreement_t *agreement,
                                  char *err, size_t err_len);
/* Caller must have completed local cleanup, stopped workers, remapped state
 * and satisfied the old-owner/lease barrier. Retain the two preceding complete
 * applied snapshots atomically with FINISH for restart cleanup/completion proof.
 * This records that fact; it does not
 * perform or authorize backend operations. The exact pending digest is required. */
int lcs_config_application_finish(lcs_config_application_t *application,
                                   const char *digest, char *err, size_t err_len);
/* Decode catalog-owned snapshots for cleanup/reconciliation; old definitions
 * and hooks remain in applied until FINISH, then in bounded predecessor history. */
int lcs_config_application_plan(const lcs_config_application_t *application,
                                 lcs_config_t *old_config, lcs_config_t *target,
                                 lcs_config_application_plan_t *plan,
                                 char *err, size_t err_len);

#endif
