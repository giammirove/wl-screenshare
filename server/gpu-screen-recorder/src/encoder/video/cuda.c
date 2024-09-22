#include "../../../include/encoder/video/cuda.h"
#include "../../../include/egl.h"
#include "../../../include/cuda.h"
#include "../../../external/nvEncodeAPI.h"

#include <libavcodec/avcodec.h>
#include <libavutil/hwcontext_cuda.h>

#include <stdlib.h>
#include <dlfcn.h>

typedef struct {
    gsr_video_encoder_cuda_params params;

    unsigned int target_textures[2];

    AVBufferRef *device_ctx;

    gsr_cuda cuda;
    CUgraphicsResource cuda_graphics_resources[2];
    CUarray mapped_arrays[2];
    CUstream cuda_stream;
} gsr_video_encoder_cuda;

static bool gsr_video_encoder_cuda_setup_context(gsr_video_encoder_cuda *self, AVCodecContext *video_codec_context) {
    self->device_ctx = av_hwdevice_ctx_alloc(AV_HWDEVICE_TYPE_CUDA);
    if(!self->device_ctx) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_context failed: failed to create hardware device context\n");
        return false;
    }

    AVHWDeviceContext *hw_device_context = (AVHWDeviceContext*)self->device_ctx->data;
    AVCUDADeviceContext *cuda_device_context = (AVCUDADeviceContext*)hw_device_context->hwctx;
    cuda_device_context->cuda_ctx = self->cuda.cu_ctx;
    if(av_hwdevice_ctx_init(self->device_ctx) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_context failed: failed to create hardware device context\n");
        av_buffer_unref(&self->device_ctx);
        return false;
    }

    AVBufferRef *frame_context = av_hwframe_ctx_alloc(self->device_ctx);
    if(!frame_context) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_context failed: failed to create hwframe context\n");
        av_buffer_unref(&self->device_ctx);
        return false;
    }

    AVHWFramesContext *hw_frame_context = (AVHWFramesContext*)frame_context->data;
    hw_frame_context->width = video_codec_context->width;
    hw_frame_context->height = video_codec_context->height;
    hw_frame_context->sw_format = self->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? AV_PIX_FMT_P010LE : AV_PIX_FMT_NV12;
    hw_frame_context->format = video_codec_context->pix_fmt;
    hw_frame_context->device_ctx = (AVHWDeviceContext*)self->device_ctx->data;

    if (av_hwframe_ctx_init(frame_context) < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_context failed: failed to initialize hardware frame context "
                        "(note: ffmpeg version needs to be > 4.0)\n");
        av_buffer_unref(&self->device_ctx);
        //av_buffer_unref(&frame_context);
        return false;
    }

    self->cuda_stream = cuda_device_context->stream;
    video_codec_context->hw_frames_ctx = av_buffer_ref(frame_context);
    av_buffer_unref(&frame_context);
    return true;
}

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

static bool cuda_register_opengl_texture(gsr_cuda *cuda, CUgraphicsResource *cuda_graphics_resource, CUarray *mapped_array, unsigned int texture_id) {
    CUresult res;
    res = cuda->cuGraphicsGLRegisterImage(cuda_graphics_resource, texture_id, GL_TEXTURE_2D, CU_GRAPHICS_REGISTER_FLAGS_NONE);
    if (res != CUDA_SUCCESS) {
        const char *err_str = "unknown";
        cuda->cuGetErrorString(res, &err_str);
        fprintf(stderr, "gsr error: cuda_register_opengl_texture: cuGraphicsGLRegisterImage failed, error: %s, texture " "id: %u\n", err_str, texture_id);
        return false;
    }

    res = cuda->cuGraphicsResourceSetMapFlags(*cuda_graphics_resource, CU_GRAPHICS_MAP_RESOURCE_FLAGS_NONE);
    res = cuda->cuGraphicsMapResources(1, cuda_graphics_resource, 0);

    res = cuda->cuGraphicsSubResourceGetMappedArray(mapped_array, *cuda_graphics_resource, 0, 0);
    return true;
}

static bool gsr_video_encoder_cuda_setup_textures(gsr_video_encoder_cuda *self, AVCodecContext *video_codec_context, AVFrame *frame) {
    const int res = av_hwframe_get_buffer(video_codec_context->hw_frames_ctx, frame, 0);
    if(res < 0) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_textures: av_hwframe_get_buffer failed: %d\n", res);
        return false;
    }

    const unsigned int internal_formats_nv12[2] = { GL_R8, GL_RG8 };
    const unsigned int internal_formats_p010[2] = { GL_R16, GL_RG16 };
    const unsigned int formats[2] = { GL_RED, GL_RG };
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size

    for(int i = 0; i < 2; ++i) {
        self->target_textures[i] = gl_create_texture(self->params.egl, video_codec_context->width / div[i], video_codec_context->height / div[i], self->params.color_depth == GSR_COLOR_DEPTH_8_BITS ? internal_formats_nv12[i] : internal_formats_p010[i], formats[i]);
        if(self->target_textures[i] == 0) {
            fprintf(stderr, "gsr error: gsr_video_encoder_cuda_setup_textures: failed to create opengl texture\n");
            return false;
        }

        if(!cuda_register_opengl_texture(&self->cuda, &self->cuda_graphics_resources[i], &self->mapped_arrays[i], self->target_textures[i])) {
            return false;
        }
    }

    return true;
}

static void* open_nvenc_library(void) {
    dlerror(); /* clear */
    void *lib = dlopen("libnvidia-encode.so.1", RTLD_LAZY);
    if(!lib) {
        lib = dlopen("libnvidia-encode.so", RTLD_LAZY);
        if(!lib) {
            fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs failed: failed to load libnvidia-encode.so/libnvidia-encode.so.1, error: %s\n", dlerror());
            return NULL;
        }
    }
    return lib;
}

static bool profile_is_h264(const GUID *profile_guid) {
    const GUID *h264_guids[] = {
        &NV_ENC_H264_PROFILE_BASELINE_GUID,
        &NV_ENC_H264_PROFILE_MAIN_GUID,
        &NV_ENC_H264_PROFILE_HIGH_GUID,
        &NV_ENC_H264_PROFILE_PROGRESSIVE_HIGH_GUID,
        &NV_ENC_H264_PROFILE_CONSTRAINED_HIGH_GUID
    };

    for(int i = 0; i < 5; ++i) {
        if(memcmp(profile_guid, h264_guids[i], sizeof(GUID)) == 0)
            return true;
    }

    return false;
}

static bool profile_is_hevc(const GUID *profile_guid) {
    const GUID *h264_guids[] = {
        &NV_ENC_HEVC_PROFILE_MAIN_GUID,
    };

    for(int i = 0; i < 1; ++i) {
        if(memcmp(profile_guid, h264_guids[i], sizeof(GUID)) == 0)
            return true;
    }

    return false;
}

static bool profile_is_hevc_10bit(const GUID *profile_guid) {
    const GUID *h264_guids[] = {
        &NV_ENC_HEVC_PROFILE_MAIN10_GUID,
    };

    for(int i = 0; i < 1; ++i) {
        if(memcmp(profile_guid, h264_guids[i], sizeof(GUID)) == 0)
            return true;
    }

    return false;
}

static bool profile_is_av1(const GUID *profile_guid) {
    const GUID *h264_guids[] = {
        &NV_ENC_AV1_PROFILE_MAIN_GUID,
    };

    for(int i = 0; i < 1; ++i) {
        if(memcmp(profile_guid, h264_guids[i], sizeof(GUID)) == 0)
            return true;
    }

    return false;
}

static bool encoder_get_supported_profiles(const NV_ENCODE_API_FUNCTION_LIST *function_list, void *nvenc_encoder, const GUID *encoder_guid, gsr_supported_video_codecs *supported_video_codecs) {
    bool success = false;
    GUID *profile_guids = NULL;

    uint32_t profile_guid_count = 0;
    if(function_list->nvEncGetEncodeProfileGUIDCount(nvenc_encoder, *encoder_guid, &profile_guid_count) != NV_ENC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncGetEncodeProfileGUIDCount failed, error: %s\n", function_list->nvEncGetLastErrorString(nvenc_encoder));
        goto fail;
    }

    if(profile_guid_count == 0)
        goto fail;

    profile_guids = calloc(profile_guid_count, sizeof(GUID));
    if(!profile_guids) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: failed to allocate %d guids\n", (int)profile_guid_count);
        goto fail;
    }

    if(function_list->nvEncGetEncodeProfileGUIDs(nvenc_encoder, *encoder_guid, profile_guids, profile_guid_count, &profile_guid_count) != NV_ENC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncGetEncodeProfileGUIDs failed, error: %s\n", function_list->nvEncGetLastErrorString(nvenc_encoder));
        goto fail;
    }

    for(uint32_t i = 0; i < profile_guid_count; ++i) {
        if(profile_is_h264(&profile_guids[i])) {
            supported_video_codecs->h264 = true;
        } else if(profile_is_hevc(&profile_guids[i])) {
            supported_video_codecs->hevc = true;
        } else if(profile_is_hevc_10bit(&profile_guids[i])) {
            supported_video_codecs->hevc_hdr = true;
            supported_video_codecs->hevc_10bit = true;
        } else if(profile_is_av1(&profile_guids[i])) {
            supported_video_codecs->av1 = true;
            supported_video_codecs->av1_hdr = true;
            supported_video_codecs->av1_10bit = true;
        }
    }

    success = true;
    fail:

    if(profile_guids)
        free(profile_guids);

    return success;
}

static bool get_supported_video_codecs(const NV_ENCODE_API_FUNCTION_LIST *function_list, void *nvenc_encoder, gsr_supported_video_codecs *supported_video_codecs) {
    bool success = false;
    GUID *encoder_guids = NULL;
    *supported_video_codecs = (gsr_supported_video_codecs){0};

    uint32_t encode_guid_count = 0;
    if(function_list->nvEncGetEncodeGUIDCount(nvenc_encoder, &encode_guid_count) != NV_ENC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncGetEncodeGUIDCount failed, error: %s\n", function_list->nvEncGetLastErrorString(nvenc_encoder));
        goto fail;
    }

    if(encode_guid_count == 0)
        goto fail;

    encoder_guids = calloc(encode_guid_count, sizeof(GUID));
    if(!encoder_guids) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: failed to allocate %d guids\n", (int)encode_guid_count);
        goto fail;
    }

    if(function_list->nvEncGetEncodeGUIDs(nvenc_encoder, encoder_guids, encode_guid_count, &encode_guid_count) != NV_ENC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncGetEncodeGUIDs failed, error: %s\n", function_list->nvEncGetLastErrorString(nvenc_encoder));
        goto fail;
    }

    for(uint32_t i = 0; i < encode_guid_count; ++i) {
        encoder_get_supported_profiles(function_list, nvenc_encoder, &encoder_guids[i], supported_video_codecs);
    }

    success = true;
    fail:

    if(encoder_guids)
        free(encoder_guids);

    return success;
}

#define NVENCAPI_VERSION_470 (11 | (1 << 24))
#define NVENCAPI_STRUCT_VERSION_470(ver) ((uint32_t)NVENCAPI_VERSION_470 | ((ver)<<16) | (0x7 << 28))

static gsr_supported_video_codecs gsr_video_encoder_cuda_get_supported_codecs(gsr_video_encoder *encoder, bool cleanup) {
    (void)encoder;

    void *nvenc_lib = NULL;
    void *nvenc_encoder = NULL;
    gsr_cuda cuda;
    memset(&cuda, 0, sizeof(cuda));
    gsr_supported_video_codecs supported_video_codecs = {0};

    if(!gsr_cuda_load(&cuda, NULL, false)) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: failed to load cuda\n");
        goto done;
    }

    nvenc_lib = open_nvenc_library();
    if(!nvenc_lib)
        goto done;

    typedef NVENCSTATUS NVENCAPI (*FUNC_NvEncodeAPICreateInstance)(NV_ENCODE_API_FUNCTION_LIST *functionList);
    FUNC_NvEncodeAPICreateInstance nvEncodeAPICreateInstance = (FUNC_NvEncodeAPICreateInstance)dlsym(nvenc_lib, "NvEncodeAPICreateInstance");
    if(!nvEncodeAPICreateInstance) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: failed to find NvEncodeAPICreateInstance in libnvidia-encode.so\n");
        goto done;
    }

    NV_ENCODE_API_FUNCTION_LIST function_list;
    memset(&function_list, 0, sizeof(function_list));
    function_list.version = NVENCAPI_STRUCT_VERSION(2);
    if(nvEncodeAPICreateInstance(&function_list) != NV_ENC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncodeAPICreateInstance failed\n");
        goto done;
    }

    NV_ENC_OPEN_ENCODE_SESSION_EX_PARAMS params;
    memset(&params, 0, sizeof(params));
    params.version = NVENCAPI_STRUCT_VERSION(1);
    params.deviceType = NV_ENC_DEVICE_TYPE_CUDA;
    params.device = cuda.cu_ctx;
    params.apiVersion = NVENCAPI_VERSION;
    if(function_list.nvEncOpenEncodeSessionEx(&params, &nvenc_encoder) != NV_ENC_SUCCESS) {
        // Old nvidia gpus dont support the new nvenc api (which is required for av1).
        // In such cases fallback to old api version if possible and try again.
        function_list.version = NVENCAPI_STRUCT_VERSION_470(2);
        if(nvEncodeAPICreateInstance(&function_list) != NV_ENC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncodeAPICreateInstance (retry) failed\n");
            goto done;
        }

        params.version = NVENCAPI_STRUCT_VERSION_470(1);
        params.apiVersion = NVENCAPI_VERSION_470;
        if(function_list.nvEncOpenEncodeSessionEx(&params, &nvenc_encoder) != NV_ENC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_video_encoder_cuda_get_supported_codecs: nvEncOpenEncodeSessionEx (retry) failed\n");
            goto done;
        }
    }

    get_supported_video_codecs(&function_list, nvenc_encoder, &supported_video_codecs);

    done:
    if(cleanup) {
        if(nvenc_encoder)
            function_list.nvEncDestroyEncoder(nvenc_encoder);
        if(nvenc_lib)
            dlclose(nvenc_lib);
        gsr_cuda_unload(&cuda);
    }

    return supported_video_codecs;
}

static void gsr_video_encoder_cuda_stop(gsr_video_encoder_cuda *self, AVCodecContext *video_codec_context);

static bool gsr_video_encoder_cuda_start(gsr_video_encoder *encoder, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_video_encoder_cuda *encoder_cuda = encoder->priv;

    const bool overclock = gsr_egl_get_display_server(encoder_cuda->params.egl) == GSR_DISPLAY_SERVER_X11 ? encoder_cuda->params.overclock : false;
    if(!gsr_cuda_load(&encoder_cuda->cuda, encoder_cuda->params.egl->x11.dpy, overclock)) {
        fprintf(stderr, "gsr error: gsr_video_encoder_cuda_start: failed to load cuda\n");
        gsr_video_encoder_cuda_stop(encoder_cuda, video_codec_context);
        return false;
    }

    if(!gsr_video_encoder_cuda_setup_context(encoder_cuda, video_codec_context)) {
        gsr_video_encoder_cuda_stop(encoder_cuda, video_codec_context);
        return false;
    }

    if(!gsr_video_encoder_cuda_setup_textures(encoder_cuda, video_codec_context, frame)) {
        gsr_video_encoder_cuda_stop(encoder_cuda, video_codec_context);
        return false;
    }

    return true;
}

void gsr_video_encoder_cuda_stop(gsr_video_encoder_cuda *self, AVCodecContext *video_codec_context) {
    self->params.egl->glDeleteTextures(2, self->target_textures);
    self->target_textures[0] = 0;
    self->target_textures[1] = 0;

    if(video_codec_context->hw_frames_ctx)
        av_buffer_unref(&video_codec_context->hw_frames_ctx);
    if(self->device_ctx)
        av_buffer_unref(&self->device_ctx);

    if(self->cuda.cu_ctx) {
        for(int i = 0; i < 2; ++i) {
            if(self->cuda_graphics_resources[i]) {
                self->cuda.cuGraphicsUnmapResources(1, &self->cuda_graphics_resources[i], 0);
                self->cuda.cuGraphicsUnregisterResource(self->cuda_graphics_resources[i]);
                self->cuda_graphics_resources[i] = 0;
            }
        }
    }

    gsr_cuda_unload(&self->cuda);
}

static void gsr_video_encoder_cuda_copy_textures_to_frame(gsr_video_encoder *encoder, AVFrame *frame) {
    gsr_video_encoder_cuda *encoder_cuda = encoder->priv;
    const int div[2] = {1, 2}; // divide UV texture size by 2 because chroma is half size
    for(int i = 0; i < 2; ++i) {
        CUDA_MEMCPY2D memcpy_struct;
        memcpy_struct.srcXInBytes = 0;
        memcpy_struct.srcY = 0;
        memcpy_struct.srcMemoryType = CU_MEMORYTYPE_ARRAY;

        memcpy_struct.dstXInBytes = 0;
        memcpy_struct.dstY = 0;
        memcpy_struct.dstMemoryType = CU_MEMORYTYPE_DEVICE;

        memcpy_struct.srcArray = encoder_cuda->mapped_arrays[i];
        memcpy_struct.srcPitch = frame->width / div[i];
        memcpy_struct.dstDevice = (CUdeviceptr)frame->data[i];
        memcpy_struct.dstPitch = frame->linesize[i];
        memcpy_struct.WidthInBytes = frame->width * (encoder_cuda->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? 2 : 1);
        memcpy_struct.Height = frame->height / div[i];
        // TODO: Remove this copy if possible
        encoder_cuda->cuda.cuMemcpy2DAsync_v2(&memcpy_struct, encoder_cuda->cuda_stream);
    }

    // TODO: needed?
    encoder_cuda->cuda.cuStreamSynchronize(encoder_cuda->cuda_stream);
}

static void gsr_video_encoder_cuda_get_textures(gsr_video_encoder *encoder, unsigned int *textures, int *num_textures, gsr_destination_color *destination_color) {
    gsr_video_encoder_cuda *encoder_cuda = encoder->priv;
    textures[0] = encoder_cuda->target_textures[0];
    textures[1] = encoder_cuda->target_textures[1];
    *num_textures = 2;
    *destination_color = encoder_cuda->params.color_depth == GSR_COLOR_DEPTH_10_BITS ? GSR_DESTINATION_COLOR_P010 : GSR_DESTINATION_COLOR_NV12;
}

static void gsr_video_encoder_cuda_destroy(gsr_video_encoder *encoder, AVCodecContext *video_codec_context) {
    gsr_video_encoder_cuda_stop(encoder->priv, video_codec_context);
    free(encoder->priv);
    free(encoder);
}

gsr_video_encoder* gsr_video_encoder_cuda_create(const gsr_video_encoder_cuda_params *params) {
    gsr_video_encoder *encoder = calloc(1, sizeof(gsr_video_encoder));
    if(!encoder)
        return NULL;

    gsr_video_encoder_cuda *encoder_cuda = calloc(1, sizeof(gsr_video_encoder_cuda));
    if(!encoder_cuda) {
        free(encoder);
        return NULL;
    }

    encoder_cuda->params = *params;

    *encoder = (gsr_video_encoder) {
        .get_supported_codecs = gsr_video_encoder_cuda_get_supported_codecs,
        .start = gsr_video_encoder_cuda_start,
        .copy_textures_to_frame = gsr_video_encoder_cuda_copy_textures_to_frame,
        .get_textures = gsr_video_encoder_cuda_get_textures,
        .destroy = gsr_video_encoder_cuda_destroy,
        .priv = encoder_cuda
    };

    return encoder;
}
