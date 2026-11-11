// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_agreement_worker.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/wait.h>
#include <unistd.h>

/* Private same-host pipe format, never a network message. */
typedef struct
{
    int result;
    uint32_t length;
    bool poisoned;
    char error[256];
} worker_result_t;

#define RECORD_MAX ((LCS_CONFIG_APPLICATION_MAX > LCS_CONFIG_JOURNAL_MAX) ? \
                    LCS_CONFIG_APPLICATION_MAX : LCS_CONFIG_JOURNAL_MAX)
#define RESULT_MAX (sizeof(worker_result_t) + RECORD_MAX)

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration storage worker: %s", message);
    return -1;
}

void lcs_config_agreement_worker_init(lcs_config_agreement_worker_t *worker)
{
    memset(worker, 0, sizeof(*worker));
    worker->pipe_fd = -1;
}

static void reset(lcs_config_agreement_worker_t *worker)
{
    if (worker->pipe_fd >= 0)
        close(worker->pipe_fd);
    free(worker->buffer);
    if (worker->requests_target)
        free(worker->requests);
    if (worker->initialization_candidate)
    {
        lcs_config_runtime_close(worker->initialization_candidate);
        free(worker->initialization_candidate);
    }
    lcs_config_agreement_worker_init(worker);
}

static int write_all(int fd, const void *data, size_t length)
{
    const unsigned char *bytes = data;
    while (length)
    {
        ssize_t written = write(fd, bytes, length);
        if (written < 0 && errno == EINTR)
            continue;
        if (written <= 0)
            return -1;
        bytes += written;
        length -= (size_t)written;
    }
    return 0;
}

static int compare_fd(const void *a, const void *b)
{
    int left = *(const int *)a, right = *(const int *)b;
    return (left > right) - (left < right);
}

static _Noreturn void execute_child(lcs_config_agreement_t *agreement,
                            lcs_config_application_t *application,
                    lcs_config_requests_t *requests,
                            const lcs_config_agreement_job_t *job,
                            const lcs_config_runtime_t *initialization, bool allow_initialize,
                            const lcs_config_runtime_t *barrier_runtime,
                            int pipe_fd, pid_t parent)
{
    int keep[] = {agreement->store->directory_fd, agreement->store->lock_fd, pipe_fd,
        job && job->action == LCS_CONFIG_PROJECTION && job->projection ? job->projection->snapshots.directory_fd : -1,
        job && job->action == LCS_CONFIG_PROJECTION && job->projection ? job->projection->snapshots.lock_fd : -1};
    const size_t keep_count = sizeof(keep) / sizeof(keep[0]);
    qsort(keep, keep_count, sizeof(keep[0]), compare_fd);
    unsigned first = 3;
    for (size_t i = 0; i < keep_count; i++)
    {
        if (keep[i] < 3 || (unsigned)keep[i] < first)
            continue;
        if (first < (unsigned)keep[i])
            (void)close_range(first, (unsigned)keep[i] - 1, 0);
        first = (unsigned)keep[i] + 1;
    }
    (void)close_range(first, ~0u, 0);
    signal(SIGTERM, SIG_DFL);
    signal(SIGINT, SIG_DFL);
    signal(SIGHUP, SIG_DFL);
    signal(SIGPIPE, SIG_DFL);
    if (prctl(PR_SET_PDEATHSIG, SIGKILL) || getppid() != parent)
        _exit(1);
    worker_result_t result = {0};
    if (barrier_runtime)
    {
        lcs_config_runtime_t recovered = {0};
        lcs_config_barrier_t barrier = {0};
        if (lcs_config_runtime_open(&recovered, agreement->store, barrier_runtime->base,
                                      result.error, sizeof(result.error)) ||
            !lcs_config_barrier_local_ready(&recovered.activation, &recovered.agreement, &recovered.application) ||
            !lcs_config_value_equal(&recovered.activation.baseline, &barrier_runtime->activation.baseline) ||
            lcs_config_barrier_open(&barrier, &recovered.activation, &recovered.agreement,
                &recovered.application, job->barrier_round, result.error, sizeof(result.error)))
        {
            result.result = -1;
            result.poisoned = true;
            if (!*result.error)
                snprintf(result.error, sizeof(result.error), "barrier requires matching pristine disk state; recover before voting");
        }
        lcs_config_runtime_close(&recovered);
        _exit(write_all(pipe_fd, &result, sizeof(result)) ? 1 : 0);
    }
    if (initialization)
    {
        if (initialization->activation.phase == LCS_CONFIG_ACTIVATION_FROZEN)
        {
            lcs_config_runtime_t recovered = {0};
            recovered.base = malloc(sizeof(*recovered.base));
            if (!recovered.base || lcs_config_activation_freeze(&recovered.activation, agreement->store,
                initialization->base, recovered.base, result.error, sizeof(result.error)) ||
                recovered.activation.phase != LCS_CONFIG_ACTIVATION_FROZEN ||
                !lcs_config_value_equal(&recovered.activation.baseline, &initialization->activation.baseline))
            {
                result.result = -1; result.poisoned = true;
                if (!*result.error) snprintf(result.error, sizeof(result.error), "durable freeze differs from authorized effective definitions");
            }
            lcs_config_runtime_close(&recovered);
            _exit(write_all(pipe_fd, &result, sizeof(result)) ? 1 : 0);
        }
        /* This acknowledgement is same-host evidence only. The parent already
         * owns an immutable copy of exactly this pristine baseline. Never use
         * the compact acknowledgement to import promises or active history. */
        lcs_config_runtime_t recovered = {0};
        recovered.base = malloc(sizeof(*recovered.base));
        char *barrier_record = NULL;
        size_t barrier_length;
        int missing_barrier = lcs_config_store_read(agreement->store, "barrier.json",
            &barrier_record, &barrier_length, LCS_CONFIG_BARRIER_MAX, result.error, sizeof(result.error));
        free(barrier_record);
        char *request_record = NULL;
        size_t request_length;
        int missing_requests = lcs_config_store_read(agreement->store, "requests.json",
            &request_record, &request_length, LCS_CONFIG_REQUEST_RECORD_MAX, result.error, sizeof(result.error));
        free(request_record);
        if (!recovered.base || missing_barrier != 1 || missing_requests < 0 ||
            lcs_config_activation_open_requests(&recovered.activation, agreement->store,
                initialization->base, recovered.base, allow_initialize,
                result.error, sizeof(result.error)) ||
            !lcs_config_value_equal(&recovered.activation.baseline, &initialization->activation.baseline) ||
            lcs_config_activation_resume(&recovered.activation, &recovered.agreement,
                &recovered.application, result.error, sizeof(result.error)) ||
            recovered.activation.phase != LCS_CONFIG_ACTIVATION_PREPARED || !recovered.activation.requests_required ||
            lcs_config_requests_recover(&recovered.requests, &recovered.agreement, false, result.error, sizeof(result.error)) ||
            recovered.requests.count ||
            recovered.agreement.promised || recovered.agreement.accepted.document ||
            !lcs_config_value_equal(&recovered.agreement.committed, &initialization->activation.baseline) ||
            recovered.application.pending.document ||
            !lcs_config_value_equal(&recovered.application.applied, &initialization->activation.baseline))
        {
            result.result = -1;
            result.poisoned = true;
            if (!*result.error)
                snprintf(result.error, sizeof(result.error), "initialization differs from authorized pristine baseline; recover from disk");
        }
        lcs_config_runtime_close(&recovered);
        _exit(write_all(pipe_fd, &result, sizeof(result)) ? 1 : 0);
    }
    if (job->action == LCS_CONFIG_REQUEST_OPEN)
    {
        lcs_config_agreement_t disk = {0};
        bool same = !lcs_config_agreement_open(&disk, agreement->store, agreement->base, false,
                                                 result.error, sizeof(result.error)) &&
            lcs_config_value_equal(&disk.committed, &agreement->committed) &&
            disk.promised == agreement->promised && disk.accepted_ballot == agreement->accepted_ballot &&
            ((!disk.accepted.document && !agreement->accepted.document) ||
             lcs_config_value_equal(&disk.accepted, &agreement->accepted)) &&
            disk.committed_ballot == agreement->committed_ballot &&
            disk.committed_voters == agreement->committed_voters &&
            !strcmp(disk.parent_digest, agreement->parent_digest);
        /* The private candidate has binding metadata only for parent import. */
        lcs_config_requests_close(requests);
        result.result = same ? lcs_config_requests_recover(requests, &disk, job->initialize_requests,
                                                           result.error, sizeof(result.error)) : -1;
        if (result.result)
        {
            requests->poisoned = true;
            if (!*result.error)
                snprintf(result.error, sizeof(result.error), "disk agreement changed; recover before opening request history");
        }
        /* requests borrows disk.base/store, which are the immutable parent's
         * bindings, not disk-owned values. No journal pointers are retained. */
        lcs_config_agreement_close(&disk);
    }
    else if (job->action == LCS_CONFIG_REQUEST_APPLY)
        result.result = lcs_config_requests_apply(requests, agreement, job->admission_catalog, job->admission,
                                                    result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_REQUEST_RESERVE)
        result.result = lcs_config_requests_reserve_client(requests, agreement, job->revision,
            job->parent_digest, job->value, job->client_digest, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_REQUEST_OBSERVE)
        result.result = lcs_config_requests_observe(requests, agreement, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_PREPARE)
        result.result = lcs_config_agreement_prepare(agreement, job->revision,
            job->parent_digest, job->ballot, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_ACCEPT)
        result.result = lcs_config_agreement_accept(agreement, job->parent_digest,
            job->ballot, job->value, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_LEARN)
        result.result = lcs_config_agreement_learn(agreement, job->parent_digest,
            job->ballot, job->voters, job->value, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_CHECKPOINT)
        result.result = lcs_config_agreement_checkpoint(agreement, job->parent_digest,
            job->ballot, job->voters, job->value, result.error, sizeof(result.error));
    else if (job->action == LCS_CONFIG_PROJECTION)
    {
        lcs_config_t *decoded = malloc(sizeof(*decoded));
        uint64_t revision;
        if (!decoded || lcs_resource_config_decode(job->value->document, job->value->length,
            agreement->base, decoded, &revision, result.error, sizeof(result.error)) ||
            decoded->resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION || revision != job->value->revision)
        {
            result.result = -1;
            if (!*result.error)
                snprintf(result.error, sizeof(result.error), "projection requires a learned dynamic configuration");
        }
        else
            result.result = lcs_config_store_write(&job->projection->snapshots, job->projection->snapshot_name,
                job->value->document, job->value->length, result.error, sizeof(result.error));
        free(decoded);
    }
    else if (job->action == LCS_CONFIG_APPLICATION_BEGIN)
        result.result = lcs_config_application_begin(application, agreement,
                                                      result.error, sizeof(result.error));
    else
        result.result = lcs_config_application_finish(application, job->application_digest,
                                                       result.error, sizeof(result.error));
    result.poisoned = agreement->poisoned || (application && application->poisoned) ||
        (requests && requests->poisoned);
    char *record = NULL;
    size_t length = 0;
    int exported = -1;
    if (!result.poisoned)
        exported = requests ? lcs_config_requests_export(requests, &record, &length,
                                                               result.error, sizeof(result.error)) :
                   application ? lcs_config_application_export(application, &record, &length,
                                                               result.error, sizeof(result.error)) :
                                 lcs_config_agreement_export(agreement, &record, &length,
                                                             result.error, sizeof(result.error));
    if (!result.poisoned && exported)
    {
        result.poisoned = true;
        result.result = -1;
    }
    result.length = (uint32_t)length;
    if (write_all(pipe_fd, &result, sizeof(result)) ||
        (length && write_all(pipe_fd, record, length)))
        _exit(1);
    free(record);
    _exit(0);
}

static int start(lcs_config_agreement_worker_t *worker,
                    lcs_config_agreement_t *agreement,
                    lcs_config_application_t *application,
                    lcs_config_requests_t *requests,
                    const lcs_config_agreement_job_t *job,
                    lcs_config_runtime_t *target, lcs_config_runtime_t *candidate,
                    bool allow_initialize,
                    lcs_config_runtime_t *barrier_runtime,
                    char *err, size_t err_len)
{
    if (worker->pid > 0)
        return 1;
    bool request_job = job && (job->action == LCS_CONFIG_REQUEST_RESERVE ||
                               job->action == LCS_CONFIG_REQUEST_OBSERVE ||
                               job->action == LCS_CONFIG_REQUEST_OPEN || job->action == LCS_CONFIG_REQUEST_APPLY);
    if (request_job != (requests != NULL) || (requests &&
        (requests->poisoned || requests->store != agreement->store || requests->base != agreement->base ||
         strcmp(requests->member, agreement->member) || strcmp(requests->membership, agreement->membership) ||
         requests->count > LCS_CONFIG_REQUEST_HISTORY_MAX ||
         (job->action == LCS_CONFIG_REQUEST_RESERVE && (!job->value ||
          !memchr(job->parent_digest, 0, sizeof(job->parent_digest)) ||
          !memchr(job->client_digest, 0, sizeof(job->client_digest)))))))
        return fail(err, err_len, "invalid request history job or recovery required");
    bool projection_job = job && job->action == LCS_CONFIG_PROJECTION;
    bool application_job = job && (job->action == LCS_CONFIG_APPLICATION_BEGIN ||
                                   job->action == LCS_CONFIG_APPLICATION_FINISH);
    if (!candidate && !barrier_runtime && (agreement->poisoned || !job || application_job != (application != NULL) ||
        (application && (application->poisoned || application->store != agreement->store ||
         strcmp(application->member, agreement->member) ||
         strcmp(application->membership, agreement->membership))) ||
        (!application_job && !request_job && (job->action < LCS_CONFIG_PREPARE || (job->action > LCS_CONFIG_LEARN && job->action != LCS_CONFIG_CHECKPOINT && !projection_job) ||
         (job->action != LCS_CONFIG_PREPARE && !job->value))) ||
        (projection_job && (!job->projection || !job->projection->initialization_locked ||
            job->projection->worker_pid > 0 || job->projection->snapshots.directory_fd < 0 ||
            job->projection->state.directory_fd != agreement->store->directory_fd ||
            job->projection->state.lock_fd != agreement->store->lock_fd || agreement->store->lock_fd < 0 ||
            !memchr(job->projection->snapshot_name, 0, sizeof(job->projection->snapshot_name)) ||
            !*job->projection->snapshot_name || !job->value->revision ||
            !lcs_config_value_equal(job->value, &agreement->committed))) ||
        (job->action == LCS_CONFIG_APPLICATION_FINISH &&
         (!memchr(job->application_digest, 0, sizeof(job->application_digest)) ||
          !application->pending.document || strcmp(job->application_digest, application->pending.digest)))))
        return fail(err, err_len, "invalid job or agreement state requires recovery");
    unsigned char *buffer = malloc(RESULT_MAX);
    if (!buffer)
        return fail(err, err_len, "out of memory allocating bounded result");
    int fds[2];
    if (pipe2(fds, O_CLOEXEC))
    {
        free(buffer);
        return fail(err, err_len, "cannot create result pipe");
    }
    if (fcntl(fds[0], F_SETFL, O_NONBLOCK))
    {
        close(fds[0]); close(fds[1]); free(buffer);
        return fail(err, err_len, "cannot make result pipe nonblocking");
    }
    pid_t parent = getpid();
    pid_t child = fork();
    if (child < 0)
    {
        close(fds[0]); close(fds[1]); free(buffer);
        return fail(err, err_len, "cannot fork storage worker");
    }
    if (!child)
        execute_child(agreement, application, requests, job, candidate, allow_initialize, barrier_runtime, fds[1], parent);
    close(fds[1]);
    worker->checkpoint = job && job->action == LCS_CONFIG_CHECKPOINT;
    worker->application = application;
    worker->requests = requests;
    worker->request_agreement = requests ? agreement : NULL;
    worker->initialization_target = target;
    worker->initialization_candidate = candidate;
    worker->barrier_target = barrier_runtime;
    worker->buffer = buffer;
    worker->pipe_fd = fds[0];
    worker->pid = child;
    worker->used = 0;
    worker->eof = false;
    worker->cancelled = false;
    return 0;
}

int lcs_config_agreement_worker_start(lcs_config_agreement_worker_t *worker,
                                      lcs_config_agreement_t *agreement,
                                      const lcs_config_agreement_job_t *job,
                    char *err, size_t err_len)
{
    return start(worker, agreement, NULL, NULL, job, NULL, NULL, false, NULL, err, err_len);
}

int lcs_config_agreement_worker_start_projection(lcs_config_agreement_worker_t *worker,
                                                 lcs_config_agreement_t *agreement,
                                                 lcs_config_bootstrap_t *bootstrap,
                                                 char *err, size_t err_len)
{
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PROJECTION,
        .value = &agreement->committed, .projection = bootstrap };
    return start(worker, agreement, NULL, NULL, &job, NULL, NULL, false, NULL, err, err_len);
}

int lcs_config_agreement_worker_start_application(lcs_config_agreement_worker_t *worker,
                                                  lcs_config_agreement_t *agreement,
                                                  lcs_config_application_t *application,
                                                  const lcs_config_agreement_job_t *job,
                                                  char *err, size_t err_len)
{
    if (!application)
        return fail(err, err_len, "application catalog is required");
    return start(worker, agreement, application, NULL, job, NULL, NULL, false, NULL, err, err_len);
}

int lcs_config_agreement_worker_open_requests(lcs_config_agreement_worker_t *worker,
                                               lcs_config_agreement_t *agreement,
                                               lcs_config_requests_t *requests,
                                               bool allow_initialize, char *err, size_t err_len)
{
    if (worker->pid > 0) return 1;
    if (!requests || requests->store || requests->base || requests->count || requests->poisoned ||
        !agreement || agreement->poisoned || !agreement->store || !agreement->base || !agreement->committed.document)
        return fail(err, err_len, "fresh request history and healthy agreement required");
    lcs_config_requests_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate) return fail(err, err_len, "cannot allocate request recovery candidate");
    candidate->store = agreement->store; candidate->base = agreement->base;
    memcpy(candidate->member, agreement->member, sizeof(candidate->member));
    memcpy(candidate->membership, agreement->membership, sizeof(candidate->membership));
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_REQUEST_OPEN, .initialize_requests = allow_initialize };
    int result = start(worker, agreement, NULL, candidate, &job, NULL, NULL, false, NULL, err, err_len);
    if (result) free(candidate);
    else worker->requests_target = requests;
    return result;
}

int lcs_config_agreement_worker_start_requests(lcs_config_agreement_worker_t *worker,
                                               lcs_config_agreement_t *agreement,
                                               lcs_config_requests_t *requests,
                                               const lcs_config_agreement_job_t *job,
                                               char *err, size_t err_len)
{
    if (!requests || !job || (job->action != LCS_CONFIG_REQUEST_RESERVE &&
                              job->action != LCS_CONFIG_REQUEST_OBSERVE && job->action != LCS_CONFIG_REQUEST_APPLY))
        return fail(err, err_len, "request history and reserve/observe job are required");
    return start(worker, agreement, NULL, requests, job, NULL, NULL, false, NULL, err, err_len);
}

static bool empty_runtime(const lcs_config_runtime_t *runtime)
{
    return !runtime->requests.store && !runtime->requests.base && !runtime->requests.count &&
        !runtime->requests.poisoned && !runtime->base && !runtime->barrier.store && !runtime->barrier.poisoned &&
        !runtime->agreement.store && !runtime->application.store &&
        !runtime->activation.store && !runtime->agreement.committed.document &&
        !runtime->agreement.accepted.document && !runtime->application.applied.document &&
        !runtime->application.pending.document && !runtime->application.history[0].document &&
        !runtime->application.history[1].document && !runtime->activation.baseline.document &&
        !runtime->agreement.promised && !runtime->agreement.accepted_ballot &&
        !runtime->agreement.committed_ballot && !runtime->agreement.committed_voters &&
        !*runtime->agreement.parent_digest && !runtime->activation.phase &&
        !runtime->agreement.poisoned && !runtime->application.poisoned && !runtime->activation.poisoned;
}

static bool frozen_runtime(const lcs_config_runtime_t *runtime, const lcs_config_value_t *baseline)
{
    if (!runtime->requires_recovery || !runtime->base || runtime->activation.base != runtime->base ||
        !runtime->activation.store || runtime->activation.poisoned || !runtime->activation.requests_required ||
        runtime->activation.phase != LCS_CONFIG_ACTIVATION_FROZEN ||
        !lcs_config_value_equal(&runtime->activation.baseline, baseline)) return false;
    lcs_config_runtime_t view = *runtime;
    view.base = NULL; memset(&view.activation, 0, sizeof(view.activation));
    return empty_runtime(&view);
}

static lcs_config_runtime_t *initialization_candidate(lcs_config_bootstrap_t *bootstrap,
    const lcs_config_t *cfg, bool freeze_only, char *err, size_t err_len)
{
    lcs_config_runtime_t *candidate = calloc(1, sizeof(*candidate));
    if (!candidate)
    { fail(err, err_len, "out of memory preparing initialization"); return NULL; }
    candidate->base = malloc(sizeof(*candidate->base));
    char *document = NULL;
    size_t length;
    uint64_t revision;
    if (!candidate->base ||
        lcs_resource_config_encode(cfg, 0, &document, &length, err, err_len) ||
        lcs_resource_config_decode(document, length, cfg, candidate->base, &revision, err, err_len) ||
        lcs_config_agreement_membership(candidate->base, candidate->activation.membership))
        goto out;
    if (candidate->base->resource_schema_version != LCS_RESOURCE_SCHEMA_VERSION)
    { fail(err, err_len, "initialization requires the legacy resource schema"); goto out; }
    int self = lcs_config_self_index(candidate->base);
    if (self < 0 || lcs_config_value_init(&candidate->activation.baseline, candidate->base,
                                          "bootstrap", document, length, err, err_len))
        goto out;
    candidate->activation.store = &bootstrap->state;
    candidate->activation.base = candidate->base;
    candidate->activation.phase = LCS_CONFIG_ACTIVATION_PREPARED;
    candidate->activation.requests_required = true;
    snprintf(candidate->activation.member, sizeof(candidate->activation.member), "%s", candidate->base->nodes[self].name);
    candidate->requires_recovery = true;
    if (freeze_only)
    {
        candidate->activation.phase = LCS_CONFIG_ACTIVATION_FROZEN;
        free(document);
        return candidate;
    }
    candidate->agreement.store = candidate->application.store = &bootstrap->state;
    candidate->agreement.base = candidate->application.base = candidate->base;
    snprintf(candidate->agreement.member, sizeof(candidate->agreement.member), "%s", candidate->activation.member);
    snprintf(candidate->application.member, sizeof(candidate->application.member), "%s", candidate->activation.member);
    snprintf(candidate->agreement.membership, sizeof(candidate->agreement.membership), "%s", candidate->activation.membership);
    snprintf(candidate->application.membership, sizeof(candidate->application.membership), "%s", candidate->activation.membership);
    candidate->requests.store = &bootstrap->state; candidate->requests.base = candidate->base;
    snprintf(candidate->requests.member, sizeof(candidate->requests.member), "%s", candidate->activation.member);
    snprintf(candidate->requests.membership, sizeof(candidate->requests.membership), "%s", candidate->activation.membership);
    if (lcs_config_value_copy(&candidate->agreement.committed, &candidate->activation.baseline) ||
        lcs_config_value_copy(&candidate->application.applied, &candidate->activation.baseline))
        goto out;
    free(document);
    return candidate;
out:
    free(document);
    lcs_config_runtime_close(candidate); free(candidate);
    return NULL;
}

int lcs_config_agreement_worker_start_initialization(lcs_config_agreement_worker_t *worker,
    lcs_config_runtime_t *runtime, lcs_config_bootstrap_t *bootstrap, const lcs_config_t *cfg,
    bool allow_initialize, char *err, size_t err_len)
{
    if (worker->pid > 0 || bootstrap->worker_pid > 0 || bootstrap->refresh_pending) return 1;
    bool from_freeze = frozen_runtime(runtime, &runtime->activation.baseline);
    if ((!from_freeze && (!empty_runtime(runtime) || runtime->requires_recovery ||
         bootstrap->initialization_locked || !bootstrap->prepared)) ||
        (from_freeze && (!allow_initialize || !bootstrap->initialization_locked || bootstrap->prepared ||
         runtime->activation.store != &bootstrap->state)) ||
        bootstrap->state.directory_fd < 0 || bootstrap->state.lock_fd < 0)
        return fail(err, err_len, "initialization requires fresh prepared storage or an explicitly authorized durable freeze");
    lcs_config_runtime_t *candidate = initialization_candidate(bootstrap, cfg, false, err, err_len);
    if (!candidate) return -1;
    if (from_freeze && !lcs_config_value_equal(&runtime->activation.baseline, &candidate->activation.baseline))
    {
        lcs_config_runtime_close(candidate); free(candidate);
        return fail(err, err_len, "initialization differs from retained durable freeze");
    }
    int result = start(worker, &candidate->agreement, NULL, NULL, NULL, runtime, candidate,
                     allow_initialize, NULL, err, err_len);
    if (!result)
    {
        worker->initialization_from_freeze = from_freeze;
        candidate = NULL; /* Owned by the worker until successful publication. */
        runtime->requires_recovery = true;
        bootstrap->initialization_locked = true;
        bootstrap->prepared = false;
    }
    if (candidate)
    {
        lcs_config_runtime_close(candidate);
        free(candidate);
    }
    return result;
}

int lcs_config_agreement_worker_start_freeze(lcs_config_agreement_worker_t *worker,
    lcs_config_runtime_t *runtime, lcs_config_bootstrap_t *bootstrap, const lcs_config_t *cfg,
    char *err, size_t err_len)
{
    if (worker->pid > 0 || bootstrap->worker_pid > 0 || bootstrap->refresh_pending) return 1;
    if (!empty_runtime(runtime) || runtime->requires_recovery || bootstrap->initialization_locked ||
        !bootstrap->prepared || bootstrap->state.directory_fd < 0 || bootstrap->state.lock_fd < 0)
        return fail(err, err_len, "freeze requires fresh runtime and prepared idle storage");
    lcs_config_runtime_t *candidate = initialization_candidate(bootstrap, cfg, true, err, err_len);
    if (!candidate) return -1;
    /* Carrier is only inherited child plumbing; the published frozen runtime
     * deliberately has no agreement/application/request handles. */
    lcs_config_agreement_t carrier = { .store = &bootstrap->state, .base = candidate->base };
    int result = start(worker, &carrier, NULL, NULL, NULL, runtime, candidate, false, NULL, err, err_len);
    if (!result)
    {
        runtime->requires_recovery = bootstrap->initialization_locked = true;
        bootstrap->prepared = false;
    }
    else { lcs_config_runtime_close(candidate); free(candidate); }
    return result;
}

int lcs_config_agreement_worker_start_initialization_round(lcs_config_agreement_worker_t *worker,
    lcs_config_runtime_t *runtime, lcs_config_bootstrap_t *bootstrap, const lcs_config_t *cfg,
    lcs_config_initialization_round_t *round, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t now_ms, char *err, size_t err_len)
{
    if (worker->pid > 0 || bootstrap->worker_pid > 0 || bootstrap->refresh_pending) return 1;
    if (!round || lcs_config_initialization_authorize(round, cfg, instances, generation, now_ms, err, err_len))
        return fail(err, err_len, "initialization requires a complete current all-member freeze round");
    int result = lcs_config_agreement_worker_start_initialization(worker, runtime, bootstrap, cfg, true, err, err_len);
    if (!result) round->consumed = true;
    return result;
}

int lcs_config_agreement_worker_start_barrier(lcs_config_agreement_worker_t *worker,
                                              lcs_config_runtime_t *runtime,
                                              const lcs_config_barrier_round_t *round,
                                              char *err, size_t err_len)
{
    if (worker->pid > 0)
        return 1;
    lcs_config_barrier_round_t local;
    if (!runtime->base || runtime->base != runtime->activation.base ||
        runtime->barrier.store || runtime->barrier.poisoned || !round || !round->sealed || round->failed ||
        lcs_config_barrier_begin(&local, &runtime->activation, &runtime->agreement,
                                   &runtime->application, err, err_len) ||
        round->required != local.required || round->ready != local.required ||
        strcmp(round->membership, local.membership) || strcmp(round->baseline, local.baseline))
        return fail(err, err_len, "barrier requires complete matching readiness and pristine initialization");
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_BARRIER, .barrier_round = round };
    int result = start(worker, &runtime->agreement, NULL, NULL, &job, NULL, NULL, false, runtime, err, err_len);
    if (!result)
    {
        lcs_config_barrier_t *candidate = &worker->barrier_candidate;
        candidate->store = runtime->activation.store;
        candidate->base = runtime->base;
        memcpy(candidate->member, runtime->activation.member, sizeof(candidate->member));
        memcpy(candidate->membership, local.membership, sizeof(candidate->membership));
        memcpy(candidate->baseline, local.baseline, sizeof(candidate->baseline));
        candidate->ready = local.required;
    }
    return result;
}

static void poison(lcs_config_agreement_worker_t *worker, lcs_config_agreement_t *agreement)
{
    if (worker->barrier_target)
    {
        lcs_config_runtime_t *runtime = worker->barrier_target;
        runtime->barrier.poisoned = runtime->agreement.poisoned = true;
        runtime->application.poisoned = runtime->activation.poisoned = true;
        runtime->requires_recovery = true;
        return;
    }
    if (worker->initialization_target)
    {
        lcs_config_runtime_t *runtime = worker->initialization_target;
        runtime->agreement.poisoned = runtime->application.poisoned = runtime->activation.poisoned = runtime->requests.poisoned = true;
        runtime->requires_recovery = true;
        return;
    }
    agreement->poisoned = true;
    if (worker->requests)
        worker->requests->poisoned = true;
    if (worker->requests_target)
        worker->requests_target->poisoned = true;
    if (worker->application)
        worker->application->poisoned = true;
}

void lcs_config_agreement_worker_cancel(lcs_config_agreement_worker_t *worker,
                                        lcs_config_agreement_t *agreement)
{
    if (worker->pid > 0)
    {
        if (worker->request_agreement)
            agreement = worker->request_agreement;
        poison(worker, agreement);
        worker->cancelled = true;
        pid_t reaped;
        do
        {
            reaped = waitpid(worker->pid, NULL, WNOHANG);
        } while (reaped < 0 && errno == EINTR);
        if (!reaped)
        {
            kill(worker->pid, SIGKILL);
            do
            {
                reaped = waitpid(worker->pid, NULL, WNOHANG);
            } while (reaped < 0 && errno == EINTR);
        }
        if (!reaped || (reaped < 0 && errno == EINTR))
            return;
    }
    reset(worker);
}

int lcs_config_agreement_worker_poll(lcs_config_agreement_worker_t *worker,
                                     lcs_config_agreement_t *agreement,
                                     int *operation_result,
                                     char *err, size_t err_len)
{
    if (worker->pid <= 0)
        return 0;
    if (worker->request_agreement && agreement != worker->request_agreement)
        return fail(err, err_len, "request completion requires the original agreement handle");
    if (worker->initialization_target && agreement != &worker->initialization_target->agreement)
        return fail(err, err_len, "initialization completion requires the original runtime handle");
    if (worker->barrier_target && agreement != &worker->barrier_target->agreement)
        return fail(err, err_len, "barrier completion requires the original runtime handle");
    if (worker->cancelled)
    {
        pid_t reaped = waitpid(worker->pid, NULL, WNOHANG);
        if (!reaped || (reaped < 0 && errno == EINTR))
            return 0;
        poison(worker, agreement);
        reset(worker);
        return fail(err, err_len, "cancelled worker has exited; recover the journal before voting");
    }
    /* Bound scheduler work per call even when a large result is available. */
    size_t budget = 64u * 1024u;
    while (!worker->eof && budget)
    {
        if (worker->used == RESULT_MAX)
            break;
        size_t count = RESULT_MAX - worker->used;
        if (count > budget)
            count = budget;
        ssize_t n = read(worker->pipe_fd, worker->buffer + worker->used, count);
        if (n < 0 && errno == EINTR)
            continue;
        if (n < 0 && (errno == EAGAIN || errno == EWOULDBLOCK))
            return 0;
        if (n < 0)
            goto broken;
        if (!n)
        {
            worker->eof = true;
            break;
        }
        worker->used += (size_t)n;
        budget -= (size_t)n;
    }
    if (!worker->eof && worker->used < RESULT_MAX)
        return 0;
    int status;
    pid_t pid = waitpid(worker->pid, &status, WNOHANG);
    if (!pid || (pid < 0 && errno == EINTR))
        return 0;
    /* Never signal a PID after it has been reaped, even for a malformed result. */
    worker->pid = 0;
    if (pid < 0 || !WIFEXITED(status) || WEXITSTATUS(status) || worker->used < sizeof(worker_result_t))
        goto broken;
    worker_result_t result;
    memcpy(&result, worker->buffer, sizeof(result));
    result.error[sizeof(result.error) - 1] = '\0';
    if (result.length > (worker->requests ? LCS_CONFIG_REQUEST_RECORD_MAX :
                         worker->application ? LCS_CONFIG_APPLICATION_MAX : LCS_CONFIG_JOURNAL_MAX) ||
        worker->used != sizeof(result) + result.length ||
        result.result < -1 || result.result > (worker->requests ? LCS_CONFIG_REQUEST_CAPACITY : LCS_CONFIG_STALE))
        goto broken;
    if (result.poisoned)
    {
        poison(worker, agreement);
        fail(err, err_len, *result.error ? result.error : "uncertain persistence; recovery required");
        reset(worker);
        return -1;
    }
    if (worker->initialization_target)
    {
        lcs_config_runtime_t *target = worker->initialization_target;
        if (result.result || result.length || !target->requires_recovery ||
            (worker->initialization_from_freeze ?
             !frozen_runtime(target, &worker->initialization_candidate->activation.baseline) : !empty_runtime(target)))
            goto broken;
        if (worker->initialization_from_freeze)
        {
            /* Owner/coordination adapters may borrow the immutable frozen base.
             * Promotion changes journals, not that base's lifetime or contents. */
            lcs_config_t *base = target->base;
            target->base = NULL; lcs_config_runtime_close(target);
            lcs_config_runtime_t *candidate = worker->initialization_candidate;
            free(candidate->base); candidate->base = base;
            candidate->activation.base = candidate->agreement.base = candidate->application.base = candidate->requests.base = base;
        }
        *target = *worker->initialization_candidate;
        memset(worker->initialization_candidate, 0, sizeof(*worker->initialization_candidate));
        *operation_result = 0;
        reset(worker);
        return 1;
    }
    if (worker->barrier_target)
    {
        lcs_config_runtime_t *target = worker->barrier_target;
        if (result.result || result.length || target->barrier.store || target->barrier.poisoned ||
            !lcs_config_barrier_local_ready(&target->activation, &target->agreement, &target->application) ||
            !lcs_config_barrier_can_vote(&worker->barrier_candidate, &target->activation,
                                           &target->agreement, &target->application))
            goto broken;
        target->barrier = worker->barrier_candidate;
        *operation_result = 0;
        reset(worker);
        return 1;
    }
    const char *record = (const char *)worker->buffer + sizeof(result);
    int imported = !result.length ? -1 : worker->requests ?
        lcs_config_requests_import(worker->requests, record, result.length, err, err_len) : worker->application ?
        lcs_config_application_import(worker->application, agreement, record, result.length, err, err_len) :
        worker->checkpoint ? lcs_config_agreement_import_checkpoint(agreement, record, result.length, err, err_len) :
        lcs_config_agreement_import(agreement, record, result.length, err, err_len);
    if (imported)
    {
        poison(worker, agreement);
        reset(worker);
        return -1;
    }
    if (worker->requests_target)
    {
        if (result.result || worker->requests_target->store || worker->requests_target->base ||
            worker->requests_target->count || worker->requests_target->poisoned)
            goto broken;
        *worker->requests_target = *worker->requests;
    }
    *operation_result = result.result;
    if (result.result < 0)
        fail(err, err_len, result.error);
    reset(worker);
    return 1;
broken:
    poison(worker, agreement);
    lcs_config_agreement_worker_cancel(worker, agreement);
    return fail(err, err_len, "worker failed before a complete durable result; recover before voting");
}
