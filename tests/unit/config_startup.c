// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_startup.h"
#include "config_recovery.h"
#include "config_recovery_live.h"
#include "daemon_state.h"
#include "cluster.h"
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

/* Keep the cancellation assertion independent of whether SIGKILL is reaped in
 * the same scheduler step. The real child is still killed and subsequently reaped. */
static pid_t held_reap_pid;
pid_t __real_waitpid(pid_t pid, int *status, int options);
pid_t __wrap_waitpid(pid_t pid, int *status, int options)
{
    if (held_reap_pid > 0 && pid == held_reap_pid && (options & WNOHANG)) return 0;
    return __real_waitpid(pid, status, options);
}

daemon_state_t g_state;
static bool admission_cleanup_blocked;
bool resources_startup_cleanup_complete(void) { return !admission_cleanup_blocked; }

static unsigned live_cancellations;
static lcs_config_owner_network_t *live_owner_network;
static bool live_reload, live_cancel_bump;
bool config_reload_in_progress(void) { return live_reload; }
void move_cancel_all(int epoll_fd, const char *reason)
{
    (void)epoll_fd; (void)reason;
    assert(g_state.config_application_fenced && g_state.config_application_remapping);
    assert(!g_state.resource_generation_admitted && !g_state.voting_ready);
    assert(live_cancellations % 3 == 0); live_cancellations++;
}
void lease_cancel_all_operations(void)
{ assert(live_cancellations % 3 == 1); live_cancellations++;
  if (live_cancel_bump) g_state.lease_grants[0].promised_epoch++; }
void peer_close_all_for_config_reload(int epoll_fd)
{
    (void)epoll_fd; assert(live_cancellations % 3 == 2); live_cancellations++;
    if (live_owner_network) lcs_config_owner_network_disconnect(live_owner_network, 1);
}

static lcs_config_store_t store;
static lcs_config_runtime_t runtime;
static char directory[96], hook[128], trace[128], error[256];
static pid_t parent;
static bool forbid_io, fail_writer_sync, stop_writer_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_io || getpid() != parent);
    if (stop_writer_sync && getpid() != parent) raise(SIGSTOP);
    if (fail_writer_sync && getpid() != parent) { errno = EIO; return -1; }
    return __real_fsync(fd);
}

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
        (revision + 1) * 32, 5, &value, error, sizeof(error)));
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
        pid_t worker = startup.pid; assert(worker > 0); held_reap_pid = worker;
        if (fault == 0 || fault == 3) (void)lcs_config_startup_process(&startup, startup.deadline_ms);
        if (fault == 1) lcs_config_startup_stop(&startup);
        if (fault == 2) { runtime.application.poisoned = true; (void)lcs_config_startup_process(&startup, lcs_now_ms()); }
        assert(startup.pid == worker && lcs_config_startup_close(&startup) < 0);
        held_reap_pid = 0;
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
        if (q)
        {
            assert(lcs_config_startup_restore_completed(&startup, &network) < 0);
            assert(lcs_config_startup_bind_owner_history(&startup, &network) < 0);
        }
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

static lcs_config_owner_message_t history_reply;
static bool history_backpressure;
static int capture_history(void *context, size_t node, const void *payload, size_t length)
{
    (void)context; assert(node == 1);
    lcs_config_owner_message_t message;
    assert(!lcs_config_owner_decode(payload, length, &message));
    if (message.kind == LCS_CONFIG_OWNER_REPLY || message.kind == LCS_CONFIG_OWNER_APPLIED_REPLY)
        history_reply = message;
    return history_backpressure ? 1 : 0;
}

static void historical_query(lcs_config_owner_network_t *network, lcs_config_owner_message_t *query, uint8_t expected)
{
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(query, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(network, 1, 901, payload, length, lcs_now_ms()));
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0};
    memset(&history_reply, 0, sizeof(history_reply));
    lcs_config_owner_network_process(network, instances, lcs_now_ms(), capture_history, NULL);
    assert(history_reply.result == expected && history_reply.id == query->id);
    assert(history_reply.sender_instance == 900 && history_reply.target_instance == 901);
    assert(lcs_config_owner_scope_equal(&history_reply.scope, &query->scope));
}

static void test_differing_history_scopes(void)
{
    setup(true, false); lcs_config_startup_t startup = {0};
    lcs_config_owner_network_t network;
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    assert(lcs_config_startup_bind_owner_history(&startup, &network) < 0);
    finish(&startup, LCS_CONFIG_STARTUP_DONE);
    assert(!lcs_config_startup_restore_completed(&startup, &network));
    assert(!lcs_config_startup_bind_owner_history(&startup, &network));
    assert(lcs_config_startup_bind_owner_history(&startup, &network) < 0);

    /* The requester skipped revision 1. Its exact 0->2 scope differs from this
     * node's pinned 1->2 scope, including replacement backend cleanup. */
    lcs_config_application_t pair = runtime.application;
    pair.applied = startup.history[1]; pair.pending = startup.applied;
    lcs_config_owner_scope_t scope;
    assert(!lcs_config_owner_scope(&pair, &scope, error, sizeof(error)));
    assert(scope.old_revision == 0 && scope.target_revision == 2 && scope.drain == 3);
    lcs_config_owner_round_t round = {0}; lcs_config_owner_receipt_t local = {0};
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0}; uint64_t now = lcs_now_ms();
    assert(!lcs_config_owner_begin(&round, runtime.base, &scope, 1, instances, 77, now));
    assert(!lcs_config_owner_receipt_seal(&local, &scope, 901));
    assert(!lcs_config_owner_local(&round, &local));
    lcs_config_owner_message_t query;
    assert(!lcs_config_owner_query(&round, 0, 900, now, &query));
    historical_query(&network, &query, LCS_CONFIG_OWNER_STOPPED);
    assert(!lcs_config_owner_reply(&round, 0, 900, lcs_now_ms(), &history_reply));
    assert(lcs_config_owner_poll(&round, lcs_now_ms()) == 1);
    /* Exact application confirmation uses the same historical old table. */
    assert(!lcs_config_owner_begin(&round, runtime.base, &scope, 1, instances, 78, now));
    assert(!lcs_config_owner_local(&round, &local));
    assert(!lcs_config_owner_query(&round, 0, 900, now, &query));
    query.kind = LCS_CONFIG_OWNER_APPLIED_QUERY;
    historical_query(&network, &query, LCS_CONFIG_OWNER_STOPPED);
    history_reply.kind = LCS_CONFIG_OWNER_REPLY;
    assert(!lcs_config_owner_reply(&round, 0, 900, lcs_now_ms(), &history_reply));
    assert(lcs_config_owner_poll(&round, lcs_now_ms()) == 1);
    assert(network.receipt.scope.old_revision == 1 && network.receipt.scope.target_revision == 2);

    /* Supplied drain masks and old value digests are never trusted. */
    query.scope.drain ^= 1;
    historical_query(&network, &query, LCS_CONFIG_OWNER_FAILED);
    query.scope = scope; query.scope.old_value[0] = scope.old_value[0] == 'a' ? 'b' : 'a';
    historical_query(&network, &query, LCS_CONFIG_OWNER_FAILED);

    pair.pending = startup.pending;
    assert(!lcs_config_owner_scope(&pair, &query.scope, error, sizeof(error)));
    query.kind = LCS_CONFIG_OWNER_QUERY;
    historical_query(&network, &query, LCS_CONFIG_OWNER_STOPPED);
    query.kind = LCS_CONFIG_OWNER_APPLIED_QUERY;
    historical_query(&network, &query, LCS_CONFIG_OWNER_FAILED); /* Pending is not applied. */

    /* Backpressure must not preserve evidence after recovery fences are lost. */
    query.scope = scope; query.kind = LCS_CONFIG_OWNER_QUERY;
    history_backpressure = true;
    historical_query(&network, &query, LCS_CONFIG_OWNER_STOPPED);
    assert(network.pending_replies == 2);
    runtime.requires_recovery = false;
    history_backpressure = false;
    lcs_config_owner_network_process(&network, instances, lcs_now_ms(), capture_history, NULL);
    assert(history_reply.result == LCS_CONFIG_OWNER_FAILED && network.receipt.failed);
    assert(network.release.failed && !network.pending_replies);
    lcs_config_owner_network_stop(&network); cleanup(&startup);

    /* Current snapshots do not recover an unknown old table from a v1 catalog. */
    setup(true, false); memset(&startup, 0, sizeof(startup));
    lcs_config_value_t saved[2] = {runtime.application.history[0], runtime.application.history[1]};
    memset(runtime.application.history, 0, sizeof(runtime.application.history));
    assert(!lcs_config_startup_begin(&startup, &runtime, 900, error, sizeof(error)));
    finish(&startup, LCS_CONFIG_STARTUP_DONE);
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
    assert(lcs_config_startup_bind_owner_history(&startup, &network) < 0);
    pair = runtime.application; pair.applied = saved[1]; pair.pending = startup.applied;
    assert(!lcs_config_owner_scope(&pair, &query.scope, error, sizeof(error)));
    historical_query(&network, &query, LCS_CONFIG_OWNER_PENDING);
    lcs_config_owner_network_stop(&network);
    runtime.application.history[0] = saved[0]; runtime.application.history[1] = saved[1];
    cleanup(&startup);
}

typedef struct
{
    lcs_config_owner_network_t nodes[2];
    size_t from, count;
    struct { size_t from, to, length; unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; } messages[64];
    uint64_t now;
} recovery_bus_t;

static int recovery_send(void *context, size_t node, const void *payload, size_t length)
{
    recovery_bus_t *bus = context;
    assert(node < 2 && node != bus->from && bus->count < 64 && length == LCS_CONFIG_OWNER_MESSAGE_SIZE);
    bus->messages[bus->count].from = bus->from; bus->messages[bus->count].to = node;
    bus->messages[bus->count].length = length;
    memcpy(bus->messages[bus->count++].payload, payload, length);
    return 0;
}

static void recovery_pump(recovery_bus_t *bus, unsigned steps)
{
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0};
    for (unsigned n = 0; n < steps; n++)
    {
        for (size_t i = 0; i < 2; i++)
        {
            bus->from = i;
            lcs_config_owner_network_process(&bus->nodes[i], instances, ++bus->now, recovery_send, bus);
        }
        for (size_t i = 0; i < bus->count; i++)
            assert(!lcs_config_owner_network_receive(&bus->nodes[bus->messages[i].to], bus->messages[i].from,
                instances[bus->messages[i].from], bus->messages[i].payload, bus->messages[i].length, bus->now));
        bus->count = 0;
    }
}

static void recover_peer(lcs_config_runtime_t *peer, lcs_config_store_t *peer_store, char *path)
{
    forbid_io = false;
    assert(mkdtemp(path)); assert(!lcs_config_store_open(peer_store, path, error, sizeof(error)));
    lcs_config_t *cfg = malloc(sizeof(*cfg)), *frozen = malloc(sizeof(*frozen)); assert(cfg && frozen);
    *cfg = *runtime.base; strcpy(cfg->self_name, "node1");
    assert(!lcs_config_finalize(cfg, error, sizeof(error)));
    assert(!lcs_config_activation_open(&peer->activation, peer_store, cfg, frozen, true, error, sizeof(error)));
    peer->base = frozen;
    assert(!lcs_config_activation_resume(&peer->activation, &peer->agreement, &peer->application, error, sizeof(error)));
    lcs_config_barrier_round_t round;
    assert(!lcs_config_barrier_begin(&round, &peer->activation, &peer->agreement, &peer->application, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) assert(!lcs_config_barrier_ready(&round, i, round.membership, round.baseline, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    assert(!lcs_config_barrier_open(&peer->barrier, &peer->activation, &peer->agreement, &peer->application, &round, error, sizeof(error)));
    const lcs_config_value_t *values[] = {&runtime.application.history[0], &runtime.application.applied, &runtime.application.pending};
    for (size_t i = 0; i < 3; i++)
    {
        assert(values[i]->revision == i + 1);
        assert(!lcs_config_agreement_learn(&peer->agreement, peer->agreement.committed.digest,
            (i + 1) * 32, 5, values[i], error, sizeof(error)));
        if (i < 2) assert(!lcs_config_application_begin(&peer->application, &peer->agreement, error, sizeof(error)));
        if (!i) assert(!lcs_config_application_finish(&peer->application, values[i]->digest, error, sizeof(error)));
    }
    lcs_config_runtime_close(peer);
    assert(!lcs_config_runtime_open(peer, peer_store, cfg, error, sizeof(error)));
    assert(peer->requires_recovery && peer->agreement.committed.revision == 3);
    assert(peer->application.applied.revision == 1 && peer->application.pending.revision == 2);
    free(cfg); forbid_io = true;
}

static void refresh_recovery(lcs_config_startup_t **startup, lcs_config_runtime_t *node,
                                lcs_config_owner_network_t *network)
{
    /* Recovery and backend/reactivation fences remain held across the handoff.
     * Retain the old borrowed runner until the new provider replaces it. */
    uint64_t serial = network->serial;
    lcs_config_startup_t *next = calloc(1, sizeof(*next)); assert(next);
    assert(!lcs_config_startup_begin(next, node, network->instance, error, sizeof(error)));
    lcs_config_owner_network_t saved = *network;
    assert(lcs_config_startup_refresh_owner_history(next, network) < 0);
    assert(!memcmp(&saved, network, sizeof(saved)));
    finish(next, LCS_CONFIG_STARTUP_DONE);
    if (network->receipt.sealed)
    {
        network->receipt.scope.drain ^= 1;
        lcs_config_owner_network_t invalid = *network;
        assert(lcs_config_startup_refresh_owner_history(next, network) < 0);
        assert(!memcmp(&invalid, network, sizeof(invalid)));
        *network = saved;
    }
    if (network->retired)
    {
        network->retired_scope.target_value[0] = network->retired_scope.target_value[0] == 'a' ? 'b' : 'a';
        lcs_config_owner_network_t invalid = *network;
        assert(lcs_config_startup_refresh_owner_history(next, network) < 0);
        assert(!memcmp(&invalid, network, sizeof(invalid)));
        *network = saved;
    }
    network->serial = UINT64_MAX;
    assert(lcs_config_startup_refresh_owner_history(next, network) < 0 && network->serial == UINT64_MAX);
    *network = saved;
    pid_t old_pid = (*startup)->pid; (*startup)->pid = 1;
    assert(lcs_config_startup_refresh_owner_history(next, network) < 0);
    assert(!memcmp(&saved, network, sizeof(saved)));
    (*startup)->pid = old_pid;

    assert(!lcs_config_startup_refresh_owner_history(next, network));
    assert(network->serial == serial && !network->pending_replies && !network->round.active && !network->release.active);
    assert(network->instances[network->self] == network->instance);
    saved = *network;
    assert(lcs_config_startup_refresh_owner_history(next, network) < 0); /* Cannot reuse old cleanup proof. */
    assert(!memcmp(&saved, network, sizeof(saved)));
    lcs_config_startup_stop(*startup); assert(!lcs_config_startup_close(*startup)); free(*startup);
    *startup = next;
}

static void test_differing_target_recovery(void)
{
    setup(true, false);
    lcs_config_runtime_t peer = {0}; lcs_config_store_t peer_store;
    char path[] = "/tmp/lcs-config-recovery-peer.XXXXXX";
    recover_peer(&peer, &peer_store, path);
    recovery_bus_t bus = {.now = lcs_now_ms()};
    lcs_config_startup_t *startup[2] = {calloc(1, sizeof(*startup[0])), calloc(1, sizeof(*startup[1]))};
    assert(startup[0] && startup[1]);
    lcs_config_runtime_t *runtimes[] = {&runtime, &peer};
    for (size_t i = 0; i < 2; i++)
    {
        assert(!lcs_config_startup_begin(startup[i], runtimes[i], 900 + i, error, sizeof(error)));
        finish(startup[i], LCS_CONFIG_STARTUP_DONE);
        assert(!lcs_config_owner_network_init(&bus.nodes[i], runtimes[i]->base, &runtimes[i]->application, i, 900 + i));
        assert(!lcs_config_startup_restore_completed(startup[i], &bus.nodes[i]));
        assert(!lcs_config_startup_bind_owner_history(startup[i], &bus.nodes[i]));
    }
    /* Node0 is at applied 2/pending 3; node1 must finish its durable pending 2,
     * even though it has already learned revision 3. */
    recovery_pump(&bus, 300);
    assert(bus.nodes[0].receipt.sealed && bus.nodes[0].receipt.scope.target_revision == 2);
    assert(bus.nodes[1].retired && !bus.nodes[1].receipt.sealed);
    assert(lcs_config_startup_restore_pending(startup[0], &bus.nodes[0], bus.now) == 1);
    assert(!lcs_config_startup_restore_pending(startup[1], &bus.nodes[1], bus.now));
    recovery_pump(&bus, 300);
    assert(lcs_config_owner_poll(&bus.nodes[1].round, bus.now) == 1);
    uint64_t old_serial = bus.nodes[1].serial;
    lcs_config_owner_message_t stale = {.kind = LCS_CONFIG_OWNER_APPLIED_REPLY, .result = LCS_CONFIG_OWNER_STOPPED,
        .sender_instance = 900, .target_instance = 901, .id = old_serial, .scope = bus.nodes[1].receipt.scope};
    forbid_io = false;
    assert(!lcs_config_application_finish(&peer.application, peer.application.pending.digest, error, sizeof(error)));
    forbid_io = true;
    recovery_pump(&bus, 300);
    assert(bus.nodes[1].receipt.failed); /* Old provider cannot survive FINISH. */
    assert(bus.nodes[0].release.failed && bus.nodes[0].receipt.sealed);
    refresh_recovery(&startup[1], &peer, &bus.nodes[1]);
    assert(!lcs_config_owner_network_retry(&bus.nodes[0], bus.now));
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0}; bus.from = 1;
    lcs_config_owner_network_process(&bus.nodes[1], instances, ++bus.now, recovery_send, &bus);
    assert(bus.nodes[1].release.id > old_serial && bus.nodes[1].release.stopped == 2);
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&stale, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[1], 0, 900, payload, length, bus.now));
    assert(bus.nodes[1].release.stopped == 2 && !bus.nodes[1].release.failed);
    recovery_pump(&bus, 300);
    for (size_t i = 0; i < 2; i++)
        assert(bus.nodes[i].retired && bus.nodes[i].retired_scope.target_revision == 2 && !bus.nodes[i].receipt.sealed);

    /* Both nodes can now advance to revision 3, after retaining/finishing 2. */
    forbid_io = false;
    assert(!lcs_config_application_begin(&peer.application, &peer.agreement, error, sizeof(error)));
    forbid_io = true;
    refresh_recovery(&startup[1], &peer, &bus.nodes[1]);
    recovery_pump(&bus, 300);
    for (size_t i = 0; i < 2; i++) assert(!lcs_config_startup_restore_pending(startup[i], &bus.nodes[i], bus.now));
    recovery_pump(&bus, 300);
    for (size_t i = 0; i < 2; i++)
    {
        assert(lcs_config_owner_poll(&bus.nodes[i].round, bus.now) == 1);
        forbid_io = false;
        assert(!lcs_config_application_finish(&runtimes[i]->application, runtimes[i]->application.pending.digest, error, sizeof(error)));
        forbid_io = true;
        refresh_recovery(&startup[i], runtimes[i], &bus.nodes[i]);
    }
    recovery_pump(&bus, 300);
    for (size_t i = 0; i < 2; i++)
    {
        assert(bus.nodes[i].retired && bus.nodes[i].retired_scope.target_revision == 3 && !bus.nodes[i].receipt.sealed);
        assert(runtimes[i]->application.applied.revision == 3 && !runtimes[i]->application.pending.document);
        assert(runtimes[i]->requires_recovery); /* No implicit lease admission. */
        lcs_config_owner_network_stop(&bus.nodes[i]);
    }
    lcs_config_startup_stop(startup[1]); assert(!lcs_config_startup_close(startup[1]));
    free(startup[1]);
    lcs_config_runtime_close(&peer); forbid_io = false;
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(peer_store.directory_fd, names[i], 0));
    lcs_config_store_close(&peer_store); assert(!rmdir(path));
    cleanup(startup[0]); free(startup[0]);
}

typedef struct
{
    lcs_config_recovery_t *manager;
    uint64_t revisions[16];
    size_t count;
    unsigned deferred;
    bool fail;
} recovery_install_t;

static int recovery_install(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t err_len)
{
    recovery_install_t *installation = context;
    lcs_config_recovery_t *manager = installation->manager;
    assert(view && view->value && view->value->document && view->attempt && view->target && view->old_config && view->plan);
    assert(manager->coordinator->application_owner == manager && !lcs_config_coordinator_can_vote(manager->coordinator));
    assert(manager->coordinator->runtime->requires_recovery);
    assert(lcs_config_startup_verified(manager->cleanup, manager->coordinator->runtime, manager->network->instance));
    if (!manager->quorum_only && !manager->current_install)
        assert(lcs_config_owner_poll(&manager->network->round, lcs_now_ms()) == 1);
    if (installation->fail) { snprintf(err, err_len, "injected table installation failure"); return -1; }
    if (installation->deferred) { installation->deferred--; return 0; }
    assert(installation->count < 16);
    installation->revisions[installation->count++] = view->value->revision;
    return 1;
}

static void manager_tick(recovery_bus_t *bus, lcs_config_recovery_t managers[2])
{
    uint64_t instances[LCS_MAX_NODES] = {900, 901, 0};
    bus->now = lcs_now_ms();
    for (size_t i = 0; i < 2; i++)
    {
        bus->from = i;
        int result = lcs_config_recovery_process(&managers[i], instances, bus->now, recovery_send, bus);
        assert(result >= 0 || managers[i].phase == LCS_CONFIG_RECOVERY_BLOCKED);
        if (managers[i].phase != LCS_CONFIG_RECOVERY_COMPLETE)
            assert(!lcs_config_coordinator_can_vote(managers[i].coordinator));
        else assert(!managers[i].coordinator->application_owner && !managers[i].coordinator->runtime->requires_recovery);
    }
    for (size_t i = 0; i < bus->count; i++)
        assert(!lcs_config_owner_network_receive(&bus->nodes[bus->messages[i].to], bus->messages[i].from,
            instances[bus->messages[i].from], bus->messages[i].payload, bus->messages[i].length, bus->now));
    bus->count = 0;
}

static void stop_manager(lcs_config_recovery_t *manager)
{
    assert(lcs_config_recovery_close(manager) < 0);
    lcs_config_recovery_stop(manager);
    for (unsigned i = 0; manager->phase != LCS_CONFIG_RECOVERY_STOPPED && i < 5000; i++)
    { assert(!lcs_config_recovery_process(manager, NULL, lcs_now_ms(), NULL, NULL)); usleep(1000); }
    assert(manager->phase == LCS_CONFIG_RECOVERY_STOPPED);
    assert(!lcs_config_recovery_close(manager));
}

static void test_recovery_manager_skew_and_repairs(void)
{
    setup(true, false);
    lcs_config_runtime_t peer = {0}; lcs_config_store_t peer_store;
    char path[] = "/tmp/lcs-config-recovery-peer.XXXXXX"; recover_peer(&peer, &peer_store, path);
    recovery_bus_t bus = {0};
    lcs_config_coordinator_t coordinators[2]; lcs_config_readiness_t readiness[2] = {0};
    lcs_config_recovery_t managers[2] = {{0}};
    recovery_install_t installed[2] = {{.manager = &managers[0], .deferred = 2}, {.manager = &managers[1], .deferred = 2}};
    lcs_config_runtime_t *runtimes[] = {&runtime, &peer};
    for (size_t i = 0; i < 2; i++)
    {
        lcs_config_coordinator_init(&coordinators[i], runtimes[i], &readiness[i]);
        assert(!lcs_config_coordinator_recover(&coordinators[i]));
        assert(!lcs_config_owner_network_init(&bus.nodes[i], runtimes[i]->base, &runtimes[i]->application, i, 900 + i));
        assert(lcs_config_recovery_start(&managers[i], &coordinators[i], &bus.nodes[i], NULL, &installed[i]) < 0);
        assert(!lcs_config_recovery_start(&managers[i], &coordinators[i], &bus.nodes[i], recovery_install, &installed[i]));
        assert(lcs_config_recovery_start(&managers[i], &coordinators[i], &bus.nodes[i], recovery_install, &installed[i]) < 0);
    }
    setenv("LCS_VIP_FAIL_DEL", "1", 1);
    for (unsigned i = 0; i < 5000; i++)
    {
        manager_tick(&bus, managers);
        if (managers[0].phase == LCS_CONFIG_RECOVERY_BLOCKED && managers[1].phase == LCS_CONFIG_RECOVERY_BLOCKED) break;
        assert(i < 4999); usleep(1000);
    }
    for (size_t i = 0; i < 2; i++)
    {
        assert(managers[i].blocked_phase == LCS_CONFIG_RECOVERY_CLEANUP && !installed[i].count);
        assert(!coordinators[i].completed_applications && runtimes[i]->requires_recovery);
    }
    unsetenv("LCS_VIP_FAIL_DEL");
    for (size_t i = 0; i < 2; i++) assert(!lcs_config_recovery_retry(&managers[i], lcs_now_ms()));
    bool failed_install = false, interrupted_install = false;
    for (unsigned n = 0; n < 10000; n++)
    {
        if (!failed_install && managers[1].phase == LCS_CONFIG_RECOVERY_INSTALL) installed[1].fail = true;
        if (failed_install && !interrupted_install && managers[1].phase == LCS_CONFIG_RECOVERY_INSTALL)
        {
            lcs_config_owner_network_disconnect(&bus.nodes[1], 0);
            interrupted_install = true;
        }
        manager_tick(&bus, managers);
        for (size_t i = 0; i < 2; i++)
            if (managers[i].phase == LCS_CONFIG_RECOVERY_BLOCKED)
            {
                if (installed[i].fail)
                {
                    assert(managers[i].blocked_phase == LCS_CONFIG_RECOVERY_INSTALL);
                    installed[i].fail = false; failed_install = true;
                }
                /* Retry only after physical evidence is repaired. This does not
                 * waive failed collections or count elapsed time as cleanup. */
                if (managers[1 - i].cleanup && lcs_config_startup_verified(managers[1 - i].cleanup,
                    runtimes[1 - i], bus.nodes[1 - i].instance))
                    (void)lcs_config_recovery_retry(&managers[i], bus.now);
            }
        if (managers[0].phase == LCS_CONFIG_RECOVERY_READY && managers[1].phase == LCS_CONFIG_RECOVERY_READY) break;
        if (n == 9999)
            fprintf(stderr, "manager states %u/%u errors %s / %s\n", managers[0].phase, managers[1].phase,
                managers[0].error, managers[1].error);
        assert(n < 9999); usleep(1000);
    }
    assert(failed_install && interrupted_install && installed[0].count == 1 && installed[0].revisions[0] == 3);
    assert(installed[1].count == 2 && installed[1].revisions[0] == 2 && installed[1].revisions[1] == 3);
    for (size_t i = 0; i < 2; i++)
    {
        assert(runtimes[i]->application.applied.revision == 3 && !runtimes[i]->application.pending.document);
        assert(runtimes[i]->requires_recovery && coordinators[i].application_owner == &managers[i]);
        assert(bus.nodes[i].retired && bus.nodes[i].retired_scope.target_revision == 3);
        stop_manager(&managers[i]);
    }
    lcs_config_runtime_close(&peer); forbid_io = false;
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(peer_store.directory_fd, names[i], 0));
    lcs_config_store_close(&peer_store); assert(!rmdir(path));
    lcs_config_startup_t empty = {0}; cleanup(&empty);
}

static int no_owner_send(void *context, size_t node, const void *payload, size_t length)
{
    (void)context; (void)node; (void)payload; (void)length;
    assert(!"quorum-only recovery cannot send owner evidence"); return -1;
}

static void test_quorum_recovery_manager_and_cancellation(void)
{
    setup(false, true);
    lcs_config_coordinator_t coordinator; lcs_config_readiness_t readiness = {0};
    lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
    assert(!lcs_config_coordinator_recover(&coordinator));
    lcs_config_owner_network_t network;
    assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 2, 902));
    lcs_config_recovery_t manager = {0}; recovery_install_t installed = {.manager = &manager};
    assert(!lcs_config_recovery_start(&manager, &coordinator, &network, recovery_install, &installed));
    uint64_t instances[LCS_MAX_NODES] = {0, 0, 902};
    for (unsigned i = 0; manager.phase != LCS_CONFIG_RECOVERY_READY && i < 5000; i++)
    { assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0); usleep(1000); }
    assert(manager.phase == LCS_CONFIG_RECOVERY_READY && installed.count == 1 && installed.revisions[0] == 2);
    assert(access(trace, F_OK) < 0 && errno == ENOENT && !network.receipt.sealed && !lcs_config_coordinator_can_vote(&coordinator));
    /* A learned checkpoint wakes a READY controller without granting a vote. */
    lcs_config_t *cfg = malloc(sizeof(*cfg)); assert(cfg); uint64_t revision;
    assert(!lcs_resource_config_decode(runtime.application.applied.document, runtime.application.applied.length,
        runtime.base, cfg, &revision, error, sizeof(error)));
    char *document; size_t length;
    assert(!lcs_resource_config_encode(cfg, 4, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, runtime.base, "manager-checkpoint-4", document, length, error, sizeof(error)));
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_CHECKPOINT, .ballot = 128, .voters = 5, .value = &value};
    strcpy(job.parent_digest, runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &job, error, sizeof(error)));
    free(document); document = NULL;
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (manager.phase == LCS_CONFIG_RECOVERY_READY && runtime.application.applied.revision == 4) break;
        assert(i < 4999); usleep(1000);
    }
    assert(installed.count == 2 && installed.revisions[1] == 4 && runtime.requires_recovery);
    assert(access(trace, F_OK) < 0 && errno == ENOENT && !lcs_config_coordinator_can_vote(&coordinator));
    /* Force another pending write, then cancel while the forked writer exists. */
    assert(!lcs_resource_config_encode(cfg, 5, &document, &length, error, sizeof(error)));
    lcs_config_value_clear(&value);
    assert(!lcs_config_value_init(&value, runtime.base, "manager-checkpoint-5", document, length, error, sizeof(error)));
    job.ballot = 160; job.value = &value; strcpy(job.parent_digest, runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &job, error, sizeof(error)));
    free(document); document = NULL;
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (manager.phase == LCS_CONFIG_RECOVERY_BEGIN && coordinator.storage.pid > 0) break;
        assert(i < 4999); usleep(1000);
    }
    assert(coordinator.storage.pid > 0 && runtime.application.applied.revision == 4);
    stop_manager(&manager);
    assert(runtime.requires_recovery && coordinator.closing && !coordinator.application_owner);
    free(document); free(cfg); lcs_config_value_clear(&value);
    lcs_config_startup_t empty = {0}; cleanup(&empty);
}

static void test_recovery_manager_storage_failure_and_worker_stop(void)
{
    for (unsigned fault = 0; fault < 2; fault++)
    {
        setup(true, fault == 0);
        lcs_config_coordinator_t coordinator; lcs_config_readiness_t readiness = {0};
        lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
        assert(!lcs_config_coordinator_recover(&coordinator));
        lcs_config_owner_network_t network;
        size_t self = fault ? 0 : 2; uint64_t instance = 900 + self;
        assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, self, instance));
        lcs_config_recovery_t manager = {0}; recovery_install_t installed = {.manager = &manager};
        assert(!lcs_config_recovery_start(&manager, &coordinator, &network, recovery_install, &installed));
        uint64_t instances[LCS_MAX_NODES] = {0}; instances[self] = instance;
        if (fault) setenv("LCS_VIP_OP_DELAY_MS", "60000", 1);
        else fail_writer_sync = true;
        for (unsigned i = 0; i < 5000; i++)
        {
            int result = lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL);
            if (!fault && manager.phase == LCS_CONFIG_RECOVERY_FAILED) break;
            if (fault && manager.next_cleanup && manager.next_cleanup->phase == LCS_CONFIG_STARTUP_BACKEND && manager.next_cleanup->pid > 0) break;
            assert(result >= 0 && i < 4999); usleep(1000);
        }
        if (!fault)
        {
            assert(coordinator.failed && manager.phase == LCS_CONFIG_RECOVERY_FAILED && network.closing);
            assert(lcs_config_recovery_retry(&manager, lcs_now_ms()) < 0 && runtime.requires_recovery);
        }
        else assert(manager.next_cleanup->pid > 0 && !installed.count && !coordinator.completed_applications);
        stop_manager(&manager);
        fail_writer_sync = false; unsetenv("LCS_VIP_OP_DELAY_MS");
        lcs_config_startup_t empty = {0}; cleanup(&empty);
    }
}

static void test_recovery_manager_missing_evidence(void)
{
    for (unsigned missing = 0; missing < 2; missing++)
    {
        setup(true, false);
        lcs_config_value_t saved[2] = {0};
        if (missing)
        {
            saved[0] = runtime.application.history[0]; saved[1] = runtime.application.history[1];
            memset(runtime.application.history, 0, sizeof(runtime.application.history));
        }
        lcs_config_coordinator_t coordinator; lcs_config_readiness_t readiness = {0};
        lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
        assert(!lcs_config_coordinator_recover(&coordinator));
        lcs_config_owner_network_t network;
        assert(!lcs_config_owner_network_init(&network, runtime.base, &runtime.application, 0, 900));
        lcs_config_recovery_t manager = {0}; recovery_install_t installed = {.manager = &manager};
        assert(!lcs_config_recovery_start(&manager, &coordinator, &network, recovery_install, &installed));
        uint64_t instances[LCS_MAX_NODES] = {900, 0, 0};
        for (unsigned i = 0; i < 5000; i++)
        {
            int result = lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL);
            if (missing && manager.phase == LCS_CONFIG_RECOVERY_FAILED) break;
            if (!missing && network.release.active) break;
            assert(result >= 0 && i < 4999); usleep(1000);
        }
        if (!missing)
        {
            assert(lcs_config_recovery_process(&manager, instances, network.release.deadline_ms, no_owner_send, NULL) < 0);
            assert(manager.phase == LCS_CONFIG_RECOVERY_BLOCKED && manager.blocked_phase == LCS_CONFIG_RECOVERY_RETIRE);
            assert(network.receipt.sealed && network.receipt.scope.target_revision == 2 && network.retired_scope.target_revision == 1);
        }
        else assert(manager.phase == LCS_CONFIG_RECOVERY_FAILED && network.closing);
        assert(!installed.count && !coordinator.completed_applications && runtime.requires_recovery);
        assert(runtime.application.applied.revision == 2 && runtime.application.pending.revision == 3);
        assert(!lcs_config_coordinator_can_vote(&coordinator)); stop_manager(&manager);
        if (missing)
        { runtime.application.history[0] = saved[0]; runtime.application.history[1] = saved[1]; }
        lcs_config_startup_t empty = {0}; cleanup(&empty);
    }
}

static void recovery_projection_open(void)
{
    bool was_forbidden = forbid_io; forbid_io = false;
    lcs_config_bootstrap_t *bootstrap = &g_state.config_bootstrap;
    bootstrap->state = store; bootstrap->initialization_locked = true;
    assert(!mkdirat(store.directory_fd, "snapshots", 0700));
    char path[128]; snprintf(path, sizeof(path), "%s/snapshots", directory);
    assert(!lcs_config_store_open(&bootstrap->snapshots, path, error, sizeof(error)));
    assert(!unlinkat(bootstrap->snapshots.directory_fd, "lock", 0));
    close(bootstrap->snapshots.lock_fd); bootstrap->snapshots.lock_fd = -1;
    strcpy(bootstrap->snapshot_name, "resources.json");
    forbid_io = was_forbidden;
}

static void recovery_projection_matches(const lcs_config_value_t *value)
{
    char *document; size_t length;
    lcs_config_bootstrap_t *bootstrap = &g_state.config_bootstrap;
    assert(!lcs_config_store_read(&bootstrap->snapshots, bootstrap->snapshot_name,
        &document, &length, value->length + 1, error, sizeof(error)));
    assert(length == value->length && !memcmp(document, value->document, length)); free(document);
    struct stat st;
    assert(!fstatat(bootstrap->snapshots.directory_fd, bootstrap->snapshot_name, &st, AT_SYMLINK_NOFOLLOW));
    assert(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
}

static void recovery_projection_close(void)
{
    assert(g_state.config_coordinator.storage.pid <= 0);
    lcs_config_bootstrap_t *bootstrap = &g_state.config_bootstrap;
    assert(!unlinkat(bootstrap->snapshots.directory_fd, bootstrap->snapshot_name, 0) || errno == ENOENT);
    assert(!unlinkat(bootstrap->snapshots.directory_fd, ".resources.json.tmp", 0) || errno == ENOENT);
    lcs_config_store_close(&bootstrap->snapshots);
    assert(!unlinkat(store.directory_fd, "snapshots", AT_REMOVEDIR));
}

static void recovery_live_target(void)
{
    lcs_config_t *cfg = malloc(3 * sizeof(*cfg)); assert(cfg); uint64_t revision;
    assert(!lcs_resource_config_decode(runtime.application.applied.document, runtime.application.applied.length,
        runtime.base, &cfg[0], &revision, error, sizeof(error)));
    cfg[1] = cfg[0]; cfg[1].resource_count = 3;
    cfg[1].resources[2] = cfg[0].resources[1]; cfg[1].resources[1] = cfg[0].resources[0];
    strcpy(cfg[1].resources[0].name, "a_new"); strcpy(cfg[1].resources[0].systemd_unit, "new.service");
    strcpy(cfg[1].resources[2].address, "192.0.2.30/32");
    assert(!lcs_resource_config_assign_identities(&cfg[0], revision, &cfg[1], &cfg[2], error, sizeof(error)));
    char *document; size_t length;
    assert(!lcs_resource_config_encode(&cfg[2], 3, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, runtime.base, "recovery-live-3", document, length, error, sizeof(error)));
    forbid_io = false;
    assert(!lcs_config_agreement_learn(&runtime.agreement, runtime.agreement.committed.digest, 96, 5, &value, error, sizeof(error)));
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    forbid_io = true; free(document); free(cfg); lcs_config_value_clear(&value);
}

static void close_admitted_manager(lcs_config_recovery_t *manager)
{
    lcs_config_coordinator_t *coordinator = manager->coordinator;
    lcs_config_owner_network_t *network = manager->network;
    assert(manager->phase == LCS_CONFIG_RECOVERY_COMPLETE && !coordinator->application_owner);
    assert(!manager->cleanup && !manager->next_cleanup && !network->recovery && !network->recovery_context);
    assert(lcs_config_recovery_process(manager, NULL, 0, NULL, NULL) == 1);
    lcs_config_recovery_stop(manager); /* Successful transfer is not shutdown. */
    assert(!coordinator->closing && !network->closing);
    assert(!lcs_config_recovery_close(manager));
    lcs_config_owner_network_stop(network); lcs_config_coordinator_stop(coordinator);
    assert(lcs_config_coordinator_can_close(coordinator));
}

static void sync_peer(size_t node, uint64_t instance)
{
    peer_runtime_t *peer = &g_state.peers[node];
    peer->fd = 44; peer->conn_state = LCS_PEER_ESTABLISHED;
    peer->instance_id = instance; peer->protocol_version = LCS_PROTO_FEATURE_RESOURCE_GENERATION;
    peer->online = true; peer->last_seen_ms = lcs_now_ms();
    peer->initial_sync_complete = peer->resource_generation_known = peer->resource_generation_matches = peer->voting_ready = true;
    peer->resource_generation = g_state.resource_generation;
}

static void test_concrete_recovery_tables(void)
{
    setup(false, false); recovery_live_target();
    lcs_config_runtime_t peer = {0}; lcs_config_store_t peer_store;
    char path[] = "/tmp/lcs-config-recovery-peer.XXXXXX"; recover_peer(&peer, &peer_store, path);
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *runtime.base; strcpy(g_state.cfg.resources[0].systemd_unit, "wrong-ini.service");
    g_state.config_runtime = runtime; memset(&runtime, 0, sizeof(runtime));
    recovery_projection_open();
    g_state.self_index = 0; g_state.instance_id = 900;
    for (size_t i = 0; i < g_state.cfg.node_count; i++) g_state.peers[i].fd = -1;
    recovery_bus_t bus = {0}; lcs_config_recovery_t managers[2] = {{0}};
    lcs_config_recovery_live_t live = {0}; live_cancellations = 0; live_reload = live_cancel_bump = false;
    /* An occupied removed slot, a private promise, or an open session prevents
     * the one-time pre-listener reset. None may be erased by preparation. */
    g_state.resources[LCS_MAX_RESOURCES - 1].service.pid = 999;
    assert(lcs_config_recovery_live_prepare(&live, &managers[0], -1, error, sizeof(error)) < 0);
    assert(!g_state.resource_generation_enabled && !g_state.config_application_fenced && !live_cancellations);
    g_state.resources[LCS_MAX_RESOURCES - 1].service.pid = 0;
    g_state.lease_grants[LCS_MAX_RESOURCES - 1].promised_epoch = 77;
    assert(lcs_config_recovery_live_prepare(&live, &managers[0], -1, error, sizeof(error)) < 0);
    assert(g_state.lease_grants[LCS_MAX_RESOURCES - 1].promised_epoch == 77 && !live_cancellations);
    g_state.lease_grants[LCS_MAX_RESOURCES - 1].promised_epoch = 0;
    g_state.peers[1].fd = 44;
    assert(lcs_config_recovery_live_prepare(&live, &managers[0], -1, error, sizeof(error)) < 0);
    g_state.peers[1].fd = -1;
    assert(!lcs_config_recovery_live_prepare(&live, &managers[0], -1, error, sizeof(error)));
    assert(lcs_config_recovery_live_prepare(&live, &managers[0], -1, error, sizeof(error)) < 0);
    assert(g_state.cfg.resource_schema_version == 2 && g_state.cfg.resource_count == 2);
    assert(!strcmp(g_state.cfg.resources[0].systemd_unit, "applied.service") && !strcmp(g_state.cfg.resources[1].address, "192.0.2.20/32"));
    assert(g_state.resource_generation.revision == 2 && !g_state.resource_generation_admitted && live_cancellations == 3);
    assert(g_state.config_runtime.requires_recovery && g_state.config_application_remapping && !g_state.voting_ready);
    assert(g_state.resources[LCS_MAX_RESOURCES - 1].owner_node == -1 && g_state.lease_grants[LCS_MAX_RESOURCES - 1].owner_node == -1);
    /* A caller may recover safety observations/private promises while fenced.
     * Physical startup cleanup must not erase a matching incarnation's promise. */
    for (size_t i = 0; i < 2; i++)
    {
        g_state.resources[i].state = LCS_RES_ACTIVE; g_state.resources[i].owner_node = 1;
        g_state.resources[i].owner_instance_id = 901; g_state.resources[i].epoch = 50 + i;
        g_state.resources[i].lease_id = 600 + i; g_state.resources[i].lease_deadline_ms = lcs_now_ms() + 30000;
        g_state.resources[i].disabled = true; g_state.resources[i].disabled_generation = 10 + i;
        g_state.resources[i].home_blocked = true; g_state.resources[i].home_generation = 20 + i;
        g_state.lease_grants[i] = (lease_grant_t){.active = true, .owner_node = 1, .owner_instance_id = 901,
            .epoch = 50 + i, .lease_id = 600 + i, .deadline_ms = lcs_now_ms() + 30000, .promised_epoch = 100 + i};
    }
    lease_grant_t promised = g_state.lease_grants[0]; promised.promised_epoch++;
    live_cancel_bump = true;
    lcs_config_coordinator_t peer_coordinator; lcs_config_readiness_t readiness[2] = {0};
    lcs_config_coordinator_init(&g_state.config_coordinator, &g_state.config_runtime, &readiness[0]);
    lcs_config_coordinator_init(&peer_coordinator, &peer, &readiness[1]);
    assert(!lcs_config_coordinator_recover(&g_state.config_coordinator) && !lcs_config_coordinator_recover(&peer_coordinator));
    assert(!lcs_config_owner_network_init(&bus.nodes[0], g_state.config_runtime.base, &g_state.config_runtime.application, 0, 900));
    assert(!lcs_config_owner_network_init(&bus.nodes[1], peer.base, &peer.application, 1, 901));
    recovery_install_t other = {.manager = &managers[1]};
    assert(!lcs_config_recovery_start(&managers[0], &g_state.config_coordinator, &bus.nodes[0], lcs_config_recovery_live_install, &live));
    assert(!lcs_config_recovery_start(&managers[1], &peer_coordinator, &bus.nodes[1], recovery_install, &other));
    assert(!symlinkat("../agreement.json", g_state.config_bootstrap.snapshots.directory_fd, "resources.json"));
    live_owner_network = &bus.nodes[0];
    bool checked = false, closed_proof = false, checked_retry = false, repaired_projection = false;
    for (unsigned i = 0; i < 10000; i++)
    {
        if (!checked && managers[0].phase == LCS_CONFIG_RECOVERY_INSTALL)
        {
            lcs_config_reconcile_view_t view = {.old_config = &managers[0].configs[0], .target = &managers[0].configs[1],
                .plan = &managers[0].plan, .value = &managers[0].target, .attempt = managers[0].attempt};
            lcs_config_application_plan_t invalid = managers[0].plan;
            invalid.new_to_old[0] = 0; view.plan = &invalid;
            assert(lcs_config_recovery_live_install(&live, &view, error, sizeof(error)) < 0 && live_cancellations == 3);
            view.plan = &managers[0].plan;
            g_state.resources[LCS_MAX_RESOURCES - 1].hook.next_type = LCS_HOOK_POST_STOP;
            assert(!lcs_config_recovery_live_install(&live, &view, error, sizeof(error)) && live_cancellations == 3);
            g_state.resources[LCS_MAX_RESOURCES - 1].hook.next_type = LCS_HOOK_NONE;
            checked = true;
        }
        if (!checked_retry && live.attempt && managers[0].phase == LCS_CONFIG_RECOVERY_INSTALL &&
            lcs_config_owner_poll(&bus.nodes[0].round, lcs_now_ms()) == 1)
        {
            lcs_config_reconcile_view_t view = {.old_config = &managers[0].configs[0], .target = &managers[0].configs[1],
                .plan = &managers[0].plan, .value = &managers[0].target, .attempt = managers[0].attempt};
            char saved = g_state.resource_generation.digest[0];
            g_state.resource_generation.digest[0] = saved == 'a' ? 'b' : 'a';
            assert(lcs_config_recovery_live_install(&live, &view, error, sizeof(error)) < 0 && live_cancellations == 6);
            g_state.resource_generation.digest[0] = saved;
            assert(lcs_config_recovery_live_install(&live, &view, error, sizeof(error)) == 1 && live_cancellations == 6);
            checked_retry = true;
        }
        manager_tick(&bus, managers);
        for (size_t n = 0; n < 2; n++)
            if (managers[n].phase == LCS_CONFIG_RECOVERY_BLOCKED)
            {
                if (!n && !repaired_projection && g_state.config_coordinator.completed_projections && g_state.config_coordinator.last_projection_result)
                {
                    assert(managers[n].blocked_phase == LCS_CONFIG_RECOVERY_INSTALL && live.attempt == 0 && live_cancellations == 3);
                    assert(!g_state.config_coordinator.failed && !g_state.config_runtime.agreement.poisoned && !g_state.config_runtime.application.poisoned);
                    assert(g_state.config_runtime.application.applied.revision == 2 && g_state.config_runtime.application.pending.revision == 3);
                    assert(g_state.cfg.resource_count == 2 && g_state.lease_grants[0].promised_epoch == 100);
                    assert(!unlinkat(g_state.config_bootstrap.snapshots.directory_fd, "resources.json", 0));
                    repaired_projection = true;
                }
                if (!n && managers[n].blocked_phase == LCS_CONFIG_RECOVERY_FINISH) closed_proof = true;
                lcs_config_runtime_t *other_runtime = n ? &g_state.config_runtime : &peer;
                if (managers[1 - n].cleanup && lcs_config_startup_verified(managers[1 - n].cleanup,
                    other_runtime, bus.nodes[1 - n].instance))
                    (void)lcs_config_recovery_retry(&managers[n], bus.now);
            }
        if (managers[0].phase == LCS_CONFIG_RECOVERY_READY && managers[1].phase == LCS_CONFIG_RECOVERY_READY) break;
        if (i == 9999) fprintf(stderr, "live recovery states %u/%u errors %s / %s\n", managers[0].phase, managers[1].phase, managers[0].error, managers[1].error);
        assert(i < 9999); usleep(1000);
    }
    assert(checked && closed_proof && checked_retry && repaired_projection && live_cancellations == 6 && live.attempt == 1);
    assert(g_state.cfg.resource_count == 3 && !strcmp(g_state.cfg.resources[1].name, "a_parent"));
    assert(g_state.resources[1].epoch == 50 && !memcmp(&g_state.lease_grants[1], &promised, sizeof(promised)));
    assert(g_state.resources[1].disabled && g_state.resources[1].disabled_generation == 10);
    assert(g_state.resources[1].home_blocked && g_state.resources[1].home_generation == 20);
    for (size_t i = 0; i < 3; i++)
        assert(g_state.resources[i].state == LCS_RES_STOPPED && g_state.resources[i].owner_node == -1 &&
            !g_state.resources[i].owner_instance_id && !g_state.resources[i].lease_id && !g_state.resources[i].lease_deadline_ms && resource_config_is_fenced((int)i));
    assert(!g_state.resources[0].epoch && !g_state.resources[2].epoch);
    assert(!g_state.lease_grants[0].promised_epoch && !g_state.lease_grants[2].promised_epoch && !g_state.resources[2].disabled_generation);
    assert(g_state.resource_generation.revision == 3 && !g_state.resource_generation_admitted && g_state.config_runtime.requires_recovery);
    assert(!g_state.voting_ready && !g_state.votes_seen && !lcs_config_coordinator_can_vote(&g_state.config_coordinator));
    recovery_projection_matches(&g_state.config_runtime.agreement.committed);
    assert(g_state.config_coordinator.completed_projections == 2);
    uint64_t serial = bus.nodes[0].serial;
    lcs_config_owner_scope_t retired = bus.nodes[0].retired_scope;
    resource_runtime_t retained = g_state.resources[1]; lease_grant_t private_grant = g_state.lease_grants[1];
    sync_peer(1, 901); /* Cached observations must be discarded at sync admission. */
    assert(!lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)));
    assert(lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)) < 0);
    manager_tick(&bus, managers);
    assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING && live.synchronizing);
    assert(g_state.resource_generation_admitted && !g_state.config_application_remapping && g_state.config_application_fenced);
    assert(!g_state.peers[1].initial_sync_complete && !g_state.peers[1].resource_generation_matches && g_state.peers[1].state_sync_pending);
    assert(!g_state.voting_ready && !lcs_config_coordinator_can_vote(&g_state.config_coordinator) && resource_config_is_fenced(1));
    sync_peer(1, 901); g_state.voting_not_before_ms = lcs_now_ms() + 60000;
    manager_tick(&bus, managers);
    assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING && !g_state.voting_ready);
    g_state.voting_not_before_ms = 0; admission_cleanup_blocked = true;
    manager_tick(&bus, managers);
    assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING && !g_state.voting_ready);
    admission_cleanup_blocked = false;
    g_state.peers[1].protocol_version = LCS_PEER_PROTO_MIN_VERSION;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    g_state.peers[1].protocol_version = LCS_PROTO_FEATURE_RESOURCE_GENERATION;
    g_state.peers[1].instance_id = 999;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    g_state.peers[1].instance_id = 901; g_state.peers[1].fd = -1;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    g_state.peers[1].fd = 44; g_state.peers[1].last_seen_ms = lcs_now_ms() - g_state.cfg.peer_timeout_ms - 1;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    sync_peer(1, 901); g_state.peers[1].resource_generation.digest[0] = g_state.peers[1].resource_generation.digest[0] == 'a' ? 'b' : 'a';
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    sync_peer(1, 901); g_state.peers[1].initial_sync_complete = false;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    sync_peer(1, 901); g_state.peers[1].voting_ready = false;
    manager_tick(&bus, managers); assert(managers[0].phase == LCS_CONFIG_RECOVERY_ADMITTING);
    assert(resource_config_is_fenced(1) && !lcs_config_coordinator_can_vote(&g_state.config_coordinator));
    sync_peer(1, 901); manager_tick(&bus, managers);
    assert(managers[0].phase == LCS_CONFIG_RECOVERY_COMPLETE && live.admitted && !live.synchronizing);
    assert(!g_state.config_runtime.requires_recovery && !g_state.config_application_fenced && !g_state.config_application_remapping);
    assert(g_state.resource_generation_admitted && cluster_local_voting_ready() && cluster_has_quorum());
    assert(lcs_config_coordinator_can_vote(&g_state.config_coordinator));
    assert(!memcmp(&retained, &g_state.resources[1], sizeof(retained)) && !memcmp(&private_grant, &g_state.lease_grants[1], sizeof(private_grant)));
    assert(bus.nodes[0].serial == serial && lcs_config_owner_scope_equal(&bus.nodes[0].retired_scope, &retired));
    lcs_config_owner_message_t query = {.kind = LCS_CONFIG_OWNER_APPLIED_QUERY, .sender_instance = 901,
        .target_instance = 900, .id = 9000, .scope = retired};
    unsigned char wire[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t wire_length;
    assert(!lcs_config_owner_encode(&query, wire, sizeof(wire), &wire_length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[0], 1, 901, wire, wire_length, lcs_now_ms()));
    assert(bus.nodes[0].replies[1].result == LCS_CONFIG_OWNER_STOPPED); /* No freed startup provider is needed. */
    query.scope.old_revision = 3; query.scope.target_revision = 4; query.scope.drain = 0;
    strcpy(query.scope.old_value, g_state.config_runtime.application.applied.digest);
    strcpy(query.scope.target_value, query.scope.old_value);
    query.scope.target_value[0] = query.scope.target_value[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_owner_encode(&query, wire, sizeof(wire), &wire_length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[0], 1, 901, wire, wire_length, lcs_now_ms()));
    assert(bus.nodes[0].replies[1].result == LCS_CONFIG_OWNER_PENDING);
    live_owner_network = NULL; live_cancel_bump = false;
    close_admitted_manager(&managers[0]); stop_manager(&managers[1]);
    lcs_config_runtime_close(&peer); forbid_io = false;
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++) assert(!unlinkat(peer_store.directory_fd, names[i], 0));
    lcs_config_store_close(&peer_store); assert(!rmdir(path));
    recovery_projection_close();
    runtime = g_state.config_runtime; memset(&g_state, 0, sizeof(g_state));
    lcs_config_startup_t empty = {0}; cleanup(&empty);
}

static void test_concrete_quorum_recovery_tables(void)
{
    setup(false, true); memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *runtime.base; g_state.config_runtime = runtime; memset(&runtime, 0, sizeof(runtime));
    recovery_projection_open();
    g_state.self_index = 2; g_state.instance_id = 902;
    for (size_t i = 0; i < g_state.cfg.node_count; i++) g_state.peers[i].fd = -1;
    live_cancellations = 0;
    lcs_config_recovery_t manager = {0}; lcs_config_recovery_live_t live = {0};
    assert(!lcs_config_recovery_live_prepare(&live, &manager, -1, error, sizeof(error)));
    lcs_config_coordinator_init(&g_state.config_coordinator, &g_state.config_runtime, &g_state.config_readiness);
    assert(!lcs_config_coordinator_recover(&g_state.config_coordinator));
    assert(!lcs_config_owner_network_init(&g_state.config_owners, g_state.config_runtime.base, &g_state.config_runtime.application, 2, 902));
    assert(!lcs_config_recovery_start(&manager, &g_state.config_coordinator, &g_state.config_owners, lcs_config_recovery_live_install, &live));
    uint64_t instances[LCS_MAX_NODES] = {0, 0, 902};
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (g_state.config_coordinator.phase == LCS_CONFIG_COORD_PROJECTING) break;
        assert(i < 4999); usleep(1000);
    }
    assert(live.projection.completion == 1 && live.attempt == 0);
    for (unsigned i = 0; g_state.config_coordinator.storage.pid > 0 && i < 5000; i++)
    { (void)lcs_config_coordinator_process(&g_state.config_coordinator, lcs_now_ms()); usleep(1000); }
    assert(g_state.config_coordinator.storage.pid <= 0 && g_state.config_coordinator.projected_revision == 2);
    recovery_projection_matches(&g_state.config_runtime.agreement.committed);
    /* Learn a newer checkpoint before the adapter observes its old publication
     * completion. It must publish 4, even while its installation target is 2. */
    char *document; size_t length;
    assert(!lcs_resource_config_encode(&g_state.cfg, 4, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, g_state.config_runtime.base, "publication-checkpoint-4", document, length, error, sizeof(error)));
    lcs_config_agreement_job_t job = {.action = LCS_CONFIG_CHECKPOINT, .ballot = 128, .voters = 5, .value = &value};
    strcpy(job.parent_digest, g_state.config_runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&g_state.config_coordinator, &job, error, sizeof(error)));
    free(document);
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (g_state.config_coordinator.projected_revision != 4) assert(live.attempt == 0);
        if (manager.phase == LCS_CONFIG_RECOVERY_READY && g_state.config_runtime.application.applied.revision == 4) break;
        assert(i < 4999); usleep(1000);
    }
    lcs_config_value_clear(&value);
    assert(manager.phase == LCS_CONFIG_RECOVERY_READY && live.attempt == 2 && live_cancellations == 6);
    assert(g_state.cfg.resource_count == 2 && g_state.resource_generation.revision == 4 && !g_state.resource_generation_admitted);
    assert(g_state.config_application_remapping && g_state.config_runtime.requires_recovery && !g_state.voting_ready);
    assert(g_state.config_coordinator.completed_projections == 2 && g_state.config_coordinator.projected_revision == 4);
    assert(access(trace, F_OK) < 0 && errno == ENOENT);
    recovery_projection_matches(&g_state.config_runtime.agreement.committed);
    g_state.resources[0].epoch = 30; g_state.lease_grants[0].promised_epoch = 37;
    assert(!lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)));
    assert(!lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL));
    assert(manager.phase == LCS_CONFIG_RECOVERY_ADMITTING && g_state.resource_generation_admitted && live.synchronizing);
    assert(!lcs_resource_config_encode(&g_state.cfg, 5, &document, &length, error, sizeof(error)));
    assert(!lcs_config_value_init(&value, g_state.config_runtime.base, "admission-checkpoint-5", document, length, error, sizeof(error)));
    job.ballot = 160; job.value = &value; strcpy(job.parent_digest, g_state.config_runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&g_state.config_coordinator, &job, error, sizeof(error)));
    free(document);
    assert(!lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL));
    assert(manager.phase == LCS_CONFIG_RECOVERY_READY && !live.synchronizing && !live.admitted);
    assert(g_state.config_application_remapping && g_state.config_application_fenced && !g_state.resource_generation_admitted && !g_state.voting_ready);
    assert(g_state.config_coordinator.application_owner == &manager && g_state.resources[0].epoch == 30 && g_state.lease_grants[0].promised_epoch == 37);
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (manager.phase == LCS_CONFIG_RECOVERY_READY && g_state.config_runtime.application.applied.revision == 5) break;
        assert(i < 4999); usleep(1000);
    }
    lcs_config_value_clear(&value);
    assert(g_state.resources[0].epoch == 30 && g_state.lease_grants[0].promised_epoch == 37);
    recovery_projection_matches(&g_state.config_runtime.agreement.committed);
    assert(!lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)));
    assert(!lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL));
    g_state.voting_not_before_ms = 0; instances[0] = 900; sync_peer(0, 900);
    assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) == 1);
    assert(manager.phase == LCS_CONFIG_RECOVERY_COMPLETE && live.admitted && !g_state.config_runtime.requires_recovery);
    assert(g_state.resources[0].epoch == 30 && g_state.lease_grants[0].promised_epoch == 37 && !g_state.config_application_fenced);
    assert(access(trace, F_OK) < 0 && errno == ENOENT);
    close_admitted_manager(&manager); recovery_projection_close(); runtime = g_state.config_runtime; memset(&g_state, 0, sizeof(g_state));
    lcs_config_startup_t empty = {0}; cleanup(&empty);
}

static void test_recovery_projection_cancellation(void)
{
    setup(false, true); memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *runtime.base; g_state.config_runtime = runtime; memset(&runtime, 0, sizeof(runtime));
    recovery_projection_open(); g_state.self_index = 2; g_state.instance_id = 902;
    for (size_t i = 0; i < g_state.cfg.node_count; i++) g_state.peers[i].fd = -1;
    live_cancellations = 0;
    lcs_config_recovery_t manager = {0}; lcs_config_recovery_live_t live = {0};
    assert(!lcs_config_recovery_live_prepare(&live, &manager, -1, error, sizeof(error)));
    lcs_config_coordinator_init(&g_state.config_coordinator, &g_state.config_runtime, &g_state.config_readiness);
    assert(!lcs_config_coordinator_recover(&g_state.config_coordinator));
    assert(!lcs_config_owner_network_init(&g_state.config_owners, g_state.config_runtime.base, &g_state.config_runtime.application, 2, 902));
    assert(!lcs_config_recovery_start(&manager, &g_state.config_coordinator, &g_state.config_owners, lcs_config_recovery_live_install, &live));
    uint64_t instances[LCS_MAX_NODES] = {0, 0, 902}; stop_writer_sync = true;
    for (unsigned i = 0; i < 5000; i++)
    {
        assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0);
        if (g_state.config_coordinator.phase == LCS_CONFIG_COORD_PROJECTING) break;
        assert(i < 4999); usleep(1000);
    }
    pid_t writer = g_state.config_coordinator.storage.pid; assert(writer > 0 && live.attempt == 0);
    bool stopped = false;
    for (unsigned i = 0; i < 5000; i++)
    {
        int status; pid_t got = waitpid(writer, &status, WNOHANG | WUNTRACED);
        if (got > 0) { assert(WIFSTOPPED(status)); stopped = true; break; }
        assert(i < 4999); usleep(1000);
    }
    assert(stopped && live.projection.completion == 1 && !lcs_config_coordinator_can_close(&g_state.config_coordinator));
    stop_manager(&manager); stop_writer_sync = false;
    assert(g_state.config_coordinator.storage.pid <= 0 && !g_state.config_coordinator.completed_projections && !g_state.config_coordinator.completed_applications);
    assert(g_state.config_runtime.agreement.poisoned && g_state.config_runtime.requires_recovery && !g_state.resource_generation_admitted);
    assert(g_state.config_runtime.application.applied.revision == 2 && !g_state.config_runtime.application.pending.document);
    recovery_projection_close(); runtime = g_state.config_runtime; memset(&g_state, 0, sizeof(g_state));
    lcs_config_startup_t empty = {0}; cleanup(&empty);
}

static void test_recovery_admission_interruption(void)
{
    for (unsigned fault = 0; fault < 2; fault++)
    {
        setup(false, true); memset(&g_state, 0, sizeof(g_state));
        g_state.cfg = *runtime.base; g_state.config_runtime = runtime; memset(&runtime, 0, sizeof(runtime));
        recovery_projection_open(); g_state.self_index = 2; g_state.instance_id = 902;
        for (size_t i = 0; i < g_state.cfg.node_count; i++) g_state.peers[i].fd = -1;
        live_cancellations = 0;
        lcs_config_recovery_t manager = {0}; lcs_config_recovery_live_t live = {0};
        assert(!lcs_config_recovery_live_prepare(&live, &manager, -1, error, sizeof(error)));
        lcs_config_coordinator_init(&g_state.config_coordinator, &g_state.config_runtime, &g_state.config_readiness);
        assert(!lcs_config_coordinator_recover(&g_state.config_coordinator));
        assert(!lcs_config_owner_network_init(&g_state.config_owners, g_state.config_runtime.base, &g_state.config_runtime.application, 2, 902));
        assert(!lcs_config_recovery_start(&manager, &g_state.config_coordinator, &g_state.config_owners, lcs_config_recovery_live_install, &live));
        assert(lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)) < 0);
        uint64_t instances[LCS_MAX_NODES] = {0, 0, 902};
        for (unsigned i = 0; manager.phase != LCS_CONFIG_RECOVERY_READY && i < 5000; i++)
        { assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) >= 0); usleep(1000); }
        assert(manager.phase == LCS_CONFIG_RECOVERY_READY);
        assert(!lcs_config_recovery_live_begin_admission(&live, error, sizeof(error)));
        assert(!lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL));
        assert(live.synchronizing && g_state.resource_generation_admitted && !g_state.config_application_remapping);
        g_state.lease_grants[0].promised_epoch = 17; g_state.voting_ready = true; g_state.votes_seen = 2;
        if (fault)
        {
            g_state.config_coordinator.failed = true;
            assert(lcs_config_recovery_process(&manager, instances, lcs_now_ms(), no_owner_send, NULL) < 0);
            assert(manager.phase == LCS_CONFIG_RECOVERY_FAILED && g_state.config_owners.closing);
            assert(!g_state.resource_generation_admitted && g_state.config_application_remapping && !g_state.voting_ready && !g_state.votes_seen);
            assert(g_state.config_coordinator.application_owner == &manager);
        }
        stop_manager(&manager);
        assert(!live.synchronizing && !live.admitted && !g_state.resource_generation_admitted && g_state.config_application_remapping);
        assert(!g_state.voting_ready && !g_state.votes_seen && g_state.config_application_fenced && g_state.config_runtime.requires_recovery);
        assert(g_state.lease_grants[0].promised_epoch == 17);
        recovery_projection_close(); runtime = g_state.config_runtime; memset(&g_state, 0, sizeof(g_state));
        lcs_config_startup_t empty = {0}; cleanup(&empty);
    }
}

int main(void)
{
    parent = getpid();
    test_applied_cleanup_and_pending_receipt(); test_failures_and_retry(); test_timeout_cancellation_and_binding();
    test_quorum_only_and_completed_history(); test_workers_die_with_parent();
    test_completed_history_cleanup_and_legacy(); test_differing_history_scopes();
    test_differing_target_recovery();
    test_recovery_manager_skew_and_repairs(); test_quorum_recovery_manager_and_cancellation();
    test_recovery_manager_storage_failure_and_worker_stop(); test_recovery_manager_missing_evidence();
    test_concrete_recovery_tables(); test_concrete_quorum_recovery_tables();
    test_recovery_projection_cancellation(); test_recovery_admission_interruption();
    puts("authoritative startup cleanup, pending receipt recovery and worker lifetime unit tests passed");
    return 0;
}
