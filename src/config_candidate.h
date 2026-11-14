// SPDX-License-Identifier: GPL-3.0-or-later

#ifndef LCS_CONFIG_CANDIDATE_H
#define LCS_CONFIG_CANDIDATE_H

#include "config.h"

/* Resource-only change boundary, shared by INI reload and JSON/CLI candidates.
 * validate takes already finalized configs. prepare canonicalizes and validates
 * a private copy, permits aliased output, and leaves output unchanged on error.
 * These helpers do not authorize agreement, drain backends or apply changes. */
int lcs_config_candidate_validate(const lcs_config_t *active,
                                   const lcs_config_t *candidate,
                                   char *error, size_t error_len);
int lcs_config_candidate_prepare(const lcs_config_t *active,
                                  const lcs_config_t *proposed,
                                  lcs_config_t *candidate,
                                  char *error, size_t error_len);

/* Compare finalized effective definitions using legacy reload semantics,
 * independent of resolved resource/group indices. Dynamic agreement must use
 * the complete JSON digest instead (including explicit/default field intent).
 * A matching backend alone is insufficient to establish dynamic incarnation. */
bool lcs_config_backend_equal(const lcs_resource_config_t *a,
                               const lcs_resource_config_t *b);
bool lcs_config_resources_equal(const lcs_config_t *a, const lcs_config_t *b);

#endif
