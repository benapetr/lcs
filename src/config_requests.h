// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_REQUESTS_H
#define LCS_CONFIG_REQUESTS_H
#include "config_agreement.h"

#define LCS_CONFIG_REQUEST_HISTORY_MAX 128u
#define LCS_CONFIG_REQUEST_RECORD_MAX (128u * 1024u)
enum { LCS_CONFIG_REQUEST_RECORDED = 0, LCS_CONFIG_REQUEST_EXISTS = 1,
       LCS_CONFIG_REQUEST_CONFLICT = 2, LCS_CONFIG_REQUEST_CAPACITY = 3 };
typedef struct
{
    char id[LCS_CONFIG_REQUEST_MAX + 1];
    uint64_t base_revision;
    char base_digest[LCS_CONFIG_DIGEST_SIZE], value_digest[LCS_CONFIG_DIGEST_SIZE];
    bool committed; /* Exact locally learned decision; NOT admission/client success. */
} lcs_config_request_record_t;
typedef struct
{
    lcs_config_store_t *store;
    const lcs_config_t *base;
    char member[LCS_NAME_MAX + 1], membership[LCS_CONFIG_DIGEST_SIZE];
    lcs_config_request_record_t records[LCS_CONFIG_REQUEST_HISTORY_MAX];
    size_t count;
    bool poisoned;
} lcs_config_requests_t;

/* Explicit storage primitive, not automatically initialized by the daemon.
 * Synchronous writes belong in a future serialized storage job shared with
 * agreement, never the lease event loop. Hold the store's exclusive lock
 * throughout handle/worker lifetime; inherited lock FDs do not serialize children.
 * Missing state requires explicit authorization; existing invalid state is never
 * reset. New empty history cannot establish outcomes for earlier/missed requests. */
int lcs_config_requests_open(lcs_config_requests_t *history, lcs_config_store_t *store,
                               const lcs_config_t *base, bool allow_initialize, char *err, size_t err_len);
void lcs_config_requests_close(lcs_config_requests_t *history);
/* Healthy handle only. Borrowed until the next mutation/close. Absence is
 * unknown, never rejection/abort or permission to reuse a forgotten identifier. */
const lcs_config_request_record_t *lcs_config_requests_find(const lcs_config_requests_t *history, const char *id);
/* Reserve an immutable request ID/expected-base/complete-value fingerprint before future
 * proposal submission. Exact retries return EXISTS even after learning advances;
 * they do not authorize another proposal or validate successor eligibility.
 * No eviction: capacity blocks new IDs. */
int lcs_config_requests_reserve(lcs_config_requests_t *history, const lcs_config_agreement_t *agreement,
                                  uint64_t expected_revision, const char *expected_digest,
                                  const lcs_config_value_t *requested, char *err, size_t err_len);
/* Only the current exact learned majority decision can become committed. No
 * acceptance/promise, revision comparison or timer proves commitment/admission. */
int lcs_config_requests_observe(lcs_config_requests_t *history, const lcs_config_agreement_t *agreement,
                                  char *err, size_t err_len);
/* Same-host worker handoff, not network evidence. Import may append immutable
 * identities and advance committed bits; it cannot drop/reorder/regress history. */
int lcs_config_requests_export(const lcs_config_requests_t *history, char **document, size_t *length, char *err, size_t err_len);
int lcs_config_requests_import(lcs_config_requests_t *history, const char *document, size_t length, char *err, size_t err_len);
#endif
