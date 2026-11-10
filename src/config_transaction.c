// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_transaction.h"
#include "protocol.h"

#include <string.h>

static void proposer_process(lcs_config_transactions_t *, uint64_t, lcs_config_tx_send_t, void *);
static int proposer_control(lcs_config_transactions_t *, size_t, const lcs_config_tx_control_t *);
static int proposer_transfer(lcs_config_transactions_t *, size_t, uint16_t, const void *, size_t, uint64_t);
static void proposer_clear(lcs_config_network_proposal_t *);

static bool sync_active(const lcs_config_transactions_t *);
static int sync_control(lcs_config_transactions_t *, size_t, const lcs_config_tx_control_t *, uint64_t);
static int sync_transfer(lcs_config_transactions_t *, size_t, uint16_t, const void *, size_t, uint64_t);
static void sync_process(lcs_config_transactions_t *, uint64_t, lcs_config_tx_send_t, void *);

static bool digest_valid(const char *text, bool empty)
{
    if (empty && !*text)
        return true;
    for (size_t i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f')))
            return false;
    return !text[64];
}

static bool valid(const lcs_config_tx_control_t *control)
{
    if (control->kind < LCS_CONFIG_TX_PREPARE || control->kind > LCS_CONFIG_TX_SYNC_STATUS ||
        control->result > LCS_CONFIG_TX_UNKNOWN || !control->sender_instance || !control->target_instance ||
        !control->id || control->revision > LCS_RESOURCE_REVISION_MAX ||
        !digest_valid(control->membership, false) ||
        !digest_valid(control->parent, false) || !digest_valid(control->value, true))
        return false;
    if (control->kind == LCS_CONFIG_TX_SYNC_QUERY)
        return !control->result && !control->ballot && !control->promised && !control->accepted_ballot &&
            digest_valid(control->value, false);
    if (control->kind == LCS_CONFIG_TX_SYNC_STATUS)
        return !control->promised && !control->accepted_ballot && (control->result ?
            !control->ballot && !*control->value : *control->value &&
                (control->revision ? !!(control->ballot >> 5) : !control->ballot));
    if (!control->revision || !(control->ballot >> 5))
        return false;
    if (control->kind == LCS_CONFIG_TX_PREPARE)
        return !control->result && !control->promised && !control->accepted_ballot && !*control->value;
    if (control->result != LCS_CONFIG_TX_OK)
        return !control->accepted_ballot && !*control->value;
    if (control->kind == LCS_CONFIG_TX_PROMISE)
        return control->promised >= control->ballot && control->accepted_ballot <= control->promised &&
            ((control->accepted_ballot && *control->value) || (!control->accepted_ballot && !*control->value));
    return !control->accepted_ballot && *control->value;
}

int lcs_config_tx_encode(const lcs_config_tx_control_t *control, void *payload, size_t cap, size_t *length)
{
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, cap);
    if (!valid(control) || lcs_buf_put_u16(&writer, LCS_CONFIG_TX_FORMAT) ||
        lcs_buf_put_u8(&writer, control->kind) || lcs_buf_put_u8(&writer, control->result) ||
        lcs_buf_put_u64(&writer, control->sender_instance) || lcs_buf_put_u64(&writer, control->target_instance) ||
        lcs_buf_put_u64(&writer, control->id) || lcs_buf_put_u64(&writer, control->revision) ||
        lcs_buf_put_u64(&writer, control->ballot) || lcs_buf_put_u64(&writer, control->promised) ||
        lcs_buf_put_u64(&writer, control->accepted_ballot) ||
        lcs_buf_put_fixed_string(&writer, control->membership, sizeof(control->membership)) ||
        lcs_buf_put_fixed_string(&writer, control->parent, sizeof(control->parent)) ||
        lcs_buf_put_fixed_string(&writer, control->value, sizeof(control->value)))
        return -1;
    *length = writer.len;
    return 0;
}

static int string(lcs_buf_reader_t *reader, char *text, size_t width)
{
    if (reader->off > reader->len || width > reader->len - reader->off)
        return -1;
    const unsigned char *start = reader->data + reader->off, *nul = memchr(start, 0, width);
    if (!nul)
        return -1;
    for (const unsigned char *p = nul; p < start + width; p++)
        if (*p)
            return -1;
    memcpy(text, start, width);
    reader->off += width;
    return 0;
}

int lcs_config_tx_decode(const void *payload, size_t length, lcs_config_tx_control_t *control)
{
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, payload, length);
    uint16_t format;
    lcs_config_tx_control_t next = {0};
    if (length > LCS_CONFIG_TX_CONTROL_MAX || lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_TX_FORMAT ||
        lcs_buf_get_u8(&reader, &next.kind) || lcs_buf_get_u8(&reader, &next.result) ||
        lcs_buf_get_u64(&reader, &next.sender_instance) || lcs_buf_get_u64(&reader, &next.target_instance) ||
        lcs_buf_get_u64(&reader, &next.id) || lcs_buf_get_u64(&reader, &next.revision) ||
        lcs_buf_get_u64(&reader, &next.ballot) || lcs_buf_get_u64(&reader, &next.promised) ||
        lcs_buf_get_u64(&reader, &next.accepted_ballot) ||
        string(&reader, next.membership, sizeof(next.membership)) || string(&reader, next.parent, sizeof(next.parent)) ||
        string(&reader, next.value, sizeof(next.value)) || reader.off != reader.len || !valid(&next))
        return -1;
    *control = next;
    return 0;
}

int lcs_config_tx_wrap(uint64_t sender, uint64_t target, const void *body, size_t body_length,
                        void *payload, size_t cap, size_t *length)
{
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, cap);
    if (!sender || !target || !body || !body_length ||
        lcs_buf_put_u16(&writer, LCS_CONFIG_TX_FORMAT) || lcs_buf_put_u64(&writer, sender) ||
        lcs_buf_put_u64(&writer, target) || body_length > writer.cap - writer.len)
        return -1;
    memcpy(writer.data + writer.len, body, body_length);
    *length = writer.len + body_length;
    return 0;
}

int lcs_config_tx_unwrap(const void *payload, size_t length, uint64_t *sender, uint64_t *target,
                          const void **body, size_t *body_length)
{
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, payload, length);
    uint16_t format;
    uint64_t from, to;
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_TX_FORMAT ||
        lcs_buf_get_u64(&reader, &from) || lcs_buf_get_u64(&reader, &to) || !from || !to || reader.off == length)
        return -1;
    *sender = from; *target = to;
    *body = reader.data + reader.off; *body_length = length - reader.off;
    return 0;
}

void lcs_config_transactions_init(lcs_config_transactions_t *transactions,
                                    lcs_config_coordinator_t *coordinator, uint64_t instance)
{
    memset(transactions, 0, sizeof(*transactions));
    transactions->coordinator = coordinator;
    transactions->instance = instance;
}

void lcs_config_transactions_disconnect(lcs_config_transactions_t *transactions, size_t node)
{
    if (node >= LCS_MAX_NODES)
        return;
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (sync_active(transactions) && transactions->sync.node == node)
        transactions->sync.phase = LCS_CONFIG_SYNC_UNKNOWN;
    lcs_config_transfer_clear(&transactions->proposer.peers[node].incoming);
    transactions->proposer.peers[node].instance = 0;
    lcs_config_transfer_clear(&peer->incoming);
    lcs_config_value_clear(&peer->outgoing);
    memset(peer, 0, sizeof(*peer));
    if (transactions->active && transactions->pending_node == node)
        transactions->reply_pending = false;
}

void lcs_config_transactions_close(lcs_config_transactions_t *transactions)
{
    for (size_t i = 0; i < LCS_MAX_NODES; i++)
        lcs_config_transactions_disconnect(transactions, i);
    proposer_clear(&transactions->proposer);
    /* Durable jobs belong to the coordinator and survive session teardown. */
    memset(transactions, 0, sizeof(*transactions));
}

static bool eligible(const lcs_config_transactions_t *transactions)
{
    return transactions->coordinator && lcs_config_coordinator_can_vote(transactions->coordinator);
}

static void response(lcs_config_transactions_t *transactions, size_t node,
                       const lcs_config_tx_control_t *request, uint8_t kind, uint8_t result, uint64_t now_ms)
{
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (peer->stage)
        return; /* Bounded one-response slot: caller retries if backpressured. */
    peer->response = *request;
    peer->response.sender_instance = transactions->instance;
    peer->response.target_instance = peer->instance;
    peer->response.kind = kind;
    peer->response.result = result;
    peer->response.promised = peer->response.accepted_ballot = 0;
    peer->offset = 0;
    memset(peer->response.value, 0, sizeof(peer->response.value));
    peer->stage = 1;
    peer->deadline_ms = now_ms + LCS_CONFIG_TRANSFER_TIMEOUT_MS;
}

static bool decision_retry(const lcs_config_transactions_t *transactions, uint64_t revision,
                             const char *digest, const char *parent)
{
    const lcs_config_coordinator_t *coordinator = transactions->coordinator;
    if (!coordinator || !coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY)
        return false;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    return lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application) &&
        revision == runtime->agreement.committed.revision && !strcmp(digest, runtime->agreement.committed.digest) &&
        !strcmp(parent, runtime->agreement.parent_digest);
}

static int submit(lcs_config_transactions_t *transactions, size_t node,
                     const lcs_config_tx_control_t *request, lcs_config_agreement_action_t action,
                     uint32_t voters, const lcs_config_value_t *value, uint64_t now_ms)
{
    uint8_t kind = action == LCS_CONFIG_PREPARE ? LCS_CONFIG_TX_PROMISE :
        action == LCS_CONFIG_ACCEPT ? LCS_CONFIG_TX_ACK : LCS_CONFIG_TX_LEARNED;
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    if (transactions->active || transactions->peers[node].stage || (coordinator && coordinator->storage.pid > 0))
    {
        response(transactions, node, request, kind, LCS_CONFIG_TX_BUSY, now_ms);
        return 0;
    }
    bool duplicate = action == LCS_CONFIG_LEARN && decision_retry(transactions, request->revision, request->value, request->parent);
    if (!eligible(transactions) && !duplicate)
    {
        response(transactions, node, request, kind, LCS_CONFIG_TX_NOT_READY, now_ms);
        return 0;
    }
    lcs_config_agreement_job_t job = { .action = action, .revision = request->revision,
        .ballot = request->ballot, .value = value, .voters = voters };
    memcpy(job.parent_digest, request->parent, sizeof(job.parent_digest));
    char error[256];
    int result = action == LCS_CONFIG_LEARN ? lcs_config_coordinator_start_learn(coordinator, &job, error, sizeof(error)) :
        lcs_config_coordinator_start_vote(coordinator, &job, error, sizeof(error));
    if (result)
    {
        response(transactions, node, request, kind, result > 0 ? LCS_CONFIG_TX_BUSY : LCS_CONFIG_TX_REJECTED, now_ms);
        return 0;
    }
    transactions->pending = *request;
    transactions->pending_node = node;
    transactions->pending_kind = kind;
    transactions->completion = coordinator->completed_votes + 1;
    transactions->active = transactions->reply_pending = true;
    return 0;
}

int lcs_config_transactions_receive(lcs_config_transactions_t *transactions, size_t node,
                                     uint64_t peer_instance, uint16_t type,
                                     const void *payload, size_t length, uint64_t now_ms)
{
    if (!transactions->instance || !peer_instance || node >= LCS_MAX_NODES ||
        now_ms > UINT64_MAX - LCS_CONFIG_TRANSFER_TIMEOUT_MS)
        return -1;
    if (transactions->coordinator && transactions->coordinator->initialized &&
        (node >= transactions->coordinator->runtime->base->node_count ||
         (int)node == lcs_config_self_index(transactions->coordinator->runtime->base)))
        return -1;
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (peer->instance && peer->instance != peer_instance)
        lcs_config_transactions_disconnect(transactions, node);
    peer->instance = peer_instance;
    lcs_config_tx_control_t control = {0};
    if (type == LCS_MSG_CONFIG_TX_CONTROL)
    {
        if (lcs_config_tx_decode(payload, length, &control) || control.sender_instance != peer_instance ||
            control.target_instance != transactions->instance)
            return -1;
        if (control.kind >= LCS_CONFIG_TX_SYNC_QUERY)
            return sync_control(transactions, node, &control, now_ms);
        if (control.kind != LCS_CONFIG_TX_PREPARE)
            return proposer_control(transactions, node, &control);
        if ((control.ballot & 31u) != node)
            return -1;
        if (transactions->coordinator && transactions->coordinator->initialized &&
            transactions->coordinator->runtime->activation.store &&
            strcmp(control.membership, transactions->coordinator->runtime->activation.membership))
            return -1;
        return submit(transactions, node, &control, LCS_CONFIG_PREPARE, 0, NULL, now_ms);
    }
    uint64_t sender, target;
    const void *body;
    size_t body_length;
    if (lcs_config_tx_unwrap(payload, length, &sender, &target, &body, &body_length) ||
        sender != peer_instance || target != transactions->instance)
        return -1;
    int routed = sync_transfer(transactions, node, type, body, body_length, now_ms);
    if (routed)
        return routed < 0 ? -1 : 0;
    routed = proposer_transfer(transactions, node, type, body, body_length, now_ms);
    if (routed)
        return routed < 0 ? -1 : 0;
    if (type == LCS_MSG_CONFIG_TX_BEGIN)
    {
        lcs_config_transfer_metadata_t metadata;
        if (lcs_config_transfer_decode_begin(body, body_length, &metadata) ||
            (metadata.kind != LCS_CONFIG_TRANSFER_ACCEPT && metadata.kind != LCS_CONFIG_TRANSFER_LEARN) ||
            !metadata.revision || (metadata.ballot & 31u) != node || !(metadata.ballot >> 5) || metadata.accepted_ballot ||
            (metadata.kind == LCS_CONFIG_TRANSFER_ACCEPT && metadata.voters))
            return -1;
        control = (lcs_config_tx_control_t){ .sender_instance = sender, .target_instance = target,
            .id = metadata.id, .revision = metadata.revision, .ballot = metadata.ballot };
        memcpy(control.membership, metadata.membership, sizeof(control.membership));
        memcpy(control.parent, metadata.parent_digest, sizeof(control.parent));
        memcpy(control.value, metadata.value_digest, sizeof(control.value));
        if (transactions->active || peer->stage || transactions->proposer.peers[node].incoming.document ||
            (transactions->coordinator && transactions->coordinator->storage.pid > 0))
        {
            response(transactions, node, &control, metadata.kind == LCS_CONFIG_TRANSFER_ACCEPT ?
                LCS_CONFIG_TX_ACK : LCS_CONFIG_TX_LEARNED, LCS_CONFIG_TX_BUSY, now_ms);
            return 0;
        }
        bool duplicate = metadata.kind == LCS_CONFIG_TRANSFER_LEARN &&
            decision_retry(transactions, metadata.revision, metadata.value_digest, metadata.parent_digest);
        if (!eligible(transactions) && !duplicate)
        {
            response(transactions, node, &control, metadata.kind == LCS_CONFIG_TRANSFER_ACCEPT ?
                LCS_CONFIG_TX_ACK : LCS_CONFIG_TX_LEARNED, LCS_CONFIG_TX_NOT_READY, now_ms);
            return 0;
        }
        const lcs_config_runtime_t *runtime = transactions->coordinator->runtime;
        if (!duplicate && (metadata.revision != runtime->agreement.committed.revision + 1 ||
            strcmp(metadata.parent_digest, runtime->agreement.committed.digest)))
        {
            response(transactions, node, &control, metadata.kind == LCS_CONFIG_TRANSFER_ACCEPT ?
                LCS_CONFIG_TX_ACK : LCS_CONFIG_TX_LEARNED,
                metadata.revision <= runtime->agreement.committed.revision ? LCS_CONFIG_TX_STALE : LCS_CONFIG_TX_REJECTED,
                now_ms);
            return 0;
        }
        if (metadata.kind == LCS_CONFIG_TRANSFER_LEARN &&
            !lcs_config_agreement_has_quorum(&runtime->agreement, metadata.voters))
        {
            response(transactions, node, &control, LCS_CONFIG_TX_LEARNED, LCS_CONFIG_TX_REJECTED, now_ms);
            return 0;
        }
        return lcs_config_transfer_begin(&peer->incoming, &metadata, runtime->activation.membership, now_ms) < 0 ? -1 : 0;
    }
    if (type == LCS_MSG_CONFIG_TX_CHUNK)
        return lcs_config_transfer_chunk(&peer->incoming, body, body_length, now_ms);
    if (type != LCS_MSG_CONFIG_TX_END)
        return -1;
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, body, body_length);
    uint64_t id;
    if (lcs_buf_get_u64(&reader, &id) || reader.off != reader.len || !peer->incoming.document)
        return -1;
    lcs_config_transfer_metadata_t metadata = peer->incoming.metadata;
    lcs_config_value_t value = {0};
    char error[256];
    if (lcs_config_transfer_finish(&peer->incoming, id, transactions->coordinator->runtime->base,
                                    now_ms, &value, error, sizeof(error)))
        return -1;
    control = (lcs_config_tx_control_t){ .sender_instance = sender, .target_instance = target,
        .id = id, .revision = metadata.revision, .ballot = metadata.ballot };
    memcpy(control.membership, metadata.membership, sizeof(control.membership));
    memcpy(control.parent, metadata.parent_digest, sizeof(control.parent));
    memcpy(control.value, metadata.value_digest, sizeof(control.value));
    int result = submit(transactions, node, &control, metadata.kind == LCS_CONFIG_TRANSFER_ACCEPT ?
        LCS_CONFIG_ACCEPT : LCS_CONFIG_LEARN, metadata.voters, &value, now_ms);
    lcs_config_value_clear(&value);
    return result;
}

static void completed(lcs_config_transactions_t *transactions, uint64_t now_ms)
{
    if (!transactions->active)
        return;
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    bool durable = coordinator->completed_votes == transactions->completion;
    bool unknown = !durable && coordinator->failed && coordinator->storage.pid <= 0;
    if (!unknown && coordinator->completed_votes < transactions->completion)
        return;
    size_t node = transactions->pending_node;
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (!transactions->reply_pending)
    {
        transactions->active = false;
        return;
    }
    if (peer->stage)
        return;
    int result = unknown || coordinator->completed_votes != transactions->completion ? LCS_CONFIG_TX_UNKNOWN :
        coordinator->last_vote_result < 0 ? LCS_CONFIG_TX_REJECTED : coordinator->last_vote_result;
    response(transactions, node, &transactions->pending, transactions->pending_kind, (uint8_t)result, now_ms);
    if (!unknown)
        peer->response.promised = coordinator->runtime->agreement.promised;
    if (result == LCS_CONFIG_TX_OK)
    {
        const lcs_config_agreement_t *agreement = &coordinator->runtime->agreement;
        if (transactions->pending_kind == LCS_CONFIG_TX_PROMISE && agreement->accepted.document)
        {
            if (lcs_config_value_copy(&peer->outgoing, &agreement->accepted))
                peer->response.result = LCS_CONFIG_TX_UNKNOWN;
            else
            {
                peer->response.accepted_ballot = agreement->accepted_ballot;
                memcpy(peer->response.value, agreement->accepted.digest, sizeof(peer->response.value));
                peer->outgoing_metadata = (lcs_config_transfer_metadata_t){ .kind = LCS_CONFIG_TRANSFER_PROMISE,
                    .id = peer->response.id, .revision = agreement->accepted.revision, .ballot = peer->response.ballot,
                    .accepted_ballot = agreement->accepted_ballot, .length = (uint32_t)agreement->accepted.length };
                memcpy(peer->outgoing_metadata.membership, agreement->membership, sizeof(peer->outgoing_metadata.membership));
                memcpy(peer->outgoing_metadata.parent_digest, transactions->pending.parent, sizeof(peer->outgoing_metadata.parent_digest));
                memcpy(peer->outgoing_metadata.value_digest, agreement->accepted.digest, sizeof(peer->outgoing_metadata.value_digest));
                memcpy(peer->outgoing_metadata.request_id, agreement->accepted.request_id, sizeof(peer->outgoing_metadata.request_id));
            }
        }
        else if (transactions->pending_kind != LCS_CONFIG_TX_PROMISE)
            memcpy(peer->response.value, transactions->pending.value, sizeof(peer->response.value));
    }
    transactions->active = false;
}

void lcs_config_transactions_process(lcs_config_transactions_t *transactions, uint64_t now_ms,
                                      lcs_config_tx_send_t send, void *context)
{
    if (!transactions->instance)
        return;
    completed(transactions, now_ms);
    proposer_process(transactions, now_ms, send, context);
    sync_process(transactions, now_ms, send, context);
    unsigned budget = 4;
    unsigned char payload[LCS_CONFIG_TRANSFER_CHUNK_MAX + 64], body[LCS_CONFIG_TRANSFER_CHUNK_MAX + 32];
    for (size_t scanned = 0; scanned < LCS_MAX_NODES && budget; scanned++)
    {
        size_t node = transactions->cursor++ % LCS_MAX_NODES;
        lcs_config_tx_peer_t *peer = &transactions->peers[node];
        (void)lcs_config_transfer_expire(&peer->incoming, now_ms);
        if (!peer->stage)
            continue;
        if (now_ms >= peer->deadline_ms)
        {
            lcs_config_value_clear(&peer->outgoing);
            peer->offset = 0;
            peer->stage = 0;
            continue;
        }
        size_t length = 0, body_length = 0;
        uint16_t type;
        if (peer->stage == 1)
        {
            type = LCS_MSG_CONFIG_TX_CONTROL;
            if (lcs_config_tx_encode(&peer->response, payload, sizeof(payload), &length))
                continue;
        }
        else
        {
            if (peer->stage == 2)
            {
                type = LCS_MSG_CONFIG_TX_BEGIN;
                if (lcs_config_transfer_encode_begin(&peer->outgoing_metadata, body, sizeof(body), &body_length))
                    continue;
            }
            else if (peer->offset < peer->outgoing.length)
            {
                type = LCS_MSG_CONFIG_TX_CHUNK;
                size_t count = peer->outgoing.length - peer->offset;
                if (count > LCS_CONFIG_TRANSFER_CHUNK_MAX)
                    count = LCS_CONFIG_TRANSFER_CHUNK_MAX;
                if (lcs_config_transfer_encode_chunk(peer->response.id, peer->offset,
                    peer->outgoing.document + peer->offset, count, body, sizeof(body), &body_length))
                    continue;
            }
            else
            {
                type = LCS_MSG_CONFIG_TX_END;
                lcs_buf_writer_t writer;
                lcs_buf_writer_init(&writer, body, sizeof(body));
                (void)lcs_buf_put_u64(&writer, peer->response.id);
                body_length = writer.len;
            }
            if (lcs_config_tx_wrap(transactions->instance, peer->instance, body, body_length,
                                     payload, sizeof(payload), &length))
                continue;
        }
        if (send(context, node, type, payload, length))
            continue;
        budget--;
        if (peer->stage == 1 && !peer->outgoing.document)
            peer->stage = 0;
        else if (peer->stage == 1)
            peer->stage = 2;
        else if (peer->stage == 2)
            peer->stage = 3;
        else if (type == LCS_MSG_CONFIG_TX_CHUNK)
            peer->offset += (uint32_t)(body_length - 13u);
        else
        {
            lcs_config_value_clear(&peer->outgoing);
            peer->offset = peer->stage = 0;
        }
    }
}

static bool proposing(const lcs_config_network_proposal_t *proposer)
{
    return proposer->phase > LCS_CONFIG_PROPOSER_IDLE && proposer->phase < LCS_CONFIG_PROPOSER_COMPLETE;
}

static void proposer_clear(lcs_config_network_proposal_t *proposer)
{
    for (size_t i = 0; i < LCS_MAX_NODES; i++)
        lcs_config_transfer_clear(&proposer->peers[i].incoming);
    lcs_config_proposal_clear(&proposer->proposal);
    memset(proposer, 0, sizeof(*proposer));
}

const lcs_config_value_t *lcs_config_transactions_selected(const lcs_config_transactions_t *transactions)
{
    const lcs_config_proposal_t *proposal = &transactions->proposer.proposal;
    return proposal->sealed ? (proposal->highest_accepted ? &proposal->recovered : &proposal->requested) : NULL;
}

static int proposer_job(lcs_config_transactions_t *transactions, lcs_config_agreement_action_t action)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    if (transactions->active || coordinator->storage.pid > 0)
        return 1;
    lcs_config_agreement_job_t job = { .action = action, .revision = proposer->proposal.revision,
        .ballot = proposer->proposal.ballot, .voters = proposer->proposal.acknowledgements,
        .value = action == LCS_CONFIG_PREPARE ? NULL : lcs_config_transactions_selected(transactions) };
    memcpy(job.parent_digest, proposer->proposal.parent_digest, sizeof(job.parent_digest));
    char error[256];
    int result = action == LCS_CONFIG_LEARN ? lcs_config_coordinator_start_learn(coordinator, &job, error, sizeof(error)) :
        lcs_config_coordinator_start_vote(coordinator, &job, error, sizeof(error));
    if (!result)
        proposer->completion = coordinator->completed_votes + 1;
    return result;
}

int lcs_config_transactions_propose(lcs_config_transactions_t *transactions,
                                     const uint64_t instances[LCS_MAX_NODES],
                                     uint64_t id, const lcs_config_value_t *requested, uint64_t now_ms)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    if (!instances || !requested || !transactions->instance || !id || id <= proposer->id ||
        now_ms > UINT64_MAX - LCS_CONFIG_TRANSFER_TIMEOUT_MS || !eligible(transactions) ||
        proposing(proposer) || sync_active(transactions) || transactions->active || transactions->coordinator->storage.pid > 0)
        return -1;
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    const lcs_config_requests_t *history = &transactions->coordinator->runtime->requests;
    if (!requested->document || !memchr(requested->request_id, 0, sizeof(requested->request_id)) ||
        !history->store || history->poisoned || history->store != agreement->store || history->base != agreement->base ||
        lcs_config_requests_find(history, requested->request_id) ||
        !strcmp(requested->request_id, agreement->committed.request_id) ||
        (agreement->accepted.document && !strcmp(requested->request_id, agreement->accepted.request_id)))
        return -1;
    int self = lcs_config_self_index(agreement->base);
    if (self < 0 || agreement->base->nodes[self].role != LCS_NODE_FULL || instances[self] != transactions->instance)
        return -1;
    uint32_t available = 0;
    for (size_t i = 0; i < agreement->base->node_count; i++)
        if (instances[i])
            available |= UINT32_C(1) << i;
    if (!lcs_config_agreement_has_quorum(agreement, available))
        return -1;
    lcs_config_proposal_t next = {0};
    uint64_t ballot = lcs_config_agreement_next_ballot(agreement, 0);
    if (!ballot || lcs_config_proposal_init(&next, agreement, ballot, requested))
        return -1;
    proposer_clear(proposer);
    proposer->proposal = next;
    proposer->id = id;
    proposer->deadline_ms = now_ms + LCS_CONFIG_TRANSFER_TIMEOUT_MS;
    for (size_t i = 0; i < agreement->base->node_count; i++)
        proposer->peers[i].instance = instances[i];
    proposer->phase = LCS_CONFIG_PROPOSER_RESERVING;
    lcs_config_agreement_job_t reservation = { .action = LCS_CONFIG_REQUEST_RESERVE,
        .revision = proposer->proposal.revision - 1, .value = &proposer->proposal.requested };
    memcpy(reservation.parent_digest, proposer->proposal.parent_digest, sizeof(reservation.parent_digest));
    char error[256];
    if (lcs_config_coordinator_start_requests(transactions->coordinator, &reservation, error, sizeof(error)))
    {
        proposer->phase = LCS_CONFIG_PROPOSER_UNKNOWN;
        return -1;
    }
    proposer->completion = transactions->coordinator->completed_requests + 1;
    return 0;
}

static bool matches(const lcs_config_transactions_t *transactions, size_t node, const lcs_config_tx_control_t *control)
{
    const lcs_config_network_proposal_t *proposer = &transactions->proposer;
    return proposing(proposer) && proposer->peers[node].sent && proposer->peers[node].instance &&
        control->sender_instance == proposer->peers[node].instance && control->id == proposer->id &&
        control->revision == proposer->proposal.revision && control->ballot == proposer->proposal.ballot &&
        !strcmp(control->membership, transactions->coordinator->runtime->agreement.membership) &&
        !strcmp(control->parent, proposer->proposal.parent_digest);
}

static int proposer_control(lcs_config_transactions_t *transactions, size_t node, const lcs_config_tx_control_t *control)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    if (!matches(transactions, node, control) || control->result != LCS_CONFIG_TX_OK)
        return 0; /* Unsolicited, stale and negative replies never confer a vote. */
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    if (proposer->phase == LCS_CONFIG_PROPOSER_PROMISES && control->kind == LCS_CONFIG_TX_PROMISE)
    {
        if (control->promised != proposer->proposal.ballot)
            return -1;
        if (proposer->proposal.sealed || (proposer->proposal.promises & (UINT32_C(1) << node)))
            return 0;
        if (control->accepted_ballot)
        {
            lcs_config_tx_control_t *previous = &proposer->peers[node].promise;
            if (previous->accepted_ballot && memcmp(previous, control, sizeof(*control)))
                return -1;
            *previous = *control; /* Count only after the complete matching snapshot. */
        }
        else if (lcs_config_proposal_promise(&proposer->proposal, agreement, node, control->revision,
                     control->parent, control->promised, 0, NULL) < 0)
            return -1;
    }
    else if (proposer->phase == LCS_CONFIG_PROPOSER_ACKS && control->kind == LCS_CONFIG_TX_ACK)
    {
        const lcs_config_value_t *value = lcs_config_transactions_selected(transactions);
        if (!strcmp(control->value, value->digest))
            (void)lcs_config_proposal_ack(&proposer->proposal, agreement, node, control->ballot, value);
    }
    return 0;
}

static int proposer_transfer(lcs_config_transactions_t *transactions, size_t node, uint16_t type,
                               const void *body, size_t length, uint64_t now_ms)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    lcs_config_proposer_peer_t *peer = &proposer->peers[node];
    if (type == LCS_MSG_CONFIG_TX_BEGIN)
    {
        lcs_config_transfer_metadata_t metadata;
        if (lcs_config_transfer_decode_begin(body, length, &metadata) || metadata.kind != LCS_CONFIG_TRANSFER_PROMISE)
            return 0;
        if (proposer->phase != LCS_CONFIG_PROPOSER_PROMISES || !peer->instance ||
            peer->instance != transactions->peers[node].instance || !peer->sent ||
            metadata.id != proposer->id)
            return 1;
        const lcs_config_tx_control_t *control = &peer->promise;
        if (!control->accepted_ballot || metadata.accepted_ballot != control->accepted_ballot ||
            metadata.ballot != control->ballot || metadata.revision != control->revision || metadata.voters ||
            strcmp(metadata.value_digest, control->value) || strcmp(metadata.parent_digest, control->parent) ||
            strcmp(metadata.membership, control->membership) || transactions->peers[node].incoming.document)
            return -1;
        return lcs_config_transfer_begin(&peer->incoming, &metadata, control->membership, now_ms) < 0 ? -1 : 1;
    }
    lcs_buf_reader_t reader;
    uint64_t id;
    lcs_buf_reader_init(&reader, body, length);
    uint8_t chunk_format;
    if ((type == LCS_MSG_CONFIG_TX_CHUNK && lcs_buf_get_u8(&reader, &chunk_format)) ||
        lcs_buf_get_u64(&reader, &id) || !proposer->id || id != proposer->id ||
        transactions->peers[node].incoming.document)
        return 0;
    if (!peer->incoming.document || proposer->phase != LCS_CONFIG_PROPOSER_PROMISES)
        return 1;
    if (type == LCS_MSG_CONFIG_TX_CHUNK)
        return lcs_config_transfer_chunk(&peer->incoming, body, length, now_ms) ? -1 : 1;
    if (type != LCS_MSG_CONFIG_TX_END || reader.off != reader.len)
        return -1;
    lcs_config_value_t value = {0};
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    char error[256];
    if (lcs_config_transfer_finish(&peer->incoming, id, agreement->base, now_ms, &value, error, sizeof(error)))
        return -1;
    int result = lcs_config_proposal_promise(&proposer->proposal, agreement, node, proposer->proposal.revision,
        proposer->proposal.parent_digest, peer->promise.promised, peer->promise.accepted_ballot, &value);
    lcs_config_value_clear(&value);
    return result < 0 ? -1 : 1;
}

static void proposer_phase(lcs_config_network_proposal_t *proposer, lcs_config_proposer_phase_t phase)
{
    proposer->phase = phase;
    proposer->completion = 0;
    for (size_t i = 0; i < LCS_MAX_NODES; i++)
    {
        proposer->peers[i].stage = 0;
        proposer->peers[i].offset = 0;
        proposer->peers[i].sent = false;
        lcs_config_transfer_clear(&proposer->peers[i].incoming);
    }
}

static int proposer_send(lcs_config_transactions_t *transactions, size_t node,
                           lcs_config_tx_send_t send, void *context)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    lcs_config_proposer_peer_t *peer = &proposer->peers[node];
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    unsigned char payload[LCS_CONFIG_TRANSFER_CHUNK_MAX + 64], body[LCS_CONFIG_TRANSFER_CHUNK_MAX + 32];
    size_t length, body_length;
    uint16_t type;
    if (proposer->phase == LCS_CONFIG_PROPOSER_PROMISES)
    {
        lcs_config_tx_control_t control = { .kind = LCS_CONFIG_TX_PREPARE,
            .sender_instance = transactions->instance, .target_instance = peer->instance,
            .id = proposer->id, .revision = proposer->proposal.revision, .ballot = proposer->proposal.ballot };
        memcpy(control.membership, agreement->membership, sizeof(control.membership));
        memcpy(control.parent, proposer->proposal.parent_digest, sizeof(control.parent));
        if (lcs_config_tx_encode(&control, payload, sizeof(payload), &length))
            return -1;
        type = LCS_MSG_CONFIG_TX_CONTROL;
    }
    else
    {
        const lcs_config_value_t *value = lcs_config_transactions_selected(transactions);
        if (!peer->stage)
        {
            lcs_config_transfer_metadata_t metadata = {
                .kind = proposer->phase == LCS_CONFIG_PROPOSER_ACKS ? LCS_CONFIG_TRANSFER_ACCEPT : LCS_CONFIG_TRANSFER_LEARN,
                .id = proposer->id, .revision = value->revision, .ballot = proposer->proposal.ballot,
                .length = (uint32_t)value->length,
                .voters = proposer->phase == LCS_CONFIG_PROPOSER_DISTRIBUTING ? proposer->proposal.acknowledgements : 0 };
            memcpy(metadata.membership, agreement->membership, sizeof(metadata.membership));
            memcpy(metadata.parent_digest, proposer->proposal.parent_digest, sizeof(metadata.parent_digest));
            memcpy(metadata.value_digest, value->digest, sizeof(metadata.value_digest));
            memcpy(metadata.request_id, value->request_id, sizeof(metadata.request_id));
            if (lcs_config_transfer_encode_begin(&metadata, body, sizeof(body), &body_length))
                return -1;
            type = LCS_MSG_CONFIG_TX_BEGIN;
        }
        else if (peer->offset < value->length)
        {
            size_t count = value->length - peer->offset;
            if (count > LCS_CONFIG_TRANSFER_CHUNK_MAX)
                count = LCS_CONFIG_TRANSFER_CHUNK_MAX;
            if (lcs_config_transfer_encode_chunk(proposer->id, peer->offset, value->document + peer->offset,
                                                   count, body, sizeof(body), &body_length))
                return -1;
            type = LCS_MSG_CONFIG_TX_CHUNK;
        }
        else
        {
            lcs_buf_writer_t writer;
            lcs_buf_writer_init(&writer, body, sizeof(body));
            if (lcs_buf_put_u64(&writer, proposer->id))
                return -1;
            body_length = writer.len;
            type = LCS_MSG_CONFIG_TX_END;
        }
        if (lcs_config_tx_wrap(transactions->instance, peer->instance, body, body_length,
                                payload, sizeof(payload), &length))
            return -1;
    }
    if (send(context, node, type, payload, length))
        return 0;
    if (type == LCS_MSG_CONFIG_TX_CONTROL || type == LCS_MSG_CONFIG_TX_END)
        peer->sent = true;
    else if (type == LCS_MSG_CONFIG_TX_BEGIN)
        peer->stage = 1;
    else
        peer->offset += (uint32_t)(body_length - 13u);
    return 1;
}

static void proposer_process(lcs_config_transactions_t *transactions, uint64_t now_ms,
                               lcs_config_tx_send_t send, void *context)
{
    lcs_config_network_proposal_t *proposer = &transactions->proposer;
    if (!proposing(proposer))
        return;
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    lcs_config_agreement_t *agreement = &coordinator->runtime->agreement;
    int self = lcs_config_self_index(agreement->base);
    /* Observe a durable local decision even if its completion and the transport
     * deadline arrive in the same scheduler iteration. */
    if (proposer->phase == LCS_CONFIG_PROPOSER_LEARNING && proposer->completion &&
        coordinator->completed_votes == proposer->completion &&
        coordinator->last_vote_result == LCS_CONFIG_AGREED)
        proposer_phase(proposer, LCS_CONFIG_PROPOSER_DISTRIBUTING);
    if (now_ms >= proposer->deadline_ms || coordinator->failed || coordinator->closing || proposer->proposal.failed)
    {
        proposer_phase(proposer, proposer->phase == LCS_CONFIG_PROPOSER_DISTRIBUTING ?
            LCS_CONFIG_PROPOSER_COMPLETE : LCS_CONFIG_PROPOSER_UNKNOWN);
        return; /* Never cancel or roll back a durable write on transport timeout. */
    }
    if (proposer->phase == LCS_CONFIG_PROPOSER_RESERVING)
    {
        if (coordinator->completed_requests < proposer->completion)
            return;
        if (coordinator->completed_requests != proposer->completion ||
            coordinator->last_request_result != LCS_CONFIG_REQUEST_RECORDED)
        {
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN);
            return;
        }
        proposer_phase(proposer, LCS_CONFIG_PROPOSER_PREPARING);
    }
    if (proposer->phase == LCS_CONFIG_PROPOSER_PREPARING && !proposer->completion)
    {
        int result = proposer_job(transactions, LCS_CONFIG_PREPARE);
        if (result < 0)
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN);
        return; /* A busy writer is retried without reserving the identity again. */
    }
    if (proposer->completion)
    {
        if (coordinator->completed_votes < proposer->completion)
            return;
        if (coordinator->completed_votes != proposer->completion || coordinator->last_vote_result != LCS_CONFIG_AGREED)
        {
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN);
            return;
        }
        if (proposer->phase == LCS_CONFIG_PROPOSER_PREPARING)
        {
            if (lcs_config_proposal_promise(&proposer->proposal, agreement, (size_t)self,
                proposer->proposal.revision, proposer->proposal.parent_digest, agreement->promised,
                agreement->accepted_ballot, agreement->accepted.document ? &agreement->accepted : NULL) < 0)
            { proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN); return; }
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_PROMISES);
        }
        else if (proposer->phase == LCS_CONFIG_PROPOSER_ACCEPTING)
        {
            (void)lcs_config_proposal_ack(&proposer->proposal, agreement, (size_t)self,
                proposer->proposal.ballot, lcs_config_transactions_selected(transactions));
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_ACKS);
        }
        else
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_DISTRIBUTING);
    }
    lcs_config_agreement_action_t action;
    lcs_config_proposer_phase_t next;
    bool job = false;
    if (proposer->phase == LCS_CONFIG_PROPOSER_PROMISES && lcs_config_proposal_seal(&proposer->proposal, agreement))
    { action = LCS_CONFIG_ACCEPT; next = LCS_CONFIG_PROPOSER_ACCEPTING; job = true; }
    else if (proposer->phase == LCS_CONFIG_PROPOSER_ACKS && lcs_config_proposal_chosen(&proposer->proposal, agreement))
    {
        bool partial = false;
        for (size_t i = 0; i < LCS_MAX_NODES; i++)
            partial = partial || (proposer->peers[i].instance && proposer->peers[i].stage && !proposer->peers[i].sent);
        if (!partial)
        { action = LCS_CONFIG_LEARN; next = LCS_CONFIG_PROPOSER_LEARNING; job = true; }
    }
    if (job)
    {
        int result = proposer_job(transactions, action);
        if (result < 0)
            proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN);
        else if (!result)
            proposer->phase = next;
        return;
    }
    if (proposer->phase != LCS_CONFIG_PROPOSER_PROMISES && proposer->phase != LCS_CONFIG_PROPOSER_ACKS &&
        proposer->phase != LCS_CONFIG_PROPOSER_DISTRIBUTING)
        return;
    unsigned budget = 4;
    bool pending = false;
    for (size_t scanned = 0; scanned < LCS_MAX_NODES && budget; scanned++)
    {
        size_t node = (transactions->cursor + scanned) % LCS_MAX_NODES;
        lcs_config_proposer_peer_t *peer = &proposer->peers[node];
        if ((int)node == self || !peer->instance || peer->sent ||
            (proposer->phase == LCS_CONFIG_PROPOSER_ACKS &&
             !(proposer->proposal.promises & (UINT32_C(1) << node))) ||
            (proposer->phase == LCS_CONFIG_PROPOSER_DISTRIBUTING &&
             !(proposer->proposal.acknowledgements & (UINT32_C(1) << node))))
            continue;
        pending = true;
        int result = proposer_send(transactions, node, send, context);
        if (result < 0)
        { proposer_phase(proposer, LCS_CONFIG_PROPOSER_UNKNOWN); return; }
        budget -= (unsigned)result;
    }
    if (!pending && proposer->phase == LCS_CONFIG_PROPOSER_DISTRIBUTING)
        proposer_phase(proposer, LCS_CONFIG_PROPOSER_COMPLETE);
}

static bool sync_active(const lcs_config_transactions_t *transactions)
{
    return transactions->sync.phase >= LCS_CONFIG_SYNC_QUERYING &&
        transactions->sync.phase <= LCS_CONFIG_SYNC_PERSISTING;
}

static bool sync_ready(const lcs_config_transactions_t *transactions)
{
    const lcs_config_coordinator_t *coordinator = transactions->coordinator;
    if (!coordinator || !coordinator->initialized || coordinator->closing || coordinator->failed ||
        coordinator->phase != LCS_CONFIG_COORD_READY || coordinator->storage.pid > 0)
        return false;
    const lcs_config_runtime_t *runtime = coordinator->runtime;
    return lcs_config_barrier_valid(&runtime->barrier, &runtime->activation, &runtime->agreement, &runtime->application);
}

int lcs_config_transactions_sync(lcs_config_transactions_t *transactions, size_t node,
                                  uint64_t peer_instance, uint64_t id, uint64_t now_ms)
{
    if (!transactions->instance || !peer_instance || !id || id <= transactions->sync.serial ||
        now_ms > UINT64_MAX - LCS_CONFIG_TRANSFER_TIMEOUT_MS || !sync_ready(transactions) ||
        sync_active(transactions) || proposing(&transactions->proposer) || transactions->active ||
        node >= transactions->coordinator->runtime->base->node_count ||
        (int)node == lcs_config_self_index(transactions->coordinator->runtime->base) ||
        transactions->peers[node].incoming.document || transactions->proposer.peers[node].incoming.document)
        return -1;
    lcs_config_sync_t *sync = &transactions->sync;
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    sync->query = (lcs_config_tx_control_t){ .kind = LCS_CONFIG_TX_SYNC_QUERY,
        .sender_instance = transactions->instance, .target_instance = peer_instance,
        .id = id, .revision = agreement->committed.revision };
    memcpy(sync->query.membership, agreement->membership, sizeof(sync->query.membership));
    memcpy(sync->query.parent, agreement->committed.digest, sizeof(sync->query.parent));
    memcpy(sync->query.value, transactions->coordinator->runtime->activation.baseline.digest, sizeof(sync->query.value));
    sync->status = (lcs_config_tx_control_t){0};
    sync->node = node;
    sync->serial = id;
    sync->deadline_ms = now_ms + LCS_CONFIG_TRANSFER_TIMEOUT_MS;
    sync->completion = 0;
    sync->sent = false;
    sync->phase = LCS_CONFIG_SYNC_QUERYING;
    return 0;
}

static int sync_control(lcs_config_transactions_t *transactions, size_t node,
                          const lcs_config_tx_control_t *control, uint64_t now_ms)
{
    lcs_config_sync_t *sync = &transactions->sync;
    if (control->kind == LCS_CONFIG_TX_SYNC_STATUS)
    {
        if (sync->phase != LCS_CONFIG_SYNC_QUERYING || !sync->sent || node != sync->node ||
            control->sender_instance != sync->query.target_instance || control->id != sync->query.id ||
            strcmp(control->membership, sync->query.membership))
            return 0;
        if (control->result)
            sync->phase = LCS_CONFIG_SYNC_UNKNOWN;
        else if (control->revision <= sync->query.revision)
        {
            const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
            sync->phase = control->revision == sync->query.revision &&
                (strcmp(control->value, sync->query.parent) ||
                 (control->revision && strcmp(control->parent, agreement->parent_digest))) ?
                LCS_CONFIG_SYNC_UNKNOWN : LCS_CONFIG_SYNC_CURRENT;
        }
        else
        {
            sync->status = *control;
            sync->phase = LCS_CONFIG_SYNC_RECEIVING;
        }
        return 0;
    }
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (peer->stage)
        return 0;
    if (sync_ready(transactions) && strcmp(control->membership, transactions->coordinator->runtime->agreement.membership))
        return -1;
    uint8_t result = !sync_ready(transactions) ? LCS_CONFIG_TX_NOT_READY :
        strcmp(control->value, transactions->coordinator->runtime->activation.baseline.digest) ? LCS_CONFIG_TX_REJECTED :
        peer->incoming.document || transactions->proposer.peers[node].incoming.document ||
        transactions->active || proposing(&transactions->proposer) ? LCS_CONFIG_TX_BUSY : LCS_CONFIG_TX_OK;
    response(transactions, node, control, LCS_CONFIG_TX_SYNC_STATUS, result, now_ms);
    peer->response.ballot = 0;
    if (result)
        return 0;
    const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
    peer->response.revision = agreement->committed.revision;
    peer->response.ballot = agreement->committed_ballot;
    memcpy(peer->response.value, agreement->committed.digest, sizeof(peer->response.value));
    memcpy(peer->response.parent, agreement->committed.revision ? agreement->parent_digest : agreement->committed.digest,
           sizeof(peer->response.parent));
    if (agreement->committed.revision <= control->revision)
        return 0;
    if (lcs_config_value_copy(&peer->outgoing, &agreement->committed))
    {
        peer->response.result = LCS_CONFIG_TX_UNKNOWN;
        peer->response.ballot = 0;
        memset(peer->response.value, 0, sizeof(peer->response.value));
        return 0;
    }
    peer->outgoing_metadata = (lcs_config_transfer_metadata_t){ .kind = LCS_CONFIG_TRANSFER_SYNC,
        .id = control->id, .revision = agreement->committed.revision, .ballot = agreement->committed_ballot,
        .voters = agreement->committed_voters, .length = (uint32_t)agreement->committed.length };
    memcpy(peer->outgoing_metadata.membership, agreement->membership, sizeof(peer->outgoing_metadata.membership));
    memcpy(peer->outgoing_metadata.parent_digest, agreement->parent_digest, sizeof(peer->outgoing_metadata.parent_digest));
    memcpy(peer->outgoing_metadata.value_digest, agreement->committed.digest, sizeof(peer->outgoing_metadata.value_digest));
    memcpy(peer->outgoing_metadata.request_id, agreement->committed.request_id, sizeof(peer->outgoing_metadata.request_id));
    return 0;
}

static int sync_transfer(lcs_config_transactions_t *transactions, size_t node, uint16_t type,
                           const void *body, size_t length, uint64_t now_ms)
{
    lcs_config_sync_t *sync = &transactions->sync;
    lcs_config_tx_peer_t *peer = &transactions->peers[node];
    if (type == LCS_MSG_CONFIG_TX_BEGIN)
    {
        lcs_config_transfer_metadata_t metadata;
        if (lcs_config_transfer_decode_begin(body, length, &metadata) || metadata.kind != LCS_CONFIG_TRANSFER_SYNC)
            return 0;
        if (sync->phase != LCS_CONFIG_SYNC_RECEIVING || sync->node != node || now_ms >= sync->deadline_ms ||
            sync->query.target_instance != peer->instance || metadata.id != sync->query.id)
            return 1; /* Unsolicited/stale checkpoints never allocate or initialize. */
        const lcs_config_agreement_t *agreement = &transactions->coordinator->runtime->agreement;
        if (metadata.revision != sync->status.revision || metadata.ballot != sync->status.ballot ||
            metadata.accepted_ballot || strcmp(metadata.membership, sync->status.membership) ||
            strcmp(metadata.parent_digest, sync->status.parent) || strcmp(metadata.value_digest, sync->status.value) ||
            !lcs_config_agreement_has_quorum(agreement, metadata.voters) ||
            !lcs_config_agreement_ballot_valid(agreement, metadata.ballot) ||
            transactions->proposer.peers[node].incoming.document ||
            (peer->incoming.document && (peer->incoming.metadata.kind != LCS_CONFIG_TRANSFER_SYNC ||
                                        peer->incoming.metadata.id != metadata.id)))
            return -1;
        return lcs_config_transfer_begin(&peer->incoming, &metadata, agreement->membership, now_ms) < 0 ? -1 : 1;
    }
    if (!peer->incoming.document || peer->incoming.metadata.kind != LCS_CONFIG_TRANSFER_SYNC)
        return 0;
    if (sync->phase != LCS_CONFIG_SYNC_RECEIVING || node != sync->node || now_ms >= sync->deadline_ms)
    {
        lcs_config_transfer_clear(&peer->incoming);
        sync->phase = LCS_CONFIG_SYNC_UNKNOWN;
        return 1;
    }
    if (type == LCS_MSG_CONFIG_TX_CHUNK)
        return lcs_config_transfer_chunk(&peer->incoming, body, length, now_ms) ? -1 : 1;
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, body, length);
    uint64_t id;
    if (type != LCS_MSG_CONFIG_TX_END || lcs_buf_get_u64(&reader, &id) || reader.off != reader.len)
        return -1;
    lcs_config_transfer_metadata_t metadata = peer->incoming.metadata;
    lcs_config_value_t value = {0};
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    char error[256];
    if (lcs_config_transfer_finish(&peer->incoming, id, coordinator->runtime->base, now_ms, &value, error, sizeof(error)))
        return -1;
    lcs_config_agreement_job_t job = { .action = LCS_CONFIG_CHECKPOINT, .revision = value.revision,
        .ballot = metadata.ballot, .voters = metadata.voters, .value = &value };
    memcpy(job.parent_digest, metadata.parent_digest, sizeof(job.parent_digest));
    int result = transactions->active || proposing(&transactions->proposer) ? -1 :
        lcs_config_coordinator_start_checkpoint(coordinator, &job, error, sizeof(error));
    lcs_config_value_clear(&value);
    sync->phase = result ? LCS_CONFIG_SYNC_UNKNOWN : LCS_CONFIG_SYNC_PERSISTING;
    if (!result)
        sync->completion = coordinator->completed_votes + 1;
    return 1;
}

static void sync_process(lcs_config_transactions_t *transactions, uint64_t now_ms,
                            lcs_config_tx_send_t send, void *context)
{
    lcs_config_sync_t *sync = &transactions->sync;
    if (!sync_active(transactions))
        return;
    lcs_config_coordinator_t *coordinator = transactions->coordinator;
    if (sync->phase == LCS_CONFIG_SYNC_PERSISTING && coordinator->completed_votes >= sync->completion)
        sync->phase = coordinator->completed_votes == sync->completion &&
            coordinator->last_vote_result == LCS_CONFIG_AGREED ? LCS_CONFIG_SYNC_COMPLETE : LCS_CONFIG_SYNC_UNKNOWN;
    if (!sync_active(transactions))
        return;
    if (now_ms >= sync->deadline_ms || coordinator->failed || coordinator->closing)
    {
        lcs_config_transfer_clear(&transactions->peers[sync->node].incoming);
        sync->phase = LCS_CONFIG_SYNC_UNKNOWN;
        return; /* Already submitted durable work is never cancelled by a timeout. */
    }
    if (sync->phase == LCS_CONFIG_SYNC_QUERYING && !sync->sent)
    {
        unsigned char payload[LCS_CONFIG_TX_CONTROL_MAX];
        size_t length;
        if (!lcs_config_tx_encode(&sync->query, payload, sizeof(payload), &length) &&
            !send(context, sync->node, LCS_MSG_CONFIG_TX_CONTROL, payload, length))
            sync->sent = true;
    }
}
