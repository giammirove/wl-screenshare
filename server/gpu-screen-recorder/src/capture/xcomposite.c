#include "../../include/capture/xcomposite.h"
#include "../../include/window_texture.h"
#include "../../include/utils.h"
#include "../../include/cursor.h"
#include "../../include/color_conversion.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>
#include <assert.h>

#include <X11/Xlib.h>

#include <libavutil/frame.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_xcomposite_params params;

    bool should_stop;
    bool stop_is_error;
    bool window_resized;
    bool follow_focused_initialized;
    bool init_new_window;

    Window window;
    vec2i window_size;
    vec2i texture_size;
    double window_resize_timer;
    
    WindowTexture window_texture;
    AVCodecContext *video_codec_context;

    Atom net_active_window_atom;

    gsr_cursor cursor;

    bool clear_background;
    bool fast_path_failed;
} gsr_capture_xcomposite;

static void gsr_capture_xcomposite_stop(gsr_capture_xcomposite *self) {
    window_texture_deinit(&self->window_texture);
    gsr_cursor_deinit(&self->cursor);
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static Window get_focused_window(Display *display, Atom net_active_window_atom) {
    Atom type;
    int format = 0;
    unsigned long num_items = 0;
    unsigned long bytes_after = 0;
    unsigned char *properties = NULL;
    if(XGetWindowProperty(display, DefaultRootWindow(display), net_active_window_atom, 0, 1024, False, AnyPropertyType, &type, &format, &num_items, &bytes_after, &properties) == Success && properties) {
        Window focused_window = *(unsigned long*)properties;
        XFree(properties);
        return focused_window;
    }
    return None;
}

static int gsr_capture_xcomposite_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_xcomposite *self = cap->priv;

    if(self->params.follow_focused) {
        self->net_active_window_atom = XInternAtom(self->params.egl->x11.dpy, "_NET_ACTIVE_WINDOW", False);
        if(!self->net_active_window_atom) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_start failed: failed to get _NET_ACTIVE_WINDOW atom\n");
            return -1;
        }
        self->window = get_focused_window(self->params.egl->x11.dpy, self->net_active_window_atom);
    } else {
        self->window = self->params.window;
    }

    /* TODO: Do these in tick, and allow error if follow_focused */

    XWindowAttributes attr;
    if(!XGetWindowAttributes(self->params.egl->x11.dpy, self->window, &attr) && !self->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start failed: invalid window id: %lu\n", self->window);
        return -1;
    }

    self->window_size.x = max_int(attr.width, 0);
    self->window_size.y = max_int(attr.height, 0);

    if(self->params.follow_focused)
        XSelectInput(self->params.egl->x11.dpy, DefaultRootWindow(self->params.egl->x11.dpy), PropertyChangeMask);

    // TODO: Get select and add these on top of it and then restore at the end. Also do the same in other xcomposite
    XSelectInput(self->params.egl->x11.dpy, self->window, StructureNotifyMask | ExposureMask);

    /* Disable vsync */
    self->params.egl->eglSwapInterval(self->params.egl->egl_display, 0);
    if(window_texture_init(&self->window_texture, self->params.egl->x11.dpy, self->window, self->params.egl) != 0 && !self->params.follow_focused) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_start: failed to get window texture for window %ld\n", self->window);
        return -1;
    }

    if(gsr_cursor_init(&self->cursor, self->params.egl, self->params.egl->x11.dpy) != 0) {
        gsr_capture_xcomposite_stop(self);
        return -1;
    }

    self->texture_size.x = 0;
    self->texture_size.y = 0;

    self->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&self->window_texture));
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &self->texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &self->texture_size.y);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    vec2i video_size = self->texture_size;

    if(self->params.region_size.x > 0 && self->params.region_size.y > 0)
        video_size = self->params.region_size;

    video_codec_context->width = FFALIGN(video_size.x, 2);
    video_codec_context->height = FFALIGN(video_size.y, 2);

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->video_codec_context = video_codec_context;
    self->window_resize_timer = clock_get_monotonic_seconds();
    return 0;
}

static void gsr_capture_xcomposite_tick(gsr_capture *cap) {
    gsr_capture_xcomposite *self = cap->priv;

    if(self->params.follow_focused && !self->follow_focused_initialized) {
        self->init_new_window = true;
    }

    if(self->init_new_window) {
        self->init_new_window = false;
        Window focused_window = get_focused_window(self->params.egl->x11.dpy, self->net_active_window_atom);
        if(focused_window != self->window || !self->follow_focused_initialized) {
            self->follow_focused_initialized = true;
            XSelectInput(self->params.egl->x11.dpy, self->window, 0);
            self->window = focused_window;
            XSelectInput(self->params.egl->x11.dpy, self->window, StructureNotifyMask | ExposureMask);

            XWindowAttributes attr;
            attr.width = 0;
            attr.height = 0;
            if(!XGetWindowAttributes(self->params.egl->x11.dpy, self->window, &attr))
                fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick failed: invalid window id: %lu\n", self->window);

            self->window_size.x = max_int(attr.width, 0);
            self->window_size.y = max_int(attr.height, 0);

            window_texture_deinit(&self->window_texture);
            window_texture_init(&self->window_texture, self->params.egl->x11.dpy, self->window, self->params.egl); // TODO: Do not do the below window_texture_on_resize after this

            self->texture_size.x = 0;
            self->texture_size.y = 0;

            self->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&self->window_texture));
            self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &self->texture_size.x);
            self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &self->texture_size.y);
            self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

            self->window_resized = false;
            self->clear_background = true;
        }
    }

    const double window_resize_timeout = 1.0; // 1 second
    if(self->window_resized && clock_get_monotonic_seconds() - self->window_resize_timer >= window_resize_timeout) {
        self->window_resized = false;

        if(window_texture_on_resize(&self->window_texture) != 0) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_tick: window_texture_on_resize failed\n");
            //self->should_stop = true;
            //self->stop_is_error = true;
            return;
        }

        self->texture_size.x = 0;
        self->texture_size.y = 0;

        self->params.egl->glBindTexture(GL_TEXTURE_2D, window_texture_get_opengl_texture_id(&self->window_texture));
        self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &self->texture_size.x);
        self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &self->texture_size.y);
        self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

        self->clear_background = true;
    }
}

static void gsr_capture_xcomposite_on_event(gsr_capture *cap, gsr_egl *egl) {
    gsr_capture_xcomposite *self = cap->priv;
    XEvent *xev = gsr_egl_get_event_data(egl);
    switch(xev->type) {
        case DestroyNotify: {
            /* Window died (when not following focused window), so we stop recording */
            if(!self->params.follow_focused && xev->xdestroywindow.window == self->window) {
                self->should_stop = true;
                self->stop_is_error = false;
            }
            break;
        }
        case Expose: {
            /* Requires window texture recreate */
            if(xev->xexpose.count == 0 && xev->xexpose.window == self->window) {
                self->window_resize_timer = clock_get_monotonic_seconds();
                self->window_resized = true;
            }
            break;
        }
        case ConfigureNotify: {
            /* Window resized */
            if(xev->xconfigure.window == self->window && (xev->xconfigure.width != self->window_size.x || xev->xconfigure.height != self->window_size.y)) {
                self->window_size.x = max_int(xev->xconfigure.width, 0);
                self->window_size.y = max_int(xev->xconfigure.height, 0);
                self->window_resize_timer = clock_get_monotonic_seconds();
                self->window_resized = true;
            }
            break;
        }
        case PropertyNotify: {
            /* Focused window changed */
            if(self->params.follow_focused && xev->xproperty.atom == self->net_active_window_atom) {
                self->init_new_window = true;
            }
            break;
        }
    }

    gsr_cursor_on_event(&self->cursor, xev);
}

static bool gsr_capture_xcomposite_should_stop(gsr_capture *cap, bool *err) {
    gsr_capture_xcomposite *self = cap->priv;
    if(self->should_stop) {
        if(err)
            *err = self->stop_is_error;
        return true;
    }

    if(err)
        *err = false;
    return false;
}

static int gsr_capture_xcomposite_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_capture_xcomposite *self = cap->priv;
    (void)frame;

    if(self->clear_background) {
        self->clear_background = false;
        gsr_color_conversion_clear(color_conversion);
    }

    const vec2i target_pos = { max_int(0, frame->width / 2 - self->texture_size.x / 2), max_int(0, frame->height / 2 - self->texture_size.y / 2) };

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    /* Fast opengl free path */
    if(!self->fast_path_failed && video_codec_context_is_vaapi(self->video_codec_context) && self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD) {
        if(!vaapi_copy_egl_image_to_video_surface(self->params.egl, self->window_texture.image, (vec2i){0, 0}, self->texture_size, target_pos, self->texture_size, self->video_codec_context, frame)) {
            fprintf(stderr, "gsr error: gsr_capture_xcomposite_capture: vaapi_copy_egl_image_to_video_surface failed, falling back to opengl copy. Please report this as an issue at https://github.com/dec05eba/gpu-screen-recorder-issues\n");
            self->fast_path_failed = true;
        }
    } else {
        self->fast_path_failed = true;
    }

    if(self->fast_path_failed) {
        gsr_color_conversion_draw(color_conversion, window_texture_get_opengl_texture_id(&self->window_texture),
            target_pos, self->texture_size,
            (vec2i){0, 0}, self->texture_size,
            0.0f, false);
    }

    if(self->params.record_cursor && self->cursor.visible) {
        gsr_cursor_tick(&self->cursor, self->window);

        const vec2i cursor_pos = {
            target_pos.x + self->cursor.position.x - self->cursor.hotspot.x,
            target_pos.y + self->cursor.position.y - self->cursor.hotspot.y
        };

        self->params.egl->glEnable(GL_SCISSOR_TEST);
        self->params.egl->glScissor(target_pos.x, target_pos.y, self->texture_size.x, self->texture_size.y);

        gsr_color_conversion_draw(color_conversion, self->cursor.texture_id,
            cursor_pos, self->cursor.size,
            (vec2i){0, 0}, self->cursor.size,
            0.0f, false);

        self->params.egl->glDisable(GL_SCISSOR_TEST);
    }

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    return 0;
}

static gsr_source_color gsr_capture_xcomposite_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_RGB;
}

static uint64_t gsr_capture_xcomposite_get_window_id(gsr_capture *cap) {
    gsr_capture_xcomposite *self = cap->priv;
    return self->window;
}

static void gsr_capture_xcomposite_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    if(cap->priv) {
        gsr_capture_xcomposite_stop(cap->priv);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_xcomposite_create(const gsr_capture_xcomposite_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_xcomposite *cap_xcomp = calloc(1, sizeof(gsr_capture_xcomposite));
    if(!cap_xcomp) {
        free(cap);
        return NULL;
    }

    cap_xcomp->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_xcomposite_start,
        .on_event = gsr_capture_xcomposite_on_event,
        .tick = gsr_capture_xcomposite_tick,
        .should_stop = gsr_capture_xcomposite_should_stop,
        .capture = gsr_capture_xcomposite_capture,
        .get_source_color = gsr_capture_xcomposite_get_source_color,
        .uses_external_image = NULL,
        .get_window_id = gsr_capture_xcomposite_get_window_id,
        .destroy = gsr_capture_xcomposite_destroy,
        .priv = cap_xcomp
    };

    return cap;
}
