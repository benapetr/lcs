// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_activation.h"
#include "config_runtime.h"
#include "config_agreement_worker.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <signal.h>
#include <sys/socket.h>
#include <sys/wait.h>

static int fail_sync;
static int crash_sync;
static int stop_sync;
static pid_t test_parent;
static bool forbid_parent_io;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_parent_io || getpid() != test_parent);
    if (stop_sync && !--stop_sync)
        raise(SIGSTOP);
    if (crash_sync && !--crash_sync) _exit(__real_fsync(fd) ? 78 : 77);
    if (fail_sync && !--fail_sync)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

int __real_lcs_config_store_read(lcs_config_store_t *store, const char *name,
    char **data, size_t *length, size_t maximum, char *err, size_t err_len);
int __wrap_lcs_config_store_read(lcs_config_store_t *store, const char *name,
    char **data, size_t *length, size_t maximum, char *err, size_t err_len)
{
    assert(!forbid_parent_io || getpid() != test_parent);
    return __real_lcs_config_store_read(store, name, data, length, maximum, err, err_len);
}

static char error[256];
static lcs_config_t config, changed, frozen;
static lcs_config_store_t store;
static lcs_config_activation_t activation;
static lcs_config_agreement_t agreement;
static lcs_config_application_t application;
static char directory[80];

static void initialize(void)
{
    lcs_config_init_defaults(&config);
    strcpy(config.cluster_name, "activation-test");
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
    config.resources[0].type = LCS_RESOURCE_VIP;
    strcpy(config.resources[0].name, "vip");
    strcpy(config.resources[0].address, "192.0.2.10/32");
    strcpy(config.resources[0].interface, "eth0");
    assert(lcs_config_finalize(&config, error, sizeof(error)) == 0);
    changed = config;
    strcpy(changed.resources[0].address, "192.0.2.20/32");
    strcpy(directory, "/tmp/lcs-activation.XXXXXX");
    assert(mkdtemp(directory));
    assert(lcs_config_store_open(&store, directory, error, sizeof(error)) == 0);
}

static void close_handles(void)
{
    lcs_config_application_close(&application);
    lcs_config_agreement_close(&agreement);
    lcs_config_activation_close(&activation);
}

static void cleanup(void)
{
    close_handles();
    const char *names[] = {"activation.json", "agreement.json", "application.json", "requests.json", ".requests.json.tmp",
        "barrier.json", ".barrier.json.tmp", ".activation.json.tmp", ".agreement.json.tmp", ".application.json.tmp", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        assert(!unlinkat(store.directory_fd, names[i], 0) || errno == ENOENT);
    lcs_config_store_close(&store);
    assert(rmdir(directory) == 0);
}

static int resume(const lcs_config_t *base, bool create)
{
    if (lcs_config_activation_open(&activation, &store, base, &frozen, create, error, sizeof(error)))
        return -1;
    return lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error));
}

static void test_write_boundaries(void)
{
    /* Intent, agreement, application, prepared record: file and directory sync
     * failures at each boundary. A restart must retain the exact old baseline. */
    for (int boundary = 1; boundary <= 8; boundary++)
    {
        initialize();
        assert(resume(&config, false) == -1);
        fail_sync = boundary;
        assert(resume(&config, true) == -1);
        fail_sync = 0;
        close_handles();
        bool intent = faccessat(store.directory_fd, "activation.json", F_OK, 0) == 0;
        assert(intent == (boundary != 1));
        if (!intent)
        {
            assert(resume(&changed, false) == -1);
            /* No durable intent existed yet, so a newly authorized attempt may
             * freeze the now-current INI. No journal/vote can have escaped. */
            assert(resume(&changed, true) == 0);
            assert(!strcmp(frozen.resources[0].address, "192.0.2.20/32"));
        }
        else
        {
            assert(resume(&changed, false) == 0);
            assert(!strcmp(frozen.resources[0].address, "192.0.2.10/32"));
        }
        assert(activation.phase == LCS_CONFIG_ACTIVATION_PREPARED && !activation.poisoned);
        assert(lcs_config_value_equal(&agreement.committed, &activation.baseline));
        assert(lcs_config_value_equal(&application.applied, &activation.baseline));
        assert(!agreement.promised && !agreement.accepted.document && !application.pending.document);
        struct stat before, after;
        assert(!fstatat(store.directory_fd, "activation.json", &before, 0));
        assert((before.st_mode & 0777) == 0600);
        close_handles();
        /* Prepared reopen/resume performs no writes. */
        fail_sync = 1;
        assert(resume(&changed, false) == 0 && fail_sync == 1);
        fail_sync = 0;
        assert(!fstatat(store.directory_fd, "activation.json", &after, 0) && before.st_ino == after.st_ino);
        lcs_config_runtime_t runtime;
        assert(lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)) == 0);
        assert(runtime.requires_recovery && runtime.activation.phase == LCS_CONFIG_ACTIVATION_PREPARED);
        assert(!runtime.activation.requests_required && !runtime.requests.store);
        assert(runtime.agreement.base == runtime.base && runtime.application.base == runtime.base);
        assert(lcs_config_value_equal(&runtime.activation.baseline, &agreement.committed));
        lcs_config_runtime_close(&runtime);
        cleanup();
    }
}

static void test_missing_state_and_promises(void)
{
    initialize();
    assert(lcs_config_activation_open(&activation, &store, &config, &frozen, true, error, sizeof(error)) == 0);
    /* An orphan application file cannot justify recreating a lost journal. */
    assert(!lcs_config_store_write(&store, "application.json", "{}", 2, error, sizeof(error)));
    assert(lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error)) == -1);
    assert(strstr(error, "without agreement"));
    assert(faccessat(store.directory_fd, "agreement.json", F_OK, 0) == -1);
    cleanup();

    initialize();
    assert(lcs_config_activation_open(&activation, &store, &config, &frozen, true, error, sizeof(error)) == 0);
    assert(lcs_config_agreement_open(&agreement, &store, &frozen, true, error, sizeof(error)) == 0);
    uint64_t ballot = lcs_config_agreement_next_ballot(&agreement, 0);
    assert(!lcs_config_agreement_prepare(&agreement, 1, agreement.committed.digest, ballot, error, sizeof(error)));
    lcs_config_agreement_close(&agreement);
    assert(lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error)) == -1);
    assert(strstr(error, "nonvoting frozen initialization"));
    assert(faccessat(store.directory_fd, "application.json", F_OK, 0) == -1);
    assert(lcs_config_agreement_open(&agreement, &store, &frozen, false, error, sizeof(error)) == 0);
    assert(agreement.promised == ballot); /* Never reset an unexpected promise. */
    cleanup();

    initialize();
    assert(resume(&config, true) == 0);
    close_handles();
    assert(!unlinkat(store.directory_fd, "application.json", 0));
    assert(resume(&changed, false) == -1 && strstr(error, "catalog missing"));
    assert(faccessat(store.directory_fd, "application.json", F_OK, 0) == -1);
    cleanup();

    initialize();
    assert(resume(&config, true) == 0);
    close_handles();
    assert(!unlinkat(store.directory_fd, "agreement.json", 0));
    assert(resume(&changed, false) == -1 && strstr(error, "without agreement"));
    assert(faccessat(store.directory_fd, "agreement.json", F_OK, 0) == -1);
    cleanup();
}

static void choose(bool dynamic)
{
    static lcs_config_t candidate;
    candidate = config;
    if (dynamic)
        assert(!lcs_resource_config_assign_identities(&config, 0, &config, &candidate, error, sizeof(error)));
    char *document;
    size_t length;
    assert(!lcs_resource_config_encode(&candidate, 1, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, &config, "activate", document, length, error, sizeof(error)));
    free(document);
    uint64_t ballot = lcs_config_agreement_next_ballot(&agreement, 0);
    char parent[65];
    strcpy(parent, agreement.committed.digest);
    assert(!lcs_config_agreement_accept(&agreement, parent, ballot, &value, error, sizeof(error)));
    assert(!lcs_config_agreement_learn(&agreement, parent, ballot, 3, &value, error, sizeof(error)));
    lcs_config_value_clear(&value);
}

static void test_recovered_authority(void)
{
    initialize();
    assert(resume(&config, true) == 0);
    choose(true);
    close_handles();
    lcs_config_runtime_t runtime;
    assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
    assert(runtime.requires_recovery && runtime.agreement.committed.revision == 1);
    assert(runtime.application.applied.revision == 0 && !runtime.application.pending.document);
    assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
    lcs_config_runtime_close(&runtime);
    cleanup();

    /* The generic agreement library can represent legacy test proposals; the
     * activation boundary must never mistake schema-1 revision 1 for activation. */
    initialize();
    assert(resume(&config, true) == 0);
    choose(false);
    close_handles();
    assert(lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)) == -1);
    assert(strstr(error, "dynamic schema"));
    cleanup();
}

static void test_corrupt_foreign_and_missing_intent(void)
{
    initialize();
    assert(resume(&config, true) == 0);
    close_handles();
    lcs_config_t other = config;
    strcpy(other.self_name, "node1");
    assert(resume(&other, false) == -1 && strstr(error, "foreign"));
    char *text;
    size_t length;
    assert(!lcs_config_store_read(&store, "activation.json", &text, &length,
                                 LCS_CONFIG_ACTIVATION_MAX, error, sizeof(error)));
    text[length / 2] ^= 1;
    assert(!lcs_config_store_write(&store, "activation.json", text, length, error, sizeof(error)));
    free(text);
    assert(resume(&config, true) == -1 && strstr(error, "corrupt"));
    assert(!unlinkat(store.directory_fd, "activation.json", 0));
    assert(resume(&config, false) == -1 && strstr(error, "record is missing"));
    assert(resume(&config, true) == -1 && strstr(error, "existing agreement/application"));
    cleanup();
}

static int await_initialization(lcs_config_agreement_worker_t *worker, lcs_config_runtime_t *runtime)
{
    int result = -99;
    for (unsigned i = 0; i < 5000; i++)
    {
        int status = lcs_config_agreement_worker_poll(worker, &runtime->agreement,
                                                      &result, error, sizeof(error));
        if (status)
        {
            if (status == 1)
                assert(result == 0);
            return status;
        }
        usleep(1000);
    }
    assert(!"initialization worker timed out");
    return -1;
}

static void await_stopped(lcs_config_agreement_worker_t *worker)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int status;
        pid_t child = waitpid(worker->pid, &status, WUNTRACED | WNOHANG);
        if (child > 0)
        {
            assert(WIFSTOPPED(status));
            return;
        }
        assert(child == 0 || errno == EINTR);
        usleep(1000);
    }
    assert(!"worker did not reach sync barrier");
}

static lcs_config_bootstrap_t bootstrap_for_test(void)
{
    lcs_config_bootstrap_t bootstrap = { .state = store, .prepared = true, .worker_pipe = -1 };
    return bootstrap;
}

static void test_initialization_worker_success(void)
{
    initialize();
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
    lcs_config_runtime_t runtime = {0};
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    /* Initialization must wait for both active and queued INI preparation. */
    bootstrap.refresh_pending = true;
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)) == 1);
    bootstrap.refresh_pending = false;
    bootstrap.worker_pid = getpid();
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)) == 1);
    bootstrap.worker_pid = 0;
    int sockets[2];
    assert(!socketpair(AF_UNIX, SOCK_STREAM | SOCK_NONBLOCK, 0, sockets));
    stop_sync = 1;
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)));
    assert(!runtime.base && !runtime.activation.baseline.document && runtime.requires_recovery);
    assert(bootstrap.initialization_locked && !bootstrap.prepared);
    await_stopped(&worker);
    /* A stopped fsync worker must not retain daemon sockets. */
    close(sockets[1]);
    char byte;
    assert(read(sockets[0], &byte, 1) == 0);
    close(sockets[0]);
    int operation = -1;
    assert(!lcs_config_agreement_worker_poll(&worker, &runtime.agreement, &operation, error, sizeof(error)));
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)) == 1);
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PREPARE };
    assert(lcs_config_agreement_worker_start(&worker, &runtime.agreement, &job, error, sizeof(error)) == 1);
    lcs_config_bootstrap_request(&bootstrap);
    assert(!bootstrap.refresh_pending);
    assert(!lcs_config_bootstrap_process(&bootstrap, &changed, error, sizeof(error)));
    assert(lcs_config_bootstrap_refresh(&bootstrap, &changed, error, sizeof(error)) == -1);
    lcs_config_agreement_t wrong_handle = {0};
    assert(lcs_config_agreement_worker_poll(&worker, &wrong_handle, &operation, error, sizeof(error)) == -1);
    assert(worker.pid > 0 && !runtime.agreement.poisoned);
    /* Mutation of the caller's INI object cannot change the frozen worker copy. */
    config = changed;
    assert(!kill(worker.pid, SIGCONT));
    stop_sync = 0;
    assert(await_initialization(&worker, &runtime) == 1);
    assert(!worker.pid && !worker.initialization_candidate && worker.pipe_fd == -1);
    assert(runtime.requires_recovery && runtime.activation.phase == LCS_CONFIG_ACTIVATION_PREPARED);
    assert(runtime.activation.requests_required && runtime.requests.store == &bootstrap.state && runtime.requests.base == runtime.base && !runtime.requests.count);
    assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
    assert(runtime.activation.base == runtime.base && runtime.agreement.base == runtime.base &&
           runtime.application.base == runtime.base);
    assert(lcs_config_value_equal(&runtime.activation.baseline, &runtime.agreement.committed));
    assert(lcs_config_value_equal(&runtime.application.applied, &runtime.agreement.committed));
    assert(!runtime.agreement.promised && !runtime.agreement.accepted.document && !runtime.application.pending.document);
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)) == -1);
    /* The same worker is reusable for later explicitly authorized durable jobs. */
    job.revision = 1;
    job.ballot = lcs_config_agreement_next_ballot(&runtime.agreement, 0);
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    assert(!lcs_config_agreement_worker_start(&worker, &runtime.agreement, &job, error, sizeof(error)));
    lcs_config_runtime_t second = {0};
    assert(lcs_config_agreement_worker_start_initialization(&worker, &second, &bootstrap,
                                                            &config, false, error, sizeof(error)) == 1);
    assert(await_initialization(&worker, &runtime) == 1);
    assert(runtime.agreement.promised == job.ballot);
    forbid_parent_io = false;
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
    assert(runtime.agreement.promised == job.ballot);
    lcs_config_runtime_close(&runtime);
    cleanup();
}

static void test_initialization_worker_sync_failures(void)
{
    for (int boundary = 1; boundary <= 10; boundary++)
    {
        initialize();
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_runtime_t runtime = {0};
        lcs_config_agreement_worker_t worker;
        lcs_config_agreement_worker_init(&worker);
        fail_sync = boundary;
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                                &config, true, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == -1);
        fail_sync = 0;
        assert(!worker.pid && !runtime.base && runtime.requires_recovery);
        assert(runtime.agreement.poisoned && runtime.application.poisoned && runtime.activation.poisoned);
        assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                                &changed, true, error, sizeof(error)) == -1);
        assert(!lcs_config_bootstrap_process(&bootstrap, &changed, error, sizeof(error)));
        forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery == (boundary != 1));
        if (boundary != 1)
        {
            assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
            assert(runtime.activation.requests_required && runtime.requests.store && !runtime.requests.count);
        }
        lcs_config_runtime_close(&runtime);
        cleanup();
    }
}

static void test_initialization_worker_cancel_and_stale(void)
{
    for (int mode = 0; mode < 3; mode++)
    {
        initialize();
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_runtime_t runtime = {0};
        lcs_config_agreement_worker_t worker;
        lcs_config_agreement_worker_init(&worker);
        stop_sync = mode == 0 ? 4 : 8;
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                                &config, true, error, sizeof(error)));
        await_stopped(&worker);
        if (mode == 0)
        {
            lcs_config_agreement_worker_cancel(&worker, &runtime.agreement);
            if (worker.pid)
                assert(await_initialization(&worker, &runtime) == -1);
        }
        else if (mode == 1)
        {
            /* A complete on-disk marker without successful child exit is NOT
             * parent-side success; only disk recovery may use that marker. */
            assert(!kill(worker.pid, SIGKILL));
            assert(await_initialization(&worker, &runtime) == -1);
        }
        else
        {
            runtime.agreement.promised = 32; /* A result cannot reset changed parent state. */
            assert(!kill(worker.pid, SIGCONT));
            assert(await_initialization(&worker, &runtime) == -1);
            assert(runtime.agreement.promised == 32);
        }
        stop_sync = 0;
        assert(!worker.pid && !runtime.base && runtime.agreement.poisoned);
        assert(runtime.application.poisoned && runtime.activation.poisoned && bootstrap.initialization_locked);
        forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery && !runtime.agreement.promised);
        assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
        lcs_config_runtime_close(&runtime);
        cleanup();
    }
}

static void test_initialization_worker_refuses_existing_history(void)
{
    for (int mode = 0; mode < 5; mode++)
    {
        initialize();
        if (mode != 0)
        {
            assert(!resume(&config, true));
            if (mode == 2)
                assert(!lcs_config_agreement_prepare(&agreement, 1, agreement.committed.digest,
                    lcs_config_agreement_next_ballot(&agreement, 0), error, sizeof(error)));
            if (mode == 3)
                choose(true);
            close_handles();
        }
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_runtime_t runtime = {0};
        lcs_config_agreement_worker_t worker;
        lcs_config_agreement_worker_init(&worker);
        if (mode == 4)
            fail_sync = 1; /* Legacy prepared state cannot authorize a new request journal. */
        forbid_parent_io = true;
        /* Missing intent without authorization, changed frozen baseline,
         * existing promises, and chosen successors all require disk recovery. */
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
            mode == 1 ? &changed : &config, false, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == -1);
        assert(!runtime.base && runtime.agreement.poisoned && runtime.requests.poisoned);
        fail_sync = 0;
        forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        if (mode == 2)
            assert(runtime.agreement.promised);
        if (mode == 3)
            assert(runtime.agreement.committed.revision == 1);
        if (mode == 4)
            assert(!runtime.activation.requests_required && !runtime.requests.store);
        lcs_config_runtime_close(&runtime);
        cleanup();
    }
}

static void test_initialization_refuses_orphan_request_history(void)
{
    initialize();
    assert(!lcs_config_store_write(&store, "requests.json", "{}", 2, error, sizeof(error)));
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
    lcs_config_runtime_t runtime = {0};
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, true, error, sizeof(error)));
    assert(await_initialization(&worker, &runtime) == -1);
    forbid_parent_io = false;
    assert(!runtime.base && runtime.agreement.poisoned);
    assert(faccessat(store.directory_fd, "activation.json", F_OK, 0) == -1);
    assert(faccessat(store.directory_fd, "agreement.json", F_OK, 0) == -1);
    char *record; size_t length;
    assert(!lcs_config_store_read(&store, "requests.json", &record, &length,
                                  LCS_CONFIG_REQUEST_RECORD_MAX, error, sizeof(error)));
    assert(length == 2 && !memcmp(record, "{}", 2)); free(record);
    lcs_config_runtime_close(&runtime);
    assert(!unlinkat(store.directory_fd, "requests.json", 0));
    cleanup();
}

static void test_required_requests_crash_recovery(void)
{
    for (int boundary = 1; boundary <= 10; boundary++)
    {
        initialize();
        pid_t child = fork(); assert(child >= 0);
        if (!child)
        {
            crash_sync = boundary;
            if (lcs_config_activation_open_requests(&activation, &store, &config, &frozen, true, error, sizeof(error)) ||
                lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error))) _exit(79);
            _exit(80); /* Every expected persistence boundary must have been reached. */
        }
        int status; assert(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 77);
        bool intent = faccessat(store.directory_fd, "activation.json", F_OK, 0) == 0;
        assert(intent == (boundary > 1));
        if (boundary == 10) assert(!faccessat(store.directory_fd, "requests.json", F_OK, 0));
        lcs_config_runtime_t runtime = {0};
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery == intent);
        if (intent)
        {
            assert(runtime.activation.requests_required && runtime.activation.phase == LCS_CONFIG_ACTIVATION_PREPARED);
            assert(runtime.requests.store == &store && runtime.requests.base == runtime.base && !runtime.requests.count);
            assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
        }
        lcs_config_runtime_close(&runtime); cleanup();
    }
}

static void test_required_requests_worker_reopen(void)
{
    initialize();
    assert(!lcs_config_activation_open_requests(&activation, &store, &config, &frozen, true, error, sizeof(error)));
    assert(!lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error)));
    close_handles();
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test(); lcs_config_runtime_t runtime = {0};
    lcs_config_agreement_worker_t worker; lcs_config_agreement_worker_init(&worker);
    fail_sync = 1; forbid_parent_io = true; /* Reopen must only read verified empty history. */
    assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap, &config, false, error, sizeof(error)));
    assert(await_initialization(&worker, &runtime) == 1);
    assert(runtime.activation.requests_required && runtime.requests.store == &bootstrap.state && !runtime.requests.count);
    forbid_parent_io = false; fail_sync = 0;
    lcs_config_runtime_close(&runtime); cleanup();
}

static void test_prepared_requests_never_reinitialized(void)
{
    for (unsigned mode = 0; mode < 3; mode++)
    {
        initialize();
        assert(!lcs_config_activation_open_requests(&activation, &store, &config, &frozen, true, error, sizeof(error)));
        assert(!lcs_config_activation_resume(&activation, &agreement, &application, error, sizeof(error)));
        assert(activation.requests_required); close_handles();
        lcs_config_runtime_t runtime = {0};
        assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)) && runtime.requests.store);
        if (mode == 2)
        {
            lcs_config_t successor;
            assert(!lcs_resource_config_assign_identities(runtime.base, 0, runtime.base, &successor, error, sizeof(error)));
            char *document; size_t length;
            assert(!lcs_resource_config_encode(&successor, 1, &document, &length, error, sizeof(error)));
            lcs_config_value_t value = {0};
            assert(!lcs_config_value_init(&value, runtime.base, "reserved-before-prepare", document, length, error, sizeof(error)));
            assert(!lcs_config_requests_reserve(&runtime.requests, &runtime.agreement, 0, runtime.agreement.committed.digest,
                &value, error, sizeof(error)));
            free(document); lcs_config_value_clear(&value);
        }
        lcs_config_runtime_close(&runtime);
        if (mode == 0) assert(!unlinkat(store.directory_fd, "requests.json", 0));
        if (mode == 1) assert(!lcs_config_store_write(&store, "requests.json", "{}", 2, error, sizeof(error)));
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_agreement_worker_t worker; lcs_config_agreement_worker_init(&worker);
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap, &config, true, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == -1 && runtime.requests.poisoned);
        forbid_parent_io = false; lcs_config_runtime_close(&runtime);
        if (mode < 2)
        {
            assert(lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)) < 0);
            if (!mode) assert(faccessat(store.directory_fd, "requests.json", F_OK, 0) < 0 && errno == ENOENT);
            else
            {
                char *record; size_t length;
                assert(!lcs_config_store_read(&store, "requests.json", &record, &length, LCS_CONFIG_REQUEST_RECORD_MAX, error, sizeof(error)));
                assert(length == 2 && !memcmp(record, "{}", 2)); free(record);
            }
        }
        else
        {
            assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
            assert(runtime.requests.count == 1 && !runtime.agreement.promised);
            assert(!strcmp(runtime.requests.records[0].id, "reserved-before-prepare"));
        }
        lcs_config_runtime_close(&runtime); cleanup();
    }
}

static lcs_config_barrier_round_t complete_barrier(const lcs_config_activation_t *intent,
                                                   const lcs_config_agreement_t *journal,
                                                   const lcs_config_application_t *catalog)
{
    lcs_config_barrier_round_t round;
    assert(!lcs_config_barrier_begin(&round, intent, journal, catalog, error, sizeof(error)));
    for (size_t i = 0; i < intent->base->node_count; i++)
        assert(!lcs_config_barrier_ready(&round, i, intent->membership, intent->baseline.digest, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    return round;
}

static void test_all_member_barrier(void)
{
    initialize();
    lcs_config_barrier_round_t round = {0};
    assert(lcs_config_barrier_begin(&round, &activation, &agreement, &application, error, sizeof(error)) == -1);
    assert(!resume(&config, true));
    assert(lcs_config_barrier_local_ready(&activation, &agreement, &application));
    assert(!lcs_config_barrier_begin(&round, &activation, &agreement, &application, error, sizeof(error)));
    assert(round.required == 7 && round.ready == 1);
    lcs_config_barrier_t barrier = {0};
    assert(lcs_config_barrier_open(&barrier, &activation, &agreement, &application, NULL, error, sizeof(error)) == 1);
    assert(!lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    assert(!lcs_config_barrier_ready(&round, 1, activation.membership, activation.baseline.digest, error, sizeof(error)));
    assert(round.ready == 3); /* A majority, including both full nodes, is insufficient. */
    assert(lcs_config_barrier_seal(&round, error, sizeof(error)) == -1);
    assert(lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)) == -1);
    assert(faccessat(store.directory_fd, "barrier.json", F_OK, 0) == -1);
    assert(!lcs_config_barrier_ready(&round, 2, activation.membership, activation.baseline.digest, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    assert(!lcs_config_barrier_ready(&round, 2, activation.membership, activation.baseline.digest, error, sizeof(error)));
    assert(lcs_config_barrier_ready(&round, 3, activation.membership, activation.baseline.digest, error, sizeof(error)) == -1);
    assert(round.sealed && !round.failed); /* Invalid later traffic cannot erase a sealed proof. */
    assert(!lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)));
    assert(lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    struct stat before, after;
    assert(!fstatat(store.directory_fd, "barrier.json", &before, 0) && (before.st_mode & 0777) == 0600);
    lcs_config_barrier_close(&barrier);
    fail_sync = 1;
    assert(!lcs_config_barrier_open(&barrier, &activation, &agreement, &application, NULL, error, sizeof(error)));
    assert(fail_sync == 1);
    fail_sync = 0;
    assert(!fstatat(store.directory_fd, "barrier.json", &after, 0) && before.st_ino == after.st_ino);
    lcs_config_barrier_round_t bad;
    assert(!lcs_config_barrier_begin(&bad, &activation, &agreement, &application, error, sizeof(error)));
    assert(lcs_config_barrier_ready(&bad, 1, activation.membership, "foreign", error, sizeof(error)) == -1);
    assert(bad.failed && lcs_config_barrier_seal(&bad, error, sizeof(error)) == -1);
    assert(!lcs_config_barrier_begin(&bad, &activation, &agreement, &application, error, sizeof(error)));
    assert(lcs_config_barrier_ready(&bad, 1, "foreign", activation.baseline.digest, error, sizeof(error)) == -1);
    assert(!lcs_config_barrier_begin(&bad, &activation, &agreement, &application, error, sizeof(error)));
    assert(lcs_config_barrier_ready(&bad, 32, activation.membership, activation.baseline.digest, error, sizeof(error)) == -1);
    choose(true);
    assert(!lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    assert(!lcs_config_application_begin(&application, &agreement, error, sizeof(error)));
    assert(!lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    assert(!lcs_config_application_finish(&application, application.pending.digest, error, sizeof(error)));
    assert(lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    application.poisoned = true;
    assert(!lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
    application.poisoned = false;
    close_handles();
    lcs_config_runtime_t runtime;
    assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
    assert(runtime.requires_recovery && runtime.barrier.ready == 7 && runtime.application.applied.revision == 1);
    assert(lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation, &runtime.agreement, &runtime.application));
    lcs_config_runtime_close(&runtime);
    cleanup();
}

static void test_barrier_limits_and_missing_history(void)
{
    const int counts[] = {1, 3, 32};
    for (size_t n = 0; n < sizeof(counts) / sizeof(counts[0]); n++)
    {
        int count = counts[n];
        initialize();
        config.node_count = (size_t)count;
        strcpy(config.self_name, count == 3 ? "node02" : "node00");
        for (int i = 0; i < count; i++)
        {
            snprintf(config.nodes[i].name, sizeof(config.nodes[i].name), "node%02d", i);
            strcpy(config.nodes[i].address, "127.0.0.1");
            config.nodes[i].port = 3322;
            config.nodes[i].role = count > 1 && i == count - 1 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
        }
        assert(!lcs_config_finalize(&config, error, sizeof(error)));
        assert(!resume(&config, true));
        lcs_config_barrier_round_t round = complete_barrier(&activation, &agreement, &application);
        assert(round.ready == (count == 32 ? UINT32_MAX : count == 3 ? 7 : 1));
        lcs_config_barrier_t barrier = {0};
        assert(!lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)));
        assert(lcs_config_barrier_can_vote(&barrier, &activation, &agreement, &application));
        assert(!unlinkat(store.directory_fd, "barrier.json", 0));
        uint64_t ballot = lcs_config_agreement_next_ballot(&agreement, 0);
        assert(!lcs_config_agreement_prepare(&agreement, 1, agreement.committed.digest, ballot, error, sizeof(error)));
        lcs_config_barrier_close(&barrier);
        assert(lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)) == -1);
        assert(faccessat(store.directory_fd, "barrier.json", F_OK, 0) == -1);
        assert(agreement.promised == ballot);
        cleanup();
    }
    initialize();
    assert(!lcs_config_store_write(&store, "barrier.json", "{}", 2, error, sizeof(error)));
    assert(resume(&config, true) == -1);
    assert(faccessat(store.directory_fd, "activation.json", F_OK, 0) == -1);
    lcs_config_runtime_t runtime;
    assert(lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)) == -1);
    assert(strstr(error, "without its frozen initialization"));
    cleanup();
}

static void test_barrier_worker(void)
{
    for (int mode = 0; mode < 6; mode++)
    {
        initialize();
        assert(!resume(&config, true));
        close_handles();
        lcs_config_runtime_t runtime;
        assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
        lcs_config_barrier_round_t round = complete_barrier(&runtime.activation, &runtime.agreement, &runtime.application);
        lcs_config_agreement_worker_t worker;
        lcs_config_agreement_worker_init(&worker);
        stop_sync = mode < 2 ? 0 : 2;
        fail_sync = mode < 2 ? mode + 1 : 0;
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_barrier(&worker, &runtime, &round, error, sizeof(error)));
        assert(!lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation, &runtime.agreement, &runtime.application));
        if (mode >= 2)
        {
            await_stopped(&worker);
            int operation = -1;
            assert(!lcs_config_agreement_worker_poll(&worker, &runtime.agreement, &operation, error, sizeof(error)));
            lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PREPARE };
            assert(lcs_config_agreement_worker_start(&worker, &runtime.agreement, &job, error, sizeof(error)) == 1);
            assert(lcs_config_agreement_worker_start_barrier(&worker, &runtime, &round, error, sizeof(error)) == 1);
            if (mode == 2)
            {
                lcs_config_agreement_worker_cancel(&worker, &runtime.agreement);
                if (worker.pid)
                    assert(await_initialization(&worker, &runtime) == -1);
            }
            else
            {
                if (mode == 3)
                    assert(!kill(worker.pid, SIGKILL));
                else
                {
                    if (mode == 4)
                        runtime.agreement.promised = 32;
                    round.failed = true; /* Child owns the sealed pre-fork receipt copy. */
                    assert(!kill(worker.pid, SIGCONT));
                }
            }
        }
        if (mode != 2)
            assert(await_initialization(&worker, &runtime) == (mode == 5 ? 1 : -1));
        fail_sync = stop_sync = 0;
        assert(!worker.pid);
        if (mode != 5)
        {
            assert(runtime.barrier.poisoned && runtime.agreement.poisoned &&
                   runtime.application.poisoned && runtime.activation.poisoned);
            if (mode == 4)
                assert(runtime.agreement.promised == 32);
        }
        assert(lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation,
                                           &runtime.agreement, &runtime.application) == (mode == 5));
        forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery && !runtime.agreement.promised);
        assert(lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation,
                                           &runtime.agreement, &runtime.application) == (mode != 0));
        if (mode == 0)
        {
            round = complete_barrier(&runtime.activation, &runtime.agreement, &runtime.application);
            assert(!lcs_config_agreement_worker_start_barrier(&worker, &runtime, &round, error, sizeof(error)));
            assert(await_initialization(&worker, &runtime) == 1);
        }
        lcs_config_runtime_close(&runtime);
        cleanup();
    }
}

static void test_barrier_worker_preserves_newer_disk_state(void)
{
    initialize();
    assert(!resume(&config, true));
    close_handles();
    lcs_config_runtime_t runtime;
    assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
    lcs_config_barrier_round_t round = complete_barrier(&runtime.activation, &runtime.agreement, &runtime.application);
    /* The parent's pristine in-memory snapshot cannot authorize resetting a
     * promise found when the storage child reopens the actual journal. */
    lcs_config_agreement_t disk = {0};
    assert(!lcs_config_agreement_open(&disk, &store, runtime.base, false, error, sizeof(error)));
    uint64_t ballot = lcs_config_agreement_next_ballot(&disk, 0);
    assert(!lcs_config_agreement_prepare(&disk, 1, disk.committed.digest, ballot, error, sizeof(error)));
    lcs_config_agreement_close(&disk);
    assert(!runtime.agreement.promised);
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_barrier(&worker, &runtime, &round, error, sizeof(error)));
    assert(await_initialization(&worker, &runtime) == -1);
    forbid_parent_io = false;
    assert(faccessat(store.directory_fd, "barrier.json", F_OK, 0) == -1);
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
    assert(runtime.agreement.promised == ballot);
    assert(lcs_config_agreement_worker_start_barrier(&worker, &runtime, &round, error, sizeof(error)) == -1);
    assert(!lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation, &runtime.agreement, &runtime.application));
    lcs_config_runtime_close(&runtime);
    cleanup();

    initialize();
    assert(!resume(&config, true));
    round = complete_barrier(&activation, &agreement, &application);
    lcs_config_barrier_t barrier = {0};
    assert(!lcs_config_barrier_open(&barrier, &activation, &agreement, &application, &round, error, sizeof(error)));
    close_handles();
    memset(&runtime, 0, sizeof(runtime));
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
                                                            &config, false, error, sizeof(error)));
    assert(await_initialization(&worker, &runtime) == -1);
    forbid_parent_io = false;
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)));
    assert(lcs_config_barrier_can_vote(&runtime.barrier, &runtime.activation, &runtime.agreement, &runtime.application));
    lcs_config_runtime_close(&runtime);
    cleanup();
}

static lcs_config_initialization_receipt_t freeze_receipt(const lcs_config_initialization_round_t *round, size_t node)
{
    lcs_config_initialization_receipt_t receipt = {
        .requester_instance = round->instances[round->self], .attempt = round->attempt,
        .responder_instance = round->instances[node], .status = LCS_INITIALIZATION_FROZEN };
    memcpy(receipt.membership, round->membership, sizeof(receipt.membership));
    memcpy(receipt.baseline, round->baseline, sizeof(receipt.baseline));
    return receipt;
}

static void freeze_all(lcs_config_initialization_round_t *round)
{
    for (size_t i = 0; i < round->count; i++)
    {
        if (i == round->self) continue;
        lcs_config_initialization_receipt_t query;
        assert(!lcs_config_initialization_query(round, i, round->instances[i], 101, &query, error, sizeof(error)));
        assert(!query.status && query.responder_instance == round->instances[i] && query.attempt == round->attempt);
        lcs_config_initialization_receipt_t receipt = freeze_receipt(round, i);
        assert(!lcs_config_initialization_receipt(round, i, round->instances[i], &receipt, 101, error, sizeof(error)));
    }
    assert(round->sealed);
}

static void test_initialization_round(void)
{
    initialize();
    uint64_t instances[LCS_MAX_NODES] = {11, 22, 33};
    lcs_config_initialization_round_t round = {0};
    /* Unknown/old/offline quorum-only members prevent even beginning a round. */
    instances[2] = 0;
    assert(lcs_config_initialization_begin(&round, &config, instances, 7, 1, 100, error, sizeof(error)) == -1);
    assert(!round.active);
    instances[2] = 33;
    assert(lcs_config_initialization_begin(&round, &config, instances, 7, 1, UINT64_MAX, error, sizeof(error)) == -1);
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 1, 100, error, sizeof(error)));
    assert(round.required == 7 && round.frozen == 1 && !round.sealed);
    assert(lcs_config_initialization_begin(&round, &config, instances, 7, 2, 101, error, sizeof(error)) == -1);
    assert(!round.failed && round.attempt == 1);
    lcs_config_initialization_receipt_t receipt = freeze_receipt(&round, 1);
    /* Even an exactly matching receipt is unsolicited until a query is registered. */
    assert(lcs_config_initialization_receipt(&round, 1, 22, &receipt, 101, error, sizeof(error)) == 1);
    lcs_config_initialization_receipt_t query;
    assert(!lcs_config_initialization_query(&round, 1, 22, 101, &query, error, sizeof(error)));
    receipt.attempt++;
    assert(lcs_config_initialization_receipt(&round, 1, 22, &receipt, 101, error, sizeof(error)) == 1);
    receipt = freeze_receipt(&round, 1); receipt.requester_instance++;
    assert(lcs_config_initialization_receipt(&round, 1, 22, &receipt, 101, error, sizeof(error)) == 1);
    receipt = freeze_receipt(&round, 1); receipt.responder_instance++;
    assert(lcs_config_initialization_receipt(&round, 1, 23, &receipt, 101, error, sizeof(error)) == 1);
    receipt = freeze_receipt(&round, 1); receipt.status = LCS_INITIALIZATION_BUSY;
    assert(!lcs_config_initialization_receipt(&round, 1, 22, &receipt, 101, error, sizeof(error)));
    assert(!lcs_config_initialization_poll(&round, instances, 7, 101, error, sizeof(error)));
    receipt.status = LCS_INITIALIZATION_FROZEN;
    assert(!lcs_config_initialization_receipt(&round, 1, 22, &receipt, 101, error, sizeof(error)));
    assert(!lcs_config_initialization_receipt(&round, 1, 22, &receipt, 102, error, sizeof(error)));
    assert(round.frozen == 3 && !round.sealed); /* A quorum cannot authorize conversion. */
    assert(lcs_config_initialization_query(&round, 1, 22, 102, &query, error, sizeof(error)) == 1);
    assert(!lcs_config_initialization_query(&round, 2, 33, 102, &query, error, sizeof(error)));
    receipt = freeze_receipt(&round, 2);
    assert(!lcs_config_initialization_receipt(&round, 2, 33, &receipt, 102, error, sizeof(error)));
    assert(!lcs_config_initialization_authorize(&round, &config, instances, 7, 103, error, sizeof(error)));
    assert(lcs_config_initialization_authorize(&round, &changed, instances, 7, 103, error, sizeof(error)) == -1);
    assert(round.failed);
    assert(lcs_config_initialization_begin(&round, &config, instances, 7, 1, 104, error, sizeof(error)) == -1);
    for (unsigned mode = 0; mode < 6; mode++)
    {
        assert(!lcs_config_initialization_begin(&round, &config, instances, 7, mode + 2, 100, error, sizeof(error)));
        freeze_all(&round);
        uint64_t live[LCS_MAX_NODES]; memcpy(live, instances, sizeof(live));
        if (mode == 0) live[2] = 0;
        if (mode == 1) live[2]++;
        if (mode == 2) live[0]++;
        if (mode <= 3)
            assert(lcs_config_initialization_poll(&round, live, mode == 3 ? 8 : 7, 103, error, sizeof(error)) == -1);
        else if (mode == 4)
            assert(lcs_config_initialization_poll(&round, live, 7, round.deadline_ms, error, sizeof(error)) == -1);
        else
        {
            receipt = freeze_receipt(&round, 2); receipt.status = LCS_INITIALIZATION_BUSY;
            assert(lcs_config_initialization_receipt(&round, 2, 33, &receipt, 103, error, sizeof(error)) == -1);
        }
        assert(round.failed && lcs_config_initialization_authorize(&round, &config, instances, 7, 103, error, sizeof(error)) == -1);
    }
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 8, 100, error, sizeof(error)));
    assert(!lcs_config_initialization_query(&round, 2, 33, 101, &query, error, sizeof(error)));
    receipt = freeze_receipt(&round, 2); receipt.baseline[0] ^= 1;
    assert(lcs_config_initialization_receipt(&round, 2, 33, &receipt, 101, error, sizeof(error)) == -1);
    assert(round.failed);
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 9, 100, error, sizeof(error)));
    assert(lcs_config_initialization_query(&round, 2, 34, 101, &query, error, sizeof(error)) == -1);
    assert(round.failed && !round.queried);
    /* Collection/authorization never writes any initialization record. */
    assert(faccessat(store.directory_fd, "activation.json", F_OK, 0) == -1 && errno == ENOENT);
    cleanup();
}

static void test_initialization_round_worker(void)
{
    initialize();
    uint64_t instances[LCS_MAX_NODES] = {11, 22, 33};
    lcs_config_initialization_round_t round = {0};
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
    lcs_config_runtime_t runtime = {0};
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 1, 100, error, sizeof(error)));
    assert(lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &config, &round, instances, 7, 101, error, sizeof(error)) == -1);
    assert(!worker.pid && !bootstrap.initialization_locked && !runtime.requires_recovery && !round.consumed);
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 2, 100, error, sizeof(error)));
    freeze_all(&round);
    bootstrap.refresh_pending = true;
    assert(lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &config, &round, instances, 7, 101, error, sizeof(error)) == 1);
    assert(!round.consumed && !round.failed);
    bootstrap.refresh_pending = false;
    assert(lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &changed, &round, instances, 7, 101, error, sizeof(error)) == -1);
    assert(!worker.pid && !bootstrap.initialization_locked && !runtime.requires_recovery && !round.consumed);
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 3, 100, error, sizeof(error)));
    freeze_all(&round);
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &config, &round, instances, 7, 101, error, sizeof(error)));
    assert(round.consumed && bootstrap.initialization_locked);
    assert(await_initialization(&worker, &runtime) == 1);
    assert(runtime.activation.requests_required && runtime.requests.store && !runtime.requests.count);
    assert(lcs_config_initialization_authorize(&round, &config, instances, 7, 102, error, sizeof(error)) == -1);
    forbid_parent_io = false;
    lcs_config_runtime_close(&runtime);
    cleanup();
}

static void assert_no_initialized_history(void)
{
    const char *names[] = {"agreement.json", "application.json", "requests.json", "barrier.json"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        assert(faccessat(store.directory_fd, names[i], F_OK, 0) == -1 && errno == ENOENT);
}

static void test_durable_freeze_worker_and_promotion(void)
{
    initialize();
    lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
    lcs_config_runtime_t runtime = {0};
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    bootstrap.refresh_pending = true;
    assert(lcs_config_agreement_worker_start_freeze(&worker, &runtime, &bootstrap, &config, error, sizeof(error)) == 1);
    bootstrap.refresh_pending = false;
    stop_sync = 1; forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_freeze(&worker, &runtime, &bootstrap, &config, error, sizeof(error)));
    assert(bootstrap.initialization_locked && !bootstrap.prepared && runtime.requires_recovery && !runtime.base);
    await_stopped(&worker);
    int operation = -1;
    assert(!lcs_config_agreement_worker_poll(&worker, &runtime.agreement, &operation, error, sizeof(error)));
    assert(!runtime.activation.store); /* No acknowledgement while fsync/child is pending. */
    assert(!kill(worker.pid, SIGCONT)); stop_sync = 0;
    assert(await_initialization(&worker, &runtime) == 1);
    assert(runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN && runtime.activation.requests_required);
    assert(!runtime.agreement.store && !runtime.application.store && !runtime.requests.store && !runtime.barrier.store);
    assert(!lcs_config_barrier_local_ready(&runtime.activation, &runtime.agreement, &runtime.application));
    forbid_parent_io = false;
    assert_no_initialized_history();
    assert(lcs_config_activation_resume(&runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)) == -1);
    assert_no_initialized_history();
    /* Restart retains frozen definitions despite edited INI; it never promotes
     * the freeze or creates history just because all records are absent. */
    lcs_config_runtime_close(&runtime);
    fail_sync = 1;
    assert(!lcs_config_runtime_open(&runtime, &bootstrap.state, &changed, error, sizeof(error)));
    assert(fail_sync == 1); fail_sync = 0;
    assert(runtime.requires_recovery && runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN);
    assert(!strcmp(runtime.base->resources[0].address, config.resources[0].address));
    assert_no_initialized_history();
    assert(lcs_config_bootstrap_refresh(&bootstrap, &changed, error, sizeof(error)) == -1);
    /* A changed candidate or incomplete round cannot promote a recovered freeze. */
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap, &changed, true, error, sizeof(error)) == -1);
    assert(lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap, &config, false, error, sizeof(error)) == -1);
    uint64_t instances[LCS_MAX_NODES] = {11, 22, 33};
    lcs_config_initialization_round_t round = {0};
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 1, 100, error, sizeof(error)));
    assert(lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &config, &round, instances, 7, 101, error, sizeof(error)) == -1);
    assert_no_initialized_history();
    assert(!lcs_config_initialization_begin(&round, &config, instances, 7, 2, 100, error, sizeof(error)));
    freeze_all(&round);
    forbid_parent_io = true;
    assert(!lcs_config_agreement_worker_start_initialization_round(&worker, &runtime, &bootstrap,
        &config, &round, instances, 7, 101, error, sizeof(error)));
    assert(round.consumed && runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN);
    assert(await_initialization(&worker, &runtime) == 1);
    assert(runtime.activation.phase == LCS_CONFIG_ACTIVATION_PREPARED && runtime.requests.store && !runtime.requests.count);
    forbid_parent_io = false;
    lcs_config_runtime_close(&runtime); cleanup();
}

static void test_freeze_failures_and_promotion_crashes(void)
{
    for (int mode = 0; mode < 2; mode++)
    for (int boundary = 1; boundary <= 2; boundary++)
    {
        initialize();
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_runtime_t runtime = {0};
        lcs_config_agreement_worker_t worker; lcs_config_agreement_worker_init(&worker);
        if (mode) crash_sync = boundary; else fail_sync = boundary;
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_freeze(&worker, &runtime, &bootstrap, &config, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == -1);
        assert(bootstrap.initialization_locked && runtime.requires_recovery && runtime.activation.poisoned && !runtime.base);
        fail_sync = crash_sync = 0; forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery == (boundary == 2));
        if (boundary == 2) assert(runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN);
        assert_no_initialized_history();
        lcs_config_runtime_close(&runtime); cleanup();
    }
    for (int mode = 0; mode < 2; mode++)
    for (int boundary = 1; boundary <= 10; boundary++)
    {
        initialize();
        lcs_config_bootstrap_t bootstrap = bootstrap_for_test();
        lcs_config_runtime_t runtime = {0};
        lcs_config_agreement_worker_t worker; lcs_config_agreement_worker_init(&worker);
        assert(!lcs_config_activation_freeze(&runtime.activation, &bootstrap.state, &config, &frozen, error, sizeof(error)));
        lcs_config_activation_close(&runtime.activation);
        assert(!lcs_config_runtime_open(&runtime, &bootstrap.state, &config, error, sizeof(error)));
        bootstrap.initialization_locked = true; bootstrap.prepared = false;
        if (mode) crash_sync = boundary; else fail_sync = boundary;
        forbid_parent_io = true;
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap, &config, true, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == -1);
        assert(runtime.activation.poisoned && bootstrap.initialization_locked);
        fail_sync = crash_sync = 0; forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        assert(runtime.requires_recovery && !strcmp(runtime.base->resources[0].address, config.resources[0].address));
        if (boundary == 1)
        { assert(runtime.activation.phase == LCS_CONFIG_ACTIVATION_FROZEN); assert_no_initialized_history(); }
        else
            assert(runtime.activation.phase == LCS_CONFIG_ACTIVATION_PREPARED && runtime.requests.store && !runtime.requests.count);
        lcs_config_runtime_close(&runtime); cleanup();
    }
}

static void test_freeze_conflicts(void)
{
    const char *names[] = {"agreement.json", "application.json", "barrier.json", "requests.json"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
    {
        initialize();
        assert(!lcs_config_activation_freeze(&activation, &store, &config, &frozen, error, sizeof(error)));
        lcs_config_activation_close(&activation);
        fail_sync = 1; /* Exact retries recover without rewriting the freeze. */
        assert(!lcs_config_activation_freeze(&activation, &store, &config, &frozen, error, sizeof(error)));
        assert(fail_sync == 1); fail_sync = 0;
        lcs_config_activation_close(&activation);
        assert(lcs_config_activation_freeze(&activation, &store, &changed, &frozen, error, sizeof(error)) == -1);
        assert(lcs_config_activation_open_requests(&activation, &store, &changed, &frozen, true, error, sizeof(error)) == -1);
        assert_no_initialized_history();
        assert(!lcs_config_store_write(&store, names[i], "{}", 2, error, sizeof(error)));
        lcs_config_runtime_t runtime = {0};
        assert(lcs_config_runtime_open(&runtime, &store, &config, error, sizeof(error)) == -1);
        assert(!runtime.base);
        assert(lcs_config_activation_freeze(&activation, &store, &config, &frozen, error, sizeof(error)) == -1);
        cleanup();
    }
}

int main(void)
{
    test_parent = getpid();
    test_write_boundaries();
    test_missing_state_and_promises();
    test_recovered_authority();
    test_corrupt_foreign_and_missing_intent();
    test_initialization_round();
    test_initialization_round_worker();
    test_durable_freeze_worker_and_promotion();
    test_freeze_failures_and_promotion_crashes();
    test_freeze_conflicts();
    test_initialization_worker_success();
    test_initialization_worker_sync_failures();
    test_initialization_worker_cancel_and_stale();
    test_initialization_worker_refuses_existing_history();
    test_initialization_refuses_orphan_request_history();
    test_required_requests_crash_recovery();
    test_prepared_requests_never_reinitialized();
    test_required_requests_worker_reopen();
    test_all_member_barrier();
    test_barrier_limits_and_missing_history();
    test_barrier_worker();
    test_barrier_worker_preserves_newer_disk_state();
    puts("configuration initialization, durable activation barrier and worker unit tests passed");
    return 0;
}
