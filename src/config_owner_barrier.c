// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_owner_barrier.h"
#include "protocol.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static bool digest_valid(const char *digest)
{
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    return !digest[64];
}

static bool scope_valid(const lcs_config_owner_scope_t *scope)
{
    return scope && scope->old_revision < scope->target_revision && scope->target_revision <= INT64_MAX &&
        digest_valid(scope->membership) && digest_valid(scope->old_value) && digest_valid(scope->target_value) &&
        strcmp(scope->old_value, scope->target_value);
}

bool lcs_config_owner_scope_equal(const lcs_config_owner_scope_t *a, const lcs_config_owner_scope_t *b)
{
    return a->old_revision == b->old_revision && a->target_revision == b->target_revision && a->drain == b->drain &&
        !memcmp(a->membership, b->membership, sizeof(a->membership)) &&
        !memcmp(a->old_value, b->old_value, sizeof(a->old_value)) &&
        !memcmp(a->target_value, b->target_value, sizeof(a->target_value));
}

int lcs_config_owner_scope(const lcs_config_application_t *application,
                             lcs_config_owner_scope_t *scope, char *err, size_t err_len)
{
    if (!application || !scope || application->poisoned || !application->applied.document || !application->pending.document)
        goto invalid;
    lcs_config_t *configs = malloc(2 * sizeof(*configs));
    if (!configs)
        goto invalid;
    lcs_config_application_plan_t plan;
    int result = lcs_config_application_plan(application, &configs[0], &configs[1], &plan, err, err_len);
    size_t count = result ? 0 : configs[0].resource_count;
    free(configs);
    if (result)
        return -1;
    lcs_config_owner_scope_t next = { .old_revision = application->applied.revision,
        .target_revision = application->pending.revision, .drain = plan.drain };
    memcpy(next.membership, application->membership, sizeof(next.membership));
    memcpy(next.old_value, application->applied.digest, sizeof(next.old_value));
    memcpy(next.target_value, application->pending.digest, sizeof(next.target_value));
    if (!scope_valid(&next) || (count < 32 && (next.drain >> count)))
        goto invalid;
    *scope = next;
    return 0;
invalid:
    if (err && err_len)
        snprintf(err, err_len, "owner barrier requires a healthy applied/pending cleanup catalog");
    return -1;
}

static bool message_valid(const lcs_config_owner_message_t *message)
{
    return message && message->sender_instance && message->target_instance && message->id && scope_valid(&message->scope) &&
        (((message->kind == LCS_CONFIG_OWNER_QUERY || message->kind == LCS_CONFIG_OWNER_APPLIED_QUERY) && !message->result) ||
         ((message->kind == LCS_CONFIG_OWNER_REPLY || message->kind == LCS_CONFIG_OWNER_APPLIED_REPLY) &&
          message->result >= LCS_CONFIG_OWNER_STOPPED &&
          message->result <= LCS_CONFIG_OWNER_FAILED));
}

int lcs_config_owner_encode(const lcs_config_owner_message_t *message,
                              void *payload, size_t capacity, size_t *length)
{
    if (!payload || !length || !message_valid(message))
        return -1;
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, capacity);
    const lcs_config_owner_scope_t *scope = &message->scope;
    if (lcs_buf_put_u16(&writer, LCS_CONFIG_OWNER_FORMAT) || lcs_buf_put_u8(&writer, message->kind) ||
        lcs_buf_put_u8(&writer, message->result) || lcs_buf_put_u64(&writer, message->sender_instance) ||
        lcs_buf_put_u64(&writer, message->target_instance) || lcs_buf_put_u64(&writer, message->id) ||
        lcs_buf_put_u64(&writer, scope->old_revision) || lcs_buf_put_u64(&writer, scope->target_revision) ||
        lcs_buf_put_u32(&writer, scope->drain) ||
        lcs_buf_put_fixed_string(&writer, scope->membership, sizeof(scope->membership)) ||
        lcs_buf_put_fixed_string(&writer, scope->old_value, sizeof(scope->old_value)) ||
        lcs_buf_put_fixed_string(&writer, scope->target_value, sizeof(scope->target_value)))
        return -1;
    *length = writer.len;
    return 0;
}

int lcs_config_owner_decode(const void *payload, size_t length, lcs_config_owner_message_t *message)
{
    if (!payload || !message || length != LCS_CONFIG_OWNER_MESSAGE_SIZE)
        return -1;
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, payload, length);
    uint16_t format;
    lcs_config_owner_message_t next = {0};
    lcs_config_owner_scope_t *scope = &next.scope;
    if (lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_OWNER_FORMAT ||
        lcs_buf_get_u8(&reader, &next.kind) || lcs_buf_get_u8(&reader, &next.result) ||
        lcs_buf_get_u64(&reader, &next.sender_instance) || lcs_buf_get_u64(&reader, &next.target_instance) ||
        lcs_buf_get_u64(&reader, &next.id) || lcs_buf_get_u64(&reader, &scope->old_revision) ||
        lcs_buf_get_u64(&reader, &scope->target_revision) || lcs_buf_get_u32(&reader, &scope->drain))
        return -1;
    /* Exact lowercase hex plus NUL; do not normalize malformed padding. */
    memcpy(scope->membership, reader.data + reader.off, sizeof(scope->membership)); reader.off += sizeof(scope->membership);
    memcpy(scope->old_value, reader.data + reader.off, sizeof(scope->old_value)); reader.off += sizeof(scope->old_value);
    memcpy(scope->target_value, reader.data + reader.off, sizeof(scope->target_value)); reader.off += sizeof(scope->target_value);
    if (reader.off != reader.len || !message_valid(&next))
        return -1;
    *message = next;
    return 0;
}

int lcs_config_owner_receipt_seal(lcs_config_owner_receipt_t *receipt,
                                    const lcs_config_owner_scope_t *scope, uint64_t instance)
{
    if (!receipt || !instance || !scope_valid(scope) || receipt->failed ||
        (receipt->sealed && (receipt->instance != instance || !lcs_config_owner_scope_equal(&receipt->scope, scope))))
        return -1;
    receipt->scope = *scope; receipt->instance = instance; receipt->sealed = true;
    return 0;
}

void lcs_config_owner_receipt_revoke(lcs_config_owner_receipt_t *receipt)
{
    receipt->failed = true;
}

int lcs_config_owner_answer(const lcs_config_owner_receipt_t *receipt, uint64_t self_instance,
                              uint64_t peer_instance, const lcs_config_owner_message_t *query,
                              lcs_config_owner_message_t *reply)
{
    if (!receipt || !reply || !self_instance || !message_valid(query) || query->kind != LCS_CONFIG_OWNER_QUERY ||
        query->sender_instance != peer_instance || query->target_instance != self_instance)
        return -1;
    *reply = *query;
    reply->kind = LCS_CONFIG_OWNER_REPLY;
    reply->sender_instance = self_instance; reply->target_instance = peer_instance;
    reply->result = receipt->failed || (receipt->sealed &&
        (receipt->instance != self_instance || !lcs_config_owner_scope_equal(&receipt->scope, &query->scope))) ?
        LCS_CONFIG_OWNER_FAILED : receipt->sealed ? LCS_CONFIG_OWNER_STOPPED : LCS_CONFIG_OWNER_PENDING;
    return 0;
}

int lcs_config_owner_begin(lcs_config_owner_round_t *round, const lcs_config_t *base,
                             const lcs_config_owner_scope_t *scope, size_t self,
                             const uint64_t instances[LCS_MAX_NODES], uint64_t id, uint64_t now_ms)
{
    char membership[LCS_CONFIG_DIGEST_SIZE];
    if (!round || !base || !scope_valid(scope) || !instances || !id || !base->node_count ||
        base->node_count > LCS_MAX_NODES || self >= base->node_count || !instances[self] ||
        base->nodes[self].role != LCS_NODE_FULL || now_ms > UINT64_MAX - LCS_CONFIG_OWNER_TIMEOUT_MS ||
        (round->active && ((!round->failed && !round->sealed) || round->instance != instances[self] || id <= round->id)) ||
        lcs_config_agreement_membership(base, membership) || strcmp(membership, scope->membership))
        return -1;
    lcs_config_owner_round_t next = { .scope = *scope, .instance = instances[self], .id = id,
        .deadline_ms = now_ms + LCS_CONFIG_OWNER_TIMEOUT_MS, .node_count = base->node_count, .self = self, .active = true };
    for (size_t i = 0; i < base->node_count; i++)
        if (base->nodes[i].role == LCS_NODE_FULL)
        {
            next.required |= UINT32_C(1) << i;
            next.expected_instance[i] = instances[i];
        }
    *round = next;
    return 0;
}

int lcs_config_owner_observe(lcs_config_owner_round_t *round, size_t node, uint64_t instance)
{
    if (!round || !round->active || round->failed || node >= round->node_count)
        return -1;
    if (!(round->required & (UINT32_C(1) << node)))
        return 0;
    if (round->expected_instance[node] && round->expected_instance[node] != instance)
    { round->failed = true; return -1; }
    round->expected_instance[node] = instance;
    return 0;
}

int lcs_config_owner_poll(lcs_config_owner_round_t *round, uint64_t now_ms)
{
    if (!round || !round->active || round->failed)
        return -1;
    if (round->sealed)
        return 1;
    if (now_ms >= round->deadline_ms)
    { round->failed = true; return -1; }
    if ((round->stopped & round->required) == round->required)
    { round->sealed = true; return 1; }
    return 0;
}

int lcs_config_owner_local(lcs_config_owner_round_t *round, const lcs_config_owner_receipt_t *receipt)
{
    if (!round || !round->active || round->failed || !receipt)
        return -1;
    if (!receipt->sealed && !receipt->failed)
        return 0;
    if (receipt->failed || receipt->instance != round->instance || !lcs_config_owner_scope_equal(&round->scope, &receipt->scope))
    { round->failed = true; return -1; }
    round->stopped |= UINT32_C(1) << round->self;
    return 0;
}

int lcs_config_owner_query(lcs_config_owner_round_t *round, size_t node, uint64_t instance,
                             uint64_t now_ms, lcs_config_owner_message_t *query)
{
    if (!query || lcs_config_owner_poll(round, now_ms) < 0 || node >= round->node_count || node == round->self)
        return -1;
    uint32_t bit = UINT32_C(1) << node;
    if (!(round->required & bit))
        return 1;
    if (lcs_config_owner_observe(round, node, instance))
        return -1;
    if (!instance || (round->stopped & bit))
        return 1;
    *query = (lcs_config_owner_message_t){ .kind = LCS_CONFIG_OWNER_QUERY, .sender_instance = round->instance,
        .target_instance = instance, .id = round->id, .scope = round->scope };
    round->queried |= bit;
    return 0;
}

int lcs_config_owner_reply(lcs_config_owner_round_t *round, size_t node, uint64_t instance,
                             uint64_t now_ms, const lcs_config_owner_message_t *reply)
{
    if (!round || !round->active || !message_valid(reply) || reply->kind != LCS_CONFIG_OWNER_REPLY ||
        node >= round->node_count || node == round->self || !(round->queried & (UINT32_C(1) << node)) ||
        reply->sender_instance != instance || reply->target_instance != round->instance || reply->id != round->id)
        return 1;
    if (lcs_config_owner_poll(round, now_ms) < 0 || lcs_config_owner_observe(round, node, instance))
        return -1;
    uint32_t bit = UINT32_C(1) << node;
    if (!lcs_config_owner_scope_equal(&round->scope, &reply->scope) || reply->result == LCS_CONFIG_OWNER_FAILED ||
        ((round->stopped & bit) && reply->result != LCS_CONFIG_OWNER_STOPPED))
    { round->failed = true; return -1; }
    if (reply->result == LCS_CONFIG_OWNER_STOPPED)
        round->stopped |= bit;
    return 0;
}
