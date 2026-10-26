// SPDX-License-Identifier: GPL-3.0-or-later

#include "resource_config.h"
#include "config_candidate.h"
#include "util.h"

#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fail(char *err, size_t len, const char *message)
{
    if (err && len)
        snprintf(err, len, "%s", message);
    return -1;
}

static bool name_valid(const char *name)
{
    return name && strlen(name) <= LCS_NAME_MAX && lcs_valid_name(name);
}

static bool keys_valid(json_t *object, const char *const *allowed)
{
    if (!json_is_object(object))
        return false;
    const char *key;
    json_t *value;
    json_object_foreach(object, key, value)
    {
        (void)value;
        bool found = false;
        for (size_t i = 0; allowed[i]; i++)
            if (strcmp(key, allowed[i]) == 0)
                found = true;
        if (!found)
            return false;
    }
    return true;
}

static int string_field(json_t *object, const char *key, char *dst, size_t cap,
                        bool required)
{
    json_t *field = json_object_get(object, key);
    if (!field)
        return required ? -1 : 0;
    const char *value = json_string_value(field);
    if (!value || json_string_length(field) != strlen(value) ||
        strlen(value) >= cap || (required && !*value))
        return -1;
    memcpy(dst, value, strlen(value) + 1);
    return 0;
}

static int put_string(json_t *object, const char *key, const char *value)
{
    return json_object_set_new(object, key, json_string(value));
}

static int decode_group(const char *name, json_t *object, lcs_group_config_t *group)
{
    static const char *const keys[] = { "type", "mode", NULL };
    char type[32] = "", mode[32] = "";
    if (!name_valid(name) || !keys_valid(object, keys) ||
        string_field(object, "type", type, sizeof(type), true) ||
        string_field(object, "mode", mode, sizeof(mode), true))
        return -1;
    snprintf(group->name, sizeof(group->name), "%s", name);
    group->type = strcmp(type, "keep-together") == 0 ? LCS_GROUP_KEEP_TOGETHER :
                  strcmp(type, "anti-affinity") == 0 ? LCS_GROUP_ANTI_AFFINITY : 0;
    group->mode = strcmp(mode, "strict") == 0 ? LCS_GROUP_MODE_STRICT :
                  strcmp(mode, "best-effort") == 0 ? LCS_GROUP_MODE_BEST_EFFORT : 0;
    return group->type && group->mode ? 0 : -1;
}

static int decode_resource(const char *name, json_t *object,
                           lcs_resource_config_t *resource)
{
    static const char *const keys[] = {
        "type", "address", "interface", "systemd_unit", "group", "home_node",
        "priority", "depends_on", "pre_start", "post_start", "pre_stop", "post_stop", NULL
    };
    char type[32] = "";
    if (!name_valid(name) || !keys_valid(object, keys) ||
        string_field(object, "type", type, sizeof(type), true))
        return -1;
    snprintf(resource->name, sizeof(resource->name), "%s", name);
    resource->type = strcmp(type, "vip") == 0 ? LCS_RESOURCE_VIP :
                     strcmp(type, "service") == 0 ? LCS_RESOURCE_SERVICE : 0;
    if (!resource->type)
        return -1;
#define GET(key, member) \
    if (string_field(object, key, resource->member, sizeof(resource->member), false)) \
        return -1
    GET("address", address);
    GET("interface", interface);
    GET("systemd_unit", systemd_unit);
    GET("group", group_name);
    GET("home_node", home_node_name);
    GET("pre_start", pre_start);
    GET("post_start", post_start);
    GET("pre_stop", pre_stop);
    GET("post_stop", post_stop);
#undef GET
    if (resource->type == LCS_RESOURCE_VIP && json_object_get(object, "systemd_unit"))
        return -1;
    if (resource->type == LCS_RESOURCE_SERVICE &&
        (json_object_get(object, "address") || json_object_get(object, "interface")))
        return -1;
    json_t *priority = json_object_get(object, "priority");
    if (priority)
    {
        if (!json_is_integer(priority) || json_integer_value(priority) <= 0 ||
            (uint64_t)json_integer_value(priority) > UINT32_MAX)
            return -1;
        resource->priority_set = true;
        resource->priority = (uint32_t)json_integer_value(priority);
    }
    json_t *deps = json_object_get(object, "depends_on");
    if (deps)
    {
        if (!json_is_array(deps) || json_array_size(deps) > LCS_MAX_RESOURCE_DEPS)
            return -1;
        for (size_t i = 0; i < json_array_size(deps); i++)
        {
            json_t *dep = json_array_get(deps, i);
            const char *value = json_string_value(dep);
            if (!name_valid(value) || json_string_length(dep) != strlen(value))
                return -1;
            for (size_t j = 0; j < i; j++)
                if (strcmp(value, resource->depends_on_names[j]) == 0)
                    return -1;
            snprintf(resource->depends_on_names[i], sizeof(resource->depends_on_names[i]), "%s", value);
            resource->depends_on_count++;
        }
    }
    return 0;
}

int lcs_resource_config_decode(const char *document, size_t length,
                               const lcs_config_t *base, lcs_config_t *candidate,
                               uint64_t *revision, char *err, size_t err_len)
{
    static const char *const keys[] = {
        "schema_version", "cluster", "revision", "groups", "resources", NULL
    };
    if (!document || !length || length > LCS_RESOURCE_DOCUMENT_MAX)
        return fail(err, err_len, "resource JSON document is empty or exceeds 512 KiB");
    json_error_t json_error;
    json_t *root = json_loadb(document, length, JSON_REJECT_DUPLICATES, &json_error);
    if (!root)
    {
        if (err && err_len)
            snprintf(err, err_len, "resource JSON line %d: %s", json_error.line, json_error.text);
        return -1;
    }
    int rc = -1;
    lcs_config_t *next = NULL;
    json_t *schema = json_object_get(root, "schema_version");
    json_t *rev = json_object_get(root, "revision");
    json_t *groups = json_object_get(root, "groups");
    json_t *resources = json_object_get(root, "resources");
    char cluster[LCS_NAME_MAX + 1] = "";
    if (!keys_valid(root, keys) || !json_is_integer(schema) ||
        json_integer_value(schema) != LCS_RESOURCE_SCHEMA_VERSION ||
        !json_is_integer(rev) || json_integer_value(rev) < 0 ||
        string_field(root, "cluster", cluster, sizeof(cluster), true) ||
        strcmp(cluster, base->cluster_name) != 0 || !json_is_object(groups) ||
        !json_is_object(resources) || json_object_size(groups) > LCS_MAX_GROUPS ||
        json_object_size(resources) > LCS_MAX_RESOURCES)
    {
        fail(err, err_len, "invalid resource JSON envelope, schema version, cluster, or collection limits");
        goto out;
    }
    next = malloc(sizeof(*next));
    if (!next)
    {
        fail(err, err_len, "out of memory decoding resource configuration");
        goto out;
    }
    *next = *base;
    next->group_count = next->resource_count = 0;
    memset(next->groups, 0, sizeof(next->groups));
    memset(next->resources, 0, sizeof(next->resources));
    const char *name;
    json_t *value;
    json_object_foreach(groups, name, value)
    {
        if (decode_group(name, value, &next->groups[next->group_count++]))
        {
            fail(err, err_len, "invalid group definition in resource JSON");
            goto out;
        }
    }
    json_object_foreach(resources, name, value)
    {
        if (decode_resource(name, value, &next->resources[next->resource_count++]))
        {
            fail(err, err_len, "invalid resource definition in resource JSON");
            goto out;
        }
    }
    if (lcs_config_candidate_prepare(base, next, next, err, err_len))
        goto out;
    *candidate = *next;
    *revision = (uint64_t)json_integer_value(rev);
    rc = 0;
out:
    free(next);
    json_decref(root);
    return rc;
}

static json_t *encode_resource(const lcs_resource_config_t *resource,
                               const lcs_config_t *cfg)
{
    json_t *object = json_object();
    json_t *deps = json_array();
    if (!object || !deps)
        goto error;
    if (put_string(object, "type", lcs_resource_type_name(resource->type)))
        goto error;
#define PUT(key, member) \
    if (*resource->member && put_string(object, key, resource->member)) \
        goto error
    PUT("address", address);
    PUT("interface", interface);
    PUT("systemd_unit", systemd_unit);
    PUT("group", group_name);
    PUT("home_node", home_node_name);
    PUT("pre_start", pre_start);
    PUT("post_start", post_start);
    PUT("pre_stop", pre_stop);
    PUT("post_stop", post_stop);
#undef PUT
    if (resource->priority_set &&
        json_object_set_new(object, "priority", json_integer(resource->priority)))
        goto error;
    /* Dependencies are a set; serialize in canonical resource-name order. */
    for (size_t i = 0; i < cfg->resource_count; i++)
        for (size_t j = 0; j < resource->depends_on_count; j++)
            if (resource->depends_on_idx[j] == (int)i &&
                json_array_append_new(deps, json_string(cfg->resources[i].name)))
                goto error;
    if (json_object_set(object, "depends_on", deps))
        goto error;
    json_decref(deps);
    return object;
error:
    json_decref(deps);
    json_decref(object);
    return NULL;
}

int lcs_resource_config_encode(const lcs_config_t *cfg, uint64_t revision,
                               char **document, size_t *length,
                               char *err, size_t err_len)
{
    if (revision > LCS_RESOURCE_REVISION_MAX)
        return fail(err, err_len, "resource revision exceeds signed 64-bit range");
    lcs_config_t *ordered = malloc(sizeof(*ordered));
    if (!ordered)
        return fail(err, err_len, "out of memory encoding resource configuration");
    *ordered = *cfg;
    if (lcs_config_finalize(ordered, err, err_len))
    {
        free(ordered);
        return -1;
    }
    json_t *root = json_object(), *groups = json_object(), *resources = json_object();
    char *result = NULL;
    int rc = -1;
    if (!root || !groups || !resources ||
        json_object_set_new(root, "schema_version", json_integer(LCS_RESOURCE_SCHEMA_VERSION)) ||
        put_string(root, "cluster", ordered->cluster_name) ||
        json_object_set_new(root, "revision", json_integer((json_int_t)revision)))
        goto out;
    for (size_t i = 0; i < ordered->group_count; i++)
    {
        const lcs_group_config_t *group = &ordered->groups[i];
        json_t *object = json_pack("{s:s,s:s}", "type",
            group->type == LCS_GROUP_KEEP_TOGETHER ? "keep-together" : "anti-affinity",
            "mode", group->mode == LCS_GROUP_MODE_STRICT ? "strict" : "best-effort");
        if (!object || json_object_set_new(groups, group->name, object))
            goto out;
    }
    for (size_t i = 0; i < ordered->resource_count; i++)
        if (json_object_set_new(resources, ordered->resources[i].name,
                                encode_resource(&ordered->resources[i], ordered)))
            goto out;
    if (json_object_set(root, "groups", groups) || json_object_set(root, "resources", resources))
        goto out;
    result = json_dumps(root, JSON_SORT_KEYS | JSON_INDENT(2) | JSON_ENSURE_ASCII);
    if (!result || strlen(result) > LCS_RESOURCE_DOCUMENT_MAX)
        goto out;
    *document = result;
    *length = strlen(result);
    result = NULL;
    rc = 0;
out:
    free(result);
    free(ordered);
    json_decref(root);
    json_decref(groups);
    json_decref(resources);
    if (rc)
        fail(err, err_len, "cannot encode resource JSON within document limits");
    return rc;
}

static int apply_options(json_t *object, int argc, const char *const *argv,
                          bool group, char *err, size_t err_len)
{
    static const char *const resource_options[] = {
        "address", "interface", "systemd_unit", "group", "home_node", "priority",
        "depends_on", "pre_start", "post_start", "pre_stop", "post_stop", NULL
    };
    static const char *const group_options[] = {"type", "mode", NULL};
    const char *const *allowed = group ? group_options : resource_options;
    json_t *seen = json_object();
    if (!seen)
        return fail(err, err_len, "out of memory editing resource configuration");
    int result = -1;
    for (int i = 0; i < argc; i++)
    {
        const char *eq = strchr(argv[i], '=');
        char key[32];
        if (!eq || eq == argv[i] || (size_t)(eq - argv[i]) >= sizeof(key))
        {
            fail(err, err_len, "definition options must use key=value syntax");
            goto out;
        }
        memcpy(key, argv[i], (size_t)(eq - argv[i]));
        key[eq - argv[i]] = '\0';
        bool supported = false;
        for (size_t j = 0; allowed[j]; j++)
            if (!strcmp(key, allowed[j]))
                supported = true;
        if (!supported || json_object_get(seen, key))
        {
            fail(err, err_len, "unknown or repeated definition option");
            goto out;
        }
        if (json_object_set(seen, key, json_true()))
            goto memory;
        const char *value = eq + 1;
        /* Like pcs resource update, an empty value removes the option. */
        if (!*value)
        {
            (void)json_object_del(object, key);
            continue;
        }
        json_t *field = NULL;
        if (!group && !strcmp(key, "priority"))
        {
            uint64_t number = 0;
            for (const char *p = value; *p; p++)
            {
                if (*p < '0' || *p > '9' || number > UINT32_MAX / 10u)
                {
                    fail(err, err_len, "priority must be a positive 32-bit integer");
                    goto out;
                }
                number = number * 10u + (unsigned)(*p - '0');
                if (number > UINT32_MAX)
                {
                    fail(err, err_len, "priority must be a positive 32-bit integer");
                    goto out;
                }
            }
            if (!number)
            {
                fail(err, err_len, "priority must be a positive 32-bit integer");
                goto out;
            }
            field = json_integer((json_int_t)number);
        }
        else if (!group && !strcmp(key, "depends_on"))
        {
            field = json_array();
            const char *start = value;
            for (const char *p = value;; p++)
            {
                if (*p != ',' && *p)
                    continue;
                size_t length = (size_t)(p - start);
                if (!length || length > LCS_NAME_MAX)
                {
                    json_decref(field);
                    fail(err, err_len, "depends_on must contain comma-separated resource names");
                    goto out;
                }
                if (json_array_append_new(field, json_stringn(start, length)))
                {
                    json_decref(field);
                    goto memory;
                }
                if (!*p)
                    break;
                start = p + 1;
            }
        }
        else
            field = json_string(value);
        if (!field || json_object_set_new(object, key, field))
            goto memory;
    }
    result = 0;
    goto out;
memory:
    fail(err, err_len, "out of memory editing resource configuration");
out:
    json_decref(seen);
    return result;
}

static int edit_group(json_t *groups, json_t *resources, int argc,
                       const char *const *argv, char *err, size_t err_len)
{
    if (argc < 4 || !name_valid(argv[3]))
        return fail(err, err_len, "expected resource group ACTION GROUP [resources or options]");
    const char *action = argv[2], *name = argv[3];
    json_t *group = json_object_get(groups, name);
    if (!strcmp(action, "create"))
    {
        if (group)
            return fail(err, err_len, "group already exists");
        group = json_pack("{s:s,s:s}", "type", "keep-together", "mode", "best-effort");
        if (!group || json_object_set_new(groups, name, group))
            return fail(err, err_len, "cannot create group definition");
        return apply_options(group, argc - 4, argv + 4, true, err, err_len);
    }
    if (!strcmp(action, "update"))
    {
        if (!group || argc == 4)
            return fail(err, err_len, "group update requires an existing group and key=value options");
        return apply_options(group, argc - 4, argv + 4, true, err, err_len);
    }
    if (!strcmp(action, "add"))
    {
        if (argc == 4)
            return fail(err, err_len, "group add requires at least one resource");
        if (!group)
        {
            group = json_pack("{s:s,s:s}", "type", "keep-together", "mode", "best-effort");
            if (!group || json_object_set_new(groups, name, group))
                return fail(err, err_len, "cannot create group definition");
        }
        for (int i = 4; i < argc; i++)
        {
            json_t *resource = json_object_get(resources, argv[i]);
            if (!resource)
                return fail(err, err_len, "group add references an unknown resource");
            if (put_string(resource, "group", name))
                return fail(err, err_len, "cannot assign resource to group");
        }
        return 0;
    }
    if (!strcmp(action, "remove") || !strcmp(action, "delete"))
    {
        if (!group)
            return fail(err, err_len, "group does not exist");
        for (int i = 4; i < argc; i++)
        {
            json_t *resource = json_object_get(resources, argv[i]);
            const char *assigned = json_string_value(json_object_get(resource, "group"));
            if (!assigned || strcmp(assigned, name))
                return fail(err, err_len, "resource is not a member of this group");
            json_object_del(resource, "group");
        }
        const char *resource_name;
        json_t *resource;
        bool members_remain = false;
        json_object_foreach(resources, resource_name, resource)
        {
            (void)resource_name;
            const char *assigned = json_string_value(json_object_get(resource, "group"));
            if (!assigned || strcmp(assigned, name))
                continue;
            if (argc == 4)
                json_object_del(resource, "group");
            else
                members_remain = true;
        }
        if (!members_remain)
            json_object_del(groups, name);
        return 0;
    }
    return fail(err, err_len, "unknown resource group definition action");
}

int lcs_resource_config_edit(const lcs_config_t *base, uint64_t current_revision,
                             uint64_t expected_revision, int argc,
                             const char *const *argv, lcs_config_t *candidate,
                             char *err, size_t err_len)
{
    if (expected_revision != current_revision)
        return fail(err, err_len, "configuration revision conflict; refresh and retry");
    if (current_revision >= LCS_RESOURCE_REVISION_MAX)
        return fail(err, err_len, "resource configuration revision exhausted");
    if (argc < 3 || argc > 64 || !argv)
        return fail(err, err_len, "expected a resource definition command");
    for (int i = 0; i < argc; i++)
        if (!argv[i] || strlen(argv[i]) > LCS_PATH_MAX + 32u)
            return fail(err, err_len, "definition argument is missing or too long");
    if (strcmp(argv[0], "resource"))
        return fail(err, err_len, "expected a resource definition command");
    char *document = NULL;
    size_t length;
    if (lcs_resource_config_encode(base, current_revision, &document, &length, err, err_len))
        return -1;
    json_error_t parse_error;
    json_t *root = json_loadb(document, length, JSON_REJECT_DUPLICATES, &parse_error);
    free(document);
    if (!root)
        return fail(err, err_len, "cannot construct definition candidate");
    int rc = -1;
    json_t *resources = json_object_get(root, "resources");
    json_t *groups = json_object_get(root, "groups");
    if (!strcmp(argv[1], "group"))
    {
        if (edit_group(groups, resources, argc, argv, err, err_len))
            goto out;
    }
    else
    {
        if (!name_valid(argv[2]))
        {
            fail(err, err_len, "invalid resource name");
            goto out;
        }
        json_t *resource = json_object_get(resources, argv[2]);
        if (!strcmp(argv[1], "create"))
        {
            if (resource || argc < 4 || (strcmp(argv[3], "vip") && strcmp(argv[3], "service")))
            {
                fail(err, err_len, "resource create requires a new name and type vip or service");
                goto out;
            }
            resource = json_pack("{s:s}", "type", argv[3]);
            if (!resource || json_object_set_new(resources, argv[2], resource))
            {
                fail(err, err_len, "cannot create resource definition");
                goto out;
            }
            if (apply_options(resource, argc - 4, argv + 4, false, err, err_len))
                goto out;
        }
        else if (!strcmp(argv[1], "update"))
        {
            if (!resource || argc < 4)
            {
                fail(err, err_len, "resource update requires an existing resource and key=value options");
                goto out;
            }
            if (apply_options(resource, argc - 3, argv + 3, false, err, err_len))
                goto out;
        }
        else if (!strcmp(argv[1], "delete") || !strcmp(argv[1], "remove"))
        {
            if (!resource || argc != 3)
            {
                fail(err, err_len, "resource delete requires one existing resource name");
                goto out;
            }
            json_object_del(resources, argv[2]);
        }
        else
        {
            fail(err, err_len, "unknown resource definition action");
            goto out;
        }
    }
    if (json_object_set_new(root, "revision", json_integer((json_int_t)current_revision + 1)))
    {
        fail(err, err_len, "cannot assign candidate revision");
        goto out;
    }
    document = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT);
    if (!document)
    {
        fail(err, err_len, "cannot encode definition candidate");
        goto out;
    }
    uint64_t revision;
    rc = lcs_resource_config_decode(document, strlen(document), base, candidate,
                                    &revision, err, err_len);
    free(document);
out:
    json_decref(root);
    return rc;
}
