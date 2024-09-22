#include "../../include/capture/kms.h"
#include "../../include/utils.h"
#include "../../include/color_conversion.h"
#include "../../include/cursor.h"
#include "../../kms/client/kms_client.h"

#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <unistd.h>
#include <fcntl.h>

#include <xf86drm.h>
#include <libdrm/drm_fourcc.h>

#include <libavcodec/avcodec.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavformat/avformat.h>

#define HDMI_STATIC_METADATA_TYPE1 0
#define HDMI_EOTF_SMPTE_ST2084 2

#define MAX_CONNECTOR_IDS 32

typedef struct {
    uint32_t connector_ids[MAX_CONNECTOR_IDS];
    int num_connector_ids;
} MonitorId;

typedef struct {
    gsr_capture_kms_params params;
    
    gsr_kms_client kms_client;
    gsr_kms_response kms_response;

    vec2i capture_pos;
    vec2i capture_size;
    MonitorId monitor_id;

    gsr_monitor_rotation monitor_rotation;

    unsigned int input_texture_id;
    unsigned int external_input_texture_id;
    unsigned int cursor_texture_id;

    bool no_modifiers_fallback;
    bool external_texture_fallback;

    struct hdr_output_metadata hdr_metadata;
    bool hdr_metadata_set;

    bool is_x11;
    gsr_cursor x11_cursor;

    AVCodecContext *video_codec_context;
    bool performance_error_shown;
    bool fast_path_failed;

    //int drm_fd;
    //uint64_t prev_sequence;
    //bool damaged;

    vec2i prev_target_pos;
    vec2i prev_plane_size;
} gsr_capture_kms;

static void gsr_capture_kms_cleanup_kms_fds(gsr_capture_kms *self) {
    for(int i = 0; i < self->kms_response.num_items; ++i) {
        for(int j = 0; j < self->kms_response.items[i].num_dma_bufs; ++j) {
            gsr_kms_response_dma_buf *dma_buf = &self->kms_response.items[i].dma_buf[j];
            if(dma_buf->fd > 0) {
                close(dma_buf->fd);
                dma_buf->fd = -1;
            }
        }
        self->kms_response.items[i].num_dma_bufs = 0;
    }
    self->kms_response.num_items = 0;
}

static void gsr_capture_kms_stop(gsr_capture_kms *self) {
    if(self->input_texture_id) {
        self->params.egl->glDeleteTextures(1, &self->input_texture_id);
        self->input_texture_id = 0;
    }

    if(self->external_input_texture_id) {
        self->params.egl->glDeleteTextures(1, &self->external_input_texture_id);
        self->external_input_texture_id = 0;
    }

    if(self->cursor_texture_id) {
        self->params.egl->glDeleteTextures(1, &self->cursor_texture_id);
        self->cursor_texture_id = 0;
    }

    // if(self->drm_fd > 0) {
    //     close(self->drm_fd);
    //     self->drm_fd = -1;
    // }

    gsr_capture_kms_cleanup_kms_fds(self);
    gsr_kms_client_deinit(&self->kms_client);
    gsr_cursor_deinit(&self->x11_cursor);
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static void gsr_capture_kms_create_input_texture_ids(gsr_capture_kms *self) {
    self->params.egl->glGenTextures(1, &self->input_texture_id);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->input_texture_id);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    self->params.egl->glGenTextures(1, &self->external_input_texture_id);
    self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, self->external_input_texture_id);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    const bool cursor_texture_id_is_external = self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA;
    const int cursor_texture_id_target = cursor_texture_id_is_external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

    self->params.egl->glGenTextures(1, &self->cursor_texture_id);
    self->params.egl->glBindTexture(cursor_texture_id_target, self->cursor_texture_id);
    self->params.egl->glTexParameteri(cursor_texture_id_target, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(cursor_texture_id_target, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(cursor_texture_id_target, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(cursor_texture_id_target, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(cursor_texture_id_target, 0);
}

/* TODO: On monitor reconfiguration, find monitor x, y, width and height again. Do the same for nvfbc. */

typedef struct {
    MonitorId *monitor_id;
    const char *monitor_to_capture;
    int monitor_to_capture_len;
    int num_monitors;
} MonitorCallbackUserdata;

static void monitor_callback(const gsr_monitor *monitor, void *userdata) {
    MonitorCallbackUserdata *monitor_callback_userdata = userdata;
    ++monitor_callback_userdata->num_monitors;

    if(monitor_callback_userdata->monitor_to_capture_len != monitor->name_len || memcmp(monitor_callback_userdata->monitor_to_capture, monitor->name, monitor->name_len) != 0)
        return;

    if(monitor_callback_userdata->monitor_id->num_connector_ids < MAX_CONNECTOR_IDS) {
        monitor_callback_userdata->monitor_id->connector_ids[monitor_callback_userdata->monitor_id->num_connector_ids] = monitor->connector_id;
        ++monitor_callback_userdata->monitor_id->num_connector_ids;
    }

    if(monitor_callback_userdata->monitor_id->num_connector_ids == MAX_CONNECTOR_IDS)
        fprintf(stderr, "gsr warning: reached max connector ids\n");
}

static vec2i rotate_capture_size_if_rotated(gsr_capture_kms *self, vec2i capture_size) {
    if(self->monitor_rotation == GSR_MONITOR_ROT_90 || self->monitor_rotation == GSR_MONITOR_ROT_270) {
        int tmp_x = capture_size.x;
        capture_size.x = capture_size.y;
        capture_size.y = tmp_x;
    }
    return capture_size;
}

static int gsr_capture_kms_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_kms *self = cap->priv;

    gsr_capture_kms_create_input_texture_ids(self);

    gsr_monitor monitor;
    self->monitor_id.num_connector_ids = 0;

    int kms_init_res = gsr_kms_client_init(&self->kms_client, self->params.egl->card_path);
    if(kms_init_res != 0)
        return kms_init_res;

    self->is_x11 = gsr_egl_get_display_server(self->params.egl) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type = self->is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;
    if(self->is_x11)
        gsr_cursor_init(&self->x11_cursor, self->params.egl, self->params.egl->x11.dpy);

    MonitorCallbackUserdata monitor_callback_userdata = {
        &self->monitor_id,
        self->params.display_to_capture, strlen(self->params.display_to_capture),
        0,
    };
    for_each_active_monitor_output(self->params.egl, connection_type, monitor_callback, &monitor_callback_userdata);

    if(!get_monitor_by_name(self->params.egl, connection_type, self->params.display_to_capture, &monitor)) {
        fprintf(stderr, "gsr error: gsr_capture_kms_start: failed to find monitor by name \"%s\"\n", self->params.display_to_capture);
        gsr_capture_kms_stop(self);
        return -1;
    }

    monitor.name = self->params.display_to_capture;
    self->monitor_rotation = drm_monitor_get_display_server_rotation(self->params.egl, &monitor);

    self->capture_pos = monitor.pos;
    /* Monitor size is already rotated on x11 when the monitor is rotated, no need to apply it ourselves */
    if(self->is_x11)
        self->capture_size = monitor.size;
    else
        self->capture_size = rotate_capture_size_if_rotated(self, monitor.size);

    /* Disable vsync */
    self->params.egl->eglSwapInterval(self->params.egl->egl_display, 0);

    video_codec_context->width = FFALIGN(self->capture_size.x, 2);
    video_codec_context->height = FFALIGN(self->capture_size.y, 2);

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->video_codec_context = video_codec_context;
    return 0;
}

static void gsr_capture_kms_on_event(gsr_capture *cap, gsr_egl *egl) {
    gsr_capture_kms *self = cap->priv;
    if(!self->is_x11)
        return;

    XEvent *xev = gsr_egl_get_event_data(egl);
    gsr_cursor_on_event(&self->x11_cursor, xev);
}

// TODO: This is disabled for now because we want to be able to record at a framerate higher than the monitor framerate
// static void gsr_capture_kms_tick(gsr_capture *cap) {
//     gsr_capture_kms *self = cap->priv;

//     if(self->drm_fd <= 0)
//         self->drm_fd = open(self->params.egl->card_path, O_RDONLY);

//     if(self->drm_fd <= 0)
//         return;

//     uint64_t sequence = 0;
//     uint64_t ns = 0;
//     if(drmCrtcGetSequence(self->drm_fd, 79, &sequence, &ns) != 0)
//         return;

//     if(sequence != self->prev_sequence) {
//         self->prev_sequence = sequence;
//         self->damaged = true;
//     }
// }

static float monitor_rotation_to_radians(gsr_monitor_rotation rot) {
    switch(rot) {
        case GSR_MONITOR_ROT_0:   return 0.0f;
        case GSR_MONITOR_ROT_90:  return M_PI_2;
        case GSR_MONITOR_ROT_180: return M_PI;
        case GSR_MONITOR_ROT_270: return M_PI + M_PI_2;
    }
    return 0.0f;
}

static gsr_kms_response_item* find_drm_by_connector_id(gsr_kms_response *kms_response, uint32_t connector_id) {
    for(int i = 0; i < kms_response->num_items; ++i) {
        if(kms_response->items[i].connector_id == connector_id && !kms_response->items[i].is_cursor)
            return &kms_response->items[i];
    }
    return NULL;
}

static gsr_kms_response_item* find_largest_drm(gsr_kms_response *kms_response) {
    if(kms_response->num_items == 0)
        return NULL;

    int64_t largest_size = 0;
    gsr_kms_response_item *largest_drm = &kms_response->items[0];
    for(int i = 0; i < kms_response->num_items; ++i) {
        const int64_t size = (int64_t)kms_response->items[i].width * (int64_t)kms_response->items[i].height;
        if(size > largest_size && !kms_response->items[i].is_cursor) {
            largest_size = size;
            largest_drm = &kms_response->items[i];
        }
    }
    return largest_drm;
}

static gsr_kms_response_item* find_cursor_drm(gsr_kms_response *kms_response, uint32_t connector_id) {
    gsr_kms_response_item *cursor_drm = NULL;
    for(int i = 0; i < kms_response->num_items; ++i) {
        if(kms_response->items[i].is_cursor) {
            cursor_drm = &kms_response->items[i];
            if(kms_response->items[i].connector_id == connector_id)
                break;
        }
    }
    return cursor_drm;
}

static bool hdr_metadata_is_supported_format(const struct hdr_output_metadata *hdr_metadata) {
    return hdr_metadata->metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.metadata_type == HDMI_STATIC_METADATA_TYPE1 &&
        hdr_metadata->hdmi_metadata_type1.eotf == HDMI_EOTF_SMPTE_ST2084;
}

// TODO: Check if this hdr data can be changed after the call to av_packet_side_data_add
static void gsr_kms_set_hdr_metadata(gsr_capture_kms *self, const gsr_kms_response_item *drm_fd) {
    if(self->hdr_metadata_set)
        return;

    self->hdr_metadata_set = true;
    self->hdr_metadata = drm_fd->hdr_metadata;
}

static vec2i swap_vec2i(vec2i value) {
    int tmp = value.x;
    value.x = value.y;
    value.y = tmp;
    return value;
}

static EGLImage gsr_capture_kms_create_egl_image(gsr_capture_kms *self, const gsr_kms_response_item *drm_fd, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, bool use_modifiers) {
    intptr_t img_attr[44];
    setup_dma_buf_attrs(img_attr, drm_fd->pixel_format, drm_fd->width, drm_fd->height, fds, offsets, pitches, modifiers, drm_fd->num_dma_bufs, use_modifiers);
    while(self->params.egl->eglGetError() != EGL_SUCCESS){}
    EGLImage image = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    if(!image || self->params.egl->eglGetError() != EGL_SUCCESS) {
        if(image)
            self->params.egl->eglDestroyImage(self->params.egl->egl_display, image);
        return NULL;
    }
    return image;
}

static EGLImage gsr_capture_kms_create_egl_image_with_fallback(gsr_capture_kms *self, const gsr_kms_response_item *drm_fd) {
    // TODO: This causes a crash sometimes on steam deck, why? is it a driver bug? a vaapi pure version doesn't cause a crash.
    // Even ffmpeg kmsgrab causes this crash. The error is:
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // amdgpu: Failed to allocate a buffer:
    // amdgpu:    size      : 28508160 bytes
    // amdgpu:    alignment : 2097152 bytes
    // amdgpu:    domains   : 4
    // amdgpu:    flags   : 4
    // EE ../jupiter-mesa/src/gallium/drivers/radeonsi/radeon_vcn_enc.c:516 radeon_create_encoder UVD - Can't create CPB buffer.
    // [hevc_vaapi @ 0x55ea72b09840] Failed to upload encode parameters: 2 (resource allocation failed).
    // [hevc_vaapi @ 0x55ea72b09840] Encode failed: -5.
    // Error: avcodec_send_frame failed, error: Input/output error
    // Assertion pic->display_order == pic->encode_order failed at libavcodec/vaapi_encode_h265.c:765
    // kms server info: kms client shutdown, shutting down the server

    int fds[GSR_KMS_MAX_DMA_BUFS];
    uint32_t offsets[GSR_KMS_MAX_DMA_BUFS];
    uint32_t pitches[GSR_KMS_MAX_DMA_BUFS];
    uint64_t modifiers[GSR_KMS_MAX_DMA_BUFS];

    for(int i = 0; i < drm_fd->num_dma_bufs; ++i) {
        fds[i] = drm_fd->dma_buf[i].fd;
        offsets[i] = drm_fd->dma_buf[i].offset;
        pitches[i] = drm_fd->dma_buf[i].pitch;
        modifiers[i] = drm_fd->modifier;
    }

    EGLImage image = NULL;
    if(self->no_modifiers_fallback) {
        image = gsr_capture_kms_create_egl_image(self, drm_fd, fds, offsets, pitches, modifiers, false);
    } else {
        image = gsr_capture_kms_create_egl_image(self, drm_fd, fds, offsets, pitches, modifiers, true);
        if(!image) {
            fprintf(stderr, "gsr error: gsr_capture_kms_create_egl_image_with_fallback: failed to create egl image with modifiers, trying without modifiers\n");
            self->no_modifiers_fallback = true;
            image = gsr_capture_kms_create_egl_image(self, drm_fd, fds, offsets, pitches, modifiers, false);
        }
    }
    return image;
}

static bool gsr_capture_kms_bind_image_to_texture(gsr_capture_kms *self, EGLImage image, unsigned int texture_id, bool external_texture) {
    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    while(self->params.egl->glGetError() != 0){}
    self->params.egl->glBindTexture(texture_target, texture_id);
    self->params.egl->glEGLImageTargetTexture2DOES(texture_target, image);
    const bool success = self->params.egl->glGetError() == 0;
    self->params.egl->glBindTexture(texture_target, 0);
    return success;
}

static void gsr_capture_kms_bind_image_to_input_texture_with_fallback(gsr_capture_kms *self, EGLImage image) {
    if(self->external_texture_fallback) {
        gsr_capture_kms_bind_image_to_texture(self, image, self->external_input_texture_id, true);
    } else {
        if(!gsr_capture_kms_bind_image_to_texture(self, image, self->input_texture_id, false)) {
            fprintf(stderr, "gsr error: gsr_capture_kms_capture: failed to bind image to texture, trying with external texture\n");
            self->external_texture_fallback = true;
            gsr_capture_kms_bind_image_to_texture(self, image, self->external_input_texture_id, true);
        }
    }
}

static gsr_kms_response_item* find_monitor_drm(gsr_capture_kms *self, bool *capture_is_combined_plane) {
    *capture_is_combined_plane = false;
    gsr_kms_response_item *drm_fd = NULL;

    for(int i = 0; i < self->monitor_id.num_connector_ids; ++i) {
        drm_fd = find_drm_by_connector_id(&self->kms_response, self->monitor_id.connector_ids[i]);
        if(drm_fd)
            break;
    }

    // Will never happen on wayland unless the target monitor has been disconnected
    if(!drm_fd) {
        drm_fd = find_largest_drm(&self->kms_response);
        *capture_is_combined_plane = true;
    }

    return drm_fd;
}

static gsr_kms_response_item* find_cursor_drm_if_on_monitor(gsr_capture_kms *self, uint32_t monitor_connector_id, bool capture_is_combined_plane) {
    gsr_kms_response_item *cursor_drm_fd = find_cursor_drm(&self->kms_response, monitor_connector_id);
    if(!capture_is_combined_plane && cursor_drm_fd && cursor_drm_fd->connector_id != monitor_connector_id)
        cursor_drm_fd = NULL;
    return cursor_drm_fd;
}

static void render_drm_cursor(gsr_capture_kms *self, gsr_color_conversion *color_conversion, const gsr_kms_response_item *cursor_drm_fd, vec2i target_pos, float texture_rotation) {
    const bool cursor_texture_id_is_external = self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA;
    const vec2i cursor_size = {cursor_drm_fd->width, cursor_drm_fd->height};

    vec2i cursor_pos = {cursor_drm_fd->x, cursor_drm_fd->y};
    switch(self->monitor_rotation) {
        case GSR_MONITOR_ROT_0:
            break;
        case GSR_MONITOR_ROT_90:
            cursor_pos = swap_vec2i(cursor_pos);
            cursor_pos.x = self->capture_size.x - cursor_pos.x;
            // TODO: Remove this horrible hack
            cursor_pos.x -= cursor_size.x;
            break;
        case GSR_MONITOR_ROT_180:
            cursor_pos.x = self->capture_size.x - cursor_pos.x;
            cursor_pos.y = self->capture_size.y - cursor_pos.y;
            // TODO: Remove this horrible hack
            cursor_pos.x -= cursor_size.x;
            cursor_pos.y -= cursor_size.y;
            break;
        case GSR_MONITOR_ROT_270:
            cursor_pos = swap_vec2i(cursor_pos);
            cursor_pos.y = self->capture_size.y - cursor_pos.y;
            // TODO: Remove this horrible hack
            cursor_pos.y -= cursor_size.y;
            break;
    }

    cursor_pos.x += target_pos.x;
    cursor_pos.y += target_pos.y;

    int fds[GSR_KMS_MAX_DMA_BUFS];
    uint32_t offsets[GSR_KMS_MAX_DMA_BUFS];
    uint32_t pitches[GSR_KMS_MAX_DMA_BUFS];
    uint64_t modifiers[GSR_KMS_MAX_DMA_BUFS];

    for(int i = 0; i < cursor_drm_fd->num_dma_bufs; ++i) {
        fds[i] = cursor_drm_fd->dma_buf[i].fd;
        offsets[i] = cursor_drm_fd->dma_buf[i].offset;
        pitches[i] = cursor_drm_fd->dma_buf[i].pitch;
        modifiers[i] = cursor_drm_fd->modifier;
    }

    intptr_t img_attr_cursor[44];
    setup_dma_buf_attrs(img_attr_cursor, cursor_drm_fd->pixel_format, cursor_drm_fd->width, cursor_drm_fd->height,
        fds, offsets, pitches, modifiers, cursor_drm_fd->num_dma_bufs, true);

    EGLImage cursor_image = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr_cursor);
    const int target = cursor_texture_id_is_external ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    self->params.egl->glBindTexture(target, self->cursor_texture_id);
    self->params.egl->glEGLImageTargetTexture2DOES(target, cursor_image);
    self->params.egl->glBindTexture(target, 0);

    if(cursor_image)
        self->params.egl->eglDestroyImage(self->params.egl->egl_display, cursor_image);

    self->params.egl->glEnable(GL_SCISSOR_TEST);
    self->params.egl->glScissor(target_pos.x, target_pos.y, self->capture_size.x, self->capture_size.y);

    gsr_color_conversion_draw(color_conversion, self->cursor_texture_id,
        cursor_pos, cursor_size,
        (vec2i){0, 0}, cursor_size,
        texture_rotation, cursor_texture_id_is_external);

    self->params.egl->glDisable(GL_SCISSOR_TEST);
}

static void render_x11_cursor(gsr_capture_kms *self, gsr_color_conversion *color_conversion, vec2i capture_pos, vec2i target_pos) {
    if(!self->x11_cursor.visible)
        return;

    gsr_cursor_tick(&self->x11_cursor, DefaultRootWindow(self->params.egl->x11.dpy));

    const vec2i cursor_pos = {
        target_pos.x + self->x11_cursor.position.x - self->x11_cursor.hotspot.x - capture_pos.x,
        target_pos.y + self->x11_cursor.position.y - self->x11_cursor.hotspot.y - capture_pos.y
    };

    self->params.egl->glEnable(GL_SCISSOR_TEST);
    self->params.egl->glScissor(target_pos.x, target_pos.y, self->capture_size.x, self->capture_size.y);

    gsr_color_conversion_draw(color_conversion, self->x11_cursor.texture_id,
        cursor_pos, self->x11_cursor.size,
        (vec2i){0, 0}, self->x11_cursor.size,
        0.0f, false);

    self->params.egl->glDisable(GL_SCISSOR_TEST);
}

static void gsr_capture_kms_update_capture_size_change(gsr_capture_kms *self, gsr_color_conversion *color_conversion, vec2i target_pos, const gsr_kms_response_item *drm_fd) {
    if(target_pos.x != self->prev_target_pos.x || target_pos.y != self->prev_target_pos.y || drm_fd->src_w != self->prev_plane_size.x || drm_fd->src_h != self->prev_plane_size.y) {
        self->prev_target_pos = target_pos;
        self->prev_plane_size = self->capture_size;
        gsr_color_conversion_clear(color_conversion);
    }
}

static int gsr_capture_kms_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_capture_kms *self = cap->priv;

    gsr_capture_kms_cleanup_kms_fds(self);

    if(gsr_kms_client_get_kms(&self->kms_client, &self->kms_response) != 0) {
        fprintf(stderr, "gsr error: gsr_capture_kms_capture: failed to get kms, error: %d (%s)\n", self->kms_response.result, self->kms_response.err_msg);
        return -1;
    }

    if(self->kms_response.num_items == 0) {
        static bool error_shown = false;
        if(!error_shown) {
            error_shown = true;
            fprintf(stderr, "gsr error: no drm found, capture will fail\n");
        }
        return -1;
    }

    bool capture_is_combined_plane = false;
    const gsr_kms_response_item *drm_fd = find_monitor_drm(self, &capture_is_combined_plane);
    if(!drm_fd) {
        gsr_capture_kms_cleanup_kms_fds(self);
        return -1;
    }

    if(drm_fd->has_hdr_metadata && self->params.hdr && hdr_metadata_is_supported_format(&drm_fd->hdr_metadata))
        gsr_kms_set_hdr_metadata(self, drm_fd);

    if(!self->performance_error_shown && self->monitor_rotation != GSR_MONITOR_ROT_0 && video_codec_context_is_vaapi(self->video_codec_context) && self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD) {
        self->performance_error_shown = true;
        fprintf(stderr,"gsr warning: gsr_capture_kms_capture: the monitor you are recording is rotated, composition will have to be used."
            " If you are experience performance problems in the video then record a single window on X11 or use portal capture option instead\n");
    }

    const float texture_rotation = monitor_rotation_to_radians(self->monitor_rotation);
    const vec2i target_pos = { max_int(0, frame->width / 2 - self->capture_size.x / 2), max_int(0, frame->height / 2 - self->capture_size.y / 2) };
    self->capture_size = rotate_capture_size_if_rotated(self, (vec2i){ drm_fd->src_w, drm_fd->src_h });
    gsr_capture_kms_update_capture_size_change(self, color_conversion, target_pos, drm_fd);

    vec2i capture_pos = self->capture_pos;
    if(!capture_is_combined_plane)
        capture_pos = (vec2i){drm_fd->x, drm_fd->y};

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    /* Fast opengl free path */
    if(!self->fast_path_failed && self->monitor_rotation == GSR_MONITOR_ROT_0 && video_codec_context_is_vaapi(self->video_codec_context) && self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD) {
        int fds[4];
        uint32_t offsets[4];
        uint32_t pitches[4];
        uint64_t modifiers[4];
        for(int i = 0; i < drm_fd->num_dma_bufs; ++i) {
            fds[i] = drm_fd->dma_buf[i].fd;
            offsets[i] = drm_fd->dma_buf[i].offset;
            pitches[i] = drm_fd->dma_buf[i].pitch;
            modifiers[i] = drm_fd->modifier;
        }
        if(!vaapi_copy_drm_planes_to_video_surface(self->video_codec_context, frame, (vec2i){capture_pos.x, capture_pos.y}, self->capture_size, target_pos, self->capture_size, drm_fd->pixel_format, (vec2i){drm_fd->width, drm_fd->height}, fds, offsets, pitches, modifiers, drm_fd->num_dma_bufs)) {
            fprintf(stderr, "gsr error: gsr_capture_kms_capture: vaapi_copy_drm_planes_to_video_surface failed, falling back to opengl copy. Please report this as an issue at https://github.com/dec05eba/gpu-screen-recorder-issues\n");
            self->fast_path_failed = true;
        }
    } else {
        self->fast_path_failed = true;
    }

    if(self->fast_path_failed) {
        EGLImage image = gsr_capture_kms_create_egl_image_with_fallback(self, drm_fd);
        if(image) {
            gsr_capture_kms_bind_image_to_input_texture_with_fallback(self, image);
            self->params.egl->eglDestroyImage(self->params.egl->egl_display, image);
        }

        gsr_color_conversion_draw(color_conversion, self->external_texture_fallback ? self->external_input_texture_id : self->input_texture_id,
            target_pos, self->capture_size,
            capture_pos, self->capture_size,
            texture_rotation, self->external_texture_fallback);
    }

    if(self->params.record_cursor) {
        gsr_kms_response_item *cursor_drm_fd = find_cursor_drm_if_on_monitor(self, drm_fd->connector_id, capture_is_combined_plane);
        // The cursor is handled by x11 on x11 instead of using the cursor drm plane because on prime systems with a dedicated nvidia gpu
        // the cursor plane is not available when the cursor is on the monitor controlled by the nvidia device.
        if(self->is_x11) {
            const vec2i cursor_monitor_offset = self->capture_pos;
            render_x11_cursor(self, color_conversion, cursor_monitor_offset, target_pos);
        } else if(cursor_drm_fd) {
            render_drm_cursor(self, color_conversion, cursor_drm_fd, target_pos, texture_rotation);
        }
    }

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    gsr_capture_kms_cleanup_kms_fds(self);

    return 0;
}

static bool gsr_capture_kms_should_stop(gsr_capture *cap, bool *err) {
    (void)cap;
    if(err)
        *err = false;
    return false;
}

static gsr_source_color gsr_capture_kms_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_RGB;
}

static bool gsr_capture_kms_uses_external_image(gsr_capture *cap) {
    (void)cap;
    return true;
}

static bool gsr_capture_kms_set_hdr_metadata(gsr_capture *cap, AVMasteringDisplayMetadata *mastering_display_metadata, AVContentLightMetadata *light_metadata) {
    gsr_capture_kms *self = cap->priv;

    if(!self->hdr_metadata_set)
        return false;

    light_metadata->MaxCLL = self->hdr_metadata.hdmi_metadata_type1.max_cll;
    light_metadata->MaxFALL = self->hdr_metadata.hdmi_metadata_type1.max_fall;

    for(int i = 0; i < 3; ++i) {
        mastering_display_metadata->display_primaries[i][0] = av_make_q(self->hdr_metadata.hdmi_metadata_type1.display_primaries[i].x, 50000);
        mastering_display_metadata->display_primaries[i][1] = av_make_q(self->hdr_metadata.hdmi_metadata_type1.display_primaries[i].y, 50000);
    }

    mastering_display_metadata->white_point[0] = av_make_q(self->hdr_metadata.hdmi_metadata_type1.white_point.x, 50000);
    mastering_display_metadata->white_point[1] = av_make_q(self->hdr_metadata.hdmi_metadata_type1.white_point.y, 50000);

    mastering_display_metadata->min_luminance = av_make_q(self->hdr_metadata.hdmi_metadata_type1.min_display_mastering_luminance, 10000);
    mastering_display_metadata->max_luminance = av_make_q(self->hdr_metadata.hdmi_metadata_type1.max_display_mastering_luminance, 1);

    mastering_display_metadata->has_primaries = mastering_display_metadata->display_primaries[0][0].num > 0;
    mastering_display_metadata->has_luminance = mastering_display_metadata->max_luminance.num > 0;

    return true;
}

// static bool gsr_capture_kms_is_damaged(gsr_capture *cap) {
//     gsr_capture_kms *self = cap->priv;
//     return self->damaged;
// }

// static void gsr_capture_kms_clear_damage(gsr_capture *cap) {
//     gsr_capture_kms *self = cap->priv;
//     self->damaged = false;
// }

static void gsr_capture_kms_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_kms *self = cap->priv;
    if(cap->priv) {
        gsr_capture_kms_stop(self);
        free((void*)self->params.display_to_capture);
        self->params.display_to_capture = NULL;
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_kms_create(const gsr_capture_kms_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_kms_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_kms *cap_kms = calloc(1, sizeof(gsr_capture_kms));
    if(!cap_kms) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_kms);
        return NULL;
    }

    cap_kms->params = *params;
    cap_kms->params.display_to_capture = display_to_capture;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_kms_start,
        .on_event = gsr_capture_kms_on_event,
        //.tick = gsr_capture_kms_tick,
        .should_stop = gsr_capture_kms_should_stop,
        .capture = gsr_capture_kms_capture,
        .get_source_color = gsr_capture_kms_get_source_color,
        .uses_external_image = gsr_capture_kms_uses_external_image,
        .set_hdr_metadata = gsr_capture_kms_set_hdr_metadata,
        //.is_damaged = gsr_capture_kms_is_damaged,
        //.clear_damage = gsr_capture_kms_clear_damage,
        .destroy = gsr_capture_kms_destroy,
        .priv = cap_kms
    };

    return cap;
}
