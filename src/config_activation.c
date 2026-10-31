// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_activation.h"
#include "config_barrier.h"

#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RECORD "activation.json"

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "configuration initialization: %s", message);
    return -1;
}

static bool keys_match(json_t *object, const char *const *keys)
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

static bool string_matches(json_t *value, const char *expected)
{
    const char *text = json_string_value(value);
    return text && json_string_length(value) == strlen(expected) && !strcmp(text, expected);
}

static int checksum(json_t *root, char hash[65])
{
    char *text = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT | JSON_ENSURE_ASCII);
    if (!text)
        return -1;
    unsigned char digest[SHA256_DIGEST_LENGTH];
    unsigned char *ok = SHA256((const unsigned char *)text, strlen(text), digest);
    free(text);
    if (!ok)
        return -1;
    for (size_t i = 0; i < sizeof(digest); i++)
        snprintf(hash + 2 * i, 3, "%02x", digest[i]);
    return 0;
}

static int persist(lcs_config_activation_t *activation, lcs_config_activation_phase_t phase,
                     char *err, size_t err_len)
{
    json_t *root = json_pack("{s:i,s:s,s:s,s:s,s:s,s:s}", "record_version", 1,
        "phase", phase == LCS_CONFIG_ACTIVATION_PREPARED ? "prepared" : "initializing",
        "member", activation->member, "membership", activation->membership,
        "baseline_digest", activation->baseline.digest, "baseline", activation->baseline.document);
    char hash[65], *text = NULL;
    if (root && !checksum(root, hash) && !json_object_set_new(root, "checksum", json_string(hash)))
        text = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    json_decref(root);
    int result = -1;
    if (text && strlen(text) <= LCS_CONFIG_ACTIVATION_MAX)
        result = lcs_config_store_write(activation->store, RECORD, text, strlen(text), err, err_len);
    else
        fail(err, err_len, "cannot encode bounded initialization record");
    free(text);
    if (result)
        activation->poisoned = true;
    else
        activation->phase = phase;
    return result;
}

void lcs_config_activation_close(lcs_config_activation_t *activation)
{
    lcs_config_value_clear(&activation->baseline);
    memset(activation, 0, sizeof(*activation));
}

static int exists(lcs_config_store_t *store, const char *name, size_t maximum, char *err, size_t err_len)
{
    char *text = NULL;
    size_t length;
    int result = lcs_config_store_read(store, name, &text, &length, maximum, err, err_len);
    free(text);
    return result < 0 ? -1 : result == 0;
}

static int decode(lcs_config_activation_t *activation, const char *text, size_t length,
                    const lcs_config_t *cfg, char *err, size_t err_len)
{
    static const char *const keys[] = {"record_version", "phase", "member", "membership",
                                       "baseline_digest", "baseline", "checksum", NULL};
    json_error_t error;
    json_t *root = json_loadb(text, length, JSON_REJECT_DUPLICATES, &error);
    json_t *format = json_object_get(root, "record_version");
    json_t *stored = json_object_get(root, "checksum");
    const char *hash = json_string_value(stored);
    char expected[65], actual[65];
    bool valid = keys_match(root, keys) && json_is_integer(format) && json_integer_value(format) == 1 &&
        string_matches(json_object_get(root, "member"), activation->member) &&
        string_matches(json_object_get(root, "membership"), activation->membership) &&
        hash && json_string_length(stored) == 64 && strlen(hash) == 64;
    if (valid)
    {
        memcpy(expected, hash, sizeof(expected));
        json_object_del(root, "checksum");
        valid = !checksum(root, actual) && !strcmp(expected, actual);
    }
    if (valid && string_matches(json_object_get(root, "phase"), "initializing"))
        activation->phase = LCS_CONFIG_ACTIVATION_INITIALIZING;
    else if (valid && string_matches(json_object_get(root, "phase"), "prepared"))
        activation->phase = LCS_CONFIG_ACTIVATION_PREPARED;
    else
        valid = false;
    json_t *baseline = json_object_get(root, "baseline");
    const char *document = json_string_value(baseline);
    if (valid)
        valid = document && strlen(document) == json_string_length(baseline) &&
            !lcs_config_value_init(&activation->baseline, cfg, "bootstrap", document,
                                    json_string_length(baseline), err, err_len);
    if (valid)
        valid = activation->baseline.revision == 0 &&
            activation->baseline.length == json_string_length(baseline) &&
            !memcmp(activation->baseline.document, document, activation->baseline.length) &&
            string_matches(json_object_get(root, "baseline_digest"), activation->baseline.digest);
    json_decref(root);
    return valid ? 0 : fail(err, err_len, "invalid, corrupt, or foreign frozen baseline; recovery required");
}

int lcs_config_activation_open(lcs_config_activation_t *activation,
                                lcs_config_store_t *store, const lcs_config_t *cfg,
                                lcs_config_t *frozen_base, bool allow_initialize,
                                char *err, size_t err_len)
{
    lcs_config_activation_t next = { .store = store };
    int self = lcs_config_self_index(cfg);
    if (self < 0 || lcs_config_agreement_membership(cfg, next.membership))
        return fail(err, err_len, "invalid static membership");
    snprintf(next.member, sizeof(next.member), "%s", cfg->nodes[self].name);
    char *text = NULL;
    size_t length;
    int result = lcs_config_store_read(store, RECORD, &text, &length, LCS_CONFIG_ACTIVATION_MAX, err, err_len);
    if (!result)
        result = decode(&next, text, length, cfg, err, err_len);
    else if (result == 1)
    {
        if (!allow_initialize)
            result = fail(err, err_len, "frozen initialization record is missing; explicit coordination/recovery required");
        else
        {
            int journal = exists(store, "agreement.json", LCS_CONFIG_JOURNAL_MAX, err, err_len);
            int catalog = journal < 0 ? -1 : exists(store, "application.json", LCS_CONFIG_APPLICATION_MAX, err, err_len);
            int barrier = catalog < 0 ? -1 : exists(store, "barrier.json", LCS_CONFIG_BARRIER_MAX, err, err_len);
            if (journal < 0 || catalog < 0 || barrier < 0)
                result = -1;
            else if (journal || catalog || barrier)
                result = fail(err, err_len, "cannot initialize over existing agreement/application history");
            else
            {
                char *document = NULL;
                size_t document_length;
                result = lcs_resource_config_encode(cfg, 0, &document, &document_length, err, err_len);
                if (!result)
                    result = lcs_config_value_init(&next.baseline, cfg, "bootstrap", document, document_length, err, err_len);
                free(document);
                if (!result)
                    result = persist(&next, LCS_CONFIG_ACTIVATION_INITIALIZING, err, err_len);
            }
        }
    }
    free(text);
    lcs_config_t *decoded = NULL;
    if (!result)
    {
        decoded = malloc(sizeof(*decoded));
        uint64_t revision;
        if (!decoded)
            result = fail(err, err_len, "out of memory decoding frozen baseline");
        else
            result = lcs_resource_config_decode(next.baseline.document, next.baseline.length,
                                                 cfg, decoded, &revision, err, err_len);
        if (!result && decoded->resource_schema_version != LCS_RESOURCE_SCHEMA_VERSION)
            result = fail(err, err_len, "initialization must freeze the legacy schema");
    }
    if (result)
        lcs_config_activation_close(&next);
    else
    {
        *frozen_base = *decoded;
        next.base = frozen_base;
        *activation = next;
    }
    free(decoded);
    return result;
}

static int validate_dynamic_value(const lcs_config_activation_t *activation,
                                    const lcs_config_value_t *value,
                                    char *err, size_t err_len)
{
    if (!value->document || value->revision == 0)
        return 0;
    lcs_config_t *config = malloc(sizeof(*config));
    if (!config)
        return fail(err, err_len, "out of memory validating activated history");
    uint64_t revision;
    int result = lcs_resource_config_decode(value->document, value->length, activation->base,
                                            config, &revision, err, err_len);
    if (!result && config->resource_schema_version != LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION)
        result = fail(err, err_len, "initialized history requires dynamic schema after bootstrap");
    if (!result && revision == 1)
        result = lcs_resource_config_validate_successor(activation->base, 0, config, 1, err, err_len);
    free(config);
    return result;
}

int lcs_config_activation_resume(lcs_config_activation_t *activation,
                                  lcs_config_agreement_t *agreement,
                                  lcs_config_application_t *application,
                                  char *err, size_t err_len)
{
    if (activation->poisoned || !activation->base || !activation->baseline.document ||
        (activation->phase != LCS_CONFIG_ACTIVATION_INITIALIZING && activation->phase != LCS_CONFIG_ACTIVATION_PREPARED))
        return fail(err, err_len, "initialization requires disk recovery");
    bool initializing = activation->phase == LCS_CONFIG_ACTIVATION_INITIALIZING;
    lcs_config_agreement_t next_agreement = {0};
    lcs_config_application_t next_application = {0};
    int result = -1;
    if (initializing)
    {
        int barrier = exists(activation->store, "barrier.json", LCS_CONFIG_BARRIER_MAX, err, err_len);
        if (barrier < 0)
            goto out;
        if (barrier)
        {
            fail(err, err_len, "readiness barrier exists before initialization is prepared; recovery required");
            goto out;
        }
    }
    int journal = exists(activation->store, "agreement.json", LCS_CONFIG_JOURNAL_MAX, err, err_len);
    int catalog = journal < 0 ? -1 : exists(activation->store, "application.json", LCS_CONFIG_APPLICATION_MAX, err, err_len);
    if (journal < 0 || catalog < 0)
        goto out;
    if (!journal && catalog)
    {
        fail(err, err_len, "application record exists without agreement; recover missing promises instead of recreating them");
        goto out;
    }
    if (lcs_config_agreement_open(&next_agreement, activation->store, activation->base,
                                   initializing, err, err_len))
        goto out;
    if ((!next_agreement.committed.revision && !lcs_config_value_equal(&next_agreement.committed, &activation->baseline)) ||
        (initializing && (next_agreement.committed.revision || next_agreement.promised ||
                          next_agreement.accepted.document)))
    {
        fail(err, err_len, "agreement does not match a nonvoting frozen initialization");
        goto out;
    }
    if (validate_dynamic_value(activation, &next_agreement.committed, err, err_len) ||
        validate_dynamic_value(activation, &next_agreement.accepted, err, err_len))
        goto out;
    if (lcs_config_application_open(&next_application, &next_agreement, initializing, err, err_len))
        goto out;
    if (initializing && (next_application.pending.document ||
        !lcs_config_value_equal(&next_application.applied, &activation->baseline)))
    {
        fail(err, err_len, "application catalog does not match frozen initialization");
        goto out;
    }
    if (initializing && persist(activation, LCS_CONFIG_ACTIVATION_PREPARED, err, err_len))
        goto out;
    *agreement = next_agreement;
    *application = next_application;
    return 0;
out:
    lcs_config_application_close(&next_application);
    lcs_config_agreement_close(&next_agreement);
    activation->poisoned = true;
    return result;
}
