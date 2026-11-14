// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_transaction.h"
#include "protocol.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

typedef struct
{
    lcs_config_store_t store;
    lcs_config_runtime_t runtime;
    lcs_config_readiness_t readiness;
    lcs_config_coordinator_t coordinator;
    lcs_config_transactions_t transactions;
    lcs_config_tx_control_t received;
    lcs_config_transfer_t promise;
    lcs_config_value_t recovered;
    char directory[80];
    unsigned controls;
    bool backpressure;
} node_t;
static node_t nodes[3];
static lcs_config_t config;
static char error[256];
static int stop_sync, fail_sync;
static pid_t parent;
static bool forbid_io;

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

static void setup(void)
{
    memset(nodes, 0, sizeof(nodes));
    lcs_config_init_defaults(&config);
    strcpy(config.cluster_name, "transaction-test");
    strcpy(config.self_name, "node0");
    config.node_count = 3;
    for (size_t i = 0; i < 3; i++)
    {
        snprintf(config.nodes[i].name, sizeof(config.nodes[i].name), "node%zu", i);
        strcpy(config.nodes[i].address, "127.0.0.1");
        config.nodes[i].port = 3322;
        config.nodes[i].role = i == 2 ? LCS_NODE_QUORUM_ONLY : LCS_NODE_FULL;
    }
    config.resource_count = 1;
    strcpy(config.resources[0].name, "vip");
    config.resources[0].type = LCS_RESOURCE_VIP;
    strcpy(config.resources[0].address, "192.0.2.10/32");
    strcpy(config.resources[0].interface, "eth0");
    assert(!lcs_config_finalize(&config, error, sizeof(error)));
    for (size_t i = 0; i < 3; i++)
    {
        node_t *node = &nodes[i];
        strcpy(node->directory, "/tmp/lcs-transaction.XXXXXX");
        assert(mkdtemp(node->directory));
        assert(!lcs_config_store_open(&node->store, node->directory, error, sizeof(error)));
        lcs_config_t *local = malloc(sizeof(*local));
        assert(local);
        *local = config;
        strcpy(local->self_name, config.nodes[i].name);
        node->runtime.base = malloc(sizeof(*node->runtime.base));
        assert(node->runtime.base);
        assert(!lcs_config_activation_open(&node->runtime.activation, &node->store, local,
                                           node->runtime.base, true, error, sizeof(error)));
        free(local);
        assert(!lcs_config_activation_resume(&node->runtime.activation, &node->runtime.agreement,
                                              &node->runtime.application, error, sizeof(error)));
        lcs_config_barrier_round_t round;
        assert(!lcs_config_barrier_begin(&round, &node->runtime.activation, &node->runtime.agreement,
                                          &node->runtime.application, error, sizeof(error)));
        for (size_t j = 0; j < 3; j++)
            assert(!lcs_config_barrier_ready(&round, j, round.membership, round.baseline, error, sizeof(error)));
        assert(!lcs_config_barrier_seal(&round, error, sizeof(error)));
        assert(!lcs_config_barrier_open(&node->runtime.barrier, &node->runtime.activation, &node->runtime.agreement,
                                         &node->runtime.application, &round, error, sizeof(error)));
        /* Explicit known-fresh history for proposal tests, not daemon policy. */
        assert(!lcs_config_requests_recover(&node->runtime.requests, &node->runtime.agreement,
                                              true, error, sizeof(error)));
        node->runtime.requires_recovery = true;
        lcs_config_coordinator_init(&node->coordinator, &node->runtime, &node->readiness);
        assert(!lcs_config_coordinator_recover(&node->coordinator));
        lcs_config_transactions_init(&node->transactions, &node->coordinator, 100 + i);
    }
}

static int receive(void *context, size_t peer, uint16_t type, const void *payload, size_t length)
{
    node_t *node = context;
    assert(peer == 0);
    if (node->backpressure)
        return 1;
    if (type == LCS_MSG_CONFIG_TX_CONTROL)
    {
        assert(!lcs_config_tx_decode(payload, length, &node->received));
        assert(node->received.target_instance == 100);
        node->controls++;
        return 0;
    }
    uint64_t sender, target;
    const void *body;
    size_t count;
    assert(!lcs_config_tx_unwrap(payload, length, &sender, &target, &body, &count));
    assert(target == 100 && sender == node->transactions.instance);
    if (type == LCS_MSG_CONFIG_TX_BEGIN)
    {
        lcs_config_transfer_metadata_t metadata;
        assert(!lcs_config_transfer_decode_begin(body, count, &metadata));
        assert(metadata.kind == LCS_CONFIG_TRANSFER_PROMISE && metadata.accepted_ballot);
        assert(!lcs_config_transfer_begin(&node->promise, &metadata, metadata.membership, 1000));
    }
    else if (type == LCS_MSG_CONFIG_TX_CHUNK)
        assert(!lcs_config_transfer_chunk(&node->promise, body, count, 1000));
    else
    {
        lcs_buf_reader_t reader;
        lcs_buf_reader_init(&reader, body, count);
        uint64_t id;
        assert(!lcs_buf_get_u64(&reader, &id) && reader.off == reader.len);
        assert(!lcs_config_transfer_finish(&node->promise, id, &config, 1000, &node->recovered, error, sizeof(error)));
    }
    return 0;
}

static void tick(node_t *node)
{
    (void)lcs_config_coordinator_process(&node->coordinator, 1000);
    lcs_config_transactions_process(&node->transactions, 1000, receive, node);
}

static void await_operation(node_t *node, uint64_t previous)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        tick(node);
        if ((node->coordinator.completed_votes > previous || node->coordinator.failed) && !node->transactions.active &&
            node->coordinator.phase != LCS_CONFIG_COORD_OBSERVING)
            return;
        usleep(1000);
    }
    assert(!"transaction did not complete");
}

static void cleanup(void)
{
    forbid_io = false; fail_sync = stop_sync = 0;
    for (size_t i = 0; i < 3; i++)
    {
        node_t *node = &nodes[i];
        lcs_config_transactions_close(&node->transactions);
        lcs_config_coordinator_stop(&node->coordinator);
        for (unsigned j = 0; j < 5000 && !lcs_config_coordinator_can_close(&node->coordinator); j++)
        { (void)lcs_config_coordinator_process(&node->coordinator, 1000); usleep(1000); }
        assert(lcs_config_coordinator_can_close(&node->coordinator));
        lcs_config_transfer_clear(&node->promise);
        lcs_config_value_clear(&node->recovered);
        lcs_config_runtime_close(&node->runtime);
        const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "requests.json", "lock"};
        for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++)
            assert(!unlinkat(node->store.directory_fd, names[j], 0));
        assert(!unlinkat(node->store.directory_fd, ".requests.json.tmp", 0) || errno == ENOENT);
        lcs_config_store_close(&node->store);
        assert(!rmdir(node->directory));
    }
}

static lcs_config_tx_control_t prepare(size_t destination, uint64_t id, uint64_t ballot)
{
    lcs_config_tx_control_t control = { .kind = LCS_CONFIG_TX_PREPARE, .sender_instance = 100,
        .target_instance = 100 + destination, .id = id, .revision = 1, .ballot = ballot };
    strcpy(control.membership, nodes[0].runtime.agreement.membership);
    strcpy(control.parent, nodes[0].runtime.agreement.committed.digest);
    return control;
}

static int send_control(size_t destination, const lcs_config_tx_control_t *control)
{
    unsigned char payload[256];
    size_t length;
    assert(!lcs_config_tx_encode(control, payload, sizeof(payload), &length));
    return lcs_config_transactions_receive(&nodes[destination].transactions, 0, 100,
                                            LCS_MSG_CONFIG_TX_CONTROL, payload, length, 1000);
}

static lcs_config_value_t candidate(const char *request)
{
    lcs_config_t *next = malloc(sizeof(*next));
    assert(next);
    *next = config;
    assert(!lcs_resource_config_assign_identities(&config, 0, &config, next, error, sizeof(error)));
    char *document;
    size_t length;
    assert(!lcs_resource_config_encode(next, 1, &document, &length, error, sizeof(error)));
    free(next);
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, &config, request, document, length, error, sizeof(error)));
    free(document);
    return value;
}

static int send_value(size_t destination, uint64_t id, lcs_config_transfer_kind_t kind,
                        uint32_t voters, const lcs_config_value_t *value, uint64_t ballot)
{
    node_t *node = &nodes[destination];
    lcs_config_transfer_metadata_t metadata = { .kind = kind, .id = id, .revision = value->revision,
        .ballot = ballot, .voters = voters, .length = (uint32_t)value->length };
    strcpy(metadata.membership, nodes[0].runtime.activation.membership);
    strcpy(metadata.parent_digest, nodes[0].runtime.activation.baseline.digest);
    strcpy(metadata.value_digest, value->digest);
    strcpy(metadata.request_id, value->request_id);
    unsigned char body[LCS_CONFIG_TRANSFER_CHUNK_MAX + 512], payload[LCS_CONFIG_TRANSFER_CHUNK_MAX + 550];
    size_t count, length;
    assert(!lcs_config_transfer_encode_begin(&metadata, body, sizeof(body), &count));
    assert(!lcs_config_tx_wrap(100, node->transactions.instance, body, count, payload, sizeof(payload), &length));
    int result = lcs_config_transactions_receive(&node->transactions, 0, 100, LCS_MSG_CONFIG_TX_BEGIN, payload, length, 1000);
    if (result || !node->transactions.peers[0].incoming.document)
        return result;
    for (size_t offset = 0; offset < value->length; )
    {
        size_t bytes = value->length - offset;
        if (bytes > LCS_CONFIG_TRANSFER_CHUNK_MAX)
            bytes = LCS_CONFIG_TRANSFER_CHUNK_MAX;
        assert(!lcs_config_transfer_encode_chunk(id, (uint32_t)offset, value->document + offset, bytes, body, sizeof(body), &count));
        assert(!lcs_config_tx_wrap(100, node->transactions.instance, body, count, payload, sizeof(payload), &length));
        assert(!lcs_config_transactions_receive(&node->transactions, 0, 100, LCS_MSG_CONFIG_TX_CHUNK, payload, length, 1000));
        offset += bytes;
    }
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, body, sizeof(body));
    assert(!lcs_buf_put_u64(&writer, id));
    assert(!lcs_config_tx_wrap(100, node->transactions.instance, body, writer.len, payload, sizeof(payload), &length));
    return lcs_config_transactions_receive(&node->transactions, 0, 100, LCS_MSG_CONFIG_TX_END, payload, length, 1000);
}

static void test_majority_and_recovery(void)
{
    setup();
    forbid_io = true;
    uint64_t ballot = lcs_config_agreement_next_ballot(&nodes[0].runtime.agreement, 0);
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_PREPARE, .revision = 1, .ballot = ballot };
    strcpy(job.parent_digest, nodes[0].runtime.agreement.committed.digest);
    assert(!lcs_config_coordinator_start_vote(&nodes[0].coordinator, &job, error, sizeof(error)));
    await_operation(&nodes[0], 0);
    lcs_config_value_t value = candidate("first");
    lcs_config_proposal_t proposal = {0};
    assert(!lcs_config_proposal_init(&proposal, &nodes[0].runtime.agreement, ballot, &value));
    assert(!lcs_config_proposal_promise(&proposal, &nodes[0].runtime.agreement, 0, 1, job.parent_digest, ballot, 0, NULL));
    for (size_t i = 1; i < 3; i++)
    {
        lcs_config_tx_control_t control = prepare(i, 1, ballot);
        assert(!send_control(i, &control));
        assert(!nodes[i].controls); /* No reply merely from queuing a storage job. */
        await_operation(&nodes[i], 0);
        assert(nodes[i].received.kind == LCS_CONFIG_TX_PROMISE && !nodes[i].received.result);
        assert(!lcs_config_proposal_promise(&proposal, &nodes[0].runtime.agreement, i, 1,
            nodes[i].received.parent, nodes[i].received.promised, nodes[i].received.accepted_ballot, NULL));
    }
    const lcs_config_value_t *selected = lcs_config_proposal_seal(&proposal, &nodes[0].runtime.agreement);
    assert(selected && lcs_config_value_equal(selected, &value));
    job.action = LCS_CONFIG_ACCEPT; job.value = selected;
    assert(!lcs_config_coordinator_start_vote(&nodes[0].coordinator, &job, error, sizeof(error)));
    await_operation(&nodes[0], 1);
    assert(!lcs_config_proposal_ack(&proposal, &nodes[0].runtime.agreement, 0, ballot, selected));
    assert(!lcs_config_proposal_chosen(&proposal, &nodes[0].runtime.agreement));
    for (size_t i = 1; i < 3; i++)
    {
        assert(!send_value(i, 2, LCS_CONFIG_TRANSFER_ACCEPT, 0, selected, ballot));
        await_operation(&nodes[i], 1);
        assert(nodes[i].received.kind == LCS_CONFIG_TX_ACK && !nodes[i].received.result);
        assert(!strcmp(nodes[i].received.value, selected->digest));
        assert(!lcs_config_proposal_ack(&proposal, &nodes[0].runtime.agreement, i, ballot, selected));
    }
    assert(lcs_config_proposal_chosen(&proposal, &nodes[0].runtime.agreement));
    /* A later proposer must receive the complete highest accepted snapshot. */
    lcs_config_tx_control_t retry = prepare(2, 3, ballot + 32);
    assert(!send_control(2, &retry));
    await_operation(&nodes[2], 2);
    for (unsigned i = 0; i < 20 && !nodes[2].recovered.document; i++)
        lcs_config_transactions_process(&nodes[2].transactions, 1000, receive, &nodes[2]);
    assert(nodes[2].received.accepted_ballot == ballot);
    assert(lcs_config_value_equal(&nodes[2].recovered, selected));
    job.action = LCS_CONFIG_PREPARE; job.value = NULL; job.ballot = ballot + 32;
    assert(!lcs_config_coordinator_start_vote(&nodes[0].coordinator, &job, error, sizeof(error)));
    await_operation(&nodes[0], 2);
    lcs_config_value_t alternative = candidate("second-request");
    lcs_config_proposal_t recovered_proposal = {0};
    assert(!lcs_config_proposal_init(&recovered_proposal, &nodes[0].runtime.agreement, ballot + 32, &alternative));
    assert(!lcs_config_proposal_promise(&recovered_proposal, &nodes[0].runtime.agreement, 0, 1,
        job.parent_digest, ballot + 32, ballot, &nodes[0].runtime.agreement.accepted));
    assert(!lcs_config_proposal_promise(&recovered_proposal, &nodes[0].runtime.agreement, 2, 1,
        nodes[2].received.parent, nodes[2].received.promised, nodes[2].received.accepted_ballot, &nodes[2].recovered));
    const lcs_config_value_t *recovered_value = lcs_config_proposal_seal(&recovered_proposal, &nodes[0].runtime.agreement);
    assert(recovered_value && lcs_config_value_equal(recovered_value, selected));
    assert(!lcs_config_value_equal(recovered_value, &alternative));
    lcs_config_proposal_clear(&recovered_proposal);
    uint64_t before_conflict = nodes[1].coordinator.completed_votes;
    assert(!send_value(1, 40, LCS_CONFIG_TRANSFER_ACCEPT, 0, &alternative, ballot));
    await_operation(&nodes[1], before_conflict);
    assert(nodes[1].received.result == LCS_CONFIG_TX_REJECTED && !nodes[1].runtime.agreement.poisoned);
    assert(lcs_config_value_equal(&nodes[1].runtime.agreement.accepted, selected));
    lcs_config_value_clear(&alternative);
    unsigned before = nodes[1].controls;
    assert(!send_value(1, 4, LCS_CONFIG_TRANSFER_LEARN, 1, selected, ballot));
    tick(&nodes[1]);
    assert(nodes[1].controls == before + 1 && nodes[1].received.result == LCS_CONFIG_TX_REJECTED);
    assert(nodes[1].runtime.agreement.committed.revision == 0);
    uint64_t before_learn = nodes[1].coordinator.completed_votes;
    assert(!send_value(1, 5, LCS_CONFIG_TRANSFER_LEARN, proposal.acknowledgements, selected, ballot));
    await_operation(&nodes[1], before_learn);
    assert(nodes[1].received.kind == LCS_CONFIG_TX_LEARNED && !nodes[1].received.result);
    assert(nodes[1].runtime.agreement.committed.revision == 1 && nodes[1].runtime.application.applied.revision == 0);
    assert(!lcs_config_coordinator_can_vote(&nodes[1].coordinator));
    before_learn = nodes[1].coordinator.completed_votes;
    assert(!send_value(1, 6, LCS_CONFIG_TRANSFER_LEARN, proposal.acknowledgements, selected, ballot));
    await_operation(&nodes[1], before_learn);
    assert(!nodes[1].received.result && nodes[1].runtime.application.applied.revision == 0);
    retry = prepare(1, 7, ballot + 32); retry.revision = 2;
    strcpy(retry.parent, selected->digest);
    assert(!send_control(1, &retry)); tick(&nodes[1]);
    assert(nodes[1].received.result == LCS_CONFIG_TX_NOT_READY);
    lcs_config_proposal_clear(&proposal);
    lcs_config_value_clear(&value);
    cleanup();
}

static void test_codecs_and_session_admission(void)
{
    setup();
    lcs_config_tx_control_t control = prepare(1, 1, 32), decoded;
    unsigned char payload[512]; size_t length;
    assert(!lcs_config_tx_encode(&control, payload, sizeof(payload), &length));
    assert(length == 255);
    for (size_t i = 0; i < length; i++)
        assert(lcs_config_tx_decode(payload, i, &decoded) == -1);
    assert(!lcs_config_tx_decode(payload, length, &decoded));
    payload[length] = 0;
    assert(lcs_config_tx_decode(payload, length + 1, &decoded) == -1);
    payload[length - 1] = 'x';
    assert(lcs_config_tx_decode(payload, length, &decoded) == -1);
    control.sender_instance++;
    assert(send_control(1, &control) == -1);
    control.sender_instance--; control.ballot++;
    assert(send_control(1, &control) == -1); /* Source node0 cannot propose node1's ballot. */
    cleanup();
}

static void await_stopped(node_t *node)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        int status;
        pid_t pid = waitpid(node->coordinator.storage.pid, &status, WNOHANG | WUNTRACED);
        if (pid > 0)
        { assert(WIFSTOPPED(status)); return; }
        assert(pid == 0 || errno == EINTR);
        usleep(1000);
    }
    assert(!"transaction writer did not reach sync boundary");
}

static void test_disconnect_and_uncertain_write(void)
{
    for (int mode = 0; mode < 2; mode++)
    {
        setup();
        lcs_config_tx_control_t control = prepare(1, 1, 32);
        fail_sync = mode ? 2 : 0;
        stop_sync = mode ? 0 : 1;
        assert(!send_control(1, &control));
        if (!mode)
        {
            await_stopped(&nodes[1]);
            tick(&nodes[1]);
            assert(!nodes[1].controls && !nodes[1].runtime.agreement.promised);
            lcs_config_transactions_disconnect(&nodes[1].transactions, 0);
            assert(!nodes[1].coordinator.storage.cancelled);
            assert(!kill(nodes[1].coordinator.storage.pid, SIGCONT));
            stop_sync = 0;
        }
        await_operation(&nodes[1], 0);
        fail_sync = 0;
        if (!mode)
            assert(!nodes[1].controls && nodes[1].runtime.agreement.promised == 32);
        else
        {
            assert(nodes[1].received.result == LCS_CONFIG_TX_UNKNOWN && nodes[1].runtime.agreement.poisoned);
            lcs_config_transactions_close(&nodes[1].transactions);
            lcs_config_coordinator_stop(&nodes[1].coordinator);
            assert(lcs_config_coordinator_can_close(&nodes[1].coordinator));
            lcs_config_runtime_close(&nodes[1].runtime);
            lcs_config_t *local = malloc(sizeof(*local));
            assert(local); *local = config; strcpy(local->self_name, "node1");
            assert(!lcs_config_runtime_open(&nodes[1].runtime, &nodes[1].store, local, error, sizeof(error)));
            free(local);
            assert(nodes[1].runtime.agreement.promised == 32 && nodes[1].runtime.requires_recovery);
        }
        cleanup();
    }
}

static void test_backpressure_and_partial_cleanup(void)
{
    setup();
    lcs_config_tx_control_t control = prepare(1, 1, 32);
    nodes[1].backpressure = true;
    assert(!send_control(1, &control));
    await_operation(&nodes[1], 0);
    assert(!nodes[1].controls && nodes[1].transactions.peers[0].stage);
    uint64_t deadline = nodes[1].transactions.peers[0].deadline_ms;
    lcs_config_transactions_process(&nodes[1].transactions, deadline - 1, receive, &nodes[1]);
    assert(nodes[1].transactions.peers[0].deadline_ms == deadline);
    lcs_config_transactions_process(&nodes[1].transactions, deadline, receive, &nodes[1]);
    assert(!nodes[1].transactions.peers[0].stage && nodes[1].runtime.agreement.promised == 32);
    nodes[1].backpressure = false;
    assert(!send_control(1, &control));
    await_operation(&nodes[1], 1);
    assert(nodes[1].controls == 1 && !nodes[1].received.result);
    lcs_config_value_t value = candidate("partial");
    lcs_config_transfer_metadata_t metadata = { .kind = LCS_CONFIG_TRANSFER_ACCEPT, .id = 2,
        .revision = 1, .ballot = 32, .length = (uint32_t)value.length };
    strcpy(metadata.membership, control.membership); strcpy(metadata.parent_digest, control.parent);
    strcpy(metadata.value_digest, value.digest); strcpy(metadata.request_id, value.request_id);
    unsigned char body[512], payload[550]; size_t count, length;
    assert(!lcs_config_transfer_encode_begin(&metadata, body, sizeof(body), &count));
    assert(!lcs_config_tx_wrap(100, 101, body, count, payload, sizeof(payload), &length));
    assert(!lcs_config_transactions_receive(&nodes[1].transactions, 0, 100, LCS_MSG_CONFIG_TX_BEGIN, payload, length, 1000));
    assert(nodes[1].transactions.peers[0].incoming.document && !nodes[1].runtime.agreement.accepted.document);
    lcs_config_transactions_process(&nodes[1].transactions, 11000, receive, &nodes[1]);
    assert(!nodes[1].transactions.peers[0].incoming.document && !nodes[1].coordinator.storage.pid);
    assert(!lcs_config_transactions_receive(&nodes[1].transactions, 0, 100, LCS_MSG_CONFIG_TX_BEGIN, payload, length, 12000));
    lcs_config_transactions_disconnect(&nodes[1].transactions, 0);
    assert(!nodes[1].transactions.peers[0].incoming.document);
    metadata.parent_digest[0] = metadata.parent_digest[0] == 'a' ? 'b' : 'a';
    assert(!lcs_config_transfer_encode_begin(&metadata, body, sizeof(body), &count));
    assert(!lcs_config_tx_wrap(100, 101, body, count, payload, sizeof(payload), &length));
    assert(!lcs_config_transactions_receive(&nodes[1].transactions, 0, 100, LCS_MSG_CONFIG_TX_BEGIN, payload, length, 12000));
    assert(!nodes[1].transactions.peers[0].incoming.document);
    lcs_config_transactions_process(&nodes[1].transactions, 12000, receive, &nodes[1]);
    assert(nodes[1].received.result == LCS_CONFIG_TX_REJECTED && !nodes[1].coordinator.storage.pid);
    lcs_config_value_clear(&value);
    cleanup();
}

typedef struct frame
{
    struct frame *next;
    size_t from, to, length;
    uint16_t type;
    unsigned char payload[];
} frame_t;
static frame_t *frames, **frame_tail = &frames;
static bool network_blocked, wrong_reply_id, duplicate_controls;
static unsigned network_errors;

static int network_send(void *context, size_t to, uint16_t type, const void *payload, size_t length)
{
    if (network_blocked)
        return 1;
    frame_t *frame = malloc(sizeof(*frame) + length);
    assert(frame);
    *frame = (frame_t){ .to = to, .type = type, .length = length };
    frame->from = (size_t)((node_t *)context - nodes);
    memcpy(frame->payload, payload, length);
    if (wrong_reply_id && frame->from && type == LCS_MSG_CONFIG_TX_CONTROL)
    {
        lcs_config_tx_control_t control;
        assert(!lcs_config_tx_decode(payload, length, &control));
        control.id++;
        assert(!lcs_config_tx_encode(&control, frame->payload, length, &frame->length));
    }
    *frame_tail = frame;
    frame_tail = &frame->next;
    if (duplicate_controls && type == LCS_MSG_CONFIG_TX_CONTROL)
    {
        frame_t *duplicate = malloc(sizeof(*duplicate) + length);
        assert(duplicate);
        memcpy(duplicate, frame, sizeof(*duplicate) + length);
        *frame_tail = duplicate;
        frame_tail = &duplicate->next;
    }
    return 0;
}

static void network_tick(uint64_t now)
{
    for (size_t i = 0; i < 3; i++)
    {
        (void)lcs_config_coordinator_process(&nodes[i].coordinator, now);
        lcs_config_transactions_process(&nodes[i].transactions, now, network_send, &nodes[i]);
    }
    while (frames)
    {
        frame_t *frame = frames;
        frames = frame->next;
        if (lcs_config_transactions_receive(&nodes[frame->to].transactions, frame->from,
              100 + frame->from, frame->type, frame->payload, frame->length, now))
        {
            fprintf(stderr, "network error from=%zu to=%zu type=%u proposer=%d output=%u incoming=%zu promise=%zu\n",
                frame->from, frame->to, frame->type, nodes[0].transactions.proposer.phase,
                nodes[frame->to].transactions.peers[frame->from].stage,
                nodes[frame->to].transactions.peers[frame->from].incoming.used,
                nodes[frame->to].transactions.proposer.peers[frame->from].incoming.used);
            network_errors++;
        }
        free(frame);
    }
    frame_tail = &frames;
    usleep(1000);
}

static void network_finish(uint64_t now)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        network_tick(now);
        if (nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_COMPLETE)
        {
            bool learned = nodes[0].coordinator.phase != LCS_CONFIG_COORD_OBSERVING;
            const lcs_config_value_t *selected = lcs_config_transactions_selected(&nodes[0].transactions);
            for (size_t j = 0; j < 3; j++)
                if (nodes[0].transactions.proposer.proposal.acknowledgements & (UINT32_C(1) << j))
                    learned = learned && nodes[j].coordinator.phase != LCS_CONFIG_COORD_OBSERVING &&
                        lcs_config_value_equal(&nodes[j].runtime.agreement.committed, selected);
            if (learned)
                return;
        }
        assert(nodes[0].transactions.proposer.phase != LCS_CONFIG_PROPOSER_UNKNOWN);
    }
    assert(!"network proposal timed out");
}

static void test_network_proposer(void)
{
    setup();
    network_errors = 0;
    uint64_t instances[LCS_MAX_NODES] = {100, 101, 102};
    lcs_config_value_t value = candidate("network-activation");
    forbid_io = true;
    duplicate_controls = true;
    uint64_t unavailable[LCS_MAX_NODES] = {100};
    assert(lcs_config_transactions_propose(&nodes[0].transactions, unavailable, 99, &value, 1000) < 0);
    assert(lcs_config_transactions_propose(&nodes[2].transactions, instances, 99, &value, 1000) < 0);
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 100, &value, 1000));
    assert(!frames); /* No wire request before local durable prepare. */
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 101, &value, 1000) < 0);
    network_finish(1000);
    assert(!network_errors);
    assert(lcs_config_proposal_chosen(&nodes[0].transactions.proposer.proposal, &nodes[0].runtime.agreement));
    for (size_t i = 0; i < 3; i++)
    {
        if (nodes[0].transactions.proposer.proposal.acknowledgements & (UINT32_C(1) << i))
            assert(lcs_config_value_equal(&nodes[i].runtime.agreement.committed, &value));
        assert(nodes[i].runtime.application.applied.revision == 0);
    }
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 100, &value, 1000) < 0);
    lcs_config_value_clear(&value);
    cleanup();
    duplicate_controls = false;

    /* Recover an earlier accepted value through real PROMISE snapshot frames. */
    setup();
    value = candidate("earlier-request");
    for (size_t i = 1; i < 3; i++)
    {
        assert(!lcs_config_agreement_prepare(&nodes[i].runtime.agreement, 1,
            nodes[i].runtime.agreement.committed.digest, 32, error, sizeof(error)));
        assert(!lcs_config_agreement_accept(&nodes[i].runtime.agreement,
            nodes[i].runtime.agreement.committed.digest, 32, &value, error, sizeof(error)));
    }
    lcs_config_value_t newer = candidate("new-request");
    /* Force recovery from node1's complete snapshot, with no local accepted value. */
    instances[2] = 0;
    forbid_io = true;
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 200, &newer, 1000));
    network_finish(1000);
    assert(!network_errors);
    assert(!strcmp(lcs_config_transactions_selected(&nodes[0].transactions)->request_id, "earlier-request"));
    const lcs_config_request_record_t *intent = lcs_config_requests_find(&nodes[0].runtime.requests, "new-request");
    assert(intent && !intent->committed && !strcmp(intent->value_digest, newer.digest));
    const lcs_config_request_record_t *recovered_intent = lcs_config_requests_find(&nodes[0].runtime.requests, "earlier-request");
    assert(recovered_intent && recovered_intent->committed && !strcmp(recovered_intent->value_digest, value.digest));
    lcs_config_value_clear(&value);
    lcs_config_value_clear(&newer);
    cleanup();

    /* Wrong request IDs cannot add promises; retries require a new ID/ballot. */
    setup();
    value = candidate("timeout-request");
    forbid_io = true;
    wrong_reply_id = true;
    instances[2] = 102;
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 300, &value, 1000));
    for (unsigned i = 0; i < 5000; i++)
    {
        network_tick(1000);
        if (nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_PROMISES &&
            nodes[1].runtime.agreement.promised && nodes[2].runtime.agreement.promised)
            break;
    }
    assert(nodes[0].runtime.agreement.promised);
    assert(nodes[0].transactions.proposer.proposal.promises == 1);
    assert(!nodes[0].runtime.agreement.accepted.document);
    uint64_t first_ballot = nodes[0].runtime.agreement.promised;
    lcs_config_tx_control_t rejection = prepare(1, 301, first_ballot);
    rejection.kind = LCS_CONFIG_TX_PROMISE; rejection.result = LCS_CONFIG_TX_REJECTED;
    rejection.sender_instance = 101; rejection.target_instance = 100; rejection.promised = (77u << 5) | 1;
    unsigned char rejection_wire[LCS_CONFIG_TX_CONTROL_MAX]; size_t rejection_length;
    assert(!lcs_config_tx_encode(&rejection, rejection_wire, sizeof(rejection_wire), &rejection_length));
    assert(!lcs_config_transactions_receive(&nodes[0].transactions, 1, 101, LCS_MSG_CONFIG_TX_CONTROL,
        rejection_wire, rejection_length, 1000));
    assert(!nodes[0].transactions.proposer.observed_ballot); /* Wrong attempt cannot raise even a hint. */
    rejection.id = 300;
    assert(!lcs_config_tx_encode(&rejection, rejection_wire, sizeof(rejection_wire), &rejection_length));
    assert(!lcs_config_transactions_receive(&nodes[0].transactions, 1, 101, LCS_MSG_CONFIG_TX_CONTROL,
        rejection_wire, rejection_length, 1000));
    assert(nodes[0].transactions.proposer.observed_ballot == rejection.promised &&
        nodes[0].transactions.proposer.proposal.promises == 1 && nodes[0].runtime.agreement.promised == first_ballot);
    network_tick(11000);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN);
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 300, &value, 12000) < 0);
    wrong_reply_id = false;
    network_blocked = false;
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 301, &value, 12000) < 0);
    lcs_config_value_clear(&value); value = candidate("timeout-new-request");
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 301, &value, 12000));
    assert(nodes[0].transactions.proposer.proposal.ballot > rejection.promised);
    network_finish(12000);
    assert(!network_errors);
    lcs_config_value_clear(&value);
    cleanup();

    /* Notification backpressure cannot turn an already learned decision unknown. */
    setup();
    value = candidate("known-decision");
    forbid_io = true;
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 400, &value, 1000));
    for (unsigned i = 0; i < 5000 &&
         nodes[0].transactions.proposer.phase != LCS_CONFIG_PROPOSER_DISTRIBUTING; i++)
        network_tick(1000);
    assert(nodes[0].runtime.agreement.committed.revision == 1);
    network_blocked = true;
    network_tick(11000);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_COMPLETE);
    assert(lcs_config_value_equal(&nodes[0].runtime.agreement.committed, &value));
    network_blocked = false;
    lcs_config_value_clear(&value);
    cleanup();

    /* Uncertain local persistence never sends a positive network request. */
    setup();
    value = candidate("uncertain-proposer");
    forbid_io = true;
    fail_sync = 2;
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 500, &value, 1000));
    fail_sync = 0;
    for (unsigned i = 0; i < 5000 &&
         nodes[0].transactions.proposer.phase != LCS_CONFIG_PROPOSER_UNKNOWN; i++)
        network_tick(1000);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN);
    assert(nodes[0].coordinator.failed && !nodes[1].runtime.agreement.promised && !nodes[2].runtime.agreement.promised);
    assert(!network_errors);
    lcs_config_value_clear(&value);
    cleanup();

}

static void test_proposal_reservation_order_and_timeout(void)
{
    for (unsigned mode = 0; mode < 3; mode++)
    {
        setup(); network_errors = 0;
        uint64_t instances[LCS_MAX_NODES] = {100, 101, 102};
        lcs_config_value_t value = candidate("reservation-order");
        forbid_io = true; stop_sync = 2;
        const char descriptor[] = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        assert(!lcs_config_transactions_propose_client(&nodes[0].transactions, instances, 600, &value, descriptor, 1000));
        await_stopped(&nodes[0]); stop_sync = 0;
        assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_RESERVING);
        assert(!nodes[0].runtime.requests.count && !nodes[0].runtime.agreement.promised && !frames);
        assert(!nodes[0].coordinator.completed_requests && !nodes[0].coordinator.completed_votes);
        assert(!nodes[1].runtime.agreement.promised && !nodes[2].runtime.agreement.promised);
        if (mode == 2)
        {
            lcs_config_transactions_close(&nodes[0].transactions);
            lcs_config_coordinator_stop(&nodes[0].coordinator);
            for (unsigned i = 0; i < 5000 && !lcs_config_coordinator_can_close(&nodes[0].coordinator); i++)
            { (void)lcs_config_coordinator_process(&nodes[0].coordinator, 1000); usleep(1000); }
            assert(lcs_config_coordinator_can_close(&nodes[0].coordinator));
            assert(nodes[0].runtime.requests.poisoned && nodes[0].runtime.agreement.poisoned);
            assert(!nodes[0].coordinator.completed_requests && !frames);
            forbid_io = false;
            lcs_config_runtime_close(&nodes[0].runtime);
            assert(!lcs_config_runtime_open(&nodes[0].runtime, &nodes[0].store, &config, error, sizeof(error)));
            assert(lcs_config_requests_find(&nodes[0].runtime.requests, value.request_id));
            assert(!strcmp(lcs_config_requests_find(&nodes[0].runtime.requests, value.request_id)->client_digest, descriptor));
            lcs_config_coordinator_init(&nodes[0].coordinator, &nodes[0].runtime, &nodes[0].readiness);
            assert(!lcs_config_coordinator_recover(&nodes[0].coordinator));
            lcs_config_transactions_init(&nodes[0].transactions, &nodes[0].coordinator, 100);
            forbid_io = true;
            assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 601, &value, 12000) < 0);
            assert(!nodes[0].runtime.agreement.promised && !frames);
        }
        else
        {
            if (mode == 1)
            {
                network_tick(11000);
                assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN && !frames);
            }
            assert(!kill(nodes[0].coordinator.storage.pid, SIGCONT));
            if (!mode) network_finish(1000);
            else
            {
                for (unsigned i = 0; i < 5000 && nodes[0].coordinator.storage.pid; i++) network_tick(11000);
                assert(!nodes[0].coordinator.storage.pid && !nodes[0].runtime.agreement.promised && !frames);
                assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN);
                assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 601, &value, 12000) < 0);
            }
            const lcs_config_request_record_t *record = lcs_config_requests_find(&nodes[0].runtime.requests, value.request_id);
            assert(record && !strcmp(record->value_digest, value.digest) && record->committed == !mode);
            assert(!strcmp(record->client_digest, descriptor));
            assert(nodes[0].coordinator.completed_requests == (mode ? 1u : 3u) && !network_errors);
        }
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_proposal_history_admission(void)
{
    setup();
    uint64_t instances[LCS_MAX_NODES] = {100, 101, 102};
    lcs_config_value_t value = candidate("history-required");
    lcs_config_requests_t saved = nodes[0].runtime.requests;
    lcs_config_requests_close(&nodes[0].runtime.requests);
    forbid_io = true;
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000) < 0);
    assert(!nodes[0].coordinator.storage.pid && !frames);
    nodes[0].runtime.requests = saved; nodes[0].runtime.requests.poisoned = true;
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000) < 0);
    nodes[0].runtime.requests.poisoned = false;
    lcs_config_value_clear(&value); value = candidate("bootstrap");
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000) < 0);
    forbid_io = false; lcs_config_value_clear(&value); value = candidate("already-accepted");
    assert(!lcs_config_agreement_accept(&nodes[0].runtime.agreement,
        nodes[0].runtime.agreement.committed.digest, 32, &value, error, sizeof(error)));
    forbid_io = true;
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000) < 0);
    assert(!nodes[0].coordinator.storage.pid && !frames);
    lcs_config_value_clear(&value); cleanup();

    setup();
    for (unsigned i = 0; i < LCS_CONFIG_REQUEST_HISTORY_MAX; i++)
    {
        char id[32]; snprintf(id, sizeof(id), "full-%u", i); value = candidate(id);
        assert(!lcs_config_requests_reserve(&nodes[0].runtime.requests, &nodes[0].runtime.agreement, 0,
            nodes[0].runtime.agreement.committed.digest, &value, error, sizeof(error)));
        lcs_config_value_clear(&value);
    }
    value = candidate("full-0"); forbid_io = true;
    assert(lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000) < 0);
    lcs_config_value_clear(&value); value = candidate("over-capacity");
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 700, &value, 1000));
    for (unsigned i = 0; i < 5000 && nodes[0].transactions.proposer.phase != LCS_CONFIG_PROPOSER_UNKNOWN; i++) network_tick(1000);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_UNKNOWN);
    assert(!nodes[0].runtime.agreement.promised && !frames && !nodes[0].coordinator.failed);
    assert(nodes[0].coordinator.completed_requests == 1 && nodes[0].coordinator.last_request_result == LCS_CONFIG_REQUEST_CAPACITY);
    assert(nodes[0].runtime.requests.count == LCS_CONFIG_REQUEST_HISTORY_MAX);
    lcs_config_value_clear(&value); cleanup();
}

static void test_accept_identity_durability(void)
{
    for (unsigned stage = 0; stage < 2; stage++) for (int boundary = 1; boundary <= 2; boundary++)
    {
        setup();
        lcs_config_tx_control_t control = prepare(1, 1, 32);
        assert(!send_control(1, &control)); await_operation(&nodes[1], 0);
        lcs_config_value_t value = candidate("accepted-identity");
        forbid_io = true; stop_sync = stage ? 0 : boundary;
        assert(!send_value(1, 2, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        if (stage)
        {
            stop_sync = boundary;
            for (unsigned i = 0; i < 5000 && nodes[1].coordinator.phase == LCS_CONFIG_COORD_ACCEPT_RESERVING; i++)
            { tick(&nodes[1]); usleep(1000); }
            assert(nodes[1].coordinator.phase == LCS_CONFIG_COORD_VOTING);
        }
        await_stopped(&nodes[1]); stop_sync = 0;
        assert(nodes[1].controls == 1 && nodes[1].transactions.active && !nodes[1].runtime.agreement.accepted.document);
        assert(nodes[1].coordinator.completed_votes == 1 && nodes[1].coordinator.completed_requests == stage);
        assert(nodes[1].runtime.requests.count == stage);
        assert(!lcs_config_coordinator_can_vote(&nodes[1].coordinator));
        assert(!kill(nodes[1].coordinator.storage.pid, SIGCONT));
        await_operation(&nodes[1], 1);
        assert(nodes[1].received.kind == LCS_CONFIG_TX_ACK && nodes[1].received.result == LCS_CONFIG_TX_OK);
        assert(lcs_config_value_equal(&nodes[1].runtime.agreement.accepted, &value));
        const lcs_config_request_record_t *record = lcs_config_requests_find(&nodes[1].runtime.requests, value.request_id);
        assert(record && !record->committed && !strcmp(record->value_digest, value.digest));
        assert(!nodes[1].coordinator.accepting.document && nodes[1].coordinator.completed_requests == 1);
        assert(!send_value(1, 3, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        await_operation(&nodes[1], 2);
        assert(nodes[1].received.result == LCS_CONFIG_TX_OK && nodes[1].runtime.requests.count == 1);
        assert(nodes[1].coordinator.completed_requests == 2 && !nodes[1].coordinator.accepting.document);
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_accept_identity_failures(void)
{
    for (unsigned stage = 0; stage < 2; stage++) for (int boundary = 1; boundary <= 2; boundary++)
    {
        setup();
        lcs_config_tx_control_t control = prepare(1, 1, 32);
        assert(!send_control(1, &control)); await_operation(&nodes[1], 0);
        lcs_config_value_t value = candidate("failed-identity");
        forbid_io = true; fail_sync = stage ? 0 : boundary;
        assert(!send_value(1, 2, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        if (stage)
        {
            fail_sync = boundary;
            for (unsigned i = 0; i < 5000 && nodes[1].coordinator.phase == LCS_CONFIG_COORD_ACCEPT_RESERVING; i++)
            { tick(&nodes[1]); usleep(1000); }
        }
        fail_sync = 0;
        await_operation(&nodes[1], 1);
        assert(nodes[1].received.result == LCS_CONFIG_TX_UNKNOWN && nodes[1].coordinator.failed);
        assert(nodes[1].runtime.agreement.poisoned && !nodes[1].runtime.agreement.accepted.document);
        assert(!nodes[1].coordinator.accepting.document && nodes[1].coordinator.completed_votes == 1);
        assert(nodes[1].coordinator.completed_requests == stage);
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_accept_history_conflicts_and_capacity(void)
{
    for (unsigned mode = 0; mode < 3; mode++)
    {
        setup();
        lcs_config_value_t value = candidate("incoming-identity");
        if (mode == 0)
        {
            lcs_config_t *bad = malloc(sizeof(*bad)); assert(bad); uint64_t revision;
            assert(!lcs_resource_config_decode(value.document, value.length, &config, bad, &revision, error, sizeof(error)));
            strcpy(bad->resources[0].address, "192.0.2.99/32");
            char *document; size_t length;
            assert(!lcs_resource_config_encode(bad, revision, &document, &length, error, sizeof(error))); free(bad);
            lcs_config_value_t conflict = {0};
            assert(!lcs_config_value_init(&conflict, &config, value.request_id, document, length, error, sizeof(error))); free(document);
            assert(!lcs_config_requests_reserve(&nodes[1].runtime.requests, &nodes[1].runtime.agreement, 0,
                nodes[1].runtime.agreement.committed.digest, &conflict, error, sizeof(error)));
            lcs_config_value_clear(&conflict);
        }
        if (mode == 1) for (unsigned i = 0; i < LCS_CONFIG_REQUEST_HISTORY_MAX; i++)
        {
            char id[32]; snprintf(id, sizeof(id), "full-%u", i); lcs_config_value_t full = candidate(id);
            assert(!lcs_config_requests_reserve(&nodes[1].runtime.requests, &nodes[1].runtime.agreement, 0,
                nodes[1].runtime.agreement.committed.digest, &full, error, sizeof(error)));
            lcs_config_value_clear(&full);
        }
        forbid_io = true;
        lcs_config_tx_control_t control = prepare(1, 1, mode == 2 ? 64 : 32);
        assert(!send_control(1, &control)); await_operation(&nodes[1], 0);
        assert(!send_value(1, 2, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        await_operation(&nodes[1], 1);
        assert(nodes[1].received.result == LCS_CONFIG_TX_REJECTED && !nodes[1].coordinator.failed);
        assert(!nodes[1].runtime.agreement.accepted.document && !nodes[1].coordinator.accepting.document);
        assert(nodes[1].coordinator.completed_requests == (mode == 2 ? 0u : 1u));
        assert(nodes[1].runtime.requests.count == (mode == 0 ? 1u : mode == 1 ? LCS_CONFIG_REQUEST_HISTORY_MAX : 0u));
        if (mode == 1)
        {
            lcs_config_value_clear(&value); value = candidate("full-0");
            assert(!send_value(1, 3, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
            await_operation(&nodes[1], 2);
            assert(nodes[1].received.result == LCS_CONFIG_TX_OK);
            assert(nodes[1].runtime.requests.count == LCS_CONFIG_REQUEST_HISTORY_MAX);
        }
        lcs_config_value_clear(&value); cleanup();
    }
}

static void test_accept_cancellation_recovery(void)
{
    for (unsigned stage = 0; stage < 2; stage++)
    {
        setup();
        lcs_config_tx_control_t control = prepare(1, 1, 32);
        assert(!send_control(1, &control)); await_operation(&nodes[1], 0);
        lcs_config_value_t value = candidate("cancelled-accept");
        forbid_io = true; stop_sync = stage ? 0 : 2;
        assert(!send_value(1, 2, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        if (stage)
        {
            stop_sync = 2;
            for (unsigned i = 0; i < 5000 && nodes[1].coordinator.phase == LCS_CONFIG_COORD_ACCEPT_RESERVING; i++)
            { tick(&nodes[1]); usleep(1000); }
        }
        await_stopped(&nodes[1]); stop_sync = 0;
        lcs_config_transactions_close(&nodes[1].transactions);
        lcs_config_coordinator_stop(&nodes[1].coordinator);
        assert(!nodes[1].coordinator.accepting.document);
        for (unsigned i = 0; i < 5000 && !lcs_config_coordinator_can_close(&nodes[1].coordinator); i++)
        { (void)lcs_config_coordinator_process(&nodes[1].coordinator, 1000); usleep(1000); }
        assert(lcs_config_coordinator_can_close(&nodes[1].coordinator) && nodes[1].controls == 1);
        assert(nodes[1].runtime.agreement.poisoned && nodes[1].coordinator.completed_votes == 1);
        forbid_io = false;
        lcs_config_runtime_close(&nodes[1].runtime);
        lcs_config_t *local = malloc(sizeof(*local)); assert(local); *local = config;
        strcpy(local->self_name, "node1");
        assert(!lcs_config_runtime_open(&nodes[1].runtime, &nodes[1].store, local, error, sizeof(error))); free(local);
        assert(nodes[1].runtime.requests.count == 1);
        assert(!!nodes[1].runtime.agreement.accepted.document == !!stage);
        lcs_config_coordinator_init(&nodes[1].coordinator, &nodes[1].runtime, &nodes[1].readiness);
        assert(!lcs_config_coordinator_recover(&nodes[1].coordinator));
        lcs_config_transactions_init(&nodes[1].transactions, &nodes[1].coordinator, 101);
        forbid_io = true;
        assert(!send_value(1, 3, LCS_CONFIG_TRANSFER_ACCEPT, 0, &value, 32));
        await_operation(&nodes[1], 0);
        assert(nodes[1].received.result == LCS_CONFIG_TX_OK && nodes[1].runtime.requests.count == 1);
        assert(lcs_config_value_equal(&nodes[1].runtime.agreement.accepted, &value));
        lcs_config_value_clear(&value); cleanup();
    }
}

static void until_observing(node_t *node)
{
    for (unsigned i = 0; i < 5000 && node->coordinator.phase != LCS_CONFIG_COORD_OBSERVING; i++)
    { assert(lcs_config_coordinator_process(&node->coordinator, 1000) >= 0); usleep(1000); }
    assert(node->coordinator.phase == LCS_CONFIG_COORD_OBSERVING);
    assert(node->coordinator.completed_votes == 1 && node->coordinator.last_vote_result == LCS_CONFIG_AGREED);
}
static void until_history_finished(node_t *node)
{
    for (unsigned i = 0; i < 5000 && node->coordinator.phase == LCS_CONFIG_COORD_OBSERVING; i++)
    { (void)lcs_config_coordinator_process(&node->coordinator, 1000); usleep(1000); }
    assert(node->coordinator.phase != LCS_CONFIG_COORD_OBSERVING && !node->coordinator.storage.pid);
}
static void test_learned_observation_order(void)
{
    for (int boundary = 1; boundary <= 2; boundary++)
    {
        setup(); lcs_config_value_t value = candidate("observed-decision");
        forbid_io = true;
        assert(!send_value(1, 1, LCS_CONFIG_TRANSFER_LEARN, 3, &value, 32));
        stop_sync = boundary; until_observing(&nodes[1]); await_stopped(&nodes[1]); stop_sync = 0;
        assert(lcs_config_value_equal(&nodes[1].runtime.agreement.committed, &value));
        assert(!nodes[1].runtime.requests.count && !nodes[1].coordinator.completed_requests);
        assert(!nodes[1].controls && !lcs_config_coordinator_can_vote(&nodes[1].coordinator));
        lcs_config_transactions_process(&nodes[1].transactions, 1000, receive, &nodes[1]);
        assert(nodes[1].controls == 1 && nodes[1].received.result == LCS_CONFIG_TX_OK);
        assert(nodes[1].received.kind == LCS_CONFIG_TX_LEARNED && !nodes[1].runtime.requests.count);
        assert(!kill(nodes[1].coordinator.storage.pid, SIGCONT)); until_history_finished(&nodes[1]);
        assert(nodes[1].runtime.requests.count == 1 && nodes[1].runtime.requests.records[0].committed);
        assert(nodes[1].coordinator.completed_votes == 1 && nodes[1].coordinator.completed_requests == 1);
        assert(nodes[1].runtime.application.applied.revision == 0);
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_learned_observation_failures(void)
{
    for (unsigned cancelled = 0; cancelled < 2; cancelled++) for (int boundary = 1; boundary <= 2; boundary++)
    {
        setup(); lcs_config_value_t value = candidate("observation-failure");
        forbid_io = true;
        assert(!send_value(1, 1, LCS_CONFIG_TRANSFER_LEARN, 3, &value, 32));
        if (cancelled) stop_sync = boundary;
        else fail_sync = boundary;
        until_observing(&nodes[1]); fail_sync = stop_sync = 0;
        if (cancelled)
        {
            await_stopped(&nodes[1]); lcs_config_coordinator_stop(&nodes[1].coordinator);
            for (unsigned i = 0; i < 5000 && !lcs_config_coordinator_can_close(&nodes[1].coordinator); i++)
            { (void)lcs_config_coordinator_process(&nodes[1].coordinator, 1000); usleep(1000); }
            assert(lcs_config_coordinator_can_close(&nodes[1].coordinator));
        }
        else { until_history_finished(&nodes[1]); assert(nodes[1].coordinator.failed); }
        assert(nodes[1].runtime.requests.poisoned);
        assert(lcs_config_value_equal(&nodes[1].runtime.agreement.committed, &value));
        assert(!nodes[1].coordinator.completed_requests && !nodes[1].runtime.requests.count);
        /* Deliberately delay transport completion until AFTER bookkeeping fails. */
        lcs_config_transactions_process(&nodes[1].transactions, 1000, receive, &nodes[1]);
        assert(nodes[1].received.result == LCS_CONFIG_TX_OK && nodes[1].controls == 1);
        assert(!nodes[1].transactions.active && !lcs_config_coordinator_can_vote(&nodes[1].coordinator));
        forbid_io = false; lcs_config_transactions_close(&nodes[1].transactions);
        lcs_config_coordinator_stop(&nodes[1].coordinator); lcs_config_runtime_close(&nodes[1].runtime);
        lcs_config_t *local = malloc(sizeof(*local)); assert(local); *local = config; strcpy(local->self_name, "node1");
        assert(!lcs_config_runtime_open(&nodes[1].runtime, &nodes[1].store, local, error, sizeof(error))); free(local);
        assert(nodes[1].runtime.requests.count == (boundary == 2 ? 1u : 0u));
        lcs_config_coordinator_init(&nodes[1].coordinator, &nodes[1].runtime, &nodes[1].readiness);
        assert(!lcs_config_coordinator_recover(&nodes[1].coordinator));
        lcs_config_transactions_init(&nodes[1].transactions, &nodes[1].coordinator, 101);
        forbid_io = true;
        assert(!send_value(1, 2, LCS_CONFIG_TRANSFER_LEARN, 3, &value, 32)); await_operation(&nodes[1], 0);
        assert(nodes[1].received.result == LCS_CONFIG_TX_OK && nodes[1].runtime.requests.records[0].committed);
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_learned_observation_rejections(void)
{
    for (unsigned mode = 0; mode < 2; mode++)
    {
        setup(); lcs_config_value_t value = candidate("learned-identity");
        if (!mode)
        {
            lcs_config_t *bad = malloc(sizeof(*bad)); assert(bad); uint64_t revision;
            assert(!lcs_resource_config_decode(value.document, value.length, &config, bad, &revision, error, sizeof(error)));
            strcpy(bad->resources[0].address, "192.0.2.99/32"); char *document; size_t length;
            assert(!lcs_resource_config_encode(bad, revision, &document, &length, error, sizeof(error))); free(bad);
            lcs_config_value_t conflict = {0};
            assert(!lcs_config_value_init(&conflict, &config, value.request_id, document, length, error, sizeof(error))); free(document);
            assert(!lcs_config_requests_reserve(&nodes[1].runtime.requests, &nodes[1].runtime.agreement, 0,
                nodes[1].runtime.agreement.committed.digest, &conflict, error, sizeof(error)));
            lcs_config_value_clear(&conflict);
        }
        else for (unsigned i = 0; i < LCS_CONFIG_REQUEST_HISTORY_MAX; i++)
        {
            char id[32]; snprintf(id, sizeof(id), "full-%u", i); lcs_config_value_t full = candidate(id);
            assert(!lcs_config_requests_reserve(&nodes[1].runtime.requests, &nodes[1].runtime.agreement, 0,
                nodes[1].runtime.agreement.committed.digest, &full, error, sizeof(error)));
            lcs_config_value_clear(&full);
        }
        forbid_io = true;
        assert(!send_value(1, 1, LCS_CONFIG_TRANSFER_LEARN, 3, &value, 32));
        until_observing(&nodes[1]); until_history_finished(&nodes[1]);
        assert(nodes[1].coordinator.failed && nodes[1].runtime.requests.poisoned);
        assert(nodes[1].coordinator.completed_votes == 1 && nodes[1].coordinator.completed_requests == 1);
        assert(nodes[1].coordinator.last_request_result == (mode ? LCS_CONFIG_REQUEST_CAPACITY : LCS_CONFIG_REQUEST_CONFLICT));
        lcs_config_transactions_process(&nodes[1].transactions, 1000, receive, &nodes[1]);
        assert(nodes[1].received.result == LCS_CONFIG_TX_OK && nodes[1].runtime.agreement.committed.revision == 1);
        for (size_t i = 0; i < nodes[1].runtime.requests.count; i++) assert(!nodes[1].runtime.requests.records[i].committed);
        lcs_config_value_clear(&value); cleanup();
    }
}
static void test_proposer_observation_failure_retains_decision(void)
{
    setup(); network_errors = 0; uint64_t instances[LCS_MAX_NODES] = {100,101,102};
    lcs_config_value_t value = candidate("known-despite-observation-failure"); forbid_io = true;
    assert(!lcs_config_transactions_propose(&nodes[0].transactions, instances, 950, &value, 1000));
    for (unsigned i = 0; i < 5000 && nodes[0].transactions.proposer.phase != LCS_CONFIG_PROPOSER_LEARNING; i++) network_tick(1000);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_LEARNING);
    fail_sync = 2;
    for (unsigned i = 0; i < 5000 && nodes[0].coordinator.phase != LCS_CONFIG_COORD_OBSERVING; i++)
    { assert(lcs_config_coordinator_process(&nodes[0].coordinator, 1000) >= 0); usleep(1000); }
    assert(nodes[0].coordinator.phase == LCS_CONFIG_COORD_OBSERVING); fail_sync = 0;
    until_history_finished(&nodes[0]);
    assert(nodes[0].coordinator.failed && lcs_config_value_equal(&nodes[0].runtime.agreement.committed, &value));
    lcs_config_transactions_process(&nodes[0].transactions, 11000, network_send, &nodes[0]);
    assert(nodes[0].transactions.proposer.phase == LCS_CONFIG_PROPOSER_COMPLETE);
    assert(lcs_config_value_equal(lcs_config_transactions_selected(&nodes[0].transactions), &value));
    assert(!network_errors && nodes[0].runtime.application.applied.revision == 0);
    lcs_config_value_clear(&value); cleanup();
}

static lcs_config_value_t next_value(const lcs_config_value_t *previous, bool present)
{
    lcs_config_t *configs = malloc(3 * sizeof(*configs));
    assert(configs);
    uint64_t revision;
    assert(!lcs_resource_config_decode(previous->document, previous->length, &config,
                                         &configs[0], &revision, error, sizeof(error)));
    configs[1] = configs[0];
    configs[1].resource_count = present ? 1 : 0;
    if (present)
        configs[1].resources[0] = config.resources[0];
    assert(!lcs_resource_config_assign_identities(&configs[0], revision, &configs[1], &configs[2], error, sizeof(error)));
    char *document;
    size_t length;
    assert(!lcs_resource_config_encode(&configs[2], revision + 1, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    char request[32];
    snprintf(request, sizeof(request), "revision-%llu", (unsigned long long)(revision + 1));
    assert(!lcs_config_value_init(&value, &config, request, document, length, error, sizeof(error)));
    free(document); free(configs);
    return value;
}

static void seed_decision(node_t *node, const lcs_config_value_t *value)
{
    assert(!lcs_config_agreement_learn(&node->runtime.agreement, node->runtime.agreement.committed.digest,
        value->revision * 32, 3, value, error, sizeof(error)));
}

static void await_sync(size_t node, uint64_t now)
{
    for (unsigned i = 0; i < 5000; i++)
    {
        network_tick(now);
        lcs_config_sync_phase_t phase = nodes[node].transactions.sync.phase;
        if ((phase == LCS_CONFIG_SYNC_COMPLETE || phase == LCS_CONFIG_SYNC_CURRENT) &&
            nodes[node].coordinator.phase != LCS_CONFIG_COORD_OBSERVING)
            return;
        assert(phase != LCS_CONFIG_SYNC_UNKNOWN);
    }
    assert(!"checkpoint synchronization timed out");
}

static void test_checkpoint_sync(void)
{
    setup();
    network_errors = 0;
    lcs_config_value_t first = candidate("activation");
    lcs_config_value_t removed = next_value(&first, false);
    lcs_config_value_t recreated = next_value(&removed, true);
    seed_decision(&nodes[1], &first);
    seed_decision(&nodes[1], &removed);
    seed_decision(&nodes[1], &recreated);
    seed_decision(&nodes[0], &first);
    assert(!lcs_config_application_begin(&nodes[0].runtime.application, &nodes[0].runtime.agreement, error, sizeof(error)));
    assert(!lcs_config_agreement_prepare(&nodes[0].runtime.agreement, 2, first.digest, 320, error, sizeof(error)));
    assert(!lcs_config_agreement_accept(&nodes[0].runtime.agreement, first.digest, 320, &removed, error, sizeof(error)));
    /* A conflicting historical incarnation cannot masquerade as unchanged. */
    lcs_config_t *bad = malloc(sizeof(*bad));
    assert(bad);
    uint64_t revision;
    assert(!lcs_resource_config_decode(recreated.document, recreated.length, &config, bad, &revision, error, sizeof(error)));
    bad->resources[0].birth_revision = 0;
    strcpy(bad->resources[0].address, "192.0.2.99/32");
    char *document;
    size_t length;
    assert(!lcs_resource_config_encode(bad, revision, &document, &length, error, sizeof(error)));
    lcs_config_value_t invalid = {0};
    assert(!lcs_config_value_init(&invalid, &config, "invalid-checkpoint", document, length, error, sizeof(error)));
    assert(lcs_config_agreement_check_checkpoint(&nodes[0].runtime.agreement,
        nodes[1].runtime.agreement.parent_digest, 96, 3, &invalid, error, sizeof(error)) < 0);
    free(document); free(bad); lcs_config_value_clear(&invalid);
    assert(lcs_config_agreement_check_checkpoint(&nodes[0].runtime.agreement,
        nodes[1].runtime.agreement.parent_digest, 96, 1, &recreated, error, sizeof(error)) < 0);
    forbid_io = true;
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 1, 1000));
    assert(lcs_config_transactions_propose(&nodes[0].transactions, (uint64_t[LCS_MAX_NODES]){100,101,102},
        900, &removed, 1000) < 0);
    await_sync(0, 1000);
    assert(!network_errors);
    assert(lcs_config_value_equal(&nodes[0].runtime.agreement.committed, &recreated));
    const lcs_config_request_record_t *checkpoint_record = lcs_config_requests_find(&nodes[0].runtime.requests, recreated.request_id);
    assert(checkpoint_record && checkpoint_record->committed);
    assert(!lcs_config_requests_find(&nodes[0].runtime.requests, removed.request_id)); /* Skipped outcome remains unknown. */
    assert(nodes[0].runtime.agreement.promised == 320 && !nodes[0].runtime.agreement.accepted.document);
    assert(nodes[0].runtime.application.applied.revision == 0);
    assert(lcs_config_value_equal(&nodes[0].runtime.application.pending, &first));
    assert(!lcs_config_coordinator_can_vote(&nodes[0].coordinator));
    /* Quorum-only nodes retain the same durable metadata and can skip activation. */
    assert(!lcs_config_transactions_sync(&nodes[2].transactions, 1, 101, 2, 1000));
    await_sync(2, 1000);
    assert(lcs_config_value_equal(&nodes[2].runtime.agreement.committed, &recreated));
    assert(nodes[2].runtime.application.applied.revision == 0);
    /* Matching checkpoints are read-only and do not consume a durable operation. */
    uint64_t completed = nodes[0].coordinator.completed_votes;
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 3, 1000));
    await_sync(0, 1000);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_CURRENT && nodes[0].coordinator.completed_votes == completed);
    assert(lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 3, 1000) < 0);
    /* A different frozen baseline is never bridged by a checkpoint request. */
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 4, 1000));
    nodes[0].transactions.sync.query.value[0] = nodes[0].transactions.sync.query.value[0] == 'a' ? 'b' : 'a';
    for (unsigned i = 0; i < 100 && nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_QUERYING; i++)
        network_tick(1000);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_UNKNOWN);
    assert(nodes[0].coordinator.completed_votes == completed);
    /* Cleanup still sees the old incarnation after catching up over delete/recreate. */
    forbid_io = false;
    assert(!lcs_config_application_finish(&nodes[0].runtime.application, first.digest, error, sizeof(error)));
    assert(!lcs_config_application_begin(&nodes[0].runtime.application, &nodes[0].runtime.agreement, error, sizeof(error)));
    lcs_config_t *configs = malloc(2 * sizeof(*configs));
    assert(configs);
    lcs_config_application_plan_t plan;
    assert(!lcs_config_application_plan(&nodes[0].runtime.application, &configs[0], &configs[1], &plan, error, sizeof(error)));
    assert(plan.cleanup == 1 && plan.new_to_old[0] == -1 && configs[1].resources[0].birth_revision == 3);
    free(configs);
    /* Reopening retains the checkpoint, promise, and pending cleanup obligations. */
    lcs_config_agreement_t recovered = {0};
    assert(!lcs_config_agreement_open(&recovered, &nodes[0].store, nodes[0].runtime.base, false, error, sizeof(error)));
    assert(recovered.committed.revision == 3 && recovered.promised == 320);
    lcs_config_application_t catalog = {0};
    assert(!lcs_config_application_open(&catalog, &recovered, false, error, sizeof(error)));
    assert(catalog.applied.revision == 1 && catalog.pending.revision == 3);
    lcs_config_application_close(&catalog);
    lcs_config_agreement_close(&recovered);
    lcs_config_value_clear(&first); lcs_config_value_clear(&removed); lcs_config_value_clear(&recreated);
    cleanup();

    /* Session loss must not cancel an already submitted checkpoint write. */
    setup();
    network_errors = 0;
    first = candidate("checkpoint-disconnect");
    seed_decision(&nodes[1], &first);
    forbid_io = true;
    stop_sync = 1;
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 1, 1000));
    for (unsigned i = 0; i < 5000 && nodes[0].transactions.sync.phase != LCS_CONFIG_SYNC_PERSISTING; i++)
        network_tick(1000);
    stop_sync = 0;
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_PERSISTING);
    await_stopped(&nodes[0]);
    pid_t writer = nodes[0].coordinator.storage.pid;
    assert(writer > 0 && nodes[0].runtime.agreement.committed.revision == 0);
    lcs_config_transactions_disconnect(&nodes[0].transactions, 1);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_UNKNOWN && nodes[0].coordinator.storage.pid == writer);
    assert(!kill(writer, SIGCONT));
    for (unsigned i = 0; i < 5000 && (!nodes[0].coordinator.completed_votes ||
         nodes[0].coordinator.phase == LCS_CONFIG_COORD_OBSERVING); i++)
        network_tick(1000);
    assert(nodes[0].runtime.agreement.committed.revision == 1 && nodes[0].runtime.application.applied.revision == 0);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_UNKNOWN && !network_errors);
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 2, 1000));
    await_sync(0, 1000);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_CURRENT);
    lcs_config_value_clear(&first);
    cleanup();

    /* Backpressure and disconnect cannot extend a request's original deadline. */
    setup();
    forbid_io = true;
    network_blocked = true;
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 1, 1000));
    network_tick(10999);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_QUERYING);
    network_tick(11000);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_UNKNOWN);
    assert(nodes[0].runtime.agreement.committed.revision == 0 && !nodes[0].coordinator.storage.pid);
    network_blocked = false;
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 101, 2, 12000));
    lcs_config_transactions_disconnect(&nodes[0].transactions, 1);
    assert(nodes[0].transactions.sync.phase == LCS_CONFIG_SYNC_UNKNOWN);
    assert(!lcs_config_transactions_sync(&nodes[0].transactions, 1, 999, 3, 12000));
    /* Reply from a replacement session is not the peer instance queried. */
    nodes[0].transactions.sync.sent = true;
    lcs_config_tx_control_t stale = nodes[0].transactions.sync.query;
    stale.kind = LCS_CONFIG_TX_SYNC_STATUS;
    stale.sender_instance = 101; stale.target_instance = 100;
    strcpy(stale.value, nodes[1].runtime.agreement.committed.digest);
    unsigned char payload[256];
    assert(!lcs_config_tx_encode(&stale, payload, sizeof(payload), &length));
    assert(!lcs_config_transactions_receive(&nodes[0].transactions, 1, 101, LCS_MSG_CONFIG_TX_CONTROL, payload, length, 12000));
    assert(nodes[0].transactions.sync.phase != LCS_CONFIG_SYNC_CURRENT);
    cleanup();

}

int main(void)
{
    parent = getpid();
    test_network_proposer();
    test_proposal_reservation_order_and_timeout();
    test_proposal_history_admission();
    test_accept_identity_durability();
    test_accept_identity_failures();
    test_accept_history_conflicts_and_capacity();
    test_accept_cancellation_recovery();
    test_learned_observation_order();
    test_learned_observation_failures();
    test_learned_observation_rejections();
    test_proposer_observation_failure_retains_decision();
    test_checkpoint_sync();
    test_codecs_and_session_admission();
    test_majority_and_recovery();
    test_disconnect_and_uncertain_write();
    test_backpressure_and_partial_cleanup();
    puts("network configuration proposer, checkpoint synchronization and majority decision unit tests passed");
    return 0;
}
