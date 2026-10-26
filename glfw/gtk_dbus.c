//========================================================================
// GLFW 3.4 GTK 4 / libadwaita - www.glfw.org
//------------------------------------------------------------------------
// Desktop notifications and session state monitoring over GDBus. The other
// Linux backends do the same with libdbus on their own event loop, see
// linux_notify.c and linux_session_monitor.c, which this follows.
//
// This software is provided 'as-is', without any express or implied
// warranty. In no event will the authors be held liable for any damages
// arising from the use of this software.
//
// Permission is granted to anyone to use this software for any purpose,
// including commercial applications, and to alter it and redistribute it
// freely, subject to the following restrictions:
//
// 1. The origin of this software must not be misrepresented; you must not
//    claim that you wrote the original software. If you use this software
//    in a product, an acknowledgment in the product documentation would
//    be appreciated but is not required.
//
// 2. Altered source versions must be plainly marked as such, and must not
//    be misrepresented as being the original software.
//
// 3. This notice may not be removed or altered from any source
//    distribution.
//
//========================================================================

#include "internal.h"

#include <string.h>

#define NOTIFICATIONS_SERVICE "org.freedesktop.Notifications"
#define NOTIFICATIONS_PATH "/org/freedesktop/Notifications"
#define NOTIFICATIONS_IFACE "org.freedesktop.Notifications"

#define PORTAL_SERVICE "org.freedesktop.portal.Desktop"
#define PORTAL_PATH "/org/freedesktop/portal/desktop"
#define INHIBIT_INTERFACE "org.freedesktop.portal.Inhibit"
#define REQUEST_INTERFACE "org.freedesktop.portal.Request"
// Object paths include the unique name of the connection, so a fixed token is
// unique, as there is only one monitor per connection
#define HANDLE_TOKEN "glfw_session_monitor"

// The session states reported by the Inhibit desktop portal, as in
// linux_session_monitor.h
#define SESSION_STATE_RUNNING 1
#define SESSION_STATE_QUERY_END 2

static struct {
    GDBusConnection *bus;
    // Cancels the calls in flight at termination, so their callbacks do not run
    GCancellable *cancellable;
} dbus;

static GDBusConnection *
session_bus(void) {
    if (!dbus.bus) {
        GError *err = NULL;
        dbus.bus = g_bus_get_sync(G_BUS_TYPE_SESSION, NULL, &err);
        if (!dbus.bus) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Failed to connect to the DBUS session bus: %s", err->message);
            g_error_free(err);
            return NULL;
        }
        dbus.cancellable = g_cancellable_new();
    }
    return dbus.bus;
}

// Notifications {{{

static GLFWDBusnotificationactivatedfun activated_handler = NULL;
static guint notification_signals = 0;

GLFWAPI void
glfwDBusSetUserNotificationHandler(GLFWDBusnotificationactivatedfun handler) {
    activated_handler = handler;
}

static void
report_notification_event(uint32_t id, int type, const char *data) {
    if (!activated_handler) return;
    _glfwGtkRestoreContext();
    activated_handler(id, type, data);
}

static void
on_notification_signal(
    GDBusConnection *conn UNUSED, const char *sender UNUSED, const char *path UNUSED, const char *iface UNUSED, const char *signal,
    GVariant *params, gpointer data UNUSED) {
    uint32_t id;
    const char *arg;
    if (strcmp(signal, "ActionInvoked") == 0 && g_variant_is_of_type(params, G_VARIANT_TYPE("(us)"))) {
        g_variant_get(params, "(u&s)", &id, &arg);
        report_notification_event(id, 2, arg);
    } else if (strcmp(signal, "ActivationToken") == 0 && g_variant_is_of_type(params, G_VARIANT_TYPE("(us)"))) {
        g_variant_get(params, "(u&s)", &id, &arg);
        report_notification_event(id, 1, arg);
    } else if (strcmp(signal, "NotificationClosed") == 0 && g_variant_is_of_type(params, G_VARIANT_TYPE("(uu)"))) {
        g_variant_get(params, "(uu)", &id, NULL);
        report_notification_event(id, 0, "");
    }
}

static void
got_capabilities(GObject *source, GAsyncResult *res, gpointer data UNUSED) {
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (!reply) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            _glfwInputError(GLFW_PLATFORM_ERROR, "Notify: Failed to get server capabilities error: %s", err->message);
        g_error_free(err);
        return;
    }
    // The capabilities, one per line
    GString *caps = g_string_new(NULL);
    GVariantIter *iter;
    const char *cap;
    g_variant_get(reply, "(as)", &iter);
    while (g_variant_iter_next(iter, "&s", &cap)) {
        if (cap[0]) g_string_append_printf(caps, "%s\n", cap);
    }
    g_variant_iter_free(iter);
    g_variant_unref(reply);
    report_notification_event(0, -1, caps->str);
    g_string_free(caps, TRUE);
}

typedef struct {
    unsigned long long id;
    GLFWDBusnotificationcreatedfun callback;
    void *data;
} NotificationCreatedData;

static void
notification_created(GObject *source, GAsyncResult *res, gpointer user_data) {
    NotificationCreatedData *ncd = user_data;
    GError *err = NULL;
    GVariant *reply = g_dbus_connection_call_finish(G_DBUS_CONNECTION(source), res, &err);
    if (!reply) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED))
            _glfwInputError(GLFW_PLATFORM_ERROR, "Notify: Failed to create notification error: %s", err->message);
        g_error_free(err);
    } else {
        uint32_t id;
        g_variant_get(reply, "(u)", &id);
        g_variant_unref(reply);
        if (ncd->callback) {
            _glfwGtkRestoreContext();
            ncd->callback(ncd->id, id, ncd->data);
        }
    }
    g_free(ncd);
}

// Like the other backends, a timeout of -9999 with urgency 255 closes the
// notification whose id data points to, and a timeout of -99999 with urgency
// 255 reports the server capabilities to the activated handler
GLFWAPI unsigned long long
glfwDBusUserNotify(const GLFWDBUSNotificationData *n, GLFWDBusnotificationcreatedfun callback, void *data) {
    GDBusConnection *bus = session_bus();
    if (!bus) return 0;
    if (n->timeout == -9999 && n->urgency == 255) {
        g_dbus_connection_call(
            bus, NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE, "CloseNotification", g_variant_new("(u)", *(uint32_t *)data),
            NULL, G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
        return 1;
    }
    if (n->timeout == -99999 && n->urgency == 255) {
        g_dbus_connection_call(
            bus, NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE, "GetCapabilities", NULL, G_VARIANT_TYPE("(as)"),
            G_DBUS_CALL_FLAGS_NONE, 1000, dbus.cancellable, got_capabilities, NULL);
        return 1;
    }
    if (!notification_signals) {
        notification_signals = g_dbus_connection_signal_subscribe(
            bus, NULL, NOTIFICATIONS_IFACE, NULL, NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_notification_signal, NULL, NULL);
    }
    static unsigned long long notification_id = 0;
    NotificationCreatedData *ncd = g_new(NotificationCreatedData, 1);
    ncd->id = ++notification_id;
    if (!ncd->id) ncd->id = ++notification_id;
    ncd->callback = callback;
    ncd->data = data;

    GVariantBuilder actions, hints;
    g_variant_builder_init(&actions, G_VARIANT_TYPE("as"));
    for (size_t i = 0; n->actions && i < n->num_actions; i++) g_variant_builder_add(&actions, "s", n->actions[i]);
    g_variant_builder_init(&hints, G_VARIANT_TYPE("a{sv}"));
    g_variant_builder_add(&hints, "{sv}", "urgency", g_variant_new_byte(n->urgency));
    if (n->category && n->category[0]) g_variant_builder_add(&hints, "{sv}", "category", g_variant_new_string(n->category));
    if (n->muted) g_variant_builder_add(&hints, "{sv}", "suppress-sound", g_variant_new_boolean(TRUE));
#define S(x) ((x) ? (x) : "")
    GVariant *params = g_variant_new(
        "(susssasa{sv}i)", S(n->app_name), n->replaces, S(n->icon), S(n->summary), S(n->body), &actions, &hints, n->timeout);
#undef S
    g_dbus_connection_call(
        bus, NOTIFICATIONS_SERVICE, NOTIFICATIONS_PATH, NOTIFICATIONS_IFACE, "Notify", params, G_VARIANT_TYPE("(u)"),
        G_DBUS_CALL_FLAGS_NONE, 5000, dbus.cancellable, notification_created, ncd);
    return ncd->id;
}

// }}}

// Session state {{{

// Monitors the state of the desktop session using the Inhibit desktop portal,
// so that the application can save its state before the user is logged out,
// see linux_session_monitor.c for details. Signals are accepted only from the
// portal that replied to CreateMonitor and only for the monitor session
// created by it, so that other clients on the session bus cannot fake a
// session ending.

static struct {
    GLFWDBussessionstatefun callback;
    uint32_t current_state;
    bool monitoring;
    // Whether the portal reported that the monitor was created
    bool established;
    // The unique name of the portal that replied to CreateMonitor, NULL until it replies
    char *portal_name;
    char *request_path, *session_path;
    guint response_signal, state_signal, owner_signal;
} session = {.current_state = SESSION_STATE_RUNNING};

static char *
portal_object_path(GDBusConnection *bus, const char *kind) {
    // The portal creates objects at PORTAL_PATH/kind/SENDER/TOKEN where SENDER
    // is the unique name of the connection without the leading colon and with
    // dots replaced by underscores
    const char *unique_name = g_dbus_connection_get_unique_name(bus);
    if (!unique_name || unique_name[0] != ':') return NULL;
    char *sender = g_strdup(unique_name + 1);
    g_strdelimit(sender, ".", '_');
    char *ans = g_strdup_printf(PORTAL_PATH "/%s/%s/" HANDLE_TOKEN, kind, sender);
    g_free(sender);
    return ans;
}

static bool
is_from_portal(const char *sender) {
    return session.monitoring && session.portal_name && sender && strcmp(sender, session.portal_name) == 0;
}

static void
on_state_changed(
    GDBusConnection *bus, const char *sender, const char *path UNUSED, const char *iface UNUSED, const char *signal UNUSED, GVariant *params,
    gpointer data UNUSED) {
    if (!is_from_portal(sender)) return;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(oa{sv})"))) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: StateChanged signal does not have a session handle and state dictionary");
        return;
    }
    const char *session_handle;
    GVariant *state_dict;
    g_variant_get(params, "(&o@a{sv})", &session_handle, &state_dict);
    uint32_t state;
    const bool has_state = g_variant_lookup(state_dict, "session-state", "u", &state);
    g_variant_unref(state_dict);
    if (strcmp(session_handle, session.session_path) != 0 || !has_state) return;
    // The signal is also sent when only the screensaver state changes
    const bool changed = state != session.current_state;
    session.current_state = state;
    if (changed && session.callback) {
        _glfwGtkRestoreContext();
        session.callback(state);
    }
    if (state == SESSION_STATE_QUERY_END) {
        g_dbus_connection_call(
            bus, PORTAL_SERVICE, PORTAL_PATH, INHIBIT_INTERFACE, "QueryEndResponse", g_variant_new("(o)", session_handle), NULL,
            G_DBUS_CALL_FLAGS_NONE, -1, NULL, NULL, NULL);
    }
}

static void
on_monitor_response(
    GDBusConnection *bus UNUSED, const char *sender, const char *path, const char *iface UNUSED, const char *signal UNUSED,
    GVariant *params, gpointer data UNUSED) {
    if (!is_from_portal(sender) || strcmp(path, session.request_path) != 0) return;
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(ua{sv})"))) {
        session.monitoring = false;
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: The response to CreateMonitor does not have a response code");
        return;
    }
    uint32_t response;
    GVariant *results;
    g_variant_get(params, "(u@a{sv})", &response, &results);
    if (response != 0) {
        session.monitoring = false;
        _glfwInputError(
            GLFW_PLATFORM_ERROR,
            "Inhibit portal: Failed to monitor the session state, make sure your desktop portal supports version 3 of the Inhibit "
            "portal. Response code: %u",
            response);
    } else {
        session.established = true;
        // Use the session handle from the results, in case the portal ignored session_handle_token
        GVariant *handle = g_variant_lookup_value(results, "session_handle", NULL);
        if (handle && (g_variant_is_of_type(handle, G_VARIANT_TYPE_STRING) || g_variant_is_of_type(handle, G_VARIANT_TYPE_OBJECT_PATH))) {
            g_free(session.session_path);
            session.session_path = g_variant_dup_string(handle, NULL);
        }
        if (handle) g_variant_unref(handle);
    }
    g_variant_unref(results);
}

static void
watch_response(GDBusConnection *bus) {
    if (session.response_signal) g_dbus_connection_signal_unsubscribe(bus, session.response_signal);
    session.response_signal = g_dbus_connection_signal_subscribe(
        bus, NULL, REQUEST_INTERFACE, "Response", session.request_path, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_monitor_response, NULL, NULL);
}

static void
monitor_created(GObject *source, GAsyncResult *res, gpointer data UNUSED) {
    GError *err = NULL;
    GDBusMessage *reply = g_dbus_connection_send_message_with_reply_finish(G_DBUS_CONNECTION(source), res, &err);
    if (reply && g_dbus_message_to_gerror(reply, &err)) g_clear_object(&reply);
    if (!reply) {
        if (!g_error_matches(err, G_IO_ERROR, G_IO_ERROR_CANCELLED)) {
            session.monitoring = false;
            _glfwInputError(
                GLFW_PLATFORM_ERROR,
                "Inhibit portal: Failed to monitor the session state, make sure you have the desktop portal running. Error: %s", err->message);
        }
        g_error_free(err);
        return;
    }
    GVariant *body = g_dbus_message_get_body(reply);
    const char *sender = g_dbus_message_get_sender(reply);
    if (!sender || !body || !g_variant_is_of_type(body, G_VARIANT_TYPE("(o)"))) {
        session.monitoring = false;
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Invalid reply to CreateMonitor");
        g_object_unref(reply);
        return;
    }
    g_free(session.portal_name);
    session.portal_name = g_strdup(sender);
    const char *handle;
    g_variant_get(body, "(&o)", &handle);
    if (strcmp(handle, session.request_path) != 0) {
        // Portals that ignore handle_token use a different request path, its
        // Response signal could be missed if it was sent before this
        g_free(session.request_path);
        session.request_path = g_strdup(handle);
        watch_response(G_DBUS_CONNECTION(source));
    }
    g_object_unref(reply);
}

static bool start_monitoring(GDBusConnection *bus);

static void
on_name_owner_changed(
    GDBusConnection *bus, const char *sender UNUSED, const char *path UNUSED, const char *iface UNUSED, const char *signal UNUSED,
    GVariant *params, gpointer data UNUSED) {
    if (!g_variant_is_of_type(params, G_VARIANT_TYPE("(sss)"))) return;
    const char *name, *old_owner;
    g_variant_get(params, "(&s&s&s)", &name, &old_owner, NULL);
    if (strcmp(name, PORTAL_SERVICE) != 0 || !session.portal_name || strcmp(old_owner, session.portal_name) != 0) return;
    // The portal that created the monitor has exited, which ends the monitor
    // session. It is created again only if it was created successfully, so
    // that a portal that crashes when creating it is not restarted in a loop
    // by DBUS activation.
    const bool restart = session.monitoring && session.established;
    session.monitoring = false;
    session.established = false;
    g_clear_pointer(&session.portal_name, g_free);
    if (restart) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: The desktop portal exited, monitoring the session state again");
        start_monitoring(bus);
    }
}

static bool
start_monitoring(GDBusConnection *bus) {
    g_free(session.request_path);
    g_free(session.session_path);
    session.request_path = portal_object_path(bus, "request");
    session.session_path = portal_object_path(bus, "session");
    if (!session.request_path || !session.session_path) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "Inhibit portal: Failed to get the unique name of the DBUS connection");
        return false;
    }
    // Subscribed before calling CreateMonitor, so that the Response signal cannot be missed
    watch_response(bus);
    if (!session.state_signal) {
        session.state_signal = g_dbus_connection_signal_subscribe(
            bus, NULL, INHIBIT_INTERFACE, "StateChanged", NULL, NULL, G_DBUS_SIGNAL_FLAGS_NONE, on_state_changed, NULL, NULL);
        session.owner_signal = g_dbus_connection_signal_subscribe(
            bus, "org.freedesktop.DBus", "org.freedesktop.DBus", "NameOwnerChanged", "/org/freedesktop/DBus", PORTAL_SERVICE,
            G_DBUS_SIGNAL_FLAGS_NONE, on_name_owner_changed, NULL, NULL);
    }
    g_clear_pointer(&session.portal_name, g_free);
    session.established = false;
    // Reset without running the callback, so that state the application saved
    // for a logout that was in progress when the portal exited is not discarded
    session.current_state = SESSION_STATE_RUNNING;

    GDBusMessage *msg = g_dbus_message_new_method_call(PORTAL_SERVICE, PORTAL_PATH, INHIBIT_INTERFACE, "CreateMonitor");
    GVariantBuilder options;
    g_variant_builder_init(&options, G_VARIANT_TYPE("a{sv}"));
    // Some versions of xdg-desktop-portal abort if the session handle token is missing
    g_variant_builder_add(&options, "{sv}", "handle_token", g_variant_new_string(HANDLE_TOKEN));
    g_variant_builder_add(&options, "{sv}", "session_handle_token", g_variant_new_string(HANDLE_TOKEN));
    g_dbus_message_set_body(msg, g_variant_new("(sa{sv})", "", &options));
    g_dbus_connection_send_message_with_reply(bus, msg, G_DBUS_SEND_MESSAGE_FLAGS_NONE, -1, NULL, dbus.cancellable, monitor_created, NULL);
    g_object_unref(msg);
    session.monitoring = true;
    return true;
}

GLFWAPI bool
glfwDBusMonitorSessionState(GLFWDBussessionstatefun callback) {
    session.callback = callback;
    if (session.monitoring) return true;
    GDBusConnection *bus = session_bus();
    return bus && start_monitoring(bus);
}

// }}}

void
_glfwGtkDBusTerminate(void) {
    if (dbus.cancellable) g_cancellable_cancel(dbus.cancellable);
    g_clear_object(&dbus.cancellable);
    if (dbus.bus) {
        guint *signals[] = {&notification_signals, &session.response_signal, &session.state_signal, &session.owner_signal};
        for (size_t i = 0; i < sizeof(signals) / sizeof(signals[0]); i++) {
            if (*signals[i]) g_dbus_connection_signal_unsubscribe(dbus.bus, *signals[i]);
            *signals[i] = 0;
        }
    }
    g_clear_object(&dbus.bus);
    g_clear_pointer(&session.portal_name, g_free);
    g_clear_pointer(&session.request_path, g_free);
    g_clear_pointer(&session.session_path, g_free);
    session.monitoring = session.established = false;
    session.current_state = SESSION_STATE_RUNNING;
    session.callback = NULL;
    activated_handler = NULL;
}
