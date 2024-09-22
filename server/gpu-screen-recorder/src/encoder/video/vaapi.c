#include "../../../include/encoder/video/vaapi.h"
#include "../../../include/utils.h"
#include "../../../include/egl.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_vaapi.h>

#include <va/va_drmcommon.h>
#include <va/va_drm.h>

#include <stdlib.h>
#include <unistd.h>
#include <fcntl.h>

typedef struct {
    gsr_video_encoder_vaapi_params params;

    unsigned int target_textures[2];

    AVBufferRef *device_ctx;
    VADisplay va_dpy;
    VADRMPRIMESurfaceDescriptor prime;
} gsr_video_encoder_vaapi;

static bool gsr_video_encoder_vaapi_setup_context(gsr_video_encoder_vaapi *self, AVCodecContext *video_codec_context) {
    char render_path[128];
    if(!gsr_card_path_get_render_path(self->params.egl->card_path, render_path)) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_context: failed to get /dev/dri/renderDXXX file from %s\n", self->params.egl->card_path);
        return false;
    }

    if(av_hwdevice_ctx_create(&self->device_ctx, AV_HWDEVICE_TYPE_VAAPI, render_path, NULL, 0) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_context: failed to create hardware device context\n");
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(self->device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_context: failed to create hwframe context\n");
        av_buffer_unref(&self->device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)self->device_ctx->data;

    //hw_frame_context->initial_pool_size = 20;

    AVVAAPIDeviceContext *vactx = ((AVHWDeviceContext*)self->device_ctx->data)->hwctx;
    self->va_dpy = vactx->display;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_context: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&self->device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    av_buffer_unref(&frame_context);
    return true;
}

static uint32_t fourcc(uint32_t a, uint32_t b, uint32_t c, uint32_t d) {
    return (d << 24) | (c << 16) | (b << 8) | a;
}

static bool gsr_video_encoder_vaapi_setup_textures(gsr_video_encoder_vaapi *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    const int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    VASurfaceID target_surface_id = (uintptr_t)frame->data[3];

    VAStatus va_status = vaExportSurfaceHandle(self->va_dpy, target_surface_id, VA_SURFACE_ATTRIB_MEM_TYPE_DRM_PRIME_2, VA_EXPORT_SURFACE_WRITE_ONLY | VA_EXPORT_SURFACE_SEPARATE_LAYERS, &self->prime);
    if(va_status != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_textures: vaExportSurfaceHandle failed, error: %d\n", va_status);
        return false;
    }
    vaSyncSurface(self->va_dpy, target_surface_id);

    const uint32_t formats_nv12[2] = { fourcc('R', '8', ' ', ' '), fourcc('G', 'R', '8', '8') };
    const uint32_t formats_p010[2] = { fourcc('R', '1', '6', ' '), fourcc('G', 'R', '3', '2') };

    if(self->prime.fourcc == VA_FOURCC_NV12 || self->prime.fourcc == VA_FOURCC_P010) {
        const uint32_t *formats = self->prime.fourcc == VA_FOURCC_NV12 ? formats_nv12 : formats_p010;
        const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

        self->params.egl->glGenTextures(2, self->target_textures);
        for(int i = 0; i < 2; ++i) {
            const int layer = i;

            int fds[4];
            uint32_t offsets[4];
            uint32_t pitches[4];
            uint64_t modifiers[4];
            for(uint32_t j = 0; j < self->prime.layers[layer].num_planes; ++j) {
                fds[j] = self->prime.objects[self->prime.layers[layer].object_index[j]].fd;
                offsets[j] = self->prime.layers[layer].offset[j];
                pitches[j] = self->prime.layers[layer].pitch[j];
                modifiers[j] = self->prime.objects[self->prime.layers[layer].object_index[j]].drm_format_modifier;
            }

            intptr_t img_attr[44];
            setup_dma_buf_attrs(img_attr, formats[i], self->prime.width / div[i], self->prime.height / div[i],
                fds, offsets, pitches, modifiers, self->prime.layers[layer].num_planes, true);

            while(self->params.egl->eglGetError() != EGL_SUCCESS){}
            EGLImage image = self->params.egl->eglCreateImage(self->params.egl->egl_display, 0, EGL_LINUX_DMA_BUF_EXT, NULL, img_attr);
            if(!image) {
                fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_textures: failed to create egl image from drm fd for output drm fd, error: %d\n", self->params.egl->eglGetError());
                return false;
            }

            self->params.egl->glBindTexture(GL_TEXTURE_2D, self->target_textures[i]);
            self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            self->params.egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);

            while(self->params.egl->glGetError()) {}
            while(self->params.egl->eglGetError() != EGL_SUCCESS){}
            self->params.egl->glEGLImageTargetTexture2DOES(GL_TEXTURE_2D, image);
            if(self->params.egl->glGetError() != 0 || self->params.egl->eglGetError() != EGL_SUCCESS) {
                // TODO: Get the error properly
                fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_textures: failed to bind egl image to gl texture, error: %d\n", self->params.egl->eglGetError());
                self->params.egl->eglDestroyImage(self->params.egl->egl_display, image);
                self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
                return false;
            }

            self->params.egl->eglDestroyImage(self->params.egl->egl_display, image);
            self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
        }

        return true;
    } else {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_setup_textures: unexpected fourcc %u for output drm fd, expected nv12 or p010\n", self->prime.fourcc);
        return false;
    }
}

static bool profile_is_h264(VAProfile profile) {
    switch(profile) {
        case 5: // VAProfileH264Baseline
        case VAProfileH264Main:
        case VAProfileH264High:
        case VAProfileH264ConstrainedBaseline:
            return true;
        default:
            return false;
    }
}

static bool profile_is_hevc_8bit(VAProfile profile) {
    switch(profile) {
        case VAProfileHEVCMain:
            return true;
        default:
            return false;
    }
}

static bool profile_is_hevc_10bit(VAProfile profile) {
    switch(profile) {
        case VAProfileHEVCMain10:
        //case VAProfileHEVCMain12:
        //case VAProfileHEVCMain422_10:
        //case VAProfileHEVCMain422_12:
        //case VAProfileHEVCMain444:
        //case VAProfileHEVCMain444_10:
        //case VAProfileHEVCMain444_12:
            return true;
        default:
            return false;
    }
}

static bool profile_is_av1(VAProfile profile) {
    switch(profile) {
        case VAProfileAV1Profile0:
        case VAProfileAV1Profile1:
            return true;
        default:
            return false;
    }
}

static bool profile_is_vp8(VAProfile profile) {
    switch(profile) {
        case VAProfileVP8Version0_3:
            return true;
        default:
            return false;
    }
}

static bool profile_is_vp9(VAProfile profile) {
    switch(profile) {
        case VAProfileVP9Profile0:
        case VAProfileVP9Profile1:
        case VAProfileVP9Profile2:
        case VAProfileVP9Profile3:
            return true;
        default:
            return false;
    }
}

static bool profile_supports_video_encoding(VADisplay va_dpy, VAProfile profile) {
    int num_entrypoints = vaMaxNumEntrypoints(va_dpy);
    if(num_entrypoints <= 0)
        return false;

    VAEntrypoint *entrypoint_list = calloc(num_entrypoints, sizeof(VAEntrypoint));
    if(!entrypoint_list)
        return false;

    bool supported = false;
    if(vaQueryConfigEntrypoints(va_dpy, profile, entrypoint_list, &num_entrypoints) == VA_STATUS_SUCCESS) {
        for(int i = 0; i < num_entrypoints; ++i) {
            if(entrypoint_list[i] == VAEntrypointEncSlice) {
                supported = true;
                break;
            }
        }
    }

    free(entrypoint_list);
    return supported;
}

static bool get_supported_video_codecs(VADisplay va_dpy, gsr_supported_video_codecs *video_codecs, bool cleanup) {
    *video_codecs = (gsr_supported_video_codecs){0};
    bool success = false;
    VAProfile *profile_list = NULL;

    vaSetInfoCallback(va_dpy, NULL, NULL);

    int va_major = 0;
    int va_minor = 0;
    if(vaInitialize(va_dpy, &va_major, &va_minor) != VA_STATUS_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_get_supported_codecs: vaInitialize failed\n");
        goto fail;
    }

    int num_profiles = vaMaxNumProfiles(va_dpy);
    if(num_profiles <= 0)
        goto fail;

    profile_list = calloc(num_profiles, sizeof(VAProfile));
    if(!profile_list || vaQueryConfigProfiles(va_dpy, profile_list, &num_profiles) != VA_STATUS_SUCCESS)
        goto fail;

    for(int i = 0; i < num_profiles; ++i) {
        if(profile_is_h264(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i]))
                video_codecs->h264 = true;
        } else if(profile_is_hevc_8bit(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i]))
                video_codecs->hevc = true;
        } else if(profile_is_hevc_10bit(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i])) {
                video_codecs->hevc_hdr = true;
                video_codecs->hevc_10bit = true;
            }
        } else if(profile_is_av1(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i])) {
                video_codecs->av1 = true;
                video_codecs->av1_hdr = true;
                video_codecs->av1_10bit = true;
            }
        } else if(profile_is_vp8(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i]))
                video_codecs->vp8 = true;
        } else if(profile_is_vp9(profile_list[i])) {
            if(profile_supports_video_encoding(va_dpy, profile_list[i]))
                video_codecs->vp9 = true;
        }
    }

    success = true;
    fail:
    if(profile_list)
        free(profile_list);

    if(cleanup)
        vaTerminate(va_dpy);

    return success;
}

static gsr_supported_video_codecs gsr_video_encoder_vaapi_get_supported_codecs(gsr_video_encoder *encoder, bool cleanup) {
    gsr_video_encoder_vaapi *encoder_vaapi = encoder->priv;
    gsr_supported_video_codecs supported_video_codecs = {0};

    char render_path[128];
    if(!gsr_card_path_get_render_path(encoder_vaapi->params.egl->card_path, render_path)) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_get_supported_codecs: failed to get /dev/dri/renderDXXX file from %s\n", encoder_vaapi->params.egl->card_path);
        return supported_video_codecs;
    }

    const int drm_fd = open(render_path, O_RDWR);
    if(drm_fd == -1) {
        fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_get_supported_codecs: failed to open device %s\n", render_path);
        return supported_video_codecs;
    }

    VADisplay va_dpy = vaGetDisplayDRM(drm_fd);
    if(va_dpy) {
        if(!get_supported_video_codecs(va_dpy, &supported_video_codecs, cleanup))
            fprintf(stderr, "gsr error: gsr_video_encoder_vaapi_get_supported_codecs: failed to query supported video codecs for device %s\n", render_path);
    }

    if(cleanup)
        close(drm_fd);

    return supported_video_codecs;
}

static void gsr_video_encoder_vaapi_stop(gsr_video_encoder_vaapi *self, AVCodecContext *video_codec_context);

static bool gsr_video_encoder_vaapi_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_video_encoder_vaapi *encoder_vaapi = encoder->priv;

    if(encoder_vaapi->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_HEVC) {
        // TODO: dont do this if using ffmpeg reports that this is not needed (AMD driver bug that was fixed recently)
        video_codec_context->width = FFALIGN(video_codec_context->width, 64);
        video_codec_context->height = FFALIGN(video_codec_context->height, 16);
    } else if(encoder_vaapi->params.egl->gpu_info.vendor == GSR_GPU_VENDOR_AMD && video_codec_context->codec_id == AV_CODEC_ID_AV1) {
        // TODO: Dont do this for VCN 5 and forward which should fix this hardware bug
        video_codec_context->width = FFALIGN(video_codec_context->width, 64);
        // AMD driver has special case handling for 1080 height to set it to 1082 instead of 1088 (1080 aligned to 16).
        // TODO: Set height to 1082 in this case, but it wont work because it will be aligned to 1088.
        if(video_codec_context->height == 1080) {
            video_codec_context->height = 1080;
        } else {
            video_codec_context->height = FFALIGN(video_codec_context->height, 16);
        }
    }

    if(video_codec_context->width != frame->width || video_codec_context->height != frame->height) {
        fprintf(stderr, "gsr warning: gsr_video_encoder_vaapi_start: black bars have been added to the video because of a bug in AMD drivers/hardware. Record with h264 codec instead (-k h264) to get around this issue\n");
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(!gsr_video_encoder_vaapi_setup_context(encoder_vaapi, video_codec_context)) {
        gsr_video_encoder_vaapi_stop(encoder_vaapi, video_codec_context);
        return false;
    }

    if(!gsr_video_encoder_vaapi_setup_textures(encoder_vaapi, video_codec_context, frame)) {
        gsr_video_encoder_vaapi_stop(encoder_vaapi, video_codec_context);
        return false;
    }

    return true;
}

void gsr_video_encoder_vaapi_stop(gsr_video_encoder_vaapi *self, AVCodecContext *video_codec_context) {
    self->params.egl->glDeleteTextures(2, self->target_textures);
    self->target_textures[0] = 0;
    self->target_textures[1] = 0;

    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);
    if(self->device_ctx)
        av_buffer_unref(&self->device_ctx);

    for(uint32_t i = 0; i < self->prime.num_objects; ++i) {
        if(self->prime.objects[i].fd > 0) {
            close(self->prime.objects[i].fd);
            self->prime.objects[i].fd = 0;
        }
    }
}

static void gsr_video_encoder_vaapi_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_vaapi *encoder_vaapi = encoder->priv;
    textures[0] = encoder_vaapi->target_textures[0];
    textures[1] = encoder_vaapi->target_textures[1];
    *num_textures = 2;
    *destination_color = encoder_vaapi->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
}

static void gsr_video_encoder_vaapi_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    gsr_video_encoder_vaapi_stop(encoder->priv, video_codec_context);
    free(encoder->priv);
    free(encoder);
}

gsr_video_encoder* gsr_video_encoder_vaapi_create(const gsr_video_encoder_vaapi_params *params) {
    gsr_video_encoder *encoder = calloc(1, sizeof(gsr_video_encoder));
    if(!encoder)
        return NULL;

    gsr_video_encoder_vaapi *encoder_vaapi = calloc(1, sizeof(gsr_video_encoder_vaapi));
    if(!encoder_vaapi) {
        free(encoder);
        return NULL;
    }

    encoder_vaapi->params = *params;

    *encoder = (gsr_video_encoder) {
        .get_supported_codecs = gsr_video_encoder_vaapi_get_supported_codecs,
        .start = gsr_video_encoder_vaapi_start,
        .copy_textures_to_frame = NULL,
        .get_textures = gsr_video_encoder_vaapi_get_textures,
        .destroy = gsr_video_encoder_vaapi_destroy,
        .priv = encoder_vaapi
    };

    return encoder;
}
