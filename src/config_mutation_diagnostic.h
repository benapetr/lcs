// SPDX-License-Identifier: GPL-3.0-or-later
#ifndef LCS_CONFIG_MUTATION_DIAGNOSTIC_H
#define LCS_CONFIG_MUTATION_DIAGNOSTIC_H
#include "config_requests.h"

typedef struct
{
    const lcs_config_value_t *current, *application_target;
    const char *application_error, *storage_error;
    bool storage_failed;
} lcs_config_mutation_diagnostic_t;
/* Explain already-published local facts without changing their outcome. Current
 * application errors apply only to the exact pending attempt request/revision/digest,
 * which can differ from the latest committed value.
 * Historical admission remains true despite later failures. No I/O or authority. */
void lcs_config_mutation_explain(const lcs_config_request_view_t *view,
                                   const lcs_config_mutation_diagnostic_t *diagnostic, char reason[256]);
#endif
