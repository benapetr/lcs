// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_agreement.h"
#include "config_agreement_worker.h"
#include "config_transfer.h"
#include "protocol.h"

#include <assert.h>
#include <errno.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int fail_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    if (fail_sync && !--fail_sync)
    {
        errno = EIO;
        return -1;
    }
    return __real_fsync(fd);
}

typedef struct
{
    char directory[80];
    lcs_config_t cfg;
    lcs_config_store_t store;
    lcs_config_agreement_t state;
} voter_t;

static voter_t voters[3];
static lcs_config_t baseline;
static char error[256];

static void initialize(void)
{
    lcs_config_init_defaults(&baseline);
    snprintf(baseline.cluster_name, sizeof(baseline.cluster_name), "agreement-test");
    snprintf(baseline.self_name, sizeof(baseline.self_name), "node0");
    baseline.node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(baseline.nodes[i].name, sizeof(baseline.nodes[i].name), "node%zu", i);
        snprintf(baseline.nodes[i].address, sizeof(baseline.nodes[i].address), "127.0.0.1");
        baseline.nodes[i].port = 3322;
        baseline.nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    baseline.resource_count = 1;
    snprintf(baseline.resources[0].name, sizeof(baseline.resources[0].name), "vip");
    snprintf(baseline.resources[0].address, sizeof(baseline.resources[0].address), "192.0.2.1/24");
    snprintf(baseline.resources[0].interface, sizeof(baseline.resources[0].interface), "eth0");
    baseline.resources[0].type = LCS_RESOURCE_VIP;
    assert(lcs_config_finalize(&baseline, error, sizeof(error)) == 0);
    for (size_t i = 0; i < 3; i++)
    {
        voter_t *voter = &voters[i];
        voter->cfg = baseline;
        snprintf(voter->cfg.self_name, sizeof(voter->cfg.self_name), "node%zu", i);
        snprintf(voter->directory, sizeof(voter->directory), "/tmp/lcs-agreement-voter.XXXXXX");
        assert(mkdtemp(voter->directory));
        assert(lcs_config_store_open(&voter->store, voter->directory, error, sizeof(error)) == 0);
        assert(lcs_config_agreement_open(&voter->state, &voter->store,
            &voter->cfg, false, error, sizeof(error)) == -1);
        assert(strstr(error, "missing"));
        assert(lcs_config_agreement_open(&voter->state, &voter->store,
            &voter->cfg, true, error, sizeof(error)) == 0);
    }
    assert(lcs_config_value_equal(&voters[0].state.committed, &voters[2].state.committed));
    assert(!strcmp(voters[0].state.membership, voters[2].state.membership));
    /* Independently generated SHA-256 vector for the specified compact,
     * ASCII-escaped, sorted-key membership representation. */
    assert(!strcmp(voters[0].state.membership,
        "9652bdedce99187b1b4adcf68aa03de1269536a1ffb252e74e3a8ebb32971480"));
}

static void reopen(size_t i)
{
    voter_t *voter = &voters[i];
    lcs_config_agreement_close(&voter->state);
    assert(lcs_config_agreement_open(&voter->state, &voter->store,
                                     &voter->cfg, false, error, sizeof(error)) == 0);
}

static void cleanup(void)
{
    for (size_t i = 0; i < 3; i++)
    {
        voter_t *voter = &voters[i];
        lcs_config_agreement_close(&voter->state);
        assert(unlinkat(voter->store.directory_fd, "agreement.json", 0) == 0);
        assert(unlinkat(voter->store.directory_fd, "lock", 0) == 0);
        lcs_config_store_close(&voter->store);
        assert(rmdir(voter->directory) == 0);
    }
}

static lcs_config_value_t value(uint64_t revision, const char *request, const char *address)
{
    lcs_config_t cfg = baseline;
    snprintf(cfg.resources[0].address, sizeof(cfg.resources[0].address), "%s", address);
    char *document;
    size_t length;
    assert(lcs_resource_config_encode(&cfg, revision, &document, &length, error, sizeof(error)) == 0);
    lcs_config_value_t result = {0};
    assert(lcs_config_value_init(&result, &baseline, request, document, length, error, sizeof(error)) == 0);
    free(document);
    return result;
}

static int promise(lcs_config_proposal_t *proposal, size_t i)
{
    lcs_config_agreement_t *state = &voters[i].state;
    int rc = lcs_config_agreement_prepare(state, proposal->revision, proposal->parent_digest,
                                          proposal->ballot, error, sizeof(error));
    if (rc)
        return rc;
    return lcs_config_proposal_promise(proposal, &voters[0].state, i,
        proposal->revision, state->committed.digest, state->promised,
        state->accepted_ballot, state->accepted.document ? &state->accepted : NULL);
}

static int accept_proposal(lcs_config_proposal_t *proposal, size_t i, const lcs_config_value_t *selected)
{
    int rc = lcs_config_agreement_accept(&voters[i].state, proposal->parent_digest,
                                          proposal->ballot, selected, error, sizeof(error));
    if (rc)
        return rc;
    return lcs_config_proposal_ack(proposal, &voters[0].state, i, proposal->ballot, selected);
}

static void test_recovery_of_chosen_value(void)
{
    initialize();
    lcs_config_value_t a = value(1, "request-a", "192.0.2.2/24");
    lcs_config_value_t b = value(1, "request-b", "192.0.2.3/24");
    assert(strcmp(a.digest, b.digest));
    lcs_config_proposal_t first = {0};
    uint64_t ballot = lcs_config_agreement_next_ballot(&voters[0].state, 0);
    assert(ballot == 32);
    assert(lcs_config_proposal_init(&first, &voters[0].state, ballot, &a) == 0);
    assert(promise(&first, 0) == 0);
    assert(!lcs_config_proposal_seal(&first, &voters[0].state));
    assert(promise(&first, 1) == 0);
    const lcs_config_value_t *selected = lcs_config_proposal_seal(&first, &voters[0].state);
    assert(selected && lcs_config_value_equal(selected, &a));
    assert(accept_proposal(&first, 0, selected) == 0);
    assert(!lcs_config_proposal_chosen(&first, &voters[0].state));
    assert(accept_proposal(&first, 1, selected) == 0);
    assert(lcs_config_proposal_chosen(&first, &voters[0].state));
    /* Coordinator and both acceptors restart before anyone learns the result. */
    lcs_config_proposal_clear(&first);
    reopen(0); reopen(1); reopen(2);
    assert(voters[1].state.accepted_ballot == ballot);
    lcs_config_proposal_t replacement = {0};
    uint64_t newer = lcs_config_agreement_next_ballot(&voters[2].state, ballot);
    assert(lcs_config_proposal_init(&replacement, &voters[2].state, newer, &b) == 0);
    assert(promise(&replacement, 2) == 0);
    assert(promise(&replacement, 1) == 0);
    selected = lcs_config_proposal_seal(&replacement, &voters[0].state);
    assert(selected && lcs_config_value_equal(selected, &a));
    assert(!lcs_config_value_equal(selected, &b));
    assert(accept_proposal(&replacement, 1, selected) == 0);
    assert(accept_proposal(&replacement, 2, selected) == 0);
    assert(lcs_config_proposal_chosen(&replacement, &voters[0].state));
    for (size_t i = 0; i < 3; i++)
    {
        assert(lcs_config_agreement_learn(&voters[i].state, replacement.parent_digest,
            newer, replacement.acknowledgements, selected, error, sizeof(error)) == 0);
        reopen(i);
        assert(lcs_config_value_equal(&voters[i].state.committed, &a));
        assert(voters[i].state.accepted.document == NULL);
        assert(voters[i].state.promised >= newer);
        assert(lcs_config_agreement_learn(&voters[i].state, replacement.parent_digest,
            newer, replacement.acknowledgements, selected, error, sizeof(error)) == 0);
        assert(lcs_config_agreement_learn(&voters[i].state, replacement.parent_digest,
            newer, replacement.acknowledgements, &b, error, sizeof(error)) == LCS_CONFIG_REJECTED);
    }
    lcs_config_value_t next = value(2, "request-c", "192.0.2.4/24");
    lcs_config_proposal_t second_slot = {0};
    uint64_t last = lcs_config_agreement_next_ballot(&voters[0].state, newer);
    assert(last > newer);
    assert(lcs_config_proposal_init(&second_slot, &voters[0].state, last, &next) == 0);
    assert(promise(&second_slot, 0) == 0 && promise(&second_slot, 2) == 0);
    selected = lcs_config_proposal_seal(&second_slot, &voters[0].state);
    assert(selected && lcs_config_value_equal(selected, &next));
    assert(accept_proposal(&second_slot, 0, selected) == 0 && accept_proposal(&second_slot, 2, selected) == 0);
    assert(lcs_config_agreement_learn(&voters[0].state, second_slot.parent_digest,
        last, second_slot.acknowledgements, selected, error, sizeof(error)) == 0);
    assert(lcs_config_agreement_accept(&voters[0].state, replacement.parent_digest,
        newer, &a, error, sizeof(error)) == LCS_CONFIG_STALE);
    lcs_config_value_clear(&next);
    lcs_config_proposal_clear(&second_slot);
    lcs_config_proposal_clear(&replacement);
    lcs_config_value_clear(&a); lcs_config_value_clear(&b);
    cleanup();
}

static void test_concurrent_proposals(void)
{
    initialize();
    lcs_config_value_t a = value(1, "a", "192.0.2.2/24");
    lcs_config_value_t b = value(1, "b", "192.0.2.3/24");
    lcs_config_proposal_t old = {0}, newer = {0};
    assert(lcs_config_proposal_init(&old, &voters[0].state, 32, &a) == 0);
    assert(promise(&old, 0) == 0 && promise(&old, 1) == 0);
    assert(lcs_config_proposal_seal(&old, &voters[0].state));
    assert(lcs_config_proposal_init(&newer, &voters[1].state, 65, &b) == 0);
    assert(promise(&newer, 1) == 0 && promise(&newer, 2) == 0);
    assert(lcs_config_proposal_seal(&newer, &voters[0].state));
    assert(accept_proposal(&old, 0, &a) == 0);
    assert(accept_proposal(&old, 1, &a) == LCS_CONFIG_REJECTED);
    assert(!lcs_config_proposal_chosen(&old, &voters[0].state));
    assert(lcs_config_agreement_accept(&voters[0].state, old.parent_digest,
        32, &b, error, sizeof(error)) == LCS_CONFIG_REJECTED);
    assert(accept_proposal(&newer, 1, &b) == 0 && accept_proposal(&newer, 2, &b) == 0);
    assert(lcs_config_proposal_chosen(&newer, &voters[0].state));
    assert(lcs_config_agreement_learn(&voters[0].state, newer.parent_digest,
        65, 1, &b, error, sizeof(error)) == -1);
    assert(lcs_config_agreement_learn(&voters[0].state, newer.parent_digest,
        65, 0x80000003u, &b, error, sizeof(error)) == -1);
    for (size_t i = 0; i < 3; i++)
        assert(lcs_config_agreement_learn(&voters[i].state, newer.parent_digest,
            65, newer.acknowledgements, &b, error, sizeof(error)) == 0);
    assert(lcs_config_proposal_ack(&newer, &voters[0].state, 0, 65, &a) == -1);
    lcs_config_proposal_clear(&old); lcs_config_proposal_clear(&newer);
    lcs_config_value_clear(&a); lcs_config_value_clear(&b);
    cleanup();
}

static void test_failed_durable_write(void)
{
    initialize();
    lcs_config_agreement_t *state = &voters[0].state;
    char parent[65];
    snprintf(parent, sizeof(parent), "%s", state->committed.digest);
    fail_sync = 1;
    assert(lcs_config_agreement_prepare(state, 1, parent, 32, error, sizeof(error)) == -1);
    assert(state->poisoned && state->promised == 0);
    assert(lcs_config_agreement_prepare(state, 1, parent, 64, error, sizeof(error)) == -1);
    assert(lcs_config_agreement_next_ballot(state, 0) == 0);
    reopen(0);
    assert(!state->poisoned && state->promised == 0);
    assert(lcs_config_agreement_prepare(state, 1, parent, 64, error, sizeof(error)) == 0);
    lcs_config_value_t a = value(1, "a", "192.0.2.2/24");
    assert(!strcmp(a.digest,
        "f2faf60fa11e01e4f420474e363b0016b21743700ccc6cf813a992844d710208"));
    lcs_config_value_t b = value(1, "b", "192.0.2.3/24");
    fail_sync = 2; /* File replaced; fsync(directory) failed. */
    assert(lcs_config_agreement_accept(state, parent, 64, &a, error, sizeof(error)) == -1);
    assert(state->poisoned && state->accepted_ballot == 0);
    reopen(0);
    assert(state->accepted_ballot == 64 && lcs_config_value_equal(&state->accepted, &a));
    assert(lcs_config_agreement_accept(state, parent, 64, &b, error, sizeof(error)) == LCS_CONFIG_REJECTED);
    assert(lcs_config_agreement_prepare(state, 1, parent, 32, error, sizeof(error)) == LCS_CONFIG_REJECTED);
    assert(lcs_config_agreement_next_ballot(state, UINT64_MAX) == 0);
    assert(lcs_config_agreement_prepare(state, 1, parent, 31, error, sizeof(error)) == -1);
    assert(lcs_config_agreement_prepare(state, 1, parent, 35, error, sizeof(error)) == -1);
    assert(lcs_config_agreement_prepare(state, 2, parent, 96, error, sizeof(error)) == LCS_CONFIG_REJECTED);
    lcs_config_value_clear(&a); lcs_config_value_clear(&b);
    cleanup();
}

static void test_corrupt_and_foreign_journal(void)
{
    initialize();
    voter_t *voter = &voters[0];
    char *original;
    size_t length;
    assert(lcs_config_store_read(&voter->store, "agreement.json", &original,
        &length, LCS_CONFIG_JOURNAL_MAX, error, sizeof(error)) == 0);
    json_error_t parse_error;
    json_t *root = json_loadb(original, length, 0, &parse_error);
    assert(root);
    assert(json_object_set_new(root, "promised", json_string("32")) == 0);
    char *corrupt = json_dumps(root, JSON_COMPACT);
    assert(corrupt);
    lcs_config_agreement_close(&voter->state);
    assert(lcs_config_store_write(&voter->store, "agreement.json", corrupt,
        strlen(corrupt), error, sizeof(error)) == 0);
    assert(lcs_config_agreement_open(&voter->state, &voter->store,
        &voter->cfg, false, error, sizeof(error)) == -1);
    assert(strstr(error, "corrupt"));
    assert(lcs_config_store_write(&voter->store, "agreement.json", original,
        length, error, sizeof(error)) == 0);
    assert(lcs_config_agreement_open(&voter->state, &voter->store,
        &voters[1].cfg, false, error, sizeof(error)) == -1);
    voter->cfg.peer_timeout_ms++;
    assert(lcs_config_agreement_open(&voter->state, &voter->store,
        &voter->cfg, false, error, sizeof(error)) == -1);
    voter->cfg.peer_timeout_ms--;
    assert(lcs_config_agreement_open(&voter->state, &voter->store,
        &voter->cfg, false, error, sizeof(error)) == 0);
    free(original); free(corrupt); json_decref(root);
    cleanup();
}

static void test_duplicate_and_conflicting_replies(void)
{
    initialize();
    lcs_config_value_t a = value(1, "a", "192.0.2.2/24");
    lcs_config_value_t b = value(1, "b", "192.0.2.3/24");
    lcs_config_proposal_t proposal = {0};
    assert(lcs_config_proposal_init(&proposal, &voters[0].state, 64, &a) == 0);
    lcs_config_proposal_t wrong_owner = {0};
    assert(lcs_config_proposal_init(&wrong_owner, &voters[1].state, 64, &a) == -1);
    assert(lcs_config_proposal_promise(&proposal, &voters[0].state, 0, 1,
        proposal.parent_digest, 64, 32, &a) == 0);
    assert(lcs_config_proposal_promise(&proposal, &voters[0].state, 0, 1,
        proposal.parent_digest, 64, 32, &a) == LCS_CONFIG_REJECTED);
    assert(!lcs_config_proposal_seal(&proposal, &voters[0].state));
    assert(lcs_config_proposal_promise(&proposal, &voters[0].state, 1, 1,
        proposal.parent_digest, 64, 32, &b) == -1);
    assert(proposal.failed && !lcs_config_proposal_seal(&proposal, &voters[0].state));
    lcs_config_proposal_clear(&proposal);
    lcs_config_value_clear(&a); lcs_config_value_clear(&b);
    cleanup();
}

static int await_worker(lcs_config_agreement_worker_t *worker, lcs_config_agreement_t *state,
                         int *result)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int rc = lcs_config_agreement_worker_poll(worker, state, result, error, sizeof(error));
        if (rc)
            return rc;
        usleep(1000);
    }
    assert(!"storage worker timed out");
    return -1;
}

static lcs_config_value_t large_value(uint64_t revision, const char *request)
{
    lcs_config_t cfg = baseline;
    cfg.resource_count = 32;
    memset(cfg.resources, 0, sizeof(cfg.resources));
    for (size_t i = 0; i < 32; i++)
    {
        lcs_resource_config_t *resource = &cfg.resources[i];
        snprintf(resource->name, sizeof(resource->name), "vip%02zu", i);
        snprintf(resource->address, sizeof(resource->address), "192.0.2.%zu/32", i + 1);
        snprintf(resource->interface, sizeof(resource->interface), "eth0");
        resource->type = LCS_RESOURCE_VIP;
        char *hooks[] = {resource->pre_start, resource->post_start,
                         resource->pre_stop, resource->post_stop};
        for (size_t j = 0; j < 4; j++)
        {
            memset(hooks[j], 'x', LCS_PATH_MAX);
            hooks[j][0] = '/';
            hooks[j][LCS_PATH_MAX] = '\0';
        }
    }
    char *document;
    size_t length;
    assert(lcs_resource_config_encode(&cfg, revision, &document, &length, error, sizeof(error)) == 0);
    lcs_config_value_t result = {0};
    assert(lcs_config_value_init(&result, &baseline, request, document, length, error, sizeof(error)) == 0);
    free(document);
    return result;
}

static void test_background_durability(void)
{
    initialize();
    lcs_config_agreement_t *state = &voters[0].state;
    lcs_config_value_t first = large_value(1, "large-first");
    char parent[65];
    snprintf(parent, sizeof(parent), "%s", state->committed.digest);
    assert(lcs_config_agreement_accept(state, parent, 32, &first, error, sizeof(error)) == 0);
    assert(lcs_config_agreement_learn(state, parent, 32, 3, &first, error, sizeof(error)) == 0);
    lcs_config_value_t second = large_value(2, "large-second");
    lcs_config_agreement_worker_t worker;
    lcs_config_agreement_worker_init(&worker);
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_ACCEPT,
        .revision = 2, .ballot = 64, .value = &second };
    snprintf(job.parent_digest, sizeof(job.parent_digest), "%s", state->committed.digest);
    assert(lcs_config_agreement_worker_start(&worker, state, &job, error, sizeof(error)) == 0);
    assert(state->promised == 32 && !state->accepted.document);
    assert(lcs_config_agreement_worker_start(&worker, state, &job, error, sizeof(error)) == 1);
    int result = -999;
    assert(await_worker(&worker, state, &result) == 1 && result == 0);
    assert(state->promised == 64 && lcs_config_value_equal(&state->accepted, &second));
    char *record;
    size_t length;
    assert(lcs_config_agreement_export(state, &record, &length, error, sizeof(error)) == 0);
    assert(length > 65536); /* Result spans many nonblocking pipe reads. */
    char *older_record = record;
    size_t older_length = length;
    reopen(0);
    assert(lcs_config_value_equal(&state->accepted, &second));
    assert(lcs_config_agreement_prepare(state, 2, state->committed.digest, 96,
                                        error, sizeof(error)) == 0);
    assert(lcs_config_agreement_import(state, older_record, older_length, error, sizeof(error)) == -1);
    assert(state->poisoned && strstr(error, "newer durable state"));
    free(older_record);
    reopen(0);

    job.action = LCS_CONFIG_PREPARE;
    job.ballot = 128;
    job.value = NULL;
    assert(lcs_config_agreement_worker_start(&worker, state, &job, error, sizeof(error)) == 0);
    lcs_config_agreement_worker_cancel(&worker, state);
    assert(state->poisoned);
    if (worker.pid > 0)
        assert(await_worker(&worker, state, &result) == -1 && worker.pid == 0);
    reopen(0);
    assert(lcs_config_value_equal(&state->accepted, &second));
    job.ballot = 160;
    fail_sync = 1;
    assert(lcs_config_agreement_worker_start(&worker, state, &job, error, sizeof(error)) == 0);
    fail_sync = 0; /* The child inherited its independent injected failure. */
    assert(await_worker(&worker, state, &result) == -1 && state->poisoned);
    reopen(0);
    assert(!state->poisoned && lcs_config_value_equal(&state->accepted, &second));
    lcs_config_agreement_worker_cancel(&worker, state);
    lcs_config_value_clear(&first); lcs_config_value_clear(&second);
    cleanup();
}

static void test_snapshot_transfer(void)
{
    initialize();
    lcs_config_value_t original = large_value(1, "snapshot-transfer"), decoded = {0};
    lcs_config_transfer_metadata_t metadata = { .kind = LCS_CONFIG_TRANSFER_ACCEPT,
        .id = 14, .revision = 1, .ballot = 32, .length = (uint32_t)original.length };
    snprintf(metadata.membership, sizeof(metadata.membership), "%s", voters[0].state.membership);
    snprintf(metadata.parent_digest, sizeof(metadata.parent_digest), "%s", voters[0].state.committed.digest);
    snprintf(metadata.value_digest, sizeof(metadata.value_digest), "%s", original.digest);
    snprintf(metadata.request_id, sizeof(metadata.request_id), "%s", original.request_id);
    unsigned char begin[512], chunk[LCS_CONFIG_TRANSFER_CHUNK_MAX + 32];
    size_t begin_len, chunk_len;
    assert(lcs_config_transfer_encode_begin(&metadata, begin, sizeof(begin), &begin_len) == 0);
    lcs_config_transfer_metadata_t incoming;
    for (size_t i = 0; i < begin_len; i++)
        assert(lcs_config_transfer_decode_begin(begin, i, &incoming) == -1);
    assert(lcs_config_transfer_decode_begin(begin, begin_len, &incoming) == 0);
    begin[begin_len] = 0;
    assert(lcs_config_transfer_decode_begin(begin, begin_len + 1, &incoming) == -1);
    begin[begin_len - 1] = 'x'; /* Nonzero padding is not a second identity. */
    assert(lcs_config_transfer_decode_begin(begin, begin_len, &incoming) == -1);
    begin[begin_len - 1] = 0;
    begin[3] = 1;
    assert(lcs_config_transfer_decode_begin(begin, begin_len, &incoming) == -1);
    begin[3] = 0;
    lcs_config_transfer_t transfer = {0};
    assert(lcs_config_transfer_begin(&transfer, &metadata, metadata.membership, 100) == 0);
    assert(lcs_config_transfer_encode_chunk(metadata.id, 0, original.document, 37,
        chunk, sizeof(chunk), &chunk_len) == 0);
    assert(chunk_len <= LCS_MAX_FRAME);
    assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 101) == 0);
    assert(transfer.used == 37);
    uint64_t deadline = transfer.deadline_ms;
    assert(lcs_config_transfer_begin(&transfer, &metadata, metadata.membership, 200) == 0);
    assert(transfer.used == 37 && transfer.deadline_ms == deadline);
    assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 300) == 0);
    assert(transfer.used == 37 && transfer.deadline_ms == deadline);
    chunk[chunk_len - 1] ^= 1;
    assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 300) == -1);
    chunk[chunk_len - 1] ^= 1;
    incoming = metadata;
    incoming.id++;
    assert(lcs_config_transfer_begin(&transfer, &incoming, metadata.membership, 300) == 1);
    assert(lcs_config_transfer_encode_chunk(metadata.id + 1, 37, original.document + 37, 10,
        chunk, sizeof(chunk), &chunk_len) == 0);
    assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 300) == -1);
    assert(lcs_config_transfer_encode_chunk(metadata.id, 38, original.document + 38, 10,
        chunk, sizeof(chunk), &chunk_len) == 0);
    assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 300) == -1);
    assert(lcs_config_transfer_finish(&transfer, metadata.id, &baseline, 300,
        &decoded, error, sizeof(error)) == -1);
    assert(transfer.document && !decoded.document);
    for (size_t offset = 37; offset < original.length;)
    {
        size_t count = original.length - offset;
        if (count > LCS_CONFIG_TRANSFER_CHUNK_MAX)
            count = LCS_CONFIG_TRANSFER_CHUNK_MAX;
        assert(lcs_config_transfer_encode_chunk(metadata.id, (uint32_t)offset,
            original.document + offset, count, chunk, sizeof(chunk), &chunk_len) == 0);
        assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 400) == 0);
        offset += count;
    }
    assert(lcs_config_transfer_finish(&transfer, metadata.id + 1, &baseline, 401,
        &decoded, error, sizeof(error)) == -1);
    assert(lcs_config_transfer_finish(&transfer, metadata.id, &baseline, 401,
        &decoded, error, sizeof(error)) == 0);
    assert(lcs_config_value_equal(&decoded, &original) && !transfer.document);
    lcs_config_value_clear(&decoded);

    incoming = metadata;
    incoming.length = LCS_RESOURCE_DOCUMENT_MAX + 1;
    assert(lcs_config_transfer_begin(&transfer, &incoming, metadata.membership, 500) == -1);
    incoming.length = LCS_RESOURCE_DOCUMENT_MAX;
    assert(lcs_config_transfer_begin(&transfer, &incoming, metadata.membership, 500) == 0);
    assert(!lcs_config_transfer_expire(&transfer, 500 + LCS_CONFIG_TRANSFER_TIMEOUT_MS - 1));
    assert(lcs_config_transfer_expire(&transfer, 500 + LCS_CONFIG_TRANSFER_TIMEOUT_MS));
    assert(!transfer.document);
    assert(lcs_config_transfer_begin(&transfer, &metadata, metadata.membership, UINT64_MAX - 1) == -1);
    char other_membership[65];
    memset(other_membership, '0', 64); other_membership[64] = '\0';
    assert(lcs_config_transfer_begin(&transfer, &metadata, other_membership, 600) == -1);
    /* A valid JSON payload with a mismatched declared revision is not a value. */
    incoming = metadata;
    incoming.revision = 2;
    assert(lcs_config_transfer_begin(&transfer, &incoming, incoming.membership, 600) == 0);
    for (size_t offset = 0; offset < original.length;)
    {
        size_t count = original.length - offset;
        if (count > LCS_CONFIG_TRANSFER_CHUNK_MAX)
            count = LCS_CONFIG_TRANSFER_CHUNK_MAX;
        assert(lcs_config_transfer_encode_chunk(metadata.id, (uint32_t)offset,
            original.document + offset, count, chunk, sizeof(chunk), &chunk_len) == 0);
        assert(lcs_config_transfer_chunk(&transfer, chunk, chunk_len, 601) == 0);
        offset += count;
    }
    assert(lcs_config_transfer_finish(&transfer, metadata.id, &baseline, 602,
        &decoded, error, sizeof(error)) == -1);
    assert(!decoded.document && !transfer.document && strstr(error, "mismatch"));
    lcs_config_transfer_clear(&transfer);
    lcs_config_value_clear(&original);
    cleanup();
}

static uint32_t random_state = UINT32_C(0xc0dec0de);
static uint32_t next_random(void)
{
    random_state = random_state * UINT32_C(1664525) + UINT32_C(1013904223);
    return random_state;
}

static void test_reordered_rounds_and_restarts(void)
{
    initialize();
    lcs_config_value_t a = value(1, "schedule-a", "192.0.2.2/24");
    lcs_config_value_t b = value(1, "schedule-b", "192.0.2.3/24");
    lcs_config_value_t chosen = {0};
    char parent[65];
    snprintf(parent, sizeof(parent), "%s", voters[0].state.committed.digest);
    uint64_t observed = 0, chosen_ballot = 0;
    uint32_t chosen_votes = 0;
    bool learned_one = false;
    for (unsigned round = 0; round < 64; round++)
    {
        size_t coordinator = next_random() % 3;
        const lcs_config_value_t *wanted = next_random() & 1 ? &a : &b;
        uint64_t ballot = lcs_config_agreement_next_ballot(&voters[coordinator].state, observed);
        lcs_config_proposal_t proposal = {0};
        if (lcs_config_proposal_init(&proposal, &voters[coordinator].state, ballot, wanted))
            continue; /* A node that already learned no longer proposes slot 1. */
        observed = ballot;
        /* Persist the coordinator's own promise before any remote request. */
        assert(promise(&proposal, coordinator) == 0);
        uint32_t prepare_mask = ((next_random() >> 8) & 7) | (1u << coordinator);
        size_t first = next_random() % 3;
        for (size_t step = 0; step < 3; step++)
        {
            size_t i = (first + step) % 3;
            if (i != coordinator && (prepare_mask & (1u << i)))
                assert(promise(&proposal, i) >= 0);
            if ((next_random() & 3) == 0)
                reopen(i);
        }
        const lcs_config_value_t *selected = lcs_config_proposal_seal(&proposal, &voters[0].state);
        if (selected)
        {
            uint32_t accept_mask = (next_random() >> 8) & 7;
            for (size_t step = 0; step < 3; step++)
            {
                size_t i = (first + step) % 3;
                if (accept_mask & (1u << i))
                    assert(accept_proposal(&proposal, i, selected) >= 0);
                if ((next_random() & 3) == 0)
                    reopen(i);
            }
            if (lcs_config_proposal_chosen(&proposal, &voters[0].state))
            {
                if (!chosen.document)
                    assert(lcs_config_value_copy(&chosen, selected) == 0);
                assert(lcs_config_value_equal(&chosen, selected));
                chosen_ballot = ballot;
                chosen_votes = proposal.acknowledgements;
                if (!learned_one)
                {
                    size_t learner = next_random() % 3;
                    assert(lcs_config_agreement_learn(&voters[learner].state, parent,
                        ballot, chosen_votes, selected, error, sizeof(error)) == 0);
                    reopen(learner);
                    learned_one = true;
                }
            }
        }
        lcs_config_proposal_clear(&proposal);
    }
    assert(chosen.document && learned_one);
    for (size_t i = 0; i < 3; i++)
    {
        assert(lcs_config_agreement_learn(&voters[i].state, parent, chosen_ballot,
            chosen_votes, &chosen, error, sizeof(error)) == 0);
        reopen(i);
        assert(lcs_config_value_equal(&voters[i].state.committed, &chosen));
    }
    lcs_config_value_clear(&a); lcs_config_value_clear(&b); lcs_config_value_clear(&chosen);
    cleanup();
}

static void test_32_member_masks(void)
{
    initialize();
    lcs_config_t cfg = baseline;
    cfg.node_count = 32;
    snprintf(cfg.self_name, sizeof(cfg.self_name), "node00");
    for (size_t i = 0; i < cfg.node_count; i++)
    {
        snprintf(cfg.nodes[i].name, sizeof(cfg.nodes[i].name), "node%02zu", i);
        snprintf(cfg.nodes[i].address, sizeof(cfg.nodes[i].address), "127.0.0.1");
        cfg.nodes[i].port = 3322;
        cfg.nodes[i].role = i ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    assert(lcs_config_finalize(&cfg, error, sizeof(error)) == 0);
    char directory[] = "/tmp/lcs-agreement-mask.XXXXXX";
    assert(mkdtemp(directory));
    lcs_config_store_t store;
    lcs_config_agreement_t state;
    assert(lcs_config_store_open(&store, directory, error, sizeof(error)) == 0);
    assert(lcs_config_agreement_open(&state, &store, &cfg, true, error, sizeof(error)) == 0);
    lcs_config_value_t first = value(1, "mask-first", "192.0.2.2/24");
    char parent[65];
    snprintf(parent, sizeof(parent), "%s", state.committed.digest);
    assert(lcs_config_agreement_learn(&state, parent, 32, 0xffffu, &first,
                                      error, sizeof(error)) == -1);
    assert(lcs_config_agreement_learn(&state, parent, 32, UINT32_MAX - 1, &first,
                                      error, sizeof(error)) == -1); /* No full member. */
    assert(lcs_config_agreement_learn(&state, parent, 32, 0x1ffffu, &first,
                                      error, sizeof(error)) == 0);
    lcs_config_value_t second = value(2, "mask-second", "192.0.2.3/24");
    snprintf(parent, sizeof(parent), "%s", state.committed.digest);
    assert(lcs_config_agreement_learn(&state, parent, 95, UINT32_MAX, &second,
                                      error, sizeof(error)) == 0); /* Proposer index 31. */
    lcs_config_agreement_close(&state);
    assert(lcs_config_agreement_open(&state, &store, &cfg, false, error, sizeof(error)) == 0);
    assert(state.committed_voters == UINT32_MAX);
    lcs_config_agreement_close(&state);
    lcs_config_value_clear(&first); lcs_config_value_clear(&second);
    assert(unlinkat(store.directory_fd, "agreement.json", 0) == 0);
    assert(unlinkat(store.directory_fd, "lock", 0) == 0);
    lcs_config_store_close(&store);
    assert(rmdir(directory) == 0);
    cleanup();
}

int main(void)
{
    test_recovery_of_chosen_value();
    test_concurrent_proposals();
    test_failed_durable_write();
    test_corrupt_and_foreign_journal();
    test_duplicate_and_conflicting_replies();
    test_background_durability();
    test_snapshot_transfer();
    test_reordered_rounds_and_restarts();
    test_32_member_masks();
    puts("durable configuration agreement unit tests passed");
    return 0;
}
