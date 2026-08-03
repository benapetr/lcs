// SPDX-License-Identifier: GPL-3.0-or-later

#include "cluster.h"
#include "lease.h"
#include "peer.h"
#include "protocol.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;

static uint64_t fake_now_ms;
static bool quorum_available;
static int activation_count;
static int drop_count;
static int commit_count;

typedef struct
{
    bool active;
    int node_idx;
    uint16_t type;
    peer_rpc_callback_t callback;
    void *callback_ctx;
} pending_rpc_t;

static pending_rpc_t pending[LCS_MAX_NODES];

uint64_t lcs_now_ms(void)
{
    return fake_now_ms;
}

uint64_t lcs_jittered_delay_ms(uint32_t base_ms)
{
    return base_ms;
}

int cluster_has_quorum(void)
{
    return quorum_available;
}

bool cluster_local_voting_ready(void)
{
    return g_state.voting_ready;
}

const char *cluster_node_name_or_none(int node_idx)
{
    if (node_idx < 0 || (size_t)node_idx >= g_state.cfg.node_count)
        return "-";
    return g_state.cfg.nodes[node_idx].name;
}

int peer_rpc_async(int epoll_fd, int node_idx, uint16_t req_type,
                   const void *req_payload, uint32_t req_len,
                   uint16_t expected_type, unsigned char *resp_payload,
                   size_t resp_cap, uint32_t *resp_len, uint32_t timeout_ms,
                   peer_rpc_callback_t callback, void *callback_ctx)
{
    (void)epoll_fd;
    (void)req_payload;
    (void)req_len;
    (void)expected_type;
    (void)resp_payload;
    (void)resp_cap;
    (void)resp_len;
    (void)timeout_ms;
    assert(node_idx > 0 && (size_t)node_idx < g_state.cfg.node_count);
    assert(!pending[node_idx].active);
    pending[node_idx].active = true;
    pending[node_idx].node_idx = node_idx;
    pending[node_idx].type = req_type;
    pending[node_idx].callback = callback;
    pending[node_idx].callback_ctx = callback_ctx;
    return 0;
}

void peer_detach_rpcs_by_context(void *callback_ctx)
{
    for (size_t i = 0; i < LCS_MAX_NODES; i++)
    {
        if (pending[i].active && pending[i].callback_ctx == callback_ctx)
            pending[i].active = false;
    }
}

void peer_broadcast_lease_commit(int epoll_fd, const void *payload, uint32_t len)
{
    (void)epoll_fd;
    (void)payload;
    (void)len;
    commit_count++;
}

int peer_queue_simple_resp(int epoll_fd, int node_idx, uint32_t seq,
                           uint16_t type, int32_t status,
                           const char *message)
{
    (void)epoll_fd;
    (void)node_idx;
    (void)seq;
    (void)type;
    (void)status;
    (void)message;
    return 0;
}

int resources_activate_acquired_local(int resource_idx, uint64_t epoch,
                                      uint64_t lease_id, int epoll_fd)
{
    (void)resource_idx;
    (void)epoch;
    (void)lease_id;
    (void)epoll_fd;
    activation_count++;
    return 0;
}

void resources_drop_local(int resource_idx, int epoll_fd)
{
    (void)epoll_fd;
    resource_runtime_t *res = &g_state.resources[resource_idx];
    drop_count++;
    res->owner_node = -1;
    res->owner_instance_id = 0;
    res->state = LCS_RES_STOPPED;
    res->lease_id = 0;
    res->lease_deadline_ms = 0;
    res->renew_after_ms = 0;
}

int resources_release_for_handoff(int resource_idx, uint64_t epoch,
                                  uint64_t lease_id, int epoll_fd)
{
    (void)resource_idx;
    (void)epoch;
    (void)lease_id;
    (void)epoll_fd;
    return 0;
}

void lcs_log_info(const char *fmt, ...)
{
    (void)fmt;
}

void lcs_log_warn(const char *fmt, ...)
{
    (void)fmt;
}

void lcs_log_debug(const char *fmt, ...)
{
    (void)fmt;
}

static void initialize_state(void)
{
    memset(&g_state, 0, sizeof(g_state));
    memset(pending, 0, sizeof(pending));
    fake_now_ms = 1000;
    quorum_available = true;
    activation_count = 0;
    drop_count = 0;
    commit_count = 0;

    g_state.self_index = 0;
    g_state.instance_id = 100;
    g_state.voting_ready = true;
    g_state.quorum_needed = 2;
    g_state.votes_seen = 3;
    g_state.cfg.node_count = 3;
    g_state.cfg.resource_count = 1;
    g_state.cfg.lease_ms = 5000;
    g_state.cfg.renew_ms = 1000;
    g_state.cfg.peer_timeout_ms = 500;
    for (size_t i = 0; i < g_state.cfg.node_count; i++)
    {
        g_state.cfg.nodes[i].role = LCS_NODE_FULL;
        g_state.cfg.nodes[i].name[0] = (char)('A' + i);
        g_state.cfg.nodes[i].name[1] = '\0';
    }
    strcpy(g_state.cfg.resources[0].name, "resource");
    g_state.resources[0].owner_node = -1;
    g_state.resources[0].state = LCS_RES_STOPPED;
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        g_state.lease_ops[i].resource_idx = -1;
        g_state.lease_ops[i].owner_idx = -1;
        g_state.lease_ops[i].release_response_node = -1;
    }
}

static void finish_rpc(int node_idx, int status)
{
    pending_rpc_t rpc = pending[node_idx];
    assert(rpc.active);
    pending[node_idx].active = false;

    unsigned char response[LCS_LEASE_RESP_PAYLOAD_SIZE];
    size_t response_len = 0;
    assert(lcs_encode_simple_resp(response, sizeof(response), &response_len,
                                  status, status == 0 ? "ack" : "reject") == 0);
    rpc.callback(rpc.callback_ctx, 0, response, (uint32_t)response_len);
}

static lease_runtime_t *active_operation(void)
{
    for (size_t i = 0; i < LCS_LEASE_OP_MAX; i++)
    {
        if (g_state.lease_ops[i].active)
            return &g_state.lease_ops[i];
    }
    return NULL;
}

static void establish_local_ownership(uint64_t deadline_ms)
{
    resource_runtime_t *res = &g_state.resources[0];
    res->owner_node = 0;
    res->owner_instance_id = g_state.instance_id;
    res->state = LCS_RES_ACTIVE;
    res->epoch = 7;
    res->lease_id = 77;
    res->lease_deadline_ms = deadline_ms;
    res->renew_after_ms = fake_now_ms;

    lease_grant_t *grant = &g_state.lease_grants[0];
    grant->active = true;
    grant->owner_node = 0;
    grant->owner_instance_id = g_state.instance_id;
    grant->epoch = 7;
    grant->lease_id = 77;
    grant->deadline_ms = deadline_ms;
    grant->promised_epoch = 7;
}

static void test_acquire_requires_processed_majority(void)
{
    initialize_state();
    assert(lease_start_acquire(0, 0, 1, 11, -1) == 0);

    /* A peer may have granted while its ACK remains lost or unprocessed. */
    assert(pending[1].type == LCS_MSG_LEASE_REQ);
    assert(g_state.resources[0].owner_node == -1);
    assert(activation_count == 0);

    finish_rpc(1, -1);
    finish_rpc(2, -1);
    lease_process_operations(-1);
    assert(!lease_operation_active(0));
    assert(g_state.resources[0].owner_node == -1);
    assert(activation_count == 0);
    assert(commit_count == 0);
}

static void test_acquire_commits_after_majority(void)
{
    initialize_state();
    assert(lease_start_acquire(0, 0, 1, 11, -1) == 0);
    finish_rpc(1, 0);
    finish_rpc(2, -1);
    lease_process_operations(-1);

    assert(g_state.resources[0].owner_node == 0);
    assert(g_state.resources[0].lease_deadline_ms == 6000);
    assert(activation_count == 1);
    assert(commit_count == 1);
}

static void test_failed_renew_does_not_extend_owner_deadline(void)
{
    initialize_state();
    establish_local_ownership(4000);
    assert(lease_start_renew(0, -1) == 0);
    finish_rpc(1, -1);
    finish_rpc(2, -1);
    lease_process_operations(-1);

    assert(g_state.resources[0].lease_deadline_ms == 4000);
    assert(g_state.resources[0].owner_node == 0);
    assert(drop_count == 0);
    assert(commit_count == 0);

    fake_now_ms = 3100;
    assert(lease_start_renew(0, -1) == 0);
    finish_rpc(1, -1);
    finish_rpc(2, -1);
    lease_process_operations(-1);
    assert(drop_count == 1);
    assert(g_state.resources[0].state == LCS_RES_STOPPED);
}

static void test_slow_majority_uses_operation_deadline(void)
{
    initialize_state();
    establish_local_ownership(7000);
    assert(lease_start_renew(0, -1) == 0);
    lease_runtime_t *op = active_operation();
    assert(op != NULL);
    uint64_t proposed_deadline = op->grant_deadline_ms;

    fake_now_ms += 400;
    finish_rpc(1, 0);
    finish_rpc(2, -1);
    lease_process_operations(-1);
    assert(g_state.resources[0].lease_deadline_ms == proposed_deadline);
    assert(g_state.resources[0].lease_deadline_ms != fake_now_ms + g_state.cfg.lease_ms);
    assert(commit_count == 1);
}

int main(void)
{
    test_acquire_requires_processed_majority();
    test_acquire_commits_after_majority();
    test_failed_renew_does_not_extend_owner_deadline();
    test_slow_majority_uses_operation_deadline();
    return 0;
}
