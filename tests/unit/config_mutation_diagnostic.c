// SPDX-License-Identifier: GPL-3.0-or-later
#include "config_mutation_diagnostic.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>
int main(void)
{
    lcs_config_request_view_t view = {.history = LCS_CONFIG_HISTORY_AVAILABLE, .state = LCS_CONFIG_REQUEST_COMMITTED, .base_revision = 6};
    strcpy(view.id, "edit-a"); memset(view.value_digest, 'a', 64);
    lcs_config_value_t current = {.document = "{}", .revision = 7};
    strcpy(current.request_id, view.id); strcpy(current.digest, view.value_digest);
    lcs_config_mutation_diagnostic_t diagnostic = {.current = &current, .application_target = &current,
        .application_error = "resource vip1 cleanup failed; confirm cleanup/fencing"};
    char reason[257]; memset(reason, '!', sizeof(reason));
    lcs_config_request_view_t original = view;
    lcs_config_mutation_explain(&view, &diagnostic, reason);
    assert(strstr(reason, "decision committed") && strstr(reason, "vip1 cleanup failed") && strstr(reason, "restart"));
    assert(!strstr(reason, "rejected") && reason[256] == '!' && !memcmp(&view, &original, sizeof(view)));
    /* A later, different request's failed stop is not this request's failure. */
    current.revision = 8;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(!strstr(reason, "vip1") && strstr(reason, "historical"));
    /* A blocked older pending target retains its own diagnostic after learning
     * advances; a newer request must not borrow that older failure. */
    lcs_config_value_t older = current; older.revision = 7; diagnostic.application_target = &older;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strstr(reason, "vip1 cleanup failed"));
    view.base_revision = 7;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(!strstr(reason, "vip1") && strstr(reason, "admission incomplete"));
    view.base_revision = 6; diagnostic.application_target = &current;
    current.revision = 7; current.digest[0] = 'b';
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(!strstr(reason, "vip1"));
    strcpy(current.digest, view.value_digest); strcpy(current.request_id, "edit-b");
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(!strstr(reason, "vip1"));
    strcpy(current.request_id, view.id); diagnostic.application_error = "";
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strstr(reason, "admission incomplete") && strstr(reason, "config status"));
    diagnostic.storage_failed = true; diagnostic.storage_error = "configuration store fsync: No space left on device";
    lcs_config_mutation_explain(&view, &diagnostic, reason);
    assert(strstr(reason, "decision committed") && strstr(reason, "No space left") && strstr(reason, "Retain state"));
    /* Historical admission never becomes a failure because later storage fails. */
    view.state = LCS_CONFIG_REQUEST_APPLIED;
    lcs_config_mutation_explain(&view, &diagnostic, reason);
    assert(strstr(reason, "admission completed") && !strstr(reason, "No space") && !strstr(reason, "failed"));
    view.state = LCS_CONFIG_REQUEST_RESERVED;
    lcs_config_mutation_explain(&view, &diagnostic, reason);
    assert(strstr(reason, "outcome unknown") && strstr(reason, "No space left") && !strstr(reason, "decision committed"));
    diagnostic.storage_failed = false;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strstr(reason, "reservation retained"));
    view.state = LCS_CONFIG_REQUEST_UNKNOWN; view.history = LCS_CONFIG_HISTORY_RECOVERY;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strstr(reason, "history requires recovery"));
    view.history = LCS_CONFIG_HISTORY_UNAVAILABLE;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strstr(reason, "outcome unknown") && strstr(reason, "may still complete"));
    char long_error[1024]; memset(long_error, 'x', sizeof(long_error) - 1); long_error[sizeof(long_error) - 1] = 0;
    diagnostic.storage_failed = true; diagnostic.storage_error = long_error;
    lcs_config_mutation_explain(&view, &diagnostic, reason); assert(strlen(reason) < 256 && reason[256] == '!' && strstr(reason, "restart"));
    puts("scope-bound mutation failure diagnostics unit tests passed");
    return 0;
}
