// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_application.h"
#include "config_candidate.h"

#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG "application.json"

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration application: %s", message);
    return -1;
}

static bool exact_keys(json_t *object, const char *const *keys)
{
    size_t count = 0;
    if (!json_is_object(object))
        return false;
    while (keys[count])
    {
        if (!json_object_get(object, keys[count]))
            return false;
        count++;
    }
    return json_object_size(object) == count;
}

static int checksum(json_t *object, char result[65])
{
    char *text = json_dumps(object, JSON_SORT_KEYS | JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!text)
        return -1;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char *ok = SHA256((const unsigned char *)text, strlen(text), digest);
    free(text);
    if (!ok)
        return -1;
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(result + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

static json_t *encode_value(const lcs_config_value_t *value)
{
    return value->document ? json_pack("{s:s,s:s,s:s}", "request_id", value->request_id,
                                      "digest", value->digest, "document", value->document) : json_null();
}

static int decode_value(json_t *object, const lcs_config_t *base,
                          lcs_config_value_t *value, char *err, size_t err_len)
{
    static const char *const keys[] = {"request_id", "digest", "document", NULL};
    if (!exact_keys(object, keys))
        return fail(err, err_len, "invalid catalog value");
    json_t *request = json_object_get(object, "request_id");
    json_t *digest = json_object_get(object, "digest");
    json_t *document = json_object_get(object, "document");
    const char *r = json_string_value(request), *d = json_string_value(digest);
    const char *text = json_string_value(document);
    if (!r || !d || !text || strlen(r) != json_string_length(request) ||
        strlen(d) != 64 || json_string_length(digest) != 64 ||
        strlen(text) != json_string_length(document) ||
        lcs_config_value_init(value, base, r, text, json_string_length(document), err, err_len))
        return fail(err, err_len, "invalid catalog snapshot");
    if (strcmp(d, value->digest) || value->length != json_string_length(document) ||
        memcmp(value->document, text, value->length))
        return fail(err, err_len, "catalog snapshot identity/canonical encoding mismatch");
    return 0;
}

static int validate_pair(const lcs_config_application_t *application, char *err, size_t err_len);

int lcs_config_application_export(const lcs_config_application_t *application,
                                    char **document, size_t *length,
                                    char *err, size_t err_len)
{
    if (application->poisoned || !application->applied.document)
        return fail(err, err_len, "application recovery required before export");
    /* Preserve readable version-1 catalogs with unknown predecessor history
     * until an actual FINISH establishes it; never invent missing ancestry. */
    int version = application->applied.revision && !application->history[0].document ? 1 : 2;
    if ((!application->history[0].document && application->history[1].document) ||
        validate_pair(application, err, err_len))
        return -1;
    json_t *root = json_pack("{s:i,s:s,s:s,s:o,s:o}", "record_version", version,
        "member", application->member, "membership", application->membership,
        "applied", encode_value(&application->applied), "pending", encode_value(&application->pending));
    if (root && version == 2 && json_object_set_new(root, "history", json_pack("[o,o]",
        encode_value(&application->history[0]), encode_value(&application->history[1]))))
    { json_decref(root); root = NULL; }
    char hash[65], *text = NULL;
    if (root && !checksum(root, hash) &&
        !json_object_set_new(root, "checksum", json_string(hash)))
        text = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    json_decref(root);
    if (!text || strlen(text) > LCS_CONFIG_APPLICATION_MAX)
    {
        free(text);
        return fail(err, err_len, "cannot encode bounded application catalog");
    }
    *document = text;
    *length = strlen(text);
    return 0;
}

static int persist(lcs_config_application_t *application,
                     const lcs_config_value_t *applied, const lcs_config_value_t *pending,
                     const lcs_config_value_t history[LCS_CONFIG_APPLICATION_HISTORY], char *err, size_t err_len)
{
    lcs_config_application_t next = *application;
    next.applied = *applied;
    next.pending = *pending;
    memcpy(next.history, history, sizeof(next.history));
    char *text = NULL;
    size_t length;
    int result = lcs_config_application_export(&next, &text, &length, err, err_len);
    if (!result)
        result = lcs_config_store_write(application->store, CATALOG, text, length, err, err_len);
    free(text);
    if (result)
        application->poisoned = true;
    return result;
}

void lcs_config_application_close(lcs_config_application_t *application)
{
    lcs_config_value_clear(&application->applied);
    lcs_config_value_clear(&application->pending);
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++) lcs_config_value_clear(&application->history[i]);
    memset(application, 0, sizeof(*application));
}

static bool bound_to(const lcs_config_application_t *application,
                       const lcs_config_agreement_t *agreement)
{
    return application->store == agreement->store &&
        !strcmp(application->member, agreement->member) &&
        !strcmp(application->membership, agreement->membership);
}

/* Identity-aware comparison also applies during multi-revision catch-up. Never
 * reuse runtime state for a name whose old incarnation was deleted in between. */
static int plan_values(const lcs_config_t *base, const lcs_config_value_t *applied,
                         const lcs_config_value_t *pending,
                         lcs_config_t *old_config, lcs_config_t *target,
                         lcs_config_application_plan_t *plan,
                         char *err, size_t err_len)
{
    uint64_t before, after;
    if (lcs_resource_config_decode(applied->document, applied->length, base,
                                    old_config, &before, err, err_len) ||
        lcs_resource_config_decode(pending->document, pending->length, base,
                                    target, &after, err, err_len))
        return -1;
    if (after <= before || target->resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION)
        return fail(err, err_len, "pending snapshot must advance dynamic configuration");
    memset(plan, 0, sizeof(*plan));
    for (size_t i = 0; i < LCS_MAX_RESOURCES; i++)
        plan->new_to_old[i] = -1;
    for (size_t i = 0; i < old_config->resource_count; i++)
    {
        const lcs_resource_config_t *old = &old_config->resources[i];
        int next = lcs_config_resource_index(target, old->name);
        if (next < 0 || target->resources[next].birth_revision != old->birth_revision)
            plan->cleanup |= UINT32_C(1) << i;
    }
    for (size_t i = 0; i < target->resource_count; i++)
    {
        const lcs_resource_config_t *next = &target->resources[i];
        int old_idx = lcs_config_resource_index(old_config, next->name);
        if (old_idx < 0 || old_config->resources[old_idx].birth_revision != next->birth_revision)
        {
            if (next->birth_revision <= before)
                return fail(err, err_len, "catch-up would reuse an absent/replaced incarnation");
            continue;
        }
        const lcs_resource_config_t *old = &old_config->resources[old_idx];
        if (!lcs_config_backend_equal(old, next))
            return fail(err, err_len, "same incarnation has conflicting backend definitions");
        plan->new_to_old[i] = old_idx;
        bool dependencies_changed = old->depends_on_count != next->depends_on_count;
        for (size_t j = 0; !dependencies_changed && j < old->depends_on_count; j++)
        {
            bool found = false;
            for (size_t k = 0; k < next->depends_on_count; k++)
                if (!strcmp(old->depends_on_names[j], next->depends_on_names[k]))
                    found = true;
            dependencies_changed = !found;
        }
        if (dependencies_changed)
            plan->drain |= UINT32_C(1) << old_idx;
    }
    plan->drain |= plan->cleanup;
    uint32_t previous;
    do
    {
        previous = plan->drain;
        for (size_t i = 0; i < old_config->resource_count; i++)
            for (size_t j = 0; j < old_config->resources[i].depends_on_count; j++)
                if (plan->drain & (UINT32_C(1) << old_config->resources[i].depends_on_idx[j]))
                    plan->drain |= UINT32_C(1) << i;
    } while (previous != plan->drain);
    return 0;
}

static int validate_pair(const lcs_config_application_t *application, char *err, size_t err_len)
{
    lcs_config_t *configs = malloc(2 * sizeof(*configs));
    if (!configs)
        return fail(err, err_len, "out of memory validating application plan");
    uint64_t revision;
    int result = lcs_resource_config_decode(application->applied.document,
        application->applied.length, application->base, &configs[0], &revision, err, err_len);
    if (!result && (revision ? configs[0].resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION :
        configs[0].resource_schema_version != LCS_RESOURCE_SCHEMA_VERSION ||
        strcmp(application->applied.request_id, "bootstrap")))
        result = fail(err, err_len, "applied catalog must describe bootstrap or dynamic authority");
    if (!result && application->pending.document)
    {
        lcs_config_application_plan_t plan;
        result = plan_values(application->base, &application->applied, &application->pending,
                               &configs[0], &configs[1], &plan, err, err_len);
    }
    for (size_t i = 0; !result && i < LCS_CONFIG_APPLICATION_HISTORY; i++)
    {
        if (!application->history[i].document)
        {
            if (i == 0 && application->history[1].document)
                result = fail(err, err_len, "noncontiguous application history");
            break;
        }
        lcs_config_application_plan_t plan;
        const lcs_config_value_t *target = i ? &application->history[i - 1] : &application->applied;
        result = plan_values(application->base, &application->history[i], target, &configs[0], &configs[1], &plan, err, err_len);
        if (!result && (application->history[i].revision ? configs[0].resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION :
            configs[0].resource_schema_version != LCS_RESOURCE_SCHEMA_VERSION || strcmp(application->history[i].request_id, "bootstrap")))
            result = fail(err, err_len, "history must describe bootstrap or dynamic authority");
    }
    free(configs);
    return result;
}

static int decode_catalog(lcs_config_application_t *application,
                             const lcs_config_agreement_t *agreement,
                             const char *text, size_t length, char *err, size_t err_len)
{
    if (agreement->poisoned || !agreement->committed.document ||
        !text || !length || length > LCS_CONFIG_APPLICATION_MAX)
        return fail(err, err_len, "invalid catalog or agreement requires recovery");
    lcs_config_application_t next = { .store = agreement->store, .base = agreement->base };
    snprintf(next.member, sizeof(next.member), "%s", agreement->member);
    snprintf(next.membership, sizeof(next.membership), "%s", agreement->membership);
    int result;
    static const char *const keys_v1[] = {"record_version", "member", "membership", "applied", "pending", "checksum", NULL};
    static const char *const keys_v2[] = {"record_version", "member", "membership", "applied", "pending", "history", "checksum", NULL};
    json_error_t error;
    json_t *root = json_loadb(text, length, JSON_REJECT_DUPLICATES, &error);
    const char *stored = json_string_value(json_object_get(root, "checksum"));
    char expected[65] = "", actual[65];
    const char *member = json_string_value(json_object_get(root, "member"));
    const char *membership = json_string_value(json_object_get(root, "membership"));
    json_t *format = json_object_get(root, "record_version");
    json_int_t version = json_is_integer(format) ? json_integer_value(format) : 0;
    bool valid = (version == 1 || version == 2) && exact_keys(root, version == 1 ? keys_v1 : keys_v2) &&
        stored && strlen(stored) == 64 && json_string_length(json_object_get(root, "checksum")) == 64 &&
        member && strlen(member) == json_string_length(json_object_get(root, "member")) && !strcmp(member, next.member) &&
        membership && json_string_length(json_object_get(root, "membership")) == 64 && !strcmp(membership, next.membership);
    if (valid)
    {
        snprintf(expected, sizeof(expected), "%s", stored);
        json_object_del(root, "checksum");
        valid = !checksum(root, actual) && !strcmp(expected, actual);
    }
    result = valid ? decode_value(json_object_get(root, "applied"), next.base, &next.applied, err, err_len) : -1;
    json_t *pending = json_object_get(root, "pending");
    if (!result && !json_is_null(pending))
        result = decode_value(pending, next.base, &next.pending, err, err_len);
    if (!result && version == 2)
    {
        json_t *history = json_object_get(root, "history");
        if (!json_is_array(history) || json_array_size(history) != LCS_CONFIG_APPLICATION_HISTORY)
            result = fail(err, err_len, "invalid completed application history");
        for (size_t i = 0; !result && i < LCS_CONFIG_APPLICATION_HISTORY; i++)
            if (!json_is_null(json_array_get(history, i)))
                result = decode_value(json_array_get(history, i), next.base, &next.history[i], err, err_len);
        if (!result && next.applied.revision && !next.history[0].document)
            result = fail(err, err_len, "completed application history is missing");
    }
    if (!result)
        result = validate_pair(&next, err, err_len);
    json_decref(root);
    if (result)
        fail(err, err_len, "invalid, corrupt, or foreign application catalog; recovery required");
    if (!result && (next.applied.revision > agreement->committed.revision ||
        (next.applied.revision == agreement->committed.revision &&
         !lcs_config_value_equal(&next.applied, &agreement->committed)) ||
        (next.pending.document && (next.pending.revision > agreement->committed.revision ||
         (next.pending.revision == agreement->committed.revision &&
          !lcs_config_value_equal(&next.pending, &agreement->committed))))))
        result = fail(err, err_len, "application catalog is ahead of/conflicts with recovered agreement");
    if (result)
        lcs_config_application_close(&next);
    else
        *application = next;
    return result;
}

int lcs_config_application_open(lcs_config_application_t *application,
                                 const lcs_config_agreement_t *agreement,
                                 bool allow_initialize, char *err, size_t err_len)
{
    if (agreement->poisoned || !agreement->committed.document)
        return fail(err, err_len, "agreement recovery required before opening application catalog");
    char *text = NULL;
    size_t length;
    int result = lcs_config_store_read(agreement->store, CATALOG, &text, &length,
                                      LCS_CONFIG_APPLICATION_MAX, err, err_len);
    if (!result)
        result = decode_catalog(application, agreement, text, length, err, err_len);
    else if (result == 1)
    {
        lcs_config_application_t next = { .store = agreement->store, .base = agreement->base };
        snprintf(next.member, sizeof(next.member), "%s", agreement->member);
        snprintf(next.membership, sizeof(next.membership), "%s", agreement->membership);
        if (!allow_initialize || agreement->committed.revision ||
            strcmp(agreement->committed.request_id, "bootstrap"))
            result = fail(err, err_len, "application catalog missing; explicit bootstrap/recovery required");
        else if (lcs_config_value_copy(&next.applied, &agreement->committed))
            result = fail(err, err_len, "out of memory initializing application catalog");
        else
            result = persist(&next, &next.applied, &next.pending, next.history, err, err_len);
        if (result)
            lcs_config_application_close(&next);
        else
            *application = next;
    }
    free(text);
    return result;
}

int lcs_config_application_import(lcs_config_application_t *application,
                                    const lcs_config_agreement_t *agreement,
                                    const char *document, size_t length,
                                    char *err, size_t err_len)
{
    if (application->poisoned || !bound_to(application, agreement))
        return fail(err, err_len, "application requires recovery before worker import");
    lcs_config_application_t next = {0};
    if (decode_catalog(&next, agreement, document, length, err, err_len))
        return -1;
    bool same_history = true;
    for (size_t i = 0; i < LCS_CONFIG_APPLICATION_HISTORY; i++)
        same_history = same_history && ((!next.history[i].document && !application->history[i].document) ||
            lcs_config_value_equal(&next.history[i], &application->history[i]));
    bool same_applied = lcs_config_value_equal(&next.applied, &application->applied);
    bool same_pending = (!next.pending.document && !application->pending.document) ||
        (next.pending.document && application->pending.document &&
         lcs_config_value_equal(&next.pending, &application->pending));
    /* Only an unchanged result, staging the exact latest decision, or finishing
     * the exact pending application may cross a local worker boundary. An old
     * cleanup obligation cannot be dropped/replaced by a newer worker replay. */
    bool staging = same_history && same_applied && !application->pending.document && next.pending.document &&
        lcs_config_value_equal(&next.pending, &agreement->committed);
    bool finishing = application->pending.document && !next.pending.document &&
        lcs_config_value_equal(&next.applied, &application->pending) &&
        lcs_config_value_equal(&next.history[0], &application->applied) &&
        ((!next.history[1].document && !application->history[0].document) ||
            lcs_config_value_equal(&next.history[1], &application->history[0]));
    if (!(same_applied && same_pending && same_history) && !staging && !finishing)
    {
        lcs_config_application_close(&next);
        return fail(err, err_len, "stale or conflicting application worker result");
    }
    lcs_config_application_close(application);
    *application = next;
    return 0;
}

int lcs_config_application_begin(lcs_config_application_t *application,
                                  const lcs_config_agreement_t *agreement,
                                  char *err, size_t err_len)
{
    if (application->poisoned || agreement->poisoned || !bound_to(application, agreement))
        return fail(err, err_len, "application/foreign agreement requires recovery");
    if (application->pending.document)
        return lcs_config_value_equal(&application->pending, &agreement->committed) ? 0 : 1;
    if (agreement->committed.revision == application->applied.revision)
        return lcs_config_value_equal(&application->applied, &agreement->committed) ? 0 :
            fail(err, err_len, "applied revision conflicts with agreement");
    lcs_config_value_t pending = {0};
    if (lcs_config_value_copy(&pending, &agreement->committed))
        return fail(err, err_len, "out of memory preparing application catalog");
    lcs_config_application_t proposed = *application;
    proposed.pending = pending;
    int result = validate_pair(&proposed, err, err_len);
    if (!result)
        result = persist(application, &application->applied, &pending, application->history, err, err_len);
    if (!result)
        application->pending = pending;
    else
        lcs_config_value_clear(&pending);
    return result;
}

int lcs_config_application_finish(lcs_config_application_t *application,
                                   const char *digest, char *err, size_t err_len)
{
    if (application->poisoned)
        return fail(err, err_len, "application storage outcome uncertain; reopen/recover first");
    if (!digest || !application->pending.document || strcmp(digest, application->pending.digest))
        return fail(err, err_len, "finish requires exact pending configuration digest");
    lcs_config_value_t empty = {0};
    lcs_config_value_t history[LCS_CONFIG_APPLICATION_HISTORY] = {application->applied, application->history[0]};
    int result = persist(application, &application->pending, &empty, history, err, err_len);
    if (!result)
    {
        lcs_config_value_clear(&application->history[1]);
        application->history[1] = application->history[0];
        application->history[0] = application->applied;
        application->applied = application->pending;
        memset(&application->pending, 0, sizeof(application->pending));
    }
    return result;
}

int lcs_config_application_plan(const lcs_config_application_t *application,
                                 lcs_config_t *old_config, lcs_config_t *target,
                                 lcs_config_application_plan_t *plan,
                                 char *err, size_t err_len)
{
    if (application->poisoned || !application->pending.document)
        return fail(err, err_len, "no recoverable pending application");
    lcs_config_t *configs = malloc(2 * sizeof(*configs));
    if (!configs)
        return fail(err, err_len, "out of memory decoding application plan");
    lcs_config_application_plan_t next;
    int result = plan_values(application->base, &application->applied, &application->pending,
                              &configs[0], &configs[1], &next, err, err_len);
    if (!result)
    {
        *old_config = configs[0];
        *target = configs[1];
        *plan = next;
    }
    free(configs);
    return result;
}
