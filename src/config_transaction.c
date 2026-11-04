// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_transaction.h"
#include "protocol.h"

#include <string.h>

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
    if (control->kind < LCS_CONFIG_TX_PREPARE || control->kind > LCS_CONFIG_TX_LEARNED ||
        control->result > LCS_CONFIG_TX_UNKNOWN || !control->sender_instance || !control->target_instance ||
        !control->id || !control->revision || control->revision > LCS_RESOURCE_REVISION_MAX ||
        !(control->ballot >> 5) || !digest_valid(control->membership, false) ||
        !digest_valid(control->parent, false) || !digest_valid(control->value, true))
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
        if (control.kind != LCS_CONFIG_TX_PREPARE)
            return 0; /* No network proposer yet: unsolicited replies confer nothing. */
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
        if (transactions->active || peer->stage ||
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
    bool unknown = coordinator->failed && coordinator->storage.pid <= 0;
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
