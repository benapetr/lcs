// SPDX-License-Identifier: GPL-3.0-or-later
// Copyright (C) 2026 Petr Bena <petr@bena.rocks>

#include "systemd_service.h"

#include "log.h"

#include <stdlib.h>
#include <string.h>

#ifdef HAVE_SYSTEMD
#include <systemd/sd-bus.h>
#include <unistd.h>

static int service_get_active_state(sd_bus *bus, const lcs_vip_config_t *res, char **state)
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

static int service_wait_state(sd_bus *bus, const lcs_vip_config_t *res, bool want_active)
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

static int service_call_unit_method(const lcs_vip_config_t *res, const char *method, bool wait_active)
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

int lcs_systemd_service_start(const lcs_vip_config_t *res)
{
    return service_call_unit_method(res, "StartUnit", true);
}

int lcs_systemd_service_stop(const lcs_vip_config_t *res)
{
    return service_call_unit_method(res, "StopUnit", false);
}

int lcs_systemd_service_is_active(const lcs_vip_config_t *res)
{
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

int lcs_systemd_service_start(const lcs_vip_config_t *res)
{
    lcs_log_warn("cannot start service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

int lcs_systemd_service_stop(const lcs_vip_config_t *res)
{
    lcs_log_warn("cannot stop service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

int lcs_systemd_service_is_active(const lcs_vip_config_t *res)
{
    lcs_log_warn("cannot inspect service %s unit=%s: built without systemd D-Bus support",
                 res->name, res->systemd_unit);
    return -1;
}

#endif
