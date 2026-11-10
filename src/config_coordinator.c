// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_coordinator.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lcs_config_coordinator_init(lcs_config_coordinator_t *coordinator,
                                  lcs_config_runtime_t *runtime,
                                  lcs_config_readiness_t *readiness)
{
    memset(coordinator, 0, sizeof(*coordinator));
    lcs_config_agreement_worker_init(&coordinator->storage);
    coordinator->runtime = runtime;
    coordinator->readiness = readiness;
    coordinator->initialized = true;
}

bool lcs_config_coordinator_can_close(const lcs_config_coordinator_t *coordinator)
{
    return !coordinator->initialized || (coordinator->closing && coordinator->storage.pid <= 0);
}

bool lcs_config_coordinator_can_vote(const lcs_config_coordinator_t *coordinator)
{
    if (!coordinator->initialized || coordinator->closing || coordinator->failed || coordinator->application_owner ||
        coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0)
        return false;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    return lcs_config_barrier_can_vote(&runtime->barrier, &runtime->activation,
                                         &runtime->agreement, &runtime->application);
}

static bool current_attempt(const lcs_config_coordinator_t *coordinator)
{
    const lcs_config_readiness_t *readiness = coordinator->readiness;
    return readiness->active && !readiness->round.failed &&
        readiness->query.requester_instance == coordinator->requester_instance &&
        readiness->query.attempt == coordinator->attempt &&
        !strcmp(readiness->query.membership, coordinator->persisted_round.membership) &&
        !strcmp(readiness->query.baseline, coordinator->persisted_round.baseline) &&
        !strcmp(readiness->round.membership, coordinator->persisted_round.membership) &&
        !strcmp(readiness->round.baseline, coordinator->persisted_round.baseline) &&
        readiness->round.required == coordinator->persisted_round.required;
}

void lcs_config_coordinator_stop(lcs_config_coordinator_t *coordinator)
{
    if (!coordinator->initialized)
        return;
    coordinator->closing = true;
    coordinator->readiness->round.failed = true;
    if (coordinator->storage.pid > 0)
        lcs_config_agreement_worker_cancel(&coordinator->storage, &coordinator->runtime->agreement);
    coordinator->phase = coordinator->storage.pid > 0 ? LCS_CONFIG_COORD_CANCELLING :
        coordinator->failed ? LCS_CONFIG_COORD_FAILED : LCS_CONFIG_COORD_STOPPED;
}

void lcs_config_coordinator_abort(lcs_config_coordinator_t *coordinator, const char *reason)
{
    if (!coordinator->initialized)
        return;
    char error[sizeof(coordinator->error)] = {0};
    snprintf(error, sizeof(error), "%s", reason);
    coordinator->failed = true;
    memcpy(coordinator->error, error, sizeof(error));
    lcs_config_coordinator_stop(coordinator);
}

int lcs_config_coordinator_start(lcs_config_coordinator_t *coordinator)
{
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->runtime->barrier.store || coordinator->runtime->barrier.poisoned)
        return -1;
    if (coordinator->storage.pid > 0 || coordinator->phase != LCS_CONFIG_COORD_IDLE)
        return 1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    lcs_config_readiness_t *readiness = coordinator->readiness;
    if (!readiness->active || readiness->round.failed ||
        !lcs_config_barrier_local_ready(&runtime->activation, &runtime->agreement, &runtime->application) ||
        strcmp(readiness->round.membership, runtime->activation.membership) ||
        strcmp(readiness->round.baseline, runtime->activation.baseline.digest))
        return -1;
    coordinator->requester_instance = readiness->query.requester_instance;
    coordinator->attempt = readiness->query.attempt;
    coordinator->persisted_round = readiness->round;
    coordinator->phase = LCS_CONFIG_COORD_COLLECTING;
    return 0;
}

int lcs_config_coordinator_recover(lcs_config_coordinator_t *coordinator)
{
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->storage.pid > 0 || coordinator->phase != LCS_CONFIG_COORD_IDLE)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation,
                                   &runtime->agreement, &runtime->application))
        return -1;
    coordinator->phase = LCS_CONFIG_COORD_READY;
    return 0;
}

static int dynamic_successor(lcs_config_runtime_t *runtime, const lcs_config_value_t *value,
                               char *err, size_t err_len)
{
    lcs_config_t *configs = malloc(2 * sizeof(*configs));
    uint64_t old_revision, new_revision;
    int result = -1;
    if (configs && value && value->document &&
        !lcs_resource_config_decode(runtime->agreement.committed.document, runtime->agreement.committed.length,
                                     runtime->base, &configs[0], &old_revision, err, err_len) &&
        !lcs_resource_config_decode(value->document, value->length, runtime->base,
                                     &configs[1], &new_revision, err, err_len) &&
        configs[1].resource_schema_version == LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION)
        result = lcs_resource_config_validate_successor(&configs[0], old_revision, &configs[1], new_revision, err, err_len);
    free(configs);
    if (result)
    {
        if (err && err_len)
            snprintf(err, err_len, "configuration acceptance requires a valid dynamic successor; activation preserves definitions");
        return -1;
    }
    return 0;
}

int lcs_config_coordinator_start_vote(lcs_config_coordinator_t *coordinator,
                                       const lcs_config_agreement_job_t *job,
                                       char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!job || !lcs_config_coordinator_can_vote(coordinator) || coordinator->completed_votes == UINT64_MAX ||
        (job->action != LCS_CONFIG_PREPARE && job->action != LCS_CONFIG_ACCEPT))
    {
        if (err && err_len)
            snprintf(err, err_len, "configuration vote requires durable readiness and applied configuration");
        return -1;
    }
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (job->action == LCS_CONFIG_ACCEPT && dynamic_successor(runtime, job->value, err, err_len))
        return -1;
    int result = lcs_config_agreement_worker_start(&coordinator->storage, &runtime->agreement, job, err, err_len);
    if (!result)
        coordinator->phase = LCS_CONFIG_COORD_VOTING;
    return result;
}

int lcs_config_coordinator_start_learn(lcs_config_coordinator_t *coordinator,
                                       const lcs_config_agreement_job_t *job,
                                       char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || !job || job->action != LCS_CONFIG_LEARN ||
        !job->value || !job->value->revision || coordinator->completed_votes == UINT64_MAX)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        !lcs_config_agreement_has_quorum(&runtime->agreement, job->voters))
        return -1;
    bool duplicate = lcs_config_value_equal(job->value, &runtime->agreement.committed) &&
        !strcmp(job->parent_digest, runtime->agreement.parent_digest);
    if (!duplicate && (!lcs_config_coordinator_can_vote(coordinator) ||
        dynamic_successor(runtime, job->value, err, err_len)))
        return -1;
    int result = lcs_config_agreement_worker_start(&coordinator->storage, &runtime->agreement, job, err, err_len);
    if (!result)
        coordinator->phase = LCS_CONFIG_COORD_VOTING;
    return result;
}

int lcs_config_coordinator_start_checkpoint(lcs_config_coordinator_t *coordinator,
                                            const lcs_config_agreement_job_t *job,
                                            char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || !job || job->action != LCS_CONFIG_CHECKPOINT ||
        coordinator->completed_votes == UINT64_MAX)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        lcs_config_agreement_check_checkpoint(&runtime->agreement, job->parent_digest, job->ballot,
                                                job->voters, job->value, err, err_len))
        return -1;
    int result = lcs_config_agreement_worker_start(&coordinator->storage, &runtime->agreement, job, err, err_len);
    if (!result)
        coordinator->phase = LCS_CONFIG_COORD_VOTING;
    return result;
}

int lcs_config_coordinator_start_application(lcs_config_coordinator_t *coordinator,
                                             const void *owner, const lcs_config_agreement_job_t *job,
                                             char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!owner || coordinator->application_owner != owner || !coordinator->initialized || coordinator->closing ||
        coordinator->failed || coordinator->phase != LCS_CONFIG_COORD_READY || !job ||
        (job->action != LCS_CONFIG_APPLICATION_BEGIN && job->action != LCS_CONFIG_APPLICATION_FINISH) ||
        coordinator->completed_applications == UINT64_MAX)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application))
        return -1;
    int result = lcs_config_agreement_worker_start_application(&coordinator->storage, &runtime->agreement,
                                                                  &runtime->application, job, err, err_len);
    if (!result)
        coordinator->phase = LCS_CONFIG_COORD_APPLYING;
    return result;
}

int lcs_config_coordinator_start_projection(lcs_config_coordinator_t *coordinator,
                                            lcs_config_bootstrap_t *bootstrap,
                                            char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->completed_projections == UINT64_MAX)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) ||
        !runtime->agreement.committed.revision)
        return -1;
    int result = lcs_config_agreement_worker_start_projection(&coordinator->storage, &runtime->agreement,
                                                                bootstrap, err, err_len);
    if (!result)
    {
        coordinator->projecting_revision = runtime->agreement.committed.revision;
        memcpy(coordinator->projecting_digest, runtime->agreement.committed.digest, sizeof(coordinator->projecting_digest));
        coordinator->phase = LCS_CONFIG_COORD_PROJECTING;
    }
    return result;
}

int lcs_config_coordinator_start_requests(lcs_config_coordinator_t *coordinator,
                                            const lcs_config_agreement_job_t *job,
                                            char *err, size_t err_len)
{
    if (coordinator->initialized && coordinator->storage.pid > 0)
        return 1;
    if (!coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || !job ||
        coordinator->completed_requests == UINT64_MAX ||
        (job->action != LCS_CONFIG_REQUEST_OPEN && job->action != LCS_CONFIG_REQUEST_RESERVE &&
         job->action != LCS_CONFIG_REQUEST_OBSERVE) || job->initialize_requests)
        return -1;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (!lcs_config_barrier_valid(&runtime->barrier, &runtime->activation,
                                    &runtime->agreement, &runtime->application))
        return -1;
    if (job->action == LCS_CONFIG_REQUEST_RESERVE)
    {
        if (!job->value || !memchr(job->value->request_id, 0, sizeof(job->value->request_id)))
            return -1;
        const lcs_config_request_record_t *known = lcs_config_requests_find(&runtime->requests, job->value->request_id);
        /* A known identity is checked by the worker without permitting another
         * proposal. New intent must pass the same successor/admission gate as ACCEPT. */
        if (!known && (!lcs_config_coordinator_can_vote(coordinator) ||
            dynamic_successor(runtime, job->value, err, err_len)))
            return -1;
    }
    int result = job->action == LCS_CONFIG_REQUEST_OPEN ?
        lcs_config_agreement_worker_open_requests(&coordinator->storage, &runtime->agreement,
                                                    &runtime->requests, false, err, err_len) :
        lcs_config_agreement_worker_start_requests(&coordinator->storage, &runtime->agreement,
                                                     &runtime->requests, job, err, err_len);
    if (!result)
        coordinator->phase = LCS_CONFIG_COORD_REQUESTING;
    return result;
}

int lcs_config_coordinator_process(lcs_config_coordinator_t *coordinator, uint64_t now_ms)
{
    if (!coordinator->initialized || coordinator->phase == LCS_CONFIG_COORD_IDLE ||
        coordinator->phase == LCS_CONFIG_COORD_STOPPED)
        return 0;
    if (coordinator->phase == LCS_CONFIG_COORD_CANCELLING)
    {
        int operation;
        (void)lcs_config_agreement_worker_poll(&coordinator->storage, &coordinator->runtime->agreement,
                                               &operation, NULL, 0);
        if (coordinator->storage.pid > 0)
            return 0;
        coordinator->phase = coordinator->failed ? LCS_CONFIG_COORD_FAILED : LCS_CONFIG_COORD_STOPPED;
        if (!coordinator->failed)
            return 0;
    }
    if (coordinator->phase == LCS_CONFIG_COORD_FAILED)
        return -1;
    if (coordinator->phase == LCS_CONFIG_COORD_READY)
        return lcs_config_coordinator_can_vote(coordinator) ? 1 : 0;
    lcs_config_runtime_t *runtime = coordinator->runtime;
    if (coordinator->phase == LCS_CONFIG_COORD_VOTING || coordinator->phase == LCS_CONFIG_COORD_APPLYING ||
        coordinator->phase == LCS_CONFIG_COORD_PROJECTING || coordinator->phase == LCS_CONFIG_COORD_REQUESTING)
    {
        bool request_job = coordinator->phase == LCS_CONFIG_COORD_REQUESTING;
        bool application_job = coordinator->phase == LCS_CONFIG_COORD_APPLYING;
        bool projection_job = coordinator->phase == LCS_CONFIG_COORD_PROJECTING;
        int operation = -1;
        int result = lcs_config_agreement_worker_poll(&coordinator->storage, &runtime->agreement,
            &operation, coordinator->error, sizeof(coordinator->error));
        if (!result)
            return 0;
        if (result < 0)
        {
            coordinator->failed = coordinator->closing = true;
            coordinator->phase = coordinator->storage.pid > 0 ? LCS_CONFIG_COORD_CANCELLING : LCS_CONFIG_COORD_FAILED;
            return coordinator->storage.pid > 0 ? 0 : -1;
        }
        if (request_job)
        {
            coordinator->last_request_result = operation;
            coordinator->completed_requests++;
        }
        else if (projection_job)
        {
            coordinator->last_projection_result = operation;
            coordinator->completed_projections++;
            if (!operation)
            {
                coordinator->projected_revision = coordinator->projecting_revision;
                memcpy(coordinator->projected_digest, coordinator->projecting_digest, sizeof(coordinator->projected_digest));
            }
            else
            {
                coordinator->projected_revision = 0;
                memset(coordinator->projected_digest, 0, sizeof(coordinator->projected_digest));
            }
        }
        else if (application_job)
        {
            coordinator->last_application_result = operation;
            coordinator->completed_applications++;
        }
        else
        {
            coordinator->last_vote_result = operation;
            coordinator->completed_votes++;
        }
        coordinator->phase = LCS_CONFIG_COORD_READY;
        return lcs_config_coordinator_can_vote(coordinator) ? 1 : 0;
    }
    if (!current_attempt(coordinator) ||
        !lcs_config_barrier_local_ready(&runtime->activation, &runtime->agreement, &runtime->application))
    {
        lcs_config_coordinator_abort(coordinator, "readiness attempt or local initialized state changed; recover before retry");
        return coordinator->storage.pid > 0 ? 0 : -1;
    }
    if (coordinator->phase == LCS_CONFIG_COORD_COLLECTING)
    {
        int ready = lcs_config_readiness_poll(coordinator->readiness, now_ms);
        if (ready < 0)
        {
            lcs_config_coordinator_abort(coordinator, "readiness collection failed or expired");
            return -1;
        }
        if (!ready || coordinator->storage.pid > 0)
            return 0;
        coordinator->persisted_round = coordinator->readiness->round;
        int result = lcs_config_agreement_worker_start_barrier(&coordinator->storage, runtime,
            &coordinator->persisted_round, coordinator->error, sizeof(coordinator->error));
        if (result < 0)
        {
            char error[sizeof(coordinator->error)];
            memcpy(error, coordinator->error, sizeof(error));
            lcs_config_coordinator_abort(coordinator, error);
            return -1;
        }
        if (result)
            return 0;
        coordinator->phase = LCS_CONFIG_COORD_PERSISTING;
        return 0;
    }
    if (!coordinator->readiness->round.sealed ||
        coordinator->readiness->round.ready != coordinator->persisted_round.ready)
    {
        lcs_config_coordinator_abort(coordinator, "sealed readiness changed during persistence; disk recovery required");
        return coordinator->storage.pid > 0 ? 0 : -1;
    }
    int operation = -1;
    int result = lcs_config_agreement_worker_poll(&coordinator->storage, &runtime->agreement,
                                                  &operation, coordinator->error, sizeof(coordinator->error));
    if (!result)
        return 0;
    if (result < 0 || operation ||
        !lcs_config_barrier_can_vote(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application))
    {
        coordinator->failed = true;
        coordinator->closing = true;
        coordinator->readiness->round.failed = true;
        coordinator->phase = coordinator->storage.pid > 0 ? LCS_CONFIG_COORD_CANCELLING : LCS_CONFIG_COORD_FAILED;
        if (!*coordinator->error)
            snprintf(coordinator->error, sizeof(coordinator->error), "barrier persistence failed; disk recovery required");
        return coordinator->storage.pid > 0 ? 0 : -1;
    }
    coordinator->phase = LCS_CONFIG_COORD_READY;
    return 1;
}
