#include "../include/utils.h"

#include <time.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <errno.h>
#include <assert.h>

#include <xf86drmMode.h>
#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>
#include <X11/Xatom.h>
#include <X11/extensions/Xrandr.h>
#include <va/va_drmcommon.h>
#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vaapi.h>

double clock_get_monotonic_seconds(void) {
    struct timespec ts;
    ts.tv_sec = 0;
    ts.tv_nsec = 0;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (double)ts.tv_sec + (double)ts.tv_nsec * 0.000000001;
}

static gsr_monitor_rotation wayland_transform_to_gsr_rotation(int32_t rot) {
    switch(rot) {
        case 0: return GSR_MONITOR_ROT_0;
        case 1: return GSR_MONITOR_ROT_90;
        case 2: return GSR_MONITOR_ROT_180;
        case 3: return GSR_MONITOR_ROT_270;
    }
    return GSR_MONITOR_ROT_0;
}

static const XRRModeInfo* get_mode_info(const XRRScreenResources *sr, RRMode id) {
    for(int i = 0; i < sr->nmode; ++i) {
        if(sr->modes[i].id == id)
            return &sr->modes[i];
    }    
    return NULL;
}

static gsr_monitor_rotation x11_rotation_to_gsr_rotation(int rot) {
    switch(rot) {
        case RR_Rotate_0:   return GSR_MONITOR_ROT_0;
        case RR_Rotate_90:  return GSR_MONITOR_ROT_90;
        case RR_Rotate_180: return GSR_MONITOR_ROT_180;
        case RR_Rotate_270: return GSR_MONITOR_ROT_270;
    }
    return GSR_MONITOR_ROT_0;
}

static uint32_t x11_output_get_connector_id(Display *dpy, RROutput output, Atom randr_connector_id_atom) {
    Atom type = 0;
    int format = 0;
    unsigned long bytes_after = 0;
    unsigned long nitems = 0;
    unsigned char *prop = NULL;
    XRRGetOutputProperty(dpy, output, randr_connector_id_atom, 0, 128, false, false, AnyPropertyType, &type, &format, &nitems, &bytes_after, &prop);

    long result = 0;
    if(type == XA_INTEGER && format == 32)
        result = *(long*)prop;

    free(prop);
    return result;
}

void for_each_active_monitor_output_x11_not_cached(Display *display, active_monitor_callback callback, void *userdata) {
    XRRScreenResources *screen_res = XRRGetScreenResources(display, DefaultRootWindow(display));
    if(!screen_res)
        return;

    const Atom randr_connector_id_atom = XInternAtom(display, "CONNECTOR_ID", False);

    char display_name[256];
    for(int i = 0; i < screen_res->noutput; ++i) {
        XRROutputInfo *out_info = XRRGetOutputInfo(display, screen_res, screen_res->outputs[i]);
        if(out_info && out_info->crtc && out_info->connection == RR_Connected) {
            XRRCrtcInfo *crt_info = XRRGetCrtcInfo(display, screen_res, out_info->crtc);
            if(crt_info && crt_info->mode) {
                const XRRModeInfo *mode_info = get_mode_info(screen_res, crt_info->mode);
                if(mode_info && out_info->nameLen < (int)sizeof(display_name)) {
                    snprintf(display_name, sizeof(display_name), "%.*s", (int)out_info->nameLen, out_info->name);
                    const gsr_monitor monitor = {
                        .name = display_name,
                        .name_len = out_info->nameLen,
                        .pos = { .x = crt_info->x, .y = crt_info->y },
                        .size = { .x = (int)crt_info->width, .y = (int)crt_info->height },
                        .connector_id = x11_output_get_connector_id(display, screen_res->outputs[i], randr_connector_id_atom),
                        .rotation = x11_rotation_to_gsr_rotation(crt_info->rotation),
                        .monitor_identifier = out_info->crtc
                    };
                    callback(&monitor, userdata);
                }
            }
            if(crt_info)
                XRRFreeCrtcInfo(crt_info);
        }
        if(out_info)
            XRRFreeOutputInfo(out_info);
    }    

    XRRFreeScreenResources(screen_res);
}

void for_each_active_monitor_output_x11(const gsr_egl *egl, active_monitor_callback callback, void *userdata) {
    for(int i = 0; i < egl->x11.num_outputs; ++i) {
        const gsr_x11_output *output = &egl->x11.outputs[i];
        const gsr_monitor monitor = {
            .name = output->name,
            .name_len = strlen(output->name),
            .pos = output->pos,
            .size = output->size,
            .connector_id = output->connector_id,
            .rotation = output->rotation,
            .monitor_identifier = output->monitor_identifier
        };
        callback(&monitor, userdata);
    }
}

typedef struct {
    int type;
    int count;
    int count_active;
} drm_connector_type_count;

#define CONNECTOR_TYPE_COUNTS 32

static drm_connector_type_count* drm_connector_types_get_index(drm_connector_type_count *type_counts, int *num_type_counts, int connector_type) {
    for(int i = 0; i < *num_type_counts; ++i) {
        if(type_counts[i].type == connector_type)
            return &type_counts[i];
    }

    if(*num_type_counts == CONNECTOR_TYPE_COUNTS)
        return NULL;

    const int index = *num_type_counts;
    type_counts[index].type = connector_type;
    type_counts[index].count = 0;
    type_counts[index].count_active = 0;
    ++*num_type_counts;
    return &type_counts[index];
}

static bool connector_get_property_by_name(int drmfd, drmModeConnectorPtr props, const char *name, uint64_t *result) {
    for(int i = 0; i < props->count_props; ++i) {
        drmModePropertyPtr prop = drmModeGetProperty(drmfd, props->props[i]);
        if(prop) {
            if(strcmp(name, prop->name) == 0) {
                *result = props->prop_values[i];
                drmModeFreeProperty(prop);
                return true;
            }
            drmModeFreeProperty(prop);
        }
    }
    return false;
}

/* TODO: Support more connector types*/
static int get_connector_type_by_name(const char *name) {
    int len = strlen(name);
    if(len >= 5 && strncmp(name, "HDMI-", 5) == 0)
        return 1;
    else if(len >= 3 && strncmp(name, "DP-", 3) == 0)
        return 2;
    else if(len >= 12 && strncmp(name, "DisplayPort-", 12) == 0)
        return 3;
    else if(len >= 4 && strncmp(name, "eDP-", 4) == 0)
        return 4;
    else
        return -1;
}

static uint32_t monitor_identifier_from_type_and_count(int monitor_type_index, int monitor_type_count) {
    return ((uint32_t)monitor_type_index << 16) | ((uint32_t)monitor_type_count);
}

static void for_each_active_monitor_output_wayland(const gsr_egl *egl, active_monitor_callback callback, void *userdata) {
    drm_connector_type_count type_counts[CONNECTOR_TYPE_COUNTS];
    int num_type_counts = 0;

    for(int i = 0; i < egl->wayland.num_outputs; ++i) {
        const gsr_wayland_output *output = &egl->wayland.outputs[i];
        if(!output->name)
            continue;

        const int connector_type_index = get_connector_type_by_name(output->name);
        drm_connector_type_count *connector_type = NULL;
        if(connector_type_index != -1)
            connector_type = drm_connector_types_get_index(type_counts, &num_type_counts, connector_type_index);
        
        if(connector_type) {
            ++connector_type->count;
            ++connector_type->count_active;
        }

        const gsr_monitor monitor = {
            .name = output->name,
            .name_len = strlen(output->name),
            .pos = { .x = output->pos.x, .y = output->pos.y },
            .size = { .x = output->size.x, .y = output->size.y },
            .connector_id = 0,
            .rotation = wayland_transform_to_gsr_rotation(output->transform),
            .monitor_identifier = connector_type ? monitor_identifier_from_type_and_count(connector_type_index, connector_type->count_active) : 0
        };
        callback(&monitor, userdata);
    }
}

static void for_each_active_monitor_output_drm(const gsr_egl *egl, active_monitor_callback callback, void *userdata) {
    int fd = open(egl->card_path, O_RDONLY);
    if(fd == -1)
        return;

    drmSetClientCap(fd, DRM_CLIENT_CAP_ATOMIC, 1);

    drm_connector_type_count type_counts[CONNECTOR_TYPE_COUNTS];
    int num_type_counts = 0;

    char display_name[256];
    drmModeResPtr resources = drmModeGetResources(fd);
    if(resources) {
        for(int i = 0; i < resources->count_connectors; ++i) {
            drmModeConnectorPtr connector = drmModeGetConnectorCurrent(fd, resources->connectors[i]);
            if(!connector)
                continue;

            drm_connector_type_count *connector_type = drm_connector_types_get_index(type_counts, &num_type_counts, connector->connector_type);
            const char *connection_name = drmModeGetConnectorTypeName(connector->connector_type);
            const int connection_name_len = strlen(connection_name);
            if(connector_type)
                ++connector_type->count;

            if(connector->connection != DRM_MODE_CONNECTED) {
                drmModeFreeConnector(connector);
                continue;
            }

            if(connector_type)
                ++connector_type->count_active;

            uint64_t crtc_id = 0;
            connector_get_property_by_name(fd, connector, "CRTC_ID", &crtc_id);

            drmModeCrtcPtr crtc = drmModeGetCrtc(fd, crtc_id);
            if(connector_type && crtc_id > 0 && crtc && connection_name_len + 5 < (int)sizeof(display_name)) {
                const int display_name_len = snprintf(display_name, sizeof(display_name), "%s-%d", connection_name, connector_type->count);
                const int connector_type_index_name = get_connector_type_by_name(display_name);
                gsr_monitor monitor = {
                    .name = display_name,
                    .name_len = display_name_len,
                    .pos = { .x = crtc->x, .y = crtc->y },
                    .size = { .x = (int)crtc->width, .y = (int)crtc->height },
                    .connector_id = connector->connector_id,
                    .rotation = GSR_MONITOR_ROT_0,
                    .monitor_identifier = connector_type_index_name != -1 ? monitor_identifier_from_type_and_count(connector_type_index_name, connector_type->count_active) : 0
                };
                callback(&monitor, userdata);
            }

            if(crtc)
                drmModeFreeCrtc(crtc);

            drmModeFreeConnector(connector);
        }
        drmModeFreeResources(resources);
    }

    close(fd);
}

void for_each_active_monitor_output(const gsr_egl *egl, gsr_connection_type connection_type, active_monitor_callback callback, void *userdata) {
    switch(connection_type) {
        case GSR_CONNECTION_X11:
            for_each_active_monitor_output_x11(egl, callback, userdata);
            break;
        case GSR_CONNECTION_WAYLAND:
            for_each_active_monitor_output_wayland(egl, callback, userdata);
            break;
        case GSR_CONNECTION_DRM:
            for_each_active_monitor_output_drm(egl, callback, userdata);
            break;
    }
}

static void get_monitor_by_name_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_name_userdata *data = (get_monitor_by_name_userdata*)userdata;
    if(!data->found_monitor && strcmp(data->name, monitor->name) == 0) {
        data->monitor->pos = monitor->pos;
        data->monitor->size = monitor->size;
        data->monitor->connector_id = monitor->connector_id;
        data->monitor->rotation = monitor->rotation;
        data->monitor->monitor_identifier = monitor->monitor_identifier;
        data->found_monitor = true;
    }
}

bool get_monitor_by_name(const gsr_egl *egl, gsr_connection_type connection_type, const char *name, gsr_monitor *monitor) {
    get_monitor_by_name_userdata userdata;
    userdata.name = name;
    userdata.name_len = strlen(name);
    userdata.monitor = monitor;
    userdata.found_monitor = false;
    for_each_active_monitor_output(egl, connection_type, get_monitor_by_name_callback, &userdata);
    return userdata.found_monitor;
}

typedef struct {
    const gsr_monitor *monitor;
    gsr_monitor_rotation rotation;
    bool match_found;
} get_monitor_by_connector_id_userdata;

static bool vec2i_eql(vec2i a, vec2i b) {
    return a.x == b.x && a.y == b.y;
}

static void get_monitor_by_name_and_size_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_connector_id_userdata *data = (get_monitor_by_connector_id_userdata*)userdata;
    if(monitor->name && data->monitor->name && strcmp(monitor->name, data->monitor->name) == 0 && vec2i_eql(monitor->size, data->monitor->size)) {
        data->rotation = monitor->rotation;
        data->match_found = true;
    }
}

static void get_monitor_by_connector_id_callback(const gsr_monitor *monitor, void *userdata) {
    get_monitor_by_connector_id_userdata *data = (get_monitor_by_connector_id_userdata*)userdata;
    if(monitor->connector_id == data->monitor->connector_id ||
        (!monitor->connector_id && monitor->monitor_identifier == data->monitor->monitor_identifier))
    {
        data->rotation = monitor->rotation;
        data->match_found = true;
    }
}

gsr_monitor_rotation drm_monitor_get_display_server_rotation(const gsr_egl *egl, const gsr_monitor *monitor) {
    if(gsr_egl_get_display_server(egl) == GSR_DISPLAY_SERVER_WAYLAND) {
        {
            get_monitor_by_connector_id_userdata userdata;
            userdata.monitor = monitor;
            userdata.rotation = GSR_MONITOR_ROT_0;
            userdata.match_found = false;
            for_each_active_monitor_output_wayland(egl, get_monitor_by_name_and_size_callback, &userdata);
            if(userdata.match_found)
                return userdata.rotation;
        }
        {
            get_monitor_by_connector_id_userdata userdata;
            userdata.monitor = monitor;
            userdata.rotation = GSR_MONITOR_ROT_0;
            userdata.match_found = false;
            for_each_active_monitor_output_wayland(egl, get_monitor_by_connector_id_callback, &userdata);
            return userdata.rotation;
        }
    } else {
        get_monitor_by_connector_id_userdata userdata;
        userdata.monitor = monitor;
        userdata.rotation = GSR_MONITOR_ROT_0;
        userdata.match_found = false;
        for_each_active_monitor_output_x11(egl, get_monitor_by_connector_id_callback, &userdata);
        return userdata.rotation;
    }

    return GSR_MONITOR_ROT_0;
}

bool gl_get_gpu_info(gsr_egl *egl, gsr_gpu_info *info) {
    const char *software_renderers[] = { "llvmpipe", "SWR", "softpipe", NULL };
    bool supported = true;
    const unsigned char *gl_vendor = egl->glGetString(GL_VENDOR);
    const unsigned char *gl_renderer = egl->glGetString(GL_RENDERER);

    info->gpu_version = 0;
    info->is_steam_deck = false;

    if(!gl_vendor) {
        fprintf(stderr, "gsr error: failed to get gpu vendor\n");
        supported = false;
        goto end;
    }

    if(gl_renderer) {
        for(int i = 0; software_renderers[i]; ++i) {
            if(strstr((const char*)gl_renderer, software_renderers[i])) {
                fprintf(stderr, "gsr error: your opengl environment is not properly setup. It's using %s (software rendering) for opengl instead of your graphics card. Please make sure your graphics driver is properly installed\n", software_renderers[i]);
                supported = false;
                goto end;
            }
        }
    }

    if(strstr((const char*)gl_vendor, "AMD"))
        info->vendor = GSR_GPU_VENDOR_AMD;
    else if(strstr((const char*)gl_vendor, "Intel"))
        info->vendor = GSR_GPU_VENDOR_INTEL;
    else if(strstr((const char*)gl_vendor, "NVIDIA"))
        info->vendor = GSR_GPU_VENDOR_NVIDIA;
    else {
        fprintf(stderr, "gsr error: unknown gpu vendor: %s\n", gl_vendor);
        supported = false;
        goto end;
    }

    if(gl_renderer) {
        if(info->vendor == GSR_GPU_VENDOR_NVIDIA)
            sscanf((const char*)gl_renderer, "%*s %*s %*s %d", &info->gpu_version);
        info->is_steam_deck = strstr((const char*)gl_renderer, "vangogh") != NULL;
    }

    end:
    return supported;
}

static bool try_card_has_valid_plane(const char *card_path) {
    drmVersion *ver = NULL;
    drmModePlaneResPtr planes = NULL;
    bool found_screen_card = false;

    int fd = open(card_path, O_RDONLY);
    if(fd == -1)
        return false;

    ver = drmGetVersion(fd);
    if(!ver || strstr(ver->name, "nouveau"))
        goto next;

    drmSetClientCap(fd, DRM_CLIENT_CAP_UNIVERSAL_PLANES, 1);

    planes = drmModeGetPlaneResources(fd);
    if(!planes)
        goto next;

    for(uint32_t j = 0; j < planes->count_planes; ++j) {
        drmModePlanePtr plane = drmModeGetPlane(fd, planes->planes[j]);
        if(!plane)
            continue;

        if(plane->fb_id)
            found_screen_card = true;

        drmModeFreePlane(plane);
        if(found_screen_card)
            break;
    }

    next:
    if(planes)
        drmModeFreePlaneResources(planes);
    if(ver)
        drmFreeVersion(ver);
    close(fd);
    if(found_screen_card)
        return true;

    return false;
}

static void string_copy(char *dst, const char *src, int len) {
    int src_len = strlen(src);
    int min_len = src_len;
    if(len - 1 < min_len)
        min_len = len - 1;
    memcpy(dst, src, min_len);
    dst[min_len] = '\0';
}

bool gsr_get_valid_card_path(gsr_egl *egl, char *output, bool is_monitor_capture) {
    if(egl->dri_card_path) {
        string_copy(output, egl->dri_card_path, 127);
        return is_monitor_capture ? try_card_has_valid_plane(output) : true;
    }

    for(int i = 0; i < 10; ++i) {
        snprintf(output, 127, DRM_DEV_NAME, DRM_DIR_NAME, i);
        if(try_card_has_valid_plane(output))
            return true;
    }
    return false;
}

bool gsr_card_path_get_render_path(const char *card_path, char *render_path) {
    int fd = open(card_path, O_RDONLY);
    if(fd == -1)
        return false;

    char *render_path_tmp = drmGetRenderDeviceNameFromFd(fd);
    if(render_path_tmp) {
        string_copy(render_path, render_path_tmp, 127);
        free(render_path_tmp);
        close(fd);
        return true;
    }

    close(fd);
    return false;
}

int create_directory_recursive(char *path) {
    int path_len = strlen(path);
    char *p = path;
    char *end = path + path_len;
    for(;;) {
        char *slash_p = strchr(p, '/');

        // Skips first '/', we don't want to try and create the root directory
        if(slash_p == path) {
            ++p;
            continue;
        }

        if(!slash_p)
            slash_p = end;

        char prev_char = *slash_p;
        *slash_p = '\0';
        int err = mkdir(path, S_IRWXU);
        *slash_p = prev_char;

        if(err == -1 && errno != EEXIST)
            return err;

        if(slash_p == end)
            break;
        else
            p = slash_p + 1;
    }
    return 0;
}

void setup_dma_buf_attrs(intptr_t *img_attr, uint32_t format, uint32_t width, uint32_t height, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, int num_planes, bool use_modifier) {
    size_t img_attr_index = 0;

    img_attr[img_attr_index++] = EGL_LINUX_DRM_FOURCC_EXT;
    img_attr[img_attr_index++] = format;

    img_attr[img_attr_index++] = EGL_WIDTH;
    img_attr[img_attr_index++] = width;

    img_attr[img_attr_index++] = EGL_HEIGHT;
    img_attr[img_attr_index++] = height;

    if(num_planes >= 1) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_FD_EXT;
        img_attr[img_attr_index++] = fds[0];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[0];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[0];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[0] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[0] >> 32ULL;
        }
    }

    if(num_planes >= 2) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_FD_EXT;
        img_attr[img_attr_index++] = fds[1];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[1];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[1];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[1] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[1] >> 32ULL;
        }
    }

    if(num_planes >= 3) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_FD_EXT;
        img_attr[img_attr_index++] = fds[2];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[2];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[2];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[2] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[2] >> 32ULL;
        }
    }

    if(num_planes >= 4) {
        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_FD_EXT;
        img_attr[img_attr_index++] = fds[3];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_OFFSET_EXT;
        img_attr[img_attr_index++] = offsets[3];

        img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_PITCH_EXT;
        img_attr[img_attr_index++] = pitches[3];

        if(use_modifier) {
            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT;
            img_attr[img_attr_index++] = modifiers[3] & 0xFFFFFFFFULL;

            img_attr[img_attr_index++] = EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT;
            img_attr[img_attr_index++] = modifiers[3] >> 32ULL;
        }
    }

    img_attr[img_attr_index++] = EGL_NONE;
    assert(img_attr_index <= 44);
}

static VADisplay video_codec_context_get_vaapi_display(AVCodecContext *video_codec_context) {
    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return NULL;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    if(device_context->type != AV_HWDEVICE_TYPE_VAAPI)
        return NULL;

    AVVAAPIDeviceContext *vactx = device_context->hwctx;
    return vactx->display;
}

bool video_codec_context_is_vaapi(AVCodecContext *video_codec_context) {
    AVBufferRef *hw_frames_ctx = video_codec_context->hw_frames_ctx;
    if(!hw_frames_ctx)
        return NULL;

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)hw_frames_ctx->data;
    AVHWDeviceContext *device_context = (AVHWDeviceContext*)hw_frame_context->device_ctx;
    return device_context->type == AV_HWDEVICE_TYPE_VAAPI;
}

static uint32_t drm_fourcc_to_va_fourcc(uint32_t drm_fourcc) {
    switch(drm_fourcc) {
        case DRM_FORMAT_XRGB8888: return VA_FOURCC_BGRX;
        case DRM_FORMAT_XBGR8888: return VA_FOURCC_RGBX;
        case DRM_FORMAT_RGBX8888: return VA_FOURCC_XBGR;
        case DRM_FORMAT_BGRX8888: return VA_FOURCC_XRGB;
        case DRM_FORMAT_ARGB8888: return VA_FOURCC_BGRA;
        case DRM_FORMAT_ABGR8888: return VA_FOURCC_RGBA;
        case DRM_FORMAT_RGBA8888: return VA_FOURCC_ABGR;
        case DRM_FORMAT_BGRA8888: return VA_FOURCC_ARGB;
        default:                  return drm_fourcc;
    }
}

bool vaapi_copy_drm_planes_to_video_surface(AVCodecContext *video_codec_context, AVFrame *video_frame, vec2i source_pos, vec2i source_size, vec2i dest_pos, vec2i dest_size, uint32_t format, vec2i size, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, int num_planes) {
    VAConfigID config_id = 0;
    VAContextID context_id = 0;
    VASurfaceID input_surface_id = 0;
    VABufferID buffer_id = 0;
    bool success = true;

    VADisplay va_dpy = video_codec_context_get_vaapi_display(video_codec_context);
    if(!va_dpy) {
        success = false;
        goto done;
    }

    VAStatus va_status = vaCreateConfig(va_dpy, VAProfileNone, VAEntrypointVideoProc, NULL, 0, &config_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaCreateConfig failed, error: %s\n", vaErrorStr(va_status));
        success = false;
        goto done;
    }

    VASurfaceID output_surface_id = (uintptr_t)video_frame->data[3];
    va_status = vaCreateContext(va_dpy, config_id, size.x, size.y, VA_PROGRESSIVE, &output_surface_id, 1, &context_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaCreateContext failed, error: %s\n", vaErrorStr(va_status));
        success = false;
        goto done;
    }

    VADRMPRIMESurfaceDescriptor buf = {0};
    buf.fourcc = drm_fourcc_to_va_fourcc(format);//VA_FOURCC_BGRX; // TODO: VA_FOURCC_BGRA, VA_FOURCC_X2R10G10B10
    buf.width = size.x;
    buf.height = size.y;
    buf.num_objects = num_planes;
    buf.num_layers = 1;
    buf.layers[0].drm_format = format;
    buf.layers[0].num_planes = buf.num_objects;
    for(int i = 0; i < num_planes; ++i) {
        buf.objects[i].fd = fds[i];
        buf.objects[i].size = size.y * pitches[i]; // TODO:
        buf.objects[i].drm_format_modifier = modifiers[i];

        buf.layers[0].object_index[i] = i;
        buf.layers[0].offset[i] = offsets[i];
        buf.layers[0].pitch[i] = pitches[i];
    }

    VASurfaceAttrib attribs[2] = {0};
    attribs[0].type = VASurfaceAttribMemoryType;
    attribs[0].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[0].value.type = VAGenericValueTypeInteger;
    attribs[0].value.value.i = VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2;
    attribs[1].type = VASurfaceAttribExternalBufferDescriptor;
    attribs[1].flags = VA_SURFACE_ATTRIB_SETTABLE;
    attribs[1].value.type = VAGenericValueTypePointer;
    attribs[1].value.value.p = &buf;
    
    // TODO: RT_FORMAT with 10 bit/hdr, VA_RT_FORMAT_RGB32_10
    // TODO: Max size same as source_size
    va_status = vaCreateSurfaces(va_dpy, VA_RT_FORMAT_RGB32, size.x, size.y, &input_surface_id, 1, attribs, 2);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaCreateSurfaces failed, error: %s\n", vaErrorStr(va_status));
        success = false;
        goto done;
    }

    const VARectangle source_region = {
        .x = source_pos.x,
        .y = source_pos.y,
        .width = source_size.x,
        .height = source_size.y
    };

    const VARectangle output_region = {
        .x = dest_pos.x,
        .y = dest_pos.y,
        .width = dest_size.x,
        .height = dest_size.y
    };

    // Copying a surface to another surface will automatically perform the color conversion. Thanks vaapi!
    VAProcPipelineParameterBuffer params = {0};
    params.surface = input_surface_id;
    params.surface_region = NULL;
    params.surface_region = &source_region;
    params.output_region = &output_region;
    params.output_background_color = 0;
    params.filter_flags = VA_FRAME_PICTURE;
    params.pipeline_flags = VA_PROC_PIPELINE_FAST;

    params.input_color_properties.colour_primaries = 1;
    params.input_color_properties.transfer_characteristics = 1;
    params.input_color_properties.matrix_coefficients = 1;
    params.surface_color_standard = VAProcColorStandardBT709; // TODO:
    params.input_color_properties.color_range = video_frame->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;

    params.output_color_properties.colour_primaries = 1;
    params.output_color_properties.transfer_characteristics = 1;
    params.output_color_properties.matrix_coefficients = 1;
    params.output_color_standard = VAProcColorStandardBT709; // TODO:
    params.output_color_properties.color_range = video_frame->color_range == AVCOL_RANGE_JPEG ? VA_SOURCE_RANGE_FULL : VA_SOURCE_RANGE_REDUCED;

    params.processing_mode = VAProcPerformanceMode;

    // VAProcPipelineCaps pipeline_caps = {0};
    // va_status = vaQueryVideoProcPipelineCaps(self->va_dpy,
    //                                    self->context_id,
    //                                    NULL, 0,
    //                                    &pipeline_caps);
    // if(va_status == VA_STATUS_SUCCESS) {
    //     fprintf(stderr, "pipeline_caps: %u, %u\n", (unsigned int)pipeline_caps.rotation_flags, pipeline_caps.blend_flags);
    // }

    // TODO: params.output_hdr_metadata

    // TODO:
    // if (first surface to render)
    //     pipeline_param->output_background_color = 0xff000000; // black

    va_status = vaCreateBuffer(va_dpy, context_id, VAProcPipelineParameterBufferType, sizeof(params), 1, &params, &buffer_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaCreateBuffer failed, error: %d\n", va_status);
        success = false;
        goto done;
    }

    va_status = vaBeginPicture(va_dpy, context_id, output_surface_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaBeginPicture failed, error: %d\n", va_status);
        success = false;
        goto done;
    }

    va_status = vaRenderPicture(va_dpy, context_id, &buffer_id, 1);
    if(va_status != VA_STATUS_SUCCESS) {
        vaEndPicture(va_dpy, context_id);
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaRenderPicture failed, error: %d\n", va_status);
        success = false;
        goto done;
    }

    va_status = vaEndPicture(va_dpy, context_id);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: vaapi_copy_drm_planes_to_video_surface: vaEndPicture failed, error: %d\n", va_status);
        success = false;
        goto done;
    }

    // vaSyncBuffer(va_dpy, buffer_id, 1000 * 1000 * 1000);
    // vaSyncSurface(va_dpy, input_surface_id);
    // vaSyncSurface(va_dpy, output_surface_id);

    done:
    if(buffer_id)
        vaDestroyBuffer(va_dpy, buffer_id);

    if(input_surface_id)
        vaDestroySurfaces(va_dpy, &input_surface_id, 1);

    if(context_id)
        vaDestroyContext(va_dpy, context_id);

    if(config_id)
        vaDestroyConfig(va_dpy, config_id);

    return success;
}

bool vaapi_copy_egl_image_to_video_surface(gsr_egl *egl, EGLImage image, vec2i source_pos, vec2i source_size, vec2i dest_pos, vec2i dest_size, AVCodecContext *video_codec_context, AVFrame *video_frame) {
    if(!image)
        return false;

    int texture_fourcc = 0;
    int texture_num_planes = 0;
    uint64_t texture_modifiers = 0;
    if(!egl->eglExportDMABUFImageQueryMESA(egl->egl_display, image, &texture_fourcc, &texture_num_planes, &texture_modifiers)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: eglExportDMABUFImageQueryMESA failed\n");
        return false;
    }

    if(texture_num_planes <= 0 || texture_num_planes > 8) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: expected planes size to be 0<planes<8 for drm buf, got %d planes\n", texture_num_planes);
        return false;
    }

    int texture_fds[8];
    int32_t texture_strides[8];
    int32_t texture_offsets[8];

    while(egl->eglGetError() != EGL_SUCCESS){}
    if(!egl->eglExportDMABUFImageMESA(egl->egl_display, image, texture_fds, texture_strides, texture_offsets)) {
        fprintf(stderr, "gsr error: gsr_capture_xcomposite_vaapi_tick: eglExportDMABUFImageMESA failed, error: %d\n", egl->eglGetError());
        return false;
    }

    int fds[8];
    uint32_t offsets[8];
    uint32_t pitches[8];
    uint64_t modifiers[8];
    for(int i = 0; i < texture_num_planes; ++i) {
        fds[i] = texture_fds[i];
        offsets[i] = texture_offsets[i];
        pitches[i] = texture_strides[i];
        modifiers[i] = texture_modifiers;

        if(fds[i] == -1)
            texture_num_planes = i;
    }
    const bool success = texture_num_planes > 0 && vaapi_copy_drm_planes_to_video_surface(video_codec_context, video_frame, source_pos, source_size, dest_pos, dest_size, texture_fourcc, source_size, fds, offsets, pitches, modifiers, texture_num_planes);

    for(int i = 0; i < texture_num_planes; ++i) {
        if(texture_fds[i] > 0) {
            close(texture_fds[i]);
            texture_fds[i] = -1;
        }
    }

    return success;
}
