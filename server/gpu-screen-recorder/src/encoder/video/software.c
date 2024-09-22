#include "../../../include/encoder/video/software.h"
#include "../../../include/egl.h"

#include <libavcodec/avcodec.h>
#include <libavutil/frame.h>

#include <stdlib.h>

#define LINESIZE_ALIGNMENT 4

typedef struct {
    gsr_video_encoder_software_params params;

    unsigned int target_textures[2];
} gsr_video_encoder_software;

static unsigned int gl_create_texture(gsr_egl *egl, int width, int height, int internal_format, unsigned int format) {
    unsigned int texture_id = 0;
    egl->glGenTextures(1, &texture_id);
    egl->glBindTexture(GL_TEXTURE_2D, texture_id);
    egl->glTexImage2D(GL_TEXTURE_2D, 0, internal_format, width, height, 0, format, GL_UNSIGNED_BYTE, NULL);

    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    egl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);

    egl->glBindTexture(GL_TEXTURE_2D, 0);
    return texture_id;
}

static bool gsr_video_encoder_software_setup_textures(gsr_video_encoder_software *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    int res = av_frame_get_buffer(frame, LINESIZE_ALIGNMENT);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_software_setup_textures: av_frame_get_buffer failed: %d\n", res);
        return false;
    }

    res = av_frame_make_writable(frame);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_software_setup_textures: av_frame_make_writable failed: %d\n", res);
        return false;
    }

    const unsigned int internal_formats_nv12[2] = { GL_R8, GL_RG8 };
    const unsigned int internal_formats_p010[2] = { GL_R16, GL_RG16 };
    const unsigned int formats[2] = { GL_RED, GL_RG };
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

    for(int i = 0; i < 2; ++i) {
        self->target_textures[i] = gl_create_texture(self->params.egl, video_codec_context->width / div[i], video_codec_context->height / div[i], self->params.color_depth == GSR_COLOR_DEPTH_8_BITS ? internal_formats_nv12[i] : internal_formats_p010[i], formats[i]);
        if(self->target_textures[i] == 0) {
            fprintf(stderr, "gsr error: gsr_capture_kms_setup_cuda_textures: failed to create opengl texture\n");
            return false;
        }
    }

    return true;
}

static gsr_supported_video_codecs gsr_video_encoder_software_get_supported_codecs(gsr_video_encoder *encoder, bool cleanup) {
    (void)encoder;
    (void)cleanup;
    return (gsr_supported_video_codecs) {
        .h264 = true,
        .hevc = false,
        .hevc_hdr = false,
        .hevc_10bit = false,
        .av1 = false,
        .av1_hdr = false,
        .av1_10bit = false,
        .vp8 = false,
        .vp9 = false
    };
}

static void gsr_video_encoder_software_stop(gsr_video_encoder_software *self, AVCodecContext *video_codec_context);

static bool gsr_video_encoder_software_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_video_encoder_software *encoder_software = encoder->priv;

    video_codec_context->width = FFALIGN(video_codec_context->width, LINESIZE_ALIGNMENT);
    video_codec_context->height = FFALIGN(video_codec_context->height, 2);

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    if(!gsr_video_encoder_software_setup_textures(encoder_software, video_codec_context, frame)) {
        gsr_video_encoder_software_stop(encoder_software, video_codec_context);
        return false;
    }

    return true;
}

void gsr_video_encoder_software_stop(gsr_video_encoder_software *self, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    self->params.egl->glDeleteTextures(2, self->target_textures);
    self->target_textures[0] = 0;
    self->target_textures[1] = 0;
}

static void gsr_video_encoder_software_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame) {
    gsr_video_encoder_software *encoder_software = encoder->priv;
    // TODO: hdr support
    const unsigned int formats[2] = { GL_RED, GL_RG };
    for(int i = 0; i < 2; ++i) {
        encoder_software->params.egl->glBindTexture(GL_TEXTURE_2D, encoder_software->target_textures[i]);
        // We could use glGetTexSubImage and then we wouldn't have to use a specific linesize (LINESIZE_ALIGNMENT) that adds padding,
        // but glGetTexSubImage is only available starting from opengl 4.5.
        encoder_software->params.egl->glGetTexImage(GL_TEXTURE_2D, 0, formats[i], GL_UNSIGNED_BYTE, frame->data[i]);
    }
    encoder_software->params.egl->glBindTexture(GL_TEXTURE_2D, 0);
    // cap_kms->kms.base.egl->eglSwapBuffers(cap_kms->kms.base.egl->egl_display, cap_kms->kms.base.egl->egl_surface);

    encoder_software->params.egl->glFlush();
    encoder_software->params.egl->glFinish();
}

static void gsr_video_encoder_software_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_software *encoder_software = encoder->priv;
    textures[0] = encoder_software->target_textures[0];
    textures[1] = encoder_software->target_textures[1];
    *num_textures = 2;
    *destination_color = encoder_software->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
}

static void gsr_video_encoder_software_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    gsr_video_encoder_software_stop(encoder->priv, video_codec_context);
    free(encoder->priv);
    free(encoder);
}

gsr_video_encoder* gsr_video_encoder_software_create(const gsr_video_encoder_software_params *params) {
    gsr_video_encoder *encoder = calloc(1, sizeof(gsr_video_encoder));
    if(!encoder)
        return NULL;

    gsr_video_encoder_software *encoder_software = calloc(1, sizeof(gsr_video_encoder_software));
    if(!encoder_software) {
        free(encoder);
        return NULL;
    }

    encoder_software->params = *params;

    *encoder = (gsr_video_encoder) {
        .get_supported_codecs = gsr_video_encoder_software_get_supported_codecs,
        .start = gsr_video_encoder_software_start,
        .copy_textures_to_frame = gsr_video_encoder_software_copy_textures_to_frame,
        .get_textures = gsr_video_encoder_software_get_textures,
        .destroy = gsr_video_encoder_software_destroy,
        .priv = encoder_software
    };

    return encoder;
}
