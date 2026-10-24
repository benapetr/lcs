// SPDX-License-Identifier: GPL-3.0-or-later

#include "resource_config.h"

#include <assert.h>
#include <jansson.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static lcs_config_t base;
static void edit(bool success, int argc, const char *const *argv)
{
    lcs_config_t *next = malloc(sizeof(*next));
    lcs_config_t *before = malloc(sizeof(*before));
    assert(next && before);
    memset(next, 0x3b, sizeof(*next));
    *before = *next;
    char err[256] = "";
    int result = lcs_resource_config_edit(&base, 42, 42, argc, argv, next, err, sizeof(err));
    if (success)
    {
        assert(result == 0);
        base = *next;
    }
    else
    {
        assert(result == -1 && *err);
        assert(memcmp(next, before, sizeof(*next)) == 0);
    }
    free(next);
    free(before);
}

#define EDIT(ok, ...) edit(ok, (int)(sizeof((const char *const[]){__VA_ARGS__}) / sizeof(char *)), \
                           (const char *const[]){__VA_ARGS__})

static void load_base(void)
{
    char path[] = "/tmp/lcs-resource-config.XXXXXX";
    int fd = mkstemp(path);
    assert(fd >= 0);
    FILE *f = fdopen(fd, "w");
    assert(f);
    assert(fputs("[cluster]\nname = test\nnode = node1\n"
        "[node node1]\nrole = full-member\naddress = 127.0.0.1\n"
        "[group web]\ntype = keep-together\nmode = best-effort\n"
        "[vip public]\naddress = 192.0.2.10/24\ninterface = eth0\n"
        "group = web\npriority = 2\nhome_node = node1\n"
        "pre_start = /bin/true\npost_start = /bin/echo\n"
        "pre_stop = /usr/bin/true\npost_stop = /usr/bin/echo\n"
        "[service app]\nsystemd_unit = app.service\ngroup = web\n"
        "priority = 1\ndepends_on = public\n", f) >= 0);
    assert(fclose(f) == 0);
    char err[256];
    assert(lcs_config_load(path, &base, err, sizeof(err)) == 0);
    assert(unlink(path) == 0);
}

static json_t *snapshot(void)
{
    char *text = NULL, err[256];
    size_t len;
    assert(lcs_resource_config_encode(&base, 42, &text, &len, err, sizeof(err)) == 0);
    json_error_t error;
    json_t *root = json_loadb(text, len, JSON_REJECT_DUPLICATES, &error);
    free(text);
    assert(root);
    return root;
}

static void rejected_text(const char *text, size_t len)
{
    lcs_config_t output;
    memset(&output, 0x7a, sizeof(output));
    lcs_config_t *before = malloc(sizeof(*before));
    assert(before);
    *before = output;
    uint64_t revision = 123;
    char err[256] = "";
    assert(lcs_resource_config_decode(text, len, &base, &output,
                                      &revision, err, sizeof(err)) == -1);
    assert(*err);
    assert(revision == 123);
    assert(memcmp(&output, before, sizeof(output)) == 0);
    free(before);
}

static void rejected(json_t *root)
{
    char *text = json_dumps(root, JSON_COMPACT);
    assert(text);
    rejected_text(text, strlen(text));
    free(text);
    json_decref(root);
}

int main(void)
{
    load_base();
    char *text = NULL, *again = NULL, err[256];
    size_t len = 0, again_len = 0;
    assert(lcs_resource_config_encode(&base, 42, &text, &len, err, sizeof(err)) == 0);
    lcs_config_t candidate;
    uint64_t revision = 0;
    assert(lcs_resource_config_decode(text, len, &base, &candidate,
                                      &revision, err, sizeof(err)) == 0);
    assert(revision == 42);
    assert(lcs_config_voting_fingerprint(&base) == lcs_config_voting_fingerprint(&candidate));
    assert(lcs_config_full_fingerprint(&base) == lcs_config_full_fingerprint(&candidate));
    assert(strcmp(candidate.resources[1].pre_start, "/bin/true") == 0);
    assert(strcmp(candidate.resources[1].post_stop, "/usr/bin/echo") == 0);
    assert(lcs_resource_config_encode(&candidate, revision, &again,
                                      &again_len, err, sizeof(err)) == 0);
    assert(len == again_len && memcmp(text, again, len) == 0);
    free(again);
    /* Sorted definitions and dependency names must not depend on input order. */
    json_t *root = snapshot();
    json_t *resources = json_object_get(root, "resources");
    assert(json_object_set_new(resources, "another", json_pack("{s:s,s:s}",
        "type", "vip", "address", "192.0.2.11/24")) == 0);
    assert(json_object_set_new(json_object_get(resources, "another"),
                               "interface", json_string("eth0")) == 0);
    json_t *app = json_object_get(resources, "app");
    assert(json_object_set_new(app, "depends_on", json_pack("[s,s]", "public", "another")) == 0);
    char *first = json_dumps(root, JSON_COMPACT);
    assert(lcs_resource_config_decode(first, strlen(first), &base, &candidate,
                                      &revision, err, sizeof(err)) == 0);
    assert(lcs_resource_config_encode(&candidate, revision, &again,
                                      &again_len, err, sizeof(err)) == 0);
    assert(json_object_set_new(app, "depends_on", json_pack("[s,s]", "another", "public")) == 0);
    char *second = json_dumps(root, JSON_COMPACT), *canonical = NULL;
    size_t canonical_len;
    assert(lcs_resource_config_decode(second, strlen(second), &base, &candidate,
                                      &revision, err, sizeof(err)) == 0);
    assert(lcs_resource_config_encode(&candidate, revision, &canonical,
                                      &canonical_len, err, sizeof(err)) == 0);
    assert(canonical_len == again_len && strcmp(canonical, again) == 0);
    free(first); free(second); free(again); free(canonical); json_decref(root);

    const char *bad[] = { "", "null", "[]", "{}", "{}{}",
        "{\"revision\":1,\"revision\":2}", "{\"schema_version\":1.0}", "{]" };
    for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); i++)
        rejected_text(bad[i], strlen(bad[i]));
    rejected_text(text, LCS_RESOURCE_DOCUMENT_MAX + 1); /* Reject before reading. */
    free(text);
    const char *top_fields[] = {"schema_version", "revision", "cluster", "groups", "resources"};
    for (size_t i = 0; i < sizeof(top_fields) / sizeof(top_fields[0]); i++)
    {
        root = snapshot();
        assert(json_object_del(root, top_fields[i]) == 0);
        rejected(root);
    }
    root = snapshot(); json_object_set_new(root, "cluster", json_string("other")); rejected(root);
    root = snapshot(); json_object_set_new(root, "schema_version", json_integer(2)); rejected(root);
    root = snapshot(); json_object_set_new(root, "revision", json_integer(-1)); rejected(root);
    root = snapshot(); json_object_set_new(root, "future", json_true()); rejected(root);
    root = snapshot(); json_object_set_new(root, "cluster", json_stringn("test\0x", 6)); rejected(root);

    const char *fields[] = {"unknown", "priority", "depends_on", "systemd_unit", "pre_start"};
    json_t *values[] = {json_string("x"), json_integer(0), json_pack("[s,s]", "public", "public"),
                        json_string("bad.timer"), json_string("relative/path")};
    for (size_t i = 0; i < 5; i++)
    {
        root = snapshot();
        app = json_object_get(json_object_get(root, "resources"), "app");
        assert(json_object_set_new(app, fields[i], values[i]) == 0);
        rejected(root);
    }
    root = snapshot();
    app = json_object_get(json_object_get(root, "resources"), "app");
    json_object_set_new(app, "depends_on", json_pack("[s]", "app")); rejected(root);
    root = snapshot();
    app = json_object_get(json_object_get(root, "resources"), "app");
    json_object_set_new(app, "depends_on", json_pack("[s]", "missing")); rejected(root);
    root = snapshot();
    json_object_set_new(json_object_get(root, "groups"), "web",
                        json_pack("{s:s,s:s}", "type", "spread", "mode", "strict")); rejected(root);

    /* Empty snapshots are valid; base daemon settings never come from JSON. */
    root = snapshot();
    json_object_set_new(root, "resources", json_object());
    json_object_set_new(root, "groups", json_object());
    first = json_dumps(root, JSON_COMPACT);
    assert(lcs_resource_config_decode(first, strlen(first), &base, &candidate,
                                      &revision, err, sizeof(err)) == 0);
    assert(!candidate.resource_count && !candidate.group_count);
    assert(candidate.port == base.port && candidate.node_count == base.node_count);
    free(first); json_decref(root);
    EDIT(true, "resource", "create", "extra", "vip", "address=192.0.2.30/24", "interface=eth0");
    assert(lcs_config_resource_index(&base, "extra") >= 0);
    EDIT(false, "resource", "create", "extra", "vip", "address=192.0.2.30/24", "interface=eth0");
    EDIT(false, "resource", "create", "bad", "service", "systemd_unit=bad.timer");
    EDIT(false, "resource", "update", "public", "interface=");
    EDIT(false, "resource", "update", "public", "type=service");
    EDIT(false, "resource", "update", "public", "priority=4294967296");
    EDIT(false, "resource", "update", "public", "priority=18446744073709551616");
    EDIT(false, "resource", "update", "public", "priority=1.0");
    EDIT(false, "resource", "update", "public", "priority=1", "priority=2");
    EDIT(false, "resource", "update", "app", "depends_on=public,public");
    EDIT(false, "resource", "update", "app", "depends_on=public,");
    EDIT(false, "resource", "delete", "public"); /* Still a dependency. */
    EDIT(true, "resource", "update", "public", "home_node=", "pre_start=/usr/bin/true");
    assert(!*base.resources[lcs_config_resource_index(&base, "public")].home_node_name);
    EDIT(true, "resource", "group", "add", "spread", "public");
    EDIT(true, "resource", "group", "update", "spread", "type=anti-affinity", "mode=strict");
    int group_index = lcs_config_group_index(&base, "spread");
    assert(base.groups[group_index].type == LCS_GROUP_ANTI_AFFINITY);
    assert(base.groups[group_index].mode == LCS_GROUP_MODE_STRICT);
    EDIT(false, "resource", "group", "update", "spread", "mode=unknown");
    EDIT(false, "resource", "group", "add", "spread", "missing");
    EDIT(false, "resource", "group", "remove", "spread", "app");
    EDIT(true, "resource", "group", "remove", "spread", "public");
    assert(lcs_config_group_index(&base, "spread") == -1);
    EDIT(true, "resource", "group", "create", "empty", "type=anti-affinity");
    EDIT(true, "resource", "group", "delete", "empty");
    EDIT(true, "resource", "group", "remove", "web");
    assert(lcs_config_resource_index(&base, "app") >= 0);
    assert(!*base.resources[lcs_config_resource_index(&base, "app")].group_name);
    EDIT(true, "resource", "update", "app", "depends_on=");
    EDIT(true, "resource", "delete", "public");
    EDIT(true, "resource", "remove", "app");
    assert(lcs_config_resource_index(&base, "app") == -1);
    const char *const remove_extra[] = {"resource", "delete", "extra"};
    assert(lcs_resource_config_edit(&base, 42, 41, 3, remove_extra, &candidate, err, sizeof(err)) == -1);
    assert(strstr(err, "revision conflict"));
    assert(lcs_resource_config_edit(&base, LCS_RESOURCE_REVISION_MAX,
        LCS_RESOURCE_REVISION_MAX, 3, remove_extra, &candidate, err, sizeof(err)) == -1);
    assert(strstr(err, "exhausted"));
    puts("resource JSON unit tests passed");
    return 0;
}
