// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_application.h"
#include "config_candidate.h"

#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CATALOG "application.json"
#define CATALOG_MAX (LCS_RESOURCE_DOCUMENT_MAX * 4u + 16384u)

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

static int persist(lcs_config_application_t *application,
                     const lcs_config_value_t *applied, const lcs_config_value_t *pending,
                     char *err, size_t err_len)
{
    json_t *root = json_pack("{s:i,s:s,s:s,s:o,s:o}", "record_version", 1,
        "member", application->member, "membership", application->membership,
        "applied", encode_value(applied), "pending", encode_value(pending));
    char hash[65], *text = NULL;
    int result = -1;
    if (root && !checksum(root, hash) &&
        !json_object_set_new(root, "checksum", json_string(hash)))
        text = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    if (text && strlen(text) <= CATALOG_MAX)
        result = lcs_config_store_write(application->store, CATALOG, text, strlen(text), err, err_len);
    else
        fail(err, err_len, "cannot encode bounded application catalog");
    free(text);
    json_decref(root);
    if (result)
        application->poisoned = true;
    return result;
}

void lcs_config_application_close(lcs_config_application_t *application)
{
    lcs_config_value_clear(&application->applied);
    lcs_config_value_clear(&application->pending);
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

static int validate_pair(lcs_config_application_t *application, char *err, size_t err_len)
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
    free(configs);
    return result;
}

int lcs_config_application_open(lcs_config_application_t *application,
                                 const lcs_config_agreement_t *agreement,
                                 bool allow_initialize, char *err, size_t err_len)
{
    if (agreement->poisoned || !agreement->committed.document)
        return fail(err, err_len, "agreement recovery required before opening application catalog");
    lcs_config_application_t next = { .store = agreement->store, .base = agreement->base };
    snprintf(next.member, sizeof(next.member), "%s", agreement->member);
    snprintf(next.membership, sizeof(next.membership), "%s", agreement->membership);
    char *text = NULL;
    size_t length;
    int result = lcs_config_store_read(next.store, CATALOG, &text, &length, CATALOG_MAX, err, err_len);
    if (result == 1)
    {
        if (!allow_initialize || agreement->committed.revision ||
            strcmp(agreement->committed.request_id, "bootstrap"))
            result = fail(err, err_len, "application catalog missing; explicit bootstrap/recovery required");
        else if (lcs_config_value_copy(&next.applied, &agreement->committed))
            result = fail(err, err_len, "out of memory initializing application catalog");
        else
            result = persist(&next, &next.applied, &next.pending, err, err_len);
    }
    else if (!result)
    {
        static const char *const keys[] = {"record_version", "member", "membership", "applied", "pending", "checksum", NULL};
        json_error_t error;
        json_t *root = json_loadb(text, length, JSON_REJECT_DUPLICATES, &error);
        const char *stored = json_string_value(json_object_get(root, "checksum"));
        char expected[65] = "", actual[65];
        const char *member = json_string_value(json_object_get(root, "member"));
        const char *membership = json_string_value(json_object_get(root, "membership"));
        json_t *format = json_object_get(root, "record_version");
        bool valid = exact_keys(root, keys) && json_is_integer(format) && json_integer_value(format) == 1 &&
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
        if (!result)
            result = validate_pair(&next, err, err_len);
        json_decref(root);
        if (result)
            fail(err, err_len, "invalid, corrupt, or foreign application catalog; recovery required");
    }
    free(text);
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
        result = persist(application, &application->applied, &pending, err, err_len);
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
    int result = persist(application, &application->pending, &empty, err, err_len);
    if (!result)
    {
        lcs_config_value_clear(&application->applied);
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
