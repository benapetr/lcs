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
static int stop_sync;
static pid_t test_parent;
static bool forbid_parent_io;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_parent_io || getpid() != test_parent);
    if (stop_sync && !--stop_sync)
        raise(SIGSTOP);
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
    const char *names[] = {"activation.json", "agreement.json", "application.json",
        ".activation.json.tmp", ".agreement.json.tmp", ".application.json.tmp", "lock"};
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
    for (int boundary = 1; boundary <= 8; boundary++)
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
            assert(!strcmp(runtime.base->resources[0].address, "192.0.2.10/32"));
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
            fail_sync = 1; /* Reopening pristine prepared state must not write. */
        forbid_parent_io = true;
        /* Missing intent without authorization, changed frozen baseline,
         * existing promises, and chosen successors all require disk recovery. */
        assert(!lcs_config_agreement_worker_start_initialization(&worker, &runtime, &bootstrap,
            mode == 1 ? &changed : &config, false, error, sizeof(error)));
        assert(await_initialization(&worker, &runtime) == (mode == 4 ? 1 : -1));
        if (mode != 4)
            assert(!runtime.base && runtime.agreement.poisoned);
        else
            assert(runtime.base && !runtime.agreement.poisoned && !runtime.agreement.promised);
        fail_sync = 0;
        forbid_parent_io = false;
        lcs_config_runtime_close(&runtime);
        assert(!lcs_config_runtime_open(&runtime, &store, &changed, error, sizeof(error)));
        if (mode == 2)
            assert(runtime.agreement.promised);
        if (mode == 3)
            assert(runtime.agreement.committed.revision == 1);
        lcs_config_runtime_close(&runtime);
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
    test_initialization_worker_success();
    test_initialization_worker_sync_failures();
    test_initialization_worker_cancel_and_stale();
    test_initialization_worker_refuses_existing_history();
    puts("resumable configuration initialization and background worker unit tests passed");
    return 0;
}
