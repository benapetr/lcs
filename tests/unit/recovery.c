// SPDX-License-Identifier: GPL-3.0-or-later

#include "cluster.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;
static uint64_t fake_now_ms;
static bool cleanup_complete;

uint64_t lcs_now_ms(void)
{
    return fake_now_ms;
}

bool resources_startup_cleanup_complete(void)
{
    return cleanup_complete;
}

void lcs_log_info(const char *fmt, ...)
{
    (void)fmt;
}

static void initialize_restarted_voter(void)
{
    memset(&g_state, 0, sizeof(g_state));
    fake_now_ms = 1000;
    cleanup_complete = true;
    g_state.self_index = 1;
    g_state.cfg.node_count = 3;
    g_state.cfg.lease_ms = 5000;
    g_state.cfg.peer_timeout_ms = 1000;
    g_state.quorum_needed = 2;
    g_state.voting_not_before_ms = fake_now_ms +
                                   g_state.cfg.lease_ms +
                                   g_state.cfg.peer_timeout_ms;

    /* A is partitioned. The restarted B can synchronize only with C. */
    g_state.peers[0].online = false;
    g_state.peers[2].online = true;
    g_state.peers[2].last_seen_ms = fake_now_ms;
    g_state.peers[2].initial_sync_complete = true;
}

static void test_partitioned_restart_waits_out_forgotten_grant(void)
{
    initialize_restarted_voter();

    cluster_update_recovery_state();
    assert(!g_state.voting_ready);

    fake_now_ms = g_state.voting_not_before_ms - 1;
    g_state.peers[2].last_seen_ms = fake_now_ms;
    cluster_update_recovery_state();
    assert(!g_state.voting_ready);

    fake_now_ms = g_state.voting_not_before_ms;
    g_state.peers[2].last_seen_ms = fake_now_ms;
    cluster_update_recovery_state();
    assert(g_state.voting_ready);
}

static void test_elapsed_timer_without_sync_is_not_enough(void)
{
    initialize_restarted_voter();
    fake_now_ms = g_state.voting_not_before_ms;
    g_state.peers[2].last_seen_ms = fake_now_ms;
    g_state.peers[2].initial_sync_complete = false;
    cluster_update_recovery_state();
    assert(!g_state.voting_ready);

    g_state.peers[2].initial_sync_complete = true;
    cleanup_complete = false;
    cluster_update_recovery_state();
    assert(!g_state.voting_ready);

    cleanup_complete = true;
    cluster_update_recovery_state();
    assert(g_state.voting_ready);
}

int main(void)
{
    test_partitioned_restart_waits_out_forgotten_grant();
    test_elapsed_timer_without_sync_is_not_enough();
    return 0;
}
