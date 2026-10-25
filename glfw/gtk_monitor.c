//========================================================================
// GLFW 3.4 GTK 4 / libadwaita - www.glfw.org
//------------------------------------------------------------------------
// Monitors, from gdk_display_get_monitors(). Video modes and gamma are not
// available under Wayland and are stubbed.
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

#include <stdlib.h>
#include <string.h>

static _GLFWmonitor *
find_monitor(GdkMonitor *m) {
    for (int i = 0; i < _glfw.monitorCount; i++) {
        if (_glfw.monitors[i]->gtk.monitor == m) return _glfw.monitors[i];
    }
    return NULL;
}

static void
update_monitor(_GLFWmonitor *monitor) {
    GdkMonitor *m = monitor->gtk.monitor;
    GdkRectangle r;
    gdk_monitor_get_geometry(m, &r);
    monitor->gtk.x = r.x;
    monitor->gtk.y = r.y;
    monitor->gtk.width = r.width;
    monitor->gtk.height = r.height;
    monitor->gtk.scale = gdk_monitor_get_scale(m);
    if (monitor->gtk.scale <= 0) monitor->gtk.scale = gdk_monitor_get_scale_factor(m);
    monitor->widthMM = gdk_monitor_get_width_mm(m);
    monitor->heightMM = gdk_monitor_get_height_mm(m);
}

void
_glfwPollMonitorsGtk(void) {
    GListModel *model = gdk_display_get_monitors(_glfw.gtk.display);
    if (!model) return;
    guint n = g_list_model_get_n_items(model);
    // Remove monitors that are gone
    for (int i = _glfw.monitorCount - 1; i >= 0; i--) {
        bool found = false;
        for (guint k = 0; k < n && !found; k++) {
            GdkMonitor *m = g_list_model_get_item(model, k);
            found = m == _glfw.monitors[i]->gtk.monitor;
            g_object_unref(m);
        }
        if (!found) _glfwInputMonitor(_glfw.monitors[i], GLFW_DISCONNECTED, 0);
    }
    for (guint k = 0; k < n; k++) {
        GdkMonitor *m = g_list_model_get_item(model, k);
        _GLFWmonitor *monitor = find_monitor(m);
        if (!monitor) {
            const char *name = gdk_monitor_get_connector(m);
            monitor = _glfwAllocMonitor(name ? name : "unknown", gdk_monitor_get_width_mm(m), gdk_monitor_get_height_mm(m));
            const char *desc = gdk_monitor_get_description(m);
            if (desc) monitor->description = _glfw_strdup(desc);
            monitor->gtk.monitor = g_object_ref(m);
            update_monitor(monitor);
            _glfwInputMonitor(monitor, GLFW_CONNECTED, _GLFW_INSERT_LAST);
        } else update_monitor(monitor);
        g_object_unref(m);
    }
}

static void
on_monitors_changed(GListModel *model UNUSED, guint position UNUSED, guint removed UNUSED, guint added UNUSED, gpointer data UNUSED) {
    _glfwPollMonitorsGtk();
}

//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

void
_glfwPlatformFreeMonitor(_GLFWmonitor *monitor) {
    if (monitor->gtk.monitor) g_object_unref(monitor->gtk.monitor);
    monitor->gtk.monitor = NULL;
}

void
_glfwPlatformGetMonitorPos(_GLFWmonitor *monitor, int *xpos, int *ypos) {
    if (xpos) *xpos = monitor->gtk.x;
    if (ypos) *ypos = monitor->gtk.y;
}

void
_glfwPlatformGetMonitorContentScale(_GLFWmonitor *monitor, float *xscale, float *yscale) {
    float s = monitor->gtk.scale > 0 ? (float)monitor->gtk.scale : 1.f;
    if (xscale) *xscale = s;
    if (yscale) *yscale = s;
}

void
_glfwPlatformGetMonitorWorkarea(_GLFWmonitor *monitor, int *xpos, int *ypos, int *width, int *height) {
    if (xpos) *xpos = monitor->gtk.x;
    if (ypos) *ypos = monitor->gtk.y;
    if (width) *width = monitor->gtk.width;
    if (height) *height = monitor->gtk.height;
}

MonitorGeometry
_glfwPlatformGetMonitorGeometry(_GLFWmonitor *monitor) {
    MonitorGeometry ans = {0};
    ans.full.x = ans.workarea.x = monitor->gtk.x;
    ans.full.y = ans.workarea.y = monitor->gtk.y;
    ans.full.width = ans.workarea.width = monitor->gtk.width;
    ans.full.height = ans.workarea.height = monitor->gtk.height;
    return ans;
}

static GLFWvidmode
current_mode(_GLFWmonitor *monitor) {
    GLFWvidmode mode = {0};
    double scale = monitor->gtk.scale > 0 ? monitor->gtk.scale : 1;
    mode.width = (int)(monitor->gtk.width * scale + 0.5);
    mode.height = (int)(monitor->gtk.height * scale + 0.5);
    mode.redBits = mode.greenBits = mode.blueBits = 8;
    int rr = monitor->gtk.monitor ? gdk_monitor_get_refresh_rate(monitor->gtk.monitor) : 0;
    mode.refreshRate = rr > 0 ? (rr + 500) / 1000 : 60;
    return mode;
}

GLFWvidmode *
_glfwPlatformGetVideoModes(_GLFWmonitor *monitor, int *found) {
    GLFWvidmode *mode = calloc(1, sizeof(GLFWvidmode));
    *mode = current_mode(monitor);
    *found = 1;
    return mode;
}

bool
_glfwPlatformGetVideoMode(_GLFWmonitor *monitor, GLFWvidmode *mode) {
    *mode = current_mode(monitor);
    return true;
}

bool
_glfwPlatformGetGammaRamp(_GLFWmonitor *monitor UNUSED, GLFWgammaramp *ramp UNUSED) {
    _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "GTK: Gamma ramp access is not available");
    return false;
}

void
_glfwPlatformSetGammaRamp(_GLFWmonitor *monitor UNUSED, const GLFWgammaramp *ramp UNUSED) {
    _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "GTK: Gamma ramp access is not available");
}

void
_glfwGtkWatchMonitors(void) {
    _glfw.gtk.monitors_model = gdk_display_get_monitors(_glfw.gtk.display);
    if (_glfw.gtk.monitors_model)
        _glfw.gtk.monitors_handler = g_signal_connect(_glfw.gtk.monitors_model, "items-changed", G_CALLBACK(on_monitors_changed), NULL);
}

//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

// The scale of the monitor a new window is most likely to open on. GTK has
// no notion of a "current" monitor before a window is mapped, so use the
// monitor of the focused window, or else the first one.
GLFWAPI double
glfwGetWaylandCurrentMonitorFractionalScale(void) {
    _GLFW_REQUIRE_INIT_OR_RETURN(1.0);
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (w->gtk.focused && w->gtk.surface) {
            GdkMonitor *m = gdk_display_get_monitor_at_surface(_glfw.gtk.display, w->gtk.surface);
            if (m) return gdk_monitor_get_scale(m);
        }
    }
    if (_glfw.monitorCount > 0 && _glfw.monitors[0]->gtk.scale > 0) return _glfw.monitors[0]->gtk.scale;
    return 1.0;
}
