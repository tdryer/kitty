//========================================================================
// GLFW 3.4 GTK 4 / libadwaita - www.glfw.org
//------------------------------------------------------------------------
// Copyright (c) 2016 Google Inc.
// Copyright (c) 2016-2019 Camilla Löwy <elmindreda@glfw.org>
// Copyright (c) 2026 Tom Dryer <tomdryer.com@gmail.com>
//
// This backend runs kitty inside a GTK 4 / libadwaita window on Wayland.
// GTK owns the event loop and the toplevel surfaces. kitty renders with a
// single GL context, shared by all windows, into per window offscreen
// textures that are handed to GTK as GdkGLTextures.
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

#include <dlfcn.h>
#include <stdatomic.h>

#define GDK_VERSION_MIN_REQUIRED GDK_VERSION_4_22
#define GDK_VERSION_MAX_ALLOWED GDK_VERSION_4_22
#define ADW_VERSION_MIN_REQUIRED ADW_VERSION_1_9
#define ADW_VERSION_MAX_ALLOWED ADW_VERSION_1_9
#include <gtk/gtk.h>
#include <adwaita.h>

#define _GLFW_PLATFORM_WINDOW_STATE _GLFWwindowGtk gtk
#define _GLFW_PLATFORM_LIBRARY_WINDOW_STATE _GLFWlibraryGtk gtk
#define _GLFW_PLATFORM_MONITOR_STATE _GLFWmonitorGtk gtk
#define _GLFW_PLATFORM_CURSOR_STATE _GLFWcursorGtk gtk

#define _GLFW_PLATFORM_CONTEXT_STATE
#define _GLFW_PLATFORM_LIBRARY_CONTEXT_STATE

#include "posix_thread.h"
#include "null_joystick.h"

#define _glfw_dlopen(name) dlopen(name, RTLD_LAZY | RTLD_LOCAL)
#define _glfw_dlclose(handle) dlclose(handle)
#define _glfw_dlsym(handle, name) dlsym(handle, name)

typedef void (*GLFWwaylandframecallbackfunc)(unsigned long long id);
typedef void (*GLFWDBusnotificationcreatedfun)(unsigned long long, uint32_t, void *);
typedef void (*GLFWDBusnotificationactivatedfun)(uint32_t, int, const char *);
typedef void (*GLFWDBussessionstatefun)(uint32_t);
typedef void (*GLFWgtkmenuactionfun)(GLFWwindow *, const char *);
typedef struct GLFWgtkmenukey {
    // A GLFW key, or a keysym when native_key is set, mods are GLFW_MOD_* flags
    uint32_t key;
    int mods;
    bool native_key;
} GLFWgtkmenukey;
typedef struct GLFWgtkmenuitem {
    // A NULL label starts a new section
    const char *label, *action;
    // The shortcut shown for the item, a sequence of num_keys keys
    const GLFWgtkmenukey *keys;
    size_t num_keys;
    // An inline item is a button in a row with the adjacent inline items. It
    // shows icon if set and the icon theme has it, else label. Its tooltip is
    // tooltip if set, else label.
    bool is_inline;
    const char *icon, *tooltip;
} GLFWgtkmenuitem;
typedef enum GLFWgtktitlebarmenu {
    // A split button at the start of the title bar, which runs the first item's
    // action and shows its icon, with a menu of all the items
    GLFW_GTK_TITLEBAR_MENU_START,
    // A menu button at the end of the title bar
    GLFW_GTK_TITLEBAR_MENU_END,
    GLFW_GTK_TITLEBAR_MENU_COUNT
} GLFWgtktitlebarmenu;

// An offscreen color buffer kitty renders into. It is heap allocated because
// GDK can hold on to the texture wrapping it after the window is gone.
typedef struct _GLFWgtkBuffer {
    unsigned int texture, view_texture, fbo;
    int width, height;
    // GDK has a GdkTexture for this buffer that it has not released yet
    bool in_use_by_gdk;
    // The window was destroyed while GDK still used the buffer
    bool orphaned;
    void *sync;
} _GLFWgtkBuffer;

typedef struct _GLFWwindowGtk {
    GtkWidget *window, *toolbar_view, *header_bar, *view, *menu_buttons[GLFW_GTK_TITLEBAR_MENU_COUNT];
    GLFWgtkmenuactionfun menu_callback;
    GtkIMContext *im_context, *compose_context;
    GtkCssProvider *titlebar_css;

    // Size of the content area in logical pixels
    int width, height;
    double scale;
    bool visible, mapped, fully_created;
    bool maximized, fullscreen, focused, hovered;
    bool transparent, titlebar_hidden;
    float opacity;
    double cursor_x, cursor_y;
    _GLFWcursor *current_cursor;
    char *title;

    _GLFWgtkBuffer **buffers;
    size_t num_buffers;
    _GLFWgtkBuffer *back;
    // The GL viewport kitty last set for this window, see make_context_current()
    int viewport[4];
    GdkTexture *texture;

    struct {
        unsigned long long id;
        GLFWwaylandframecallbackfunc callback;
        guint tick_id;
    } frame;

    struct {
        bool in_key_press, composing;
        char *commit;
    } ime;

    GdkSurface *surface;
    gulong scale_handler;
} _GLFWwindowGtk;

typedef struct _GLFWmonitorGtk {
    GdkMonitor *monitor;
    double scale;
    int x, y, width, height;
} _GLFWmonitorGtk;

typedef struct _GLFWcursorGtk {
    GdkCursor *cursor;
} _GLFWcursorGtk;

typedef struct _GLFWlibraryGtk {
    GdkDisplay *display;
    GdkGLContext *gl_context;
    GMainLoop *main_loop;
    AdwStyleManager *style_manager;
    GListModel *monitors_model;
    gulong monitors_handler, dark_handler, clipboard_handler, primary_handler;

    GLFWtickcallback tick_callback;
    void *tick_callback_data;
    atomic_bool tick_pending;
    // > 0 while spinning a nested main loop, ticks are deferred then
    int nested_loop_depth;
    bool tick_deferred;

    bool has_texture_view;
    int key_repeat_delay_ms, key_repeat_interval_ms;
} _GLFWlibraryGtk;

GLFWAPI double glfwGetWaylandCurrentMonitorFractionalScale(void);
void _glfwPollMonitorsGtk(void);
void _glfwGtkWatchMonitors(void);
void _glfwGtkEndNestedLoop(void);
void _glfwGtkPresentTexture(_GLFWwindow *window, GdkTexture *texture);
void _glfwGtkRestoreContext(void);
void _glfwGtkDBusTerminate(void);
void _glfwGtkDestroyBuffers(_GLFWwindow *window);
bool _glfwGtkGLInit(void);
void _glfwGtkGLTerminate(void);
bool _glfwGtkAttachContext(_GLFWwindow *window);
void _glfwGtkReleaseBuffer(_GLFWgtkBuffer *b);

// Keyboard, see gtk_keys.c
uint32_t _glfwGtkKeyForSym(uint32_t keysym);
uint32_t _glfwGtkSymForKey(uint32_t key);
int _glfwGtkModsFromState(GdkModifierType state, GdkDevice *device);
void _glfwGtkHandleKeyEvent(_GLFWwindow *window, GdkEvent *event, bool is_press);
void _glfwGtkSetupIME(_GLFWwindow *window);
void _glfwGtkDestroyIME(_GLFWwindow *window);
