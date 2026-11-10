// SPDX-License-Identifier: GPL-3.0-or-later

#include "config_runtime.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lcs_config_runtime_close(lcs_config_runtime_t *runtime)
{
    lcs_config_requests_close(&runtime->requests);
    lcs_config_barrier_close(&runtime->barrier);
    lcs_config_application_close(&runtime->application);
    lcs_config_agreement_close(&runtime->agreement);
    lcs_config_activation_close(&runtime->activation);
    free(runtime->base);
    memset(runtime, 0, sizeof(*runtime));
}

int lcs_config_runtime_open(lcs_config_runtime_t *runtime,
                             lcs_config_store_t *store, const lcs_config_t *cfg,
                             char *err, size_t err_len)
{
    memset(runtime, 0, sizeof(*runtime));
    /* Use the same bounded, no-follow/ownership-checked reads as recovery. A
     * malformed or unsafe file is not evidence of a never-initialized node. */
    char *record = NULL;
    size_t length;
    int requests = lcs_config_store_read(store, "requests.json", &record, &length,
                                          LCS_CONFIG_REQUEST_RECORD_MAX, err, err_len);
    free(record);
    record = NULL;
    if (requests < 0)
        return -1;
    int barrier = lcs_config_store_read(store, "barrier.json", &record, &length,
                                        LCS_CONFIG_BARRIER_MAX, err, err_len);
    free(record);
    record = NULL;
    if (barrier < 0)
        return -1;
    int intent = lcs_config_store_read(store, "activation.json", &record, &length,
                                       LCS_CONFIG_ACTIVATION_MAX, err, err_len);
    free(record);
    record = NULL;
    if (intent < 0)
        return -1;
    if (!intent)
    {
        runtime->base = malloc(sizeof(*runtime->base));
        if (!runtime->base)
        {
            if (err && err_len)
                snprintf(err, err_len, "configuration recovery: out of memory retaining frozen base");
            return -1;
        }
        int result = lcs_config_activation_open(&runtime->activation, store, cfg, runtime->base,
                                                 false, err, err_len);
        if (!result && !barrier && runtime->activation.phase != LCS_CONFIG_ACTIVATION_PREPARED)
        {
            if (err && err_len)
                snprintf(err, err_len, "configuration recovery: barrier without prepared initialization");
            result = -1;
        }
        if (!result)
            result = lcs_config_activation_resume(&runtime->activation, &runtime->agreement,
                                                    &runtime->application, err, err_len);
        if (!result && !barrier)
            result = lcs_config_barrier_open(&runtime->barrier, &runtime->activation,
                &runtime->agreement, &runtime->application, NULL, err, err_len);
        if (!result)
            result = requests ? 0 : lcs_config_requests_recover(&runtime->requests, &runtime->agreement, false, err, err_len);
        if (result)
        {
            lcs_config_runtime_close(runtime);
            return -1;
        }
        runtime->requires_recovery = true;
        return 0;
    }
    if (!barrier)
    {
        if (err && err_len)
            snprintf(err, err_len, "configuration recovery: activation barrier exists without its frozen initialization; recovery required");
        return -1;
    }
    int journal = lcs_config_store_read(store, "agreement.json", &record, &length,
                                        LCS_CONFIG_JOURNAL_MAX, err, err_len);
    free(record);
    if (journal < 0)
        return -1;
    record = NULL;
    int catalog = lcs_config_store_read(store, "application.json", &record, &length,
                                        LCS_CONFIG_APPLICATION_MAX, err, err_len);
    free(record);
    if (catalog < 0)
        return -1;
    if (journal == 1 && catalog == 1)
    {
        if (!requests)
        {
            if (err && err_len)
                snprintf(err, err_len, "configuration recovery: orphan request history without agreement/application state; recovery required");
            return -1;
        }
        return 0;
    }
    runtime->base = malloc(sizeof(*runtime->base));
    if (!runtime->base)
    {
        if (err && err_len)
            snprintf(err, err_len, "configuration recovery: out of memory retaining static base");
        return -1;
    }
    *runtime->base = *cfg;
    if (lcs_config_agreement_open(&runtime->agreement, store, runtime->base,
                                  false, err, err_len) ||
        lcs_config_application_open(&runtime->application, &runtime->agreement,
                                    false, err, err_len) ||
        (!requests && lcs_config_requests_recover(&runtime->requests, &runtime->agreement, false, err, err_len)))
    {
        lcs_config_runtime_close(runtime);
        return -1;
    }
    runtime->requires_recovery = true;
    return 0;
}
