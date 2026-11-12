// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_RUNTIME_H
#define LCS_CONFIG_RUNTIME_H

#include "config_barrier.h"
#include "config_requests.h"

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
    lcs_config_requests_t requests; /* Existing journal only; absent means outcomes unknown. */
    bool requires_recovery;
} lcs_config_runtime_t;

/* Call after locking storage, before bootstrap refresh or opening listeners.
 * No distributed records means legacy mode; orphan request history never does.
 * Existing request history is recovered without writes against the frozen base.
 * Missing history remains unavailable only for legacy format-1 initialization,
 * preserving older catalogs without inventing empty history. Format-2 intents
 * require history: resume creation only while INITIALIZING, never after PREPARED.
 * Format-3 FROZEN recovers only the immutable baseline and reload exclusion,
 * never creates history or promotes initialization. The daemon can restore this
 * state for coordination with resource voting/placement fenced. Fresh all-member
 * freeze evidence may authorize local initialization through the shared writer;
 * automatic first-freeze creation and JSON activation remain unfinished.
 * Partial/corrupt/foreign or contradictory records fail before listeners open. */
int lcs_config_runtime_open(lcs_config_runtime_t *runtime,
                             lcs_config_store_t *store, const lcs_config_t *cfg,
                             char *err, size_t err_len);
void lcs_config_runtime_close(lcs_config_runtime_t *runtime);
/* Healthy format-2 revision-zero preparation, with empty bound request history
 * and no readiness barrier, promises, decisions or application work. No I/O.
 * This permits fenced coordination/reaffirming the freeze, never a vote. */
bool lcs_config_runtime_initialization_prepared(const lcs_config_runtime_t *runtime);
/* Same pristine revision-zero state plus its valid durable all-member barrier.
 * Permits fenced coordination recovery, not resource/JSON admission. */
bool lcs_config_runtime_readiness_prepared(const lcs_config_runtime_t *runtime);

/* Recovered mandatory-history revision-zero baseline plus a valid barrier.
 * Retains promises/reservations and a validated accepted activation value with
 * its exact request identity. No learned successor or application work. Read-only
 * startup gate after runtime_open; never clears history or admits resources. */
bool lcs_config_runtime_activation_pending(const lcs_config_runtime_t *runtime);

/* After runtime_open validation: a learned schema-2 revision-one activation with
 * its exact baseline still applied and no unrelated pending application/history.
 * Allows fenced initial application, never generation or resource admission. */
bool lcs_config_runtime_activation_chosen(const lcs_config_runtime_t *runtime);

#endif
