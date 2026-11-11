// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_mutation_diagnostic.h"
#include <stdio.h>
#include <string.h>

static bool matches(const lcs_config_value_t *value, const lcs_config_request_view_t *view)
{
    return value && value->document && value->revision == view->base_revision + 1 &&
        !strcmp(value->request_id, view->id) && !strcmp(value->digest, view->value_digest);
}

void lcs_config_mutation_explain(const lcs_config_request_view_t *view,
                                   const lcs_config_mutation_diagnostic_t *diagnostic, char reason[256])
{
    if (view->state == LCS_CONFIG_REQUEST_APPLIED)
        snprintf(reason, 256, "decision durable and local configuration admission completed");
    else if (diagnostic->storage_failed)
        snprintf(reason, 256, "%s; configuration recovery required: %.120s. Retain state, repair and restart; query this request ID",
            view->state == LCS_CONFIG_REQUEST_COMMITTED ? "decision committed" : "outcome unknown",
            diagnostic->storage_error && *diagnostic->storage_error ? diagnostic->storage_error : "configuration coordinator/storage failed");
    else if (view->state == LCS_CONFIG_REQUEST_COMMITTED)
    {
        if (diagnostic->application_error && *diagnostic->application_error && matches(diagnostic->application_target, view))
            snprintf(reason, 256, "decision committed; local admission blocked: %.120s. Inspect config status; repair and restart if cleanup failed",
                diagnostic->application_error);
        else if (!matches(diagnostic->current, view))
            snprintf(reason, 256, "decision committed; no local admission receipt for this historical request. Query request-status; inspect config status for current progress");
        else
            snprintf(reason, 256, "decision committed; local admission incomplete. Inspect config status and query request-status for this ID");
    }
    else if (view->history == LCS_CONFIG_HISTORY_RECOVERY)
        snprintf(reason, 256, "outcome unknown; request history requires recovery. Retain state and inspect config status before retrying this ID");
    else if (view->state == LCS_CONFIG_REQUEST_RESERVED)
        snprintf(reason, 256, "outcome unknown; original reservation retained. Query request-status; exact retries do not submit a second proposal");
    else
        snprintf(reason, 256, "outcome unknown; storage/consensus may still complete. Query or retry this request ID on the original node");
}
