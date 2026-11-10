// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_requests.h"
#include "util.h"
#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t size, const char *message)
{ if (err && size) snprintf(err, size, "configuration request history: %s", message); return -1; }

static bool digest_valid(const char *text)
{
    if (strnlen(text, LCS_CONFIG_DIGEST_SIZE) != 64) return false;
    for (size_t i = 0; i < 64; i++)
        if (!((text[i] >= '0' && text[i] <= '9') || (text[i] >= 'a' && text[i] <= 'f'))) return false;
    return true;
}
static bool keys_valid(json_t *object, const char *const *keys)
{
    if (!json_is_object(object)) return false;
    size_t count = 0;
    while (keys[count]) { if (!json_object_get(object, keys[count])) return false; count++; }
    return json_object_size(object) == count;
}
static const char *string_value(json_t *object, const char *key)
{
    json_t *value = json_object_get(object, key); const char *text = json_string_value(value);
    return text && json_string_length(value) == strlen(text) ? text : NULL;
}
static int checksum(json_t *object, char digest[LCS_CONFIG_DIGEST_SIZE])
{
    char *text = json_dumps(object, JSON_SORT_KEYS | JSON_COMPACT | JSON_ENSURE_ASCII);
    unsigned char hash[SHA256_DIGEST_LENGTH];
    if (!text) return -1;
    unsigned char *result = SHA256((const unsigned char *)text, strlen(text), hash); free(text);
    if (!result) return -1;
    const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(hash); i++) { digest[2*i] = hex[hash[i] >> 4]; digest[2*i+1] = hex[hash[i] & 15]; }
    digest[64] = 0; return 0;
}
static bool record_valid(const lcs_config_request_record_t *record)
{
    return memchr(record->id, 0, sizeof(record->id)) && lcs_valid_name(record->id) &&
        record->base_revision < LCS_RESOURCE_REVISION_MAX && digest_valid(record->base_digest) && digest_valid(record->value_digest);
}
static bool same_identity(const lcs_config_request_record_t *a, const lcs_config_request_record_t *b)
{
    return !strcmp(a->id, b->id) && a->base_revision == b->base_revision &&
        !strcmp(a->base_digest, b->base_digest) && !strcmp(a->value_digest, b->value_digest);
}
const lcs_config_request_record_t *lcs_config_requests_find(const lcs_config_requests_t *history, const char *id)
{
    if (!history || history->poisoned || history->count > LCS_CONFIG_REQUEST_HISTORY_MAX || !id) return NULL;
    for (size_t i = 0; i < history->count; i++) if (!strcmp(history->records[i].id, id)) return &history->records[i];
    return NULL;
}
int lcs_config_requests_query(const lcs_config_requests_t *history, const char *id, lcs_config_request_view_t *view)
{
    if (!id || strnlen(id, LCS_CONFIG_REQUEST_MAX + 1) > LCS_CONFIG_REQUEST_MAX || !lcs_valid_name(id) || !view) return -1;
    lcs_config_request_view_t next = {0}; strcpy(next.id, id);
    if (history && (history->poisoned || history->count > LCS_CONFIG_REQUEST_HISTORY_MAX ||
        (!!history->store != !!history->base) || (!history->store && history->count)))
        next.history = LCS_CONFIG_HISTORY_RECOVERY;
    else if (history && history->store && history->base)
    {
        next.history = LCS_CONFIG_HISTORY_AVAILABLE;
        const lcs_config_request_record_t *record = lcs_config_requests_find(history, id);
        if (record)
        {
            if (!record_valid(record)) next.history = LCS_CONFIG_HISTORY_RECOVERY;
            else
            {
                next.state = record->committed ? LCS_CONFIG_REQUEST_COMMITTED : LCS_CONFIG_REQUEST_RESERVED;
                next.base_revision = record->base_revision;
                strcpy(next.base_digest, record->base_digest); strcpy(next.value_digest, record->value_digest);
            }
        }
    }
    *view = next; return 0;
}
int lcs_config_requests_export(const lcs_config_requests_t *history, char **document, size_t *length, char *err, size_t err_len)
{
    if (!history->store || !history->base || history->poisoned || history->count > LCS_CONFIG_REQUEST_HISTORY_MAX)
        return fail(err, err_len, "unavailable/poisoned history requires recovery");
    json_t *root = json_pack("{s:i,s:s,s:s}", "record_version", 1, "member", history->member, "membership", history->membership);
    json_t *records = json_array(); int result = -1; char *text = NULL, digest[LCS_CONFIG_DIGEST_SIZE];
    if (!root || !records) goto done;
    for (size_t i = 0; i < history->count; i++)
    {
        const lcs_config_request_record_t *r = &history->records[i];
        if (!record_valid(r)) goto done;
        for (size_t j = 0; j < i; j++) if (!strcmp(r->id, history->records[j].id)) goto done;
        if (json_array_append_new(records, json_pack("{s:s,s:I,s:s,s:s,s:b}", "request_id", r->id,
            "base_revision", (json_int_t)r->base_revision, "base_digest", r->base_digest,
            "value_digest", r->value_digest, "committed", r->committed))) goto done;
    }
    if (json_object_set(root, "requests", records) || checksum(root, digest) ||
        json_object_set_new(root, "checksum", json_string(digest))) goto done;
    text = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    if (!text || strlen(text) > LCS_CONFIG_REQUEST_RECORD_MAX) goto done;
    *length = strlen(text); *document = text; text = NULL; result = 0;
done:
    free(text); json_decref(root); json_decref(records);
    return result ? fail(err, err_len, "cannot encode bounded request history") : 0;
}
static int load(lcs_config_requests_t *history, const char *document, size_t length, char *err, size_t err_len)
{
    const char *const keys[] = {"record_version", "member", "membership", "requests", "checksum", NULL};
    const char *const entry_keys[] = {"request_id", "base_revision", "base_digest", "value_digest", "committed", NULL};
    if (!length || length > LCS_CONFIG_REQUEST_RECORD_MAX) return fail(err, err_len, "invalid history size");
    json_error_t error; json_t *root = json_loadb(document, length, JSON_REJECT_DUPLICATES, &error);
    int result = -1; char expected[LCS_CONFIG_DIGEST_SIZE], actual[LCS_CONFIG_DIGEST_SIZE];
    if (!keys_valid(root, keys)) goto done;
    const char *hash = string_value(root, "checksum"), *member = string_value(root, "member"), *membership = string_value(root, "membership");
    json_t *version = json_object_get(root, "record_version"), *records = json_object_get(root, "requests");
    if (!hash || !digest_valid(hash) || !member || strcmp(member, history->member) || !membership ||
        strcmp(membership, history->membership) || !json_is_integer(version) || json_integer_value(version) != 1 ||
        !json_is_array(records) || json_array_size(records) > LCS_CONFIG_REQUEST_HISTORY_MAX) goto done;
    strcpy(expected, hash); json_object_del(root, "checksum");
    if (checksum(root, actual) || strcmp(expected, actual)) goto done;
    for (size_t i = 0; i < json_array_size(records); i++)
    {
        json_t *entry = json_array_get(records, i), *revision = json_object_get(entry, "base_revision"), *chosen = json_object_get(entry, "committed");
        const char *id = string_value(entry, "request_id"), *base = string_value(entry, "base_digest"), *value = string_value(entry, "value_digest");
        if (!keys_valid(entry, entry_keys) || !id || strlen(id) > LCS_CONFIG_REQUEST_MAX || !lcs_valid_name(id) ||
            !base || !digest_valid(base) || !value || !digest_valid(value) || !json_is_integer(revision) ||
            json_integer_value(revision) < 0 || json_integer_value(revision) >= LCS_RESOURCE_REVISION_MAX || !json_is_boolean(chosen)) goto done;
        if (lcs_config_requests_find(history, id)) goto done;
        lcs_config_request_record_t *r = &history->records[history->count++];
        strcpy(r->id, id); strcpy(r->base_digest, base); strcpy(r->value_digest, value);
        r->base_revision = (uint64_t)json_integer_value(revision); r->committed = json_is_true(chosen);
    }
    result = 0;
done:
    json_decref(root);
    return result ? fail(err, err_len, "invalid, corrupt or foreign request history; recover instead of resetting") : 0;
}
static int save(const lcs_config_requests_t *history, char *err, size_t err_len)
{
    char *document; size_t length;
    if (lcs_config_requests_export(history, &document, &length, err, err_len)) return -1;
    int result = lcs_config_store_write(history->store, "requests.json", document, length, err, err_len);
    free(document); return result;
}
int lcs_config_requests_open(lcs_config_requests_t *history, lcs_config_store_t *store,
                               const lcs_config_t *base, bool allow_initialize, char *err, size_t err_len)
{
    if (!history || !store || store->directory_fd < 0 || store->lock_fd < 0 || !base) return fail(err, err_len, "locked store/static context required");
    lcs_config_requests_t *next = calloc(1, sizeof(*next));
    if (!next) return fail(err, err_len, "cannot allocate history");
    next->base = base; next->store = store;
    int self = lcs_config_self_index(base), result = -1;
    char *document = NULL; size_t length;
    if (self < 0 || lcs_config_agreement_membership(base, next->membership))
    { fail(err, err_len, "invalid static membership/local member"); goto done; }
    strcpy(next->member, base->nodes[self].name);
    int status = lcs_config_store_read(store, "requests.json", &document, &length, LCS_CONFIG_REQUEST_RECORD_MAX, err, err_len);
    if (!status) result = load(next, document, length, err, err_len);
    else if (status == 1 && allow_initialize) result = save(next, err, err_len);
    else if (status == 1) fail(err, err_len, "request history is missing; initialization must be explicitly authorized");
    if (!result) *history = *next;
done:
    free(document); free(next); return result;
}
void lcs_config_requests_close(lcs_config_requests_t *history) { memset(history, 0, sizeof(*history)); }

int lcs_config_requests_import(lcs_config_requests_t *history, const char *document, size_t length, char *err, size_t err_len)
{
    if (!history->store || history->poisoned) return fail(err, err_len, "poisoned history requires disk recovery");
    lcs_config_requests_t *next = calloc(1, sizeof(*next));
    if (!next) return fail(err, err_len, "cannot allocate imported history");
    next->store = history->store; next->base = history->base;
    strcpy(next->member, history->member); strcpy(next->membership, history->membership);
    int result = load(next, document, length, err, err_len);
    if (!result && next->count < history->count) result = -1;
    for (size_t i = 0; !result && i < history->count; i++)
        if (!same_identity(&next->records[i], &history->records[i]) ||
            (history->records[i].committed && !next->records[i].committed)) result = -1;
    if (!result) *history = *next;
    else { history->poisoned = true; fail(err, err_len, "worker replay would replace durable request history; recovery required"); }
    free(next); return result;
}
static bool bound(const lcs_config_requests_t *history, const lcs_config_agreement_t *agreement)
{
    return history->store && !history->poisoned && history->count <= LCS_CONFIG_REQUEST_HISTORY_MAX &&
        agreement && !agreement->poisoned &&
        history->store == agreement->store && history->base == agreement->base &&
        !strcmp(history->member, agreement->member) && !strcmp(history->membership, agreement->membership);
}
static int validate_agreement(const lcs_config_requests_t *history,
                                 const lcs_config_agreement_t *agreement, char *err, size_t err_len)
{
    if (!bound(history, agreement)) return fail(err, err_len, "history/agreement binding requires recovery");
    for (size_t i = 0; i < history->count; i++)
    {
        const lcs_config_request_record_t *r = &history->records[i];
        const lcs_config_value_t *committed = &agreement->committed;
        bool current_id = !strcmp(r->id, committed->request_id);
        if (r->base_revision > committed->revision ||
            (r->base_revision == committed->revision &&
             (r->committed || strcmp(r->base_digest, committed->digest))) ||
            ((current_id || (r->committed && r->base_revision + 1 == committed->revision)) &&
             (r->base_revision + 1 != committed->revision ||
              strcmp(r->id, committed->request_id) || strcmp(r->base_digest, agreement->parent_digest) ||
              strcmp(r->value_digest, committed->digest))))
        {
            if (err && err_len)
                snprintf(err, err_len, "configuration recovery: request history contradicts the learned journal; recovery required");
            return -1;
        }
        if (agreement->accepted.document && !strcmp(r->id, agreement->accepted.request_id) &&
            (r->base_revision != committed->revision || strcmp(r->base_digest, committed->digest) ||
             strcmp(r->value_digest, agreement->accepted.digest)))
        {
            if (err && err_len)
                snprintf(err, err_len, "configuration recovery: accepted request contradicts its immutable identity; recovery required");
            return -1;
        }
    }
    return 0;
}
int lcs_config_requests_recover(lcs_config_requests_t *history, const lcs_config_agreement_t *agreement,
                                 bool allow_initialize, char *err, size_t err_len)
{
    if (!history || history->store || history->base || history->count || history->poisoned ||
        !agreement || agreement->poisoned || !agreement->store || !agreement->base || !agreement->committed.document)
        return fail(err, err_len, "fresh history and recovered agreement required");
    /* Initialization is safe only before any agreement activity, never as a
     * substitute for lost request history on a running/previously active node. */
    bool pristine = !agreement->committed.revision && !strcmp(agreement->committed.request_id, "bootstrap") &&
        !agreement->promised && !agreement->accepted_ballot && !agreement->accepted.document &&
        !agreement->committed_ballot && !agreement->committed_voters && !*agreement->parent_digest;
    lcs_config_requests_t *next = calloc(1, sizeof(*next));
    if (!next) return fail(err, err_len, "cannot allocate recovered request history");
    int result = lcs_config_requests_open(next, agreement->store, agreement->base,
                                            allow_initialize && pristine, err, err_len);
    if (!result) result = validate_agreement(next, agreement, err, err_len);
    if (!result) *history = *next;
    free(next); return result;
}
static int record(lcs_config_requests_t *history, const lcs_config_request_record_t *request, char *err, size_t err_len)
{
    const lcs_config_request_record_t *existing = lcs_config_requests_find(history, request->id);
    if (existing && !same_identity(existing, request)) return LCS_CONFIG_REQUEST_CONFLICT;
    if (existing && (existing->committed || !request->committed)) return LCS_CONFIG_REQUEST_EXISTS;
    if (!existing && history->count == LCS_CONFIG_REQUEST_HISTORY_MAX) return LCS_CONFIG_REQUEST_CAPACITY;
    lcs_config_requests_t *next = malloc(sizeof(*next));
    if (!next) return fail(err, err_len, "cannot allocate request transition");
    *next = *history;
    if (existing) next->records[(size_t)(existing - history->records)].committed = true;
    else next->records[next->count++] = *request;
    int result = save(next, err, err_len);
    if (!result) *history = *next;
    else history->poisoned = true;
    free(next); return result;
}
int lcs_config_requests_reserve(lcs_config_requests_t *history, const lcs_config_agreement_t *agreement,
                                  uint64_t expected_revision, const char *expected_digest,
                                  const lcs_config_value_t *requested, char *err, size_t err_len)
{
    if (!bound(history, agreement) || !expected_digest || !digest_valid(expected_digest) ||
        expected_revision >= LCS_RESOURCE_REVISION_MAX || !requested || !requested->document ||
        requested->revision != expected_revision + 1)
        return fail(err, err_len, "valid immutable request and matching healthy agreement required");
    lcs_config_value_t checked = {0};
    if (lcs_config_value_init(&checked, agreement->base, requested->request_id, requested->document, requested->length, err, err_len)) return -1;
    bool valid = lcs_config_value_equal(&checked, requested); lcs_config_value_clear(&checked);
    if (!valid) return fail(err, err_len, "request fingerprint differs from canonical value");
    const lcs_config_request_record_t *existing = lcs_config_requests_find(history, requested->request_id);
    if (existing)
        return existing->base_revision == expected_revision && !strcmp(existing->base_digest, expected_digest) &&
            !strcmp(existing->value_digest, requested->digest) ?
            LCS_CONFIG_REQUEST_EXISTS : LCS_CONFIG_REQUEST_CONFLICT;
    if (expected_revision != agreement->committed.revision || strcmp(expected_digest, agreement->committed.digest))
        return fail(err, err_len, "request requires the current exact base revision/digest");
    lcs_config_request_record_t request = {.base_revision = agreement->committed.revision};
    strcpy(request.id, requested->request_id); strcpy(request.base_digest, agreement->committed.digest); strcpy(request.value_digest, requested->digest);
    return record(history, &request, err, err_len);
}
int lcs_config_requests_observe(lcs_config_requests_t *history, const lcs_config_agreement_t *agreement, char *err, size_t err_len)
{
    if (!bound(history, agreement) || !agreement->committed.document || !agreement->committed.revision ||
        !digest_valid(agreement->parent_digest) || !digest_valid(agreement->committed.digest) ||
        !lcs_config_agreement_ballot_valid(agreement, agreement->committed_ballot) ||
        !lcs_config_agreement_has_quorum(agreement, agreement->committed_voters))
        return fail(err, err_len, "an exact learned majority decision is required");
    lcs_config_value_t checked = {0};
    if (lcs_config_value_init(&checked, agreement->base, agreement->committed.request_id,
        agreement->committed.document, agreement->committed.length, err, err_len)) return -1;
    bool valid = lcs_config_value_equal(&checked, &agreement->committed); lcs_config_value_clear(&checked);
    if (!valid) return fail(err, err_len, "committed fingerprint differs from canonical value");
    lcs_config_request_record_t request = {.base_revision = agreement->committed.revision - 1, .committed = true};
    strcpy(request.id, agreement->committed.request_id); strcpy(request.base_digest, agreement->parent_digest);
    strcpy(request.value_digest, agreement->committed.digest);
    if (!record_valid(&request)) return fail(err, err_len, "invalid committed request identity");
    return record(history, &request, err, err_len);
}
