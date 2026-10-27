//========================================================================
// GLFW 3.4 GTK 4 / libadwaita - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2016 Google Inc.
// Copyright (c) 2016-2019 Camilla Löwy <elmindreda@glfw.org>
// Copyright (c) 2026 Tom Dryer <tomdryer.com@gmail.com>
//
// Windows: an AdwWindow with an AdwToolbarView whose top bar is an
// AdwHeaderBar and whose content is a KittyView, a small widget that shows
// the texture kitty last rendered and receives input.
//
// Started from the null backend (null_*.c), whose stubs remain in places.
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

#include <errno.h>
#include <math.h>
#include <poll.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <gio/gunixinputstream.h>

#define debug debug_rendering

// KittyView {{{

#define KITTY_TYPE_VIEW (kitty_view_get_type())
G_DECLARE_FINAL_TYPE(KittyView, kitty_view, KITTY, VIEW, GtkWidget)

struct _KittyView {
    GtkWidget parent_instance;
    _GLFWwindow *window;
    GdkTexture *texture;
    double texture_scale;
    int natural_width, natural_height;
    GdkRGBA background;
};

G_DEFINE_FINAL_TYPE(KittyView, kitty_view, GTK_TYPE_WIDGET)

static void update_size(_GLFWwindow *window, bool force);

static void
kitty_view_snapshot(GtkWidget *widget, GtkSnapshot *snapshot) {
    KittyView *self = KITTY_VIEW(widget);
    const float width = (float)gtk_widget_get_width(widget), height = (float)gtk_widget_get_height(widget);
    float tw = 0, th = 0;
    if (self->texture) {
        const double scale = self->texture_scale > 0 ? self->texture_scale : 1;
        tw = (float)(gdk_texture_get_width(self->texture) / scale);
        th = (float)(gdk_texture_get_height(self->texture) / scale);
        gtk_snapshot_save(snapshot);
        // With fractional scales the widget can start in the middle of a
        // device pixel, for example because the header bar is 47 logical
        // pixels high, and GSK then resamples the texture, blurring the text.
        // Shift the texture so it starts on a device pixel.
        GtkNative *native = gtk_widget_get_native(widget);
        graphene_point_t origin;
        if (native && gtk_widget_compute_point(widget, GTK_WIDGET(native), &GRAPHENE_POINT_INIT(0, 0), &origin)) {
            double sx = 0, sy = 0;
            gtk_native_get_surface_transform(native, &sx, &sy);
            const double dx = (origin.x + sx) * scale, dy = (origin.y + sy) * scale;
            const float ox = (float)((dx - floor(dx)) / scale), oy = (float)((dy - floor(dy)) / scale);
            if (ox > 0 || oy > 0) gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(-ox, -oy));
        }
        // GL textures have their origin at the bottom left
        gtk_snapshot_translate(snapshot, &GRAPHENE_POINT_INIT(0, th));
        gtk_snapshot_scale(snapshot, 1, -1);
        // The texture maps 1:1 to device pixels so no filtering happens. Do
        // not use gtk_snapshot_append_scaled_texture() with
        // GSK_SCALING_FILTER_NEAREST here, GSK then resamples the texture,
        // shifting and blurring it.
        gtk_snapshot_append_texture(snapshot, self->texture, &GRAPHENE_RECT_INIT(0, 0, tw, th));
        gtk_snapshot_restore(snapshot);
    }
    // Fill whatever the texture does not cover, for example while kitty
    // has not yet rendered at a new size
    if (tw < width) gtk_snapshot_append_color(snapshot, &self->background, &GRAPHENE_RECT_INIT(tw, 0, width - tw, height));
    if (th < height) gtk_snapshot_append_color(snapshot, &self->background, &GRAPHENE_RECT_INIT(0, th, MIN(tw, width), height - th));
}

static void
kitty_view_measure(
    GtkWidget *widget, GtkOrientation orientation, int for_size UNUSED, int *minimum, int *natural, int *minimum_baseline, int *natural_baseline) {
    KittyView *self = KITTY_VIEW(widget);
    *minimum = 1;
    *natural = orientation == GTK_ORIENTATION_HORIZONTAL ? self->natural_width : self->natural_height;
    if (*natural < 1) *natural = 1;
    *minimum_baseline = *natural_baseline = -1;
}

static void
kitty_view_size_allocate(GtkWidget *widget, int width UNUSED, int height UNUSED, int baseline UNUSED) {
    KittyView *self = KITTY_VIEW(widget);
    if (self->window) update_size(self->window, false);
}

static void
kitty_view_dispose(GObject *object) {
    KittyView *self = KITTY_VIEW(object);
    g_clear_object(&self->texture);
    self->window = NULL;
    G_OBJECT_CLASS(kitty_view_parent_class)->dispose(object);
}

static void
kitty_view_class_init(KittyViewClass *klass) {
    GtkWidgetClass *widget_class = GTK_WIDGET_CLASS(klass);
    widget_class->snapshot = kitty_view_snapshot;
    widget_class->measure = kitty_view_measure;
    widget_class->size_allocate = kitty_view_size_allocate;
    G_OBJECT_CLASS(klass)->dispose = kitty_view_dispose;
    gtk_widget_class_set_css_name(widget_class, "kittyview");
}

static void
kitty_view_init(KittyView *self) {
    gtk_widget_set_focusable(GTK_WIDGET(self), TRUE);
    gtk_widget_set_hexpand(GTK_WIDGET(self), TRUE);
    gtk_widget_set_vexpand(GTK_WIDGET(self), TRUE);
    self->background = (GdkRGBA){0, 0, 0, 1};
}

void
_glfwGtkPresentTexture(_GLFWwindow *window, GdkTexture *texture) {
    if (!window->gtk.view) return;
    KittyView *view = KITTY_VIEW(window->gtk.view);
    g_set_object(&view->texture, texture);
    view->texture_scale = window->gtk.scale;
    gtk_widget_queue_draw(window->gtk.view);
}
// }}}

static _GLFWwindow *
live_window(gpointer data) {
    // Signal handlers get the _GLFWwindow, make sure it still exists
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (w == data) return w;
    }
    return NULL;
}

static double
surface_scale(_GLFWwindow *window) {
    if (window->gtk.surface) {
        double s = gdk_surface_get_scale(window->gtk.surface);
        if (s > 0) return s;
    }
    return window->gtk.scale > 0 ? window->gtk.scale : 1;
}

static void
update_swaps_allowed(_GLFWwindow *window) {
    window->swaps_disallowed = !(window->gtk.mapped && window->gtk.width > 0 && window->gtk.height > 0);
}

static void
update_size(_GLFWwindow *window, bool force) {
    if (!window->gtk.view) return;
    int width = gtk_widget_get_width(window->gtk.view), height = gtk_widget_get_height(window->gtk.view);
    double scale = surface_scale(window);
    if (width < 1 || height < 1) return;
    bool size_changed = width != window->gtk.width || height != window->gtk.height;
    bool scale_changed = scale != window->gtk.scale;
    if (!size_changed && !scale_changed && !force) return;
    int old_fw, old_fh;
    _glfwPlatformGetFramebufferSize(window, &old_fw, &old_fh);
    window->gtk.width = width;
    window->gtk.height = height;
    window->gtk.scale = scale;
    int fw, fh;
    _glfwPlatformGetFramebufferSize(window, &fw, &fh);
    debug("GTK: window %llu size: %dx%d scale: %.3f framebuffer: %dx%d\n", window->id, width, height, scale, fw, fh);
    update_swaps_allowed(window);
    _glfwGtkRestoreContext();
    if (scale_changed) _glfwInputWindowContentScale(window, (float)scale, (float)scale);
    if (size_changed || force) _glfwInputWindowSize(window, width, height);
    if (fw != old_fw || fh != old_fh || force) _glfwInputFramebufferSize(window, fw, fh);
    _glfwInputWindowDamage(window);
}

static void
on_scale_changed(GObject *obj UNUSED, GParamSpec *pspec UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (window) update_size(window, false);
}

static void
on_view_realize(GtkWidget *widget, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    GtkNative *native = gtk_widget_get_native(widget);
    GdkSurface *surface = native ? gtk_native_get_surface(native) : NULL;
    if (surface && surface != window->gtk.surface) {
        if (window->gtk.surface && window->gtk.scale_handler) g_signal_handler_disconnect(window->gtk.surface, window->gtk.scale_handler);
        window->gtk.surface = surface;
        window->gtk.scale_handler = g_signal_connect(surface, "notify::scale", G_CALLBACK(on_scale_changed), window);
        window->gtk.scale = gdk_surface_get_scale(surface);
    }
    if (_glfw.hints.init.debugRendering) {
        GskRenderer *r = native ? gtk_native_get_renderer(native) : NULL;
        debug("GTK: window %llu realized, GSK renderer: %s\n", window->id, r ? G_OBJECT_TYPE_NAME(r) : "none");
    }
}

static void
on_view_map(GtkWidget *widget, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    window->gtk.mapped = true;
    window->gtk.fully_created = true;
    gtk_widget_grab_focus(widget);
    update_size(window, false);
    update_swaps_allowed(window);
    // kitty only renders on a tick, make sure one happens now that it can
    _glfwPlatformPostEmptyEvent();
}

static void
on_view_unmap(GtkWidget *widget UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    window->gtk.mapped = false;
    update_swaps_allowed(window);
}

static gboolean
on_close_request(GtkWindow *w UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (window) {
        _glfwGtkRestoreContext();
        _glfwInputWindowCloseRequest(window);
    }
    // kitty decides whether and when to destroy the window
    return TRUE;
}

static void
on_is_active(GObject *obj, GParamSpec *pspec UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    bool focused = gtk_window_is_active(GTK_WINDOW(obj));
    if (focused == window->gtk.focused) return;
    window->gtk.focused = focused;
    _glfwGtkRestoreContext();
    _glfwInputWindowFocus(window, focused);
}

static void
on_maximized(GObject *obj, GParamSpec *pspec UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    bool maximized = gtk_window_is_maximized(GTK_WINDOW(obj));
    if (maximized == window->gtk.maximized) return;
    window->gtk.maximized = maximized;
    _glfwGtkRestoreContext();
    _glfwInputWindowMaximize(window, maximized);
}

static void
update_top_bar(_GLFWwindow *window) {
    // hide_window_decorations=yes means no CSD shadow and rounded corners either
    gtk_window_set_decorated(GTK_WINDOW(window->gtk.window), window->decorated);
    // Fullscreen windows have no header bar, like other GNOME apps
    bool reveal = window->decorated && !window->gtk.titlebar_hidden && !window->gtk.fullscreen;
    adw_toolbar_view_set_reveal_top_bars(ADW_TOOLBAR_VIEW(window->gtk.toolbar_view), reveal);
}

static void
on_fullscreened(GObject *obj, GParamSpec *pspec UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return;
    window->gtk.fullscreen = gtk_window_is_fullscreen(GTK_WINDOW(obj));
    update_top_bar(window);
}

// Input {{{

static bool
input_blocked(void) {
    // While kitty is blocked in a nested loop, for example reading the
    // clipboard, do not call into it.
    return _glfw.gtk.nested_loop_depth > 0;
}

static gboolean
on_key_pressed(GtkEventControllerKey *controller, guint keyval UNUSED, guint keycode UNUSED, GdkModifierType state UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return TRUE;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (!event) return FALSE;
    _glfwGtkRestoreContext();
    _glfwGtkHandleKeyEvent(window, event, true);
    // kitty handles every key, GTK must not do focus navigation, mnemonics, F10...
    return TRUE;
}

static void
on_key_released(GtkEventControllerKey *controller, guint keyval UNUSED, guint keycode UNUSED, GdkModifierType state UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return;
    GdkEvent *event = gtk_event_controller_get_current_event(GTK_EVENT_CONTROLLER(controller));
    if (!event) return;
    _glfwGtkRestoreContext();
    _glfwGtkHandleKeyEvent(window, event, false);
}

static int
glfw_button(guint button) {
    switch (button) {
        case GDK_BUTTON_PRIMARY: return GLFW_MOUSE_BUTTON_LEFT;
        case GDK_BUTTON_MIDDLE: return GLFW_MOUSE_BUTTON_MIDDLE;
        case GDK_BUTTON_SECONDARY: return GLFW_MOUSE_BUTTON_RIGHT;
        case 8: return GLFW_MOUSE_BUTTON_4;
        case 9: return GLFW_MOUSE_BUTTON_5;
        default: return button >= 10 && button - 10 + GLFW_MOUSE_BUTTON_6 <= GLFW_MOUSE_BUTTON_LAST ? (int)(button - 10 + GLFW_MOUSE_BUTTON_6) : -1;
    }
}

// GtkGestureClick gives up on a press once the pointer moves past the drag
// threshold and then never reports the release, which breaks selecting
// text. So buttons come from the raw events.
static gboolean
on_legacy_event(GtkEventControllerLegacy *controller UNUSED, GdkEvent *event, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return FALSE;
    GdkEventType type = gdk_event_get_event_type(event);
    if (type != GDK_BUTTON_PRESS && type != GDK_BUTTON_RELEASE) return FALSE;
    if (input_blocked()) return TRUE;
    int button = glfw_button(gdk_button_event_get_button(event));
    if (button < 0) return FALSE;
    if (type == GDK_BUTTON_PRESS && !gtk_widget_has_focus(window->gtk.view)) gtk_widget_grab_focus(window->gtk.view);
    int mods = _glfwGtkModsFromState(gdk_event_get_modifier_state(event), gdk_event_get_device(event));
    double x, y;
    if (gdk_event_get_position(event, &x, &y)) {
        // Event positions are surface relative, kitty wants view relative
        graphene_point_t p;
        GtkNative *native = gtk_widget_get_native(window->gtk.view);
        double nx = 0, ny = 0;
        if (native) gtk_native_get_surface_transform(native, &nx, &ny);
        if (native && gtk_widget_compute_point(GTK_WIDGET(native), window->gtk.view, &GRAPHENE_POINT_INIT((float)(x - nx), (float)(y - ny)), &p)) {
            window->gtk.cursor_x = p.x;
            window->gtk.cursor_y = p.y;
        }
    }
    _glfwGtkRestoreContext();
    _glfwInputMouseClick(window, button, type == GDK_BUTTON_PRESS ? GLFW_PRESS : GLFW_RELEASE, mods);
    return TRUE;
}

static void
on_motion(GtkEventControllerMotion *controller UNUSED, double x, double y, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return;
    window->gtk.cursor_x = x;
    window->gtk.cursor_y = y;
    _glfwGtkRestoreContext();
    _glfwInputCursorPos(window, x, y);
}

static void
on_enter(GtkEventControllerMotion *controller UNUSED, double x, double y, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return;
    window->gtk.hovered = true;
    window->gtk.cursor_x = x;
    window->gtk.cursor_y = y;
    _glfwGtkRestoreContext();
    _glfwInputCursorEnter(window, true);
    _glfwInputCursorPos(window, x, y);
}

static void
on_leave(GtkEventControllerMotion *controller UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return;
    window->gtk.hovered = false;
    _glfwGtkRestoreContext();
    _glfwInputCursorEnter(window, false);
}

static bool
is_finger_scroll(GtkEventController *controller) {
    GdkEvent *event = gtk_event_controller_get_current_event(controller);
    GdkDevice *device = event ? gdk_event_get_device(event) : NULL;
    return device && gdk_device_get_source(device) == GDK_SOURCE_TOUCHPAD;
}

static int
scroll_modifiers(GtkEventController *controller) {
    GdkEvent *event = gtk_event_controller_get_current_event(controller);
    return event ? _glfwGtkModsFromState(gdk_event_get_modifier_state(event), gdk_event_get_device(event)) : 0;
}

static gboolean
on_scroll(GtkEventControllerScroll *controller, double dx, double dy, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return TRUE;
    GLFWScrollEvent ev = {.keyboard_modifiers = scroll_modifiers(GTK_EVENT_CONTROLLER(controller))};
    // Both GTK and Wayland use positive values for down/right, GLFW uses
    // positive values for up/right after the sign flips in wl_init.c
    if (gtk_event_controller_scroll_get_unit(controller) == GDK_SCROLL_UNIT_WHEEL) {
        ev.offset_type = GLFW_SCROLL_OFFEST_V120;
        ev.unscaled.x = -dx * 120;
        ev.unscaled.y = -dy * 120;
        ev.x_offset = ev.unscaled.x;
        ev.y_offset = ev.unscaled.y;
    } else {
        ev.offset_type = GLFW_SCROLL_OFFEST_HIGHRES;
        ev.unscaled.x = -dx;
        ev.unscaled.y = -dy;
        const double scale = surface_scale(window);
        ev.x_offset = ev.unscaled.x * scale;
        ev.y_offset = ev.unscaled.y * scale;
    }
    _glfwGtkRestoreContext();
    glfw_handle_scroll_event_for_momentum(window, &ev, false, is_finger_scroll(GTK_EVENT_CONTROLLER(controller)));
    return TRUE;
}

static void
on_scroll_end(GtkEventControllerScroll *controller, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked()) return;
    GLFWScrollEvent ev = {.keyboard_modifiers = scroll_modifiers(GTK_EVENT_CONTROLLER(controller)), .offset_type = GLFW_SCROLL_OFFEST_HIGHRES};
    _glfwGtkRestoreContext();
    glfw_handle_scroll_event_for_momentum(window, &ev, true, true);
}
// }}}

static GtkCssProvider *global_css = NULL;

static void
ensure_global_css(void) {
    if (global_css) return;
    global_css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(
        global_css,
        "window.kitty-transparent, window.kitty-transparent kittyview { background: transparent; }\n"
        "window.kitty-transparent headerbar { background-color: var(--headerbar-bg-color); }\n"
        "window.kitty-transparent headerbar:backdrop { background-color: var(--headerbar-backdrop-color); }\n");
    gtk_style_context_add_provider_for_display(_glfw.gtk.display, GTK_STYLE_PROVIDER(global_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION);
}

//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int
_glfwPlatformCreateWindow(
    _GLFWwindow *window, const _GLFWwndconfig *wndconfig, const _GLFWctxconfig *ctxconfig, const _GLFWfbconfig *fbconfig, const GLFWLayerShellConfig *lsc) {
    if (lsc) {
        _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "GTK: Layer shell windows (panels) are not supported");
        return false;
    }
    if (ctxconfig->client == GLFW_NO_API) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "GTK: Windows without an OpenGL context are not supported");
        return false;
    }
    if (ctxconfig->client == GLFW_OPENGL_ES_API) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "GTK: OpenGL ES contexts are not supported");
        return false;
    }
    static bool app_id_set = false;
    if (!app_id_set && wndconfig->wl.appId[0]) {
        // GNOME matches the window to kitty.desktop through the app id
        g_set_prgname(wndconfig->wl.appId);
        app_id_set = true;
    }
    ensure_global_css();
    window->gtk.width = wndconfig->width;
    window->gtk.height = wndconfig->height;
    window->gtk.scale = glfwGetWaylandCurrentMonitorFractionalScale();
    window->gtk.opacity = 1.f;
    window->gtk.transparent = fbconfig->transparent;
    window->swaps_disallowed = true;

    GtkWidget *win = adw_window_new();
    window->gtk.window = win;
    gtk_window_set_title(GTK_WINDOW(win), wndconfig->title ? wndconfig->title : "");
    window->gtk.title = _glfw_strdup(wndconfig->title ? wndconfig->title : "");
    gtk_window_set_resizable(GTK_WINDOW(win), wndconfig->resizable);
    gtk_window_set_handle_menubar_accel(GTK_WINDOW(win), FALSE);
    if (window->gtk.transparent) gtk_widget_add_css_class(win, "kitty-transparent");

    window->gtk.toolbar_view = adw_toolbar_view_new();
    window->gtk.header_bar = adw_header_bar_new();
    adw_toolbar_view_add_top_bar(ADW_TOOLBAR_VIEW(window->gtk.toolbar_view), window->gtk.header_bar);
    char cls[64];
    snprintf(cls, sizeof(cls), "kitty-titlebar-%llu", window->id);
    gtk_widget_add_css_class(window->gtk.header_bar, cls);

    KittyView *view = g_object_new(KITTY_TYPE_VIEW, NULL);
    view->window = window;
    view->natural_width = wndconfig->width;
    view->natural_height = wndconfig->height;
    const uint32_t bg = wndconfig->wl.bgcolor;
    view->background =
        (GdkRGBA){((bg >> 16) & 0xff) / 255.f, ((bg >> 8) & 0xff) / 255.f, (bg & 0xff) / 255.f, window->gtk.transparent ? ((bg >> 24) & 0xff) / 255.f : 1.f};
    window->gtk.view = GTK_WIDGET(view);
    adw_toolbar_view_set_content(ADW_TOOLBAR_VIEW(window->gtk.toolbar_view), window->gtk.view);
    adw_window_set_content(ADW_WINDOW(win), window->gtk.toolbar_view);
    update_top_bar(window);

    g_signal_connect(window->gtk.view, "realize", G_CALLBACK(on_view_realize), window);
    g_signal_connect(window->gtk.view, "map", G_CALLBACK(on_view_map), window);
    g_signal_connect(window->gtk.view, "unmap", G_CALLBACK(on_view_unmap), window);
    g_signal_connect(win, "close-request", G_CALLBACK(on_close_request), window);
    g_signal_connect(win, "notify::is-active", G_CALLBACK(on_is_active), window);
    g_signal_connect(win, "notify::maximized", G_CALLBACK(on_maximized), window);
    g_signal_connect(win, "notify::fullscreened", G_CALLBACK(on_fullscreened), window);

    // Keys are captured on the toplevel so GTK never sees them
    GtkEventController *key = gtk_event_controller_key_new();
    gtk_event_controller_set_propagation_phase(key, GTK_PHASE_CAPTURE);
    g_signal_connect(key, "key-pressed", G_CALLBACK(on_key_pressed), window);
    g_signal_connect(key, "key-released", G_CALLBACK(on_key_released), window);
    gtk_widget_add_controller(win, key);

    GtkEventController *legacy = gtk_event_controller_legacy_new();
    g_signal_connect(legacy, "event", G_CALLBACK(on_legacy_event), window);
    gtk_widget_add_controller(window->gtk.view, legacy);

    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "motion", G_CALLBACK(on_motion), window);
    g_signal_connect(motion, "enter", G_CALLBACK(on_enter), window);
    g_signal_connect(motion, "leave", G_CALLBACK(on_leave), window);
    gtk_widget_add_controller(window->gtk.view, motion);

    // Not kinetic, kitty does its own momentum scrolling
    GtkEventController *scroll = gtk_event_controller_scroll_new(GTK_EVENT_CONTROLLER_SCROLL_BOTH_AXES);
    g_signal_connect(scroll, "scroll", G_CALLBACK(on_scroll), window);
    g_signal_connect(scroll, "scroll-end", G_CALLBACK(on_scroll_end), window);
    gtk_widget_add_controller(window->gtk.view, scroll);

    _glfwGtkSetupIME(window);
    _glfwGtkAttachContext(window);

    if (wndconfig->maximized) gtk_window_maximize(GTK_WINDOW(win));
    if (wndconfig->visible) _glfwPlatformShowWindow(window, false);
    return true;
}

void
_glfwPlatformDestroyWindow(_GLFWwindow *window) {
    if (window->gtk.frame.tick_id && window->gtk.view) gtk_widget_remove_tick_callback(window->gtk.view, window->gtk.frame.tick_id);
    window->gtk.frame.tick_id = 0;
    if (window->gtk.surface && window->gtk.scale_handler) g_signal_handler_disconnect(window->gtk.surface, window->gtk.scale_handler);
    window->gtk.surface = NULL;
    window->gtk.scale_handler = 0;
    _glfwGtkDestroyIME(window);
    if (window->context.destroy) window->context.destroy(window);
    if (window->gtk.titlebar_css) {
        gtk_style_context_remove_provider_for_display(_glfw.gtk.display, GTK_STYLE_PROVIDER(window->gtk.titlebar_css));
        g_clear_object(&window->gtk.titlebar_css);
    }
    if (window->gtk.view) KITTY_VIEW(window->gtk.view)->window = NULL;
    if (window->gtk.window) {
        g_signal_handlers_disconnect_by_data(window->gtk.window, window);
        gtk_window_destroy(GTK_WINDOW(window->gtk.window));
    }
    window->gtk.window = window->gtk.view = window->gtk.header_bar = window->gtk.toolbar_view = NULL;
    memset(window->gtk.menu_buttons, 0, sizeof(window->gtk.menu_buttons));
    free(window->gtk.title);
    window->gtk.title = NULL;
    free(window->gtk.ime.commit);
    window->gtk.ime.commit = NULL;
}

void
_glfwPlatformSetWindowTitle(_GLFWwindow *window, const char *title) {
    if (window->gtk.title && strcmp(window->gtk.title, title) == 0) return;
    free(window->gtk.title);
    window->gtk.title = _glfw_strdup(title);
    gtk_window_set_title(GTK_WINDOW(window->gtk.window), title);
}

void
_glfwPlatformSetWindowIcon(_GLFWwindow *window UNUSED, int count UNUSED, const GLFWimage *images UNUSED) {
    // GNOME takes the icon from the .desktop file matching the app id
}

bool
_glfwPlatformSetLayerShellConfig(_GLFWwindow *window UNUSED, const GLFWLayerShellConfig *value UNUSED) {
    return false;
}

const GLFWLayerShellConfig *
_glfwPlatformGetLayerShellConfig(_GLFWwindow *window UNUSED) {
    return NULL;
}

void
_glfwPlatformSetWindowMonitor(_GLFWwindow *window, _GLFWmonitor *monitor, int xpos UNUSED, int ypos UNUSED, int width, int height, int refreshRate UNUSED) {
    if (monitor) gtk_window_fullscreen_on_monitor(GTK_WINDOW(window->gtk.window), monitor->gtk.monitor);
    else {
        if (window->gtk.fullscreen) gtk_window_unfullscreen(GTK_WINDOW(window->gtk.window));
        _glfwPlatformSetWindowSize(window, width, height);
    }
    _glfwInputWindowMonitor(window, monitor);
}

void
_glfwPlatformGetWindowPos(_GLFWwindow *window UNUSED, int *xpos, int *ypos) {
    // Wayland does not expose window positions
    if (xpos) *xpos = 0;
    if (ypos) *ypos = 0;
}

void
_glfwPlatformSetWindowPos(_GLFWwindow *window UNUSED, int xpos UNUSED, int ypos UNUSED) {}

void
_glfwPlatformGetWindowSize(_GLFWwindow *window, int *width, int *height) {
    if (width) *width = window->gtk.width;
    if (height) *height = window->gtk.height;
}

void
_glfwPlatformSetWindowSize(_GLFWwindow *window, int width, int height) {
    KittyView *view = KITTY_VIEW(window->gtk.view);
    view->natural_width = width;
    view->natural_height = height;
    if (!gtk_widget_get_mapped(window->gtk.window)) {
        gtk_widget_queue_resize(window->gtk.view);
        return;
    }
    // The default size is for the whole window, add the size of the header bar
    int extra_w = gtk_widget_get_width(window->gtk.window) - gtk_widget_get_width(window->gtk.view);
    int extra_h = gtk_widget_get_height(window->gtk.window) - gtk_widget_get_height(window->gtk.view);
    gtk_window_set_default_size(GTK_WINDOW(window->gtk.window), width + MAX(0, extra_w), height + MAX(0, extra_h));
}

void
_glfwPlatformSetWindowSizeLimits(_GLFWwindow *window, int minwidth, int minheight, int maxwidth UNUSED, int maxheight UNUSED) {
    // GTK has no maximum size
    gtk_widget_set_size_request(window->gtk.view, minwidth == GLFW_DONT_CARE ? -1 : minwidth, minheight == GLFW_DONT_CARE ? -1 : minheight);
}

void
_glfwPlatformSetWindowAspectRatio(_GLFWwindow *window UNUSED, int numer UNUSED, int denom UNUSED) {}

void
_glfwPlatformSetWindowSizeIncrements(_GLFWwindow *window UNUSED, int widthincr UNUSED, int heightincr UNUSED) {}

void
_glfwPlatformGetFramebufferSize(_GLFWwindow *window, int *width, int *height) {
    const double scale = window->gtk.scale > 0 ? window->gtk.scale : 1;
    if (width) *width = (int)round(window->gtk.width * scale);
    if (height) *height = (int)round(window->gtk.height * scale);
}

void
_glfwPlatformGetWindowFrameSize(_GLFWwindow *window, int *left, int *top, int *right, int *bottom) {
    int t = 0;
    if (window->gtk.header_bar && window->gtk.toolbar_view && adw_toolbar_view_get_reveal_top_bars(ADW_TOOLBAR_VIEW(window->gtk.toolbar_view)))
        t = MAX(0, gtk_widget_get_height(window->gtk.header_bar));
    if (top) *top = t;
    if (left) *left = 0;
    if (right) *right = 0;
    if (bottom) *bottom = 0;
}

void
_glfwPlatformGetWindowContentScale(_GLFWwindow *window, float *xscale, float *yscale) {
    const float s = window->gtk.scale > 0 ? (float)window->gtk.scale : 1.f;
    if (xscale) *xscale = s;
    if (yscale) *yscale = s;
}

monotonic_t
_glfwPlatformGetDoubleClickInterval(_GLFWwindow *window UNUSED) {
    int ms = 400;
    GtkSettings *s = gtk_settings_get_default();
    if (s) g_object_get(s, "gtk-double-click-time", &ms, NULL);
    return ms_to_monotonic_t((long long)ms);
}

void
_glfwPlatformIconifyWindow(_GLFWwindow *window) {
    gtk_window_minimize(GTK_WINDOW(window->gtk.window));
}

void
_glfwPlatformRestoreWindow(_GLFWwindow *window) {
    GtkWindow *w = GTK_WINDOW(window->gtk.window);
    if (window->gtk.fullscreen) gtk_window_unfullscreen(w);
    else if (window->gtk.maximized) gtk_window_unmaximize(w);
    else gtk_window_unminimize(w);
}

void
_glfwPlatformMaximizeWindow(_GLFWwindow *window) {
    gtk_window_maximize(GTK_WINDOW(window->gtk.window));
}

int
_glfwPlatformWindowMaximized(_GLFWwindow *window) {
    return window->gtk.maximized;
}

int
_glfwPlatformWindowHovered(_GLFWwindow *window) {
    return window->gtk.hovered;
}

int
_glfwPlatformFramebufferTransparent(_GLFWwindow *window) {
    return window->gtk.transparent;
}

void
_glfwPlatformSetWindowResizable(_GLFWwindow *window, bool enabled) {
    gtk_window_set_resizable(GTK_WINDOW(window->gtk.window), enabled);
}

void
_glfwPlatformSetWindowDecorated(_GLFWwindow *window, bool enabled UNUSED) {
    // window->decorated is already updated
    update_top_bar(window);
}

void
_glfwPlatformSetWindowFloating(_GLFWwindow *window UNUSED, bool enabled UNUSED) {}

void
_glfwPlatformSetWindowMousePassthrough(_GLFWwindow *window UNUSED, bool enabled UNUSED) {}

float
_glfwPlatformGetWindowOpacity(_GLFWwindow *window) {
    return window->gtk.opacity;
}

void
_glfwPlatformSetWindowOpacity(_GLFWwindow *window, float opacity) {
    window->gtk.opacity = opacity;
}

int
_glfwPlatformSetWindowBlur(_GLFWwindow *window UNUSED, int value UNUSED) {
    return 0;
}

void
_glfwPlatformSetRawMouseMotion(_GLFWwindow *window UNUSED, bool enabled UNUSED) {}

bool
_glfwPlatformRawMouseMotionSupported(void) {
    return false;
}

void
_glfwPlatformShowWindow(_GLFWwindow *window, bool move_to_active_screen UNUSED) {
    window->gtk.visible = true;
    gtk_window_present(GTK_WINDOW(window->gtk.window));
}

void
_glfwPlatformRequestWindowAttention(_GLFWwindow *window) {
    // Without an activation token GNOME shows a "ready" notification
    gtk_window_present(GTK_WINDOW(window->gtk.window));
}

int
_glfwPlatformWindowBell(_GLFWwindow *window) {
    gtk_widget_error_bell(window->gtk.view);
    return true;
}

void
_glfwPlatformHideWindow(_GLFWwindow *window) {
    window->gtk.visible = false;
    gtk_widget_set_visible(window->gtk.window, FALSE);
}

void
_glfwPlatformFocusWindow(_GLFWwindow *window) {
    gtk_window_present(GTK_WINDOW(window->gtk.window));
}

bool
_glfwPlatformToggleFullscreen(_GLFWwindow *window, unsigned int flags UNUSED) {
    GtkWindow *w = GTK_WINDOW(window->gtk.window);
    bool was_fullscreen = gtk_window_is_fullscreen(w);
    if (was_fullscreen) gtk_window_unfullscreen(w);
    else gtk_window_fullscreen(w);
    window->gtk.fullscreen = !was_fullscreen;
    return !was_fullscreen;
}

bool
_glfwPlatformIsFullscreen(_GLFWwindow *window, unsigned int flags UNUSED) {
    return window->gtk.fullscreen;
}

int
_glfwPlatformWindowFocused(_GLFWwindow *window) {
    return window->gtk.focused;
}

int
_glfwPlatformWindowOccluded(_GLFWwindow *window UNUSED) {
    return false;
}

int
_glfwPlatformWindowIconified(_GLFWwindow *window UNUSED) {
    // Wayland does not tell clients when they are minimized
    return false;
}

int
_glfwPlatformWindowVisible(_GLFWwindow *window) {
    return window->gtk.visible;
}

void
_glfwPlatformGetCursorPos(_GLFWwindow *window, double *xpos, double *ypos) {
    if (xpos) *xpos = window->gtk.cursor_x;
    if (ypos) *ypos = window->gtk.cursor_y;
}

void
_glfwPlatformSetCursorPos(_GLFWwindow *window UNUSED, double x UNUSED, double y UNUSED) {
    // Wayland does not allow warping the pointer
}

// Cursors {{{

static void
apply_cursor(_GLFWwindow *window) {
    if (!window->gtk.view) return;
    if (window->cursorMode != GLFW_CURSOR_NORMAL) {
        // NULL would mean the parent's cursor, not no cursor
        gtk_widget_set_cursor_from_name(window->gtk.view, "none");
        return;
    }
    _GLFWcursor *c = window->gtk.current_cursor;
    gtk_widget_set_cursor(window->gtk.view, c ? c->gtk.cursor : NULL);
}

void
_glfwPlatformSetCursorMode(_GLFWwindow *window, int mode UNUSED) {
    apply_cursor(window);
}

static const char *
css_name_for_shape(GLFWCursorShape shape) {
#define C(g, n) \
    case g: return n
    switch (shape) {
        /* start glfw to css mapping (auto generated by gen-key-constants.py do not edit) */
        C(GLFW_DEFAULT_CURSOR, "default");
        C(GLFW_TEXT_CURSOR, "text");
        C(GLFW_POINTER_CURSOR, "pointer");
        C(GLFW_HELP_CURSOR, "help");
        C(GLFW_WAIT_CURSOR, "wait");
        C(GLFW_PROGRESS_CURSOR, "progress");
        C(GLFW_CROSSHAIR_CURSOR, "crosshair");
        C(GLFW_CELL_CURSOR, "cell");
        C(GLFW_VERTICAL_TEXT_CURSOR, "vertical-text");
        C(GLFW_MOVE_CURSOR, "move");
        C(GLFW_E_RESIZE_CURSOR, "e-resize");
        C(GLFW_NE_RESIZE_CURSOR, "ne-resize");
        C(GLFW_NW_RESIZE_CURSOR, "nw-resize");
        C(GLFW_N_RESIZE_CURSOR, "n-resize");
        C(GLFW_SE_RESIZE_CURSOR, "se-resize");
        C(GLFW_SW_RESIZE_CURSOR, "sw-resize");
        C(GLFW_S_RESIZE_CURSOR, "s-resize");
        C(GLFW_W_RESIZE_CURSOR, "w-resize");
        C(GLFW_EW_RESIZE_CURSOR, "ew-resize");
        C(GLFW_NS_RESIZE_CURSOR, "ns-resize");
        C(GLFW_NESW_RESIZE_CURSOR, "nesw-resize");
        C(GLFW_NWSE_RESIZE_CURSOR, "nwse-resize");
        C(GLFW_ZOOM_IN_CURSOR, "zoom-in");
        C(GLFW_ZOOM_OUT_CURSOR, "zoom-out");
        C(GLFW_ALIAS_CURSOR, "alias");
        C(GLFW_COPY_CURSOR, "copy");
        C(GLFW_NOT_ALLOWED_CURSOR, "not-allowed");
        C(GLFW_NO_DROP_CURSOR, "no-drop");
        C(GLFW_GRAB_CURSOR, "grab");
        C(GLFW_GRABBING_CURSOR, "grabbing");
            /* end glfw to css mapping */
        case GLFW_INVALID_CURSOR: break;
    }
#undef C
    return NULL;
}

int
_glfwPlatformCreateCursor(_GLFWcursor *cursor, const GLFWimage *image, int xhot, int yhot, int count UNUSED) {
    GBytes *bytes = g_bytes_new(image->pixels, (gsize)image->width * image->height * 4);
    GdkTexture *texture = gdk_memory_texture_new(image->width, image->height, GDK_MEMORY_R8G8B8A8, bytes, (gsize)image->width * 4);
    g_bytes_unref(bytes);
    cursor->gtk.cursor = gdk_cursor_new_from_texture(texture, xhot, yhot, NULL);
    g_object_unref(texture);
    return cursor->gtk.cursor != NULL;
}

int
_glfwPlatformCreateStandardCursor(_GLFWcursor *cursor, GLFWCursorShape shape) {
    const char *name = css_name_for_shape(shape);
    if (!name) {
        _glfwInputError(GLFW_INVALID_ENUM, "GTK: Unknown standard cursor shape: %d", shape);
        return false;
    }
    cursor->gtk.cursor = gdk_cursor_new_from_name(name, NULL);
    return cursor->gtk.cursor != NULL;
}

void
_glfwPlatformDestroyCursor(_GLFWcursor *cursor) {
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (w->gtk.current_cursor == cursor) {
            w->gtk.current_cursor = NULL;
            apply_cursor(w);
        }
    }
    g_clear_object(&cursor->gtk.cursor);
}

void
_glfwPlatformSetCursor(_GLFWwindow *window, _GLFWcursor *cursor) {
    window->gtk.current_cursor = cursor;
    apply_cursor(window);
}

void
_glfwPlatformChangeCursorTheme(void) {}
// }}}

// Drag and drop is not supported {{{

void
_glfwPlatformRequestDropUpdate(_GLFWwindow *window UNUSED) {}

ssize_t
_glfwPlatformReadAvailableDropData(GLFWwindow *w UNUSED, GLFWDropEvent *ev UNUSED, char *buffer UNUSED, size_t sz UNUSED) {
    return -ENOENT;
}

void
_glfwPlatformEndDrop(GLFWwindow *w UNUSED, GLFWDragOperationType op UNUSED) {}

int
_glfwPlatformRequestDropData(_GLFWwindow *window UNUSED, const char *mime UNUSED) {
    return EINVAL;
}

int
_glfwPlatformStartDrag(_GLFWwindow *window UNUSED, const GLFWimage *thumbnail UNUSED) {
    return ENOTSUP;
}

void
_glfwPlatformCancelDrag(_GLFWwindow *window UNUSED) {}

void
_glfwPlatformFreeDragSourceData(void) {}

int
_glfwPlatformDragDataReady(const char *mime_type UNUSED, const char *data UNUSED, size_t sz UNUSED, int type UNUSED) {
    return ENOTSUP;
}

int
_glfwPlatformChangeDragImage(const GLFWimage *thumbnail UNUSED) {
    return ENOTSUP;
}
// }}}

// Clipboard {{{

static GdkClipboard *
clipboard_for(GLFWClipboardType t) {
    return t == GLFW_PRIMARY_SELECTION ? gdk_display_get_primary_clipboard(_glfw.gtk.display) : gdk_display_get_clipboard(_glfw.gtk.display);
}

static void
on_clipboard_local(GObject *obj, GParamSpec *pspec UNUSED, gpointer data) {
    GLFWClipboardType t = (GLFWClipboardType)GPOINTER_TO_INT(data);
    if (gdk_clipboard_is_local(GDK_CLIPBOARD(obj))) return;
    debug_input("GTK: lost ownership of the %s\n", t == GLFW_CLIPBOARD ? "clipboard" : "primary selection");
    _glfw_free_clipboard_data(t == GLFW_CLIPBOARD ? &_glfw.clipboard : &_glfw.primary);
    _glfwGtkRestoreContext();
    _glfwInputClipboardLost(t);
}

static GBytes *
read_all_chunks(const _GLFWClipboardData *cd, const char *mime) {
    GByteArray *arr = g_byte_array_new();
    GLFWDataChunk chunk = cd->get_data(mime, NULL, cd->ctype);
    void *iter = chunk.iter;
    if (iter) {
        while (true) {
            chunk = cd->get_data(mime, iter, cd->ctype);
            if (!chunk.sz) break;
            g_byte_array_append(arr, (const guint8 *)chunk.data, (guint)chunk.sz);
            if (chunk.free) chunk.free((void *)chunk.free_data);
        }
        cd->get_data(NULL, iter, cd->ctype);
    }
    return g_byte_array_free_to_bytes(arr);
}

void
_glfwPlatformSetClipboard(GLFWClipboardType t) {
    _GLFWClipboardData *cd = t == GLFW_CLIPBOARD ? &_glfw.clipboard : &_glfw.primary;
    GdkClipboard *cb = clipboard_for(t);
    gulong *handler = t == GLFW_CLIPBOARD ? &_glfw.gtk.clipboard_handler : &_glfw.gtk.primary_handler;
    if (!*handler) *handler = g_signal_connect(cb, "notify::local", G_CALLBACK(on_clipboard_local), GINT_TO_POINTER(t));
    // The data is pulled from kitty right away, the GLFW API has no way to
    // serve it asynchronously.
    GPtrArray *providers = g_ptr_array_new();
    for (size_t i = 0; i < cd->num_mime_types; i++) {
        const char *mime = cd->mime_types[i];
        GBytes *bytes = read_all_chunks(cd, mime);
        if (strcmp(mime, "text/plain") == 0) {
            static const char *aliases[] = {"text/plain;charset=utf-8", "UTF8_STRING", "STRING", "TEXT"};
            for (size_t a = 0; a < arraysz(aliases); a++) g_ptr_array_add(providers, gdk_content_provider_new_for_bytes(aliases[a], bytes));
        }
        g_ptr_array_add(providers, gdk_content_provider_new_for_bytes(mime, bytes));
        g_bytes_unref(bytes);
    }
    GdkContentProvider *union_provider = gdk_content_provider_new_union((GdkContentProvider **)providers->pdata, providers->len);
    g_ptr_array_free(providers, TRUE);
    gdk_clipboard_set_content(cb, union_provider);
    g_object_unref(union_provider);
}

typedef struct ClipboardRead {
    bool done;
    GInputStream *stream;
    GError *error;
} ClipboardRead;

static void
on_clipboard_read(GObject *source, GAsyncResult *res, gpointer data) {
    ClipboardRead *r = data;
    r->stream = gdk_clipboard_read_finish(GDK_CLIPBOARD(source), res, NULL, &r->error);
    r->done = true;
}

static void
read_stream(GInputStream *stream, GLFWclipboardwritedatafun write_data, void *object) {
    char buf[8192];
    int fd = G_IS_UNIX_INPUT_STREAM(stream) ? g_unix_input_stream_get_fd(G_UNIX_INPUT_STREAM(stream)) : -1;
    while (true) {
        if (fd > -1) {
            // Do not hang forever on a client that never closes the pipe
            struct pollfd pfd = {.fd = fd, .events = POLLIN};
            int ret = poll(&pfd, 1, 2000);
            if (ret < 0) {
                if (errno == EINTR) continue;
                break;
            }
            if (ret == 0) {
                _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Failed to read clipboard data (timed out)");
                break;
            }
        }
        GError *err = NULL;
        gssize n = g_input_stream_read(stream, buf, sizeof(buf), NULL, &err);
        if (n < 0) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Failed to read clipboard data: %s", err ? err->message : "unknown error");
            g_clear_error(&err);
            break;
        }
        if (n == 0) break;
        if (!write_data(object, buf, (size_t)n)) break;
    }
}

void
_glfwPlatformGetClipboard(GLFWClipboardType clipboard_type, const char *mime_type, GLFWclipboardwritedatafun write_data, void *object) {
    GdkClipboard *cb = clipboard_for(clipboard_type);
    if (gdk_clipboard_is_local(cb)) {
        // kitty already has the data
        write_data(object, NULL, 1);
        return;
    }
    GdkContentFormats *formats = gdk_clipboard_get_formats(cb);
    gsize n_mimes = 0;
    const char *const *mimes = formats ? gdk_content_formats_get_mime_types(formats, &n_mimes) : NULL;
    if (mime_type == NULL) {
        bool ok = true;
        for (gsize i = 0; i < n_mimes && ok; i++) {
            const char *q = mimes[i];
            if (strcmp(q, "text/plain;charset=utf-8") == 0 || strcmp(q, "UTF8_STRING") == 0 || strcmp(q, "STRING") == 0 || strcmp(q, "TEXT") == 0)
                q = "text/plain";
            ok = write_data(object, q, strlen(q));
        }
        return;
    }
    const char *text_mimes[] = {"text/plain;charset=utf-8", "text/plain", "UTF8_STRING", "STRING", "TEXT", NULL};
    const char *one_mime[] = {mime_type, NULL};
    const char **wanted = strcmp(mime_type, "text/plain") == 0 ? text_mimes : one_mime;
    ClipboardRead r = {0};
    gdk_clipboard_read_async(cb, wanted, G_PRIORITY_DEFAULT, NULL, on_clipboard_read, &r);
    // The GLFW API is synchronous, spin the main loop until GDK has a stream
    // for us. Ticks, timers and input are kept away from kitty meanwhile.
    _glfw.gtk.nested_loop_depth++;
    while (!r.done) g_main_context_iteration(NULL, TRUE);
    _glfwGtkEndNestedLoop();
    if (!r.stream) {
        // Not having the requested type is not an error
        if (r.error && !g_error_matches(r.error, G_IO_ERROR, G_IO_ERROR_NOT_SUPPORTED))
            _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Failed to read clipboard: %s", r.error->message);
        g_clear_error(&r.error);
        return;
    }
    read_stream(r.stream, write_data, object);
    g_input_stream_close(r.stream, NULL, NULL);
    g_object_unref(r.stream);
}
// }}}

bool
_glfwPlatformGrabKeyboard(bool grab) {
    for (_GLFWwindow *w = _glfw.windowListHead; w; w = w->next) {
        if (!w->gtk.surface || !GDK_IS_TOPLEVEL(w->gtk.surface)) continue;
        if (grab) gdk_toplevel_inhibit_system_shortcuts(GDK_TOPLEVEL(w->gtk.surface), NULL);
        else gdk_toplevel_restore_system_shortcuts(GDK_TOPLEVEL(w->gtk.surface));
    }
    return true;
}

//////////////////////////////////////////////////////////////////////////
//////                        GLFW native API                       //////
//////////////////////////////////////////////////////////////////////////

static gboolean
on_frame_tick(GtkWidget *widget UNUSED, GdkFrameClock *clock UNUSED, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window) return G_SOURCE_REMOVE;
    window->gtk.frame.tick_id = 0;
    GLFWwaylandframecallbackfunc cb = window->gtk.frame.callback;
    unsigned long long id = window->gtk.frame.id;
    window->gtk.frame.callback = NULL;
    window->gtk.frame.id = 0;
    if (cb) {
        _glfwGtkRestoreContext();
        cb(id);
    }
    return G_SOURCE_REMOVE;
}

// kitty renders once per GTK frame clock tick, the same model as a Wayland
// frame callback
GLFWAPI void
glfwRequestWaylandFrameEvent(GLFWwindow *handle, unsigned long long id, void (*callback)(unsigned long long id)) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (!window || !window->gtk.view) return;
    window->gtk.frame.id = id;
    window->gtk.frame.callback = callback;
    if (!window->gtk.frame.tick_id) window->gtk.frame.tick_id = gtk_widget_add_tick_callback(window->gtk.view, on_frame_tick, window, NULL);
}

GLFWAPI void
glfwWaylandActivateWindow(GLFWwindow *handle, const char *activation_token) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    _GLFW_REQUIRE_INIT();
    if (activation_token && activation_token[0]) gtk_window_set_startup_id(GTK_WINDOW(window->gtk.window), activation_token);
    gtk_window_present(GTK_WINDOW(window->gtk.window));
}

GLFWAPI void
glfwWaylandRunWithActivationToken(GLFWwindow *handle, GLFWactivationcallback cb, void *cb_data) {
    _GLFW_REQUIRE_INIT();
    // GTK 4 has no public API to request an activation token
    if (cb) cb(handle, NULL, cb_data);
}

GLFWAPI bool
glfwWaylandSetTitlebarColor(GLFWwindow *handle, uint32_t color, bool use_system_color) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (window->gtk.titlebar_css) {
        gtk_style_context_remove_provider_for_display(_glfw.gtk.display, GTK_STYLE_PROVIDER(window->gtk.titlebar_css));
        g_clear_object(&window->gtk.titlebar_css);
    }
    if (use_system_color) return true;
    const unsigned r = (color >> 16) & 0xff, g = (color >> 8) & 0xff, b = color & 0xff;
    // Pick a readable foreground, like the CSD code in wl_client_side_decorations.c
    const double luminance = (0.299 * r + 0.587 * g + 0.114 * b) / 255.0;
    const char *fg = luminance < 0.5 ? "#ffffff" : "#000000";
    char css[512];
    snprintf(
        css,
        sizeof(css),
        "headerbar.kitty-titlebar-%llu { background: #%02x%02x%02x; color: %s; box-shadow: none; }\n"
        "headerbar.kitty-titlebar-%llu:backdrop { background: #%02x%02x%02x; }\n",
        window->id,
        r,
        g,
        b,
        fg,
        window->id,
        r,
        g,
        b);
    window->gtk.titlebar_css = gtk_css_provider_new();
    gtk_css_provider_load_from_string(window->gtk.titlebar_css, css);
    gtk_style_context_add_provider_for_display(_glfw.gtk.display, GTK_STYLE_PROVIDER(window->gtk.titlebar_css), GTK_STYLE_PROVIDER_PRIORITY_APPLICATION + 1);
    return true;
}

GLFWAPI void
glfwWaylandRedrawCSDWindowTitle(GLFWwindow *handle UNUSED) {}

GLFWAPI void
glfwWaylandSetTitlebarHidden(GLFWwindow *handle, bool hidden) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    window->gtk.titlebar_hidden = hidden;
    update_top_bar(window);
}

static void
on_menu_action(GSimpleAction *action UNUSED, GVariant *parameter, gpointer data) {
    _GLFWwindow *window = live_window(data);
    if (!window || input_blocked() || !window->gtk.menu_callback) return;
    _glfwGtkRestoreContext();
    window->gtk.menu_callback((GLFWwindow *)window, g_variant_get_string(parameter, NULL));
}

static void
on_menu_closed(GtkPopover *popover UNUSED, gpointer data) {
    // GTK gives the focus back to the menu button, the terminal should have it
    _GLFWwindow *window = live_window(data);
    if (window && window->gtk.view) gtk_widget_grab_focus(window->gtk.view);
}

static char *
menu_key_label(const GLFWgtkmenukey *k) {
    const guint keyval = k->native_key ? k->key : _glfwGtkSymForKey(k->key);
    if (!keyval) return NULL;
    GdkModifierType mods = 0;
    if (k->mods & GLFW_MOD_SHIFT) mods |= GDK_SHIFT_MASK;
    if (k->mods & GLFW_MOD_CONTROL) mods |= GDK_CONTROL_MASK;
    if (k->mods & GLFW_MOD_ALT) mods |= GDK_ALT_MASK;
    if (k->mods & GLFW_MOD_SUPER) mods |= GDK_SUPER_MASK;
    if (k->mods & GLFW_MOD_HYPER) mods |= GDK_HYPER_MASK;
    if (k->mods & GLFW_MOD_META) mods |= GDK_META_MASK;
    return gtk_accelerator_get_label(keyval, mods);
}

// The shortcut of the item as shown in the menu, e.g. "Ctrl+B C", NULL for none
static char *
menu_shortcut_label(const GLFWgtkmenuitem *item) {
    if (!item->num_keys) return NULL;
    GString *ans = g_string_new(NULL);
    for (size_t i = 0; i < item->num_keys; i++) {
        char *label = menu_key_label(&item->keys[i]);
        if (!label) return g_string_free(ans, TRUE), NULL;
        if (i) g_string_append_c(ans, ' ');
        g_string_append(ans, label);
        g_free(label);
    }
    return g_string_free(ans, FALSE);
}

// GtkModelButton can only show a single key shortcut, so the menu items are
// buttons that look like it, see the popover.menu modelbutton theme rules.
// Like GtkPopoverMenu does for its items, the focused item is the selected one
// and hovering an item focuses it, so only one is highlighted.
static void
on_menu_row_focus_enter(GtkEventControllerFocus *controller, gpointer data UNUSED) {
    gtk_widget_set_state_flags(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)), GTK_STATE_FLAG_SELECTED, FALSE);
}

static void
on_menu_row_focus_leave(GtkEventControllerFocus *controller, gpointer data UNUSED) {
    gtk_widget_unset_state_flags(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)), GTK_STATE_FLAG_SELECTED);
}

static void
on_menu_row_enter(GtkEventControllerMotion *controller, double x UNUSED, double y UNUSED, gpointer data UNUSED) {
    gtk_widget_grab_focus(gtk_event_controller_get_widget(GTK_EVENT_CONTROLLER(controller)));
}

static void
on_menu_row_clicked(GtkButton *button, gpointer data UNUSED) {
    GtkWidget *popover = gtk_widget_get_ancestor(GTK_WIDGET(button), GTK_TYPE_POPOVER);
    if (popover) gtk_popover_popdown(GTK_POPOVER(popover));
}

// Not every icon theme has the icon, GTK's own icons fill in for the theme's
static const char *
menu_icon(const char *icon, const char *fallback) {
    if (icon && gtk_icon_theme_has_icon(gtk_icon_theme_get_for_display(_glfw.gtk.display), icon)) return icon;
    return fallback;
}

static void
menu_set_action(GtkWidget *widget, const GLFWgtkmenuitem *item) {
    gtk_actionable_set_action_name(GTK_ACTIONABLE(widget), "kitty.menu");
    gtk_actionable_set_action_target_value(GTK_ACTIONABLE(widget), g_variant_new_string(item->action ? item->action : ""));
}

// The tooltip of an item, with its shortcut, e.g. "Zoom In (Ctrl+=)"
static char *
menu_tooltip(const GLFWgtkmenuitem *item) {
    const char *text = item->tooltip ? item->tooltip : item->label;
    char *shortcut = menu_shortcut_label(item);
    char *ans = shortcut ? g_strdup_printf("%s (%s)", text, shortcut) : g_strdup(text);
    g_free(shortcut);
    return ans;
}

static GtkWidget *
menu_row_new(const GLFWgtkmenuitem *item, GtkWidget **label_widget) {
    GtkWidget *button = g_object_new(GTK_TYPE_BUTTON, "css-name", "modelbutton", "accessible-role", GTK_ACCESSIBLE_ROLE_MENU_ITEM, NULL);
    menu_set_action(button, item);
    g_signal_connect(button, "clicked", G_CALLBACK(on_menu_row_clicked), NULL);
    GtkEventController *focus = gtk_event_controller_focus_new();
    g_signal_connect(focus, "enter", G_CALLBACK(on_menu_row_focus_enter), NULL);
    g_signal_connect(focus, "leave", G_CALLBACK(on_menu_row_focus_leave), NULL);
    gtk_widget_add_controller(button, focus);
    GtkEventController *motion = gtk_event_controller_motion_new();
    g_signal_connect(motion, "enter", G_CALLBACK(on_menu_row_enter), NULL);
    gtk_widget_add_controller(button, motion);

    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    GtkWidget *label = gtk_label_new(item->label);
    gtk_label_set_xalign(GTK_LABEL(label), 0);
    gtk_widget_set_hexpand(label, TRUE);
    gtk_box_append(GTK_BOX(box), label);
    *label_widget = label;
    char *shortcut = menu_shortcut_label(item);
    if (shortcut) {
        // Only shown, kitty handles the keys itself
        GtkWidget *accel = g_object_new(GTK_TYPE_LABEL, "css-name", "accelerator", "label", shortcut, NULL);
        gtk_box_append(GTK_BOX(box), accel);
        g_free(shortcut);
    }
    gtk_button_set_child(GTK_BUTTON(button), box);
    return button;
}

// A row of buttons, like the zoom controls in the menus of GNOME Console and
// Text Editor. Unlike the other items, they leave the menu open.
static GtkWidget *
menu_inline_row_new(const GLFWgtkmenuitem *items, size_t count, GtkWidget **label_widgets) {
    GtkWidget *box = gtk_box_new(GTK_ORIENTATION_HORIZONTAL, 0);
    for (size_t i = 0; i < count; i++) {
        GtkWidget *button = gtk_button_new();
        menu_set_action(button, &items[i]);
        gtk_widget_add_css_class(button, "flat");
        const char *icon = menu_icon(items[i].icon, NULL);
        if (icon) {
            gtk_button_set_icon_name(GTK_BUTTON(button), icon);
            gtk_widget_add_css_class(button, "circular");
        } else {
            GtkWidget *label = gtk_label_new(items[i].label);
            gtk_widget_add_css_class(label, "numeric");
            gtk_button_set_child(GTK_BUTTON(button), label);
            gtk_widget_set_hexpand(button, TRUE);
            label_widgets[i] = label;
        }
        char *tooltip = menu_tooltip(&items[i]);
        gtk_widget_set_tooltip_text(button, tooltip);
        g_free(tooltip);
        gtk_box_append(GTK_BOX(box), button);
    }
    return box;
}

// The menu model only has the sections and a slot for each row, filled with its
// widget by menu_fill_popover()
static GMenu *
menu_model_new(const GLFWgtkmenuitem *items, size_t count) {
    GMenu *menu = g_menu_new(), *section = g_menu_new();
    for (size_t i = 0; i < count; i++) {
        if (!items[i].label) {
            if (g_menu_model_get_n_items(G_MENU_MODEL(section))) g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
            g_object_unref(section);
            section = g_menu_new();
            continue;
        }
        if (items[i].is_inline && i && items[i - 1].label && items[i - 1].is_inline) continue;
        GMenuItem *item = g_menu_item_new(NULL, NULL);
        char id[32];
        snprintf(id, sizeof(id), "item-%zu", i);
        g_menu_item_set_attribute(item, "custom", "s", id);
        g_menu_append_item(section, item);
        g_object_unref(item);
    }
    if (g_menu_model_get_n_items(G_MENU_MODEL(section))) g_menu_append_section(menu, NULL, G_MENU_MODEL(section));
    g_object_unref(section);
    return menu;
}

#define MENU_LABELS_KEY "kitty-menu-labels"

// Keeps the label widget of each item, by index, on button for
// glfwGtkSetTitlebarMenuItemLabel(). The popover owns the widgets.
static void
menu_fill_popover(GtkWidget *button, GtkPopover *popover, _GLFWwindow *window, const GLFWgtkmenuitem *items, size_t count) {
    GtkWidget **labels = g_new0(GtkWidget *, count + 1);
    g_object_set_data_full(G_OBJECT(button), MENU_LABELS_KEY, labels, g_free);
    g_object_set_data(G_OBJECT(button), MENU_LABELS_KEY "-count", GSIZE_TO_POINTER(count));
    if (!popover) return;
    g_signal_connect(popover, "closed", G_CALLBACK(on_menu_closed), window);
    for (size_t i = 0; i < count; i++) {
        if (!items[i].label) continue;
        char id[32];
        snprintf(id, sizeof(id), "item-%zu", i);
        GtkWidget *row;
        if (items[i].is_inline) {
            size_t n = 1;
            while (i + n < count && items[i + n].label && items[i + n].is_inline) n++;
            row = menu_inline_row_new(items + i, n, labels + i);
            i += n - 1;
        } else row = menu_row_new(&items[i], labels + i);
        gtk_popover_menu_add_child(GTK_POPOVER_MENU(popover), row, id);
    }
}

static void
on_split_button_clicked(AdwSplitButton *button UNUSED, gpointer data) {
    // The button took the keyboard focus, the terminal should have it
    _GLFWwindow *window = live_window(data);
    if (window && window->gtk.view) gtk_widget_grab_focus(window->gtk.view);
}

// Shows a button with a menu of the items in the header bar, see
// GLFWgtkmenuitem and GLFWgtktitlebarmenu. Activating an item calls callback
// with its action.
GLFWAPI void
glfwGtkSetTitlebarMenu(GLFWwindow *handle, GLFWgtktitlebarmenu which, const GLFWgtkmenuitem *items, size_t count, GLFWgtkmenuactionfun callback) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (!window->gtk.header_bar || which >= GLFW_GTK_TITLEBAR_MENU_COUNT) return;
    if (window->gtk.menu_buttons[which]) {
        adw_header_bar_remove(ADW_HEADER_BAR(window->gtk.header_bar), window->gtk.menu_buttons[which]);
        window->gtk.menu_buttons[which] = NULL;
    }
    window->gtk.menu_callback = callback;
    if (!count || !callback || !items[0].label) return;

    GSimpleActionGroup *group = g_simple_action_group_new();
    GSimpleAction *action = g_simple_action_new("menu", G_VARIANT_TYPE_STRING);
    g_signal_connect(action, "activate", G_CALLBACK(on_menu_action), window);
    g_action_map_add_action(G_ACTION_MAP(group), G_ACTION(action));
    g_object_unref(action);
    gtk_widget_insert_action_group(window->gtk.window, "kitty", G_ACTION_GROUP(group));
    g_object_unref(group);

    GMenu *menu = menu_model_new(items, count);
    GtkWidget *button;
    GtkPopover *popover;
    if (which == GLFW_GTK_TITLEBAR_MENU_START) {
        button = adw_split_button_new();
        adw_split_button_set_icon_name(ADW_SPLIT_BUTTON(button), menu_icon(items[0].icon, "list-add-symbolic"));
        adw_split_button_set_menu_model(ADW_SPLIT_BUTTON(button), G_MENU_MODEL(menu));
        menu_set_action(button, &items[0]);
        char *tooltip = menu_tooltip(&items[0]);
        gtk_widget_set_tooltip_text(button, tooltip);
        g_free(tooltip);
        adw_split_button_set_dropdown_tooltip(ADW_SPLIT_BUTTON(button), "More");
        g_signal_connect(button, "clicked", G_CALLBACK(on_split_button_clicked), window);
        // Keyboard focus stays with the terminal
        for (GtkWidget *c = gtk_widget_get_first_child(button); c; c = gtk_widget_get_next_sibling(c)) gtk_widget_set_focus_on_click(c, FALSE);
        popover = adw_split_button_get_popover(ADW_SPLIT_BUTTON(button));
    } else {
        button = gtk_menu_button_new();
        gtk_menu_button_set_icon_name(GTK_MENU_BUTTON(button), "open-menu-symbolic");
        gtk_menu_button_set_menu_model(GTK_MENU_BUTTON(button), G_MENU_MODEL(menu));
        gtk_menu_button_set_primary(GTK_MENU_BUTTON(button), TRUE);
        gtk_widget_set_tooltip_text(button, "Main Menu");
        gtk_widget_set_focus_on_click(button, FALSE);
        popover = gtk_menu_button_get_popover(GTK_MENU_BUTTON(button));
    }
    g_object_unref(menu);
    menu_fill_popover(button, popover, window, items, count);
    if (which == GLFW_GTK_TITLEBAR_MENU_START) adw_header_bar_pack_start(ADW_HEADER_BAR(window->gtk.header_bar), button);
    else adw_header_bar_pack_end(ADW_HEADER_BAR(window->gtk.header_bar), button);
    window->gtk.menu_buttons[which] = button;
}

// Changes the label of an item, while its menu may be open
GLFWAPI void
glfwGtkSetTitlebarMenuItemLabel(GLFWwindow *handle, GLFWgtktitlebarmenu which, size_t index, const char *label) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (which >= GLFW_GTK_TITLEBAR_MENU_COUNT || !window->gtk.menu_buttons[which]) return;
    GObject *button = G_OBJECT(window->gtk.menu_buttons[which]);
    GtkWidget **labels = g_object_get_data(button, MENU_LABELS_KEY);
    const size_t count = GPOINTER_TO_SIZE(g_object_get_data(button, MENU_LABELS_KEY "-count"));
    if (labels && index < count && labels[index]) gtk_label_set_label(GTK_LABEL(labels[index]), label);
}

static GLFWwaylandinitialsizefun initial_window_size_callback = NULL;

GLFWAPI void
glfwWaylandSetInitialWindowSizeCallback(GLFWwaylandinitialsizefun callback) {
    // The window is created at the size kitty computed for the scale of the
    // current monitor, which is also what GTK maps it at, so this is unused.
    initial_window_size_callback = callback;
}

GLFWAPI bool
glfwWaylandIsWindowFullyCreated(GLFWwindow *handle) {
    return handle != NULL && ((_GLFWwindow *)handle)->gtk.fully_created;
}

GLFWAPI bool
glfwWaylandBeep(GLFWwindow *handle) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (!window) window = _glfwFocusedWindow();
    if (!window) window = _glfw.windowListHead;
    if (!window || !window->gtk.view) return false;
    gtk_widget_error_bell(window->gtk.view);
    return true;
}
