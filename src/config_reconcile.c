// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_reconcile.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool healthy(const lcs_config_reconcile_t *reconcile)
{
    const lcs_config_coordinator_t *coordinator = reconcile->coordinator;
    if (!coordinator || !coordinator->initialized || coordinator->failed || coordinator->closing)
        return false;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    return lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application);
}

static void clear_target(lcs_config_reconcile_t *reconcile)
{
    free(reconcile->configs);
    reconcile->configs = NULL;
    lcs_config_value_clear(&reconcile->value);
    memset(&reconcile->plan, 0, sizeof(reconcile->plan));
    reconcile->completion = 0;
    reconcile->admission_completed = false;
}

static int recovery(lcs_config_reconcile_t *reconcile, const char *message)
{
    if (reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT && reconcile->ops.cancel_admission)
        reconcile->ops.cancel_admission(reconcile->context);
    reconcile->phase = LCS_CONFIG_RECONCILE_RECOVERY;
    snprintf(reconcile->error, sizeof(reconcile->error), "%s", message);
    return -1;
}

void lcs_config_reconcile_init(lcs_config_reconcile_t *reconcile, lcs_config_coordinator_t *coordinator,
                                 const lcs_config_reconcile_ops_t *ops, void *context)
{
    memset(reconcile, 0, sizeof(*reconcile));
    reconcile->coordinator = coordinator;
    if (ops)
        reconcile->ops = *ops;
    reconcile->context = context;
}

int lcs_config_reconcile_start(lcs_config_reconcile_t *reconcile)
{
    lcs_config_coordinator_t *coordinator = reconcile->coordinator;
    if (!healthy(reconcile) || coordinator->application_owner || coordinator->phase != LCS_CONFIG_COORD_READY ||
        coordinator->storage.pid > 0 || reconcile->attempt == UINT64_MAX ||
        (reconcile->phase != LCS_CONFIG_RECONCILE_IDLE && reconcile->phase != LCS_CONFIG_RECONCILE_DONE))
        return -1;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    int self = lcs_config_self_index(runtime->base);
    if (self < 0)
        return -1;
    reconcile->quorum_only = runtime->base->nodes[self].role == LCS_NODE_QUORUM_ONLY;
    if (!reconcile->ops.install || !reconcile->ops.admit ||
        (!!reconcile->ops.cancel_admission != !!reconcile->ops.complete_admission) || (!reconcile->quorum_only &&
        (!reconcile->ops.quiesce || !reconcile->ops.drain || !reconcile->ops.owners)))
        return -1;
    clear_target(reconcile);
    reconcile->error[0] = '\0';
    if (!runtime->application.pending.document &&
        lcs_config_value_equal(&runtime->application.applied, &runtime->agreement.committed))
    {
        reconcile->phase = LCS_CONFIG_RECONCILE_DONE;
        return 0;
    }
    coordinator->application_owner = reconcile;
    reconcile->phase = LCS_CONFIG_RECONCILE_BEGIN;
    return 0;
}

static int capture(lcs_config_reconcile_t *reconcile)
{
    lcs_config_application_t *application = &reconcile->coordinator->runtime->application;
    if (!application->pending.document || reconcile->attempt == UINT64_MAX)
        return recovery(reconcile, "application target missing or attempt counter exhausted; recovery required");
    reconcile->configs = malloc(2 * sizeof(*reconcile->configs));
    if (!reconcile->configs || lcs_config_value_copy(&reconcile->value, &application->pending))
        return recovery(reconcile, "out of memory retaining immutable application plan; recovery required");
    if (lcs_config_application_plan(application, &reconcile->configs[0], &reconcile->configs[1],
                                    &reconcile->plan, reconcile->error, sizeof(reconcile->error)))
    {
        reconcile->phase = LCS_CONFIG_RECONCILE_RECOVERY;
        return -1;
    }
    reconcile->attempt++;
    reconcile->completion = 0;
    reconcile->phase = reconcile->quorum_only ? LCS_CONFIG_RECONCILE_INSTALL : LCS_CONFIG_RECONCILE_QUIESCE;
    return 0;
}

static int catalog_job(lcs_config_reconcile_t *reconcile, bool finish)
{
    lcs_config_coordinator_t *coordinator = reconcile->coordinator;
    if (reconcile->completion)
    {
        if (coordinator->completed_applications < reconcile->completion)
            return 0;
        if (coordinator->completed_applications != reconcile->completion || coordinator->last_application_result)
            return recovery(reconcile, "unexpected application writer completion; recover durable catalog");
        reconcile->completion = 0;
        if (!finish)
            return capture(reconcile);
        if (coordinator->runtime->application.pending.document ||
            !lcs_config_value_equal(&reconcile->value, &coordinator->runtime->application.applied))
            return recovery(reconcile, "applied catalog differs from installed target; recovery required");
        reconcile->phase = LCS_CONFIG_RECONCILE_ADMIT;
        return 0;
    }
    if (coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0)
        return 0;
    if (!finish && coordinator->runtime->application.pending.document)
        return capture(reconcile); /* Resume the existing target, even if learning advanced. */
    lcs_config_agreement_job_t job = { .action = finish ? LCS_CONFIG_APPLICATION_FINISH : LCS_CONFIG_APPLICATION_BEGIN };
    if (finish)
        memcpy(job.application_digest, reconcile->value.digest, sizeof(job.application_digest));
    int result = lcs_config_coordinator_start_application(coordinator, reconcile, &job,
                                                           reconcile->error, sizeof(reconcile->error));
    if (result < 0)
    {
        reconcile->phase = LCS_CONFIG_RECONCILE_RECOVERY;
        return -1;
    }
    if (!result)
        reconcile->completion = coordinator->completed_applications + 1;
    return 0;
}

int lcs_config_reconcile_process(lcs_config_reconcile_t *reconcile)
{
    if (reconcile->phase == LCS_CONFIG_RECONCILE_IDLE)
        return 0;
    if (reconcile->phase == LCS_CONFIG_RECONCILE_DONE)
        return 1;
    if (reconcile->phase >= LCS_CONFIG_RECONCILE_BLOCKED)
        return -1;
    if (!healthy(reconcile) || reconcile->coordinator->application_owner != reconcile)
        return recovery(reconcile, "application coordinator/storage lost eligibility; recovery required");
    lcs_config_coordinator_t *coordinator = reconcile->coordinator;
    if (reconcile->phase == LCS_CONFIG_RECONCILE_BEGIN || reconcile->phase == LCS_CONFIG_RECONCILE_FINISH)
        return catalog_job(reconcile, reconcile->phase == LCS_CONFIG_RECONCILE_FINISH);
    const lcs_config_application_t *application = &coordinator->runtime->application;
    if (reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT &&
        (coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0))
        return 0;
    if (reconcile->phase != LCS_CONFIG_RECONCILE_ADMIT &&
        !lcs_config_value_equal(&application->pending, &reconcile->value))
        return recovery(reconcile, "pending cleanup target changed during reconciliation; recovery required");
    if (reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT &&
        !lcs_config_value_equal(&application->applied, &coordinator->runtime->agreement.committed))
    {
        if (reconcile->ops.cancel_admission) reconcile->ops.cancel_admission(reconcile->context);
        clear_target(reconcile);
        reconcile->phase = LCS_CONFIG_RECONCILE_BEGIN;
        return 0; /* Keep grants fenced until the latest learned snapshot is installed. */
    }
    lcs_config_reconcile_view_t view = { .old_config = &reconcile->configs[0], .target = &reconcile->configs[1],
        .plan = &reconcile->plan, .value = &reconcile->value, .attempt = reconcile->attempt };
    lcs_config_reconcile_step_t step = reconcile->phase == LCS_CONFIG_RECONCILE_QUIESCE ? reconcile->ops.quiesce :
        reconcile->phase == LCS_CONFIG_RECONCILE_DRAIN ? reconcile->ops.drain :
        reconcile->phase == LCS_CONFIG_RECONCILE_OWNERS ? reconcile->ops.owners :
        reconcile->phase == LCS_CONFIG_RECONCILE_INSTALL ? reconcile->ops.install : reconcile->ops.admit;
    int result = step(reconcile->context, &view, reconcile->error, sizeof(reconcile->error));
    if (result < 0)
    {
        if (reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT && reconcile->ops.cancel_admission)
            reconcile->ops.cancel_admission(reconcile->context);
        if (!reconcile->error[0])
            snprintf(reconcile->error, sizeof(reconcile->error), "application adapter blocked; repair the barrier before retrying");
        reconcile->blocked_phase = reconcile->phase;
        reconcile->phase = LCS_CONFIG_RECONCILE_BLOCKED;
        return -1;
    }
    if (!result)
        return 0;
    reconcile->error[0] = '\0';
    if (reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT)
    {
        if (coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0 ||
            !lcs_config_value_equal(&application->applied, &coordinator->runtime->agreement.committed))
            return recovery(reconcile, "configuration changed during admission; keep resource activity fenced");
        coordinator->application_owner = NULL;
        reconcile->phase = LCS_CONFIG_RECONCILE_DONE;
        if (reconcile->ops.complete_admission) reconcile->ops.complete_admission(reconcile->context);
        reconcile->admission_completed = true;
        return 1;
    }
    reconcile->phase++;
    return 0;
}

int lcs_config_reconcile_admission_receipt(const lcs_config_reconcile_t *reconcile,
                                            lcs_config_request_admission_t *receipt)
{
    if (!receipt || !healthy(reconcile) || reconcile->phase != LCS_CONFIG_RECONCILE_DONE ||
        !reconcile->admission_completed || !reconcile->attempt || !reconcile->value.revision ||
        reconcile->coordinator->application_owner || reconcile->coordinator->phase != LCS_CONFIG_COORD_READY ||
        reconcile->coordinator->storage.pid > 0 || reconcile->coordinator->runtime->application.pending.document ||
        !lcs_config_value_equal(&reconcile->value, &reconcile->coordinator->runtime->application.applied) ||
        !lcs_config_value_equal(&reconcile->value, &reconcile->coordinator->runtime->agreement.committed)) return -1;
    *receipt = (lcs_config_request_admission_t){.issuer = reconcile, .attempt = reconcile->attempt,
        .revision = reconcile->value.revision};
    memcpy(receipt->digest, reconcile->value.digest, sizeof(receipt->digest));
    return 0;
}

int lcs_config_reconcile_retry(lcs_config_reconcile_t *reconcile)
{
    if (reconcile->phase != LCS_CONFIG_RECONCILE_BLOCKED || !healthy(reconcile) ||
        reconcile->coordinator->application_owner != reconcile)
        return -1;
    reconcile->phase = reconcile->blocked_phase;
    return 0;
}

void lcs_config_reconcile_stop(lcs_config_reconcile_t *reconcile)
{
    if ((reconcile->phase == LCS_CONFIG_RECONCILE_ADMIT ||
        (reconcile->phase == LCS_CONFIG_RECONCILE_BLOCKED && reconcile->blocked_phase == LCS_CONFIG_RECONCILE_ADMIT)) &&
        reconcile->ops.cancel_admission) reconcile->ops.cancel_admission(reconcile->context);
    reconcile->phase = LCS_CONFIG_RECONCILE_STOPPED;
    if (reconcile->coordinator)
        lcs_config_coordinator_stop(reconcile->coordinator);
}

int lcs_config_reconcile_close(lcs_config_reconcile_t *reconcile)
{
    lcs_config_coordinator_t *coordinator = reconcile->coordinator;
    if (coordinator && (coordinator->storage.pid > 0 ||
        (coordinator->application_owner == reconcile && !lcs_config_coordinator_can_close(coordinator))))
        return -1;
    if (coordinator && coordinator->application_owner == reconcile)
        coordinator->application_owner = NULL;
    clear_target(reconcile);
    memset(reconcile, 0, sizeof(*reconcile));
    return 0;
}
