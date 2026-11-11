// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_mutation_protocol.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
int main(void)
{
    lcs_config_mutation_t *request = calloc(1, sizeof(*request)), *decoded = malloc(sizeof(*decoded));
    assert(request && decoded); strcpy(request->id, "edit-a"); request->argc = 4;
    request->argv[0] = "resource"; request->argv[1] = "update"; request->argv[2] = "vip1"; request->argv[3] = "home_node=";
    unsigned char payload[LCS_MAX_FRAME + 1], copy[LCS_MAX_FRAME + 1]; size_t length;
    assert(!lcs_config_mutation_encode(request, payload, sizeof(payload), &length));
    assert(!lcs_config_mutation_decode(payload, length, decoded));
    assert(!decoded->revision_set && !decoded->digest_set && !strcmp(decoded->argv[3], "home_node="));
    for (size_t n = 0; n < length; n++) assert(lcs_config_mutation_decode(payload, n, decoded) < 0);
    memcpy(copy, payload, length); copy[length] = 0; assert(lcs_config_mutation_decode(copy, length + 1, decoded) < 0);
    copy[1] = 2; assert(lcs_config_mutation_decode(copy, length, decoded) < 0);
    memcpy(copy, payload, length); copy[2 + strlen(request->id) + 1] = 'x';
    assert(lcs_config_mutation_decode(copy, length, decoded) < 0); /* Noncanonical fixed padding. */
    memcpy(copy, payload, length); copy[66] = 4; assert(lcs_config_mutation_decode(copy, length, decoded) < 0);
    memcpy(copy, payload, length); copy[74] = 1; assert(lcs_config_mutation_decode(copy, length, decoded) < 0); /* Unset revision must be zero. */
    memcpy(copy, payload, length); copy[length - 1] = 0; assert(lcs_config_mutation_decode(copy, length, decoded) < 0);
    request->revision_set = request->digest_set = true; request->revision = 7;
    memset(request->digest, 'a', 64); request->digest[64] = 0;
    assert(!lcs_config_mutation_encode(request, payload, sizeof(payload), &length));
    assert(!lcs_config_mutation_decode(payload, length, decoded) && decoded->revision == 7 && decoded->digest_set);
    request->digest[0] = 'A'; assert(lcs_config_mutation_encode(request, payload, sizeof(payload), &length) < 0); request->digest[0] = 'a';
    request->argc = 65; assert(lcs_config_mutation_encode(request, payload, sizeof(payload), &length) < 0);
    request->argc = 4; request->argv[3] = NULL; assert(lcs_config_mutation_encode(request, payload, sizeof(payload), &length) < 0);
    free(request); free(decoded);

    lcs_config_mutation_result_t result = {.result = LCS_CONFIG_MUTATION_UNKNOWN}; strcpy(result.request.id, "edit-a"); strcpy(result.reason, "outcome unknown");
    lcs_config_mutation_result_t outcome;
    assert(!lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length));
    assert(length == LCS_CONFIG_MUTATION_RESPONSE_SIZE && !lcs_config_mutation_result_decode(payload, length, &outcome));
    for (size_t n = 0; n < length; n++) assert(lcs_config_mutation_result_decode(payload, n, &outcome) < 0);
    assert(lcs_config_mutation_result_decode(payload, length + 1, &outcome) < 0);
    result.result = LCS_CONFIG_MUTATION_APPLIED; assert(lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length) < 0);
    result.result = LCS_CONFIG_MUTATION_PENDING; assert(lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length) < 0);
    result.request.history = LCS_CONFIG_HISTORY_AVAILABLE; result.request.state = LCS_CONFIG_REQUEST_COMMITTED;
    memset(result.request.base_digest, 'b', 64); memset(result.request.value_digest, 'c', 64);
    assert(!lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length));
    assert(!lcs_config_mutation_result_decode(payload, length, &outcome) && outcome.result == LCS_CONFIG_MUTATION_PENDING);
    result.request.state = LCS_CONFIG_REQUEST_APPLIED; result.result = LCS_CONFIG_MUTATION_APPLIED;
    assert(!lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length));
    assert(!lcs_config_mutation_result_decode(payload, length, &outcome) && outcome.request.state == LCS_CONFIG_REQUEST_APPLIED);
    payload[2] = LCS_CONFIG_MUTATION_REJECTED; assert(lcs_config_mutation_result_decode(payload, length, &outcome) < 0);
    result.result = LCS_CONFIG_MUTATION_REJECTED; assert(lcs_config_mutation_result_encode(&result, payload, sizeof(payload), &length) < 0);
    puts("bounded definition mutation request/result codec unit tests passed");
    return 0;
}
