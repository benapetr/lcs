// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_recovery.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static void cancel_admission(lcs_config_recovery_t *recovery)
{
    if (recovery->admission_cancel) recovery->admission_cancel(recovery->admission_context);
    recovery->admission = NULL; recovery->admission_cancel = NULL; recovery->admission_complete = NULL;
    recovery->admission_context = NULL;
}

static int failure(lcs_config_recovery_t *recovery, const char *message, bool terminal)
{
    if (terminal) cancel_admission(recovery);
    if (terminal && recovery->network) lcs_config_owner_network_stop(recovery->network);
    recovery->blocked_phase = recovery->phase;
    recovery->phase = terminal ? LCS_CONFIG_RECOVERY_FAILED : LCS_CONFIG_RECOVERY_BLOCKED;
    snprintf(recovery->error, sizeof(recovery->error), "%s", message);
    return -1;
}

static int failure_detail(lcs_config_recovery_t *recovery, const char *message, const char *detail, bool terminal)
{
    char error[sizeof(recovery->error)];
    snprintf(error, sizeof(error), "%.90s: %.160s", message, detail);
    return failure(recovery, error, terminal);
}

static void clear_target(lcs_config_recovery_t *recovery)
{
    free(recovery->configs); recovery->configs = NULL;
    lcs_config_value_clear(&recovery->target);
    memset(&recovery->plan, 0, sizeof(recovery->plan));
}

static int start_cleanup(lcs_config_recovery_t *recovery)
{
    if (recovery->next_cleanup) return failure(recovery, "cleanup already in progress", true);
    recovery->next_cleanup = calloc(1, sizeof(*recovery->next_cleanup));
    if (!recovery->next_cleanup || lcs_config_startup_begin(recovery->next_cleanup,
        recovery->coordinator->runtime, recovery->network->instance, recovery->error, sizeof(recovery->error)))
        return failure_detail(recovery, "cannot capture authoritative recovery cleanup", recovery->error, true);
    recovery->phase = LCS_CONFIG_RECOVERY_CLEANUP;
    return 0;
}

int lcs_config_recovery_start(lcs_config_recovery_t *recovery,
                                lcs_config_coordinator_t *coordinator,
                                lcs_config_owner_network_t *network,
                                lcs_config_reconcile_step_t install, void *context)
{
    if (!recovery || recovery->coordinator || recovery->phase != LCS_CONFIG_RECOVERY_IDLE || !coordinator ||
        !coordinator->initialized || !coordinator->runtime || !coordinator->runtime->base ||
        !coordinator->readiness || coordinator->failed || coordinator->closing || coordinator->application_owner ||
        coordinator->storage.pid > 0 || coordinator->phase != LCS_CONFIG_COORD_READY || !network || !install ||
        !network->instance || network->serial || network->closing || network->recovery || network->receipt.sealed ||
        network->receipt.failed || network->retired || network->round.active || network->release.active ||
        network->base != coordinator->runtime->base || network->application != &coordinator->runtime->application ||
        !coordinator->runtime->requires_recovery || network->self != (size_t)lcs_config_self_index(network->base) ||
        !lcs_config_barrier_valid(&coordinator->runtime->barrier, &coordinator->runtime->activation,
            &coordinator->runtime->agreement, &coordinator->runtime->application)) return -1;
    recovery->coordinator = coordinator; recovery->network = network;
    recovery->install = install; recovery->context = context;
    recovery->quorum_only = network->base->nodes[network->self].role == LCS_NODE_QUORUM_ONLY;
    coordinator->application_owner = recovery;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!runtime->application.pending.document && !lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed))
    { recovery->phase = LCS_CONFIG_RECOVERY_BEGIN; return 0; }
    return start_cleanup(recovery);
}

static int capture(lcs_config_recovery_t *recovery, bool current)
{
    const lcs_config_application_t *application = &recovery->coordinator->runtime->application;
    clear_target(recovery);
    if (recovery->attempt == UINT64_MAX) return failure(recovery, "recovery install attempts exhausted", true);
    recovery->configs = malloc(2 * sizeof(*recovery->configs));
    if (!recovery->configs || lcs_config_value_copy(&recovery->target, current ? &application->applied : &application->pending))
        return failure(recovery, "cannot retain recovery installation target", true);
    if (current)
    {
        recovery->configs[0] = *recovery->cleanup->config;
        recovery->configs[1] = recovery->configs[0];
        for (size_t i = 0; i < recovery->configs[0].resource_count; i++) recovery->plan.new_to_old[i] = (int)i;
    }
    else if (lcs_config_application_plan(application, &recovery->configs[0], &recovery->configs[1],
        &recovery->plan, recovery->error, sizeof(recovery->error)))
        return failure(recovery, "invalid pending recovery installation plan", true);
    recovery->attempt++; recovery->current_install = current;
    recovery->phase = LCS_CONFIG_RECOVERY_INSTALL;
    return 0;
}

static int resolve(lcs_config_recovery_t *recovery)
{
    const lcs_config_runtime_t *runtime = recovery->coordinator->runtime;
    if (runtime->application.pending.document)
    { recovery->phase = LCS_CONFIG_RECOVERY_OWNERS; return 0; }
    if (!lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed))
    { recovery->phase = LCS_CONFIG_RECOVERY_BEGIN; return 0; }
    if (strcmp(recovery->installed, runtime->application.applied.digest)) return capture(recovery, true);
    recovery->phase = LCS_CONFIG_RECOVERY_READY;
    return 1;
}

static bool admission_ready(const lcs_config_recovery_t *recovery)
{
    const lcs_config_coordinator_t *coordinator = recovery->coordinator;
    if (!coordinator || !coordinator->initialized || coordinator->failed || coordinator->closing ||
        coordinator->application_owner != recovery || coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0 ||
        !recovery->cleanup || recovery->next_cleanup || !recovery->network || recovery->network->closing ||
        recovery->network->receipt.sealed || recovery->network->receipt.failed || recovery->network->round.active ||
        recovery->network->release.active) return false;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!runtime->application.applied.revision || runtime->application.pending.document ||
        !lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed) ||
        strcmp(recovery->installed, runtime->application.applied.digest) ||
        !lcs_config_startup_verified(recovery->cleanup, runtime, recovery->network->instance)) return false;
    if (recovery->quorum_only) return !recovery->network->recovery;
    lcs_config_application_t pair = runtime->application;
    pair.pending = pair.applied; pair.applied = runtime->application.history[0];
    lcs_config_owner_scope_t scope; char error[256];
    return recovery->network->retired && recovery->network->recovery && recovery->network->recovery_context == recovery->cleanup &&
        !lcs_config_owner_scope(&pair, &scope, error, sizeof(error)) &&
        lcs_config_owner_scope_equal(&scope, &recovery->network->retired_scope);
}

int lcs_config_recovery_begin_admission(lcs_config_recovery_t *recovery,
                                         lcs_config_reconcile_step_t step,
                                         void (*cancel)(void *), void (*complete)(void *), void *context)
{
    if (!recovery || recovery->phase != LCS_CONFIG_RECOVERY_READY || !step || !cancel || !complete || !admission_ready(recovery))
        return -1;
    recovery->admission = step; recovery->admission_cancel = cancel; recovery->admission_complete = complete;
    recovery->admission_context = context; recovery->phase = LCS_CONFIG_RECOVERY_ADMITTING;
    return 0;
}

static int progress_admission(lcs_config_recovery_t *recovery)
{
    if (!admission_ready(recovery))
    {
        cancel_admission(recovery); recovery->phase = LCS_CONFIG_RECOVERY_READY;
        return 0; /* Re-fence before the normal controller can stage newer learning. */
    }
    lcs_config_application_plan_t plan = {0};
    for (size_t i = 0; i < recovery->cleanup->config->resource_count; i++) plan.new_to_old[i] = (int)i;
    lcs_config_reconcile_view_t view = {.old_config = recovery->cleanup->config, .target = recovery->cleanup->config,
        .value = &recovery->cleanup->applied, .plan = &plan, .attempt = recovery->attempt};
    int result = recovery->admission(recovery->admission_context, &view, recovery->error, sizeof(recovery->error));
    if (result < 0 || result > 1) return failure_detail(recovery, "generation/restart admission failed", recovery->error, true);
    if (!result) return 0;
    if (!admission_ready(recovery)) return failure(recovery, "admission changed recovery authority", true);
    /* No callback, transport work, or writer may reenter between these checks
     * and ownership transfer. Retain the current completion scope and serials. */
    if (lcs_config_startup_close(recovery->cleanup)) return failure(recovery, "cleanup still borrowed during admission", true);
    recovery->network->recovery = NULL; recovery->network->recovery_context = NULL;
    free(recovery->cleanup); recovery->cleanup = NULL; clear_target(recovery);
    recovery->coordinator->runtime->requires_recovery = false;
    recovery->coordinator->application_owner = NULL;
    recovery->phase = LCS_CONFIG_RECOVERY_COMPLETE;
    recovery->admission_complete(recovery->admission_context);
    recovery->admission = NULL; recovery->admission_cancel = NULL; recovery->admission_complete = NULL;
    recovery->admission_context = NULL;
    return 1;
}

static int catalog_job(lcs_config_recovery_t *recovery, bool finish, uint64_t now_ms)
{
    lcs_config_coordinator_t *coordinator = recovery->coordinator;
    if (recovery->completion)
    {
        if (coordinator->completed_applications < recovery->completion) return 0;
        if (coordinator->completed_applications != recovery->completion || coordinator->last_application_result)
            return failure(recovery, "unexpected recovery catalog completion; recover from disk", true);
        recovery->completion = 0;
        if ((finish && (coordinator->runtime->application.pending.document ||
            !lcs_config_value_equal(&recovery->target, &coordinator->runtime->application.applied))) ||
            (!finish && !coordinator->runtime->application.pending.document))
            return failure(recovery, "recovery catalog completion does not match its target", true);
        clear_target(recovery);
        return start_cleanup(recovery);
    }
    if (coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0) return 0;
    if (finish && !recovery->quorum_only)
    {
        int proof = lcs_config_owner_poll(&recovery->network->round, now_ms);
        if (proof < 0) return failure(recovery, "old-owner proof lost before recovery FINISH", false);
        if (!proof) return 0;
    }
    lcs_config_agreement_job_t job = {.action = finish ? LCS_CONFIG_APPLICATION_FINISH : LCS_CONFIG_APPLICATION_BEGIN};
    if (finish) memcpy(job.application_digest, recovery->target.digest, sizeof(job.application_digest));
    int result = lcs_config_coordinator_start_application(coordinator, recovery, &job, recovery->error, sizeof(recovery->error));
    if (result < 0) return failure_detail(recovery, "cannot persist recovery catalog", recovery->error, true);
    if (!result) recovery->completion = coordinator->completed_applications + 1;
    return 0;
}

int lcs_config_recovery_process(lcs_config_recovery_t *recovery,
                                  const uint64_t instances[LCS_MAX_NODES], uint64_t now_ms,
                                  lcs_config_owner_send_t send, void *context)
{
    if (!recovery || !recovery->coordinator) return -1;
    lcs_config_coordinator_t *coordinator = recovery->coordinator;
    if (recovery->phase == LCS_CONFIG_RECOVERY_COMPLETE) return 1;
    if (recovery->phase == LCS_CONFIG_RECOVERY_STOPPED) return 0;
    (void)lcs_config_coordinator_process(coordinator, now_ms);
    if (recovery->phase == LCS_CONFIG_RECOVERY_STOPPING)
    {
        if (recovery->cleanup) (void)lcs_config_startup_process(recovery->cleanup, now_ms);
        if (recovery->next_cleanup) (void)lcs_config_startup_process(recovery->next_cleanup, now_ms);
        if (lcs_config_coordinator_can_close(coordinator) && (!recovery->cleanup || recovery->cleanup->pid <= 0) &&
            (!recovery->next_cleanup || recovery->next_cleanup->pid <= 0)) recovery->phase = LCS_CONFIG_RECOVERY_STOPPED;
        return 0;
    }
    if (recovery->phase == LCS_CONFIG_RECOVERY_FAILED) return -1;
    if (!instances || !send || instances[recovery->network->self] != recovery->network->instance) return failure(recovery, "recovery requires admitted transport observations", true);
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    if (coordinator->failed || coordinator->closing || coordinator->application_owner != recovery || !runtime->requires_recovery ||
        !lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application))
        return failure(recovery, "recovery authority or writer changed; retain fences and recover from disk", true);
    lcs_config_owner_network_process(recovery->network, instances, now_ms, send, context);
    if (recovery->phase == LCS_CONFIG_RECOVERY_FAILED || recovery->phase == LCS_CONFIG_RECOVERY_BLOCKED) return -1;
    if (recovery->phase == LCS_CONFIG_RECOVERY_BEGIN || recovery->phase == LCS_CONFIG_RECOVERY_FINISH)
        return catalog_job(recovery, recovery->phase == LCS_CONFIG_RECOVERY_FINISH, now_ms);
    if (recovery->phase == LCS_CONFIG_RECOVERY_CLEANUP)
    {
        int result = lcs_config_startup_process(recovery->next_cleanup, now_ms);
        if (result < 0) return failure_detail(recovery, "physical recovery cleanup failed", recovery->next_cleanup->error, false);
        if (!lcs_config_startup_verified(recovery->next_cleanup, runtime, recovery->network->instance)) return 0;
        if (!recovery->quorum_only)
        {
            if (recovery->network->recovery)
                result = lcs_config_startup_refresh_owner_history(recovery->next_cleanup, recovery->network);
            else
            {
                result = runtime->application.applied.revision ?
                    lcs_config_startup_restore_completed(recovery->next_cleanup, recovery->network) : 0;
                if (!result) result = lcs_config_startup_bind_owner_history(recovery->next_cleanup, recovery->network);
            }
            if (result) return failure(recovery, "recovery scope history unavailable; retain cleanup evidence", true);
        }
        if (recovery->cleanup)
        {
            lcs_config_startup_stop(recovery->cleanup);
            if (lcs_config_startup_close(recovery->cleanup)) return failure(recovery, "old recovery worker still exists", true);
            free(recovery->cleanup);
        }
        recovery->cleanup = recovery->next_cleanup; recovery->next_cleanup = NULL;
        recovery->phase = LCS_CONFIG_RECOVERY_RETIRE;
        return 0;
    }
    if (!lcs_config_startup_verified(recovery->cleanup, runtime, recovery->network->instance))
        return failure(recovery, "catalog changed outside the recovery sequencer", true);
    if (recovery->phase == LCS_CONFIG_RECOVERY_ADMITTING) return progress_admission(recovery);
    if (!recovery->quorum_only && recovery->network->receipt.failed)
        return failure(recovery, "local recovery cleanup proof revoked; retain fences", true);
    if (recovery->phase == LCS_CONFIG_RECOVERY_RETIRE)
    {
        if (!recovery->quorum_only && recovery->network->receipt.sealed)
        {
            if (recovery->network->receipt.failed || recovery->network->release.failed)
                return failure(recovery, "completed recovery scope needs an explicit repaired retry", false);
            return 0;
        }
        return resolve(recovery);
    }
    if (recovery->phase == LCS_CONFIG_RECOVERY_OWNERS)
    {
        if (!recovery->quorum_only)
        {
            if (!recovery->network->round.active && lcs_config_startup_restore_pending(recovery->cleanup, recovery->network, now_ms))
                return failure(recovery, "cannot start exact pending recovery owner scope", true);
            int result = lcs_config_owner_poll(&recovery->network->round, now_ms);
            if (result < 0) return failure(recovery, "pending recovery stop collection failed", false);
            if (!result) return 0;
        }
        return capture(recovery, false);
    }
    if (recovery->phase == LCS_CONFIG_RECOVERY_INSTALL)
    {
        if (!recovery->current_install && !recovery->quorum_only)
        {
            int proof = lcs_config_owner_poll(&recovery->network->round, now_ms);
            if (proof < 0) return failure(recovery, "old-owner proof lost before recovery installation", false);
            if (!proof) return 0;
        }
        lcs_config_reconcile_view_t view = {.old_config = &recovery->configs[0], .target = &recovery->configs[1],
            .plan = &recovery->plan, .value = &recovery->target, .attempt = recovery->attempt};
        int result = recovery->install(recovery->context, &view, recovery->error, sizeof(recovery->error));
        if (result < 0) return failure_detail(recovery, "recovery table installation failed", recovery->error, false);
        if (!result) return 0;
        if (!lcs_config_startup_verified(recovery->cleanup, runtime, recovery->network->instance) ||
            coordinator->application_owner != recovery) return failure(recovery, "installation changed recovery authority", true);
        memcpy(recovery->installed, recovery->target.digest, sizeof(recovery->installed));
        if (!recovery->current_install) { recovery->phase = LCS_CONFIG_RECOVERY_FINISH; return 0; }
        clear_target(recovery);
        return resolve(recovery);
    }
    return resolve(recovery); /* READY can become behind while remaining fenced. */
}

int lcs_config_recovery_retry(lcs_config_recovery_t *recovery, uint64_t now_ms)
{
    if (!recovery || recovery->phase != LCS_CONFIG_RECOVERY_BLOCKED || recovery->coordinator->failed || recovery->coordinator->closing)
        return -1;
    lcs_config_recovery_phase_t phase = recovery->blocked_phase;
    if (phase == LCS_CONFIG_RECOVERY_CLEANUP)
    { if (lcs_config_startup_retry(recovery->next_cleanup)) return -1; }
    else if (!recovery->quorum_only && (phase == LCS_CONFIG_RECOVERY_RETIRE || phase == LCS_CONFIG_RECOVERY_OWNERS ||
        ((phase == LCS_CONFIG_RECOVERY_INSTALL || phase == LCS_CONFIG_RECOVERY_FINISH) && !recovery->current_install)))
    {
        if (lcs_config_owner_network_retry(recovery->network, now_ms) &&
            (phase != LCS_CONFIG_RECOVERY_INSTALL || lcs_config_owner_poll(&recovery->network->round, now_ms) != 1)) return -1;
        if (phase == LCS_CONFIG_RECOVERY_FINISH) phase = LCS_CONFIG_RECOVERY_INSTALL;
    }
    recovery->phase = phase; recovery->error[0] = 0;
    return 0;
}

void lcs_config_recovery_stop(lcs_config_recovery_t *recovery)
{
    if (!recovery || !recovery->coordinator || recovery->phase == LCS_CONFIG_RECOVERY_COMPLETE) return;
    cancel_admission(recovery);
    lcs_config_owner_network_stop(recovery->network);
    if (recovery->cleanup) lcs_config_startup_stop(recovery->cleanup);
    if (recovery->next_cleanup) lcs_config_startup_stop(recovery->next_cleanup);
    lcs_config_coordinator_stop(recovery->coordinator);
    recovery->phase = LCS_CONFIG_RECOVERY_STOPPING;
}

int lcs_config_recovery_close(lcs_config_recovery_t *recovery)
{
    if (recovery && recovery->phase == LCS_CONFIG_RECOVERY_COMPLETE)
    { memset(recovery, 0, sizeof(*recovery)); return 0; }
    if (!recovery || recovery->phase != LCS_CONFIG_RECOVERY_STOPPED || !lcs_config_coordinator_can_close(recovery->coordinator)) return -1;
    if ((recovery->cleanup && lcs_config_startup_close(recovery->cleanup)) ||
        (recovery->next_cleanup && lcs_config_startup_close(recovery->next_cleanup))) return -1;
    free(recovery->cleanup); free(recovery->next_cleanup); clear_target(recovery);
    if (recovery->coordinator->application_owner == recovery) recovery->coordinator->application_owner = NULL;
    memset(recovery, 0, sizeof(*recovery));
    return 0;
}
