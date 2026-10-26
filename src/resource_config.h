// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_RESOURCE_CONFIG_H
#define LCS_RESOURCE_CONFIG_H

#include "config.h"

#define LCS_RESOURCE_SCHEMA_VERSION 1
#define LCS_RESOURCE_DYNAMIC_SCHEMA_VERSION 2
#define LCS_RESOURCE_DOCUMENT_MAX (512u * 1024u)
#define LCS_RESOURCE_REVISION_MAX INT64_MAX

/* These helpers construct candidates only. They never authorize a commit or
 * change the running daemon. Caller owns the malloc-allocated encoded result.
 * Decoding replaces only groups/resources in a copy of base. Outputs are left
 * untouched on failure. Revision zero is reserved for legacy bootstrap. */
int lcs_resource_config_encode(const lcs_config_t *cfg, uint64_t revision,
                               char **document, size_t *length,
                               char *err, size_t err_len);
int lcs_resource_config_decode(const char *document, size_t length,
                               const lcs_config_t *base, lcs_config_t *candidate,
                               uint64_t *revision, char *err, size_t err_len);

/* Construct a validated candidate from pcs-style argv (starting with resource).
 * This is deliberately not a live CLI endpoint. A future distributed transaction
 * must authorize and durably commit the result before runtime application. */
int lcs_resource_config_edit(const lcs_config_t *base, uint64_t current_revision,
                             uint64_t expected_revision, int argc,
                             const char *const *argv, lcs_config_t *candidate,
                             char *err, size_t err_len);

/* Identity-aware candidates for future dynamic authority. Activation is a
 * definitions-preserving successor from schema 1 to 2; later edits allocate
 * birth_revision only for new/replaced backends. validate_successor verifies
 * immediate history (including schema downgrade rejection), not quorum proof.
 * assign validates a private candidate and leaves output untouched on error. */
int lcs_resource_config_assign_identities(const lcs_config_t *base,
                                          uint64_t current_revision,
                                          const lcs_config_t *proposed,
                                          lcs_config_t *candidate,
                                          char *err, size_t err_len);
int lcs_resource_config_validate_successor(const lcs_config_t *base,
                                           uint64_t current_revision,
                                           const lcs_config_t *candidate,
                                           uint64_t revision,
                                           char *err, size_t err_len);

#endif
