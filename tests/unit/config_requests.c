// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_requests.h"
#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <jansson.h>
#include <openssl/sha.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

static lcs_config_t base;
static lcs_config_store_t store;
static lcs_config_agreement_t agreement;
static lcs_config_requests_t history;
static char directory[96], error[256];
static unsigned fail_sync;
static unsigned crash_sync;
static bool forbid_sync;
int __real_fsync(int fd);
int __wrap_fsync(int fd)
{
    assert(!forbid_sync);
    if (crash_sync && !--crash_sync) _exit(__real_fsync(fd) ? 78 : 77);
    if (fail_sync && !--fail_sync) { errno = EIO; return -1; }
    return __real_fsync(fd);
}
static lcs_config_value_t proposal(const char *id)
{
    lcs_config_t *configs = calloc(2, sizeof(*configs)); assert(configs);
    uint64_t revision; char *document; size_t length;
    assert(!lcs_resource_config_decode(agreement.committed.document, agreement.committed.length,
        &base, &configs[0], &revision, error, sizeof(error)));
    assert(!lcs_resource_config_assign_identities(&configs[0], revision, &configs[0], &configs[1], error, sizeof(error)));
    assert(!lcs_resource_config_encode(&configs[1], revision + 1, &document, &length, error, sizeof(error)));
    lcs_config_value_t value = {0};
    assert(!lcs_config_value_init(&value, &base, id, document, length, error, sizeof(error)));
    free(document); free(configs); return value;
}
static void learn(const lcs_config_value_t *value)
{
    assert(!lcs_config_agreement_learn(&agreement, agreement.committed.digest,
        value->revision * 32, 3, value, error, sizeof(error)));
}
static int reserve(const lcs_config_value_t *value)
{
    return lcs_config_requests_reserve(&history, &agreement, agreement.committed.revision,
        agreement.committed.digest, value, error, sizeof(error));
}
static void reopen(void)
{
    lcs_config_requests_close(&history);
    assert(!lcs_config_requests_open(&history, &store, &base, false, error, sizeof(error)));
}
static void setup(void)
{
    strcpy(directory, "/tmp/lcs-request-history.XXXXXX"); assert(mkdtemp(directory));
    char path[128]; snprintf(path, sizeof(path), "%s/node.conf", directory);
    FILE *file = fopen(path, "w"); assert(file);
    assert(fputs("[cluster]\nname = requests\nnode = node1\n"
        "[node node1]\nrole = full-member\naddress = 127.0.0.1\n"
        "[node node2]\nrole = full-member\naddress = 127.0.0.1\n"
        "[node node3]\nrole = quorum-only\naddress = 127.0.0.1\n"
        "[vip public]\naddress = 192.0.2.10/32\ninterface = lo\n", file) >= 0);
    assert(!fclose(file)); assert(!lcs_config_load(path, &base, error, sizeof(error))); assert(!unlink(path));
    assert(!lcs_config_store_open(&store, directory, error, sizeof(error)));
    assert(!lcs_config_agreement_open(&agreement, &store, &base, true, error, sizeof(error)));
    assert(lcs_config_requests_open(&history, &store, &base, false, error, sizeof(error)) < 0);
    assert(faccessat(store.directory_fd, "requests.json", F_OK, 0) < 0 && errno == ENOENT);
    assert(!lcs_config_requests_open(&history, &store, &base, true, error, sizeof(error)));
    assert(!history.count && !lcs_config_requests_find(&history, "missing"));
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) < 0);
}
static void identities_and_decisions(void)
{
    lcs_config_value_t first = proposal("request-a");
    char parent[LCS_CONFIG_DIGEST_SIZE]; strcpy(parent, agreement.committed.digest);
    assert(reserve(&first) == LCS_CONFIG_REQUEST_RECORDED);
    char *reserved; size_t reserved_length;
    assert(!lcs_config_requests_export(&history, &reserved, &reserved_length, error, sizeof(error)));
    forbid_sync = true;
    assert(reserve(&first) == LCS_CONFIG_REQUEST_EXISTS && history.count == 1);
    char wrong[LCS_CONFIG_DIGEST_SIZE]; memset(wrong, 'f', 64); wrong[64] = 0;
    assert(lcs_config_requests_reserve(&history, &agreement, 0, wrong, &first, error, sizeof(error)) == LCS_CONFIG_REQUEST_CONFLICT);
    forbid_sync = false;
    assert(!lcs_config_agreement_prepare(&agreement, 1, parent, 32, error, sizeof(error)));
    assert(!lcs_config_agreement_accept(&agreement, parent, 32, &first, error, sizeof(error)));
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) < 0);
    assert(!lcs_config_requests_find(&history, "request-a")->committed); /* Acceptance is not a decision. */
    learn(&first);
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) == LCS_CONFIG_REQUEST_RECORDED);
    assert(lcs_config_requests_find(&history, "request-a")->committed);
    forbid_sync = true;
    assert(lcs_config_requests_reserve(&history, &agreement, 0, parent, &first, error, sizeof(error)) == LCS_CONFIG_REQUEST_EXISTS);
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) == LCS_CONFIG_REQUEST_EXISTS);
    forbid_sync = false;
    lcs_config_requests_t *mirror = malloc(sizeof(*mirror)); assert(mirror); *mirror = history;
    assert(lcs_config_requests_import(mirror, reserved, reserved_length, error, sizeof(error)) < 0 && mirror->poisoned);
    assert(mirror->count == 1 && mirror->records[0].committed);
    free(reserved);
    reopen(); assert(history.count == 1 && history.records[0].committed);
    lcs_config_value_t reused = proposal("request-a");
    forbid_sync = true; assert(reserve(&reused) == LCS_CONFIG_REQUEST_CONFLICT); forbid_sync = false;
    lcs_config_value_clear(&reused);
    lcs_config_value_t second = proposal("request-b"); learn(&second);
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) == LCS_CONFIG_REQUEST_RECORDED);
    assert(history.count == 2 && history.records[1].committed);
    char *document; size_t length;
    assert(!lcs_config_requests_export(&history, &document, &length, error, sizeof(error)));
    *mirror = history; mirror->count = 0;
    forbid_sync = true; assert(!lcs_config_requests_import(mirror, document, length, error, sizeof(error))); forbid_sync = false;
    assert(mirror->count == 2 && mirror->records[1].committed);
    for (size_t size = 0; size < 3; size++)
    { *mirror = history; assert(lcs_config_requests_import(mirror, document, size, error, sizeof(error)) < 0 && mirror->poisoned); }
    free(document); free(mirror); lcs_config_value_clear(&first); lcs_config_value_clear(&second);
}
static void rewrite(json_t *root)
{
    json_object_del(root, "checksum");
    char *canonical = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT | JSON_ENSURE_ASCII); assert(canonical);
    unsigned char hash[SHA256_DIGEST_LENGTH]; assert(SHA256((unsigned char *)canonical, strlen(canonical), hash)); free(canonical);
    char digest[65]; const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < sizeof(hash); i++) { digest[2*i] = hex[hash[i] >> 4]; digest[2*i+1] = hex[hash[i] & 15]; }
    digest[64] = 0; assert(!json_object_set_new(root, "checksum", json_string(digest)));
}
static void invalid_records(void)
{
    char *original; size_t length;
    assert(!lcs_config_requests_export(&history, &original, &length, error, sizeof(error)));
    lcs_config_requests_t *scratch = calloc(1, sizeof(*scratch)); assert(scratch);
    for (unsigned mode = 0; mode < 2; mode++)
    {
        json_error_t parser; json_t *root = json_loadb(original, length, 0, &parser); assert(root);
        json_t *records = json_object_get(root, "requests");
        if (!mode)
        {
            json_t *a = json_deep_copy(json_array_get(records, 0)), *b = json_deep_copy(json_array_get(records, 1));
            assert(a && b && !json_array_set_new(records, 0, b) && !json_array_set_new(records, 1, a));
        }
        else json_object_set_new(json_array_get(records, 0), "value_digest",
            json_string("ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff"));
        rewrite(root); char *document = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT); assert(document);
        *scratch = history;
        assert(lcs_config_requests_import(scratch, document, strlen(document), error, sizeof(error)) < 0 && scratch->poisoned);
        assert(!strcmp(scratch->records[0].value_digest, history.records[0].value_digest));
        free(document); json_decref(root);
    }
    for (unsigned mode = 0; mode < 13; mode++)
    {
        json_error_t parser; json_t *root = json_loadb(original, length, 0, &parser); assert(root);
        json_t *records = json_object_get(root, "requests"), *entry = json_array_get(records, 0);
        if (mode == 0) json_object_set_new(root, "record_version", json_integer(2));
        if (mode == 1) json_object_set_new(root, "unknown", json_true());
        if (mode == 2) json_array_append(records, entry);
        if (mode == 3) json_object_set_new(entry, "request_id", json_string("bad/id"));
        if (mode == 4) json_object_set_new(entry, "request_id", json_stringn("a\0b", 3));
        if (mode == 5) json_object_set_new(entry, "base_revision", json_integer(INT64_MAX));
        if (mode == 6) json_object_set_new(entry, "base_digest", json_string("not-a-digest"));
        if (mode == 7) json_object_set_new(entry, "committed", json_integer(1));
        if (mode == 8) json_object_set_new(root, "member", json_string("node2"));
        if (mode == 9) json_object_set_new(root, "membership", json_string("foreign"));
        if (mode == 10) for (size_t i = 0; i < LCS_CONFIG_REQUEST_HISTORY_MAX; i++) json_array_append(records, entry);
        if (mode == 11) json_object_del(entry, "value_digest");
        rewrite(root);
        if (mode == 12) json_object_set_new(root, "checksum", json_string("bad"));
        char *document = json_dumps(root, JSON_SORT_KEYS | JSON_COMPACT); assert(document); json_decref(root);
        assert(!lcs_config_store_write(&store, "requests.json", document, strlen(document), error, sizeof(error)));
        assert(lcs_config_requests_open(scratch, &store, &base, true, error, sizeof(error)) < 0);
        char *after; size_t after_length;
        assert(!lcs_config_store_read(&store, "requests.json", &after, &after_length, LCS_CONFIG_REQUEST_RECORD_MAX, error, sizeof(error)));
        assert(after_length == strlen(document) && !memcmp(after, document, after_length));
        free(after); free(document);
    }
    assert(!lcs_config_store_write(&store, "requests.json", original, length, error, sizeof(error))); free(original);
    assert(!renameat(store.directory_fd, "requests.json", store.directory_fd, "saved"));
    assert(!symlinkat("agreement.json", store.directory_fd, "requests.json"));
    assert(lcs_config_requests_open(scratch, &store, &base, true, error, sizeof(error)) < 0);
    assert(!unlinkat(store.directory_fd, "requests.json", 0));
    assert(!renameat(store.directory_fd, "saved", store.directory_fd, "requests.json"));
    free(scratch); reopen();
}
static void failures_and_capacity(void)
{
    lcs_config_value_t value = proposal("uncertain");
    pid_t child = fork(); assert(child >= 0);
    if (!child) { crash_sync = 1; (void)reserve(&value); _exit(1); }
    int child_status; assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 77);
    reopen(); assert(history.count == 2 && !lcs_config_requests_find(&history, "uncertain"));
    fail_sync = 1; assert(reserve(&value) < 0 && history.poisoned && history.count == 2);
    assert(reserve(&value) < 0); reopen(); assert(history.count == 2);
    fail_sync = 2; assert(reserve(&value) < 0 && history.poisoned && history.count == 2);
    reopen(); assert(history.count == 3 && !history.records[2].committed);
    learn(&value); fail_sync = 2;
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) < 0 && history.poisoned);
    reopen(); assert(history.records[2].committed); lcs_config_value_clear(&value);
    value = proposal("worker-result"); child = fork(); assert(child >= 0);
    if (!child) _exit(reserve(&value) == LCS_CONFIG_REQUEST_RECORDED ? 0 : 1);
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && !WEXITSTATUS(child_status));
    assert(history.count == 3); /* A child write alone does not publish parent state. */
    char *document; size_t length;
    assert(!lcs_config_store_read(&store, "requests.json", &document, &length, LCS_CONFIG_REQUEST_RECORD_MAX, error, sizeof(error)));
    forbid_sync = true; assert(!lcs_config_requests_import(&history, document, length, error, sizeof(error))); forbid_sync = false;
    assert(history.count == 4 && lcs_config_requests_find(&history, "worker-result")); free(document); lcs_config_value_clear(&value);
    value = proposal("crash-after"); child = fork(); assert(child >= 0);
    if (!child) { crash_sync = 2; (void)reserve(&value); _exit(1); }
    assert(waitpid(child, &child_status, 0) == child && WIFEXITED(child_status) && WEXITSTATUS(child_status) == 77);
    assert(history.count == 4); reopen(); assert(history.count == 5 && lcs_config_requests_find(&history, "crash-after"));
    lcs_config_value_clear(&value);
    while (history.count < LCS_CONFIG_REQUEST_HISTORY_MAX)
    {
        char id[32]; snprintf(id, sizeof(id), "reserved-%zu", history.count);
        value = proposal(id); assert(reserve(&value) == LCS_CONFIG_REQUEST_RECORDED); lcs_config_value_clear(&value);
    }
    value = proposal("overflow"); forbid_sync = true;
    assert(reserve(&value) == LCS_CONFIG_REQUEST_CAPACITY && !history.poisoned);
    lcs_config_value_clear(&value); value = proposal("reserved-127");
    assert(reserve(&value) == LCS_CONFIG_REQUEST_EXISTS); forbid_sync = false;
    learn(&value);
    assert(lcs_config_requests_observe(&history, &agreement, error, sizeof(error)) == LCS_CONFIG_REQUEST_RECORDED);
    lcs_config_value_clear(&value); reopen(); assert(history.count == LCS_CONFIG_REQUEST_HISTORY_MAX && history.records[127].committed);
    assert(lcs_config_requests_find(&history, "request-a")->committed && !lcs_config_requests_find(&history, "overflow"));
    struct stat status; assert(!fstatat(store.directory_fd, "requests.json", &status, 0) && (status.st_mode & 0777) == 0600);
}
int main(void)
{
    setup(); identities_and_decisions(); invalid_records(); failures_and_capacity();
    lcs_config_requests_close(&history); lcs_config_agreement_close(&agreement);
    assert(!unlinkat(store.directory_fd, "requests.json", 0)); assert(!unlinkat(store.directory_fd, "agreement.json", 0));
    assert(!unlinkat(store.directory_fd, "lock", 0)); lcs_config_store_close(&store); assert(!rmdir(directory));
    puts("durable request identity, decision facts, replay and sync-failure unit tests passed"); return 0;
}
