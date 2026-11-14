// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_coordinator.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static lcs_config_t config;
static lcs_config_runtime_t runtime;
static lcs_config_readiness_t readiness;
static lcs_config_coordinator_t coordinator;
static lcs_config_store_t store;
static char directory[80], error[256];
static pid_t parent;
static bool forbid_io;
static int stop_sync, fail_sync;

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_io || getpid() != parent);
    if (stop_sync && !--stop_sync)
        raise(SIGSTOP);
    if (fail_sync && !--fail_sync)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}
int __real_lcs_config_store_read(lcs_config_store_t *, const char *, char **, size_t *, size_t, char *, size_t);
int __wrap_lcs_config_store_read(lcs_config_store_t *storage, const char *name, char **text,
                                 size_t *length, size_t maximum, char *err, size_t err_len)
{
    assert(!forbid_io || getpid() != parent);
    return __real_lcs_config_store_read(storage, name, text, length, maximum, err, err_len);
}

static void initialize(void)
{
    memset(&runtime, 0, sizeof(runtime));
    memset(&readiness, 0, sizeof(readiness));
    lcs_config_init_defaults(&config);
    strcpy(config.cluster_name, "coordinator-test");
    strcpy(config.self_name, "node0");
    config.node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(config.nodes[i].name, sizeof(config.nodes[i].name), "node%zu", i);
        strcpy(config.nodes[i].address, "127.0.0.1");
        config.nodes[i].port = 3322;
        config.nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    config.resource_count = 1;
    strcpy(config.resources[0].name, "vip");
    config.resources[0].type = LCS_RESOURCE_VIP;
    strcpy(config.resources[0].address, "192.0.2.10/32");
    strcpy(config.resources[0].interface, "eth0");
    assert(!lcs_config_finalize(&config, error, sizeof(error)));
    strcpy(directory, "/tmp/lcs-coordinator.XXXXXX");
    assert(mkdtemp(directory));
    assert(!lcs_config_store_open(&store, directory, error, sizeof(error)));
    runtime.base = malloc(sizeof(*runtime.base));
    assert(runtime.base);
    assert(!lcs_config_activation_open(&runtime.activation, &store, &config, runtime.base, true, error, sizeof(error)));
    assert(!lcs_config_activation_resume(&runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)));
    /* Explicit fresh fixture initialization, not daemon upgrade policy. */
    assert(!lcs_config_requests_recover(&runtime.requests, &runtime.agreement, true, error, sizeof(error)));
    runtime.requires_recovery = true;
    lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
    assert(!lcs_config_readiness_begin(&readiness, &runtime.activation, &runtime.agreement,
                                       &runtime.application, 100, 1, 1000, error, sizeof(error)));
    assert(!lcs_config_coordinator_start(&coordinator));
    assert(coordinator.phase == LCS_CONFIG_COORD_COLLECTING && !lcs_config_coordinator_can_vote(&coordinator));
    assert(!lcs_config_coordinator_can_close(&coordinator));
}

static void confirm(size_t node)
{
    lcs_config_readiness_message_t message;
    assert(!lcs_config_readiness_query(&readiness, node, 100 + node, 1001, &message));
    message.kind = LCS_READINESS_REPLY;
    message.responder_instance = 100 + node;
    message.status = LCS_READINESS_READY;
    assert(!lcs_config_readiness_reply(&readiness, node, 100 + node, 1002, &message, error, sizeof(error)));
}

static int await_completion(void)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int result = lcs_config_coordinator_process(&coordinator, 1005);
        if (result || coordinator.phase == LCS_CONFIG_COORD_STOPPED)
            return result;
        usleep(1000);
    }
    assert(!"coordinator did not finish");
    return -1;
}

static void await_stopped(void)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int status;
        pid_t pid = waitpid(coordinator.storage.pid, &status, WNOHANG | WUNTRACED);
        if (pid > 0)
        {
            assert(WIFSTOPPED(status));
            return;
        }
        assert(pid == 0 || errno == EINTR);
        usleep(1000);
    }
    assert(!"storage child did not reach sync barrier");
}

static void cleanup(void)
{
    forbid_io = false;
    fail_sync = stop_sync = 0;
    lcs_config_coordinator_stop(&coordinator);
    for (unsigned i = 0; i < 5000 && !lcs_config_coordinator_can_close(&coordinator); i++)
    {
        (void)lcs_config_coordinator_process(&coordinator, 1005);
        usleep(1000);
    }
    assert(lcs_config_coordinator_can_close(&coordinator));
    lcs_config_runtime_close(&runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json",
        ".barrier.json.tmp", ".agreement.json.tmp", "requests.json", ".requests.json.tmp", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        assert(!unlinkat(store.directory_fd, names[i], 0) || errno == ENOENT);
    lcs_config_store_close(&store);
    assert(!rmdir(directory));
}

static void test_collection_and_durability(void)
{
    initialize();
    confirm(1);
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(!coordinator.storage.pid && !runtime.barrier.store);
    confirm(2);
    assert(lcs_config_readiness_complete(&readiness));
    forbid_io = true;
    stop_sync = 2;
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(coordinator.phase == LCS_CONFIG_COORD_PERSISTING && !lcs_config_coordinator_can_close(&coordinator));
    await_stopped();
    assert(!runtime.barrier.store && !lcs_config_coordinator_can_vote(&coordinator));
    assert(!lcs_config_coordinator_process(&coordinator, 20000)); /* No timeout after collection sealed. */
    assert(lcs_config_coordinator_start(&coordinator) == 1);
    assert(!kill(coordinator.storage.pid, SIGCONT));
    stop_sync = 0;
    assert(await_completion() == 1);
    assert(lcs_config_coordinator_can_vote(&coordinator) && runtime.requires_recovery);
    assert(coordinator.phase == LCS_CONFIG_COORD_READY && runtime.barrier.ready == 7);
    lcs_config_coordinator_stop(&coordinator);
    assert(!lcs_config_coordinator_can_vote(&coordinator) && lcs_config_coordinator_can_close(&coordinator));
    assert(lcs_config_coordinator_start(&coordinator) == -1);
    cleanup();
}

static void test_failure_and_cancellation(void)
{
    for (int mode = 0; mode < 5; mode++)
    {
        initialize();
        confirm(1); confirm(2);
        fail_sync = mode < 2 ? mode + 1 : 0;
        stop_sync = mode < 2 ? 0 : 2;
        forbid_io = true;
        assert(!lcs_config_coordinator_process(&coordinator, 1003));
        if (mode >= 2)
        {
            await_stopped();
            if (mode == 2)
                lcs_config_coordinator_stop(&coordinator);
            else if (mode == 3)
                readiness.query.attempt++;
            else
                runtime.agreement.promised = 32;
            if (mode != 2)
                assert(!lcs_config_coordinator_process(&coordinator, 1004) || coordinator.phase == LCS_CONFIG_COORD_FAILED);
        }
        int result = await_completion();
        assert(mode == 2 ? result == 0 : result == -1);
        assert(lcs_config_coordinator_can_close(&coordinator) && !lcs_config_coordinator_can_vote(&coordinator));
        assert(runtime.agreement.poisoned && runtime.activation.poisoned && runtime.barrier.poisoned);
        fail_sync = stop_sync = 0;
        forbid_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
        assert((runtime.barrier.store != NULL) == (mode != 0));
        assert(!runtime.agreement.promised);
        cleanup();
    }
}

static void test_incomplete_timeout_and_changed_attempt(void)
{
    initialize();
    confirm(1);
    assert(lcs_config_coordinator_process(&coordinator, 11000) == -1);
    assert(!coordinator.storage.pid && !runtime.agreement.poisoned);
    assert(faccessat(store.directory_fd, "barrier.json", F_OK, 0) == -1);
    cleanup();
    initialize();
    readiness.query.attempt++;
    assert(lcs_config_coordinator_process(&coordinator, 1003) == -1);
    assert(!coordinator.storage.pid && !runtime.agreement.poisoned);
    cleanup();
}

static void test_gated_votes(void)
{
    initialize();
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PREPARE, .revision = 1 };
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    job.ballot = lcs_config_agreement_next_ballot(&runtime.agreement, 0);
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == -1);
    confirm(1); confirm(2);
    forbid_io = true;
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == 1);
    assert(await_completion() == 1);
    stop_sync = 1;
    assert(!lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)));
    await_stopped();
    assert(coordinator.phase == LCS_CONFIG_COORD_VOTING && !lcs_config_coordinator_can_vote(&coordinator));
    assert(!runtime.agreement.promised && !coordinator.completed_votes);
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == 1);
    assert(!kill(coordinator.storage.pid, SIGCONT));
    stop_sync = 0;
    assert(await_completion() == 1);
    assert(runtime.agreement.promised == job.ballot && coordinator.completed_votes == 1 && !coordinator.last_vote_result);
    job.revision = 0;
    assert(!lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)));
    assert(await_completion() == 1);
    assert(coordinator.completed_votes == 2 && coordinator.last_vote_result == LCS_CONFIG_STALE);
    assert(!runtime.agreement.poisoned && runtime.agreement.promised == job.ballot);
    job.revision = 1;
    char *document;
    size_t length;
    lcs_config_value_t value = {0};
    assert(!lcs_resource_config_encode(&config, 1, &document, &length, error, sizeof(error)));
    assert(!lcs_config_value_init(&value, &config, "bad-legacy", document, length, error, sizeof(error)));
    free(document);
    job.action = LCS_CONFIG_ACCEPT;
    job.value = &value;
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == -1);
    lcs_config_value_clear(&value);
    lcs_config_t *candidate = malloc(sizeof(*candidate));
    assert(candidate);
    *candidate = config;
    assert(!lcs_resource_config_assign_identities(&config, 0, &config, candidate, error, sizeof(error)));
    strcpy(candidate->resources[0].address, "192.0.2.20/32");
    assert(!lcs_resource_config_encode(candidate, 1, &document, &length, error, sizeof(error)));
    assert(!lcs_config_value_init(&value, &config, "bad-change", document, length, error, sizeof(error)));
    free(document);
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == -1);
    assert(!runtime.agreement.accepted.document && coordinator.completed_votes == 2);
    lcs_config_value_clear(&value);
    strcpy(candidate->resources[0].address, "192.0.2.10/32");
    assert(!lcs_resource_config_encode(candidate, 1, &document, &length, error, sizeof(error)));
    free(candidate);
    assert(!lcs_config_value_init(&value, &config, "activate", document, length, error, sizeof(error)));
    free(document);
    assert(!lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)));
    assert(await_completion() == 1);
    assert(runtime.agreement.accepted.revision == 1 && coordinator.completed_votes == 3);
    assert(runtime.application.applied.revision == 0); /* Acceptance does not activate. */
    lcs_config_value_clear(&value);
    forbid_io = false;
    cleanup();
}

static void test_vote_cancellation_and_recovered_barrier(void)
{
    initialize();
    confirm(1); confirm(2);
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(await_completion() == 1);
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PREPARE, .revision = 1 };
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    job.ballot = lcs_config_agreement_next_ballot(&runtime.agreement, 0);
    stop_sync = 2; /* A visible promise without a completed storage result. */
    forbid_io = true;
    assert(!lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)));
    await_stopped();
    lcs_config_coordinator_stop(&coordinator);
    assert(await_completion() == 0 && lcs_config_coordinator_can_close(&coordinator));
    assert(runtime.agreement.poisoned && !lcs_config_coordinator_can_vote(&coordinator));
    forbid_io = false;
    stop_sync = 0;
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
    assert(runtime.agreement.promised == job.ballot && runtime.barrier.ready == 7);
    memset(&readiness, 0, sizeof(readiness)); /* No live peer readiness observations after restart. */
    lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
    forbid_io = true;
    assert(!lcs_config_coordinator_recover(&coordinator));
    assert(lcs_config_coordinator_can_vote(&coordinator));
    assert(!readiness.active && runtime.requires_recovery);
    assert(!lcs_config_value_copy(&runtime.application.pending, &runtime.application.applied));
    assert(!lcs_config_coordinator_can_vote(&coordinator));
    assert(lcs_config_coordinator_start_vote(&coordinator, &job, error, sizeof(error)) == -1);
    lcs_config_value_clear(&runtime.application.pending);
    cleanup();
}

static void ready_with_requests(void)
{
    confirm(1); confirm(2);
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(await_completion() == 1);
    assert(runtime.requests.store);
}
static lcs_config_value_t request_value(const char *id)
{
    lcs_config_t *candidate = malloc(sizeof(*candidate)); assert(candidate);
    assert(!lcs_resource_config_assign_identities(&config, 0, &config, candidate, error, sizeof(error)));
    char *document; size_t length; lcs_config_value_t value = {0};
    assert(!lcs_resource_config_encode(candidate, 1, &document, &length, error, sizeof(error)));
    assert(!lcs_config_value_init(&value, &config, id, document, length, error, sizeof(error)));
    free(document); free(candidate); return value;
}
static void await_request_ready(void)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int result = lcs_config_coordinator_process(&coordinator, 1005);
        assert(result >= 0 && !coordinator.failed);
        if (coordinator.phase == LCS_CONFIG_COORD_READY) return;
        usleep(1000);
    }
    assert(!"request coordinator did not finish");
}
static void test_request_metadata_gates_and_completions(void)
{
    initialize();
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_REQUEST_OPEN};
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    ready_with_requests();
    lcs_config_requests_close(&runtime.requests);
    job.initialize_requests = true;
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    job.initialize_requests = false;
    forbid_io = true;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    assert(coordinator.phase == LCS_CONFIG_COORD_REQUESTING && !runtime.requests.store);
    assert(!lcs_config_coordinator_can_vote(&coordinator));
    await_request_ready();
    assert(runtime.requests.store && coordinator.completed_requests == 1 && !coordinator.last_request_result);
    assert(!coordinator.completed_votes && !coordinator.completed_applications && !coordinator.completed_projections);
    coordinator.completed_requests = UINT64_MAX;
    job.action = LCS_CONFIG_REQUEST_OBSERVE;
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    coordinator.completed_requests = 1;
    lcs_config_value_t value = request_value("request-one");
    job.action = LCS_CONFIG_REQUEST_RESERVE; job.value = &value;
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    int owner;
    coordinator.application_owner = &owner;
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    coordinator.application_owner = NULL;
    stop_sync = 2;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_stopped();
    assert(!runtime.requests.count && coordinator.completed_requests == 1);
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) == 1);
    lcs_config_agreement_job_t vote = {.action = LCS_CONFIG_PREPARE};
    assert(lcs_config_coordinator_start_vote(&coordinator, &vote, error, sizeof(error)) == 1);
    assert(lcs_config_coordinator_start_application(&coordinator, &owner, &vote, error, sizeof(error)) == 1);
    assert(!kill(coordinator.storage.pid, SIGCONT)); stop_sync = 0;
    await_request_ready();
    assert(runtime.requests.count == 1 && !runtime.requests.records[0].committed);
    assert(coordinator.completed_requests == 2 && coordinator.last_request_result == LCS_CONFIG_REQUEST_RECORDED);
    coordinator.application_owner = &owner;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_requests == 3 && coordinator.last_request_result == LCS_CONFIG_REQUEST_EXISTS);
    memset(job.parent_digest, 'f', 64); job.parent_digest[64] = 0;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_requests == 4 && coordinator.last_request_result == LCS_CONFIG_REQUEST_CONFLICT);
    assert(!runtime.requests.poisoned && !runtime.agreement.poisoned && coordinator.application_owner == &owner);
    coordinator.application_owner = NULL;
    /* Persist a decision, then observe it while the application is behind. */
    vote.action = LCS_CONFIG_LEARN; vote.value = &value; vote.ballot = 32; vote.voters = 3;
    strcpy(vote.parent_digest, runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_learn(&coordinator, &vote, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_votes == 1 && !lcs_config_coordinator_can_vote(&coordinator));
    job.action = LCS_CONFIG_REQUEST_OBSERVE;
    coordinator.application_owner = &owner;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_requests == 6 && coordinator.last_request_result == LCS_CONFIG_REQUEST_EXISTS && runtime.requests.records[0].committed);
    assert(coordinator.completed_votes == 1 && !coordinator.completed_applications && !coordinator.completed_projections);
    assert(runtime.application.applied.revision == 0 && coordinator.application_owner == &owner);
    job.action = LCS_CONFIG_REQUEST_RESERVE;
    strcpy(job.parent_digest, vote.parent_digest);
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_requests == 7 && coordinator.last_request_result == LCS_CONFIG_REQUEST_EXISTS);
    assert(!lcs_config_coordinator_can_vote(&coordinator));
    lcs_config_value_clear(&value); cleanup();
}
static void test_request_capacity_and_invalid_successor(void)
{
    initialize(); ready_with_requests();
    lcs_config_value_t value = request_value("bad-activation");
    lcs_config_t *bad = malloc(sizeof(*bad)); assert(bad);
    uint64_t revision; char *document; size_t length;
    assert(!lcs_resource_config_decode(value.document, value.length, &config, bad, &revision, error, sizeof(error)));
    strcpy(bad->resources[0].address, "192.0.2.20/32");
    assert(!lcs_resource_config_encode(bad, 1, &document, &length, error, sizeof(error))); free(bad);
    lcs_config_value_clear(&value);
    assert(!lcs_config_value_init(&value, &config, "bad-activation", document, length, error, sizeof(error))); free(document);
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_REQUEST_RESERVE, .value = &value};
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    forbid_io = true;
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    assert(!runtime.requests.count && !coordinator.completed_requests && !coordinator.storage.pid);
    forbid_io = false; lcs_config_value_clear(&value);
    for (unsigned i = 0; i < LCS_CONFIG_REQUEST_HISTORY_MAX; i++)
    {
        char id[32]; snprintf(id, sizeof(id), "full-%u", i); value = request_value(id);
        assert(!lcs_config_requests_reserve(&runtime.requests, &runtime.agreement, 0,
            runtime.agreement.committed.digest, &value, error, sizeof(error)));
        lcs_config_value_clear(&value);
    }
    value = request_value("overflow"); forbid_io = true;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    await_request_ready();
    assert(coordinator.completed_requests == 1 && coordinator.last_request_result == LCS_CONFIG_REQUEST_CAPACITY);
    assert(runtime.requests.count == LCS_CONFIG_REQUEST_HISTORY_MAX && !runtime.requests.poisoned && !coordinator.failed);
    lcs_config_value_clear(&value); cleanup();
}
static void test_request_open_never_initializes(void)
{
    initialize();
    confirm(1); confirm(2);
    assert(!lcs_config_coordinator_process(&coordinator, 1003));
    assert(await_completion() == 1);
    lcs_config_requests_close(&runtime.requests);
    assert(!unlinkat(store.directory_fd, "requests.json", 0));
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_REQUEST_OPEN, .initialize_requests = true};
    forbid_io = true;
    assert(lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)) < 0);
    assert(!coordinator.storage.pid && !runtime.requests.poisoned);
    job.initialize_requests = false;
    assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
    assert(await_completion() < 0 && coordinator.failed && lcs_config_coordinator_can_close(&coordinator));
    assert(runtime.requests.poisoned && runtime.agreement.poisoned && !coordinator.completed_requests);
    forbid_io = false;
    assert(faccessat(store.directory_fd, "requests.json", F_OK, 0) < 0 && errno == ENOENT);
    cleanup();
}
static void test_request_failure_and_shutdown(void)
{
    for (unsigned mode = 0; mode < 3; mode++)
    {
        initialize(); ready_with_requests();
        lcs_config_value_t value = request_value("uncertain-request");
        lcs_config_agreement_job_t job = {.action = LCS_CONFIG_REQUEST_RESERVE, .value = &value};
        strcpy(job.parent_digest, runtime.agreement.committed.digest);
        forbid_io = true;
        if (mode < 2) fail_sync = (int)mode + 1;
        else stop_sync = 2;
        assert(!lcs_config_coordinator_start_requests(&coordinator, &job, error, sizeof(error)));
        if (mode == 2)
        {
            await_stopped(); lcs_config_coordinator_stop(&coordinator);
            assert(await_completion() == 0);
        }
        else assert(await_completion() < 0 && coordinator.failed);
        assert(lcs_config_coordinator_can_close(&coordinator) && !coordinator.completed_requests);
        assert(!runtime.requests.count && runtime.requests.poisoned && runtime.agreement.poisoned);
        assert(!lcs_config_coordinator_can_vote(&coordinator));
        forbid_io = false; fail_sync = stop_sync = 0;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
        assert(runtime.requests.count == (mode ? 1u : 0u));
        lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
        assert(!lcs_config_coordinator_recover(&coordinator));
        assert(lcs_config_coordinator_can_vote(&coordinator) && !coordinator.completed_requests);
        lcs_config_value_clear(&value); cleanup();
    }
}
int main(void)
{
    parent = getpid();
    test_collection_and_durability();
    test_failure_and_cancellation();
    test_incomplete_timeout_and_changed_attempt();
    test_gated_votes();
    test_vote_cancellation_and_recovered_barrier();
    test_request_metadata_gates_and_completions();
    test_request_capacity_and_invalid_successor();
    test_request_open_never_initializes();
    test_request_failure_and_shutdown();
    puts("readiness-to-durable-barrier coordinator and gated vote unit tests passed");
    return 0;
}
