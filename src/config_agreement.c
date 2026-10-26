// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_agreement.h"
#include "util.h"

#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define JOURNAL "agreement.json"

static int checked_successor(const lcs_config_agreement_t *agreement,
                               const lcs_config_value_t *value, char *err, size_t err_len);

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration agreement: %s", message);
    return -1;
}

static int hash_json(json_t *object, char digest[LCS_CONFIG_DIGEST_SIZE])
{
    char *canonical = json_dumps(object, JSON_SORT_KEYS | JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!canonical)
        return -1;
    unsigned char hash[SHA256_DIGEST_LENGTH];
    unsigned char *result = SHA256((const unsigned char *)canonical, strlen(canonical), hash);
    free(canonical);
    if (!result)
        return -1;
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(hash); i++)
    {
        digest[i * 2] = hex[hash[i] >> 4];
        digest[i * 2 + 1] = hex[hash[i] & 15];
    }
    digest[64] = '\0';
    return 0;
}

static bool digest_valid(const char *value)
{
    if (!value || strlen(value) != 64)
        return false;
    for (size_t i = 0; i < 64; i++)
        if (!((value[i] >= '0' && value[i] <= '9') ||
              (value[i] >= 'a' && value[i] <= 'f')))
            return false;
    return true;
}

static bool exact_keys(json_t *object, const char *const *keys)
{
    if (!json_is_object(object))
        return false;
    size_t count = 0;
    while (keys[count])
    {
        if (!json_object_get(object, keys[count]))
            return false;
        count++;
    }
    return json_object_size(object) == count;
}

static json_t *value_object(const lcs_config_value_t *value)
{
    json_error_t error;
    json_t *snapshot = json_loadb(value->document, value->length, JSON_REJECT_DUPLICATES, &error);
    if (!snapshot)
        return NULL;
    json_t *object = json_pack("{s:s,s:o}", "request_id", value->request_id, "snapshot", snapshot);
    return object;
}

void lcs_config_value_clear(lcs_config_value_t *value)
{
    free(value->document);
    memset(value, 0, sizeof(*value));
}

int lcs_config_value_copy(lcs_config_value_t *dst, const lcs_config_value_t *src)
{
    lcs_config_value_t next = *src;
    next.document = NULL;
    if (src->document)
    {
        next.document = malloc(src->length + 1);
        if (!next.document)
            return -1;
        memcpy(next.document, src->document, src->length);
        next.document[src->length] = '\0';
    }
    *dst = next;
    return 0;
}

bool lcs_config_value_equal(const lcs_config_value_t *a, const lcs_config_value_t *b)
{
    return a->document && b->document && a->revision == b->revision &&
           a->length == b->length && !strcmp(a->request_id, b->request_id) &&
           !strcmp(a->digest, b->digest) && !memcmp(a->document, b->document, a->length);
}

int lcs_config_value_init(lcs_config_value_t *value, const lcs_config_t *base,
                          const char *request_id, const char *document, size_t length,
                          char *err, size_t err_len)
{
    if (!request_id || !lcs_valid_name(request_id) || strlen(request_id) > LCS_CONFIG_REQUEST_MAX)
        return fail(err, err_len, "invalid request identifier");
    lcs_config_t *candidate = malloc(sizeof(*candidate));
    if (!candidate)
        return fail(err, err_len, "out of memory validating proposal");
    lcs_config_value_t next = {0};
    int rc = lcs_resource_config_decode(document, length, base, candidate, &next.revision, err, err_len);
    if (!rc)
        rc = lcs_resource_config_encode(candidate, next.revision,
                                        &next.document, &next.length, err, err_len);
    free(candidate);
    if (rc)
        return -1;
    snprintf(next.request_id, sizeof(next.request_id), "%s", request_id);
    json_t *object = value_object(&next);
    rc = object ? hash_json(object, next.digest) : -1;
    json_decref(object);
    if (rc)
    {
        lcs_config_value_clear(&next);
        return fail(err, err_len, "cannot hash canonical proposal");
    }
    *value = next;
    return 0;
}

static int parse_value(json_t *object, const lcs_config_t *base,
                        lcs_config_value_t *value, char *err, size_t err_len)
{
    static const char *const keys[] = {"request_id", "snapshot", NULL};
    const char *request = json_string_value(json_object_get(object, "request_id"));
    if (!exact_keys(object, keys) || !request)
        return fail(err, err_len, "invalid journal value");
    char *document = json_dumps(json_object_get(object, "snapshot"), JSON_COMPACT);
    if (!document)
        return fail(err, err_len, "cannot decode journal snapshot");
    int rc = lcs_config_value_init(value, base, request, document, strlen(document), err, err_len);
    free(document);
    return rc;
}

static int membership_digest(const lcs_config_t *base, char digest[LCS_CONFIG_DIGEST_SIZE])
{
    json_t *nodes = json_array();
    json_t *root = json_pack("{s:s,s:I,s:I,s:I}", "cluster", base->cluster_name,
        "lease_ms", (json_int_t)base->lease_ms, "renew_ms", (json_int_t)base->renew_ms,
        "peer_timeout_ms", (json_int_t)base->peer_timeout_ms);
    int rc = -1;
    if (!nodes || !root)
        goto out;
    for (size_t i = 0; i < base->node_count; i++)
        if (json_array_append_new(nodes, json_pack("{s:s,s:i}", "name", base->nodes[i].name,
            "role", (int)base->nodes[i].role)))
            goto out;
    if (json_object_set(root, "nodes", nodes))
        goto out;
    rc = hash_json(root, digest);
out:
    json_decref(nodes);
    json_decref(root);
    return rc;
}

static bool quorum(const lcs_config_agreement_t *agreement, uint32_t voters)
{
    size_t count = agreement->base->node_count;
    uint32_t valid = count == 32 ? UINT32_MAX : (UINT32_C(1) << count) - 1;
    if (voters & ~valid)
        return false;
    unsigned votes = 0;
    bool full = false;
    for (size_t i = 0; i < count; i++)
        if (voters & (UINT32_C(1) << i))
        {
            votes++;
            if (agreement->base->nodes[i].role == LCS_NODE_FULL)
                full = true;
        }
    return full && votes >= lcs_config_quorum(agreement->base);
}

bool lcs_config_agreement_ballot_valid(const lcs_config_agreement_t *agreement,
                                      uint64_t ballot)
{
    return ballot >> 5 && (ballot & 31) < agreement->base->node_count;
}

uint64_t lcs_config_agreement_next_ballot(const lcs_config_agreement_t *agreement,
                                         uint64_t observed)
{
    if (agreement->poisoned)
        return 0;
    uint64_t maximum = observed > agreement->promised ? observed : agreement->promised;
    uint64_t counter = maximum >> 5;
    int index = lcs_config_self_index(agreement->base);
    if (index < 0 || counter == (UINT64_MAX >> 5))
        return 0;
    return ((counter + 1) << 5) | (uint64_t)index;
}

static json_t *u64_json(uint64_t number)
{
    char text[32];
    snprintf(text, sizeof(text), "%llu", (unsigned long long)number);
    return json_string(text);
}

static int get_u64(json_t *object, const char *key, uint64_t *number)
{
    const char *value = json_string_value(json_object_get(object, key));
    if (!value || !*value || (value[0] == '0' && value[1]))
        return -1;
    uint64_t result = 0;
    for (const char *p = value; *p; p++)
    {
        if (*p < '0' || *p > '9' || result > (UINT64_MAX - (unsigned)(*p - '0')) / 10)
            return -1;
        result = result * 10 + (unsigned)(*p - '0');
    }
    *number = result;
    return 0;
}

int lcs_config_agreement_export(const lcs_config_agreement_t *agreement,
                                char **result, size_t *length,
                                char *err, size_t err_len)
{
    if (agreement->poisoned)
        return fail(err, err_len, "cannot export poisoned state; recovery required");
    json_t *root = json_pack("{s:i,s:s,s:s,s:s,s:I}", "record_version", 1,
        "member", agreement->member, "membership", agreement->membership,
        "parent_digest", agreement->parent_digest,
        "committed_voters", (json_int_t)agreement->committed_voters);
    char *document = NULL;
    int rc = -1;
    if (!root ||
        json_object_set_new(root, "committed", value_object(&agreement->committed)) ||
        json_object_set_new(root, "committed_ballot", u64_json(agreement->committed_ballot)) ||
        json_object_set_new(root, "promised", u64_json(agreement->promised)) ||
        json_object_set_new(root, "accepted_ballot", u64_json(agreement->accepted_ballot)) ||
        json_object_set_new(root, "accepted", agreement->accepted.document ?
            value_object(&agreement->accepted) : json_null()))
        goto memory;
    char checksum[LCS_CONFIG_DIGEST_SIZE];
    if (hash_json(root, checksum) || json_object_set_new(root, "checksum", json_string(checksum)))
        goto memory;
    document = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    if (!document || strlen(document) > LCS_CONFIG_JOURNAL_MAX)
        goto memory;
    *result = document;
    *length = strlen(document);
    document = NULL;
    rc = 0;
    goto out;
memory:
    fail(err, err_len, "cannot encode bounded agreement record");
out:
    free(document);
    json_decref(root);
    return rc;
}

static int save(lcs_config_agreement_t *agreement, char *err, size_t err_len)
{
    char *document;
    size_t length;
    if (lcs_config_agreement_export(agreement, &document, &length, err, err_len))
        return -1;
    int rc = lcs_config_store_write(agreement->store, JOURNAL, document, length, err, err_len);
    free(document);
    return rc;
}

void lcs_config_agreement_close(lcs_config_agreement_t *agreement)
{
    lcs_config_value_clear(&agreement->committed);
    lcs_config_value_clear(&agreement->accepted);
    memset(agreement, 0, sizeof(*agreement));
}

static int clone(lcs_config_agreement_t *dst, const lcs_config_agreement_t *src)
{
    *dst = *src;
    dst->committed.document = dst->accepted.document = NULL;
    if (lcs_config_value_copy(&dst->committed, &src->committed) ||
        lcs_config_value_copy(&dst->accepted, &src->accepted))
    {
        lcs_config_agreement_close(dst);
        return -1;
    }
    return 0;
}

static int publish(lcs_config_agreement_t *current, lcs_config_agreement_t *next,
                     char *err, size_t err_len)
{
    if (save(next, err, err_len))
    {
        lcs_config_agreement_close(next);
        current->poisoned = true;
        return -1;
    }
    lcs_config_agreement_close(current);
    *current = *next;
    memset(next, 0, sizeof(*next));
    return LCS_CONFIG_AGREED;
}

static int load(lcs_config_agreement_t *agreement, const char *document, size_t length,
                  char *err, size_t err_len)
{
    static const char *const keys[] = {"record_version", "member", "membership",
        "parent_digest", "committed_voters", "committed", "committed_ballot",
        "promised", "accepted_ballot", "accepted", "checksum", NULL};
    json_error_t error;
    json_t *root = json_loadb(document, length, JSON_REJECT_DUPLICATES, &error);
    int rc = -1;
    if (!exact_keys(root, keys))
        goto invalid;
    const char *checksum = json_string_value(json_object_get(root, "checksum"));
    char expected[LCS_CONFIG_DIGEST_SIZE], actual[LCS_CONFIG_DIGEST_SIZE];
    if (!digest_valid(checksum))
        goto invalid;
    snprintf(expected, sizeof(expected), "%s", checksum);
    json_object_del(root, "checksum");
    if (hash_json(root, actual) || strcmp(expected, actual))
        goto invalid;
    const char *member = json_string_value(json_object_get(root, "member"));
    const char *membership = json_string_value(json_object_get(root, "membership"));
    const char *parent = json_string_value(json_object_get(root, "parent_digest"));
    json_t *format = json_object_get(root, "record_version");
    json_t *voters = json_object_get(root, "committed_voters");
    if (!json_is_integer(format) || json_integer_value(format) != 1 ||
        !member || strcmp(member, agreement->member) ||
        !membership || strcmp(membership, agreement->membership) ||
        !parent || (*parent && !digest_valid(parent)) ||
        !json_is_integer(voters) || json_integer_value(voters) < 0 ||
        (uint64_t)json_integer_value(voters) > UINT32_MAX ||
        get_u64(root, "committed_ballot", &agreement->committed_ballot) ||
        get_u64(root, "promised", &agreement->promised) ||
        get_u64(root, "accepted_ballot", &agreement->accepted_ballot))
        goto invalid;
    snprintf(agreement->parent_digest, sizeof(agreement->parent_digest), "%s", parent);
    agreement->committed_voters = (uint32_t)json_integer_value(voters);
    if (parse_value(json_object_get(root, "committed"), agreement->base,
                     &agreement->committed, err, err_len))
        goto out;
    if (agreement->committed.revision == 0)
    {
        if (*parent || agreement->committed_ballot || agreement->committed_voters ||
            strcmp(agreement->committed.request_id, "bootstrap"))
            goto invalid;
    }
    else if (!digest_valid(parent) || !lcs_config_agreement_ballot_valid(agreement, agreement->committed_ballot) ||
             !quorum(agreement, agreement->committed_voters) || agreement->promised < agreement->committed_ballot)
        goto invalid;
    if (agreement->promised && !lcs_config_agreement_ballot_valid(agreement, agreement->promised))
        goto invalid;
    json_t *accepted = json_object_get(root, "accepted");
    if (!json_is_null(accepted))
    {
        if (parse_value(accepted, agreement->base, &agreement->accepted, err, err_len))
            goto out;
        if (!lcs_config_agreement_ballot_valid(agreement, agreement->accepted_ballot) ||
            agreement->accepted_ballot > agreement->promised ||
            agreement->accepted_ballot < agreement->committed_ballot ||
            agreement->accepted.revision != agreement->committed.revision + 1 ||
            checked_successor(agreement, &agreement->accepted, err, err_len))
            goto invalid;
    }
    else if (agreement->accepted_ballot)
        goto invalid;
    rc = 0;
    goto out;
invalid:
    fail(err, err_len, "invalid, corrupt, or foreign agreement journal; recovery required");
out:
    json_decref(root);
    return rc;
}

int lcs_config_agreement_open(lcs_config_agreement_t *agreement,
                              lcs_config_store_t *store, const lcs_config_t *base,
                              bool allow_initialize, char *err, size_t err_len)
{
    lcs_config_agreement_t next = { .store = store, .base = base };
    if (!base->node_count || base->node_count > LCS_MAX_NODES)
        return fail(err, err_len, "invalid agreement membership");
    int self = lcs_config_self_index(base);
    if (self < 0)
        return fail(err, err_len, "local member is absent from agreement membership");
    for (size_t i = 0; i < base->node_count; i++)
        if (!lcs_valid_name(base->nodes[i].name) ||
            (base->nodes[i].role != LCS_NODE_FULL && base->nodes[i].role != LCS_NODE_QUORUM_ONLY) ||
            (i && strcmp(base->nodes[i - 1].name, base->nodes[i].name) >= 0))
            return fail(err, err_len, "agreement membership must use unique canonical sorted node names and valid roles");
    if (membership_digest(base, next.membership))
        return fail(err, err_len, "cannot compute agreement membership identity");
    snprintf(next.member, sizeof(next.member), "%s", base->nodes[self].name);
    char *document = NULL;
    size_t length;
    int status = lcs_config_store_read(store, JOURNAL, &document, &length,
                                       LCS_CONFIG_JOURNAL_MAX, err, err_len);
    int rc = -1;
    if (status == 0)
        rc = load(&next, document, length, err, err_len);
    else if (status == 1 && !allow_initialize)
        fail(err, err_len, "initialized agreement journal is missing; recover instead of resetting promises");
    else if (status == 1)
    {
        char *bootstrap = NULL;
        size_t bootstrap_len;
        if (!lcs_resource_config_encode(base, 0, &bootstrap, &bootstrap_len, err, err_len))
        {
            if (!lcs_config_value_init(&next.committed, base, "bootstrap", bootstrap,
                                       bootstrap_len, err, err_len))
                rc = save(&next, err, err_len);
            free(bootstrap);
        }
    }
    free(document);
    if (rc)
    {
        lcs_config_agreement_close(&next);
        return -1;
    }
    *agreement = next;
    return 0;
}

int lcs_config_agreement_import(lcs_config_agreement_t *agreement,
                                const char *document, size_t length,
                                char *err, size_t err_len)
{
    if (agreement->poisoned)
        return fail(err, err_len, "poisoned state requires disk recovery, not a worker replay");
    lcs_config_agreement_t next = { .base = agreement->base, .store = agreement->store };
    snprintf(next.member, sizeof(next.member), "%s", agreement->member);
    snprintf(next.membership, sizeof(next.membership), "%s", agreement->membership);
    if (!length || length > LCS_CONFIG_JOURNAL_MAX || load(&next, document, length, err, err_len))
    {
        lcs_config_agreement_close(&next);
        agreement->poisoned = true;
        return fail(err, err_len, "cannot restore local worker state; recovery required");
    }
    bool same_revision = next.committed.revision == agreement->committed.revision;
    if (next.promised < agreement->promised ||
        next.committed.revision < agreement->committed.revision ||
        next.committed.revision > agreement->committed.revision + 1 ||
        (same_revision && (!lcs_config_value_equal(&next.committed, &agreement->committed) ||
            next.accepted_ballot < agreement->accepted_ballot ||
            (next.accepted_ballot && next.accepted_ballot == agreement->accepted_ballot &&
             !lcs_config_value_equal(&next.accepted, &agreement->accepted)))) ||
        (!same_revision && (strcmp(next.parent_digest, agreement->committed.digest) ||
            checked_successor(agreement, &next.committed, err, err_len))))
    {
        lcs_config_agreement_close(&next);
        agreement->poisoned = true;
        return fail(err, err_len, "worker result would replace newer durable state; recovery required");
    }
    lcs_config_agreement_close(agreement);
    *agreement = next;
    return 0;
}

static int slot_check(lcs_config_agreement_t *agreement, uint64_t revision,
                       const char *parent, uint64_t ballot, char *err, size_t err_len)
{
    if (agreement->poisoned)
        return fail(err, err_len, "storage outcome uncertain; reopen/recover before voting");
    if (!lcs_config_agreement_ballot_valid(agreement, ballot) || !digest_valid(parent))
        return fail(err, err_len, "invalid ballot or parent digest");
    if (revision <= agreement->committed.revision)
        return LCS_CONFIG_STALE;
    if (revision > LCS_RESOURCE_REVISION_MAX || revision != agreement->committed.revision + 1 ||
        strcmp(parent, agreement->committed.digest))
        return LCS_CONFIG_REJECTED;
    return LCS_CONFIG_AGREED;
}

int lcs_config_agreement_prepare(lcs_config_agreement_t *agreement,
                                 uint64_t revision, const char *parent_digest,
                                 uint64_t ballot, char *err, size_t err_len)
{
    int rc = slot_check(agreement, revision, parent_digest, ballot, err, err_len);
    if (rc)
        return rc;
    if (ballot < agreement->promised)
        return LCS_CONFIG_REJECTED;
    if (ballot == agreement->promised)
        return LCS_CONFIG_AGREED;
    lcs_config_agreement_t next;
    if (clone(&next, agreement))
        return fail(err, err_len, "out of memory preparing promise");
    next.promised = ballot;
    return publish(agreement, &next, err, err_len);
}

static int checked_successor(const lcs_config_agreement_t *agreement,
                               const lcs_config_value_t *value, char *err, size_t err_len)
{
    lcs_config_t *previous = malloc(sizeof(*previous));
    lcs_config_t *candidate = malloc(sizeof(*candidate));
    if (!previous || !candidate)
    {
        free(previous);
        free(candidate);
        return fail(err, err_len, "out of memory validating incarnation history");
    }
    uint64_t before = 0, after = 0;
    int result = lcs_resource_config_decode(agreement->committed.document,
        agreement->committed.length, agreement->base, previous, &before, err, err_len);
    if (!result)
        result = lcs_resource_config_decode(value->document, value->length,
            agreement->base, candidate, &after, err, err_len);
    if (!result)
        result = lcs_resource_config_validate_successor(previous, before, candidate,
                                                          after, err, err_len);
    free(previous);
    free(candidate);
    return result;
}

static int checked_value(const lcs_config_agreement_t *agreement,
                           const lcs_config_value_t *value, char *err, size_t err_len)
{
    lcs_config_value_t actual = {0};
    if (lcs_config_value_init(&actual, agreement->base, value->request_id,
                              value->document, value->length, err, err_len))
        return -1;
    bool same = lcs_config_value_equal(&actual, value);
    lcs_config_value_clear(&actual);
    return same ? 0 : fail(err, err_len, "proposal is not canonical or its identity is invalid");
}

int lcs_config_agreement_accept(lcs_config_agreement_t *agreement,
                                const char *parent_digest, uint64_t ballot,
                                const lcs_config_value_t *value,
                                char *err, size_t err_len)
{
    int rc = slot_check(agreement, value->revision, parent_digest, ballot, err, err_len);
    if (rc)
        return rc;
    if (ballot < agreement->promised)
        return LCS_CONFIG_REJECTED;
    if (checked_value(agreement, value, err, err_len) ||
        checked_successor(agreement, value, err, err_len))
        return -1;
    if (agreement->accepted_ballot == ballot)
        return lcs_config_value_equal(&agreement->accepted, value) ?
               LCS_CONFIG_AGREED : LCS_CONFIG_REJECTED;
    lcs_config_agreement_t next;
    if (clone(&next, agreement))
        return fail(err, err_len, "out of memory accepting proposal");
    lcs_config_value_clear(&next.accepted);
    if (lcs_config_value_copy(&next.accepted, value))
    {
        lcs_config_agreement_close(&next);
        return fail(err, err_len, "out of memory accepting proposal");
    }
    next.promised = next.accepted_ballot = ballot;
    return publish(agreement, &next, err, err_len);
}

int lcs_config_agreement_learn(lcs_config_agreement_t *agreement,
                               const char *parent_digest, uint64_t ballot,
                               uint32_t voters, const lcs_config_value_t *value,
                               char *err, size_t err_len)
{
    if (!quorum(agreement, voters) || !digest_valid(parent_digest) ||
        !lcs_config_agreement_ballot_valid(agreement, ballot))
        return fail(err, err_len, "decision lacks a valid voting majority");
    int rc = slot_check(agreement, value->revision, parent_digest, ballot, err, err_len);
    if (rc == LCS_CONFIG_STALE)
    {
        if (value->revision == agreement->committed.revision)
            return lcs_config_value_equal(value, &agreement->committed) &&
                   !strcmp(parent_digest, agreement->parent_digest) ?
                   LCS_CONFIG_AGREED : LCS_CONFIG_REJECTED;
        return rc;
    }
    if (rc)
        return rc;
    if (checked_value(agreement, value, err, err_len) ||
        checked_successor(agreement, value, err, err_len))
        return -1;
    if (agreement->accepted_ballot >= ballot &&
        !lcs_config_value_equal(&agreement->accepted, value))
        return fail(err, err_len, "decision conflicts with equal/newer accepted value; recovery required");
    lcs_config_agreement_t next;
    if (clone(&next, agreement))
        return fail(err, err_len, "out of memory learning decision");
    lcs_config_value_clear(&next.committed);
    if (lcs_config_value_copy(&next.committed, value))
    {
        lcs_config_agreement_close(&next);
        return fail(err, err_len, "out of memory learning decision");
    }
    snprintf(next.parent_digest, sizeof(next.parent_digest), "%s", parent_digest);
    next.committed_ballot = ballot;
    next.committed_voters = voters;
    if (next.promised < ballot)
        next.promised = ballot;
    next.accepted_ballot = 0;
    lcs_config_value_clear(&next.accepted);
    return publish(agreement, &next, err, err_len);
}

int lcs_config_proposal_init(lcs_config_proposal_t *proposal,
                             const lcs_config_agreement_t *local, uint64_t ballot,
                             const lcs_config_value_t *requested)
{
    lcs_config_proposal_t next = { .revision = requested->revision, .ballot = ballot };
    if (local->poisoned || requested->revision != local->committed.revision + 1 ||
        !lcs_config_agreement_ballot_valid(local, ballot) ||
        (ballot & 31) != (uint64_t)lcs_config_self_index(local->base) ||
        checked_value(local, requested, NULL, 0) ||
        checked_successor(local, requested, NULL, 0) ||
        lcs_config_value_copy(&next.requested, requested))
        return -1;
    snprintf(next.parent_digest, sizeof(next.parent_digest), "%s", local->committed.digest);
    *proposal = next;
    return 0;
}

void lcs_config_proposal_clear(lcs_config_proposal_t *proposal)
{
    lcs_config_value_clear(&proposal->requested);
    lcs_config_value_clear(&proposal->recovered);
    memset(proposal, 0, sizeof(*proposal));
}

int lcs_config_proposal_promise(lcs_config_proposal_t *proposal,
                                const lcs_config_agreement_t *membership,
                                size_t member, uint64_t revision,
                                const char *parent_digest, uint64_t promised,
                                uint64_t accepted_ballot,
                                const lcs_config_value_t *accepted)
{
    if (proposal->failed || proposal->sealed || member >= membership->base->node_count ||
        !parent_digest || revision != proposal->revision || promised != proposal->ballot ||
        strcmp(parent_digest, proposal->parent_digest))
        return -1;
    uint32_t bit = UINT32_C(1) << member;
    if (proposal->promises & bit)
        return LCS_CONFIG_REJECTED;
    if (accepted_ballot)
    {
        if (!accepted || !accepted->document || accepted->revision != revision ||
            accepted_ballot > promised || !lcs_config_agreement_ballot_valid(membership, accepted_ballot) ||
            checked_value(membership, accepted, NULL, 0))
            return -1;
        if (accepted_ballot == proposal->highest_accepted &&
            !lcs_config_value_equal(&proposal->recovered, accepted))
        {
            proposal->failed = true;
            return -1;
        }
        if (accepted_ballot > proposal->highest_accepted)
        {
            lcs_config_value_t recovered = {0};
            if (lcs_config_value_copy(&recovered, accepted))
                return -1;
            lcs_config_value_clear(&proposal->recovered);
            proposal->recovered = recovered;
            proposal->highest_accepted = accepted_ballot;
        }
    }
    else if (accepted && accepted->document)
        return -1;
    proposal->promises |= bit;
    return LCS_CONFIG_AGREED;
}

const lcs_config_value_t *lcs_config_proposal_seal(lcs_config_proposal_t *proposal,
                                                 const lcs_config_agreement_t *membership)
{
    if (proposal->failed || !quorum(membership, proposal->promises))
        return NULL;
    proposal->sealed = true;
    return proposal->highest_accepted ? &proposal->recovered : &proposal->requested;
}

int lcs_config_proposal_ack(lcs_config_proposal_t *proposal,
                            const lcs_config_agreement_t *membership, size_t member,
                            uint64_t ballot, const lcs_config_value_t *value)
{
    if (proposal->failed || !proposal->sealed || member >= membership->base->node_count ||
        proposal->ballot != ballot || !lcs_config_value_equal(value,
            proposal->highest_accepted ? &proposal->recovered : &proposal->requested))
        return -1;
    uint32_t bit = UINT32_C(1) << member;
    if (proposal->acknowledgements & bit)
        return LCS_CONFIG_REJECTED;
    proposal->acknowledgements |= bit;
    return LCS_CONFIG_AGREED;
}

bool lcs_config_proposal_chosen(const lcs_config_proposal_t *proposal,
                                const lcs_config_agreement_t *membership)
{
    return proposal->sealed && !proposal->failed && quorum(membership, proposal->acknowledgements);
}
