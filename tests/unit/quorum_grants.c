// SPDX-License-Identifier: GPL-3.0-or-later

#include "cluster.h"
#include "lease.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;
static uint64_t fake_now_ms = 1000;

uint64_t lcs_now_ms(void)
{
    return fake_now_ms;
}

bool cluster_local_voting_ready(void)
{
    return g_state.voting_ready;
}

int resources_stop_local_backend(const lcs_resource_config_t *resource)
{
    (void)resource;
    return 0;
}

void resources_enter_stop_failed_state(int resource_idx, uint64_t epoch,
                                       const char *reason, int epoll_fd)
{
    (void)resource_idx;
    (void)epoch;
    (void)reason;
    (void)epoll_fd;
    assert(!"unexpected stop_failed transition");
}

void lcs_log_warn(const char *fmt, ...)
{
    (void)fmt;
}

static void initialize_state(void)
{
    memset(&g_state, 0, sizeof(g_state));
    fake_now_ms = 1000;
    g_state.self_index = 0;
    g_state.instance_id = 100;
    g_state.voting_ready = true;
    g_state.cfg.node_count = 3;
    g_state.cfg.resource_count = 1;
    g_state.cfg.lease_ms = 5000;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
        g_state.cfg.nodes[i].role = LCS_NODE_FULL;
    g_state.peers[1].instance_id = 101;
    g_state.peers[2].instance_id = 202;
    g_state.resources[0].owner_node = -1;
    g_state.resources[0].state = LCS_RES_STOPPED;
}

static size_t encode(unsigned char *payload, int owner, uint64_t epoch,
                     uint64_t lease_id, uint64_t instance_id)
{
    size_t len = 0;
    assert(lease_encode_msg(payload, LCS_MAX_FRAME, &len, 0,
                            (uint16_t)owner, epoch, lease_id,
                            g_state.cfg.lease_ms, instance_id) == 0);
    return len;
}

static void test_grant_conflict_and_release(void)
{
    initialize_state();

    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode(payload, 1, 1, 11, 101);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 1) == 0);

    /* A tentative vote is private and must not become resource ownership. */
    assert(g_state.lease_grants[0].active);
    assert(g_state.resources[0].owner_node == -1);
    assert(g_state.resources[0].state == LCS_RES_STOPPED);
    uint64_t first_deadline = g_state.lease_grants[0].deadline_ms;

    /* A higher epoch cannot preempt a still-live conflicting grant. */
    len = encode(payload, 2, 2, 22, 202);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 2) != 0);

    /* Retrying the same request is idempotent and cannot ratchet its deadline. */
    fake_now_ms += 100;
    len = encode(payload, 1, 1, 11, 101);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 1) == 0);
    assert(g_state.lease_grants[0].deadline_ms == first_deadline);

    assert(lease_accept_message(LCS_MSG_LEASE_RELEASE, payload, len, 1) == 0);
    assert(!g_state.lease_grants[0].active);
    assert(g_state.lease_grants[0].promised_epoch == 2);

    /* Delayed old requests stay rejected after release. */
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 1) != 0);

    len = encode(payload, 2, 2, 22, 202);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 2) == 0);
    assert(g_state.lease_grants[0].owner_node == 2);

    /* A delayed release for lease 11 cannot erase the newer lease 22 grant. */
    len = encode(payload, 1, 1, 11, 101);
    assert(lease_accept_message(LCS_MSG_LEASE_RELEASE, payload, len, 1) == 0);
    assert(g_state.lease_grants[0].active);
    assert(g_state.lease_grants[0].owner_node == 2);
    assert(g_state.lease_grants[0].lease_id == 22);
}

static void test_tentative_grant_expires_without_commit(void)
{
    initialize_state();
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode(payload, 1, 1, 11, 101);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 1) == 0);
    assert(g_state.resources[0].state == LCS_RES_STOPPED);

    fake_now_ms += g_state.cfg.lease_ms;
    len = encode(payload, 2, 2, 22, 202);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 2) == 0);
    assert(g_state.lease_grants[0].owner_node == 2);
    assert(g_state.resources[0].state == LCS_RES_STOPPED);
}

static void test_commit_without_vote_is_observation_only(void)
{
    initialize_state();
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode(payload, 1, 5, 55, 101);
    assert(lease_apply_commit(payload, len, 1, -1) == 0);
    assert(g_state.resources[0].owner_node == 1);
    assert(g_state.resources[0].epoch == 5);
    assert(g_state.resources[0].lease_deadline_ms ==
           fake_now_ms + g_state.cfg.lease_ms);
    assert(!g_state.lease_grants[0].active);
}

static void test_commit_does_not_extend_matching_grant(void)
{
    initialize_state();
    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode(payload, 1, 5, 55, 101);
    assert(lease_accept_message(LCS_MSG_LEASE_REQ, payload, len, 1) == 0);
    uint64_t grant_deadline = g_state.lease_grants[0].deadline_ms;

    fake_now_ms += 200;
    assert(lease_apply_commit(payload, len, 1, -1) == 0);
    assert(g_state.lease_grants[0].deadline_ms == grant_deadline);
    assert(g_state.resources[0].lease_deadline_ms ==
           fake_now_ms + g_state.cfg.lease_ms);
}

int main(void)
{
    test_grant_conflict_and_release();
    test_tentative_grant_expires_without_commit();
    test_commit_without_vote_is_observation_only();
    test_commit_does_not_extend_matching_grant();
    return 0;
}
