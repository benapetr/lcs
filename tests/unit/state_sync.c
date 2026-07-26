// SPDX-License-Identifier: GPL-3.0-or-later

#include "cluster.h"
#include "resources.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;
static uint64_t fake_now_ms = 1000;

uint64_t lcs_now_ms(void)
{
    return fake_now_ms;
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

void lcs_log_debug3(const char *fmt, ...)
{
    (void)fmt;
}

static size_t encode_state(unsigned char *payload, uint16_t owner,
                           uint64_t owner_instance, uint64_t epoch,
                           uint64_t lease_id, uint64_t remaining_ms)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, LCS_MAX_FRAME);
    assert(lcs_buf_put_u64(&w, 202) == 0);
    assert(lcs_buf_put_u16(&w, 1) == 0);
    assert(lcs_buf_put_u16(&w, 0) == 0);
    assert(lcs_buf_put_u16(&w, owner) == 0);
    assert(lcs_buf_put_u64(&w, owner_instance) == 0);
    assert(lcs_buf_put_u8(&w, LCS_RES_ACTIVE) == 0);
    assert(lcs_buf_put_u64(&w, epoch) == 0);
    assert(lcs_buf_put_u64(&w, lease_id) == 0);
    assert(lcs_buf_put_u64(&w, remaining_ms) == 0);
    assert(lcs_buf_put_u64(&w, 0) == 0);
    assert(lcs_buf_put_u64(&w, 0) == 0);
    assert(lcs_buf_put_u8(&w, 0) == 0);
    assert(lcs_buf_put_u64(&w, 0) == 0);
    assert(lcs_buf_put_u8(&w, 0) == 0);
    assert(lcs_buf_put_fixed_string(&w, "", LCS_REASON_MAX + 1) == 0);
    return w.len;
}

int main(void)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.self_index = 0;
    g_state.instance_id = 100;
    g_state.cfg.node_count = 3;
    g_state.cfg.resource_count = 1;
    g_state.cfg.lease_ms = 5000;
    g_state.peers[2].instance_id = 202;

    resource_runtime_t *res = &g_state.resources[0];
    res->owner_node = 1;
    res->owner_instance_id = 101;
    res->state = LCS_RES_ACTIVE;
    res->epoch = 5;
    res->lease_id = 55;
    res->lease_deadline_ms = fake_now_ms + 1000;

    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode_state(payload, 1, 101, 5, 55, 5000);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == 2000);

    len = encode_state(payload, 2, 202, 6, 66, 50000);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == fake_now_ms + g_state.cfg.lease_ms);
    uint64_t initialized_deadline = res->lease_deadline_ms;

    fake_now_ms += 100;
    len = encode_state(payload, 2, 202, 6, 66, 5000);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == initialized_deadline);

    res->owner_node = g_state.self_index;
    res->owner_instance_id = g_state.instance_id;
    res->epoch = 7;
    res->lease_id = 77;
    res->lease_deadline_ms = 9000;
    len = encode_state(payload, 2, 202, 8, 88, 5000);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->owner_node == g_state.self_index);
    assert(res->epoch == 7);
    assert(res->lease_deadline_ms == 9000);
    return 0;
}
