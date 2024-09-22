#include "../../include/capture/portal.h"
#include "../../include/color_conversion.h"
#include "../../include/egl.h"
#include "../../include/utils.h"
#include "../../include/dbus.h"
#include "../../include/pipewire.h"

#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#include <assert.h>

#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_portal_params params;

    gsr_texture_map texture_map;

    gsr_dbus dbus;
    char *session_handle;

    gsr_pipewire pipewire;
    vec2i capture_size;
    gsr_pipewire_dmabuf_data dmabuf_data[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    int num_dmabuf_data;

    AVCodecContext *video_codec_context;
    bool fast_path_failed;
} gsr_capture_portal;

static void gsr_capture_portal_cleanup_plane_fds(gsr_capture_portal *self) {
    for(int i = 0; i < self->num_dmabuf_data; ++i) {
        if(self->dmabuf_data[i].fd > 0) {
            close(self->dmabuf_data[i].fd);
            self->dmabuf_data[i].fd = 0;
        }
    }
    self->num_dmabuf_data = 0;
}

static void gsr_capture_portal_stop(gsr_capture_portal *self) {
    if(self->texture_map.texture_id) {
        self->params.egl->glDeleteTextures(1, &self->texture_map.texture_id);
        self->texture_map.texture_id = 0;
    }

    if(self->texture_map.external_texture_id) {
        self->params.egl->glDeleteTextures(1, &self->texture_map.external_texture_id);
        self->texture_map.external_texture_id = 0;
    }

    if(self->texture_map.cursor_texture_id) {
        self->params.egl->glDeleteTextures(1, &self->texture_map.cursor_texture_id);
        self->texture_map.cursor_texture_id = 0;
    }

    gsr_capture_portal_cleanup_plane_fds(self);

    gsr_pipewire_deinit(&self->pipewire);

    if(self->session_handle) {
        free(self->session_handle);
        self->session_handle = NULL;
    }

    gsr_dbus_deinit(&self->dbus);
}

static void gsr_capture_portal_create_input_textures(gsr_capture_portal *self) {
    self->params.egl->glGenTextures(1, &self->texture_map.texture_id);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->texture_map.texture_id);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    self->params.egl->glGenTextures(1, &self->texture_map.external_texture_id);
    self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, self->texture_map.external_texture_id);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_EXTERNAL_OES, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_EXTERNAL_OES, 0);

    self->params.egl->glGenTextures(1, &self->texture_map.cursor_texture_id);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->texture_map.cursor_texture_id);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
}

static void get_default_gpu_screen_recorder_restore_token_path(char *buffer, size_t buffer_size) {
    const char *xdg_config_home = getenv("XDG_CONFIG_HOME");
    if(xdg_config_home) {
        snprintf(buffer, buffer_size, "%s/gpu-screen-recorder/restore_token", xdg_config_home);
    } else {
        const char *home = getenv("HOME");
        if(!home)
            home = "/tmp";
        snprintf(buffer, buffer_size, "%s/.config/gpu-screen-recorder/restore_token", home);
    }
}

static bool create_directory_to_file(const char *filepath) {
    char dir[PATH_MAX];
    dir[0] = '\0';

    const char *split = strrchr(filepath, '/');
    if(!split) /* Assuming it's the current directory (for example if filepath is "restore_token"), which doesn't need to be created */
        return true;

    snprintf(dir, sizeof(dir), "%.*s", (int)(split - filepath), filepath);
    if(create_directory_recursive(dir) != 0) {
        fprintf(stderr, "gsr warning: gsr_capture_portal_save_restore_token: failed to create directory (%s) for restore token\n", dir);
        return false;
    }
    return true;
}

static void gsr_capture_portal_save_restore_token(const char *restore_token, const char *portal_session_token_filepath) {
    char restore_token_path[PATH_MAX];
    restore_token_path[0] = '\0';
    if(portal_session_token_filepath)
        snprintf(restore_token_path, sizeof(restore_token_path), "%s", portal_session_token_filepath);
    else
        get_default_gpu_screen_recorder_restore_token_path(restore_token_path, sizeof(restore_token_path));

    if(!create_directory_to_file(restore_token_path))
        return;

    FILE *f = fopen(restore_token_path, "wb");
    if(!f) {
        fprintf(stderr, "gsr warning: gsr_capture_portal_save_restore_token: failed to create restore token file (%s)\n", restore_token_path);
        return;
    }

    const int restore_token_len = strlen(restore_token);
    if((long)fwrite(restore_token, 1, restore_token_len, f) != restore_token_len) {
        fprintf(stderr, "gsr warning: gsr_capture_portal_save_restore_token: failed to write restore token to file (%s)\n", restore_token_path);
        fclose(f);
        return;
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_save_restore_token: saved restore token to cache (%s)\n", restore_token);
    fclose(f);
}

static void gsr_capture_portal_get_restore_token_from_cache(char *buffer, size_t buffer_size, const char *portal_session_token_filepath) {
    assert(buffer_size > 0);
    buffer[0] = '\0';

    char restore_token_path[PATH_MAX];
    restore_token_path[0] = '\0';
    if(portal_session_token_filepath)
        snprintf(restore_token_path, sizeof(restore_token_path), "%s", portal_session_token_filepath);
    else
        get_default_gpu_screen_recorder_restore_token_path(restore_token_path, sizeof(restore_token_path));

    FILE *f = fopen(restore_token_path, "rb");
    if(!f) {
        fprintf(stderr, "gsr info: gsr_capture_portal_get_restore_token_from_cache: no restore token found in cache or failed to load (%s)\n", restore_token_path);
        return;
    }

    fseek(f, 0, SEEK_END);
    long file_size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if(file_size > 0 && file_size < 1024 && file_size < (long)buffer_size && (long)fread(buffer, 1, file_size, f) != file_size) {
        buffer[0] = '\0';
        fprintf(stderr, "gsr warning: gsr_capture_portal_get_restore_token_from_cache: failed to read restore token (%s)\n", restore_token_path);
        fclose(f);
        return;
    }

    if(file_size > 0 && file_size < (long)buffer_size)
        buffer[file_size] = '\0';

    fprintf(stderr, "gsr info: gsr_capture_portal_get_restore_token_from_cache: read cached restore token (%s)\n", buffer);
    fclose(f);
}

static int gsr_capture_portal_setup_dbus(gsr_capture_portal *self, int *pipewire_fd, uint32_t *pipewire_node) {
    *pipewire_fd = 0;
    *pipewire_node = 0;
    int response_status = 0;

    char restore_token[1024];
    restore_token[0] = '\0';
    if(self->params.restore_portal_session)
        gsr_capture_portal_get_restore_token_from_cache(restore_token, sizeof(restore_token), self->params.portal_session_token_filepath);

    if(!gsr_dbus_init(&self->dbus, restore_token))
        return -1;

    fprintf(stderr, "gsr info: gsr_capture_portal_setup_dbus: CreateSession\n");
    response_status = gsr_dbus_screencast_create_session(&self->dbus, &self->session_handle);
    if(response_status != 0) {
        fprintf(stderr, "gsr error: gsr_capture_portal_setup_dbus: CreateSession failed\n");
        return response_status;
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_setup_dbus: SelectSources\n");
    response_status = gsr_dbus_screencast_select_sources(&self->dbus, self->session_handle, GSR_PORTAL_CAPTURE_TYPE_ALL, self->params.record_cursor ? GSR_PORTAL_CURSOR_MODE_EMBEDDED : GSR_PORTAL_CURSOR_MODE_HIDDEN);
    if(response_status != 0) {
        fprintf(stderr, "gsr error: gsr_capture_portal_setup_dbus: SelectSources failed\n");
        return response_status;
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_setup_dbus: Start\n");
    response_status = gsr_dbus_screencast_start(&self->dbus, self->session_handle, pipewire_node);
    if(response_status != 0) {
        fprintf(stderr, "gsr error: gsr_capture_portal_setup_dbus: Start failed\n");
        return response_status;
    }

    const char *screencast_restore_token = gsr_dbus_screencast_get_restore_token(&self->dbus);
    if(screencast_restore_token)
        gsr_capture_portal_save_restore_token(screencast_restore_token, self->params.portal_session_token_filepath);

    fprintf(stderr, "gsr info: gsr_capture_portal_setup_dbus: OpenPipeWireRemote\n");
    if(!gsr_dbus_screencast_open_pipewire_remote(&self->dbus, self->session_handle, pipewire_fd)) {
        fprintf(stderr, "gsr error: gsr_capture_portal_setup_dbus: OpenPipeWireRemote failed\n");
        return -1;
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_setup_dbus: desktop portal setup finished\n");
    return 0;
}

static bool gsr_capture_portal_get_frame_dimensions(gsr_capture_portal *self) {
    gsr_pipewire_region region = {0, 0, 0, 0};
    gsr_pipewire_region cursor_region = {0, 0, 0, 0};
    fprintf(stderr, "gsr info: gsr_capture_portal_start: waiting for pipewire negotiation\n");

    const double start_time = clock_get_monotonic_seconds();
    while(clock_get_monotonic_seconds() - start_time < 5.0) {
        bool uses_external_image = false;
        uint32_t fourcc = 0;
        uint64_t modifiers = 0;
        if(gsr_pipewire_map_texture(&self->pipewire, self->texture_map, &region, &cursor_region, self->dmabuf_data, &self->num_dmabuf_data, &fourcc, &modifiers, &uses_external_image)) {
            gsr_capture_portal_cleanup_plane_fds(self);
            self->capture_size.x = region.width;
            self->capture_size.y = region.height;
            fprintf(stderr, "gsr info: gsr_capture_portal_start: pipewire negotiation finished\n");
            return true;
        }
        usleep(30 * 1000); /* 30 milliseconds */
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_start: timed out waiting for pipewire negotiation (5 seconds)\n");
    return false;
}

static int gsr_capture_portal_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_portal *self = cap->priv;

    gsr_capture_portal_create_input_textures(self);

    int pipewire_fd = 0;
    uint32_t pipewire_node = 0;
    const int response_status = gsr_capture_portal_setup_dbus(self, &pipewire_fd, &pipewire_node);
    if(response_status != 0) {
        gsr_capture_portal_stop(self);
        // Response status values:
        // 0: Success, the request is carried out
        // 1: The user cancelled the interaction
        // 2: The user interaction was ended in some other way
        // Response status value 2 happens usually if there was some kind of error in the desktop portal on the system
        if(response_status == 2) {
            fprintf(stderr, "gsr error: gsr_capture_portal_start: desktop portal capture failed. Either you Wayland compositor doesn't support desktop portal capture or it's incorrectly setup on your system\n");
            return 50;
        } else if(response_status == 1) {
            fprintf(stderr, "gsr error: gsr_capture_portal_start: desktop portal capture failed. It seems like desktop portal capture was canceled by the user.\n");
            return 60;
        } else {
            return -1;
        }
    }

    fprintf(stderr, "gsr info: gsr_capture_portal_start: setting up pipewire\n");
    /* TODO: support hdr when pipewire supports it */
    /* gsr_pipewire closes the pipewire fd, even on failure */
    if(!gsr_pipewire_init(&self->pipewire, pipewire_fd, pipewire_node, video_codec_context->framerate.num, self->params.record_cursor, self->params.egl)) {
        fprintf(stderr, "gsr error: gsr_capture_portal_start: failed to setup pipewire with fd: %d, node: %" PRIu32 "\n", pipewire_fd, pipewire_node);
        gsr_capture_portal_stop(self);
        return -1;
    }
    fprintf(stderr, "gsr info: gsr_capture_portal_start: pipewire setup finished\n");

    if(!gsr_capture_portal_get_frame_dimensions(self)) {
        gsr_capture_portal_stop(self);
        return -1;
    }

    /* Disable vsync */
    self->params.egl->eglSwapInterval(self->params.egl->egl_display, 0);

    video_codec_context->width = FFALIGN(self->capture_size.x, 2);
    video_codec_context->height = FFALIGN(self->capture_size.y, 2);

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    self->video_codec_context = video_codec_context;
    return 0;
}

static int max_int(int a, int b) {
    return a > b ? a : b;
}

static int gsr_capture_portal_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    (void)frame;
    (void)color_conversion;
    gsr_capture_portal *self = cap->priv;

    /* TODO: Handle formats other than RGB(a) */
    gsr_pipewire_region region = {0, 0, 0, 0};
    gsr_pipewire_region cursor_region = {0, 0, 0, 0};
    uint32_t pipewire_fourcc = 0;
    uint64_t pipewire_modifiers = 0;
    bool using_external_image = false;
    if(gsr_pipewire_map_texture(&self->pipewire, self->texture_map, &region, &cursor_region, self->dmabuf_data, &self->num_dmabuf_data, &pipewire_fourcc, &pipewire_modifiers, &using_external_image)) {
        if(region.width != self->capture_size.x || region.height != self->capture_size.y) {
            self->capture_size.x = region.width;
            self->capture_size.y = region.height;
            gsr_color_conversion_clear(color_conversion);
        }
    } else {
        return 0;
    }
    
    const vec2i target_pos = { max_int(0, frame->width / 2 - self->capture_size.x / 2), max_int(0, frame->height / 2 - self->capture_size.y / 2) };

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    // TODO: Handle region crop

    /* Fast opengl free path */
    if(!self->fast_path_failed && video_codec_context_is_vaapi(self->video_codec_context) && self->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD) {
        int fds[4];
        uint32_t offsets[4];
        uint32_t pitches[4];
        uint64_t modifiers[4];
        for(int i = 0; i < self->num_dmabuf_data; ++i) {
            fds[i] = self->dmabuf_data[i].fd;
            offsets[i] = self->dmabuf_data[i].offset;
            pitches[i] = self->dmabuf_data[i].stride;
            modifiers[i] = pipewire_modifiers;
        }
        if(!vaapi_copy_drm_planes_to_video_surface(self->video_codec_context, frame, (vec2i){region.x, region.y}, self->capture_size, target_pos, self->capture_size, pipewire_fourcc, self->capture_size, fds, offsets, pitches, modifiers, self->num_dmabuf_data)) {
            fprintf(stderr, "gsr error: gsr_capture_portal_capture: vaapi_copy_drm_planes_to_video_surface failed, falling back to opengl copy. Please report this as an issue at https://github.com/dec05eba/gpu-screen-recorder-issues\n");
            self->fast_path_failed = true;
        }
    } else {
        self->fast_path_failed = true;
    }

    if(self->fast_path_failed) {
        gsr_color_conversion_draw(color_conversion, using_external_image ? self->texture_map.external_texture_id : self->texture_map.texture_id,
            target_pos, self->capture_size,
            (vec2i){region.x, region.y}, self->capture_size,
            0.0f, using_external_image);
    }

    if(self->params.record_cursor) {
        const vec2i cursor_pos = {
            target_pos.x + cursor_region.x,
            target_pos.y + cursor_region.y
        };

        self->params.egl->glEnable(GL_SCISSOR_TEST);
        self->params.egl->glScissor(target_pos.x, target_pos.y, self->capture_size.x, self->capture_size.y);
        gsr_color_conversion_draw(color_conversion, self->texture_map.cursor_texture_id,
            (vec2i){cursor_pos.x, cursor_pos.y}, (vec2i){cursor_region.width, cursor_region.height},
            (vec2i){0, 0}, (vec2i){cursor_region.width, cursor_region.height},
            0.0f, false);
        self->params.egl->glDisable(GL_SCISSOR_TEST);
    }

    self->params.egl->glFlush();
    self->params.egl->glFinish();

    gsr_capture_portal_cleanup_plane_fds(self);

    return 0;
}

static gsr_source_color gsr_capture_portal_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_RGB;
}

static bool gsr_capture_portal_uses_external_image(gsr_capture *cap) {
    (void)cap;
    return true;
}

static bool gsr_capture_portal_is_damaged(gsr_capture *cap) {
    gsr_capture_portal *self = cap->priv;
    return gsr_pipewire_is_damaged(&self->pipewire);
}

static void gsr_capture_portal_clear_damage(gsr_capture *cap) {
    gsr_capture_portal *self = cap->priv;
    gsr_pipewire_clear_damage(&self->pipewire);
}

static void gsr_capture_portal_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_portal *cap_portal = cap->priv;
    if(cap->priv) {
        gsr_capture_portal_stop(cap_portal);
        free(cap->priv);
        cap->priv = NULL;
    }
    free(cap);
}

gsr_capture* gsr_capture_portal_create(const gsr_capture_portal_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_portal_create params is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_portal *cap_portal = calloc(1, sizeof(gsr_capture_portal));
    if(!cap_portal) {
        free(cap);
        return NULL;
    }

    cap_portal->params = *params;
    
    *cap = (gsr_capture) {
        .start = gsr_capture_portal_start,
        .tick = NULL,
        .should_stop = NULL,
        .capture = gsr_capture_portal_capture,
        .get_source_color = gsr_capture_portal_get_source_color,
        .uses_external_image = gsr_capture_portal_uses_external_image,
        .is_damaged = gsr_capture_portal_is_damaged,
        .clear_damage = gsr_capture_portal_clear_damage,
        .destroy = gsr_capture_portal_destroy,
        .priv = cap_portal
    };

    return cap;
}
