#include "../include/damage.h"
#include "../include/utils.h"

#include <stdio.h>
#include <string.h>
#include <X11/extensions/Xdamage.h>
#include <X11/extensions/Xrandr.h>

typedef struct {
    vec2i pos;
    vec2i size;
} gsr_rectangle;

static bool rectangles_intersect(gsr_rectangle rect1, gsr_rectangle rect2) {
    return rect1.pos.x < rect2.pos.x + rect2.size.x && rect1.pos.x + rect1.size.x > rect2.pos.x &&
        rect1.pos.y < rect2.pos.y + rect2.size.y && rect1.pos.y + rect1.size.y > rect2.pos.y;
}

static bool xrandr_is_supported(Display *display) {
    int major_version = 0;
    int minor_version = 0;
    if(!XRRQueryVersion(display, &major_version, &minor_version))
        return false;

    return major_version > 1 || (major_version == 1 && minor_version >= 2);
}

bool gsr_damage_init(gsr_damage *self, gsr_egl *egl, bool track_cursor) {
    memset(self, 0, sizeof(*self));
    self->egl = egl;
    self->track_cursor = track_cursor;

    if(gsr_egl_get_display_server(egl) != GSR_DISPLAY_SERVER_X11) {
        fprintf(stderr, "gsr warning: gsr_damage_init: damage tracking is not supported on wayland\n");
        return false;
    }

    if(!XDamageQueryExtension(self->egl->x11.dpy, &self->damage_event, &self->damage_error)) {
        fprintf(stderr, "gsr warning: gsr_damage_init: XDamage is not supported by your X11 server\n");
        gsr_damage_deinit(self);
        return false;
    }

    if(!XRRQueryExtension(self->egl->x11.dpy, &self->randr_event, &self->randr_error)) {
        fprintf(stderr, "gsr warning: gsr_damage_init: XRandr is not supported by your X11 server\n");
        gsr_damage_deinit(self);
        return false;
    }

    if(!xrandr_is_supported(self->egl->x11.dpy)) {
        fprintf(stderr, "gsr warning: gsr_damage_init: your X11 randr version is too old\n");
        gsr_damage_deinit(self);
        return false;
    }

    if(self->track_cursor)
        self->track_cursor = gsr_cursor_init(&self->cursor, self->egl, self->egl->x11.dpy) == 0;

    XRRSelectInput(self->egl->x11.dpy, DefaultRootWindow(self->egl->x11.dpy), RRScreenChangeNotifyMask | RRCrtcChangeNotifyMask | RROutputChangeNotifyMask);

    self->damaged = true;
    return true;
}

void gsr_damage_deinit(gsr_damage *self) {
    if(self->damage) {
        XDamageDestroy(self->egl->x11.dpy, self->damage);
        self->damage = None;
    }

    gsr_cursor_deinit(&self->cursor);

    self->damage_event = 0;
    self->damage_error = 0;

    self->randr_event = 0;
    self->randr_error = 0;
}

bool gsr_damage_set_target_window(gsr_damage *self, uint64_t window) {
    if(self->damage_event == 0)
        return false;

    if(window == self->window)
        return true;

    if(self->damage) {
        XDamageDestroy(self->egl->x11.dpy, self->damage);
        self->damage = None;
    }

    if(self->window)
        XSelectInput(self->egl->x11.dpy, self->window, 0);

    self->window = window;
    XSelectInput(self->egl->x11.dpy, self->window, StructureNotifyMask | ExposureMask);

    XWindowAttributes win_attr;
    win_attr.x = 0;
    win_attr.y = 0;
    win_attr.width = 0;
    win_attr.height = 0;
    if(!XGetWindowAttributes(self->egl->x11.dpy, self->window, &win_attr))
        fprintf(stderr, "gsr warning: gsr_damage_set_target_window failed: failed to get window attributes: %ld\n", (long)self->window);

    //self->window_pos.x = win_attr.x;
    //self->window_pos.y = win_attr.y;

    self->window_size.x = win_attr.width;
    self->window_size.y = win_attr.height;

    self->damage = XDamageCreate(self->egl->x11.dpy, window, XDamageReportNonEmpty);
    if(self->damage) {
        XDamageSubtract(self->egl->x11.dpy, self->damage, None, None);
        self->damaged = true;
        self->track_type = GSR_DAMAGE_TRACK_WINDOW;
        return true;
    } else {
        fprintf(stderr, "gsr warning: gsr_damage_set_target_window: XDamageCreate failed\n");
        self->track_type = GSR_DAMAGE_TRACK_NONE;
        return false;
    }
}

bool gsr_damage_set_target_monitor(gsr_damage *self, const char *monitor_name) {
    if(self->damage_event == 0)
        return false;

    if(strcmp(self->monitor_name, monitor_name) == 0)
        return true;

    if(self->damage) {
        XDamageDestroy(self->egl->x11.dpy, self->damage);
        self->damage = None;
    }

    memset(&self->monitor, 0, sizeof(self->monitor));
    if(strcmp(monitor_name, "screen") != 0 && strcmp(monitor_name, "screen-direct") != 0 && strcmp(monitor_name, "screen-direct-force") != 0) {
        if(!get_monitor_by_name(self->egl, GSR_CONNECTION_X11, monitor_name, &self->monitor))
            fprintf(stderr, "gsr warning: gsr_damage_set_target_monitor: failed to find monitor: %s\n", monitor_name);
    }

    if(self->window)
        XSelectInput(self->egl->x11.dpy, self->window, 0);

    self->window = DefaultRootWindow(self->egl->x11.dpy);
    self->damage = XDamageCreate(self->egl->x11.dpy, self->window, XDamageReportNonEmpty);
    if(self->damage) {
        XDamageSubtract(self->egl->x11.dpy, self->damage, None, None);
        self->damaged = true;
        snprintf(self->monitor_name, sizeof(self->monitor_name), "%s", monitor_name);
        self->track_type = GSR_DAMAGE_TRACK_MONITOR;
        return true;
    } else {
        fprintf(stderr, "gsr warning: gsr_damage_set_target_monitor: XDamageCreate failed\n");
        self->track_type = GSR_DAMAGE_TRACK_NONE;
        return false;
    }
}

static void gsr_damage_on_crtc_change(gsr_damage *self, XEvent *xev) {
    const XRRCrtcChangeNotifyEvent *rr_crtc_change_event = (XRRCrtcChangeNotifyEvent*)xev;
    if(rr_crtc_change_event->crtc == 0 || self->monitor.monitor_identifier == 0)
        return;

    if(rr_crtc_change_event->crtc != self->monitor.monitor_identifier)
        return;

    if(rr_crtc_change_event->width == 0 || rr_crtc_change_event->height == 0)
        return;

    if(rr_crtc_change_event->x != self->monitor.pos.x || rr_crtc_change_event->y != self->monitor.pos.y ||
        (int)rr_crtc_change_event->width != self->monitor.size.x || (int)rr_crtc_change_event->height != self->monitor.size.y) {
        self->monitor.pos.x = rr_crtc_change_event->x;
        self->monitor.pos.y = rr_crtc_change_event->y;

        self->monitor.size.x = rr_crtc_change_event->width;
        self->monitor.size.y = rr_crtc_change_event->height;
    }
}

static void gsr_damage_on_output_change(gsr_damage *self, XEvent *xev) {
    const XRROutputChangeNotifyEvent *rr_output_change_event = (XRROutputChangeNotifyEvent*)xev;
    if(!rr_output_change_event->output || self->monitor.monitor_identifier == 0)
        return;

    XRRScreenResources *screen_res = XRRGetScreenResources(self->egl->x11.dpy, DefaultRootWindow(self->egl->x11.dpy));
    if(!screen_res)
        return;

    XRROutputInfo *out_info = XRRGetOutputInfo(self->egl->x11.dpy, screen_res, rr_output_change_event->output);
    if(out_info && out_info->crtc && out_info->crtc == self->monitor.monitor_identifier) {
        XRRCrtcInfo *crtc_info = XRRGetCrtcInfo(self->egl->x11.dpy, screen_res, out_info->crtc);
        if(crtc_info && (crtc_info->x != self->monitor.pos.x || crtc_info->y != self->monitor.pos.y ||
            (int)crtc_info->width != self->monitor.size.x || (int)crtc_info->height != self->monitor.size.y))
        {
            self->monitor.pos.x = crtc_info->x;
            self->monitor.pos.y = crtc_info->y;

            self->monitor.size.x = crtc_info->width;
            self->monitor.size.y = crtc_info->height;
        }

        if(crtc_info)
            XRRFreeCrtcInfo(crtc_info);
    }

    if(out_info)
        XRRFreeOutputInfo(out_info);
    
    XRRFreeScreenResources(screen_res);
}

static void gsr_damage_on_randr_event(gsr_damage *self, XEvent *xev) {
    const XRRNotifyEvent *rr_event = (XRRNotifyEvent*)xev;
    switch(rr_event->subtype) {
        case RRNotify_CrtcChange:
            gsr_damage_on_crtc_change(self, xev);
            break;
        case RRNotify_OutputChange:
            gsr_damage_on_output_change(self, xev);
            break;
    }
}

static void gsr_damage_on_damage_event(gsr_damage *self, XEvent *xev) {
    const XDamageNotifyEvent *de = (XDamageNotifyEvent*)xev;
    XserverRegion region = XFixesCreateRegion(self->egl->x11.dpy, NULL, 0);
    /* Subtract all the damage, repairing the window */
    XDamageSubtract(self->egl->x11.dpy, de->damage, None, region);

    if(self->track_type == GSR_DAMAGE_TRACK_WINDOW || (self->track_type == GSR_DAMAGE_TRACK_MONITOR && self->monitor.connector_id == 0)) {
        self->damaged = true;
    } else {
        int num_rectangles = 0;
        XRectangle *rectangles = XFixesFetchRegion(self->egl->x11.dpy, region, &num_rectangles);
        if(rectangles) {
            const gsr_rectangle monitor_region = { self->monitor.pos, self->monitor.size };
            for(int i = 0; i < num_rectangles; ++i) {
                const gsr_rectangle damage_region = { (vec2i){rectangles[i].x, rectangles[i].y}, (vec2i){rectangles[i].width, rectangles[i].height} };
                self->damaged = rectangles_intersect(monitor_region, damage_region);
                if(self->damaged)
                    break;
            }
            XFree(rectangles);
        }
    }

    XFixesDestroyRegion(self->egl->x11.dpy, region);
    XFlush(self->egl->x11.dpy);
}

static void gsr_damage_on_tick_cursor(gsr_damage *self) {
    vec2i prev_cursor_pos = self->cursor.position;
    gsr_cursor_tick(&self->cursor, self->window);
    if(self->cursor.position.x != prev_cursor_pos.x || self->cursor.position.y != prev_cursor_pos.y) {
        const gsr_rectangle cursor_region = { self->cursor.position, self->cursor.size };
        switch(self->track_type) {
            case GSR_DAMAGE_TRACK_NONE: {
                self->damaged = true;
                break;
            }
            case GSR_DAMAGE_TRACK_WINDOW: {
                const gsr_rectangle window_region = { (vec2i){0, 0}, self->window_size };
                self->damaged = self->window_size.x == 0 || rectangles_intersect(window_region, cursor_region);
                break;
            }
            case GSR_DAMAGE_TRACK_MONITOR: {
                const gsr_rectangle monitor_region = { self->monitor.pos, self->monitor.size };
                self->damaged = self->monitor.monitor_identifier == 0 || rectangles_intersect(monitor_region, cursor_region);
                break;
            }
        }
    }
}

static void gsr_damage_on_window_configure_notify(gsr_damage *self, XEvent *xev) {
    if(xev->xconfigure.window != self->window)
        return;

    //self->window_pos.x = xev->xconfigure.x;
    //self->window_pos.y = xev->xconfigure.y;
    
    self->window_size.x = xev->xconfigure.width;
    self->window_size.y = xev->xconfigure.height;
}

void gsr_damage_on_event(gsr_damage *self, XEvent *xev) {
    if(self->damage_event == 0 || self->track_type == GSR_DAMAGE_TRACK_NONE)
        return;

    if(self->track_type == GSR_DAMAGE_TRACK_WINDOW && xev->type == ConfigureNotify)
        gsr_damage_on_window_configure_notify(self, xev);

    if(self->randr_event) {
        if(xev->type == self->randr_event + RRScreenChangeNotify)
            XRRUpdateConfiguration(xev);

        if(xev->type == self->randr_event + RRNotify)
            gsr_damage_on_randr_event(self, xev);
    }

    if(self->damage_event && xev->type == self->damage_event + XDamageNotify)
        gsr_damage_on_damage_event(self, xev);

    if(self->track_cursor)
        gsr_cursor_on_event(&self->cursor, xev);
}

void gsr_damage_tick(gsr_damage *self) {
    if(self->damage_event == 0 || self->track_type == GSR_DAMAGE_TRACK_NONE)
        return;

    if(self->track_cursor && self->cursor.visible && !self->damaged)
        gsr_damage_on_tick_cursor(self);
}

bool gsr_damage_is_damaged(gsr_damage *self) {
    return self->damage_event == 0 || !self->damage || self->damaged || self->track_type == GSR_DAMAGE_TRACK_NONE;
}

void gsr_damage_clear(gsr_damage *self) {
    self->damaged = false;
}
