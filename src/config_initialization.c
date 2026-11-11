// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_initialization.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(lcs_config_initialization_round_t *round, char *err, size_t len, const char *message)
{
    if (round) round->failed = true;
    if (err && len) snprintf(err, len, "configuration initialization round: %s", message);
    return -1;
}

static int binding(const lcs_config_t *cfg, char membership[LCS_CONFIG_DIGEST_SIZE],
                   char baseline[LCS_CONFIG_DIGEST_SIZE], char *err, size_t err_len)
{
    char *document = NULL;
    size_t length;
    lcs_config_value_t value = {0};
    int result = lcs_config_agreement_membership(cfg, membership);
    if (!result) result = lcs_resource_config_encode(cfg, 0, &document, &length, err, err_len);
    if (!result) result = lcs_config_value_init(&value, cfg, "bootstrap", document, length, err, err_len);
    if (!result) memcpy(baseline, value.digest, LCS_CONFIG_DIGEST_SIZE);
    lcs_config_value_clear(&value);
    free(document);
    return result;
}

int lcs_config_initialization_begin(lcs_config_initialization_round_t *round,
    const lcs_config_t *cfg, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t attempt, uint64_t now_ms, char *err, size_t err_len)
{
    if (!round || !cfg || !instances || !cfg->node_count || cfg->node_count > LCS_MAX_NODES)
        return fail(NULL, err, err_len, "invalid membership/session view");
    int self = lcs_config_self_index(cfg);
    if (self < 0 || !attempt || now_ms > UINT64_MAX - LCS_CONFIG_INITIALIZATION_TIMEOUT_MS)
        return fail(NULL, err, err_len, "invalid local member, attempt or deadline");
    if (round->active && ((!round->failed && !round->consumed && now_ms < round->deadline_ms) ||
        (round->instances[round->self] == instances[self] && attempt <= round->attempt)))
        return fail(NULL, err, err_len, "active or reused initialization attempt");
    for (size_t i = 0; i < cfg->node_count; i++)
        if (!instances[i])
            return fail(NULL, err, err_len, "every configured member must have a current upgrade-capable instance");
    lcs_config_initialization_round_t next = { .active = true, .self = (size_t)self,
        .count = cfg->node_count, .attempt = attempt, .local_generation = generation,
        .deadline_ms = now_ms + LCS_CONFIG_INITIALIZATION_TIMEOUT_MS,
        .required = cfg->node_count == 32 ? UINT32_MAX : (UINT32_C(1) << cfg->node_count) - 1,
        .frozen = UINT32_C(1) << self };
    if (binding(cfg, next.membership, next.baseline, err, err_len)) return -1;
    memcpy(next.instances, instances, cfg->node_count * sizeof(*instances));
    next.sealed = next.frozen == next.required;
    *round = next;
    return 0;
}

static int usable(lcs_config_initialization_round_t *round, uint64_t now_ms, char *err, size_t len)
{
    if (!round || !round->active || round->failed || round->consumed || now_ms >= round->deadline_ms)
        return fail(round, err, len, "inactive, failed, consumed or expired attempt");
    return 0;
}

int lcs_config_initialization_query(lcs_config_initialization_round_t *round,
    size_t node, uint64_t admitted_instance, uint64_t now_ms,
    lcs_config_initialization_receipt_t *query, char *err, size_t err_len)
{
    if (usable(round, now_ms, err, err_len)) return -1;
    if (!query || node >= round->count || node == round->self ||
        !admitted_instance || admitted_instance != round->instances[node])
        return fail(round, err, err_len, "query requires the original admitted peer instance");
    uint32_t bit = UINT32_C(1) << node;
    if (round->frozen & bit) return 1;
    lcs_config_initialization_receipt_t next = {
        .requester_instance = round->instances[round->self], .attempt = round->attempt,
        .responder_instance = admitted_instance };
    memcpy(next.membership, round->membership, sizeof(next.membership));
    memcpy(next.baseline, round->baseline, sizeof(next.baseline));
    *query = next;
    round->queried |= bit;
    return 0;
}

int lcs_config_initialization_receipt(lcs_config_initialization_round_t *round,
    size_t node, uint64_t admitted_instance,
    const lcs_config_initialization_receipt_t *receipt, uint64_t now_ms,
    char *err, size_t err_len)
{
    if (!receipt || receipt->status < LCS_INITIALIZATION_FROZEN || receipt->status > LCS_INITIALIZATION_MISMATCH ||
        !receipt->requester_instance || !receipt->attempt || !receipt->responder_instance)
        return fail(NULL, err, err_len, "invalid freeze receipt");
    if (!round || !round->active || node >= round->count || node == round->self ||
        !(round->queried & (UINT32_C(1) << node)) ||
        receipt->requester_instance != round->instances[round->self] || receipt->attempt != round->attempt ||
        receipt->responder_instance != admitted_instance || admitted_instance != round->instances[node])
        return 1;
    if (usable(round, now_ms, err, err_len)) return -1;
    if (memcmp(receipt->membership, round->membership, sizeof(round->membership)) ||
        memcmp(receipt->baseline, round->baseline, sizeof(round->baseline)) || receipt->status == LCS_INITIALIZATION_MISMATCH)
        return fail(round, err, err_len, "conflicting current membership or effective resource definitions");
    uint32_t bit = UINT32_C(1) << node;
    if (receipt->status == LCS_INITIALIZATION_BUSY)
        return round->frozen & bit ? fail(round, err, err_len, "confirmed member withdrew its freeze") : 0;
    round->frozen |= bit;
    round->sealed = round->frozen == round->required;
    return 0;
}

int lcs_config_initialization_poll(lcs_config_initialization_round_t *round,
    const uint64_t instances[LCS_MAX_NODES], uint64_t generation, uint64_t now_ms,
    char *err, size_t err_len)
{
    if (usable(round, now_ms, err, err_len)) return -1;
    if (!instances || generation != round->local_generation)
        return fail(round, err, err_len, "local reload generation changed");
    for (size_t i = 0; i < round->count; i++)
        if (!instances[i] || instances[i] != round->instances[i])
            return fail(round, err, err_len, "configured member disconnected, restarted or lost capability");
    return round->sealed && round->frozen == round->required ? 1 : 0;
}

int lcs_config_initialization_authorize(lcs_config_initialization_round_t *round,
    const lcs_config_t *cfg, const uint64_t instances[LCS_MAX_NODES],
    uint64_t generation, uint64_t now_ms, char *err, size_t err_len)
{
    if (lcs_config_initialization_poll(round, instances, generation, now_ms, err, err_len) != 1)
        return fail(round, err, err_len, "complete current all-member freeze is required");
    char membership[LCS_CONFIG_DIGEST_SIZE], baseline[LCS_CONFIG_DIGEST_SIZE];
    if (!cfg || cfg->node_count != round->count || lcs_config_self_index(cfg) != (int)round->self ||
        binding(cfg, membership, baseline, err, err_len) || strcmp(membership, round->membership) ||
        strcmp(baseline, round->baseline))
        return fail(round, err, err_len, "effective configuration changed before initialization");
    return 0;
}
