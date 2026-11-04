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
        if ((node->coordinator.completed_votes > previous || node->coordinator.failed) && !node->transactions.active)
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
        const char *names[] = {"activation.json", "agreement.json", "application.json", "barrier.json", "lock"};
        for (size_t j = 0; j < sizeof(names) / sizeof(names[0]); j++)
            assert(!unlinkat(node->store.directory_fd, names[j], 0));
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

int main(void)
{
    parent = getpid();
    test_codecs_and_session_admission();
    test_majority_and_recovery();
    test_disconnect_and_uncertain_write();
    test_backpressure_and_partial_cleanup();
    puts("network configuration acceptor and majority decision unit tests passed");
    return 0;
}
