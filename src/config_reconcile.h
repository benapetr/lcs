// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_RECONCILE_H
#define LCS_CONFIG_RECONCILE_H

#include "config_coordinator.h"

typedef enum
{
    LCS_CONFIG_RECONCILE_IDLE = 0,
    LCS_CONFIG_RECONCILE_BEGIN,
    LCS_CONFIG_RECONCILE_QUIESCE,
    LCS_CONFIG_RECONCILE_DRAIN,
    LCS_CONFIG_RECONCILE_OWNERS,
    LCS_CONFIG_RECONCILE_INSTALL,
    LCS_CONFIG_RECONCILE_FINISH,
    LCS_CONFIG_RECONCILE_ADMIT,
    LCS_CONFIG_RECONCILE_DONE,
    LCS_CONFIG_RECONCILE_BLOCKED,
    LCS_CONFIG_RECONCILE_RECOVERY,
    LCS_CONFIG_RECONCILE_STOPPED,
} lcs_config_reconcile_phase_t;

/* Borrowed immutable data for ONE callback invocation. Adapters must copy what
 * asynchronous work needs and bind results to attempt + target digest/incarnation;
 * never retain these pointers or treat a delayed result as a different attempt. */
typedef struct
{
    const lcs_config_t *old_config, *target;
    const lcs_config_application_plan_t *plan;
    const lcs_config_value_t *value;
    uint64_t attempt;
} lcs_config_reconcile_view_t;

typedef int (*lcs_config_reconcile_step_t)(void *context, const lcs_config_reconcile_view_t *view,
                                           char *err, size_t err_len);
/* Nonblocking, idempotent steps: 0 pending, 1 complete, negative blocked.
 * quiesce suspends affected grants/acquisition. drain uses OLD definitions/hooks
 * and dependency order, waiting for backend/hook/probe/handoff workers and stop
 * failures. owners requires confirmed old-owner stop or valid lease expiry;
 * known/suspected failed stops require confirmed cleanup/fencing, never timeout.
 * install closes old indexed sessions/operations and remaps matching incarnations
 * while keeping new activity fenced. admit releases activity only after durable
 * applied state and generation/restart/lease admission. A completed step must
 * keep its guarantees until admission or shutdown; no timeouts imply success.
 * Callbacks must not reenter/mutate agreement, catalog, coordinator or reconciler.
 * Quorum-only members skip quiesce/drain/owners, but still install/admit metadata.
 * All required callbacks must be supplied; no permissive production defaults. */
typedef struct
{
    lcs_config_reconcile_step_t quiesce, drain, owners, install, admit;
    /* Required by adapters which enable synchronization before final admission.
     * cancel re-fences on superseding learning/failure/stop; complete releases
     * activity only after the sequencer transfers its application hold. */
    void (*cancel_admission)(void *context), (*complete_admission)(void *context);
} lcs_config_reconcile_ops_t;

typedef struct
{
    lcs_config_coordinator_t *coordinator;
    lcs_config_reconcile_ops_t ops;
    void *context;
    lcs_config_t *configs; /* Owned old/target pair. */
    lcs_config_application_plan_t plan;
    lcs_config_value_t value;
    uint64_t attempt, completion;
    lcs_config_reconcile_phase_t phase, blocked_phase;
    bool quorum_only;
    char error[256];
} lcs_config_reconcile_t;

/* Fresh object, explicitly started only after activation authorization and
 * startup fencing. Already-current catalogs return DONE without adapters: this
 * is not a substitute for normal restart/generation admission. No daemon backend
 * adapter or automatic activation is installed by this module. */
void lcs_config_reconcile_init(lcs_config_reconcile_t *reconcile, lcs_config_coordinator_t *coordinator,
                                 const lcs_config_reconcile_ops_t *ops, void *context);
int lcs_config_reconcile_start(lcs_config_reconcile_t *reconcile);
/* Caller advances the coordinator first; never performs filesystem I/O here.
 * Returns 1 done, 0 pending, -1 blocked/recovery/stopped. A newer checkpoint cannot
 * overwrite an existing pending target: finish it, then stage the latest value. */
int lcs_config_reconcile_process(lcs_config_reconcile_t *reconcile);
/* Retry only an adapter failure after external repair; never retry uncertain I/O
 * using poisoned handles. Old snapshots and the exact failed stage are retained. */
int lcs_config_reconcile_retry(lcs_config_reconcile_t *reconcile);
/* Terminal shutdown: stop the shared coordinator, retaining its writer/lock until
 * reaped. Adapters must drain/cancel their own asynchronous work before disposal. */
void lcs_config_reconcile_stop(lcs_config_reconcile_t *reconcile);
/* Returns -1 if the coordinator is still live/owns a writer or pending manager.
 * Does not cancel or silently drop cleanup obligations. */
int lcs_config_reconcile_close(lcs_config_reconcile_t *reconcile);
#endif
