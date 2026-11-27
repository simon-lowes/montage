/* A reproducer for a Weston fault: a mapped surface whose image description changes (color-management-v1) is left out
 * of every repaint until Weston next rebuilds its list of views, so the screen goes on showing what was there before
 * (the surface's old picture, or what is beneath it) instead of its new picture.
 *
 * Weston 16.0.2, its GL renderer, colour management on. When a commit changes a surface's image description,
 * weston_surface_set_color_profile() (libweston/compositor.c) finalises each of the surface's paint nodes' colour
 * transformations and marks them not valid (surf_xform_valid = false). A transformation is made again only by
 * weston_paint_node_ensure_color_transform(), which is called from add_to_z_order_list() while the output's list of
 * paint nodes is rebuilt; a commit that only changes the description does not ask for that, and the GL renderer skips
 * a paint node whose transformation is not valid (gl_shader_config_init_for_paint_node() returns false). The surface
 * stays out of the picture until something else rebuilds the views: here, attaching no buffer and then a buffer
 * again (unmapping and mapping it) brings it back at once, which is what Montage does (app/WaylandColor's remap).
 * Read in Weston 16.0.2's source (and seen in Montage's CI under it, Phase 17).
 *
 * What this client does, under a Weston with colour management on (Montage's CI drives its output as BT.2100 PQ):
 *   1. a fullscreen surface described as PQ with BT.2020 primaries and a MaxCLL of 1000, a dark grey buffer: shown;
 *   2. the same surface described anew (MaxCLL 400) with a new buffer of a light grey, committed together: the
 *      surface is not drawn ("skipped repaint: shader config failure" in the gl-renderer-paint-nodes log scope), so
 *      the screen keeps what it showed before (step 1's dark grey, left in the output's shadow framebuffer; black
 *      where nothing was drawn before), and still a second later;
 *   3. no buffer, then the light grey buffer again: shown.
 * (The two greys differ so that the screenshots tell a surface not drawn from one drawn: with one grey for both, as
 * the first version of this file had, the picture left from step 1 looked the same as the new one.)
 * After each step it waits for the frame to be shown and runs the command in $SCREENSHOOTER from the directory
 * step-1, step-2, step-2-later and step-3 under the directory it is given; scripts/hdr-reproducers.sh then reads each
 * screenshot's middle.
 *
 *   weston-dropped-transformation <directory>   (WAYLAND_DISPLAY set; built by scripts/hdr-reproducers.sh)
 *
 * Exit status: 0 when every step ran (what the screenshots show is judged by the script), 2 when it could not run (no
 * compositor, no colour management, a description refused).
 *
 * Part of Montage's tests (GPL-3.0-or-later); written for Weston's issue tracker, free to send there as it is. */
#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#include <wayland-client.h>

#include "color-management-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

struct App {
    struct wl_display* display;
    struct wl_compositor* compositor;
    struct wl_shm* shm;
    struct xdg_wm_base* wm;
    struct wp_color_manager_v1* manager;
    int pq, bt2020;
    int managerDone;
    struct wl_surface* surface;
    struct xdg_surface* xdgSurface;
    struct xdg_toplevel* toplevel;
    int width, height;
    int configured;
    int frameDone;
    int descriptionReady, descriptionFailed;
};

static double now(void) {
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (double)t.tv_sec + (double)t.tv_nsec / 1e9;
}

/* Dispatches the display's events until *flag is set or `seconds` have gone; 1 when it was set. */
static int waitFor(struct App* app, const int* flag, double seconds) {
    const double end = now() + seconds;
    while (!*flag) {
        struct pollfd p;
        const double left = end - now();
        if (left <= 0) return 0;
        while (wl_display_prepare_read(app->display) != 0) wl_display_dispatch_pending(app->display);
        if (wl_display_flush(app->display) < 0 && errno != EAGAIN) {
            wl_display_cancel_read(app->display);
            return 0;
        }
        p.fd = wl_display_get_fd(app->display);
        p.events = POLLIN;
        p.revents = 0;
        if (poll(&p, 1, (int)(left * 1000) + 1) > 0) {
            if (wl_display_read_events(app->display) < 0) return 0;
        } else {
            wl_display_cancel_read(app->display);
        }
        if (wl_display_dispatch_pending(app->display) < 0) return 0;
    }
    return 1;
}

static void pause_for(struct App* app, double seconds) {
    const int never = 0;
    waitFor(app, &never, seconds);
}

static void wmPing(void* data, struct xdg_wm_base* wm, uint32_t serial) {
    (void)data;
    xdg_wm_base_pong(wm, serial);
}
static const struct xdg_wm_base_listener kWm = {wmPing};

static void managerIntent(void* data, struct wp_color_manager_v1* m, uint32_t v) { (void)data, (void)m, (void)v; }
static void managerFeature(void* data, struct wp_color_manager_v1* m, uint32_t v) { (void)data, (void)m, (void)v; }
static void managerTf(void* data, struct wp_color_manager_v1* m, uint32_t v) {
    (void)m;
    if (v == WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ) ((struct App*)data)->pq = 1;
}
static void managerPrimaries(void* data, struct wp_color_manager_v1* m, uint32_t v) {
    (void)m;
    if (v == WP_COLOR_MANAGER_V1_PRIMARIES_BT2020) ((struct App*)data)->bt2020 = 1;
}
static void managerDone(void* data, struct wp_color_manager_v1* m) {
    (void)m;
    ((struct App*)data)->managerDone = 1;
}
static const struct wp_color_manager_v1_listener kManager = {managerIntent, managerFeature, managerTf, managerPrimaries, managerDone};

static void global(void* data, struct wl_registry* r, uint32_t name, const char* interface, uint32_t version) {
    struct App* app = data;
    if (strcmp(interface, wl_compositor_interface.name) == 0)
        app->compositor = wl_registry_bind(r, name, &wl_compositor_interface, version < 4 ? version : 4);
    else if (strcmp(interface, wl_shm_interface.name) == 0)
        app->shm = wl_registry_bind(r, name, &wl_shm_interface, 1);
    else if (strcmp(interface, xdg_wm_base_interface.name) == 0) {
        app->wm = wl_registry_bind(r, name, &xdg_wm_base_interface, 1);
        xdg_wm_base_add_listener(app->wm, &kWm, app);
    } else if (strcmp(interface, wp_color_manager_v1_interface.name) == 0) {
        app->manager = wl_registry_bind(r, name, &wp_color_manager_v1_interface, 1);
        wp_color_manager_v1_add_listener(app->manager, &kManager, app);
    }
}
static void globalGone(void* data, struct wl_registry* r, uint32_t name) { (void)data, (void)r, (void)name; }
static const struct wl_registry_listener kRegistry = {global, globalGone};

static void surfaceConfigure(void* data, struct xdg_surface* s, uint32_t serial) {
    xdg_surface_ack_configure(s, serial);
    ((struct App*)data)->configured = 1;
}
static const struct xdg_surface_listener kXdgSurface = {surfaceConfigure};

static void toplevelConfigure(void* data, struct xdg_toplevel* t, int32_t w, int32_t h, struct wl_array* states) {
    struct App* app = data;
    (void)t, (void)states;
    if (w > 0 && h > 0) app->width = w, app->height = h;
}
static void toplevelClose(void* data, struct xdg_toplevel* t) { (void)data, (void)t; }
static void toplevelBounds(void* data, struct xdg_toplevel* t, int32_t w, int32_t h) { (void)data, (void)t, (void)w, (void)h; }
static void toplevelCapabilities(void* data, struct xdg_toplevel* t, struct wl_array* c) { (void)data, (void)t, (void)c; }
static const struct xdg_toplevel_listener kToplevel = {toplevelConfigure, toplevelClose, toplevelBounds, toplevelCapabilities};

static void frameDone(void* data, struct wl_callback* cb, uint32_t time) {
    (void)time;
    ((struct App*)data)->frameDone = 1;
    wl_callback_destroy(cb);
}
static const struct wl_callback_listener kFrame = {frameDone};

static void descriptionFailed(void* data, struct wp_image_description_v1* d, uint32_t cause, const char* msg) {
    (void)d;
    fprintf(stderr, "The image description is refused (%u): %s\n", cause, msg ? msg : "");
    ((struct App*)data)->descriptionFailed = 1;
}
static void descriptionReady(void* data, struct wp_image_description_v1* d, uint32_t identity) {
    (void)d, (void)identity;
    ((struct App*)data)->descriptionReady = 1;
}
static const struct wp_image_description_v1_listener kDescription = {descriptionFailed, descriptionReady};

/* A buffer of one grey, XRGB8888, at the surface's size. */
static struct wl_buffer* greyBuffer(struct App* app, uint8_t grey) {
    const int stride = app->width * 4;
    const size_t size = (size_t)stride * (size_t)app->height;
    struct wl_shm_pool* pool;
    struct wl_buffer* buffer;
    uint32_t* px;
    size_t i;
    const int fd = memfd_create("weston-dropped-transformation", MFD_CLOEXEC);
    if (fd < 0 || ftruncate(fd, (off_t)size) < 0) return NULL;
    px = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (px == MAP_FAILED) {
        close(fd);
        return NULL;
    }
    for (i = 0; i < size / 4; ++i) px[i] = 0xff000000u | (uint32_t)grey << 16 | (uint32_t)grey << 8 | grey;
    munmap(px, size);
    pool = wl_shm_create_pool(app->shm, fd, (int32_t)size);
    buffer = wl_shm_pool_create_buffer(pool, 0, app->width, app->height, stride, WL_SHM_FORMAT_XRGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);
    return buffer;
}

/* PQ with BT.2020 primaries and the MaxCLL given, as a parametric description; NULL when refused. */
static struct wp_image_description_v1* describe(struct App* app, uint32_t maxCll) {
    struct wp_image_description_creator_params_v1* c = wp_color_manager_v1_create_parametric_creator(app->manager);
    struct wp_image_description_v1* d;
    wp_image_description_creator_params_v1_set_tf_named(c, WP_COLOR_MANAGER_V1_TRANSFER_FUNCTION_ST2084_PQ);
    wp_image_description_creator_params_v1_set_primaries_named(c, WP_COLOR_MANAGER_V1_PRIMARIES_BT2020);
    wp_image_description_creator_params_v1_set_max_cll(c, maxCll);
    d = wp_image_description_creator_params_v1_create(c);  /* (the creator goes with it) */
    app->descriptionReady = app->descriptionFailed = 0;
    wp_image_description_v1_add_listener(d, &kDescription, app);
    if (!waitFor(app, &app->descriptionReady, 5) || app->descriptionFailed) {
        wp_image_description_v1_destroy(d);
        return NULL;
    }
    return d;
}

/* Commits what has been set with a frame callback, waits for the frame (or a second), then half a second more. */
static void commitAndWait(struct App* app) {
    struct wl_callback* cb = wl_surface_frame(app->surface);
    app->frameDone = 0;
    wl_callback_add_listener(cb, &kFrame, app);
    wl_surface_commit(app->surface);
    if (!waitFor(app, &app->frameDone, 1)) printf("(no frame callback within a second)\n");
    pause_for(app, 0.5);
}

/* Runs $SCREENSHOOTER from <dir>/<step>; 1 when it ran and succeeded. */
static int screenshot(const char* dir, const char* step) {
    char path[4096], command[8192];
    const char* shooter = getenv("SCREENSHOOTER");
    if (!shooter || !*shooter) return 0;
    snprintf(path, sizeof path, "%s/%s", dir, step);
    mkdir(path, 0755);
    snprintf(command, sizeof command, "cd '%s' && '%s'", path, shooter);
    fflush(stdout);
    return system(command) == 0;
}

int main(int argc, char** argv) {
    struct App app;
    struct wl_registry* registry;
    struct wp_color_management_surface_v1* cm;
    struct wp_image_description_v1 *first, *second;
    struct wl_buffer *a, *b;
    int ok = 1;
    if (argc < 2) {
        fprintf(stderr, "usage: %s <directory for the screenshots>\n", argv[0]);
        return 2;
    }
    memset(&app, 0, sizeof app);
    if (!(app.display = wl_display_connect(NULL))) {
        fprintf(stderr, "No Wayland compositor\n");
        return 2;
    }
    registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(registry, &kRegistry, &app);
    wl_display_roundtrip(app.display);
    if (!app.compositor || !app.shm || !app.wm || !app.manager) {
        fprintf(stderr, "The compositor lacks wl_compositor, wl_shm, xdg_wm_base or wp_color_manager_v1\n");
        return 2;
    }
    if (!waitFor(&app, &app.managerDone, 5) || !app.pq || !app.bt2020) {
        fprintf(stderr, "The compositor's colour management has no PQ with BT.2020 primaries\n");
        return 2;
    }
    app.surface = wl_compositor_create_surface(app.compositor);
    app.xdgSurface = xdg_wm_base_get_xdg_surface(app.wm, app.surface);
    xdg_surface_add_listener(app.xdgSurface, &kXdgSurface, &app);
    app.toplevel = xdg_surface_get_toplevel(app.xdgSurface);
    xdg_toplevel_add_listener(app.toplevel, &kToplevel, &app);
    xdg_toplevel_set_title(app.toplevel, "Weston's dropped transformation");
    xdg_toplevel_set_fullscreen(app.toplevel, NULL);
    app.width = 640, app.height = 480;
    wl_surface_commit(app.surface);
    if (!waitFor(&app, &app.configured, 5)) {
        fprintf(stderr, "The surface was not configured\n");
        return 2;
    }
    cm = wp_color_manager_v1_get_surface(app.manager, app.surface);
    if (!(first = describe(&app, 1000)) || !(second = describe(&app, 400))) return 2;
    if (!(a = greyBuffer(&app, 0x40)) || !(b = greyBuffer(&app, 0xc0))) {
        fprintf(stderr, "No shared memory buffer\n");
        return 2;
    }
    printf("Surface %dx%d\n", app.width, app.height);

    /* 1. Described and shown. */
    wp_color_management_surface_v1_set_image_description(cm, first, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wl_surface_attach(app.surface, a, 0, 0);
    wl_surface_damage_buffer(app.surface, 0, 0, INT32_MAX, INT32_MAX);
    commitAndWait(&app);
    ok = screenshot(argv[1], "step-1") && ok;

    /* 2. Described anew, with a new buffer (another grey) in the same commit. */
    wp_color_management_surface_v1_set_image_description(cm, second, WP_COLOR_MANAGER_V1_RENDER_INTENT_PERCEPTUAL);
    wl_surface_attach(app.surface, b, 0, 0);
    wl_surface_damage_buffer(app.surface, 0, 0, INT32_MAX, INT32_MAX);
    commitAndWait(&app);
    ok = screenshot(argv[1], "step-2") && ok;
    /* (a frame later, and a second later, still: nothing rebuilt the views) */
    wl_surface_damage_buffer(app.surface, 0, 0, INT32_MAX, INT32_MAX);
    commitAndWait(&app);
    pause_for(&app, 1.0);
    ok = screenshot(argv[1], "step-2-later") && ok;

    /* 3. No buffer, then the new buffer again: mapped anew, with a transformation made for its new view. */
    wl_surface_attach(app.surface, NULL, 0, 0);
    wl_surface_commit(app.surface);
    wl_display_roundtrip(app.display);
    wl_surface_attach(app.surface, b, 0, 0);
    wl_surface_damage_buffer(app.surface, 0, 0, INT32_MAX, INT32_MAX);
    commitAndWait(&app);
    ok = screenshot(argv[1], "step-3") && ok;

    wp_image_description_v1_destroy(first);
    wp_image_description_v1_destroy(second);
    wp_color_management_surface_v1_destroy(cm);
    xdg_toplevel_destroy(app.toplevel);
    xdg_surface_destroy(app.xdgSurface);
    wl_surface_destroy(app.surface);
    wl_buffer_destroy(a);
    wl_buffer_destroy(b);
    wl_display_roundtrip(app.display);
    wl_display_disconnect(app.display);
    if (!ok) fprintf(stderr, "A screenshot was not taken ($SCREENSHOOTER)\n");
    return ok ? 0 : 2;
}
