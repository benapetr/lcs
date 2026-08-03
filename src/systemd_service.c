// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "systemd_service.h"

#include "log.h"

#include <errno.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

static bool systemd_test_stop_failed(void)
{
    const char *fail_file = getenv("LCS_SYSTEMD_FAIL_STOP_FILE");
    return getenv("LCS_SYSTEMD_FAIL_STOP") != NULL ||
           (fail_file && *fail_file && access(fail_file, F_OK) == 0);
}

#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <unistd.h>

static int service_get_active_state(sd_bus *bus, const lcs_resource_config_t *res, char **state)
{
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    char *unit_path = NULL;
    int rc = sd_bus_call_method(bus,
                                "org.freedesktop.systemd1",
                                "/org/freedesktop/systemd1",
                                "org.freedesktop.systemd1.Manager",
                                "GetUnit",
                                &err,
                                &reply,
                                "s",
                                res->systemd_unit);
    if (rc < 0)
    {
        if (sd_bus_error_has_name(&err, "org.freedesktop.systemd1.NoSuchUnit"))
        {
            /* An installed but currently unloaded unit is already inactive. */
            *state = strdup("inactive");
            sd_bus_error_free(&err);
            return *state ? 0 : -1;
        }
        lcs_log_warn("systemd GetUnit failed for service %s unit=%s: %s",
                     res->name, res->systemd_unit,
                     err.message ? err.message : "D-Bus call failed");
        sd_bus_error_free(&err);
        return -1;
    }
    if (sd_bus_message_read(reply, "o", &unit_path) < 0)
    {
        sd_bus_message_unref(reply);
        sd_bus_error_free(&err);
        return -1;
    }
    unit_path = strdup(unit_path);
    sd_bus_message_unref(reply);
    sd_bus_error_free(&err);
    if (!unit_path)
        return -1;

    err = SD_BUS_ERROR_NULL;
    rc = sd_bus_get_property_string(bus,
                                    "org.freedesktop.systemd1",
                                    unit_path,
                                    "org.freedesktop.systemd1.Unit",
                                    "ActiveState",
                                    &err,
                                    state);
    if (rc < 0)
    {
        lcs_log_warn("systemd ActiveState failed for service %s unit=%s: %s",
                     res->name, res->systemd_unit,
                     err.message ? err.message : "D-Bus property failed");
        free(unit_path);
        sd_bus_error_free(&err);
        return -1;
    }
    free(unit_path);
    sd_bus_error_free(&err);
    return 0;
}

static int service_wait_state(sd_bus *bus, const lcs_resource_config_t *res, bool want_active)
{
    for (unsigned i = 0; i < 50; i++)
    {
        char *state = NULL;
        if (service_get_active_state(bus, res, &state) != 0)
            return -1;
        bool done = want_active ?
                    strcmp(state, "active") == 0 :
                    (strcmp(state, "inactive") == 0 || strcmp(state, "failed") == 0);
        free(state);
        if (done)
            return 0;
        usleep(100000);
    }
    lcs_log_warn("systemd service %s unit=%s did not reach %s state in time",
                 res->name, res->systemd_unit, want_active ? "active" : "inactive");
    return -1;
}

static int service_call_unit_method(const lcs_resource_config_t *res, const char *method, bool wait_active)
{
    sd_bus *bus = NULL;
    sd_bus_error err = SD_BUS_ERROR_NULL;
    sd_bus_message *reply = NULL;
    const char *job = NULL;
    int rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        lcs_log_warn("systemd D-Bus connect failed for service %s unit=%s rc=%d",
                     res->name, res->systemd_unit, rc);
        return -1;
    }

    rc = sd_bus_call_method(bus,
                            "org.freedesktop.systemd1",
                            "/org/freedesktop/systemd1",
                            "org.freedesktop.systemd1.Manager",
                            method,
                            &err,
                            &reply,
                            "ss",
                            res->systemd_unit,
                            "replace");
    if (rc < 0)
    {
        lcs_log_warn("systemd %s failed for service %s unit=%s: %s",
                     method, res->name, res->systemd_unit,
                     err.message ? err.message : "D-Bus call failed");
        sd_bus_error_free(&err);
        sd_bus_unref(bus);
        return -1;
    }
    (void)sd_bus_message_read(reply, "o", &job);
    lcs_log_info("systemd %s requested for service %s unit=%s job=%s",
                 method, res->name, res->systemd_unit, job ? job : "-");
    sd_bus_message_unref(reply);
    if (service_wait_state(bus, res, wait_active) != 0)
    {
        sd_bus_error_free(&err);
        sd_bus_unref(bus);
        return -1;
    }
    sd_bus_error_free(&err);
    sd_bus_unref(bus);
    return 0;
}

static int systemd_service_start_sync(const lcs_resource_config_t *res)
{
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
    {
        lcs_log_info("dry-run systemd start service %s unit=%s", res->name, res->systemd_unit);
        return 0;
    }
    return service_call_unit_method(res, "StartUnit", true);
}

static int systemd_service_stop_sync(const lcs_resource_config_t *res)
{
    if (systemd_test_stop_failed())
    {
        lcs_log_warn("forced systemd stop failure for service %s unit=%s", res->name, res->systemd_unit);
        return -1;
    }
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
    {
        lcs_log_info("dry-run systemd stop service %s unit=%s", res->name, res->systemd_unit);
        return 0;
    }
    sd_bus *bus = NULL;
    char *state = NULL;
    int rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        lcs_log_warn("systemd D-Bus connect failed for service %s unit=%s rc=%d",
                     res->name, res->systemd_unit, rc);
        return -1;
    }
    rc = service_get_active_state(bus, res, &state);
    if (rc == 0 &&
        (strcmp(state, "inactive") == 0 || strcmp(state, "failed") == 0))
    {
        lcs_log_info("systemd service %s unit=%s already stopped state=%s",
                     res->name, res->systemd_unit, state);
        free(state);
        sd_bus_unref(bus);
        return 0;
    }
    free(state);
    sd_bus_unref(bus);
    if (rc != 0)
        return -1;
    return service_call_unit_method(res, "StopUnit", false);
}

static int systemd_service_is_active_sync(const lcs_resource_config_t *res)
{
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
        return 1;
    sd_bus *bus = NULL;
    char *state = NULL;
    int rc = sd_bus_open_system(&bus);
    if (rc < 0)
    {
        lcs_log_warn("systemd D-Bus connect failed for service %s unit=%s rc=%d",
                     res->name, res->systemd_unit, rc);
        return -1;
    }

    rc = service_get_active_state(bus, res, &state);
    if (rc < 0)
    {
        sd_bus_unref(bus);
        return -1;
    }
    bool active = strcmp(state, "active") == 0;
    free(state);
    sd_bus_unref(bus);
    return active ? 1 : 0;
}

#else

static int systemd_service_start_sync(const lcs_resource_config_t *res)
{
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
        return 0;
    lcs_log_warn("cannot start service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

static int systemd_service_stop_sync(const lcs_resource_config_t *res)
{
    if (systemd_test_stop_failed())
        return -1;
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
        return 0;
    lcs_log_warn("cannot stop service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

static int systemd_service_is_active_sync(const lcs_resource_config_t *res)
{
    if (getenv("LCS_SYSTEMD_DRY_RUN"))
        return 1;
    lcs_log_warn("cannot inspect service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

#endif

typedef enum
{
    SYSTEMD_WORKER_START = 1,
    SYSTEMD_WORKER_STOP,
    SYSTEMD_WORKER_CHECK,
} systemd_worker_action_t;

static void systemd_worker_test_delay(void)
{
    const char *value = getenv("LCS_SYSTEMD_DELAY_MS");
    if (!value || !*value)
        return;
    char *end = NULL;
    unsigned long delay_ms = strtoul(value, &end, 10);
    if (!end || *end != '\0' || delay_ms > 60000ul)
        return;
    usleep((useconds_t)(delay_ms * 1000ul));
}

static int systemd_service_spawn_worker(const lcs_resource_config_t *res,
                                        systemd_worker_action_t action,
                                        pid_t *pid)
{
    if (!res || !pid)
        return -1;
    pid_t child = fork();
    if (child < 0)
        return -1;
    if (child == 0)
    {
        /* Do not keep daemon listeners or peer sockets alive if the parent
         * exits while this short-lived worker is waiting on systemd. */
        (void)close_range(3, ~0u, 0);
        systemd_worker_test_delay();
        int result = -1;
        if (action == SYSTEMD_WORKER_START)
            result = systemd_service_start_sync(res) == 0 ? 0 : -1;
        else if (action == SYSTEMD_WORKER_STOP)
            result = systemd_service_stop_sync(res) == 0 ? 0 : -1;
        else
            result = systemd_service_is_active_sync(res);

        if (action == SYSTEMD_WORKER_CHECK && result > 0)
            _exit(1);
        if (result == 0)
            _exit(0);
        _exit(2);
    }
    *pid = child;
    return 0;
}

int lcs_systemd_service_start_async(const lcs_resource_config_t *res,
                                    pid_t *pid)
{
    return systemd_service_spawn_worker(res, SYSTEMD_WORKER_START, pid);
}

int lcs_systemd_service_stop_async(const lcs_resource_config_t *res,
                                   pid_t *pid)
{
    return systemd_service_spawn_worker(res, SYSTEMD_WORKER_STOP, pid);
}

int lcs_systemd_service_check_async(const lcs_resource_config_t *res,
                                    pid_t *pid)
{
    return systemd_service_spawn_worker(res, SYSTEMD_WORKER_CHECK, pid);
}

int lcs_systemd_service_collect(pid_t pid, int *result)
{
    if (pid <= 0 || !result)
        return -1;
    int status = 0;
    pid_t rc;
    do
    {
        rc = waitpid(pid, &status, WNOHANG);
    } while (rc < 0 && errno == EINTR);
    if (rc == 0)
        return 0;
    if (rc < 0)
        return -1;
    if (!WIFEXITED(status))
    {
        *result = -1;
        return 1;
    }
    int code = WEXITSTATUS(status);
    *result = code == 0 ? 0 : code == 1 ? 1 : -1;
    return 1;
}

void lcs_systemd_service_cancel(pid_t pid)
{
    if (pid <= 0)
        return;
    (void)kill(pid, SIGKILL);
    while (waitpid(pid, NULL, 0) < 0 && errno == EINTR)
    {
    }
}
