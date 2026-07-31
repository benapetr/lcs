// SPDX-License-Identifier: GPL-3.0-or-later

#include "cluster.h"
#include "resources.h"

#include <assert.h>
#include <stdarg.h>
#include <string.h>

daemon_state_t g_state;
static uint64_t fake_now_ms = 1000;

typedef struct
{
    uint16_t id;
    uint16_t owner;
    uint64_t owner_instance_id;
    uint8_t state;
    uint64_t epoch;
    uint64_t lease_id;
    uint64_t remaining_ms;
    uint64_t failover_count;
    uint64_t home_generation;
    uint8_t home_blocked;
    uint64_t disabled_generation;
    uint8_t disabled;
    const char *reason;
} test_state_entry_t;

uint64_t lcs_now_ms(void)
{
    return fake_now_ms;
}

int resources_stop_local_backend(const lcs_resource_config_t *resource)
{
    (void)resource;
    return 0;
}

bool resources_preserve_startup_cleanup_failure(int resource_idx,
                                                uint64_t incoming_epoch)
{
    (void)resource_idx;
    (void)incoming_epoch;
    return false;
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

void lcs_log_warn(const char *fmt, ...)
{
    (void)fmt;
}

static test_state_entry_t active_entry(uint16_t id, uint16_t owner,
                                       uint64_t owner_instance_id,
                                       uint64_t epoch, uint64_t lease_id,
                                       uint64_t remaining_ms)
{
    test_state_entry_t entry;
    memset(&entry, 0, sizeof(entry));
    entry.id = id;
    entry.owner = owner;
    entry.owner_instance_id = owner_instance_id;
    entry.state = LCS_RES_ACTIVE;
    entry.epoch = epoch;
    entry.lease_id = lease_id;
    entry.remaining_ms = remaining_ms;
    entry.reason = "";
    return entry;
}

static void encode_entry(lcs_buf_writer_t *w, const test_state_entry_t *entry)
{
    assert(lcs_buf_put_u16(w, entry->id) == 0);
    assert(lcs_buf_put_u16(w, entry->owner) == 0);
    assert(lcs_buf_put_u64(w, entry->owner_instance_id) == 0);
    assert(lcs_buf_put_u8(w, entry->state) == 0);
    assert(lcs_buf_put_u64(w, entry->epoch) == 0);
    assert(lcs_buf_put_u64(w, entry->lease_id) == 0);
    assert(lcs_buf_put_u64(w, entry->remaining_ms) == 0);
    assert(lcs_buf_put_u64(w, entry->failover_count) == 0);
    assert(lcs_buf_put_u64(w, entry->home_generation) == 0);
    assert(lcs_buf_put_u8(w, entry->home_blocked) == 0);
    assert(lcs_buf_put_u64(w, entry->disabled_generation) == 0);
    assert(lcs_buf_put_u8(w, entry->disabled) == 0);
    assert(lcs_buf_put_fixed_string(w, entry->reason ? entry->reason : "",
                                    LCS_REASON_MAX + 1) == 0);
}

static size_t encode_state(unsigned char *payload,
                           const test_state_entry_t *entries, size_t count)
{
    lcs_buf_writer_t w;
    lcs_buf_writer_init(&w, payload, LCS_MAX_FRAME);
    assert(lcs_buf_put_u64(&w, 202) == 0);
    assert(lcs_buf_put_u16(&w, (uint16_t)count) == 0);
    for (size_t i = 0; i < count; i++)
        encode_entry(&w, &entries[i]);
    return w.len;
}

static void assert_rejected_unchanged(unsigned char *payload, size_t len)
{
    resource_runtime_t before[LCS_MAX_RESOURCES];
    memcpy(before, g_state.resources, sizeof(before));
    assert(cluster_apply_state(payload, len, 2) != 0);
    assert(memcmp(before, g_state.resources, sizeof(before)) == 0);
}

static void initialize_state(size_t resource_count)
{
    memset(&g_state, 0, sizeof(g_state));
    g_state.self_index = 0;
    g_state.instance_id = 100;
    g_state.cfg.node_count = 3;
    g_state.cfg.nodes[0].role = LCS_NODE_FULL;
    g_state.cfg.nodes[1].role = LCS_NODE_FULL;
    g_state.cfg.nodes[2].role = LCS_NODE_FULL;
    g_state.cfg.resource_count = resource_count;
    g_state.cfg.lease_ms = 5000;
    g_state.peers[2].instance_id = 202;
    for (size_t i = 0; i < resource_count; i++)
    {
        g_state.resources[i].owner_node = -1;
        g_state.resources[i].state = LCS_RES_STOPPED;
    }
}

static void test_valid_merge_rules(void)
{
    initialize_state(1);
    resource_runtime_t *res = &g_state.resources[0];
    res->owner_node = 1;
    res->owner_instance_id = 101;
    res->state = LCS_RES_ACTIVE;
    res->epoch = 5;
    res->lease_id = 55;
    res->lease_deadline_ms = fake_now_ms + 1000;

    unsigned char payload[LCS_MAX_FRAME];
    test_state_entry_t entry = active_entry(0, 1, 101, 5, 55, 5000);
    size_t len = encode_state(payload, &entry, 1);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == 2000);

    entry = active_entry(0, 2, 202, 6, 66, 5000);
    len = encode_state(payload, &entry, 1);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == fake_now_ms + g_state.cfg.lease_ms);
    uint64_t initialized_deadline = res->lease_deadline_ms;

    fake_now_ms += 100;
    len = encode_state(payload, &entry, 1);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->lease_deadline_ms == initialized_deadline);

    res->owner_node = g_state.self_index;
    res->owner_instance_id = g_state.instance_id;
    res->epoch = 7;
    res->lease_id = 77;
    res->lease_deadline_ms = 9000;
    entry = active_entry(0, 2, 202, 8, 88, 5000);
    len = encode_state(payload, &entry, 1);
    assert(cluster_apply_state(payload, len, 2) == 0);
    assert(res->owner_node == g_state.self_index);
    assert(res->epoch == 7);
    assert(res->lease_deadline_ms == 9000);
}

static void test_invalid_entries_are_atomic(void)
{
    initialize_state(1);
    resource_runtime_t *res = &g_state.resources[0];
    res->owner_node = 1;
    res->owner_instance_id = 101;
    res->state = LCS_RES_ACTIVE;
    res->epoch = 5;
    res->lease_id = 55;
    res->lease_deadline_ms = 2000;

    unsigned char payload[LCS_MAX_FRAME];
    test_state_entry_t entry = active_entry(0, 3, 303, 6, 66, 1000);
    size_t len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    g_state.cfg.nodes[2].role = LCS_NODE_QUORUM_ONLY;
    entry = active_entry(0, 2, 202, 6, 66, 1000);
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);
    g_state.cfg.nodes[2].role = LCS_NODE_FULL;

    entry = active_entry(0, 1, 101, 6, 66, 1000);
    entry.state = 99;
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, 1, 101, 6, 66, 1000);
    entry.home_blocked = 2;
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, 1, 101, 6, 66, 5001);
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, UINT16_MAX, 0, 6, 66, 1000);
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(1, 1, 101, 6, 66, 1000);
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, 1, 101, 6, 66, 0);
    entry.state = LCS_RES_STOPPED;
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, UINT16_MAX, 101, 6, 0, 0);
    entry.state = LCS_RES_STOP_FAILED;
    len = encode_state(payload, &entry, 1);
    assert_rejected_unchanged(payload, len);

    entry = active_entry(0, 1, 101, 6, 66, 1000);
    len = encode_state(payload, &entry, 1);
    payload[len++] = 0xaa;
    assert_rejected_unchanged(payload, len);

    assert_rejected_unchanged(payload, len - 10);
}

static void test_duplicate_id_does_not_partially_apply(void)
{
    initialize_state(2);
    g_state.resources[0].epoch = 5;
    g_state.resources[0].home_generation = 3;
    g_state.resources[1].epoch = 7;

    test_state_entry_t entries[2];
    entries[0] = active_entry(0, 1, 101, 9, 99, 1000);
    entries[0].home_generation = 20;
    entries[0].home_blocked = 1;
    entries[1] = active_entry(0, 2, 202, 10, 100, 1000);

    unsigned char payload[LCS_MAX_FRAME];
    size_t len = encode_state(payload, entries, 2);
    assert_rejected_unchanged(payload, len);
    assert(g_state.resources[0].epoch == 5);
    assert(g_state.resources[0].home_generation == 3);
}

int main(void)
{
    test_valid_merge_rules();
    test_invalid_entries_are_atomic();
    test_duplicate_id_does_not_partially_apply();
    return 0;
}
