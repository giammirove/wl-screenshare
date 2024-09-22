#include "../include/pipewire.h"
#include "../include/egl.h"
#include "../include/utils.h"

#include <pipewire/pipewire.h>
#include <spa/param/video/format-utils.h>
#include <spa/debug/types.h>

#include <libdrm/drm_fourcc.h>

#include <fcntl.h>
#include <unistd.h>

/* This code is partially based on xr-video-player pipewire implementation which is based on obs-studio's pipewire implementation */

/* TODO: Make gsr_pipewire_init asynchronous */
/* TODO: Support 10-bit capture (hdr) when pipewire supports it */
/* TODO: Test all of the image formats */

#ifndef SPA_POD_PROP_FLAG_DONT_FIXATE
#define SPA_POD_PROP_FLAG_DONT_FIXATE (1 << 4)
#endif

#define CURSOR_META_SIZE(width, height)                                    \
    (sizeof(struct spa_meta_cursor) + sizeof(struct spa_meta_bitmap) + \
     width * height * 4)

static bool parse_pw_version(gsr_pipewire_data_version *dst, const char *version) {
    const int n_matches = sscanf(version, "%d.%d.%d", &dst->major, &dst->minor, &dst->micro);
    return n_matches == 3;
}

static bool check_pw_version(const gsr_pipewire_data_version *pw_version, int major, int minor, int micro) {
    if (pw_version->major != major)
        return pw_version->major > major;
    if (pw_version->minor != minor)
        return pw_version->minor > minor;
    return pw_version->micro >= micro;
}

static void update_pw_versions(gsr_pipewire *self, const char *version) {
    fprintf(stderr, "gsr info: pipewire: server version: %s\n", version);
    fprintf(stderr, "gsr info: pipewire: library version: %s\n", pw_get_library_version());
    fprintf(stderr, "gsr info: pipewire: header version: %s\n", pw_get_headers_version());
    if(!parse_pw_version(&self->server_version, version))
        fprintf(stderr, "gsr error: pipewire: failed to parse server version\n");
}

static void on_core_info_cb(void *user_data, const struct pw_core_info *info) {
    gsr_pipewire *self = user_data;
    update_pw_versions(self, info->version);
}

static void on_core_error_cb(void *user_data, uint32_t id, int seq, int res, const char *message) {
    gsr_pipewire *self = user_data;
    fprintf(stderr, "gsr error: pipewire: error id:%u seq:%d res:%d: %s\n", id, seq, res, message);
    pw_thread_loop_signal(self->thread_loop, false);
}

static void on_core_done_cb(void *user_data, uint32_t id, int seq) {
    gsr_pipewire *self = user_data;
    if (id == PW_ID_CORE && self->server_version_sync == seq)
        pw_thread_loop_signal(self->thread_loop, false);
}

static bool is_cursor_format_supported(const enum spa_video_format format) {
    switch(format) {
        case SPA_VIDEO_FORMAT_RGBx: return true;
        case SPA_VIDEO_FORMAT_BGRx: return true;
        case SPA_VIDEO_FORMAT_xRGB: return true;
        case SPA_VIDEO_FORMAT_xBGR: return true;
        case SPA_VIDEO_FORMAT_RGBA: return true;
        case SPA_VIDEO_FORMAT_BGRA: return true;
        case SPA_VIDEO_FORMAT_ARGB: return true;
        case SPA_VIDEO_FORMAT_ABGR: return true;
        default:                    break;
    }
    return false;
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .info = on_core_info_cb,
    .done = on_core_done_cb,
    .error = on_core_error_cb,
};

static void on_process_cb(void *user_data) {
    gsr_pipewire *self = user_data;
    struct spa_meta_cursor *cursor = NULL;
    //struct spa_meta *video_damage = NULL;

    /* Find the most recent buffer */
    struct pw_buffer *pw_buf = NULL;
    for(;;) {
        struct pw_buffer *aux = pw_stream_dequeue_buffer(self->stream);
        if(!aux)
            break;
        if(pw_buf)
            pw_stream_queue_buffer(self->stream, pw_buf);
        pw_buf = aux;
    }

    if(!pw_buf) {
        fprintf(stderr, "gsr info: pipewire: out of buffers!\n");
        return;
    }

    struct spa_buffer *buffer = pw_buf->buffer;
    const bool has_buffer = buffer->datas[0].chunk->size != 0;
    if(!has_buffer)
        goto read_metadata;

    pthread_mutex_lock(&self->mutex);

    if(buffer->datas[0].type == SPA_DATA_DmaBuf) {
        for(size_t i = 0; i < self->dmabuf_num_planes; ++i) {
            if(self->dmabuf_data[i].fd > 0) {
                close(self->dmabuf_data[i].fd);
                self->dmabuf_data[i].fd = -1;
            }
        }

        self->dmabuf_num_planes = buffer->n_datas;
        if(self->dmabuf_num_planes > GSR_PIPEWIRE_DMABUF_MAX_PLANES)
            self->dmabuf_num_planes = GSR_PIPEWIRE_DMABUF_MAX_PLANES;

        for(size_t i = 0; i < self->dmabuf_num_planes; ++i) {
            self->dmabuf_data[i].fd = dup(buffer->datas[i].fd);
            self->dmabuf_data[i].offset = buffer->datas[i].chunk->offset;
            self->dmabuf_data[i].stride = buffer->datas[i].chunk->stride;
        }

        self->damaged = true;
    } else {
        // TODO:
    }

    // TODO: Move down to read_metadata
    struct spa_meta_region *region = spa_buffer_find_meta_data(buffer, SPA_META_VideoCrop, sizeof(*region));
    if(region && spa_meta_region_is_valid(region)) {
        // fprintf(stderr, "gsr info: pipewire: crop Region available (%dx%d+%d+%d)\n",
        //      region->region.position.x, region->region.position.y,
        //      region->region.size.width, region->region.size.height);
        self->crop.x = region->region.position.x;
        self->crop.y = region->region.position.y;
        self->crop.width = region->region.size.width;
        self->crop.height = region->region.size.height;
        self->crop.valid = true;
    } else {
        self->crop.valid = false;
    }

    pthread_mutex_unlock(&self->mutex);

read_metadata:

    // video_damage = spa_buffer_find_meta(buffer, SPA_META_VideoDamage);
    // if(video_damage) {
    //     struct spa_meta_region *r = spa_meta_first(video_damage);
    //     if(spa_meta_check(r, video_damage)) {
    //         //fprintf(stderr, "damage: %d,%d %ux%u\n", r->region.position.x, r->region.position.y, r->region.size.width, r->region.size.height);
    //         pthread_mutex_lock(&self->mutex);
    //         self->damaged = true;
    //         pthread_mutex_unlock(&self->mutex);
    //     }
    // }

    cursor = spa_buffer_find_meta_data(buffer, SPA_META_Cursor, sizeof(*cursor));
    self->cursor.valid = cursor && spa_meta_cursor_is_valid(cursor);
    
    if (self->cursor.visible && self->cursor.valid) {
        pthread_mutex_lock(&self->mutex);

        struct spa_meta_bitmap *bitmap = NULL;
        if (cursor->bitmap_offset)
            bitmap = SPA_MEMBER(cursor, cursor->bitmap_offset, struct spa_meta_bitmap);

        if (bitmap && bitmap->size.width > 0 && bitmap->size.height && is_cursor_format_supported(bitmap->format)) {
            const uint8_t *bitmap_data = SPA_MEMBER(bitmap, bitmap->offset, uint8_t);
            fprintf(stderr, "gsr info: pipewire: cursor bitmap update, size: %dx%d, format: %s\n",
                (int)bitmap->size.width, (int)bitmap->size.height, spa_debug_type_find_name(spa_type_video_format, bitmap->format));

            const size_t bitmap_size = bitmap->size.width * bitmap->size.height * 4;
            uint8_t *new_bitmap_data = realloc(self->cursor.data, bitmap_size);
            if(new_bitmap_data) {
                self->cursor.data = new_bitmap_data;
                /* TODO: Convert bgr and other image formats to rgb here */
                memcpy(self->cursor.data, bitmap_data, bitmap_size);
            }
        
            self->cursor.hotspot_x = cursor->hotspot.x;
            self->cursor.hotspot_y = cursor->hotspot.y;
            self->cursor.width = bitmap->size.width;
            self->cursor.height = bitmap->size.height;
        }

        self->cursor.x = cursor->position.x;
        self->cursor.y = cursor->position.y;
        pthread_mutex_unlock(&self->mutex);

        //fprintf(stderr, "gsr info: pipewire: cursor: %d %d %d %d\n", cursor->hotspot.x, cursor->hotspot.y, cursor->position.x, cursor->position.y);
    }

    pw_stream_queue_buffer(self->stream, pw_buf);
}

static void on_param_changed_cb(void *user_data, uint32_t id, const struct spa_pod *param) {
    gsr_pipewire *self = user_data;

    if (!param || id != SPA_PARAM_Format)
        return;

    int result = spa_format_parse(param, &self->format.media_type, &self->format.media_subtype);
    if (result < 0)
        return;

    if (self->format.media_type != SPA_MEDIA_TYPE_video || self->format.media_subtype != SPA_MEDIA_SUBTYPE_raw)
        return;

    pthread_mutex_lock(&self->mutex);
    spa_format_video_raw_parse(param, &self->format.info.raw);
    pthread_mutex_unlock(&self->mutex);

    uint32_t buffer_types = 0;
    const bool has_modifier = spa_pod_find_prop(param, NULL, SPA_FORMAT_VIDEO_modifier) != NULL;
    if(has_modifier || check_pw_version(&self->server_version, 0, 3, 24))
        buffer_types |= 1 << SPA_DATA_DmaBuf;

    fprintf(stderr, "gsr info: pipewire: negotiated format:\n");

    fprintf(stderr, "gsr info: pipewire:    Format: %d (%s)\n",
         self->format.info.raw.format,
         spa_debug_type_find_name(spa_type_video_format, self->format.info.raw.format));

    if(has_modifier) {
        fprintf(stderr, "gsr info: pipewire:    Modifier: 0x%" PRIx64 "\n", self->format.info.raw.modifier);
    }

    fprintf(stderr, "gsr info: pipewire:    Size: %dx%d\n", self->format.info.raw.size.width, self->format.info.raw.size.height);
    fprintf(stderr, "gsr info: pipewire:    Framerate: %d/%d\n", self->format.info.raw.framerate.num, self->format.info.raw.framerate.denom);

    uint8_t params_buffer[1024];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    const struct spa_pod *params[4];

    params[0] = spa_pod_builder_add_object(
        &pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoCrop),
        SPA_PARAM_META_size,
        SPA_POD_Int(sizeof(struct spa_meta_region)));

    params[1] = spa_pod_builder_add_object(
        &pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_VideoDamage),
        SPA_PARAM_META_size, SPA_POD_CHOICE_RANGE_Int(
                                sizeof(struct spa_meta_region) * 16,
                                sizeof(struct spa_meta_region) * 1,
                                sizeof(struct spa_meta_region) * 16));

    params[2] = spa_pod_builder_add_object(
        &pod_builder, SPA_TYPE_OBJECT_ParamMeta, SPA_PARAM_Meta,
        SPA_PARAM_META_type, SPA_POD_Id(SPA_META_Cursor),
        SPA_PARAM_META_size,
        SPA_POD_CHOICE_RANGE_Int(CURSOR_META_SIZE(64, 64),
                     CURSOR_META_SIZE(1, 1),
                     CURSOR_META_SIZE(1024, 1024)));

    params[3] = spa_pod_builder_add_object(
        &pod_builder, SPA_TYPE_OBJECT_ParamBuffers, SPA_PARAM_Buffers,
        SPA_PARAM_BUFFERS_dataType, SPA_POD_Int(buffer_types));

    pw_stream_update_params(self->stream, params, 4);
    self->negotiated = true;
}

static void on_state_changed_cb(void *user_data, enum pw_stream_state old, enum pw_stream_state state, const char *error) {
    (void)old;
    gsr_pipewire *self = user_data;

    fprintf(stderr, "gsr info: pipewire: stream %p state: \"%s\" (error: %s)\n",
         (void*)self->stream, pw_stream_state_as_string(state),
         error ? error : "none");
}

static const struct pw_stream_events stream_events = {
    PW_VERSION_STREAM_EVENTS,
    .state_changed = on_state_changed_cb,
    .param_changed = on_param_changed_cb,
    .process = on_process_cb,
};

static inline struct spa_pod *build_format(struct spa_pod_builder *b,
                       const gsr_pipewire_video_info *ovi,
                       uint32_t format, const uint64_t *modifiers,
                       size_t modifier_count)
{
    struct spa_pod_frame format_frame;

    spa_pod_builder_push_object(b, &format_frame, SPA_TYPE_OBJECT_Format, SPA_PARAM_EnumFormat);
    spa_pod_builder_add(b, SPA_FORMAT_mediaType, SPA_POD_Id(SPA_MEDIA_TYPE_video), 0);
    spa_pod_builder_add(b, SPA_FORMAT_mediaSubtype, SPA_POD_Id(SPA_MEDIA_SUBTYPE_raw), 0);

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_format, SPA_POD_Id(format), 0);

    if (modifier_count > 0) {
        struct spa_pod_frame modifier_frame;

        spa_pod_builder_prop(b, SPA_FORMAT_VIDEO_modifier, SPA_POD_PROP_FLAG_MANDATORY | SPA_POD_PROP_FLAG_DONT_FIXATE);
        spa_pod_builder_push_choice(b, &modifier_frame, SPA_CHOICE_Enum, 0);

        /* The first element of choice pods is the preferred value. Here
         * we arbitrarily pick the first modifier as the preferred one.
         */
        // TODO:
        spa_pod_builder_long(b, modifiers[0]);

        for(uint32_t i = 0; i < modifier_count; i++)
            spa_pod_builder_long(b, modifiers[i]);

        spa_pod_builder_pop(b, &modifier_frame);
    }

    spa_pod_builder_add(b, SPA_FORMAT_VIDEO_size,
                SPA_POD_CHOICE_RANGE_Rectangle(
                    &SPA_RECTANGLE(32, 32),
                    &SPA_RECTANGLE(1, 1),
                    &SPA_RECTANGLE(16384, 16384)),
                SPA_FORMAT_VIDEO_framerate,
                SPA_POD_CHOICE_RANGE_Fraction(
                    &SPA_FRACTION(ovi->fps_num, ovi->fps_den),
                    &SPA_FRACTION(0, 1), &SPA_FRACTION(500, 1)),
                0);
    return spa_pod_builder_pop(b, &format_frame);
}

/* https://gstreamer.freedesktop.org/documentation/additional/design/mediatype-video-raw.html?gi-language=c#formats */
/* For some reason gstreamer formats are in opposite order to drm formats */
static int64_t spa_video_format_to_drm_format(const enum spa_video_format format) {
    switch(format) {
        case SPA_VIDEO_FORMAT_RGBx: return DRM_FORMAT_XBGR8888;
        case SPA_VIDEO_FORMAT_BGRx: return DRM_FORMAT_XRGB8888;
        case SPA_VIDEO_FORMAT_RGBA: return DRM_FORMAT_ABGR8888;
        case SPA_VIDEO_FORMAT_BGRA: return DRM_FORMAT_ARGB8888;
        case SPA_VIDEO_FORMAT_RGB:  return DRM_FORMAT_XBGR8888;
        case SPA_VIDEO_FORMAT_BGR:  return DRM_FORMAT_XRGB8888;
        default:                    break;
    }
    return DRM_FORMAT_INVALID;
}

static const enum spa_video_format video_formats[] = {
    SPA_VIDEO_FORMAT_BGRA,
    SPA_VIDEO_FORMAT_BGRx,
    SPA_VIDEO_FORMAT_BGR,
    SPA_VIDEO_FORMAT_RGBx,
    SPA_VIDEO_FORMAT_RGBA,
    SPA_VIDEO_FORMAT_RGB,
};

static bool gsr_pipewire_build_format_params(gsr_pipewire *self, struct spa_pod_builder *pod_builder, struct spa_pod **params, uint32_t *num_params) {
    *num_params = 0;

    if(!check_pw_version(&self->server_version, 0, 3, 33))
        return false;

    for(size_t i = 0; i < GSR_PIPEWIRE_NUM_VIDEO_FORMATS; i++) {
        if(self->supported_video_formats[i].modifiers_size == 0)
            continue;
        params[i] = build_format(pod_builder, &self->video_info, self->supported_video_formats[i].format, self->modifiers + self->supported_video_formats[i].modifiers_index, self->supported_video_formats[i].modifiers_size);
        ++(*num_params);
    }

    return true;
}

static void renegotiate_format(void *data, uint64_t expirations) {
    (void)expirations;
    gsr_pipewire *self = (gsr_pipewire*)data;

    pw_thread_loop_lock(self->thread_loop);

    struct spa_pod *params[GSR_PIPEWIRE_NUM_VIDEO_FORMATS];
    uint32_t num_video_formats = 0;
    uint8_t params_buffer[2048];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));
    if (!gsr_pipewire_build_format_params(self, &pod_builder, params, &num_video_formats)) {
        pw_thread_loop_unlock(self->thread_loop);
        return;
    }

    pw_stream_update_params(self->stream, (const struct spa_pod**)params, num_video_formats);
    pw_thread_loop_unlock(self->thread_loop);
}

static bool spa_video_format_get_modifiers(gsr_pipewire *self, const enum spa_video_format format, uint64_t *modifiers, int32_t max_modifiers, int32_t *num_modifiers) {
    *num_modifiers = 0;

    if(max_modifiers == 0) {
        fprintf(stderr, "gsr error: spa_video_format_get_modifiers: no space for modifiers left\n");
        //modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        //modifiers[1] = DRM_FORMAT_MOD_INVALID;
        //*num_modifiers = 2;
        return false;
    }

    if(!self->egl->eglQueryDmaBufModifiersEXT) {
        fprintf(stderr, "gsr error: spa_video_format_get_modifiers: failed to initialize modifiers because eglQueryDmaBufModifiersEXT is not available\n");
        //modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        //modifiers[1] = DRM_FORMAT_MOD_INVALID;
        //*num_modifiers = 2;
        return false;
    }

    const int64_t drm_format = spa_video_format_to_drm_format(format);
    if(!self->egl->eglQueryDmaBufModifiersEXT(self->egl->egl_display, drm_format, max_modifiers, modifiers, NULL, num_modifiers)) {
        fprintf(stderr, "gsr error: spa_video_format_get_modifiers: eglQueryDmaBufModifiersEXT failed with drm format %d, %" PRIi64 "\n", (int)format, drm_format);
        //modifiers[0] = DRM_FORMAT_MOD_LINEAR;
        //modifiers[1] = DRM_FORMAT_MOD_INVALID;
        //*num_modifiers = 2;
        *num_modifiers = 0;
        return false;
    }

    // if(*num_modifiers + 2 <= max_modifiers) {
    //     modifiers[*num_modifiers + 0] = DRM_FORMAT_MOD_LINEAR;
    //     modifiers[*num_modifiers + 1] = DRM_FORMAT_MOD_INVALID;
    //     *num_modifiers += 2;
    // }
    return true;
}

static void gsr_pipewire_init_modifiers(gsr_pipewire *self) {
    for(size_t i = 0; i < GSR_PIPEWIRE_NUM_VIDEO_FORMATS; i++) {
        self->supported_video_formats[i].format = video_formats[i];
        int32_t num_modifiers = 0;
        spa_video_format_get_modifiers(self, self->supported_video_formats[i].format, self->modifiers + self->num_modifiers, GSR_PIPEWIRE_MAX_MODIFIERS - self->num_modifiers, &num_modifiers);
        self->supported_video_formats[i].modifiers_index = self->num_modifiers;
        self->supported_video_formats[i].modifiers_size = num_modifiers;
    }
}

static bool gsr_pipewire_setup_stream(gsr_pipewire *self) {
    struct spa_pod *params[GSR_PIPEWIRE_NUM_VIDEO_FORMATS];
    uint32_t num_video_formats = 0;
    uint8_t params_buffer[2048];
    struct spa_pod_builder pod_builder = SPA_POD_BUILDER_INIT(params_buffer, sizeof(params_buffer));

    self->thread_loop = pw_thread_loop_new("PipeWire thread loop", NULL);
    if(!self->thread_loop) {
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to create pipewire thread\n");
        goto error;
    }

    self->context = pw_context_new(pw_thread_loop_get_loop(self->thread_loop), NULL, 0);
    if(!self->context) {
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to create pipewire context\n");
        goto error;
    }

    if(pw_thread_loop_start(self->thread_loop) < 0) {
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to start thread\n");
        goto error;
    }

    pw_thread_loop_lock(self->thread_loop);

    // TODO: Why pass 5 to fcntl?
    self->core = pw_context_connect_fd(self->context, fcntl(self->fd, F_DUPFD_CLOEXEC, 5), NULL, 0);
    if(!self->core) {
        pw_thread_loop_unlock(self->thread_loop);
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to connect to fd %d\n", self->fd);
        goto error;
    }

    // TODO: Error check
    pw_core_add_listener(self->core, &self->core_listener, &core_events, self);

    gsr_pipewire_init_modifiers(self);

    // TODO: Cleanup?
    self->reneg = pw_loop_add_event(pw_thread_loop_get_loop(self->thread_loop), renegotiate_format, self);
    if(!self->reneg) {
        pw_thread_loop_unlock(self->thread_loop);
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: pw_loop_add_event failed\n");
        goto error;
    }

    self->server_version_sync = pw_core_sync(self->core, PW_ID_CORE, 0);
    pw_thread_loop_wait(self->thread_loop);

    self->stream = pw_stream_new(self->core, "com.dec05eba.gpu_screen_recorder",
        pw_properties_new(PW_KEY_MEDIA_TYPE, "Video",
                          PW_KEY_MEDIA_CATEGORY, "Capture",
                          PW_KEY_MEDIA_ROLE, "Screen", NULL));
    if(!self->stream) {
        pw_thread_loop_unlock(self->thread_loop);
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to create stream\n");
        goto error;
    }
    pw_stream_add_listener(self->stream, &self->stream_listener, &stream_events, self);

    if(!gsr_pipewire_build_format_params(self, &pod_builder, params, &num_video_formats)) {
        pw_thread_loop_unlock(self->thread_loop);
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to build format params\n");
        goto error;
    }

    if(pw_stream_connect(
        self->stream, PW_DIRECTION_INPUT, self->node,
        PW_STREAM_FLAG_AUTOCONNECT | PW_STREAM_FLAG_MAP_BUFFERS, (const struct spa_pod**)params,
        num_video_formats) < 0)
    {
        pw_thread_loop_unlock(self->thread_loop);
        fprintf(stderr, "gsr error: gsr_pipewire_setup_stream: failed to connect stream\n");
        goto error;
    }

    pw_thread_loop_unlock(self->thread_loop);
    return true;

    error:
    if(self->thread_loop) {
        //pw_thread_loop_wait(self->thread_loop);
        pw_thread_loop_stop(self->thread_loop);
    }

    if(self->stream) {
        pw_stream_disconnect(self->stream);
        pw_stream_destroy(self->stream);
        self->stream = NULL;
    }

    if(self->core) {
        pw_core_disconnect(self->core);
        self->core = NULL;
    }

    if(self->context) {
        pw_context_destroy(self->context);
        self->context = NULL;
    }

    if(self->thread_loop) {
        pw_thread_loop_destroy(self->thread_loop);
        self->thread_loop = NULL;
    }
    return false;
}

static int pw_init_counter = 0;
bool gsr_pipewire_init(gsr_pipewire *self, int pipewire_fd, uint32_t pipewire_node, int fps, bool capture_cursor, gsr_egl *egl) {
    if(pw_init_counter == 0)
        pw_init(NULL, NULL);
    ++pw_init_counter;

    memset(self, 0, sizeof(*self));
    self->egl = egl;
    self->fd = pipewire_fd;
    self->node = pipewire_node;
    if(pthread_mutex_init(&self->mutex, NULL) != 0) {
        fprintf(stderr, "gsr error: gsr_pipewire_init: failed to initialize mutex\n");
        gsr_pipewire_deinit(self);
        return false;
    }
    self->mutex_initialized = true;
    self->video_info.fps_num = fps;
    self->video_info.fps_den = 1;
    self->cursor.visible = capture_cursor;
    
    if(!gsr_pipewire_setup_stream(self)) {
        gsr_pipewire_deinit(self);
        return false;
    }

    return true;
}

void gsr_pipewire_deinit(gsr_pipewire *self) {
    if(self->thread_loop) {
        //pw_thread_loop_wait(self->thread_loop);
        pw_thread_loop_stop(self->thread_loop);
    }

    if(self->stream) {
        pw_stream_disconnect(self->stream);
        pw_stream_destroy(self->stream);
        self->stream = NULL;
    }

    if(self->core) {
        pw_core_disconnect(self->core);
        self->core = NULL;
    }

    if(self->context) {
        pw_context_destroy(self->context);
        self->context = NULL;
    }

    if(self->thread_loop) {
        pw_thread_loop_destroy(self->thread_loop);
        self->thread_loop = NULL;
    }

    if(self->fd > 0) {
        close(self->fd);
        self->fd = -1;
    }

    for(size_t i = 0; i < self->dmabuf_num_planes; ++i) {
        if(self->dmabuf_data[i].fd > 0) {
            close(self->dmabuf_data[i].fd);
            self->dmabuf_data[i].fd = -1;
        }
    }
    self->dmabuf_num_planes = 0;

    self->negotiated = false;

    if(self->mutex_initialized) {
        pthread_mutex_destroy(&self->mutex);
        self->mutex_initialized = false;
    }

    if(self->cursor.data) {
        free(self->cursor.data);
        self->cursor.data = NULL;
    }

    --pw_init_counter;
    if(pw_init_counter == 0) {
#if PW_CHECK_VERSION(0, 3, 49)
        pw_deinit();
#endif
    }
}

static EGLImage gsr_pipewire_create_egl_image(gsr_pipewire *self, const int *fds, const uint32_t *offsets, const uint32_t *pitches, const uint64_t *modifiers, bool use_modifiers) {
    intptr_t img_attr[44];
    setup_dma_buf_attrs(img_attr, spa_video_format_to_drm_format(self->format.info.raw.format), self->format.info.raw.size.width, self->format.info.raw.size.height,
        fds, offsets, pitches, modifiers, self->dmabuf_num_planes, use_modifiers);
    while(self->egl->eglGetError() != EGL_SUCCESS){}
    EGLImage image = self->egl->eglCreateImage(self->egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
    if(!image || self->egl->eglGetError() != EGL_SUCCESS) {
        if(image)
            self->egl->eglDestroyImage(self->egl->egl_display, image);
        return NULL;
    }
    return image;
}

static EGLImage gsr_pipewire_create_egl_image_with_fallback(gsr_pipewire *self) {
    int fds[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    uint32_t offsets[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    uint32_t pitches[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    uint64_t modifiers[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    for(size_t i = 0; i < self->dmabuf_num_planes; ++i) {
        fds[i] = self->dmabuf_data[i].fd;
        offsets[i] = self->dmabuf_data[i].offset;
        pitches[i] = self->dmabuf_data[i].stride;
        modifiers[i] = self->format.info.raw.modifier;
    }

    EGLImage image = NULL;
    if(self->no_modifiers_fallback) {
        image = gsr_pipewire_create_egl_image(self, fds, offsets, pitches, modifiers, false);
    } else {
        image = gsr_pipewire_create_egl_image(self, fds, offsets, pitches, modifiers, true);
        if(!image) {
            fprintf(stderr, "gsr error: gsr_pipewire_create_egl_image_with_fallback: failed to create egl image with modifiers, trying without modifiers\n");
            self->no_modifiers_fallback = true;
            image = gsr_pipewire_create_egl_image(self, fds, offsets, pitches, modifiers, false);
        }
    }
    return image;
}

static bool gsr_pipewire_bind_image_to_texture(gsr_pipewire *self, EGLImage image, unsigned int texture_id, bool external_texture) {
    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;
    while(self->egl->glGetError() != 0){}
    self->egl->glBindTexture(texture_target, texture_id);
    self->egl->glEGLImageTargetTexture2DOES(texture_target, image);
    const bool success = self->egl->glGetError() == 0;
    self->egl->glBindTexture(texture_target, 0);
    return success;
}

static void gsr_pipewire_bind_image_to_texture_with_fallback(gsr_pipewire *self, gsr_texture_map texture_map, EGLImage image) {
    if(self->external_texture_fallback) {
        gsr_pipewire_bind_image_to_texture(self, image, texture_map.external_texture_id, true);
    } else {
        if(!gsr_pipewire_bind_image_to_texture(self, image, texture_map.texture_id, false)) {
            fprintf(stderr, "gsr error: gsr_pipewire_map_texture: failed to bind image to texture, trying with external texture\n");
            self->external_texture_fallback = true;
            gsr_pipewire_bind_image_to_texture(self, image, texture_map.external_texture_id, true);
        }
    }
}

static void gsr_pipewire_update_cursor_texture(gsr_pipewire *self, gsr_texture_map texture_map) {
    if(!self->cursor.data)
        return;

    self->egl->glBindTexture(GL_TEXTURE_2D, texture_map.cursor_texture_id);
    // TODO: glTextureSubImage2D if same size
    self->egl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, self->cursor.width, self->cursor.height, 0, GL_RGBA, GL_UNSIGNED_BYTE, self->cursor.data);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->egl->glBindTexture(GL_TEXTURE_2D, 0);

    free(self->cursor.data);
    self->cursor.data = NULL;
}

bool gsr_pipewire_map_texture(gsr_pipewire *self, gsr_texture_map texture_map, gsr_pipewire_region *region, gsr_pipewire_region *cursor_region, gsr_pipewire_dmabuf_data *dmabuf_data, int *num_dmabuf_data, uint32_t *fourcc, uint64_t *modifiers, bool *using_external_image) {
    for(int i = 0; i < GSR_PIPEWIRE_DMABUF_MAX_PLANES; ++i) {
        memset(&dmabuf_data[i], 0, sizeof(gsr_pipewire_dmabuf_data));
    }
    *num_dmabuf_data = 0;
    *using_external_image = self->external_texture_fallback;
    *fourcc = 0;
    *modifiers = 0;
    pthread_mutex_lock(&self->mutex);

    if(!self->negotiated || self->dmabuf_data[0].fd <= 0) {
        pthread_mutex_unlock(&self->mutex);
        return false;
    }

    EGLImage image = gsr_pipewire_create_egl_image_with_fallback(self);
    if(image) {
        gsr_pipewire_bind_image_to_texture_with_fallback(self, texture_map, image);
        *using_external_image = self->external_texture_fallback;
        self->egl->eglDestroyImage(self->egl->egl_display, image);
    }

    gsr_pipewire_update_cursor_texture(self, texture_map);

    region->x = 0;
    region->y = 0;

    region->width = self->format.info.raw.size.width;
    region->height = self->format.info.raw.size.height;

    if(self->crop.valid) {
        region->x = self->crop.x;
        region->y = self->crop.y;

        region->width = self->crop.width;
        region->height = self->crop.height;
    }

    /* TODO: Test if cursor hotspot is correct */
    cursor_region->x = self->cursor.x - self->cursor.hotspot_x;
    cursor_region->y = self->cursor.y - self->cursor.hotspot_y;

    cursor_region->width = self->cursor.width;
    cursor_region->height = self->cursor.height;

    for(size_t i = 0; i < self->dmabuf_num_planes; ++i) {
        dmabuf_data[i] = self->dmabuf_data[i];
        self->dmabuf_data[i].fd = -1;
    }
    *num_dmabuf_data = self->dmabuf_num_planes;
    *fourcc = spa_video_format_to_drm_format(self->format.info.raw.format);
    *modifiers = self->format.info.raw.modifier;
    self->dmabuf_num_planes = 0;

    pthread_mutex_unlock(&self->mutex);
    return true;
}

bool gsr_pipewire_is_damaged(gsr_pipewire *self) {
    bool damaged = false;
    pthread_mutex_lock(&self->mutex);
    damaged = self->damaged;
    pthread_mutex_unlock(&self->mutex);
    return damaged;
}

void gsr_pipewire_clear_damage(gsr_pipewire *self) {
    pthread_mutex_lock(&self->mutex);
    self->damaged = false;
    pthread_mutex_unlock(&self->mutex);
}
