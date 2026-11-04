// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_reconcile.h"
#include "peer.h"
#include <assert.h>
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

daemon_state_t g_state;
static unsigned generation_cancellations;
static bool generation_reload;

bool config_reload_in_progress(void) { return generation_reload; }
void move_cancel_all(int epoll_fd, const char *message)
{
    (void)epoll_fd; (void)message;
    assert(!g_state.resource_generation_admitted && !g_state.voting_ready);
    assert(generation_cancellations % 3 == 0); generation_cancellations++;
}
void lease_cancel_all_operations(void)
{
    assert(generation_cancellations % 3 == 1); generation_cancellations++;
}
void peer_close_all_for_config_reload(int epoll_fd)
{
    (void)epoll_fd;
    assert(generation_cancellations % 3 == 2); generation_cancellations++;
}

static lcs_config_store_t store;
static lcs_config_runtime_t runtime;
static lcs_config_readiness_t readiness;
static lcs_config_coordinator_t coordinator;
static lcs_config_reconcile_t reconcile;
static char directory[80], error[256];
static pid_t parent;
static bool forbid_io, fail_drain, fail_admit, wait_owners, wait_admit;
static int stop_sync, fail_sync;
static unsigned steps[128], count;
static uint64_t installed, admitted;

int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_io || getpid() != parent);
    if (stop_sync && !--stop_sync)
        raise(SIGSTOP);
    if (fail_sync && !--fail_sync)
    { errno = EIO; return -1; }
    return __real_fsync(fd);
}

static int step(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t err_len)
{
    assert(context == &count && view->attempt && view->old_config && view->target && view->plan);
    assert(coordinator.application_owner == &reconcile && !lcs_config_coordinator_can_vote(&coordinator));
    assert(view->value->revision == reconcile.value.revision && !strcmp(view->value->digest, reconcile.value.digest));
    if (reconcile.phase == LCS_CONFIG_RECONCILE_ADMIT)
    {
        assert(!runtime.application.pending.document && installed == runtime.application.applied.revision);
        assert(lcs_config_value_equal(&runtime.application.applied, &runtime.agreement.committed));
        if (fail_admit)
        {
            snprintf(err, err_len, "resource generation context is not established");
            return -1;
        }
        if (wait_admit)
            return 0;
        admitted = view->value->revision;
    }
    else
    {
        assert(lcs_config_value_equal(&runtime.application.pending, view->value));
        assert(runtime.application.applied.revision < view->value->revision);
        if (reconcile.phase == LCS_CONFIG_RECONCILE_DRAIN && fail_drain)
        {
            snprintf(err, err_len, "backend stop failed; confirmed cleanup/fencing required");
            return -1;
        }
        if (reconcile.phase == LCS_CONFIG_RECONCILE_OWNERS && wait_owners)
            return 0;
        if (reconcile.phase == LCS_CONFIG_RECONCILE_INSTALL)
            installed = view->value->revision;
    }
    assert(count < sizeof(steps) / sizeof(steps[0]));
    steps[count++] = (unsigned)reconcile.phase;
    return 1;
}

static const lcs_config_reconcile_ops_t ops = {step, step, step, step, step};

static void setup(bool quorum_only)
{
    memset(&runtime, 0, sizeof(runtime));
    memset(&readiness, 0, sizeof(readiness));
    count = 0; installed = admitted = 0;
    fail_drain = fail_admit = wait_owners = wait_admit = forbid_io = false;
    stop_sync = fail_sync = 0;
    lcs_config_t *base = malloc(sizeof(*base));
    assert(base);
    lcs_config_init_defaults(base);
    strcpy(base->cluster_name, "reconcile"); strcpy(base->self_name, quorum_only ? "node2" : "node0");
    base->node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(base->nodes[i].name, sizeof(base->nodes[i].name), "node%zu", i);
        strcpy(base->nodes[i].address, "127.0.0.1"); base->nodes[i].port = 3322;
        base->nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    base->resource_count = 1;
    strcpy(base->resources[0].name, "vip"); base->resources[0].type = LCS_RESOURCE_VIP;
    strcpy(base->resources[0].address, "192.0.2.1/32"); strcpy(base->resources[0].interface, "eth0");
    assert(!lcs_config_finalize(base, error, sizeof(error)));
    strcpy(directory, "/tmp/lcs-reconcile.XXXXXX"); assert(mkdtemp(directory));
    assert(!lcs_config_store_open(&store, directory, error, sizeof(error)));
    runtime.base = malloc(sizeof(*runtime.base)); assert(runtime.base);
    assert(!lcs_config_activation_open(&runtime.activation, &store, base, runtime.base, true, error, sizeof(error)));
    free(base);
    assert(!lcs_config_activation_resume(&runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)));
    lcs_config_barrier_round_t round;
    assert(!lcs_config_barrier_begin(&round, &runtime.activation, &runtime.agreement, &runtime.application, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++)
        assert(!lcs_config_barrier_ready(&round, i, round.membership, round.baseline, error, sizeof(error)));
    assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
    assert(!lcs_config_barrier_open(&runtime.barrier, &runtime.activation, &runtime.agreement,
                                    &runtime.application, &round, error, sizeof(error)));
    lcs_config_coordinator_init(&coordinator, &runtime, &readiness);
    assert(!lcs_config_coordinator_recover(&coordinator));
    lcs_config_reconcile_init(&reconcile, &coordinator, &ops, &count);
}

static lcs_config_value_t successor(const lcs_config_value_t *previous, bool remove)
{
    lcs_config_t *configs = malloc(3 * sizeof(*configs)); assert(configs);
    uint64_t revision;
    assert(!lcs_resource_config_decode(previous->document, previous->length, runtime.base, &configs[0], &revision, error, sizeof(error)));
    configs[1] = configs[0];
    if (remove)
        configs[1].resource_count = 0;
    assert(!lcs_resource_config_assign_identities(&configs[0], revision, &configs[1], &configs[2], error, sizeof(error)));
    char *document; size_t length;
    assert(!lcs_resource_config_encode(&configs[2], revision + 1, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0}; char request[32];
    snprintf(request, sizeof(request), "revision-%llu", (unsigned long long)(revision + 1));
    assert(!lcs_config_value_init(&value, runtime.base, request, document, length, error, sizeof(error)));
    free(document); free(configs);
    return value;
}

static void learn(const lcs_config_value_t *value)
{
    assert(!lcs_config_agreement_learn(&runtime.agreement, runtime.agreement.committed.digest,
        value->revision * 32, 3, value, error, sizeof(error)));
}

static int tick(void)
{
    (void)lcs_config_coordinator_process(&coordinator, 1000);
    int result = lcs_config_reconcile_process(&reconcile);
    usleep(1000);
    return result;
}

static void until(lcs_config_reconcile_phase_t phase)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        if (reconcile.phase == phase)
            return;
        assert(tick() >= 0 || reconcile.phase == phase);
    }
    assert(!"reconciliation did not reach expected phase");
}

static void cleanup(void)
{
    forbid_io = false; stop_sync = fail_sync = 0;
    lcs_config_reconcile_stop(&reconcile);
    for (unsigned i = 0; i < 5000 && !lcs_config_coordinator_can_close(&coordinator); i++)
    { (void)lcs_config_coordinator_process(&coordinator, 1000); usleep(1000); }
    assert(lcs_config_coordinator_can_close(&coordinator));
    assert(!lcs_config_reconcile_close(&reconcile));
    lcs_config_runtime_close(&runtime);
    const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock", ".application.json.tmp"};
    for (size_t i = 0; i < sizeof(names) / sizeof(names[0]); i++)
        assert(!unlinkat(store.directory_fd, names[i], 0) || errno == ENOENT);
    lcs_config_store_close(&store); assert(!rmdir(directory));
}

static void test_order_and_blocked_barriers(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    assert(!count && coordinator.application_owner == &reconcile);
    assert(lcs_config_reconcile_close(&reconcile) < 0);
    assert(lcs_config_reconcile_start(&reconcile) < 0);
    fail_drain = true;
    until(LCS_CONFIG_RECONCILE_BLOCKED);
    assert(runtime.application.pending.document && runtime.application.applied.revision == 0);
    assert(count == 1 && steps[0] == LCS_CONFIG_RECONCILE_QUIESCE);
    for (unsigned i = 0; i < 30; i++)
        assert(tick() < 0);
    assert(strstr(reconcile.error, "stop failed") && reconcile.blocked_phase == LCS_CONFIG_RECONCILE_DRAIN);
    fail_drain = false; wait_owners = true;
    assert(!lcs_config_reconcile_retry(&reconcile));
    until(LCS_CONFIG_RECONCILE_OWNERS);
    for (unsigned i = 0; i < 30; i++)
        assert(!tick());
    assert(!installed && !lcs_config_coordinator_can_vote(&coordinator));
    wait_owners = false; wait_admit = true;
    until(LCS_CONFIG_RECONCILE_ADMIT);
    assert(installed == 1 && runtime.application.applied.revision == 1 && !runtime.application.pending.document);
    assert(coordinator.completed_votes == 0 && coordinator.completed_applications == 2);
    assert(!lcs_config_coordinator_can_vote(&coordinator));
    assert(!tick() && !admitted);
    wait_admit = false; fail_admit = true;
    assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED);
    assert(runtime.application.applied.revision == 1 && !lcs_config_coordinator_can_vote(&coordinator));
    fail_admit = false;
    assert(!lcs_config_reconcile_retry(&reconcile));
    until(LCS_CONFIG_RECONCILE_DONE);
    assert(admitted == 1 && lcs_config_coordinator_can_vote(&coordinator));
    assert(count == 5);
    const unsigned expected[] = {LCS_CONFIG_RECONCILE_QUIESCE, LCS_CONFIG_RECONCILE_DRAIN,
        LCS_CONFIG_RECONCILE_OWNERS, LCS_CONFIG_RECONCILE_INSTALL, LCS_CONFIG_RECONCILE_ADMIT};
    for (unsigned i = 0; i < count; i++)
        assert(steps[i] == expected[i]);
    assert(!lcs_config_reconcile_start(&reconcile)); /* Already current: no storage/backend work. */
    assert(reconcile.phase == LCS_CONFIG_RECONCILE_DONE && count == 5);
    lcs_config_value_clear(&first); cleanup();
}

static void test_pending_target_and_newer_checkpoint(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    lcs_config_value_t second = successor(&first, true);
    forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_DRAIN);
    assert(reconcile.value.revision == 1 && reconcile.attempt == 1 && runtime.application.pending.revision == 1);
    lcs_config_agreement_job_t checkpoint = { .action = LCS_CONFIG_CHECKPOINT, .ballot = 64,
        .voters = 3, .value = &second };
    strcpy(checkpoint.parent_digest, first.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &checkpoint, error, sizeof(error)));
    while (!coordinator.completed_votes)
        assert(tick() >= 0);
    assert(runtime.agreement.committed.revision == 2 && reconcile.value.revision == 1);
    until(LCS_CONFIG_RECONCILE_ADMIT);
    assert(runtime.application.applied.revision == 1 && !admitted);
    assert(!tick());
    assert(reconcile.phase == LCS_CONFIG_RECONCILE_BEGIN && coordinator.application_owner == &reconcile);
    until(LCS_CONFIG_RECONCILE_DRAIN);
    assert(reconcile.value.revision == 2 && reconcile.attempt == 2 && reconcile.plan.cleanup == 1);
    until(LCS_CONFIG_RECONCILE_DONE);
    assert(installed == 2 && admitted == 2 && runtime.application.applied.revision == 2);
    assert(count == 9 && lcs_config_coordinator_can_vote(&coordinator));
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_checkpoint_during_admission(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    lcs_config_value_t second = successor(&first, true);
    forbid_io = true; wait_admit = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_ADMIT);
    lcs_config_agreement_job_t checkpoint = { .action = LCS_CONFIG_CHECKPOINT, .ballot = 64,
        .voters = 3, .value = &second };
    strcpy(checkpoint.parent_digest, first.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &checkpoint, error, sizeof(error)));
    unsigned previous = count;
    assert(!lcs_config_reconcile_process(&reconcile));
    assert(reconcile.phase == LCS_CONFIG_RECONCILE_ADMIT && count == previous && !admitted);
    until(LCS_CONFIG_RECONCILE_BEGIN);
    wait_admit = false;
    until(LCS_CONFIG_RECONCILE_DONE);
    assert(admitted == 2 && installed == 2 && lcs_config_coordinator_can_vote(&coordinator));
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_quorum_only_and_missing_adapters(void)
{
    setup(true);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    reconcile.ops.quiesce = reconcile.ops.drain = reconcile.ops.owners = NULL;
    forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_DONE);
    assert(count == 2 && steps[0] == LCS_CONFIG_RECONCILE_INSTALL && steps[1] == LCS_CONFIG_RECONCILE_ADMIT);
    assert(admitted == 1 && runtime.application.applied.revision == 1);
    lcs_config_value_clear(&first); cleanup();
    setup(false);
    reconcile.ops.owners = NULL;
    assert(lcs_config_reconcile_start(&reconcile) < 0 && !coordinator.application_owner);
    cleanup();
}

static void test_uncertain_writes_and_shutdown(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    forbid_io = true; fail_sync = 2;
    assert(!lcs_config_reconcile_start(&reconcile));
    assert(!tick()); fail_sync = 0;
    for (unsigned i = 0; i < 5000 && reconcile.phase != LCS_CONFIG_RECONCILE_RECOVERY; i++)
        tick();
    assert(reconcile.phase == LCS_CONFIG_RECONCILE_RECOVERY && coordinator.failed && !count);
    assert(lcs_config_reconcile_retry(&reconcile) < 0 && !lcs_config_coordinator_can_vote(&coordinator));
    lcs_config_value_clear(&first); cleanup();

    setup(false);
    first = successor(&runtime.agreement.committed, false); learn(&first);
    forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_FINISH);
    assert(installed == 1 && !admitted);
    fail_sync = 2;
    assert(!tick()); fail_sync = 0;
    for (unsigned i = 0; i < 5000 && reconcile.phase != LCS_CONFIG_RECONCILE_RECOVERY; i++)
        tick();
    assert(reconcile.phase == LCS_CONFIG_RECONCILE_RECOVERY && coordinator.failed);
    assert(runtime.application.applied.revision == 0 && runtime.application.pending.revision == 1 && !admitted);
    assert(lcs_config_reconcile_retry(&reconcile) < 0);
    lcs_config_value_clear(&first); cleanup();

    setup(false);
    first = successor(&runtime.agreement.committed, false); learn(&first);
    forbid_io = true; stop_sync = 1;
    assert(!lcs_config_reconcile_start(&reconcile));
    assert(!tick()); stop_sync = 0;
    assert(coordinator.storage.pid > 0 && !count && !runtime.application.pending.document);
    pid_t writer = coordinator.storage.pid;
    bool stopped = false;
    for (unsigned i = 0; i < 5000; i++)
    {
        int status;
        pid_t result = waitpid(writer, &status, WNOHANG | WUNTRACED);
        if (result > 0)
        { assert(WIFSTOPPED(status)); stopped = true; break; }
        usleep(1000);
    }
    assert(stopped);
    lcs_config_reconcile_stop(&reconcile);
    assert(coordinator.storage.pid == writer && lcs_config_reconcile_close(&reconcile) < 0);
    assert(tick() < 0 && !count);
    lcs_config_value_clear(&first); cleanup();
}

static void test_generation_installation_and_admission(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = *runtime.base; g_state.self_index = 0; g_state.instance_id = 100;
    g_state.config_runtime = runtime;
    g_state.config_coordinator = coordinator;
    g_state.config_bootstrap.initialization_locked = true;
    generation_cancellations = 0; generation_reload = true;
    assert(peer_install_resource_generation(-1, &first) < 0);
    generation_reload = false;
    g_state.resources[LCS_MAX_RESOURCES - 1].vip.pid = 999;
    assert(peer_install_resource_generation(-1, &first) == 1 && !generation_cancellations);
    g_state.resources[LCS_MAX_RESOURCES - 1].vip.pid = 0;
    g_state.resources[0].hook.next_type = LCS_HOOK_POST_STOP;
    assert(peer_install_resource_generation(-1, &first) == 1 && !generation_cancellations);
    g_state.resources[0].hook.next_type = LCS_HOOK_NONE;
    g_state.voting_ready = true; g_state.votes_seen = 3;
    assert(!peer_install_resource_generation(-1, &first));
    assert(generation_cancellations == 3 && g_state.resource_generation_enabled && !g_state.resource_generation_admitted);
    assert(!g_state.voting_ready && !g_state.votes_seen && g_state.cfg.resource_schema_version == LCS_RESOURCE_SCHEMA_VERSION);
    assert(peer_admit_resource_generation() < 0); /* Marker is not durable. */
    assert(!peer_install_resource_generation(-1, &first) && generation_cancellations == 3);
    uint64_t revision;
    assert(!lcs_resource_config_decode(first.document, first.length, runtime.base, &g_state.cfg, &revision, error, sizeof(error)));
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    g_state.config_runtime = runtime;
    g_state.config_coordinator.phase = LCS_CONFIG_COORD_VOTING;
    assert(peer_admit_resource_generation() < 0); /* A checkpoint is still in flight. */
    g_state.config_coordinator.phase = LCS_CONFIG_COORD_READY;
    strcpy(g_state.cfg.resources[0].address, "192.0.2.2/32");
    assert(peer_admit_resource_generation() < 0); /* Runtime table must match full snapshot. */
    assert(!lcs_resource_config_decode(first.document, first.length, runtime.base, &g_state.cfg, &revision, error, sizeof(error)));
    assert(!peer_admit_resource_generation() && g_state.resource_generation_admitted && !g_state.voting_ready);
    g_state.peers[1].state_sync_pending = false;
    assert(!peer_admit_resource_generation() && !g_state.peers[1].state_sync_pending);
    lcs_config_value_t second = successor(&first, true); learn(&second);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    g_state.config_runtime = runtime;
    assert(!peer_install_resource_generation(-1, &second) && generation_cancellations == 6);
    assert(g_state.cfg.resource_count == 1 && !g_state.resource_generation_admitted); /* Remap happens AFTER cancellation. */
    assert(peer_install_resource_generation(-1, &first) < 0);
    assert(!lcs_resource_config_decode(second.document, second.length, runtime.base, &g_state.cfg, &revision, error, sizeof(error)));
    assert(!lcs_config_application_finish(&runtime.application, second.digest, error, sizeof(error)));
    g_state.config_runtime = runtime;
    assert(!peer_admit_resource_generation() && g_state.resource_generation.revision == 2);
    memset(&g_state, 0, sizeof(g_state));
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

int main(void)
{
    parent = getpid();
    test_order_and_blocked_barriers();
    test_pending_target_and_newer_checkpoint();
    test_checkpoint_during_admission();
    test_quorum_only_and_missing_adapters();
    test_uncertain_writes_and_shutdown();
    test_generation_installation_and_admission();
    puts("durable configuration reconciliation ordering and safety barriers unit tests passed");
    return 0;
}
