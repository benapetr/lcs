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

static void test_incarnations(void)
{
    static lcs_config_t active, next, forged, returning;
    char err[256], *document = NULL;
    size_t length;
    uint64_t revision;
    load_base();
    forged = base;
    strcpy(forged.resources[0].post_start, "/bin/false");
    assert(lcs_resource_config_assign_identities(&base, 0, &forged, &next, err, sizeof(err)) == -1);
    assert(strstr(err, "preserve complete"));
    forged = base;
    forged.resources[0].priority_set = false;
    assert(lcs_resource_config_assign_identities(&base, 0, &forged, &next, err, sizeof(err)) == -1);
    assert(lcs_resource_config_assign_identities(&base, 0, &base, &active, err, sizeof(err)) == 0);
    assert(active.resource_schema_version == LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION);
    assert(!active.resources[0].birth_revision && !active.resources[1].birth_revision);
    assert(lcs_resource_config_encode(&active, 0, &document, &length, err, sizeof(err)) == -1);
    assert(lcs_resource_config_encode(&active, 1, &document, &length, err, sizeof(err)) == 0);
    assert(lcs_resource_config_decode(document, length, &base, &returning, &revision, err, sizeof(err)) == 0);
    assert(revision == 1 && returning.resource_schema_version == 2);
    json_error_t parse_error;
    json_t *root = json_loads(document, 0, &parse_error);
    free(document);
    json_t *resource = json_object_get(json_object_get(root, "resources"), "public");
    json_object_del(resource, "birth_revision");
    rejected(root); /* Dynamic identity cannot be omitted or guessed. */
    root = snapshot();
    resource = json_object_get(json_object_get(root, "resources"), "public");
    json_object_set_new(resource, "birth_revision", json_integer(0));
    rejected(root); /* Schema 1 never silently accepts identity metadata. */

#define DYNAMIC_EDIT(rev, ...) do { \
    const char *args[] = {__VA_ARGS__}; \
    assert(lcs_resource_config_edit(&active, rev, rev, (int)(sizeof(args) / sizeof(*args)), \
                                    args, &next, err, sizeof(err)) == 0); \
    assert(lcs_resource_config_validate_successor(&active, rev, &next, (rev) + 1, err, sizeof(err)) == 0); \
    active = next; \
} while (0)
    DYNAMIC_EDIT(1, "resource", "create", "aaa", "vip", "address=192.0.2.22/24", "interface=eth0");
    assert(active.resources[0].birth_revision == 2); /* Sorted before existing resources. */
    assert(active.resources[1].depends_on_idx[0] == 2);
    returning = active; /* Node goes offline before deletion and recreation. */
    DYNAMIC_EDIT(2, "resource", "delete", "aaa");
    DYNAMIC_EDIT(3, "resource", "create", "aaa", "vip", "address=192.0.2.22/24", "interface=eth0");
    assert(active.resources[0].birth_revision == 4);
    assert(returning.resources[0].birth_revision == 2);
    forged = active;
    forged.resources[0].birth_revision = 2;
    assert(lcs_resource_config_validate_successor(&next, 4, &forged, 5, err, sizeof(err)) == -1);
    DYNAMIC_EDIT(4, "resource", "update", "aaa", "address=192.0.2.23/24");
    assert(active.resources[0].birth_revision == 5);
    DYNAMIC_EDIT(5, "resource", "update", "aaa", "post_stop=/bin/true", "priority=9");
    assert(active.resources[0].birth_revision == 5); /* Policies/hooks do not replace backend. */
    DYNAMIC_EDIT(6, "resource", "update", "app", "systemd_unit=other.service");
    assert(active.resources[1].birth_revision == 7);
    assert(!active.resources[2].birth_revision); /* Untouched bootstrap VIP. */
    DYNAMIC_EDIT(7, "resource", "group", "update", "web", "mode=strict");
    assert(active.resources[0].birth_revision == 5 && active.resources[1].birth_revision == 7);
#undef DYNAMIC_EDIT
    assert(lcs_resource_config_encode(&active, 8, &document, &length, err, sizeof(err)) == 0);
    assert(lcs_resource_config_decode(document, length, &base, &returning, &revision, err, sizeof(err)) == 0);
    assert(revision == 8 && returning.resources[0].birth_revision == 5);
    assert(returning.resources[1].birth_revision == 7);
    root = json_loads(document, 0, &parse_error);
    free(document);
    resource = json_object_get(json_object_get(root, "resources"), "aaa");
    json_object_set_new(resource, "birth_revision", json_integer(9));
    rejected(root); /* A birth cannot be ahead of the snapshot revision. */
    forged = active;
    forged.resource_schema_version = 1;
    assert(lcs_resource_config_validate_successor(&active, 8, &forged, 9, err, sizeof(err)) == -1);
    assert(strstr(err, "revert"));
    next = active;
    assert(lcs_resource_config_assign_identities(&active, LCS_RESOURCE_REVISION_MAX,
                                                 &active, &next, err, sizeof(err)) == -1);
    assert(memcmp(&active, &next, sizeof(active)) == 0);
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
    test_incarnations();
    puts("resource JSON unit tests passed");
    return 0;
}
