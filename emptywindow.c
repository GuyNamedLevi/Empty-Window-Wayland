#define _POSIX_C_SOURCE 200112L
#include <errno.h>
#include <fcntl.h>
#include <stdbool.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>
#include "xdg-shell-client-protocol.h"
#include "xdg-decoration-unstable-v1.h"

/* Shared memory support code */
static void
randname(char *buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A'+(r&15)+(r&16)*2;
        r >>= 5;
    }
}

static int
create_shm_file(void)
{
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int
allocate_shm_file(size_t size)
{
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

/* Wayland code */
struct client_state {
    /* Globals */
    struct wl_display *wl_display;
    struct wl_registry *wl_registry;
    struct wl_shm *wl_shm;
    struct wl_compositor *wl_compositor;
    struct xdg_wm_base *xdg_wm_base;
    struct wl_seat *wl_seat;
    /* Objects */
    struct wl_surface *wl_surface;
    struct xdg_surface *xdg_surface;
    struct xdg_toplevel *xdg_toplevel;
    struct wl_keyboard *wl_keyboard;
    struct wl_pointer *wl_pointer;
    struct wl_touch *wl_touch;
    struct zxdg_toplevel_decoration_v1 *zxdg_toplevel_decoration_v1;
    struct zxdg_decoration_manager_v1 *zxdg_decoration_manager_v1;
    /* State */
    float fade;
    uint32_t last_frame;
    uint32_t bg;
    int width, height;
    bool closed;
    bool window_focused;
};


static void
wl_buffer_release(void *data, struct wl_buffer *wl_buffer)
{
    /* Sent by the compositor when it's no longer using this buffer */
    wl_buffer_destroy(wl_buffer);
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_release,
};

static const struct wl_callback_listener wl_surface_frame_listener;

static struct wl_buffer *
draw_frame(struct client_state *state)
{
    int width = state->width, height = state->height;
    int stride = width * 4;
    int size = stride * height;

    int fd = allocate_shm_file(size);
    if (fd == -1) {
        return NULL;
    }

    uint32_t *data = mmap(NULL, size,
            PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(state->wl_shm, fd, size);
    struct wl_buffer *buffer = wl_shm_pool_create_buffer(pool, 0,
            width, height, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    
    if (state->fade > 0) {
        int newAlpha = (int)(state->fade * 0xff);
        state->bg = newAlpha << 24 | 0x00000000 ;
        for (int y = 0; y < height; ++y) {
            for (int x = 0; x < width; ++x) {
                /* Color when active */
                data[y * width + x] = state->bg;
            }
        }   
    } else {
        memset(data, 0x00, width * height * 4);
    }

    munmap(data, size);
    wl_buffer_add_listener(buffer, &wl_buffer_listener, NULL);
    return buffer;
}


static void 
xdg_toplevel_configure(void *data, 
                       struct xdg_toplevel *xdg_toplevel, int32_t width, int32_t height,
                       struct wl_array *states)
{
    struct client_state *state = data;
    if (width == 0 || height == 0) {
        /* Compositor is deferring to us */
        return;
    }
    state->width = width;
    state->height = height;
    
    if (states->size != XDG_TOPLEVEL_STATE_ACTIVATED) {
        state->window_focused = true;
    } else {
        state->window_focused = false;
    }
    
    /* Request another frame */     
    struct wl_callback *cb = wl_surface_frame(state->wl_surface);
    wl_callback_add_listener(cb, &wl_surface_frame_listener, state);
}

static void 
xdg_toplevel_close(void *data, struct xdg_toplevel *toplevel)
{
    struct client_state *state = data;
    state->closed = true;
}

/* Pointer event functions */
static void
wl_pointer_enter(void *data, struct wl_pointer *wl_pointer,
               uint32_t serial, struct wl_surface *surface,
               wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct client_state *state = data;
    state->fade = 0.25;
}

static void
wl_pointer_leave(void *data, struct wl_pointer *wl_pointer,
               uint32_t serial, struct wl_surface *surface)
{
}

static void
wl_pointer_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time,
               wl_fixed_t surface_x, wl_fixed_t surface_y)
{
}

/* Close window at mouse button press */
static void 
wl_pointer_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial,
                  uint32_t time, uint32_t button, uint32_t state) 
{
    struct client_state *client_state = data;
    client_state->closed = true;
}

static void
wl_pointer_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time,
               uint32_t axis, wl_fixed_t value)
{
}

static void
wl_pointer_axis_source(void *data, struct wl_pointer *wl_pointer,
               uint32_t axis_source)
{
}

static void
wl_pointer_axis_stop(void *data, struct wl_pointer *wl_pointer,
               uint32_t time, uint32_t axis)
{
}

static void
wl_pointer_axis_discrete(void *data, struct wl_pointer *wl_pointer,
               uint32_t axis, int32_t discrete)
{
}

static void
wl_pointer_frame(void *data, struct wl_pointer *wl_pointer)
{
       /* Nothing to do */
}

/* Keyboard event functions */
static void
wl_keyboard_keymap(void *data, struct wl_keyboard *wl_keyboard,
               uint32_t format, int32_t fd, uint32_t size)
{
}

static void
wl_keyboard_enter(void *data, struct wl_keyboard *wl_keyboard,
               uint32_t serial, struct wl_surface *surface,
               struct wl_array *keys)
{
}

static void
wl_keyboard_key(void *data, struct wl_keyboard *wl_keyboard,
               uint32_t serial, uint32_t time, uint32_t key, uint32_t state)
{
    struct client_state *client_state = data;
    if (key == 1) {
        client_state->closed = true;
    }
    
}
static void
wl_keyboard_leave(void *data, struct wl_keyboard *wl_keyboard,
               uint32_t serial, struct wl_surface *surface)
{
}

static void
wl_keyboard_modifiers(void *data, struct wl_keyboard *wl_keyboard,
               uint32_t serial, uint32_t mods_depressed,
               uint32_t mods_latched, uint32_t mods_locked,
               uint32_t group)
{
}

static void
wl_keyboard_repeat_info(void *data, struct wl_keyboard *wl_keyboard,
               int32_t rate, int32_t delay)
{
}

static const struct xdg_toplevel_listener xdg_toplevel_listener = {
    .configure = xdg_toplevel_configure, 
    .close = xdg_toplevel_close,
};

static void
xdg_surface_configure(void *data,
        struct xdg_surface *xdg_surface, uint32_t serial)
{
    struct client_state *state = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_commit(state->wl_surface);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void
xdg_wm_base_ping(void *data, struct xdg_wm_base *xdg_wm_base, uint32_t serial)
{
    xdg_wm_base_pong(xdg_wm_base, serial);
}

static const struct xdg_wm_base_listener xdg_wm_base_listener = {
    .ping = xdg_wm_base_ping,
};

static void 
wl_surface_frame_done(void *data, struct wl_callback *cb, uint32_t time) {
    /* Destroy this callback */
    wl_callback_destroy(cb);

    /* Request another frame */ 
    struct client_state *state = data;

    if (state->fade > 0) {
        if (state->fade == 0.25) { state->last_frame = time -1; }
        int elapsed = time - state->last_frame;
        state->fade -= elapsed * 0.002;
        struct wl_callback *cb = wl_surface_frame(state->wl_surface);
        wl_callback_add_listener(cb, &wl_surface_frame_listener, state);
        state->last_frame = time;
    }
    /* Submit a frame for this event */
    struct wl_buffer *buffer = draw_frame(state);
    wl_surface_attach(state->wl_surface, buffer, 0, 0);
    wl_surface_damage_buffer(state->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(state->wl_surface);
    
}

static const struct wl_callback_listener wl_surface_frame_listener = {
    .done = wl_surface_frame_done,
};

static const struct wl_pointer_listener wl_pointer_listener = {
    .enter = wl_pointer_enter,
    .leave = wl_pointer_leave,
    .motion = wl_pointer_motion,
    .button = wl_pointer_button,
    .axis = wl_pointer_axis,
    .frame = wl_pointer_frame,
    .axis_source = wl_pointer_axis_source,
    .axis_stop = wl_pointer_axis_stop,
    .axis_discrete = wl_pointer_axis_discrete,
};

static const struct wl_keyboard_listener wl_keyboard_listener = {
    .keymap = wl_keyboard_keymap,
    .enter = wl_keyboard_enter,
    .leave = wl_keyboard_leave,
    .key = wl_keyboard_key,
    .modifiers = wl_keyboard_modifiers,
    .repeat_info = wl_keyboard_repeat_info,
};

static void
wl_seat_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities)
{
    struct client_state *state = data;
    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (have_pointer && state->wl_pointer == NULL) {
        state->wl_pointer = wl_seat_get_pointer(state->wl_seat);
        wl_pointer_add_listener(state->wl_pointer,
                                &wl_pointer_listener, state);
    } else if (!have_pointer && state->wl_pointer != NULL) {
        wl_pointer_release(state->wl_pointer);
        state->wl_pointer = NULL;
    }

    bool have_keyboard = capabilities & WL_SEAT_CAPABILITY_KEYBOARD;

    if (have_keyboard && state->wl_keyboard == NULL) {
        state->wl_keyboard = wl_seat_get_keyboard(state->wl_seat);
        wl_keyboard_add_listener(state->wl_keyboard, 
                                &wl_keyboard_listener, state);
    } else if (!have_keyboard && state->wl_keyboard != NULL) {
        wl_keyboard_release(state->wl_keyboard);
        state->wl_keyboard = NULL;
    }
}

static void 
wl_seat_name(void *data, struct wl_seat *wl_seat, const char *name)
{
    /* TODO */
}

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_capabilities,
    .name = wl_seat_name,
};

static void
registry_global(void *data, struct wl_registry *wl_registry,
        uint32_t name, const char *interface, uint32_t version)
{
    struct client_state *state = data;
    if (strcmp(interface, wl_shm_interface.name) == 0) {
        state->wl_shm = wl_registry_bind(
                wl_registry, name, &wl_shm_interface, 1);
    } else if (strcmp(interface, wl_compositor_interface.name) == 0) {
        state->wl_compositor = wl_registry_bind(
                wl_registry, name, &wl_compositor_interface, 4);
    } else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        state->xdg_wm_base = wl_registry_bind(
                wl_registry, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(state->xdg_wm_base,
                &xdg_wm_base_listener, state);
    } else if (strcmp(interface, wl_seat_interface.name) == 0) {
        state->wl_seat = wl_registry_bind(
            wl_registry, name, &wl_seat_interface, 7);
        wl_seat_add_listener(state->wl_seat,
                             &wl_seat_listener, state);
    } else if (strcmp(interface, zxdg_decoration_manager_v1_interface.name) == 0) {
        state->zxdg_decoration_manager_v1 = wl_registry_bind(
                wl_registry, name, &zxdg_decoration_manager_v1_interface, 1);
    }
}

static void
registry_global_remove(void *data,
        struct wl_registry *wl_registry, uint32_t name)
{
    /* This space deliberately left blank */
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = registry_global,
    .global_remove = registry_global_remove,
};

int
main(int argc, char *argv[])
{
    struct client_state state = { 0 };
    state.width = 640;
    state.height = 480;
    state.wl_display = wl_display_connect(NULL);
    state.wl_registry = wl_display_get_registry(state.wl_display);
    wl_registry_add_listener(state.wl_registry, &wl_registry_listener, &state);
    wl_display_roundtrip(state.wl_display);

    state.wl_surface = wl_compositor_create_surface(state.wl_compositor);
    state.xdg_surface = xdg_wm_base_get_xdg_surface(
            state.xdg_wm_base, state.wl_surface);
    xdg_surface_add_listener(state.xdg_surface, &xdg_surface_listener, &state);
    state.xdg_toplevel = xdg_surface_get_toplevel(state.xdg_surface);
    xdg_toplevel_add_listener(state.xdg_toplevel,
                              &xdg_toplevel_listener, &state);
    xdg_toplevel_set_title(state.xdg_toplevel, " ");
    xdg_toplevel_set_app_id(state.xdg_toplevel, "emptywindow");
    state.zxdg_toplevel_decoration_v1 = zxdg_decoration_manager_v1_get_toplevel_decoration(state.zxdg_decoration_manager_v1, state.xdg_toplevel);
    zxdg_toplevel_decoration_v1_set_mode(state.zxdg_toplevel_decoration_v1, 1);
    wl_surface_commit(state.wl_surface);

    struct wl_callback *cb = wl_surface_frame(state.wl_surface);
    wl_callback_add_listener(cb, &wl_surface_frame_listener, &state);

    while ((wl_display_dispatch(state.wl_display)) && (state.closed == false)) {
        /* This space deliberately left blank */
    }
    
    wl_display_disconnect(state.wl_display);
    return 0;
}

