// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_barrier.h"

#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration activation barrier: %s", message);
    return -1;
}

static uint32_t all_members(const lcs_config_t *base)
{
    if (!base || !base->node_count || base->node_count > LCS_MAX_NODES)
        return 0;
    return base->node_count == 32 ? UINT32_MAX : (UINT32_C(1) << base->node_count) - 1;
}

static bool bound(const lcs_config_activation_t *activation,
                     const lcs_config_agreement_t *agreement,
                     const lcs_config_application_t *application)
{
    return activation->store && all_members(activation->base) &&
        activation->phase == LCS_CONFIG_ACTIVATION_PREPARED && !activation->poisoned &&
        activation->baseline.document && !activation->baseline.revision &&
        !strcmp(activation->baseline.request_id, "bootstrap") &&
        !agreement->poisoned && !application->poisoned &&
        agreement->store == activation->store && application->store == activation->store &&
        agreement->base == activation->base && application->base == activation->base &&
        !strcmp(activation->member, agreement->member) && !strcmp(activation->member, application->member) &&
        !strcmp(activation->membership, agreement->membership) && !strcmp(activation->membership, application->membership);
}

bool lcs_config_barrier_local_ready(const lcs_config_activation_t *activation,
                                      const lcs_config_agreement_t *agreement,
                                      const lcs_config_application_t *application)
{
    return bound(activation, agreement, application) && !agreement->promised &&
        !agreement->accepted.document && !application->pending.document &&
        lcs_config_value_equal(&activation->baseline, &agreement->committed) &&
        lcs_config_value_equal(&activation->baseline, &application->applied);
}

int lcs_config_barrier_begin(lcs_config_barrier_round_t *round,
                              const lcs_config_activation_t *activation,
                              const lcs_config_agreement_t *agreement,
                              const lcs_config_application_t *application,
                              char *err, size_t err_len)
{
    if (!lcs_config_barrier_local_ready(activation, agreement, application))
        return fail(err, err_len, "durable pristine initialization is required before readiness");
    int self = lcs_config_self_index(activation->base);
    if (self < 0)
        return fail(err, err_len, "local member is absent");
    lcs_config_barrier_round_t next = { .required = all_members(activation->base),
                                        .ready = UINT32_C(1) << self };
    memcpy(next.membership, activation->membership, sizeof(next.membership));
    memcpy(next.baseline, activation->baseline.digest, sizeof(next.baseline));
    *round = next;
    return 0;
}

int lcs_config_barrier_ready(lcs_config_barrier_round_t *round, size_t node,
                              const char *membership, const char *baseline,
                              char *err, size_t err_len)
{
    if (round->failed || !round->required || node >= LCS_MAX_NODES ||
        !(round->required & (UINT32_C(1) << node)) || !membership || !baseline ||
        strcmp(membership, round->membership) || strcmp(baseline, round->baseline))
    {
        if (!round->sealed)
            round->failed = true;
        return fail(err, err_len, "invalid/foreign readiness receipt; restart the barrier round");
    }
    round->ready |= UINT32_C(1) << node;
    return 0;
}

int lcs_config_barrier_seal(lcs_config_barrier_round_t *round, char *err, size_t err_len)
{
    if (round->failed || !round->required || round->ready != round->required)
        return fail(err, err_len, "every configured member, including quorum-only, must confirm durable readiness");
    round->sealed = true;
    return 0;
}

static int checksum(json_t *root, char digest[LCS_CONFIG_DIGEST_SIZE])
{
    char *text = json_dumps(root, JSON_COMPACT | JSON_SORT_KEYS | JSON_ENSURE_ASCII);
    if (!text)
        return -1;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned char *ok = SHA256((unsigned char *)text, strlen(text), hash);
    free(text);
    if (!ok)
        return -1;
    for (size_t i = 0; i < sizeof(hash); i++)
        snprintf(digest + i * 2, 3, "%02x", hash[i]);
    return 0;
}

static bool matches(json_t *object, const char *key, const char *expected)
{
    json_t *value = json_object_get(object, key);
    const char *text = json_string_value(value);
    return text && json_string_length(value) == strlen(expected) && !strcmp(text, expected);
}

static int decode(lcs_config_barrier_t *barrier, const char *text, size_t length,
                     char *err, size_t err_len)
{
    json_error_t error;
    json_t *root = json_loadb(text, length, JSON_REJECT_DUPLICATES, &error);
    json_t *version = json_object_get(root, "record_version");
    json_t *ready = json_object_get(root, "ready_voters");
    json_t *stored = json_object_get(root, "checksum");
    const char *digest = json_string_value(stored);
    char expected[65], actual[65];
    bool valid = json_is_object(root) && json_object_size(root) == 6 &&
        json_is_integer(version) && json_integer_value(version) == 1 &&
        matches(root, "member", barrier->member) && matches(root, "membership", barrier->membership) &&
        matches(root, "baseline_digest", barrier->baseline) &&
        json_is_integer(ready) && json_integer_value(ready) == (json_int_t)all_members(barrier->base) &&
        digest && json_string_length(stored) == 64 && strlen(digest) == 64;
    if (valid)
    {
        memcpy(expected, digest, sizeof(expected));
        json_object_del(root, "checksum");
        valid = !checksum(root, actual) && !strcmp(expected, actual);
    }
    if (valid)
        barrier->ready = all_members(barrier->base);
    json_decref(root);
    return valid ? 0 : fail(err, err_len, "invalid, corrupt, or foreign durable barrier; recovery required");
}

int lcs_config_barrier_open(lcs_config_barrier_t *barrier,
                             const lcs_config_activation_t *activation,
                             const lcs_config_agreement_t *agreement,
                             const lcs_config_application_t *application,
                             const lcs_config_barrier_round_t *authorization,
                             char *err, size_t err_len)
{
    if (barrier->poisoned || !bound(activation, agreement, application))
        return fail(err, err_len, "valid frozen initialization and recovered local state are required");
    lcs_config_barrier_t next = { .store = activation->store, .base = activation->base };
    memcpy(next.member, activation->member, sizeof(next.member));
    memcpy(next.membership, activation->membership, sizeof(next.membership));
    memcpy(next.baseline, activation->baseline.digest, sizeof(next.baseline));
    char *text = NULL;
    size_t length;
    int result = lcs_config_store_read(next.store, "barrier.json", &text, &length,
                                       LCS_CONFIG_BARRIER_MAX, err, err_len);
    if (!result)
        result = decode(&next, text, length, err, err_len);
    else if (result == 1 && authorization)
    {
        if (!authorization->sealed || authorization->failed ||
            authorization->ready != all_members(next.base) || authorization->required != all_members(next.base) ||
            strcmp(authorization->membership, next.membership) || strcmp(authorization->baseline, next.baseline) ||
            !lcs_config_barrier_local_ready(activation, agreement, application))
            result = fail(err, err_len, "cannot create readiness without a complete matching round and pristine local state");
        else
        {
            json_t *root = json_pack("{s:i,s:s,s:s,s:s,s:I}", "record_version", 1,
                "member", next.member, "membership", next.membership, "baseline_digest", next.baseline,
                "ready_voters", (json_int_t)authorization->ready);
            char hash[65];
            if (root && !checksum(root, hash) && !json_object_set_new(root, "checksum", json_string(hash)))
                text = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
            json_decref(root);
            result = text && strlen(text) <= LCS_CONFIG_BARRIER_MAX ?
                lcs_config_store_write(next.store, "barrier.json", text, strlen(text), err, err_len) :
                fail(err, err_len, "cannot encode bounded barrier record");
            if (result)
                barrier->poisoned = true;
            else
                next.ready = authorization->ready;
        }
    }
    free(text);
    if (!result)
        *barrier = next;
    return result;
}

void lcs_config_barrier_close(lcs_config_barrier_t *barrier)
{
    memset(barrier, 0, sizeof(*barrier));
}

bool lcs_config_barrier_valid(const lcs_config_barrier_t *barrier,
                                   const lcs_config_activation_t *activation,
                                   const lcs_config_agreement_t *agreement,
                                   const lcs_config_application_t *application)
{
    return bound(activation, agreement, application) && !barrier->poisoned &&
        barrier->store == activation->store && barrier->base == activation->base &&
        barrier->ready == all_members(activation->base) &&
        !strcmp(barrier->member, activation->member) && !strcmp(barrier->membership, activation->membership) &&
        !strcmp(barrier->baseline, activation->baseline.digest);
}

bool lcs_config_barrier_can_vote(const lcs_config_barrier_t *barrier,
                                   const lcs_config_activation_t *activation,
                                   const lcs_config_agreement_t *agreement,
                                   const lcs_config_application_t *application)
{
    return lcs_config_barrier_valid(barrier, activation, agreement, application) && !application->pending.document &&
        lcs_config_value_equal(&application->applied, &agreement->committed) &&
        (agreement->committed.revision || lcs_config_value_equal(&activation->baseline, &agreement->committed));
}
