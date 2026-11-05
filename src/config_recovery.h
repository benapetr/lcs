// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_RECOVERY_H
#define LCS_CONFIG_RECOVERY_H
#include "config_startup.h"

typedef enum
{
    LCS_CONFIG_RECOVERY_IDLE = 0,
    LCS_CONFIG_RECOVERY_BEGIN,
    LCS_CONFIG_RECOVERY_CLEANUP,
    LCS_CONFIG_RECOVERY_RETIRE,
    LCS_CONFIG_RECOVERY_OWNERS,
    LCS_CONFIG_RECOVERY_INSTALL,
    LCS_CONFIG_RECOVERY_FINISH,
    LCS_CONFIG_RECOVERY_READY,
    LCS_CONFIG_RECOVERY_BLOCKED,
    LCS_CONFIG_RECOVERY_FAILED,
    LCS_CONFIG_RECOVERY_STOPPING,
    LCS_CONFIG_RECOVERY_STOPPED,
} lcs_config_recovery_phase_t;

typedef struct
{
    lcs_config_coordinator_t *coordinator;
    lcs_config_owner_network_t *network;
    lcs_config_startup_t *cleanup, *next_cleanup;
    lcs_config_reconcile_step_t install;
    void *context;
    lcs_config_t *configs;
    lcs_config_value_t target;
    lcs_config_application_plan_t plan;
    char installed[LCS_CONFIG_DIGEST_SIZE];
    uint64_t attempt, completion;
    lcs_config_recovery_phase_t phase, blocked_phase;
    bool quorum_only, current_install;
    char error[256];
} lcs_config_recovery_t;

/* Explicit startup recovery controller, not automatic activation/admission.
 * Zero initialize; keep all backend/reactivation and lease fences held until a
 * SEPARATE generation/restart admission operation. Start before ordinary
 * backend/hook/probe/handoff workers run, or drain/cancel them first.
 * The coordinator must already
 * have recovered the durable barrier, and the owner network must be fresh and
 * borrow that exact runtime. install is mandatory, nonblocking and idempotent:
 * restore/remap definition tables and incarnation-aware private promises without
 * starting backends, granting votes, or clearing recovery. Its view is borrowed
 * only during the callback. No permissive backend/admission defaults exist.
 * READY still owns the coordinator's application/voting hold. Once start has
 * acquired that hold, a failure retains the object: stop/process/close it. */
int lcs_config_recovery_start(lcs_config_recovery_t *recovery,
                                lcs_config_coordinator_t *coordinator,
                                lcs_config_owner_network_t *network,
                                lcs_config_reconcile_step_t install, void *context);
/* Advances the shared writer, owner transport and cleanup workers without
 * synchronous disk/backend I/O. Call after transport/session admission updates;
 * admitted messages enter through owner_network_receive. 1 caught up/fenced,
 * 0 pending/stopping, -1 blocked/failed. READY resumes if learning advances.
 * The same transport/instance rules apply during BLOCKED and writer waits. */
int lcs_config_recovery_process(lcs_config_recovery_t *recovery,
                                  const uint64_t instances[LCS_MAX_NODES], uint64_t now_ms,
                                  lcs_config_owner_send_t send, void *context);
/* Explicit repair retry only; uncertain storage requires disk recovery. */
int lcs_config_recovery_retry(lcs_config_recovery_t *recovery, uint64_t now_ms);
/* Terminal shutdown retains the writer lock, workers and snapshots until reaped.
 * Process STOPPING to completion before close; neither path releases admission. */
void lcs_config_recovery_stop(lcs_config_recovery_t *recovery);
int lcs_config_recovery_close(lcs_config_recovery_t *recovery);
#endif
