// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_reconcile.h"
#include "peer.h"
#include "config_live.h"
#include "config_owner_barrier.h"
#include "util.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>

daemon_state_t g_state;
static unsigned generation_cancellations;
static bool generation_reload, expanded_fixture, all_full_fixture, live_mode, project_ready, owners_ready, invalid_owner_result;
static unsigned released[16], released_count, projected, dropped;
void resources_drop_local(int resource_idx, int epoll_fd)
{ (void)epoll_fd; assert(resource_idx == 0); dropped++; }

bool cluster_local_voting_ready(void) { return g_state.voting_ready; }
int cluster_has_quorum(void) { return g_state.votes_seen >= 2; }
void lease_cancel_operations(int resource_idx) { assert(resource_config_is_fenced(resource_idx)); }
void resources_release_local(int resource_idx, int epoll_fd)
{
    (void)epoll_fd;
    assert(released_count < 16 && resource_config_is_fenced(resource_idx));
    assert(!strcmp(g_state.cfg.resources[resource_idx].post_stop, resource_idx == 0 ? "/old-parent-stop" : "/old-child-stop"));
    released[released_count++] = (unsigned)resource_idx;
    g_state.resources[resource_idx].state = LCS_RES_STOPPING;
}

bool config_reload_in_progress(void) { return generation_reload; }
void move_cancel_all(int epoll_fd, const char *message)
{
    (void)epoll_fd;
    if (!strcmp(message, "resource definitions are being reconciled"))
    { assert(g_state.config_application_fenced); return; }
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

static const lcs_config_reconcile_ops_t ops = {step, step, step, step, step, NULL, NULL};

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
        base->nodes[i].role = i == 2 && !all_full_fixture ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    base->resource_count = 1;
    strcpy(base->resources[0].name, "vip"); base->resources[0].type = LCS_RESOURCE_VIP;
    strcpy(base->resources[0].address, "192.0.2.1/32"); strcpy(base->resources[0].interface, "eth0");
    if (expanded_fixture)
    {
        base->resource_count = 3;
        strcpy(base->resources[0].name, "a_parent");
        strcpy(base->resources[0].post_stop, "/old-parent-stop");
        base->resources[1] = base->resources[0];
        strcpy(base->resources[1].name, "b_child");
        strcpy(base->resources[1].address, "192.0.2.2/32");
        strcpy(base->resources[1].post_stop, "/old-child-stop");
        base->resources[1].depends_on_count = 1;
        strcpy(base->resources[1].depends_on_names[0], "a_parent");
        base->resources[2] = base->resources[0];
        strcpy(base->resources[2].name, "z_keep");
        strcpy(base->resources[2].address, "192.0.2.3/32");
    }
    if (lcs_config_finalize(base, error, sizeof(error)))
    { fprintf(stderr, "fixture config: %s\n", error); abort(); }
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
    if (live_mode)
    {
        g_state.config_runtime = runtime;
        g_state.config_coordinator = coordinator;
    }
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
    reconcile.ops.complete_admission = lcs_config_live_ops()->complete_admission;
    assert(lcs_config_reconcile_start(&reconcile) < 0); /* Two-stage callbacks must be paired. */
    reconcile.ops.complete_admission = NULL;
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

static int project_live(void *context, const lcs_config_value_t *committed, char *err, size_t length)
{
    (void)context; (void)err; (void)length;
    assert(lcs_config_value_equal(committed, &runtime.agreement.committed));
    projected++;
    return project_ready ? 1 : 0;
}

static int owner_live(void *context, const lcs_config_reconcile_view_t *view, char *err, size_t length)
{
    (void)context; (void)view; (void)err; (void)length;
    assert(!released_count || released_count == 2);
    return invalid_owner_result ? 2 : owners_ready ? 1 : 0;
}

static void complete_stopped(size_t index)
{
    g_state.resources[index].state = LCS_RES_STOPPED;
    g_state.resources[index].owner_node = -1;
    g_state.resources[index].owner_instance_id = 0;
    g_state.resources[index].lease_id = g_state.resources[index].lease_deadline_ms = 0;
    g_state.lease_grants[index].active = false;
    g_state.lease_grants[index].owner_node = -1;
}

static void live_synchronized(void)
{
    g_state.voting_ready = true; g_state.votes_seen = 2;
    size_t remote = g_state.self_index == 0 ? 1 : 0;
    peer_runtime_t *peer = &g_state.peers[remote];
    memset(peer, 0, sizeof(*peer));
    peer->fd = 17; peer->conn_state = LCS_PEER_ESTABLISHED; peer->instance_id = 101;
    peer->online = peer->voting_ready = peer->initial_sync_complete = true;
    peer->protocol_version = LCS_PROTO_FEATURE_RESOURCE_GENERATION;
    peer->last_seen_ms = lcs_now_ms();
    peer->resource_generation_known = peer->resource_generation_matches = true;
    peer->resource_generation = g_state.resource_generation;
}

static void observe_live_completion(void *context)
{
    assert(!coordinator.application_owner && reconcile.phase == LCS_CONFIG_RECONCILE_DONE);
    assert(g_state.config_application_fenced && g_state.config_application_drain_mask == UINT32_MAX);
    lcs_config_live_ops()->complete_admission(context);
}

static void test_live_drain_and_incarnation_remap(void)
{
    expanded_fixture = true; setup(false); expanded_fixture = false;
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    lcs_config_t *configs = malloc(3 * sizeof(*configs)); assert(configs);
    uint64_t revision;
    assert(!lcs_resource_config_decode(first.document, first.length, runtime.base, &configs[0], &revision, error, sizeof(error)));
    configs[1] = configs[0];
    strcpy(configs[1].resources[0].address, "192.0.2.9/32");
    strcpy(configs[1].resources[0].post_stop, "/new-parent-stop");
    configs[1].resources[3] = configs[1].resources[2];
    strcpy(configs[1].resources[3].name, "a_new"); strcpy(configs[1].resources[3].address, "192.0.2.4/32");
    configs[1].resource_count = 4;
    assert(!lcs_config_finalize(&configs[1], error, sizeof(error)));
    assert(!lcs_resource_config_assign_identities(&configs[0], 1, &configs[1], &configs[2], error, sizeof(error)));
    char *document; size_t length;
    assert(!lcs_resource_config_encode(&configs[2], 2, &document, &length, error, sizeof(error)));
    lcs_config_value_t second = {0};
    assert(!lcs_config_value_init(&second, runtime.base, "live-remap", document, length, error, sizeof(error)));
    free(document); learn(&second);
    memset(&g_state, 0, sizeof(g_state));
    g_state.cfg = configs[0]; g_state.self_index = 0; g_state.instance_id = 100;
    g_state.voting_ready = true; g_state.votes_seen = 3;
    generation_cancellations = 0; generation_reload = false; released_count = projected = 0;
    for (size_t i = 0; i < 3; i++)
    {
        g_state.resources[i].owner_node = 0; g_state.resources[i].owner_instance_id = 100;
        g_state.resources[i].state = LCS_RES_ACTIVE; g_state.resources[i].epoch = 10 + i;
        g_state.resources[i].lease_id = 1000 + i; g_state.resources[i].lease_deadline_ms = lcs_now_ms() + 10000;
        g_state.lease_grants[i].active = true; g_state.lease_grants[i].owner_node = 0;
        g_state.lease_grants[i].promised_epoch = 50 + i;
    }
    resource_runtime_t kept = g_state.resources[2]; lease_grant_t grant = g_state.lease_grants[2];
    lcs_config_live_t live;
    assert(lcs_config_live_init(&live, -1, project_live, NULL, NULL) < 0);
    assert(!lcs_config_live_init(&live, -1, project_live, owner_live, NULL));
    lcs_config_reconcile_init(&reconcile, &coordinator, lcs_config_live_ops(), &live);
    reconcile.ops.complete_admission = observe_live_completion;
    live_mode = true; project_ready = owners_ready = invalid_owner_result = false; forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_QUIESCE);
    assert(!tick());
    assert(g_state.config_application_fenced && g_state.config_application_drain_mask == 3 && !released_count);
    assert(projected && resource_config_is_fenced(0) && resource_config_is_fenced(1) && !resource_config_is_fenced(2));
    project_ready = true;
    until(LCS_CONFIG_RECONCILE_DRAIN);
    assert(!tick() && released_count == 1 && released[0] == 1);
    complete_stopped(1); g_state.resources[1].hook.pid = 999;
    assert(!tick() && released_count == 1); /* Child post-stop work blocks parent cleanup. */
    g_state.resources[1].hook.pid = 0;
    assert(!tick() && released_count == 2 && released[1] == 0);
    g_state.resources[0].state = LCS_RES_STOP_FAILED;
    assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED && !live.installed);
    complete_stopped(0);
    assert(!lcs_config_reconcile_retry(&reconcile));
    until(LCS_CONFIG_RECONCILE_OWNERS);
    for (unsigned i = 0; i < 5; i++) assert(!tick());
    assert(!live.installed && g_state.cfg.resource_count == 3);
    invalid_owner_result = true;
    assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED && !live.installed);
    invalid_owner_result = false; owners_ready = true;
    assert(!lcs_config_reconcile_retry(&reconcile));
    until(LCS_CONFIG_RECONCILE_FINISH);
    assert(live.installed && generation_cancellations == 3 && g_state.cfg.resource_count == 4);
    assert(!memcmp(&kept, &g_state.resources[3], sizeof(kept)) && !memcmp(&grant, &g_state.lease_grants[3], sizeof(grant)));
    assert(g_state.resources[0].owner_node == -1 && !g_state.resources[0].epoch && !g_state.lease_grants[0].promised_epoch);
    assert(g_state.resources[1].owner_node == -1 && !g_state.resources[1].epoch && !g_state.lease_grants[1].promised_epoch);
    assert(g_state.resources[2].epoch == 11 && g_state.lease_grants[2].promised_epoch == 51);
    assert(g_state.config_application_drain_mask == 7 && g_state.config_application_remapping);
    until(LCS_CONFIG_RECONCILE_ADMIT);
    live_synchronized(); /* Even apparently fresh cached flags must be discarded first. */
    assert(!tick() && g_state.resource_generation_admitted && !g_state.config_application_remapping);
    assert(!g_state.peers[1].initial_sync_complete && !g_state.voting_ready);
    assert(g_state.config_application_fenced && coordinator.application_owner == &reconcile && resource_config_is_fenced(3));
    for (unsigned fault = 0; fault < 12; fault++)
    {
        live_synchronized(); peer_runtime_t *peer = &g_state.peers[1];
        switch (fault)
        {
            case 0: peer->fd = -1; break;
            case 1: peer->conn_state = LCS_PEER_HELLO_SENT; break;
            case 2: peer->instance_id = 0; break;
            case 3: peer->online = false; break;
            case 4: peer->protocol_version = 5; break;
            case 5: peer->last_seen_ms = lcs_now_ms() - g_state.cfg.peer_timeout_ms - 1; break;
            case 6: peer->initial_sync_complete = false; break;
            case 7: peer->resource_generation_known = false; break;
            case 8: peer->resource_generation_matches = false; break;
            case 9: peer->resource_generation.revision++; break;
            case 10: peer->resource_generation.digest[0] ^= 1; break;
            case 11: peer->voting_ready = false; break;
        }
        assert(!tick() && reconcile.phase == LCS_CONFIG_RECONCILE_ADMIT);
        assert(g_state.config_application_drain_mask == UINT32_MAX && coordinator.application_owner == &reconcile);
        assert(g_state.lease_grants[3].promised_epoch == grant.promised_epoch);
    }
    live_synchronized(); g_state.voting_not_before_ms = lcs_now_ms() + 1000;
    assert(!tick()); g_state.voting_not_before_ms = 0;
    until(LCS_CONFIG_RECONCILE_DONE);
    assert(!g_state.config_application_fenced && !g_state.config_application_drain_mask);
    live_mode = false; memset(&g_state, 0, sizeof(g_state));
    free(configs); lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_valid_lease_retention_during_remap(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.self_index = 0; g_state.instance_id = 100; g_state.cfg.resource_count = 1;
    g_state.config_application_fenced = g_state.config_application_remapping = true;
    g_state.resources[0].owner_node = 0; g_state.resources[0].owner_instance_id = 100;
    g_state.resources[0].state = LCS_RES_ACTIVE; g_state.resources[0].epoch = 7;
    g_state.resources[0].lease_deadline_ms = lcs_now_ms() + 10000;
    g_state.lease_grants[0].promised_epoch = 9; dropped = 0;
    resource_runtime_t before = g_state.resources[0]; lease_grant_t grant = g_state.lease_grants[0];
    assert(lcs_config_live_maintain_leases(-1) && !dropped);
    assert(!memcmp(&before, &g_state.resources[0], sizeof(before)) && !memcmp(&grant, &g_state.lease_grants[0], sizeof(grant)));
    g_state.resources[0].lease_deadline_ms = lcs_now_ms() - 1;
    assert(lcs_config_live_maintain_leases(-1) && dropped == 1);
    g_state.resources[0].state = LCS_RES_STOP_FAILED;
    assert(lcs_config_live_maintain_leases(-1) && dropped == 1);
    g_state.config_application_remapping = false; g_state.voting_ready = true; g_state.votes_seen = 2;
    assert(!lcs_config_live_maintain_leases(-1));
    memset(&g_state, 0, sizeof(g_state));
}


/* The projection directory is deliberately separate from the authoritative state. */
static void projection_open(lcs_config_bootstrap_t *bootstrap)
{
    memset(bootstrap, 0, sizeof(*bootstrap));
    bootstrap->state = store; /* Borrowed; cleanup() owns these descriptors. */
    bootstrap->initialization_locked = true;
    assert(!mkdirat(store.directory_fd, "snapshots", 0700));
    char path[128]; snprintf(path, sizeof(path), "%s/snapshots", directory);
    assert(!lcs_config_store_open(&bootstrap->snapshots, path, error, sizeof(error)));
    /* Production snapshot directories borrow the state lock. */
    assert(!unlinkat(bootstrap->snapshots.directory_fd, "lock", 0));
    close(bootstrap->snapshots.lock_fd); bootstrap->snapshots.lock_fd = -1;
    strcpy(bootstrap->snapshot_name, "resources.json");
}

static void projection_close(lcs_config_bootstrap_t *bootstrap)
{
    assert(coordinator.storage.pid <= 0);
    const char *names[] = {"resources.json", ".resources.json.tmp"};
    for (size_t i = 0; i < 2; i++)
        assert(!unlinkat(bootstrap->snapshots.directory_fd, names[i], 0) || errno == ENOENT);
    lcs_config_store_close(&bootstrap->snapshots);
    assert(!unlinkat(store.directory_fd, "snapshots", AT_REMOVEDIR));
}

static void projection_matches(lcs_config_bootstrap_t *bootstrap, const lcs_config_value_t *value)
{
    char *data = NULL; size_t length = 0;
    assert(!lcs_config_store_read(&bootstrap->snapshots, bootstrap->snapshot_name,
        &data, &length, value->length + 1, error, sizeof(error)));
    assert(length == value->length && !memcmp(data, value->document, length));
    free(data);
    struct stat st;
    assert(!fstatat(bootstrap->snapshots.directory_fd, bootstrap->snapshot_name, &st, AT_SYMLINK_NOFOLLOW));
    assert(S_ISREG(st.st_mode) && (st.st_mode & 0777) == 0600);
}

static void projection_wait(void)
{
    for (unsigned i = 0; i < 5000 && coordinator.storage.pid > 0; i++)
    { (void)lcs_config_coordinator_process(&coordinator, 1000); usleep(1000); }
    assert(coordinator.storage.pid <= 0);
}

static void projection_stopped(pid_t writer)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int status;
        if (waitpid(writer, &status, WNOHANG | WUNTRACED) > 0)
        { assert(WIFSTOPPED(status)); return; }
        usleep(1000);
    }
    assert(!"projection writer did not reach fsync");
}

static void test_durable_projection(void)
{
    setup(false);
    lcs_config_bootstrap_t bootstrap; projection_open(&bootstrap);
    lcs_config_projection_t publisher;
    lcs_config_projection_init(&publisher, &coordinator, &bootstrap);
    assert(lcs_config_projection_step(&publisher, &runtime.agreement.committed, error, sizeof(error)) < 0);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    bootstrap.initialization_locked = false;
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) < 0);
    bootstrap.initialization_locked = true;
    bootstrap.worker_pid = 123;
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) < 0);
    bootstrap.worker_pid = 0;
    forbid_io = true; stop_sync = 1;
    assert(!lcs_config_projection_step(&publisher, &first, error, sizeof(error)));
    stop_sync = 0;
    pid_t writer = coordinator.storage.pid; assert(writer > 0);
    projection_stopped(writer);
    assert(!lcs_config_projection_step(&publisher, &first, error, sizeof(error)));
    assert(lcs_config_coordinator_start_application(&coordinator, NULL, NULL, error, sizeof(error)) == 1);
    struct stat st;
    assert(fstatat(bootstrap.snapshots.directory_fd, "resources.json", &st, 0) < 0 && errno == ENOENT);
    assert(!coordinator.completed_projections && !coordinator.completed_applications && !coordinator.completed_votes);
    assert(!kill(writer, SIGCONT)); projection_wait();
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) == 1);
    projection_matches(&bootstrap, &first);
    assert(coordinator.completed_projections == 1 && coordinator.projected_revision == 1);
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) == 1 && coordinator.storage.pid <= 0);
    assert(runtime.application.applied.revision == 0 && !runtime.application.pending.document);

    /* Complete a publication, then learn a newer revision before observing it. */
    coordinator.projected_revision = 0;
    assert(!lcs_config_projection_step(&publisher, &first, error, sizeof(error)));
    projection_wait();
    forbid_io = false;
    lcs_config_value_t second = successor(&first, true); learn(&second);
    forbid_io = true;
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) < 0);
    assert(!lcs_config_projection_step(&publisher, &second, error, sizeof(error)));
    assert(coordinator.storage.pid > 0 && coordinator.projected_revision == 1);
    projection_wait();
    assert(lcs_config_projection_step(&publisher, &second, error, sizeof(error)) == 1);
    projection_matches(&bootstrap, &second);

    /* Retain cleanup obligations even if publication fails after rename. */
    forbid_io = false;
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    forbid_io = true;
    /* A directory fsync error after rename is retryable for this derived file. */
    coordinator.projected_revision = 0; fail_sync = 2;
    assert(!lcs_config_projection_step(&publisher, &second, error, sizeof(error)));
    fail_sync = 0; projection_wait();
    assert(lcs_config_projection_step(&publisher, &second, error, sizeof(error)) < 0);
    assert(!coordinator.failed && !runtime.agreement.poisoned && !runtime.application.poisoned);
    assert(!coordinator.projected_revision && !coordinator.completed_applications && !coordinator.completed_votes);
    assert(lcs_config_value_equal(&runtime.agreement.committed, &second));
    assert(lcs_config_value_equal(&runtime.application.pending, &second) && runtime.application.applied.revision == 0);
    assert(!lcs_config_projection_step(&publisher, &second, error, sizeof(error)));
    projection_wait();
    assert(lcs_config_projection_step(&publisher, &second, error, sizeof(error)) == 1);
    projection_matches(&bootstrap, &second);

    /* Unsafe destination rejection must not touch the authoritative journal. */
    assert(!unlinkat(bootstrap.snapshots.directory_fd, "resources.json", 0));
    assert(!symlinkat("../agreement.json", bootstrap.snapshots.directory_fd, "resources.json"));
    coordinator.projected_revision = 0;
    assert(!lcs_config_projection_step(&publisher, &second, error, sizeof(error)));
    projection_wait();
    assert(lcs_config_projection_step(&publisher, &second, error, sizeof(error)) < 0 && !coordinator.failed);
    assert(!unlinkat(bootstrap.snapshots.directory_fd, "resources.json", 0));
    assert(!lcs_config_projection_step(&publisher, &second, error, sizeof(error)));
    projection_wait();
    assert(lcs_config_projection_step(&publisher, &second, error, sizeof(error)) == 1);
    projection_matches(&bootstrap, &second);
    projection_close(&bootstrap);
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_projection_cancellation(void)
{
    setup(false);
    lcs_config_bootstrap_t bootstrap; projection_open(&bootstrap);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    lcs_config_projection_t publisher;
    lcs_config_projection_init(&publisher, &coordinator, &bootstrap);
    forbid_io = true; stop_sync = 1;
    assert(!lcs_config_projection_step(&publisher, &first, error, sizeof(error)));
    stop_sync = 0;
    pid_t writer = coordinator.storage.pid; projection_stopped(writer);
    lcs_config_coordinator_stop(&coordinator);
    assert(coordinator.storage.pid == writer && !lcs_config_coordinator_can_close(&coordinator));
    assert(lcs_config_projection_step(&publisher, &first, error, sizeof(error)) < 0);
    projection_wait();
    assert(!coordinator.completed_projections && runtime.agreement.poisoned);
    projection_close(&bootstrap);
    lcs_config_value_clear(&first); cleanup();
}

static void test_live_managed_projection(bool quorum_only)
{
    setup(quorum_only);
    lcs_config_bootstrap_t bootstrap; projection_open(&bootstrap);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    memset(&g_state, 0, sizeof(g_state)); g_state.cfg = *runtime.base;
    g_state.self_index = 0; g_state.instance_id = 100;
    g_state.self_index = quorum_only ? 2 : 0;
    g_state.resources[0].owner_node = -1;
    generation_cancellations = 0; generation_reload = false; released_count = 0;
    owners_ready = true; invalid_owner_result = false;
    lcs_config_live_t live;
    assert(!lcs_config_live_init_with_projection(&live, -1, &coordinator, &bootstrap, owner_live, NULL));
    lcs_config_reconcile_init(&reconcile, &coordinator, lcs_config_live_ops(), &live);
    live_mode = true; forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile));
    until(LCS_CONFIG_RECONCILE_ADMIT);
    assert(coordinator.completed_projections == 1 && coordinator.completed_applications == 2);
    projection_matches(&bootstrap, &first);
    assert(!tick()); live_synchronized();
    until(LCS_CONFIG_RECONCILE_DONE);
    live_mode = false; memset(&g_state, 0, sizeof(g_state));
    projection_close(&bootstrap);
    lcs_config_value_clear(&first); cleanup();
}


static void owner_message_roundtrip(const lcs_config_owner_message_t *message)
{
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE + 1]; size_t length = 0;
    lcs_config_owner_message_t decoded = {0};
    assert(!lcs_config_owner_encode(message, payload, sizeof(payload), &length));
    assert(length == LCS_CONFIG_OWNER_MESSAGE_SIZE);
    assert(!lcs_config_owner_decode(payload, length, &decoded));
    assert(decoded.kind == message->kind && decoded.result == message->result && decoded.id == message->id);
    assert(decoded.sender_instance == message->sender_instance && decoded.target_instance == message->target_instance);
    assert(lcs_config_owner_scope_equal(&decoded.scope, &message->scope));
    for (size_t i = 0; i < length; i++)
        assert(lcs_config_owner_decode(payload, i, &decoded) < 0);
    assert(lcs_config_owner_decode(payload, length + 1, &decoded) < 0);
    assert(lcs_config_owner_encode(message, payload, length - 1, &length) < 0);
    assert(lcs_config_owner_decode(NULL, LCS_CONFIG_OWNER_MESSAGE_SIZE, &decoded) < 0);
}

static void test_owner_barrier_proofs(void)
{
    setup(false);
    lcs_config_owner_scope_t scope;
    assert(lcs_config_owner_scope(&runtime.application, &scope, error, sizeof(error)) < 0);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    lcs_config_value_t second = successor(&first, true); learn(&second);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    forbid_io = true;
    assert(!lcs_config_owner_scope(&runtime.application, &scope, error, sizeof(error)));
    assert(scope.old_revision == 1 && scope.target_revision == 2 && scope.drain == 1);
    assert(!strcmp(scope.old_value, first.digest) && !strcmp(scope.target_value, second.digest));
    runtime.application.poisoned = true;
    assert(lcs_config_owner_scope(&runtime.application, &scope, error, sizeof(error)) < 0);
    runtime.application.poisoned = false;

    uint64_t instances[LCS_MAX_NODES] = {100, 101, 102};
    lcs_config_owner_round_t rounds[2] = {0};
    lcs_config_owner_receipt_t receipts[2] = {0};
    for (size_t i = 0; i < 2; i++)
    {
        assert(!lcs_config_owner_begin(&rounds[i], runtime.base, &scope, i, instances, 1, 1000));
        assert(rounds[i].required == 3 && !rounds[i].stopped);
        assert(lcs_config_owner_begin(&rounds[i], runtime.base, &scope, i, instances, 2, 1001) < 0);
    }
    assert(!lcs_config_owner_local(&rounds[0], &receipts[0]) && !rounds[0].stopped);
    lcs_config_owner_message_t query, reply;
    assert(lcs_config_owner_query(&rounds[0], 2, 102, 1001, &query) == 1); /* Quorum only. */
    assert(!lcs_config_owner_query(&rounds[0], 1, 101, 1001, &query));
    owner_message_roundtrip(&query);
    assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
    assert(reply.result == LCS_CONFIG_OWNER_PENDING && !receipts[1].sealed);
    assert(!lcs_config_owner_reply(&rounds[0], 1, 101, 1002, &reply));
    assert(!rounds[0].stopped && !lcs_config_owner_poll(&rounds[0], 1003));
    assert(!lcs_config_owner_receipt_seal(&receipts[1], &scope, 101));
    assert(!lcs_config_owner_local(&rounds[1], &receipts[1]));
    assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
    assert(reply.result == LCS_CONFIG_OWNER_STOPPED);
    owner_message_roundtrip(&reply);
    lcs_config_owner_message_t stale = reply;
    stale.id++;
    assert(lcs_config_owner_reply(&rounds[0], 1, 101, 1004, &stale) == 1);
    stale = reply; stale.target_instance++;
    assert(lcs_config_owner_reply(&rounds[0], 1, 101, 1004, &stale) == 1);
    assert(lcs_config_owner_reply(&rounds[0], 2, 101, 1004, &reply) == 1);
    assert(lcs_config_owner_reply(&rounds[0], 1, 999, 1004, &reply) == 1);
    assert(!lcs_config_owner_reply(&rounds[0], 1, 101, 1004, &reply));
    assert(!lcs_config_owner_reply(&rounds[0], 1, 101, 1004, &reply)); /* Idempotent. */
    assert(rounds[0].stopped == 2 && !lcs_config_owner_poll(&rounds[0], 1005));
    assert(!lcs_config_owner_receipt_seal(&receipts[0], &scope, 100));
    assert(!lcs_config_owner_local(&rounds[0], &receipts[0]));
    assert(lcs_config_owner_poll(&rounds[0], 1006) == 1);
    assert(lcs_config_owner_poll(&rounds[0], 99999) == 1); /* Completion, not expiry proof. */
    assert(!lcs_config_owner_query(&rounds[1], 0, 100, 1006, &query));
    assert(!lcs_config_owner_answer(&receipts[0], 100, 101, &query, &reply));
    assert(!lcs_config_owner_reply(&rounds[1], 0, 100, 1007, &reply));
    assert(lcs_config_owner_poll(&rounds[1], 1008) == 1);
    assert(lcs_config_owner_observe(&rounds[0], 1, 0) < 0); /* Even a sealed round is invalidated. */
    assert(lcs_config_owner_poll(&rounds[0], 1009) < 0);
    assert(lcs_config_owner_begin(&rounds[0], runtime.base, &scope, 0, instances, 1, 1010) < 0);
    assert(!lcs_config_owner_begin(&rounds[0], runtime.base, &scope, 0, instances, 2, 1010));
    assert(!lcs_config_owner_query(&rounds[0], 1, 101, 1011, &query));
    assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
    assert(lcs_config_owner_reply(&rounds[0], 1, 102, 1012, &reply) == 1);
    assert(lcs_config_owner_observe(&rounds[0], 1, 102) < 0); /* Restart cannot inherit proof. */

    /* Contradictory current evidence blocks; neither a timeout nor majority repairs it. */
    for (unsigned mode = 0; mode < 4; mode++)
    {
        assert(!lcs_config_owner_begin(&rounds[0], runtime.base, &scope, 0, instances, 3 + mode, 2000));
        assert(!lcs_config_owner_local(&rounds[0], &receipts[0]));
        assert(!lcs_config_owner_query(&rounds[0], 1, 101, 2001, &query));
        assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
        if (!mode) reply.scope.target_value[0] = reply.scope.target_value[0] == 'a' ? 'b' : 'a';
        if (mode == 1) reply.scope.drain ^= 1;
        if (mode == 2) reply.result = LCS_CONFIG_OWNER_FAILED;
        if (mode == 3)
        {
            assert(!lcs_config_owner_reply(&rounds[0], 1, 101, 2002, &reply));
            assert(lcs_config_owner_poll(&rounds[0], 2003) == 1);
            reply.result = LCS_CONFIG_OWNER_PENDING;
        }
        assert(lcs_config_owner_reply(&rounds[0], 1, 101, 2004, &reply) < 0);
        assert(lcs_config_owner_poll(&rounds[0], 2005) < 0);
    }
    lcs_config_owner_scope_t different = scope; different.drain ^= 1;
    assert(lcs_config_owner_receipt_seal(&receipts[0], &different, 100) < 0);
    assert(lcs_config_owner_receipt_seal(&receipts[0], &scope, 999) < 0);
    query.scope = different;
    assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
    assert(reply.result == LCS_CONFIG_OWNER_FAILED);
    query.scope = scope;
    assert(lcs_config_owner_answer(&receipts[1], 102, 100, &query, &reply) < 0);
    lcs_config_owner_receipt_revoke(&receipts[1]);
    assert(lcs_config_owner_receipt_seal(&receipts[1], &scope, 101) < 0);
    assert(!lcs_config_owner_answer(&receipts[1], 101, 100, &query, &reply));
    assert(reply.result == LCS_CONFIG_OWNER_FAILED);
    owner_message_roundtrip(&reply);

    /* Strict codecs reject malformed fields/padding rather than normalizing them. */
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE + 1], original[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&query, original, sizeof(original), &length));
    const size_t bad_offsets[] = {0, 2, 3, 48, 48 + 64, 48 + 65, 48 + 130, 242};
    for (size_t i = 0; i < sizeof(bad_offsets) / sizeof(bad_offsets[0]); i++)
    {
        memcpy(payload, original, length); payload[bad_offsets[i]] = 255;
        assert(lcs_config_owner_decode(payload, length, &reply) < 0);
    }
    memcpy(payload, original, length); memset(payload + 4, 0, 8);
    assert(lcs_config_owner_decode(payload, length, &reply) < 0);
    memcpy(payload, original, length); payload[49] = 0;
    assert(lcs_config_owner_decode(payload, length, &reply) < 0);

    lcs_config_t *three_full = malloc(sizeof(*three_full)); assert(three_full); *three_full = *runtime.base;
    three_full->nodes[2].role = LCS_NODE_FULL;
    lcs_config_owner_scope_t three_scope = scope;
    assert(!lcs_config_agreement_membership(three_full, three_scope.membership));
    lcs_config_owner_round_t round = {0};
    assert(lcs_config_owner_begin(&round, three_full, &scope, 0, instances, 1, 3000) < 0);
    instances[2] = 0;
    assert(!lcs_config_owner_begin(&round, three_full, &three_scope, 0, instances, 1, 3000));
    assert(round.required == 7);
    lcs_config_owner_receipt_t a = {0}, b = {0};
    assert(!lcs_config_owner_receipt_seal(&a, &three_scope, 100));
    assert(!lcs_config_owner_receipt_seal(&b, &three_scope, 101));
    assert(!lcs_config_owner_local(&round, &a));
    assert(!lcs_config_owner_query(&round, 1, 101, 3001, &query));
    assert(!lcs_config_owner_answer(&b, 101, 100, &query, &reply));
    assert(!lcs_config_owner_reply(&round, 1, 101, 3002, &reply));
    assert(round.stopped == 3 && !lcs_config_owner_poll(&round, 3003)); /* Majority is insufficient. */
    assert(lcs_config_owner_query(&round, 2, 0, 3004, &query) == 1);
    assert(!lcs_config_owner_poll(&round, 12999));
    assert(lcs_config_owner_query(&round, 1, 101, 13000, &query) < 0);
    assert(lcs_config_owner_poll(&round, 13000) < 0);
    assert(!lcs_config_owner_begin(&round, three_full, &three_scope, 0, instances, 2, 13001));
    assert(!round.stopped && round.deadline_ms == 23001);
    assert(!lcs_config_owner_query(&round, 2, 102, 13002, &query));
    assert(round.expected_instance[2] == 102);
    assert(lcs_config_owner_query(&round, 2, 103, 13003, &query) < 0);
    assert(lcs_config_owner_begin(&round, three_full, &three_scope, 0, instances, 3, UINT64_MAX) < 0);
    free(three_full);
    round = (lcs_config_owner_round_t){0};
    instances[2] = 102;
    assert(lcs_config_owner_begin(&round, runtime.base, &scope, 2, instances, 1, 0) < 0);
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}


typedef struct
{
    lcs_config_owner_network_t nodes[3];
    struct { size_t source, target, length; unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; } frames[128];
    size_t head, tail, source;
    bool backpressure;
    uint32_t drop_applied_replies;
} owner_bus_t;

static int owner_send(void *context, size_t node, const void *payload, size_t length)
{
    owner_bus_t *bus = context;
    if (bus->backpressure)
        return 1;
    if (((const unsigned char *)payload)[2] == LCS_CONFIG_OWNER_APPLIED_REPLY &&
        (bus->drop_applied_replies & (UINT32_C(1) << node)))
        return 0; /* Simulate a copied response lost before delivery. */
    assert(node != bus->source && node < 3 && bus->tail < 128 && length == LCS_CONFIG_OWNER_MESSAGE_SIZE);
    bus->frames[bus->tail].source = bus->source; bus->frames[bus->tail].target = node;
    bus->frames[bus->tail].length = length; memcpy(bus->frames[bus->tail++].payload, payload, length);
    return 0;
}

static void owner_exchange(owner_bus_t *bus, const uint64_t instances[LCS_MAX_NODES], uint64_t now)
{
    for (unsigned pass = 0; pass < 4; pass++)
    {
        for (size_t i = 0; i < 3; i++)
        {
            bus->source = i;
            lcs_config_owner_network_process(&bus->nodes[i], instances, now, owner_send, bus);
        }
        while (bus->head < bus->tail)
        {
            size_t frame = bus->head++;
            size_t from = bus->frames[frame].source, to = bus->frames[frame].target;
            assert(!lcs_config_owner_network_receive(&bus->nodes[to], from, instances[from],
                bus->frames[frame].payload, bus->frames[frame].length, now));
        }
        bus->head = bus->tail = 0;
    }
}

static void test_owner_network_exchange(void)
{
    all_full_fixture = true; setup(false); all_full_fixture = false;
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    lcs_config_owner_scope_t scope;
    assert(!lcs_config_owner_scope(&runtime.application, &scope, error, sizeof(error)));
    owner_bus_t bus = {0}; uint64_t instances[LCS_MAX_NODES] = {100, 101, 102};
    for (size_t i = 0; i < 3; i++)
        assert(!lcs_config_owner_network_init(&bus.nodes[i], runtime.base, &runtime.application, i, instances[i]));
    owner_exchange(&bus, instances, 900);
    assert(!lcs_config_owner_network_start(&bus.nodes[0], &scope, 1000));
    bus.backpressure = true; owner_exchange(&bus, instances, 1001);
    assert(bus.nodes[0].round.stopped == 1 && bus.nodes[0].round.deadline_ms == 11000);
    bus.backpressure = false; owner_exchange(&bus, instances, 1300);
    assert(bus.nodes[0].round.stopped == 1 && !bus.nodes[1].receipt.sealed && !bus.nodes[2].receipt.sealed);
    assert(!lcs_config_owner_network_start(&bus.nodes[1], &scope, 9000));
    owner_exchange(&bus, instances, 9500);
    assert(bus.nodes[0].round.stopped == 3 && !lcs_config_owner_poll(&bus.nodes[0].round, 9501));
    assert(!lcs_config_owner_network_start(&bus.nodes[2], &scope, 10000));
    owner_exchange(&bus, instances, 10050);
    for (size_t i = 0; i < 3; i++)
        assert(lcs_config_owner_poll(&bus.nodes[i].round, 10051) == 1);

    /* A session change invalidates a completed proof and clears buffered replies. */
    lcs_config_owner_network_disconnect(&bus.nodes[0], 1);
    assert(bus.nodes[0].round.failed && bus.nodes[0].receipt.sealed && !bus.nodes[0].receipt.failed);
    assert(!lcs_config_owner_network_retry(&bus.nodes[0], 11000));
    owner_exchange(&bus, instances, 11001);
    assert(lcs_config_owner_poll(&bus.nodes[0].round, 11002) == 1 && bus.nodes[0].serial == 2);
    instances[1] = 201; bus.source = 0;
    lcs_config_owner_network_process(&bus.nodes[0], instances, 11003, owner_send, &bus);
    assert(bus.nodes[0].round.failed);
    instances[1] = 101;
    lcs_config_owner_network_process(&bus.nodes[0], instances, 11004, owner_send, &bus);

    /* Revocation rewrites unsent STOPPED replies under output backpressure. */
    assert(!lcs_config_owner_network_retry(&bus.nodes[0], 12000));
    lcs_config_owner_message_t query;
    assert(!lcs_config_owner_query(&bus.nodes[0].round, 1, 101, 12001, &query));
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[1], 0, 100, payload, length, 12001));
    assert(bus.nodes[1].pending_replies == 1 && bus.nodes[1].replies[0].result == LCS_CONFIG_OWNER_STOPPED);
    lcs_config_owner_network_revoke(&bus.nodes[1]);
    assert(bus.nodes[1].replies[0].result == LCS_CONFIG_OWNER_FAILED);
    owner_exchange(&bus, instances, 12002);
    assert(bus.nodes[0].round.failed && lcs_config_owner_network_retry(&bus.nodes[1], 12003) < 0);

    /* An unavailable full member stays required; query retries cannot extend expiry. */
    /* Force a fresh explicit round after session loss, with node0 unavailable. */
    lcs_config_owner_network_disconnect(&bus.nodes[2], 0);
    assert(!lcs_config_owner_network_retry(&bus.nodes[2], 13001));
    instances[0] = 0;
    bus.source = 2;
    lcs_config_owner_network_process(&bus.nodes[2], instances, 23001, owner_send, &bus);
    assert(bus.nodes[2].round.failed && bus.nodes[2].round.required == 7);
    bus.head = bus.tail = 0;
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    lcs_config_value_t second = successor(&first, true); learn(&second);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    lcs_config_owner_scope_t newer;
    assert(!lcs_config_owner_scope(&runtime.application, &newer, error, sizeof(error)));
    assert(lcs_config_owner_network_start(&bus.nodes[0], &newer, 24000) < 0);
    assert(lcs_config_owner_scope_equal(&bus.nodes[0].receipt.scope, &scope));
    lcs_config_value_clear(&second);
    for (size_t i = 0; i < 3; i++)
    {
        lcs_config_owner_network_stop(&bus.nodes[i]);
        assert(bus.nodes[i].closing && bus.nodes[i].receipt.failed && !bus.nodes[i].pending_replies);
    }
    lcs_config_value_clear(&first); cleanup();
}

static void test_live_projection_at_install(bool quorum_only)
{
    setup(quorum_only);
    lcs_config_bootstrap_t bootstrap; projection_open(&bootstrap);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    lcs_config_value_t second = successor(&first, true);
    memset(&g_state, 0, sizeof(g_state)); g_state.cfg = *runtime.base;
    g_state.self_index = quorum_only ? 2 : 0; g_state.instance_id = 100; g_state.resources[0].owner_node = -1;
    generation_cancellations = released_count = 0; generation_reload = false;
    owners_ready = true; invalid_owner_result = false;
    lcs_config_live_t live;
    assert(!lcs_config_live_init_with_projection(&live, -1, &coordinator, &bootstrap, owner_live, NULL));
    lcs_config_reconcile_init(&reconcile, &coordinator, lcs_config_live_ops(), &live);
    live_mode = true; forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile)); until(LCS_CONFIG_RECONCILE_INSTALL);
    /* Seed the old projection even on a quorum-only member, whose INSTALL is
     * its first projection callback. Then learn a successor while still pending 1. */
    (void)lcs_config_projection_step(&live.projection, &first, error, sizeof(error));
    projection_wait();
    assert(lcs_config_projection_step(&live.projection, &first, error, sizeof(error)) == 1);
    lcs_config_agreement_job_t checkpoint = {.action = LCS_CONFIG_CHECKPOINT, .ballot = 64, .voters = 3, .value = &second};
    strcpy(checkpoint.parent_digest, first.digest);
    assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &checkpoint, error, sizeof(error)));
    projection_wait();
    assert(runtime.agreement.committed.revision == 2 && runtime.application.pending.revision == 1);
    assert(!unlinkat(bootstrap.snapshots.directory_fd, "resources.json", 0));
    assert(!symlinkat("../agreement.json", bootstrap.snapshots.directory_fd, "resources.json"));
    assert(!tick() && !live.installed && coordinator.storage.pid > 0);
    projection_wait();
    assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED && !live.installed);
    assert(!generation_cancellations && runtime.application.applied.revision == 0 && !coordinator.failed);
    assert(!unlinkat(bootstrap.snapshots.directory_fd, "resources.json", 0));
    assert(!lcs_config_reconcile_retry(&reconcile)); until(LCS_CONFIG_RECONCILE_FINISH);
    projection_matches(&bootstrap, &second);
    assert(live.installed && generation_cancellations == 3 && runtime.application.pending.revision == 1);
    /* An exact install retry rechecks publication but does not close/remap twice. */
    lcs_config_reconcile_view_t view = {.old_config = &reconcile.configs[0], .target = &reconcile.configs[1],
        .plan = &reconcile.plan, .value = &reconcile.value, .attempt = reconcile.attempt};
    assert(lcs_config_live_ops()->install(&live, &view, error, sizeof(error)) == 1 && generation_cancellations == 3);
    until(LCS_CONFIG_RECONCILE_ADMIT); until(LCS_CONFIG_RECONCILE_BEGIN);
    until(LCS_CONFIG_RECONCILE_ADMIT); assert(!tick()); live_synchronized(); until(LCS_CONFIG_RECONCILE_DONE);
    assert(runtime.application.applied.revision == 2 && generation_cancellations == 6);
    live_mode = false; memset(&g_state, 0, sizeof(g_state));
    projection_close(&bootstrap); lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_live_admission_cancellation(unsigned fault)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    memset(&g_state, 0, sizeof(g_state)); g_state.cfg = *runtime.base;
    g_state.self_index = 0; g_state.instance_id = 100; g_state.resources[0].owner_node = -1;
    generation_cancellations = released_count = 0; generation_reload = false;
    project_ready = owners_ready = true; invalid_owner_result = false;
    lcs_config_live_t live;
    assert(!lcs_config_live_init(&live, -1, project_live, owner_live, NULL));
    lcs_config_reconcile_init(&reconcile, &coordinator, lcs_config_live_ops(), &live);
    live_mode = true; forbid_io = true;
    assert(!lcs_config_reconcile_start(&reconcile)); until(LCS_CONFIG_RECONCILE_ADMIT);
    assert(!tick() && live.synchronizing && g_state.resource_generation_admitted);
    live_synchronized();
    lcs_config_value_t second = {0};
    if (!fault)
    {
        second = successor(&first, true);
        lcs_config_agreement_job_t checkpoint = {.action = LCS_CONFIG_CHECKPOINT, .ballot = 64, .voters = 3, .value = &second};
        strcpy(checkpoint.parent_digest, first.digest);
        assert(!lcs_config_coordinator_start_checkpoint(&coordinator, &checkpoint, error, sizeof(error)));
        until(LCS_CONFIG_RECONCILE_BEGIN);
    }
    else if (fault == 1) lcs_config_reconcile_stop(&reconcile);
    else if (fault == 2)
    {
        coordinator.failed = true;
        assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_RECOVERY);
    }
    else
    {
        g_state.resource_generation.digest[0] ^= 1;
        assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED);
    }
    assert(!live.synchronizing && !g_state.resource_generation_admitted && !g_state.voting_ready && !g_state.votes_seen);
    assert(g_state.config_application_fenced && g_state.config_application_remapping &&
        g_state.config_application_drain_mask == UINT32_MAX && !g_state.peers[1].initial_sync_complete);
    if (!fault || fault == 3)
    {
        if (fault == 3)
        { g_state.resource_generation.digest[0] ^= 1; assert(!lcs_config_reconcile_retry(&reconcile)); }
        until(LCS_CONFIG_RECONCILE_ADMIT);
        assert(!tick() && live.synchronizing && !g_state.voting_ready);
        live_synchronized(); until(LCS_CONFIG_RECONCILE_DONE);
        assert(!live.synchronizing && !g_state.config_application_fenced && !coordinator.application_owner);
        assert(runtime.application.applied.revision == (fault ? 1 : 2));
    }
    live_mode = false; memset(&g_state, 0, sizeof(g_state));
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
}

static void test_live_owner_network_recheck(void)
{
    for (unsigned fault = 0; fault < 2; fault++)
    {
        setup(false);
        lcs_config_bootstrap_t bootstrap; projection_open(&bootstrap);
        lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
        assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
        assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
        lcs_config_value_t second = successor(&first, true); learn(&second);
        memset(&g_state, 0, sizeof(g_state));
        uint64_t revision;
        assert(!lcs_resource_config_decode(first.document, first.length, runtime.base, &g_state.cfg, &revision, error, sizeof(error)));
        g_state.self_index = 0; g_state.instance_id = 100; g_state.resources[0].owner_node = -1;
        generation_cancellations = 0; generation_reload = false; released_count = 0;
        owner_bus_t bus = {0}; uint64_t instances[LCS_MAX_NODES] = {100, 101, 0};
        for (size_t i = 0; i < 2; i++)
            assert(!lcs_config_owner_network_init(&bus.nodes[i], runtime.base, &runtime.application, i, instances[i]));
        /* Third participant is quorum-only and never sends owner proof. */
        assert(!lcs_config_owner_network_init(&bus.nodes[2], runtime.base, NULL, 2, 102));
        lcs_config_live_t live;
        assert(!lcs_config_live_init_with_owner_network(&live, -1, &coordinator, &bootstrap, &bus.nodes[0]));
        lcs_config_reconcile_init(&reconcile, &coordinator, lcs_config_live_ops(), &live);
        live_mode = true; forbid_io = true;
        assert(!lcs_config_reconcile_start(&reconcile));
        until(LCS_CONFIG_RECONCILE_OWNERS);
        assert(!tick() && bus.nodes[0].receipt.sealed && !live.installed);
        lcs_config_owner_scope_t scope = bus.nodes[0].receipt.scope;
        assert(scope.drain == 1);
        assert(!lcs_config_owner_network_start(&bus.nodes[1], &scope, lcs_now_ms()));
        owner_exchange(&bus, instances, lcs_now_ms() + 250);
        until(LCS_CONFIG_RECONCILE_INSTALL);
        if (!fault)
            lcs_config_owner_network_disconnect(&bus.nodes[0], 1);
        else
            g_state.resources[0].state = LCS_RES_STOP_FAILED;
        assert(tick() < 0 && reconcile.phase == LCS_CONFIG_RECONCILE_BLOCKED && !live.installed);
        assert(!generation_cancellations);
        if (!fault)
        {
            assert(!bus.nodes[0].receipt.failed);
            assert(!lcs_config_owner_network_retry(&bus.nodes[0], lcs_now_ms()));
            owner_exchange(&bus, instances, lcs_now_ms() + 250);
            assert(!lcs_config_reconcile_retry(&reconcile));
            until(LCS_CONFIG_RECONCILE_ADMIT);
            assert(!tick()); live_synchronized();
            bus.nodes[0].instances[1] = 999;
            assert(!tick() && reconcile.phase == LCS_CONFIG_RECONCILE_ADMIT && g_state.config_application_fenced);
            bus.nodes[0].instances[1] = 101;
            until(LCS_CONFIG_RECONCILE_DONE);
            assert(live.installed && generation_cancellations == 3);
        }
        else
        {
            assert(bus.nodes[0].receipt.failed && lcs_config_owner_network_retry(&bus.nodes[0], lcs_now_ms()) < 0);
        }
        for (size_t i = 0; i < 3; i++) lcs_config_owner_network_stop(&bus.nodes[i]);
        live_mode = false; memset(&g_state, 0, sizeof(g_state));
        projection_close(&bootstrap);
        lcs_config_value_clear(&first); lcs_config_value_clear(&second); cleanup();
    }
}


static void copy_owner_catalog(lcs_config_application_t *copy)
{
    *copy = runtime.application;
    memset(&copy->applied, 0, sizeof(copy->applied)); memset(&copy->pending, 0, sizeof(copy->pending));
    memset(copy->history, 0, sizeof(copy->history));
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++)
        assert(!lcs_config_value_copy(&copy->history[i], &runtime.application.history[i]));
    assert(!lcs_config_value_copy(&copy->applied, &runtime.application.applied));
    assert(!lcs_config_value_copy(&copy->pending, &runtime.application.pending));
}

/* Import an actual durable application record independently for each member. */
static void publish_owner_catalog(lcs_config_application_t *copy)
{
    char *document; size_t length;
    assert(!lcs_config_application_export(&runtime.application, &document, &length, error, sizeof(error)));
    assert(!lcs_config_application_import(copy, &runtime.agreement, document, length, error, sizeof(error)));
    free(document);
}

static void test_owner_receipt_retirement(void)
{
    all_full_fixture = true; setup(false); all_full_fixture = false;
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    lcs_config_owner_scope_t scopes[3];
    assert(!lcs_config_owner_scope(&runtime.application, &scopes[0], error, sizeof(error)));
    lcs_config_application_t catalogs[3];
    owner_bus_t bus = {0}; uint64_t instances[LCS_MAX_NODES] = {100, 101, 102}, now = 1000;
    for (size_t i = 0; i < 3; i++)
    {
        copy_owner_catalog(&catalogs[i]);
        assert(!lcs_config_owner_network_init(&bus.nodes[i], runtime.base, &catalogs[i], i, instances[i]));
        assert(!lcs_config_owner_network_start(&bus.nodes[i], &scopes[0], now));
    }
    owner_exchange(&bus, instances, ++now);
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    publish_owner_catalog(&catalogs[0]); publish_owner_catalog(&catalogs[1]);
    owner_exchange(&bus, instances, now += 300);
    for (size_t i = 0; i < 2; i++)
        assert(bus.nodes[i].applied && bus.nodes[i].release.active && bus.nodes[i].release.stopped == 3 && !bus.nodes[i].retired);
    assert(!bus.nodes[2].applied && !bus.nodes[2].release.active); /* STOPPED is not APPLIED. */

    lcs_config_owner_message_t codec = { .kind = LCS_CONFIG_OWNER_APPLIED_QUERY, .sender_instance = 100,
        .target_instance = 101, .id = 55, .scope = scopes[0] };
    owner_message_roundtrip(&codec);
    codec.kind = LCS_CONFIG_OWNER_APPLIED_REPLY; codec.result = LCS_CONFIG_OWNER_STOPPED;
    owner_message_roundtrip(&codec);
    unsigned char bad[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t bad_length;
    codec.kind = LCS_CONFIG_OWNER_APPLIED_QUERY;
    assert(lcs_config_owner_encode(&codec, bad, sizeof(bad), &bad_length) < 0);
    codec.kind = 5; codec.result = 0;
    assert(lcs_config_owner_encode(&codec, bad, sizeof(bad), &bad_length) < 0);

    /* Stop replies with a release ID cannot count as application confirmation. */
    lcs_config_owner_message_t query, reply;
    assert(!lcs_config_owner_query(&bus.nodes[0].release, 2, 102, ++now, &query));
    reply = query; reply.kind = LCS_CONFIG_OWNER_REPLY; reply.result = LCS_CONFIG_OWNER_STOPPED;
    reply.sender_instance = 102; reply.target_instance = 100;
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&reply, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[0], 2, 102, payload, length, ++now));
    assert(bus.nodes[0].release.stopped == 3);
    reply.kind = LCS_CONFIG_OWNER_APPLIED_REPLY;
    reply.scope.target_value[0] = reply.scope.target_value[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_owner_encode(&reply, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[0], 2, 102, payload, length, ++now));
    assert(bus.nodes[0].release.failed && bus.nodes[0].receipt.sealed && !bus.nodes[0].retired);
    assert(!lcs_config_owner_network_retry(&bus.nodes[0], ++now));
    lcs_config_owner_network_disconnect(&bus.nodes[1], 0);
    assert(bus.nodes[1].release.failed && bus.nodes[1].receipt.sealed);
    assert(!lcs_config_owner_network_retry(&bus.nodes[1], ++now));
    owner_exchange(&bus, instances, now += 300);

    bus.drop_applied_replies = 4;
    publish_owner_catalog(&catalogs[2]);
    owner_exchange(&bus, instances, now += 300);
    assert(bus.nodes[0].retired && bus.nodes[1].retired && !bus.nodes[2].retired);
    assert(!bus.nodes[0].receipt.sealed && bus.nodes[2].receipt.sealed && bus.nodes[2].applied);
    /* Expiry preserves the receipt; only explicit retry starts another round. */
    now = bus.nodes[2].release.deadline_ms;
    bus.source = 2;
    lcs_config_owner_network_process(&bus.nodes[2], instances, now, owner_send, &bus);
    assert(bus.nodes[2].release.failed && bus.nodes[2].receipt.sealed && !bus.nodes[2].retired);
    assert(!lcs_config_owner_network_retry(&bus.nodes[2], ++now));

    lcs_config_value_t second = successor(&first, true); learn(&second);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    assert(!lcs_config_owner_scope(&runtime.application, &scopes[1], error, sizeof(error)));
    for (size_t i = 0; i < 2; i++)
    {
        publish_owner_catalog(&catalogs[i]);
        assert(!lcs_config_owner_network_start(&bus.nodes[i], &scopes[1], now));
    }
    owner_exchange(&bus, instances, now += 300);
    assert(bus.nodes[0].round.stopped == 3 && bus.nodes[2].receipt.scope.target_revision == 1);
    assert(lcs_config_owner_scope_equal(&bus.nodes[0].retired_scope, &scopes[0]));
    /* A restarted daemon cannot reconstruct a stop/completion receipt from a
     * revision number or a catalog marker without backend startup recovery. */
    lcs_config_owner_network_t restarted;
    assert(!lcs_config_owner_network_init(&restarted, runtime.base, &catalogs[0], 0, 900));
    query = (lcs_config_owner_message_t){ .kind = LCS_CONFIG_OWNER_APPLIED_QUERY, .sender_instance = 102,
        .target_instance = 900, .id = 1, .scope = scopes[0] };
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&restarted, 2, 102, payload, length, now));
    assert(restarted.replies[2].result == LCS_CONFIG_OWNER_PENDING && !restarted.receipt.sealed);
    lcs_config_owner_network_stop(&restarted);
    /* A fast peer answers a slow peer's exact completion query from its tombstone,
     * even though its live catalog is already pending the next transition. */
    bus.drop_applied_replies = 0;
    owner_exchange(&bus, instances, now += 300);
    assert(bus.nodes[2].retired && !bus.nodes[2].receipt.sealed);
    publish_owner_catalog(&catalogs[2]);
    assert(!lcs_config_owner_network_start(&bus.nodes[2], &scopes[1], now));
    owner_exchange(&bus, instances, now += 300);
    for (size_t i = 0; i < 3; i++)
        assert(bus.nodes[i].round.sealed && lcs_config_owner_scope_equal(&bus.nodes[i].retired_scope, &scopes[0]));

    assert(!lcs_config_application_finish(&runtime.application, second.digest, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) publish_owner_catalog(&catalogs[i]);
    bus.drop_applied_replies = 4;
    owner_exchange(&bus, instances, now += 300);
    assert(lcs_config_owner_scope_equal(&bus.nodes[0].retired_scope, &scopes[1]));
    assert(lcs_config_owner_scope_equal(&bus.nodes[2].retired_scope, &scopes[0]));
    assert(bus.nodes[2].receipt.sealed && bus.nodes[2].applied);

    lcs_config_value_t third = successor(&second, false); learn(&third);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    assert(!lcs_config_owner_scope(&runtime.application, &scopes[2], error, sizeof(error)));
    for (size_t i = 0; i < 2; i++)
    {
        publish_owner_catalog(&catalogs[i]);
        assert(!lcs_config_owner_network_start(&bus.nodes[i], &scopes[2], now));
    }
    owner_exchange(&bus, instances, now += 300);
    assert(bus.nodes[0].round.stopped == 3 && bus.nodes[2].receipt.scope.target_revision == 2);
    bus.drop_applied_replies = 0;
    owner_exchange(&bus, instances, now += 300);
    assert(bus.nodes[2].retired && !bus.nodes[2].receipt.sealed);
    publish_owner_catalog(&catalogs[2]);
    assert(!lcs_config_owner_network_start(&bus.nodes[2], &scopes[2], now));
    owner_exchange(&bus, instances, now += 300);
    assert(!lcs_config_application_finish(&runtime.application, third.digest, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++) publish_owner_catalog(&catalogs[i]);
    owner_exchange(&bus, instances, now += 300);
    for (size_t i = 0; i < 3; i++)
    {
        assert(bus.nodes[i].retired && lcs_config_owner_scope_equal(&bus.nodes[i].retired_scope, &scopes[2]));
        assert(!bus.nodes[i].receipt.sealed && !bus.nodes[i].round.active && !bus.nodes[i].release.active);
        lcs_config_owner_network_stop(&bus.nodes[i]); lcs_config_application_close(&catalogs[i]);
    }
    lcs_config_value_clear(&first); lcs_config_value_clear(&second); lcs_config_value_clear(&third); cleanup();
}


static void test_owner_release_poisoned_catalog(void)
{
    setup(false);
    lcs_config_value_t first = successor(&runtime.agreement.committed, false); learn(&first);
    assert(!lcs_config_application_begin(&runtime.application, &runtime.agreement, error, sizeof(error)));
    lcs_config_owner_scope_t scope;
    assert(!lcs_config_owner_scope(&runtime.application, &scope, error, sizeof(error)));
    lcs_config_application_t catalog; copy_owner_catalog(&catalog);
    owner_bus_t bus = {0}; uint64_t instances[LCS_MAX_NODES] = {100, 101, 0};
    assert(!lcs_config_owner_network_init(&bus.nodes[0], runtime.base, &catalog, 0, 100));
    assert(!lcs_config_owner_network_start(&bus.nodes[0], &scope, 1000));
    assert(!lcs_config_application_finish(&runtime.application, first.digest, error, sizeof(error)));
    publish_owner_catalog(&catalog);
    lcs_config_owner_message_t query = { .kind = LCS_CONFIG_OWNER_APPLIED_QUERY, .sender_instance = 101,
        .target_instance = 100, .id = 99, .scope = scope };
    unsigned char payload[LCS_CONFIG_OWNER_MESSAGE_SIZE]; size_t length;
    assert(!lcs_config_owner_encode(&query, payload, sizeof(payload), &length));
    assert(!lcs_config_owner_network_receive(&bus.nodes[0], 1, 101, payload, length, 1001));
    assert(bus.nodes[0].replies[1].result == LCS_CONFIG_OWNER_STOPPED && bus.nodes[0].applied);
    catalog.poisoned = true; bus.backpressure = true;
    lcs_config_owner_network_process(&bus.nodes[0], instances, 1002, owner_send, &bus);
    assert(bus.nodes[0].replies[1].result == LCS_CONFIG_OWNER_FAILED && bus.nodes[0].receipt.failed);
    assert(!bus.nodes[0].retired && lcs_config_owner_network_retry(&bus.nodes[0], 1003) < 0);
    lcs_config_owner_network_stop(&bus.nodes[0]); lcs_config_application_close(&catalog);
    lcs_config_value_clear(&first); cleanup();
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
    test_live_drain_and_incarnation_remap();
    test_valid_lease_retention_during_remap();
    test_durable_projection();
    test_projection_cancellation();
    test_live_managed_projection(false);
    test_live_managed_projection(true);
    test_owner_barrier_proofs();
    test_owner_network_exchange();
    test_live_owner_network_recheck();
    test_live_projection_at_install(false);
    test_live_projection_at_install(true);
    for (unsigned fault = 0; fault < 4; fault++) test_live_admission_cancellation(fault);
    test_owner_receipt_retirement();
    test_owner_release_poisoned_catalog();
    puts("durable configuration reconciliation ordering and safety barriers unit tests passed");
    return 0;
}
