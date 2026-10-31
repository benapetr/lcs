// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_readiness.h"
#include "protocol.h"

#include <stdio.h>
#include <string.h>

static bool digest_valid(const char *digest)
{
    for (size_t i = 0; i < 64; i++)
        if (!((digest[i] >= '0' && digest[i] <= '9') || (digest[i] >= 'a' && digest[i] <= 'f')))
            return false;
    return digest[64] == 0;
}

static bool valid(const lcs_config_readiness_message_t *message)
{
    return message->requester_instance && message->attempt &&
        digest_valid(message->membership) && digest_valid(message->baseline) &&
        ((message->kind == LCS_READINESS_QUERY && !message->responder_instance && !message->status) ||
         (message->kind == LCS_READINESS_REPLY && message->responder_instance &&
          message->status >= LCS_READINESS_READY && message->status <= LCS_READINESS_MISMATCH));
}

int lcs_config_readiness_encode(const lcs_config_readiness_message_t *message,
                                 void *payload, size_t capacity, size_t *length)
{
    lcs_buf_writer_t writer;
    lcs_buf_writer_init(&writer, payload, capacity);
    if (!valid(message) || lcs_buf_put_u16(&writer, LCS_CONFIG_READINESS_FORMAT) ||
        lcs_buf_put_u8(&writer, message->kind) ||
        lcs_buf_put_u64(&writer, message->requester_instance) || lcs_buf_put_u64(&writer, message->attempt) ||
        lcs_buf_put_u64(&writer, message->responder_instance) || lcs_buf_put_u8(&writer, message->status) ||
        lcs_buf_put_fixed_string(&writer, message->membership, sizeof(message->membership)) ||
        lcs_buf_put_fixed_string(&writer, message->baseline, sizeof(message->baseline)))
        return -1;
    *length = writer.len;
    return 0;
}

int lcs_config_readiness_decode(const void *payload, size_t length,
                                 lcs_config_readiness_message_t *message)
{
    lcs_buf_reader_t reader;
    lcs_buf_reader_init(&reader, payload, length);
    uint16_t format;
    lcs_config_readiness_message_t next = {0};
    if (length > LCS_CONFIG_READINESS_MAX ||
        lcs_buf_get_u16(&reader, &format) || format != LCS_CONFIG_READINESS_FORMAT ||
        lcs_buf_get_u8(&reader, &next.kind) ||
        lcs_buf_get_u64(&reader, &next.requester_instance) || lcs_buf_get_u64(&reader, &next.attempt) ||
        lcs_buf_get_u64(&reader, &next.responder_instance) || lcs_buf_get_u8(&reader, &next.status) ||
        lcs_buf_get_fixed_string(&reader, next.membership, sizeof(next.membership), sizeof(next.membership)) ||
        lcs_buf_get_fixed_string(&reader, next.baseline, sizeof(next.baseline), sizeof(next.baseline)) ||
        reader.off != reader.len || !valid(&next))
        return -1;
    /* Fixed digest strings must be full lower-case hex plus their terminator;
     * get_fixed_string alone would hide nonzero trailing bytes after a NUL. */
    const unsigned char *bytes = payload;
    if (bytes[length - 1] || bytes[length - 1 - LCS_CONFIG_DIGEST_SIZE])
        return -1;
    *message = next;
    return 0;
}

static int fail(char *err, size_t length, const char *text)
{
    if (err && length)
        snprintf(err, length, "configuration readiness: %s", text);
    return -1;
}

int lcs_config_readiness_begin(lcs_config_readiness_t *state,
                                const lcs_config_activation_t *activation,
                                const lcs_config_agreement_t *agreement,
                                const lcs_config_application_t *application,
                                uint64_t requester_instance, uint64_t attempt, uint64_t now_ms,
                                char *err, size_t err_len)
{
    if (state->active && !state->round.failed && !state->round.sealed && now_ms < state->deadline_ms)
        return fail(err, err_len, "a readiness attempt is already pending");
    if (!requester_instance || !attempt || now_ms > UINT64_MAX - LCS_CONFIG_READINESS_TIMEOUT_MS ||
        (state->active && state->query.requester_instance == requester_instance && attempt <= state->query.attempt))
        return fail(err, err_len, "invalid/reused attempt or deadline");
    lcs_config_readiness_t next = { .active = true,
        .deadline_ms = now_ms + LCS_CONFIG_READINESS_TIMEOUT_MS };
    if (lcs_config_barrier_begin(&next.round, activation, agreement, application, err, err_len))
        return -1;
    next.query.kind = LCS_READINESS_QUERY;
    next.query.requester_instance = requester_instance;
    next.query.attempt = attempt;
    memcpy(next.query.membership, next.round.membership, sizeof(next.query.membership));
    memcpy(next.query.baseline, next.round.baseline, sizeof(next.query.baseline));
    if (next.round.ready == next.round.required)
        (void)lcs_config_barrier_seal(&next.round, NULL, 0);
    *state = next;
    return 0;
}

bool lcs_config_readiness_complete(const lcs_config_readiness_t *state)
{
    return state->active && !state->round.failed && state->round.sealed;
}

static bool expired(lcs_config_readiness_t *state, uint64_t now_ms)
{
    if (state->active && !state->round.sealed && now_ms >= state->deadline_ms)
        state->round.failed = true;
    return state->round.failed;
}

int lcs_config_readiness_poll(lcs_config_readiness_t *state, uint64_t now_ms)
{
    if (!state->active || expired(state, now_ms))
        return -1;
    return lcs_config_readiness_complete(state) ? 1 : 0;
}

int lcs_config_readiness_query(lcs_config_readiness_t *state, size_t node,
                                uint64_t peer_instance, uint64_t now_ms,
                                lcs_config_readiness_message_t *query)
{
    if (!state->active || expired(state, now_ms) || node >= LCS_MAX_NODES || !peer_instance ||
        !(state->round.required & (UINT32_C(1) << node)))
        return -1;
    if (state->round.ready & (UINT32_C(1) << node))
        return 1;
    state->expected_instance[node] = peer_instance;
    state->sent |= UINT32_C(1) << node;
    *query = state->query;
    return 0;
}

int lcs_config_readiness_reply(lcs_config_readiness_t *state, size_t node,
                                uint64_t peer_instance, uint64_t now_ms,
                                const lcs_config_readiness_message_t *reply,
                                char *err, size_t err_len)
{
    if (!valid(reply) || reply->kind != LCS_READINESS_REPLY)
        return fail(err, err_len, "invalid reply");
    if (!state->active || node >= LCS_MAX_NODES || !(state->sent & (UINT32_C(1) << node)) ||
        reply->requester_instance != state->query.requester_instance || reply->attempt != state->query.attempt ||
        reply->responder_instance != peer_instance || state->expected_instance[node] != peer_instance)
        return 1;
    if (expired(state, now_ms))
        return fail(err, err_len, "fixed readiness deadline expired");
    if (strcmp(reply->membership, state->query.membership) || strcmp(reply->baseline, state->query.baseline) ||
        reply->status == LCS_READINESS_MISMATCH)
    {
        if (!state->round.sealed)
            state->round.failed = true;
        return fail(err, err_len, "conflicting current readiness reply");
    }
    if (reply->status == LCS_READINESS_NOT_READY)
        return 0;
    if (lcs_config_barrier_ready(&state->round, node, reply->membership, reply->baseline, err, err_len))
        return -1;
    if (state->round.ready == state->round.required)
        return lcs_config_barrier_seal(&state->round, err, err_len);
    return 0;
}
