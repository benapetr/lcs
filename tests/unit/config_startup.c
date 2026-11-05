// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_startup.h"
#include "vip.h"
#include "systemd_service.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/prctl.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static lcs_config_store_t store;
static lcs_config_runtime_t runtime;
static char directory[96], hook[128], trace[128], error[256];
static pid_t parent;
static bool forbid_io;
int __real_fsync(int fd);
int __wrap_fsync(int fd) { assert(!forbid_io || getpid() != parent); return __real_fsync(fd); }

static lcs_config_value_t successor(unsigned action)
{
    lcs_config_t *configs = malloc(3 * sizeof(*configs)); assert(configs);
    uint64_t revision;
    assert(!lcs_resource_config_decode(runtime.agreement.committed.document, runtime.agreement.committed.length,
        runtime.base, &configs[0], &revision, error, sizeof(error)));
    configs[1] = configs[0];
    if (action == 1)
    {
        strcpy(configs[1].resources[0].systemd_unit, "applied.service");
        strcpy(configs[1].resources[1].address, "192.0.2.20/32");
    }
    if (action == 2) configs[1].resource_count = 0;
    assert(!lcs_resource_config_assign_identities(&configs[0], revision, &configs[1], &configs[2], error, sizeof(error)));
    char *document; size_t length;
    assert(!lcs_resource_config_encode(&configs[2], revision + 1, &document, &length, error, sizeof(error)));
    char request[32]; snprintf(request, sizeof(request), "startup-%llu", (unsigned long long)(revision + 1));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, runtime.base, request, document, length, error, sizeof(error)));
    assert(!lcs_config_agreement_learn(&runtime.agreement, runtime.agreement.committed.digest,
        (revision + 1) * 32, 3, &value, error, sizeof(error)));
    free(document); free(configs);
    return value;
}

static void setup(bool pending, bool quorum_only)
{
    forbid_io = false;
    memset(&runtime, 0, sizeof(runtime));
    strcpy(directory, "/tmp/lcs-config-startup.XXXXXX"); assert(mkdtemp(directory));
    snprintf(hook, sizeof(hook), "%s/hook.sh", directory);
    snprintf(trace, sizeof(trace), "%s/trace", directory);
    FILE *script = fopen(hook, "w"); assert(script);
    fputs("#!/bin/sh\n"
          "if [ -n \"$LCS_STARTUP_HOOK_PID\" ]; then echo \"$$\" > \"$LCS_STARTUP_HOOK_PID\"; fi\n"
          "printf '%s|%s|%s|%s|%s|%s|%s|%s\\n' \"$LCS_RESOURCE\" \"$LCS_EVENT\" \"$LCS_CONFIG_REVISION\" \"$LCS_RECOVERY\" \"$LCS_EPOCH\" \"$LCS_LEASE_ID\" \"$LCS_ADDRESS\" \"$LCS_SYSTEMD_UNIT\" >> \"$LCS_STARTUP_TRACE\"\n"
          "if [ \"$LCS_EVENT\" = \"$LCS_STARTUP_FAIL_EVENT\" ]; then exit 1; fi\n"
          "if [ \"$LCS_EVENT\" = \"$LCS_STARTUP_SLEEP_EVENT\" ]; then sleep 60; fi\n", script);
    assert(!fclose(script) && !chmod(hook, 0700));
    setenv("LCS_STARTUP_TRACE", trace, 1); setenv("LCS_VIP_DRY_RUN", "1", 1); setenv("LCS_SYSTEMD_DRY_RUN", "1", 1);
    lcs_config_t *cfg = malloc(sizeof(*cfg)), *frozen = malloc(sizeof(*frozen)); assert(cfg && frozen);
    lcs_config_init_defaults(cfg); strcpy(cfg->cluster_name, "startup");
    strcpy(cfg->self_name, quorum_only ? "node2" : "node0"); cfg->node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(cfg->nodes[i].name, sizeof(cfg->nodes[i].name), "node%zu", i);
        strcpy(cfg->nodes[i].address, "127.0.0.1"); cfg->nodes[i].port = 3322;
        cfg->nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    cfg->resource_count = 2;
    strcpy(cfg->resources[0].name, "a_parent"); cfg->resources[0].type = LCS_RESOURCE_SERVICE;
    strcpy(cfg->resources[0].systemd_unit, "baseline.service");
    strcpy(cfg->resources[1].name, "b_child"); cfg->resources[1].type = LCS_RESOURCE_VIP;
    strcpy(cfg->resources[1].address, "192.0.2.10/32"); strcpy(cfg->resources[1].interface, "eth0");
    strcpy(cfg->resources[1].depends_on_names[0], "a_parent"); cfg->resources[1].depends_on_count = 1;
    for (size_t i = 0; i < 2; i++)
    { strcpy(cfg->resources[i].pre_stop, hook); strcpy(cfg->resources[i].post_stop, hook); }
    assert(!lcs_config_finalize(cfg, error, sizeof(error)));
    assert(!lcs_config_store_open(&store, directory, error, sizeof(error)));
    assert(!lcs_config_activation_open(&runtime.activation, &store, cfg, frozen, true, error, sizeof(error)));
    runtime.base = frozen;
    assert(!lcs_config_activation_resume(&runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)));
    lcs_config_barrier_round_t round;
    assert(!lcs_config_barrier_begin(&round, &runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) assert(!lcs_config_barrier_ready(&round, i, round.membership, round.baseline, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    assert(!lcs_config_barrier_open(&runtime.barrier, &runtime.activation, &runtime.agreement, &runtime.application, &round, error, sizeof(error)));
    for (unsigned action = 0; action < 2; action++)
    {
        lcs_config_value_t value = successor(action);
        assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
        assert(!lcs_config_application_finish(&runtime.application, value.digest, error, sizeof(error)));
        lcs_config_value_clear(&value);
    }
    if (pending)
    {
        lcs_config_value_t value = successor(2);
        assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
        lcs_config_value_clear(&value);
    }
    /* Ordinary restart with conflicting INI resources must keep the frozen
     * journal/catalog; neither INI nor the newer pending definition is cleanup. */
    cfg->resource_count = 1; strcpy(cfg->resources[0].systemd_unit, "wrong-ini.service");
    strcpy(cfg->resources[0].pre_stop, "/must-not-run"); strcpy(cfg->resources[0].post_stop, "/must-not-run");
    assert(!lcs_config_finalize(cfg, error, sizeof(error)));
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, cfg, error, sizeof(error)));
    assert(runtime.requires_recovery && runtime.application.applied.revision == 2);
    assert(!strcmp(runtime.base->resources[0].systemd_unit, "baseline.service"));
    free(cfg); forbid_io = true;
}

static void finish(lcs_config_startup_t *startup, lcs_config_startup_phase_t expected)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int result = lcs_config_startup_process(startup, lcs_now_ms());
        if (startup->phase == expected && startup->pid <= 0) return;
        assert(result >= 0);
        usleep(1000);
    }
    fprintf(stderr, "startup phase=%u pid=%ld error=%s\n", startup->phase, (long)startup->pid, startup->error);
    assert(!"startup recovery timed out");
}

static void until_backend(lcs_config_startup_t *startup)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_startup_process(startup, lcs_now_ms()) >= 0);
        if (startup->phase == LCS_CONFIG_STARTUP_BACKEND && startup->pid > 0) return;
        usleep(1000);
    }
    assert(!"backend worker did not start");
}

static void cleanup(lcs_config_startup_t *startup)
{
    lcs_config_startup_stop(startup);
    for (unsigned i = 0; startup->pid > 0 && i < 5000; i++)
    { (void)lcs_config_startup_process(startup, lcs_now_ms()); usleep(1000); }
    assert(!lcs_config_startup_close(startup));
    forbid_io = false;
    lcs_config_runtime_close(&runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock", "hook.sh", "trace", "hook-pid"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(store.directory_fd, names[i], 0) || errno == ENOENT);
    lcs_config_store_close(&store); assert(!rmdir(directory));
    const char *env[] = {"LCS_STARTUP_FAIL_EVENT", "LCS_STARTUP_SLEEP_EVENT", "LCS_VIP_FAIL_DEL", "LCS_SYSTEMD_FAIL_STOP", "LCS_VIP_OP_DELAY_MS"};
    for (size_t i = 0; i < sizeof(env) / sizeof(env[0]); i++) unsetenv(env[i]);
}

static lcs_config_owner_message_t owner_query;
static int capture_owner(void *context, size_t node, const void *payload, size_t length)
{
    (void)context; assert(node == 1);
    assert(!lcs_config_owner_decode(payload, length, &owner_query));
    return 0;
}

static void confirm_completed(lcs_config_owner_network_t *network)
{
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0};
    uint64_t now = lcs_now_ms();
    lcs_config_owner_network_process(network, instances, now, capture_owner, NULL);
    assert(owner_query.kind == LCS_CONFIG_OWNER_APPLIED_QUERY);
    lcs_config_owner_message_t reply = owner_query;
    reply.kind = LCS_CONFIG_OWNER_APPLIED_REPLY; reply.result = LCS_CONFIG_OWNER_STOPPED;
    reply.sender_instance = 901; reply.target_instance = 900;
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&reply, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(network, 1, 901, payload, length, now));
    lcs_config_owner_network_process(network, instances, now, capture_owner, NULL);
    assert(network->retired && !network->receipt.sealed);
}

static void test_applied_cleanup_and_pending_receipt(void)
{
    setup(true, false);
    lcs_config_startup_t startup = {0};
    runtime.requires_recovery = false;
    assert(lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)) < 0);
    runtime.requires_recovery = true;
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    assert(startup.config->resource_count == 2 && !strcmp(startup.config->resources[0].systemd_unit, "applied.service"));
    assert(!strcmp(startup.config->resources[1].address, "192.0.2.20/32"));
    assert(lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)) < 0);
    lcs_config_owner_network_t network;
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()) < 0 && !network.receipt.sealed);
    uint64_t promised = runtime.agreement.promised;
    finish(&startup, LCS_CONFIG_STARTUP_DONE);
    assert(lcs_config_startup_verified(&startup, &runtime, 900) && !lcs_config_startup_verified(&startup, &runtime, 901));
    assert(runtime.requires_recovery && runtime.agreement.promised == promised);
    assert(runtime.application.applied.revision == 2 && runtime.application.pending.revision == 3);
    FILE *events = fopen(trace, "r"); assert(events); char line[512];
    for (int revision = 2; revision >= 0; revision--)
    {
        const char *names[] = {"b_child", "b_child", "a_parent", "a_parent"};
        const char *event[] = {"pre-stop", "post-stop", "pre-stop", "post-stop"};
        for (size_t i = 0; i < 4; i++)
        {
            char expected[512];
            snprintf(expected, sizeof(expected), "%s|%s|%d|1|0|0|%s|%s\n", names[i], event[i], revision,
                i < 2 ? (revision == 2 ? "192.0.2.20/32" : "192.0.2.10/32") : "",
                i >= 2 ? (revision == 2 ? "applied.service" : "baseline.service") : "");
            assert(fgets(line, sizeof(line), events) && !strcmp(line, expected));
        }
    }
    assert(!fgets(line, sizeof(line), events)); fclose(events);
    assert(lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()) == 1);
    assert(!lcs_config_startup_restore_completed(&startup, &network));
    assert(network.applied && network.receipt.scope.old_revision == 1 && network.receipt.scope.target_revision == 2);
    assert(network.retired && network.retired_scope.old_revision == 0 && network.retired_scope.target_revision == 1);
    lcs_config_owner_message_t slow = { .kind = LCS_CONFIG_OWNER_APPLIED_QUERY, .sender_instance = 901,
        .target_instance = 900, .id = 77, .scope = network.retired_scope };
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&slow, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&network, 1, 901, payload, length, lcs_now_ms()));
    assert(network.replies[1].kind == LCS_CONFIG_OWNER_APPLIED_REPLY && network.replies[1].result == LCS_CONFIG_OWNER_STOPPED);
    assert(lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()) == 1);
    confirm_completed(&network);
    assert(network.retired_scope.old_revision == 1 && network.retired_scope.target_revision == 2);
    assert(!lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()));
    assert(network.receipt.sealed && network.receipt.instance == 900 && network.round.stopped == 1);
    assert(network.receipt.scope.old_revision == 2 && network.receipt.scope.target_revision == 3);
    lcs_config_owner_network_stop(&network); cleanup(&startup);
}

static void test_failures_and_retry(void)
{
    const char *faults[] = {"LCS_VIP_FAIL_DEL", "LCS_SYSTEMD_FAIL_STOP", "LCS_STARTUP_FAIL_EVENT", "LCS_STARTUP_FAIL_EVENT"};
    const char *values[] = {"1", "1", "pre-stop", "post-stop"};
    for (size_t f = 0; f < 4; f++)
    {
        setup(true, false); setenv(faults[f], values[f], 1);
        lcs_config_startup_t startup = {0};
        assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
        finish(&startup, LCS_CONFIG_STARTUP_BLOCKED);
        assert(!lcs_config_startup_verified(&startup, &runtime, 900));
        assert(!(startup.completed & (UINT32_C(1) << startup.index)));
        if (f >= 2)
        { FILE *events = fopen(trace, "r"); assert(events); char line[512]; assert(fgets(line, sizeof(line), events));
          assert(fgets(line, sizeof(line), events) && strstr(line, "post-stop")); fclose(events); }
        unsetenv(faults[f]);
        assert(!lcs_config_startup_retry(&startup)); finish(&startup, LCS_CONFIG_STARTUP_DONE);
        assert(lcs_config_startup_verified(&startup, &runtime, 900)); cleanup(&startup);
    }
}

static void test_timeout_cancellation_and_binding(void)
{
    for (unsigned fault = 0; fault < 4; fault++)
    {
        setup(true, false);
        if (fault < 3) setenv("LCS_VIP_OP_DELAY_MS", "60000", 1);
        else setenv("LCS_STARTUP_SLEEP_EVENT", "post-stop", 1);
        lcs_config_startup_t startup = {0};
        assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
        if (fault < 3) until_backend(&startup);
        else
            for (unsigned i = 0; i < 5000; i++)
            {
                assert(lcs_config_startup_process(&startup, lcs_now_ms()) >= 0);
                if (startup.phase == LCS_CONFIG_STARTUP_POST_STOP && startup.pid > 0) break;
                assert(i < 4999); usleep(1000);
            }
        pid_t worker = startup.pid;
        if (fault == 0 || fault == 3) (void)lcs_config_startup_process(&startup, startup.deadline_ms);
        if (fault == 1) lcs_config_startup_stop(&startup);
        if (fault == 2) { runtime.application.poisoned = true; (void)lcs_config_startup_process(&startup, lcs_now_ms()); }
        assert(startup.pid == worker && lcs_config_startup_close(&startup) < 0);
        finish(&startup, (fault == 0 || fault == 3) ? LCS_CONFIG_STARTUP_BLOCKED : fault == 1 ? LCS_CONFIG_STARTUP_STOPPED : LCS_CONFIG_STARTUP_RECOVERY);
        assert(!lcs_config_startup_verified(&startup, &runtime, 900));
        if (!fault || fault == 3) { unsetenv("LCS_VIP_OP_DELAY_MS"); unsetenv("LCS_STARTUP_SLEEP_EVENT"); assert(!lcs_config_startup_retry(&startup)); finish(&startup, LCS_CONFIG_STARTUP_DONE); }
        else assert(lcs_config_startup_retry(&startup) < 0);
        cleanup(&startup);
    }
    setup(true, false); lcs_config_startup_t startup = {0};
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error))); finish(&startup, LCS_CONFIG_STARTUP_DONE);
    runtime.requires_recovery = false;
    assert(!lcs_config_startup_verified(&startup, &runtime, 900)); cleanup(&startup);
}

static void test_quorum_only_and_completed_history(void)
{
    for (unsigned q = 0; q < 2; q++)
    {
        setup(false, q != 0); lcs_config_startup_t startup = {0};
        assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error))); finish(&startup, LCS_CONFIG_STARTUP_DONE);
        assert(lcs_config_startup_verified(&startup, &runtime, 900));
        if (q) assert(access(trace, F_OK) < 0 && errno == ENOENT);
        lcs_config_owner_network_t network;
        assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, q ? 2 : 0, 900));
        assert(lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()) < 0 && !network.receipt.sealed && !network.retired);
        if (q) assert(lcs_config_startup_restore_completed(&startup, &network) < 0);
        else
        {
            assert(!lcs_config_startup_restore_completed(&startup, &network));
            assert(network.applied && network.receipt.scope.target_revision == 2);
            confirm_completed(&network);
        }
        lcs_config_owner_network_stop(&network); cleanup(&startup);
    }
}

static void test_workers_die_with_parent(void)
{
    setup(false, false); lcs_config_startup_t startup = {0};
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    assert(!prctl(PR_SET_CHILD_SUBREAPER, 1));
    setenv("LCS_VIP_OP_DELAY_MS", "60000", 1); setenv("LCS_SYSTEMD_DELAY_MS", "60000", 1); setenv("LCS_VIP_PROBE_DELAY_MS", "60000", 1);
    for (unsigned type = 0; type < 3; type++)
    {
        int pipefd[2]; assert(!pipe(pipefd)); pid_t launcher = fork(); assert(launcher >= 0);
        if (!launcher)
        {
            close(pipefd[0]); pid_t worker; int result;
            if (!type) result = lcs_vip_operation_async(startup.config, &startup.config->resources[1], LCS_VIP_WORKER_DEL, &worker);
            else if (type == 1) result = lcs_systemd_service_stop_async(&startup.config->resources[0], &worker);
            else result = lcs_vip_conflict_check_async(startup.config, &startup.config->resources[1], &worker);
            assert(!result && write(pipefd[1], &worker, sizeof(worker)) == sizeof(worker)); _exit(0);
        }
        close(pipefd[1]); pid_t worker; assert(read(pipefd[0], &worker, sizeof(worker)) == sizeof(worker)); close(pipefd[0]);
        int status; assert(waitpid(launcher, &status, 0) == launcher && WIFEXITED(status) && !WEXITSTATUS(status));
        bool reaped = false;
        for (unsigned i = 0; i < 5000; i++)
        {
            pid_t got = waitpid(worker, &status, WNOHANG);
            if (got > 0) { reaped = true; break; }
            assert(got == 0 || errno == EINTR); usleep(1000);
        }
        assert(reaped && ((WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL) || (WIFEXITED(status) && WEXITSTATUS(status) == 125)));
    }

    /* The hook supervisor must kill the foreground script and its child commands,
     * rather than leaving a delayed backend-mutating command behind. */
    char command_path[128]; snprintf(command_path, sizeof(command_path), "%s/hook-pid", directory);
    int pipefd[2]; assert(!pipe(pipefd)); pid_t launcher = fork(); assert(launcher >= 0);
    if (!launcher)
    {
        close(pipefd[0]); setenv("LCS_STARTUP_SLEEP_EVENT", "pre-stop", 1); setenv("LCS_STARTUP_HOOK_PID", command_path, 1);
        for (unsigned i = 0; i < 5000; i++)
        {
            assert(lcs_config_startup_process(&startup, lcs_now_ms()) >= 0);
            if (startup.pid > 0)
            {
                FILE *ready = fopen(command_path, "r"); long command_pid;
                bool running = ready && fscanf(ready, "%ld", &command_pid) == 1 && command_pid > 0;
                if (ready) fclose(ready);
                if (running) { assert(write(pipefd[1], &startup.pid, sizeof(startup.pid)) == sizeof(startup.pid)); _exit(0); }
            }
            usleep(1000);
        }
        _exit(1);
    }
    close(pipefd[1]); pid_t supervisor;
    assert(read(pipefd[0], &supervisor, sizeof(supervisor)) == sizeof(supervisor)); close(pipefd[0]);
    int status; assert(waitpid(launcher, &status, 0) == launcher && WIFEXITED(status) && !WEXITSTATUS(status));
    FILE *pid_file = fopen(command_path, "r"); assert(pid_file); long command;
    assert(fscanf(pid_file, "%ld", &command) == 1); fclose(pid_file);
    pid_t expected[] = {supervisor, (pid_t)command};
    for (size_t child = 0; child < 2; child++)
    {
        bool reaped = false;
        for (unsigned i = 0; i < 5000; i++)
        {
            pid_t got = waitpid(expected[child], &status, WNOHANG);
            if (got > 0) { reaped = true; break; }
            assert(!got || errno == EINTR); usleep(1000);
        }
        assert(reaped && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
    }
    for (unsigned i = 0; i < 5000; i++)
    {
        pid_t got = waitpid(-1, &status, WNOHANG);
        if (got < 0 && errno == ECHILD) break;
        assert(i < 4999);
        if (got > 0) assert(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
        usleep(1000);
    }
    assert(!prctl(PR_SET_CHILD_SUBREAPER, 0)); unsetenv("LCS_SYSTEMD_DELAY_MS"); unsetenv("LCS_VIP_PROBE_DELAY_MS");
    cleanup(&startup);
}


static void test_completed_history_cleanup_and_legacy(void)
{
    setup(false, false); lcs_config_startup_t startup = {0};
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_startup_process(&startup, lcs_now_ms()) >= 0);
        if (startup.snapshot == 1 && startup.phase == LCS_CONFIG_STARTUP_BACKEND && startup.pid <= 0) break;
        assert(i < 4999); usleep(1000);
    }
    assert(startup.completed == 3 && startup.history_completed[0] == 0);
    setenv("LCS_VIP_FAIL_DEL", "1", 1); finish(&startup, LCS_CONFIG_STARTUP_BLOCKED);
    assert(!lcs_config_startup_verified(&startup, &runtime, 900));
    lcs_config_owner_network_t network;
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(lcs_config_startup_restore_completed(&startup, &network) < 0 && !network.receipt.sealed);
    unsetenv("LCS_VIP_FAIL_DEL"); assert(!lcs_config_startup_retry(&startup)); finish(&startup, LCS_CONFIG_STARTUP_DONE);
    char digest[65]; strcpy(digest, runtime.application.history[0].digest);
    runtime.application.history[0].digest[0] = digest[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_startup_verified(&startup, &runtime, 900));
    assert(lcs_config_startup_restore_completed(&startup, &network) < 0);
    strcpy(runtime.application.history[0].digest, digest);
    assert(!lcs_config_startup_restore_completed(&startup, &network));
    lcs_config_owner_network_stop(&network); cleanup(&startup);

    /* FINISH can leave no current resources; predecessors still need cleanup. */
    setup(true, false); memset(&startup, 0, sizeof(startup));
    forbid_io = false;
    assert(!lcs_config_application_finish(&runtime.application, runtime.application.pending.digest, error, sizeof(error)));
    forbid_io = true;
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    assert(!startup.config->resource_count && startup.phase != LCS_CONFIG_STARTUP_DONE);
    finish(&startup, LCS_CONFIG_STARTUP_DONE);
    assert(startup.history_completed[0] == 3 && startup.history_completed[1] == 3);
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(!lcs_config_startup_restore_completed(&startup, &network));
    assert(network.receipt.scope.old_revision == 2 && network.receipt.scope.target_revision == 3);
    lcs_config_owner_network_stop(&network); cleanup(&startup);

    /* Old record format is readable but missing history must not become proof. */
    setup(true, false); memset(&startup, 0, sizeof(startup));
    lcs_config_application_t legacy = runtime.application;
    memset(legacy.history, 0, sizeof(legacy.history));
    char *record; size_t length;
    assert(!lcs_config_application_export(&legacy, &record, &length, error, sizeof(error)));
    lcs_config_t *cfg = malloc(sizeof(*cfg)); assert(cfg); *cfg = *runtime.base;
    forbid_io = false;
    assert(!lcs_config_store_write(&store, "application.json", record, length, error, sizeof(error)));
    lcs_config_runtime_close(&runtime);
    assert(!lcs_config_runtime_open(&runtime, &store, cfg, error, sizeof(error)));
    forbid_io = true; free(cfg); free(record);
    assert(!runtime.application.history[0].document && runtime.application.applied.revision == 2);
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error))); finish(&startup, LCS_CONFIG_STARTUP_DONE);
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(lcs_config_startup_restore_completed(&startup, &network) < 0);
    assert(lcs_config_startup_restore_pending(&startup, &network, lcs_now_ms()) < 0);
    assert(!network.receipt.sealed && !network.retired);
    lcs_config_owner_network_stop(&network); cleanup(&startup);
}

int main(void)
{
    parent = getpid();
    test_applied_cleanup_and_pending_receipt(); test_failures_and_retry(); test_timeout_cancellation_and_binding();
    test_quorum_only_and_completed_history(); test_workers_die_with_parent();
    test_completed_history_cleanup_and_legacy();
    puts("authoritative startup cleanup, pending receipt recovery and worker lifetime unit tests passed");
    return 0;
}
