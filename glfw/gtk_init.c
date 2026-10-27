//========================================================================
// GLFW 3.4 GTK 4 / libadwaita - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2016 Google Inc.
// Copyright (c) 2016-2019 Camilla Löwy <elmindreda@glfw.org>
// Copyright (c) 2026 Tom Dryer <tomdryer.com@gmail.com>
//
// Initialization, the GTK owned main loop, timers and the shared GL context.
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

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define debug debug_rendering

// Color scheme {{{

static GLFWColorScheme
current_color_scheme(void) {
    if (!_glfw.gtk.style_manager) return GLFW_COLOR_SCHEME_NO_PREFERENCE;
    return adw_style_manager_get_dark(_glfw.gtk.style_manager) ? GLFW_COLOR_SCHEME_DARK : GLFW_COLOR_SCHEME_LIGHT;
}

static void
on_dark_changed(GObject *obj UNUSED, GParamSpec *pspec UNUSED, gpointer data UNUSED) {
    _glfwGtkRestoreContext();
    _glfwInputColorScheme(current_color_scheme(), false);
}

static gboolean
report_initial_color_scheme(gpointer data UNUSED) {
    _glfwGtkRestoreContext();
    _glfwInputColorScheme(current_color_scheme(), true);
    return G_SOURCE_REMOVE;
}

GLFWAPI GLFWColorScheme
glfwGetCurrentSystemColorTheme(bool query_if_unintialized UNUSED) {
    return current_color_scheme();
}

void
_glfwPlatformInputColorScheme(GLFWColorScheme appearance UNUSED) {}
// }}}

static void
read_keyboard_repeat_settings(void) {
    // GTK has no keyboard repeat setting, it comes from the compositor. Use
    // the GNOME defaults, kitty only uses these for its own key repeat
    // emulation in the absence of compositor repeat events.
    _glfw.gtk.key_repeat_delay_ms = 500;
    _glfw.gtk.key_repeat_interval_ms = 33;
    GSettingsSchemaSource *src = g_settings_schema_source_get_default();
    GSettingsSchema *schema = src ? g_settings_schema_source_lookup(src, "org.gnome.desktop.peripherals.keyboard", TRUE) : NULL;
    if (!schema) return;
    GSettings *s = g_settings_new("org.gnome.desktop.peripherals.keyboard");
    if (g_settings_schema_has_key(schema, "delay")) _glfw.gtk.key_repeat_delay_ms = (int)g_settings_get_uint(s, "delay");
    if (g_settings_schema_has_key(schema, "repeat-interval")) _glfw.gtk.key_repeat_interval_ms = (int)g_settings_get_uint(s, "repeat-interval");
    g_object_unref(s);
    g_settings_schema_unref(schema);
}

//////////////////////////////////////////////////////////////////////////
//////                       GLFW platform API                      //////
//////////////////////////////////////////////////////////////////////////

int
_glfwPlatformInit(bool *supports_window_occlusion) {
    *supports_window_occlusion = false;
    // kitty's own GLFW event loop is replaced by GTK's, and GDK must talk
    // Wayland, never X11.
    gdk_set_allowed_backends("wayland");
    if (!gtk_init_check()) {
        _glfwInputError(
            GLFW_PLATFORM_ERROR, "GTK: Failed to initialize GTK on Wayland. The GTK backend requires a Wayland compositor, is WAYLAND_DISPLAY set?");
        return false;
    }
    _glfw.gtk.display = gdk_display_get_default();
    if (!_glfw.gtk.display) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: No default display");
        return false;
    }
    adw_init();
    _glfw.gtk.style_manager = adw_style_manager_get_default();
    _glfw.gtk.dark_handler = g_signal_connect(_glfw.gtk.style_manager, "notify::dark", G_CALLBACK(on_dark_changed), NULL);
    _glfw.gtk.main_loop = g_main_loop_new(NULL, FALSE);
    read_keyboard_repeat_settings();
    if (!_glfwGtkGLInit()) return false;
    _glfwPollMonitorsGtk();
    _glfwGtkWatchMonitors();
    // Report the initial color scheme once the main loop runs, by then kitty
    // has set its callback. The DBus based desktop settings code of the other
    // Linux backends also reports it asynchronously.
    g_idle_add(report_initial_color_scheme, NULL);
    return true;
}

void
_glfwPlatformTerminate(void) {
    if (_glfw.gtk.monitors_model && _glfw.gtk.monitors_handler) g_signal_handler_disconnect(_glfw.gtk.monitors_model, _glfw.gtk.monitors_handler);
    _glfw.gtk.monitors_handler = 0;
    if (_glfw.gtk.style_manager && _glfw.gtk.dark_handler) g_signal_handler_disconnect(_glfw.gtk.style_manager, _glfw.gtk.dark_handler);
    _glfw.gtk.dark_handler = 0;
    if (_glfw.gtk.display) {
        if (_glfw.gtk.clipboard_handler) g_signal_handler_disconnect(gdk_display_get_clipboard(_glfw.gtk.display), _glfw.gtk.clipboard_handler);
        if (_glfw.gtk.primary_handler) g_signal_handler_disconnect(gdk_display_get_primary_clipboard(_glfw.gtk.display), _glfw.gtk.primary_handler);
    }
    _glfw.gtk.clipboard_handler = 0;
    _glfw.gtk.primary_handler = 0;
    _glfwGtkDBusTerminate();
    _glfwGtkGLTerminate();
    if (_glfw.gtk.main_loop) g_main_loop_unref(_glfw.gtk.main_loop);
    _glfw.gtk.main_loop = NULL;
    _glfw_free_clipboard_data(&_glfw.clipboard);
    _glfw_free_clipboard_data(&_glfw.primary);
}

const char *
_glfwPlatformGetVersionString(void) {
    return _GLFW_VERSION_NUMBER " GTK4 libadwaita";
}

// Main loop {{{

static gboolean
run_tick(gpointer data UNUSED) {
    if (_glfw.gtk.nested_loop_depth > 0) {
        // Never re-enter kitty while it is blocked waiting for us, for
        // example while reading the clipboard.
        _glfw.gtk.tick_deferred = true;
        atomic_store(&_glfw.gtk.tick_pending, false);
        return G_SOURCE_REMOVE;
    }
    // Clear the flag before the tick so that wakeups that happen during
    // the tick schedule another one.
    atomic_store(&_glfw.gtk.tick_pending, false);
    if (_glfw.gtk.tick_callback) {
        _glfwGtkRestoreContext();
        _glfw.gtk.tick_callback(_glfw.gtk.tick_callback_data);
    }
    return G_SOURCE_REMOVE;
}

void
_glfwPlatformPostEmptyEvent(void) {
    // Called from any thread. g_idle_add() is thread safe and wakes up the
    // main context.
    bool expected = false;
    if (atomic_compare_exchange_strong(&_glfw.gtk.tick_pending, &expected, true)) g_idle_add(run_tick, NULL);
}

void
_glfwGtkEndNestedLoop(void) {
    if (_glfw.gtk.nested_loop_depth > 0) _glfw.gtk.nested_loop_depth--;
    if (!_glfw.gtk.nested_loop_depth && _glfw.gtk.tick_deferred) {
        _glfw.gtk.tick_deferred = false;
        _glfwPlatformPostEmptyEvent();
    }
}

void
_glfwPlatformRunMainLoop(GLFWtickcallback tick, void *data) {
    _glfw.gtk.tick_callback = tick;
    _glfw.gtk.tick_callback_data = data;
    // Run an initial tick, like the other backends do after their first wakeup
    _glfwPlatformPostEmptyEvent();
    g_main_loop_run(_glfw.gtk.main_loop);
    _glfw.gtk.tick_callback = NULL;
    _glfw.gtk.tick_callback_data = NULL;
}

void
_glfwPlatformStopMainLoop(void) {
    if (_glfw.gtk.main_loop) g_main_loop_quit(_glfw.gtk.main_loop);
}

void
_glfwPlatformPollEvents(void) {
    _glfw.gtk.nested_loop_depth++;
    while (g_main_context_iteration(NULL, FALSE));
    _glfwGtkEndNestedLoop();
}

void
_glfwPlatformWaitEvents(void) {
    _glfw.gtk.nested_loop_depth++;
    g_main_context_iteration(NULL, TRUE);
    _glfwGtkEndNestedLoop();
}

void
_glfwPlatformWaitEventsTimeout(monotonic_t timeout UNUSED) {
    _glfwPlatformPollEvents();
}
// }}}

// Timers {{{

typedef struct GtkTimer {
    unsigned long long id;
    guint source_id;
    monotonic_t interval;
    bool repeats, enabled;
    GLFWuserdatafun callback, free_callback;
    void *callback_data;
} GtkTimer;

static struct {
    GtkTimer *items;
    size_t count, capacity;
    unsigned long long counter;
} timers;

static GtkTimer *
find_timer(unsigned long long id, size_t *idx) {
    for (size_t i = 0; i < timers.count; i++) {
        if (timers.items[i].id == id) {
            if (idx) *idx = i;
            return timers.items + i;
        }
    }
    return NULL;
}

static gboolean dispatch_timer(gpointer data);

static void
arm_timer(GtkTimer *t) {
    if (t->source_id) g_source_remove(t->source_id);
    t->source_id = 0;
    if (!t->enabled) return;
    gpointer data = (gpointer)(uintptr_t)t->id;
    if (t->interval <= 0) t->source_id = g_idle_add_full(G_PRIORITY_DEFAULT, dispatch_timer, data, NULL);
    else {
        // monotonic_t is in nanoseconds, round up to whole milliseconds
        guint ms = (guint)((t->interval + 999999) / 1000000);
        t->source_id = g_timeout_add_full(G_PRIORITY_DEFAULT, ms, dispatch_timer, data, NULL);
    }
}

static void
remove_timer_at(size_t idx) {
    GtkTimer t = timers.items[idx];
    if (t.source_id) g_source_remove(t.source_id);
    remove_i_from_array(timers.items, idx, timers.count);
    if (t.free_callback && t.callback_data) t.free_callback(t.id, t.callback_data);
}

static gboolean
dispatch_timer(gpointer data) {
    unsigned long long id = (uintptr_t)data;
    GtkTimer *t = find_timer(id, NULL);
    if (!t) return G_SOURCE_REMOVE;
    const guint source_id = t->source_id;
    GLFWuserdatafun callback = t->callback;
    void *callback_data = t->callback_data;
    _glfwGtkRestoreContext();
    callback(id, callback_data);
    // The callback can add, remove or update timers, so look it up again
    size_t idx;
    t = find_timer(id, &idx);
    if (!t || t->source_id != source_id) return G_SOURCE_REMOVE;
    if (!t->repeats) {
        t->source_id = 0; // this source is removed by returning G_SOURCE_REMOVE
        remove_timer_at(idx);
        return G_SOURCE_REMOVE;
    }
    return G_SOURCE_CONTINUE;
}

unsigned long long
_glfwPlatformAddTimer(monotonic_t interval, bool repeats, GLFWuserdatafun callback, void *callback_data, GLFWuserdatafun free_callback) {
    if (timers.count >= timers.capacity) {
        size_t cap = timers.capacity ? timers.capacity * 2 : 32;
        GtkTimer *n = realloc(timers.items, cap * sizeof(GtkTimer));
        if (!n) {
            _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Out of memory adding timer");
            return 0;
        }
        timers.items = n;
        timers.capacity = cap;
    }
    GtkTimer *t = timers.items + timers.count++;
    memset(t, 0, sizeof(*t));
    t->id = ++timers.counter;
    t->interval = interval;
    t->repeats = repeats;
    t->enabled = true;
    t->callback = callback;
    t->callback_data = callback_data;
    t->free_callback = free_callback;
    arm_timer(t);
    return t->id;
}

void
_glfwPlatformUpdateTimer(unsigned long long timer_id, monotonic_t interval, bool enabled) {
    GtkTimer *t = find_timer(timer_id, NULL);
    if (!t) return;
    bool changed = t->interval != interval || t->enabled != enabled;
    t->interval = interval;
    t->enabled = enabled;
    // Re-enabling an enabled timer restarts its interval, like the other
    // backends do.
    if (changed || enabled) arm_timer(t);
}

void
_glfwPlatformRemoveTimer(unsigned long long timer_id) {
    size_t idx;
    if (find_timer(timer_id, &idx)) remove_timer_at(idx);
}
// }}}

// GL context {{{

#define GL_TEXTURE_2D 0x0DE1
#define GL_TEXTURE_MIN_FILTER 0x2801
#define GL_TEXTURE_MAG_FILTER 0x2800
#define GL_NEAREST 0x2600
#define GL_LINEAR 0x2601
#define GL_RGBA8 0x8058
#define GL_SRGB8_ALPHA8 0x8C43
#define GL_FRAMEBUFFER 0x8D40
#define GL_COLOR_ATTACHMENT0 0x8CE0
#define GL_FRAMEBUFFER_COMPLETE 0x8CD5
#define GL_SYNC_GPU_COMMANDS_COMPLETE 0x9117
#define GL_FRAMEBUFFER_BINDING 0x8CA6
#define GL_TEXTURE_BINDING_2D 0x8069
#define GL_VIEWPORT 0x0BA2

typedef void(APIENTRY *PFN_glGenTextures)(int, GLuint *);
typedef void(APIENTRY *PFN_glDeleteTextures)(int, const GLuint *);
typedef void(APIENTRY *PFN_glBindTexture)(GLenum, GLuint);
typedef void(APIENTRY *PFN_glTexParameteri)(GLenum, GLenum, GLint);
typedef void(APIENTRY *PFN_glTexStorage2D)(GLenum, int, GLenum, int, int);
typedef void(APIENTRY *PFN_glTextureView)(GLuint, GLenum, GLuint, GLenum, GLuint, GLuint, GLuint, GLuint);
typedef void(APIENTRY *PFN_glGenFramebuffers)(int, GLuint *);
typedef void(APIENTRY *PFN_glDeleteFramebuffers)(int, const GLuint *);
typedef void(APIENTRY *PFN_glBindFramebuffer)(GLenum, GLuint);
typedef void(APIENTRY *PFN_glFramebufferTexture2D)(GLenum, GLenum, GLenum, GLuint, GLint);
typedef GLenum(APIENTRY *PFN_glCheckFramebufferStatus)(GLenum);
typedef void *(APIENTRY *PFN_glFenceSync)(GLenum, GLbitfield);
typedef void(APIENTRY *PFN_glDeleteSync)(void *);
typedef void(APIENTRY *PFN_glFlush)(void);
typedef void(APIENTRY *PFN_glViewport)(GLint, GLint, int, int);

static struct {
    PFN_glGenTextures GenTextures;
    PFN_glDeleteTextures DeleteTextures;
    PFN_glBindTexture BindTexture;
    PFN_glTexParameteri TexParameteri;
    PFN_glTexStorage2D TexStorage2D;
    PFN_glTextureView TextureView;
    PFN_glGenFramebuffers GenFramebuffers;
    PFN_glDeleteFramebuffers DeleteFramebuffers;
    PFN_glBindFramebuffer BindFramebuffer;
    PFN_glFramebufferTexture2D FramebufferTexture2D;
    PFN_glCheckFramebufferStatus CheckFramebufferStatus;
    PFN_glFenceSync FenceSync;
    PFN_glDeleteSync DeleteSync;
    PFN_glFlush Flush;
    PFN_glViewport Viewport;
    PFNGLGETINTEGERVPROC GetIntegerv;
    PFNGLGETSTRINGIPROC GetStringi;
    PFNGLGETSTRINGPROC GetString;
} gl;

// egl_context.h maps this to the dynamically loaded EGL, which this backend
// does not use, GDK has already loaded libEGL
#undef eglGetProcAddress
extern void *eglGetProcAddress(const char *);

static GLFWglproc
get_proc_address(const char *name) {
    return (GLFWglproc)eglGetProcAddress(name);
}

static int
extension_supported(const char *extension) {
    if (!gl.GetIntegerv || !gl.GetStringi) return false;
    GLint count = 0;
    gl.GetIntegerv(GL_NUM_EXTENSIONS, &count);
    for (GLint i = 0; i < count; i++) {
        const char *en = (const char *)gl.GetStringi(GL_EXTENSIONS, i);
        if (en && strcmp(en, extension) == 0) return true;
    }
    return false;
}

static const char *
api_name(GdkGLAPI api) {
    switch (api) {
        case GDK_GL_API_GL: return "GL";
        case GDK_GL_API_GLES: return "GLES";
        default: return "none";
    }
}

bool
_glfwGtkGLInit(void) {
    GError *err = NULL;
    GdkGLContext *ctx = gdk_display_create_gl_context(_glfw.gtk.display, &err);
    if (!ctx) {
        _glfwInputError(GLFW_API_UNAVAILABLE, "GTK: Failed to create GL context: %s", err ? err->message : "unknown error");
        g_clear_error(&err);
        return false;
    }
    gdk_gl_context_set_allowed_apis(ctx, GDK_GL_API_GL);
    gdk_gl_context_set_required_version(ctx, 3, 3);
    gdk_gl_context_set_forward_compatible(ctx, TRUE);
    if (!gdk_gl_context_realize(ctx, &err)) {
        _glfwInputError(
            GLFW_API_UNAVAILABLE,
            "GTK: Failed to realize a desktop OpenGL 3.3 context: %s. GDK may have chosen GLES for its shared context, try GDK_DISABLE=gles-api",
            err ? err->message : "unknown error");
        g_clear_error(&err);
        g_object_unref(ctx);
        return false;
    }
    _glfw.gtk.gl_context = ctx;
    gdk_gl_context_make_current(ctx);
    int major = 0, minor = 0;
    gdk_gl_context_get_version(ctx, &major, &minor);
#define L(name)                                                                                    \
    *(void **)&gl.name = (void *)get_proc_address("gl" #name);                                     \
    if (!gl.name) {                                                                                \
        _glfwInputError(GLFW_API_UNAVAILABLE, "GTK: Failed to load the GL function: gl%s", #name); \
        return false;                                                                              \
    }
    L(GenTextures);
    L(DeleteTextures);
    L(BindTexture);
    L(TexParameteri);
    L(TexStorage2D);
    L(GenFramebuffers);
    L(DeleteFramebuffers);
    L(BindFramebuffer);
    L(FramebufferTexture2D);
    L(CheckFramebufferStatus);
    L(FenceSync);
    L(DeleteSync);
    L(Flush);
    L(Viewport);
    L(GetIntegerv);
    L(GetStringi);
    L(GetString);
#undef L
    *(void **)&gl.TextureView = (void *)get_proc_address("glTextureView");
    _glfw.gtk.has_texture_view = gl.TextureView && ((major > 4 || (major == 4 && minor >= 3)) || extension_supported("GL_ARB_texture_view"));
    debug(
        "GTK: GL context API: %s version: %d.%d renderer: %s texture_view: %d shared_context_api: %s\n",
        api_name(gdk_gl_context_get_api(ctx)),
        major,
        minor,
        (const char *)gl.GetString(0x1F01),
        _glfw.gtk.has_texture_view,
        api_name(gdk_gl_context_get_api(ctx)));
    return true;
}

void
_glfwGtkGLTerminate(void) {
    if (_glfw.gtk.gl_context) {
        gdk_gl_context_clear_current();
        g_object_unref(_glfw.gtk.gl_context);
    }
    _glfw.gtk.gl_context = NULL;
}

void
_glfwGtkRestoreContext(void) {
    // GSK and GDK make their own GL contexts current while rendering, kitty
    // expects its context to stay current between calls into it.
    if (_glfw.gtk.gl_context && gdk_gl_context_get_current() != _glfw.gtk.gl_context) gdk_gl_context_make_current(_glfw.gtk.gl_context);
}

static void
free_buffer_gl(_GLFWgtkBuffer *b) {
    if (b->sync) gl.DeleteSync(b->sync);
    if (b->fbo) gl.DeleteFramebuffers(1, &b->fbo);
    if (b->view_texture) gl.DeleteTextures(1, &b->view_texture);
    if (b->texture) gl.DeleteTextures(1, &b->texture);
    b->sync = NULL;
    b->fbo = b->view_texture = b->texture = 0;
}

static _GLFWgtkBuffer *
create_buffer(int width, int height) {
    _GLFWgtkBuffer *b = calloc(1, sizeof(_GLFWgtkBuffer));
    if (!b) return NULL;
    b->width = width;
    b->height = height;
    GLint prev_tex = 0;
    gl.GetIntegerv(GL_TEXTURE_BINDING_2D, &prev_tex);
    gl.GenTextures(1, &b->texture);
    gl.BindTexture(GL_TEXTURE_2D, b->texture);
    // kitty enables GL_FRAMEBUFFER_SRGB when rendering its final output,
    // which only converts when the attachment is sRGB. GDK must sample the
    // raw sRGB encoded bytes, so it gets a non-sRGB view of the same storage.
    gl.TexStorage2D(GL_TEXTURE_2D, 1, _glfw.gtk.has_texture_view ? GL_SRGB8_ALPHA8 : GL_RGBA8, width, height);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    if (_glfw.gtk.has_texture_view) {
        gl.GenTextures(1, &b->view_texture);
        gl.TextureView(b->view_texture, GL_TEXTURE_2D, b->texture, GL_RGBA8, 0, 1, 0, 1);
        gl.BindTexture(GL_TEXTURE_2D, b->view_texture);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl.TexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    }
    gl.BindTexture(GL_TEXTURE_2D, prev_tex);
    gl.GenFramebuffers(1, &b->fbo);
    gl.BindFramebuffer(GL_FRAMEBUFFER, b->fbo);
    gl.FramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, b->texture, 0);
    GLenum status = gl.CheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        _glfwInputError(GLFW_PLATFORM_ERROR, "GTK: Offscreen framebuffer of size %dx%d is incomplete: 0x%x", width, height, status);
    }
    return b;
}

static void
fb_size(_GLFWwindow *window, int *w, int *h) {
    _glfwPlatformGetFramebufferSize(window, w, h);
    if (*w < 1) *w = 1;
    if (*h < 1) *h = 1;
}

// Returns a back buffer of the current framebuffer size that GDK is not using
static _GLFWgtkBuffer *
ensure_back_buffer(_GLFWwindow *window) {
    int w, h;
    fb_size(window, &w, &h);
    _GLFWgtkBuffer *b = window->gtk.back;
    if (b && !b->in_use_by_gdk && b->width == w && b->height == h) return b;
    window->gtk.back = NULL;
    // Drop free buffers of the wrong size and pick a free one of the right size
    for (size_t i = 0; i < window->gtk.num_buffers;) {
        _GLFWgtkBuffer *q = window->gtk.buffers[i];
        if (!q->in_use_by_gdk && (q->width != w || q->height != h)) {
            free_buffer_gl(q);
            free(q);
            remove_i_from_array(window->gtk.buffers, i, window->gtk.num_buffers);
            continue;
        }
        if (!q->in_use_by_gdk && !window->gtk.back) window->gtk.back = q;
        i++;
    }
    if (window->gtk.back) {
        if (window->gtk.back->sync) {
            gl.DeleteSync(window->gtk.back->sync);
            window->gtk.back->sync = NULL;
        }
        return window->gtk.back;
    }
    _GLFWgtkBuffer **n = realloc(window->gtk.buffers, sizeof(n[0]) * (window->gtk.num_buffers + 1));
    if (!n) return NULL;
    window->gtk.buffers = n;
    b = create_buffer(w, h);
    if (!b) return NULL;
    window->gtk.buffers[window->gtk.num_buffers++] = b;
    debug("GTK: window %llu now has %zu offscreen buffers, size: %dx%d\n", window->id, window->gtk.num_buffers, w, h);
    window->gtk.back = b;
    return b;
}

static void
bind_back_buffer(_GLFWwindow *window) {
    _GLFWgtkBuffer *b = ensure_back_buffer(window);
    gl.BindFramebuffer(GL_FRAMEBUFFER, b ? b->fbo : 0);
}

// All windows share one GL context, but kitty expects each window to have
// its own, as on the other backends: it only sets the viewport when a
// window's size changes. So switching windows swaps in their viewport.
static void
switch_viewport(_GLFWwindow *from, _GLFWwindow *to) {
    if (from) gl.GetIntegerv(GL_VIEWPORT, from->gtk.viewport);
    if (!to) return;
    if (!to->gtk.viewport[2] || !to->gtk.viewport[3]) {
        // A new context's viewport is the size of the surface it is first
        // made current with
        to->gtk.viewport[0] = to->gtk.viewport[1] = 0;
        fb_size(to, &to->gtk.viewport[2], &to->gtk.viewport[3]);
    }
    gl.Viewport(to->gtk.viewport[0], to->gtk.viewport[1], to->gtk.viewport[2], to->gtk.viewport[3]);
}

static void
make_context_current(_GLFWwindow *window) {
    _GLFWwindow *prev = _glfwPlatformGetTls(&_glfw.contextSlot);
    if (window || prev) _glfwGtkRestoreContext();
    if (prev != window) switch_viewport(prev, window);
    if (window) bind_back_buffer(window);
    _glfwPlatformSetTls(&_glfw.contextSlot, window);
}

GLFWAPI unsigned
glfwGetWindowDefaultFramebuffer(GLFWwindow *handle) {
    _GLFWwindow *window = (_GLFWwindow *)handle;
    if (!window) return 0;
    _GLFWgtkBuffer *b = ensure_back_buffer(window);
    return b ? b->fbo : 0;
}

static void
release_texture(gpointer data) {
    _GLFWgtkBuffer *b = data;
    b->in_use_by_gdk = false;
    if (b->orphaned) {
        GdkGLContext *prev = gdk_gl_context_get_current();
        if (prev) g_object_ref(prev);
        gdk_gl_context_make_current(_glfw.gtk.gl_context);
        free_buffer_gl(b);
        free(b);
        if (prev) {
            gdk_gl_context_make_current(prev);
            g_object_unref(prev);
        }
    }
}

void _glfwGtkPresentTexture(_GLFWwindow *window, GdkTexture *texture);

static void
swap_buffers(_GLFWwindow *window) {
    _GLFWgtkBuffer *b = window->gtk.back;
    if (!b) return;
    _glfwGtkRestoreContext();
    b->sync = gl.FenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
    gl.Flush();
    GdkGLTextureBuilder *builder = gdk_gl_texture_builder_new();
    gdk_gl_texture_builder_set_context(builder, _glfw.gtk.gl_context);
    gdk_gl_texture_builder_set_id(builder, b->view_texture ? b->view_texture : b->texture);
    gdk_gl_texture_builder_set_width(builder, b->width);
    gdk_gl_texture_builder_set_height(builder, b->height);
    gdk_gl_texture_builder_set_format(builder, window->gtk.transparent ? GDK_MEMORY_R8G8B8A8_PREMULTIPLIED : GDK_MEMORY_R8G8B8X8);
    gdk_gl_texture_builder_set_has_mipmap(builder, FALSE);
    gdk_gl_texture_builder_set_sync(builder, b->sync);
    b->in_use_by_gdk = true;
    GdkTexture *texture = gdk_gl_texture_builder_build(builder, release_texture, b);
    g_object_unref(builder);
    window->gtk.back = NULL;
    _glfwGtkPresentTexture(window, texture);
    g_object_unref(texture);
    // kitty keeps rendering to "the default framebuffer" after a swap
    if (_glfwPlatformGetTls(&_glfw.contextSlot) == window) bind_back_buffer(window);
}

void
_glfwGtkReleaseBuffer(_GLFWgtkBuffer *b) {
    // Called with our context current
    if (b->in_use_by_gdk) b->orphaned = true;
    else {
        free_buffer_gl(b);
        free(b);
    }
}

void
_glfwGtkDestroyBuffers(_GLFWwindow *window) {
    if (!window->gtk.num_buffers) return;
    _glfwGtkRestoreContext();
    for (size_t i = 0; i < window->gtk.num_buffers; i++) _glfwGtkReleaseBuffer(window->gtk.buffers[i]);
    free(window->gtk.buffers);
    window->gtk.buffers = NULL;
    window->gtk.num_buffers = 0;
    window->gtk.back = NULL;
}

static void
swap_interval(int interval UNUSED) {}

static void
destroy_context(_GLFWwindow *window) {
    _glfwGtkDestroyBuffers(window);
}

bool
_glfwGtkAttachContext(_GLFWwindow *window) {
    window->context.makeCurrent = make_context_current;
    window->context.swapBuffers = swap_buffers;
    window->context.swapInterval = swap_interval;
    window->context.extensionSupported = extension_supported;
    window->context.getProcAddress = get_proc_address;
    window->context.destroy = destroy_context;
    window->context.client = GLFW_OPENGL_API;
    window->context.source = GLFW_NATIVE_CONTEXT_API;
    return true;
}

EGLenum
_glfwPlatformGetEGLPlatform(EGLint **attribs UNUSED) {
    return 0;
}

EGLNativeDisplayType
_glfwPlatformGetEGLNativeDisplay(void) {
    return 0;
}

EGLNativeWindowType
_glfwPlatformGetEGLNativeWindow(_GLFWwindow *window UNUSED) {
    return 0;
}
// }}}

// Stubs for kitty's Linux/Wayland specific API {{{

GLFWAPI pid_t
glfwWaylandCompositorPID(void) {
    return -1;
}

GLFWAPI const char *
glfwWaylandMissingCapabilities(void) {
    return "";
}

GLFWAPI bool
glfwWaylandCreateVirtualDevices(void) {
    _glfwInputError(GLFW_FEATURE_UNAVAILABLE, "GTK: Virtual input devices are not supported");
    return false;
}

GLFWAPI void
glfwWaylandInjectMouseMotionAbsolute(unsigned int x UNUSED, unsigned int y UNUSED, unsigned int x_extent UNUSED, unsigned int y_extent UNUSED) {}

GLFWAPI void
glfwWaylandInjectMouseButton(int button UNUSED, int action UNUSED) {}

GLFWAPI void
glfwWaylandInjectKey(int key UNUSED, int action UNUSED, int mods UNUSED) {}

GLFWAPI bool
glfwIsLayerShellSupported(void) {
    return false;
}
// }}}

// Joysticks, gamma and Vulkan are not supported {{{

void
_glfwPlatformGetRequiredInstanceExtensions(char **extensions UNUSED) {}

int
_glfwPlatformGetPhysicalDevicePresentationSupport(VkInstance instance UNUSED, VkPhysicalDevice device UNUSED, uint32_t queuefamily UNUSED) {
    return false;
}

VkResult
_glfwPlatformCreateWindowSurface(
    VkInstance instance UNUSED, _GLFWwindow *window UNUSED, const VkAllocationCallbacks *allocator UNUSED, VkSurfaceKHR *surface UNUSED) {
    return VK_ERROR_EXTENSION_NOT_PRESENT;
}
// }}}
