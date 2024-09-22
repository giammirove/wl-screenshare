#include "../../include/capture/nvfbc.h"
#include "../../external/NvFBC.h"
#include "../../include/egl.h"
#include "../../include/utils.h"
#include "../../include/color_conversion.h"

#include <dlfcn.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <math.h>

#include <X11/Xlib.h>
#include <libavcodec/avcodec.h>

typedef struct {
    gsr_capture_nvfbc_params params;
    void *library;

    NVFBC_SESSION_HANDLE nv_fbc_handle;
    PNVFBCCREATEINSTANCE nv_fbc_create_instance;
    NVFBC_API_FUNCTION_LIST nv_fbc_function_list;
    bool fbc_handle_created;
    bool capture_session_created;

    NVFBC_TOGL_SETUP_PARAMS setup_params;

    bool supports_direct_cursor;
    bool capture_region;
    uint32_t x, y, width, height;
    NVFBC_TRACKING_TYPE tracking_type;
    uint32_t output_id;
    uint32_t tracking_width, tracking_height;
    bool nvfbc_needs_recreate;
    double nvfbc_dead_start;
} gsr_capture_nvfbc;

static int max_int(int a, int b) {
    return a > b ? a : b;
}

/* Returns 0 on failure */
static uint32_t get_output_id_from_display_name(NVFBC_RANDR_OUTPUT_INFO *outputs, uint32_t num_outputs, const char *display_name, uint32_t *width, uint32_t *height) {
    if(!outputs)
        return 0;

    for(uint32_t i = 0; i < num_outputs; ++i) {
        if(strcmp(outputs[i].name, display_name) == 0) {
            *width = outputs[i].trackedBox.w;
            *height = outputs[i].trackedBox.h;
            return outputs[i].dwId;
        }
    }

    return 0;
}

/* TODO: Test with optimus and open kernel modules */
static bool get_driver_version(int *major, int *minor) {
    *major = 0;
    *minor = 0;

    FILE *f = fopen("/proc/driver/nvidia/version", "rb");
    if(!f) {
        fprintf(stderr, "gsr warning: failed to get nvidia driver version (failed to read /proc/driver/nvidia/version)\n");
        return false;
    }

    char buffer[2048];
    size_t bytes_read = fread(buffer, 1, sizeof(buffer) - 1, f);
    buffer[bytes_read] = '\0';

    bool success = false;
    const char *p = strstr(buffer, "Kernel Module");
    if(p) {
        p += 13;
        int driver_major_version = 0, driver_minor_version = 0;
        if(sscanf(p, "%d.%d", &driver_major_version, &driver_minor_version) == 2) {
            *major = driver_major_version;
            *minor = driver_minor_version;
            success = true;
        }
    }

    if(!success)
        fprintf(stderr, "gsr warning: failed to get nvidia driver version\n");

    fclose(f);
    return success;
}

static bool version_at_least(int major, int minor, int expected_major, int expected_minor) {
    return major > expected_major || (major == expected_major && minor >= expected_minor);
}

static bool version_less_than(int major, int minor, int expected_major, int expected_minor) {
    return major < expected_major || (major == expected_major && minor < expected_minor);
}

static void set_func_ptr(void **dst, void *src) {
    *dst = src;
}

static bool gsr_capture_nvfbc_load_library(gsr_capture *cap) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    dlerror(); /* clear */
    void *lib = dlopen("libnvidia-fbc.so.1", RTLD_LAZY);
    if(!lib) {
        fprintf(stderr, "gsr error: failed to load libnvidia-fbc.so.1, error: %s\n", dlerror());
        return false;
    }

    set_func_ptr((void**)&cap_nvfbc->nv_fbc_create_instance, dlsym(lib, "NvFBCCreateInstance"));
    if(!cap_nvfbc->nv_fbc_create_instance) {
        fprintf(stderr, "gsr error: unable to resolve symbol 'NvFBCCreateInstance'\n");
        dlclose(lib);
        return false;
    }

    memset(&cap_nvfbc->nv_fbc_function_list, 0, sizeof(cap_nvfbc->nv_fbc_function_list));
    cap_nvfbc->nv_fbc_function_list.dwVersion = NVFBC_VERSION;
    NVFBCSTATUS status = cap_nvfbc->nv_fbc_create_instance(&cap_nvfbc->nv_fbc_function_list);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: failed to create NvFBC instance (status: %d)\n", status);
        dlclose(lib);
        return false;
    }

    cap_nvfbc->library = lib;
    return true;
}

/* TODO: check for glx swap control extension string (GLX_EXT_swap_control, etc) */
static void set_vertical_sync_enabled(gsr_egl *egl, int enabled) {
    int result = 0;

    if(egl->glXSwapIntervalEXT) {
        egl->glXSwapIntervalEXT(egl->x11.dpy, egl->x11.window, enabled ? 1 : 0);
    } else if(egl->glXSwapIntervalMESA) {
        result = egl->glXSwapIntervalMESA(enabled ? 1 : 0);
    } else if(egl->glXSwapIntervalSGI) {
        result = egl->glXSwapIntervalSGI(enabled ? 1 : 0);
    } else {
        static int warned = 0;
        if (!warned) {
            warned = 1;
            fprintf(stderr, "gsr warning: setting vertical sync not supported\n");
        }
    }

    if(result != 0)
        fprintf(stderr, "gsr warning: setting vertical sync failed\n");
}

static void gsr_capture_nvfbc_destroy_session(gsr_capture_nvfbc *cap_nvfbc) {
    if(cap_nvfbc->fbc_handle_created && cap_nvfbc->capture_session_created) {
        NVFBC_DESTROY_CAPTURE_SESSION_PARAMS destroy_capture_params;
        memset(&destroy_capture_params, 0, sizeof(destroy_capture_params));
        destroy_capture_params.dwVersion = NVFBC_DESTROY_CAPTURE_SESSION_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyCaptureSession(cap_nvfbc->nv_fbc_handle, &destroy_capture_params);
        cap_nvfbc->capture_session_created = false;
    }
}

static void gsr_capture_nvfbc_destroy_handle(gsr_capture_nvfbc *cap_nvfbc) {
    if(cap_nvfbc->fbc_handle_created) {
        NVFBC_DESTROY_HANDLE_PARAMS destroy_params;
        memset(&destroy_params, 0, sizeof(destroy_params));
        destroy_params.dwVersion = NVFBC_DESTROY_HANDLE_PARAMS_VER;
        cap_nvfbc->nv_fbc_function_list.nvFBCDestroyHandle(cap_nvfbc->nv_fbc_handle, &destroy_params);
        cap_nvfbc->fbc_handle_created = false;
        cap_nvfbc->nv_fbc_handle = 0;
    }
}

static void gsr_capture_nvfbc_destroy_session_and_handle(gsr_capture_nvfbc *cap_nvfbc) {
    gsr_capture_nvfbc_destroy_session(cap_nvfbc);
    gsr_capture_nvfbc_destroy_handle(cap_nvfbc);
}

static int gsr_capture_nvfbc_setup_handle(gsr_capture_nvfbc *cap_nvfbc) {
    NVFBCSTATUS status;

    NVFBC_CREATE_HANDLE_PARAMS create_params;
    memset(&create_params, 0, sizeof(create_params));
    create_params.dwVersion = NVFBC_CREATE_HANDLE_PARAMS_VER;
    create_params.bExternallyManagedContext = NVFBC_TRUE;
    create_params.glxCtx = cap_nvfbc->params.egl->glx_context;
    create_params.glxFBConfig = cap_nvfbc->params.egl->glx_fb_config;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
    if(status != NVFBC_SUCCESS) {
        // Reverse engineering for interoperability
        const uint8_t enable_key[] = { 0xac, 0x10, 0xc9, 0x2e, 0xa5, 0xe6, 0x87, 0x4f, 0x8f, 0x4b, 0xf4, 0x61, 0xf8, 0x56, 0x27, 0xe9 };
        create_params.privateData = enable_key;
        create_params.privateDataSize = 16;

        status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateHandle(&cap_nvfbc->nv_fbc_handle, &create_params);
        if(status != NVFBC_SUCCESS) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
            goto error_cleanup;
        }
    }
    cap_nvfbc->fbc_handle_created = true;

    NVFBC_GET_STATUS_PARAMS status_params;
    memset(&status_params, 0, sizeof(status_params));
    status_params.dwVersion = NVFBC_GET_STATUS_PARAMS_VER;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCGetStatus(cap_nvfbc->nv_fbc_handle, &status_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        goto error_cleanup;
    }

    if(status_params.bCanCreateNow == NVFBC_FALSE) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: it's not possible to create a capture session on this system\n");
        goto error_cleanup;
    }

    cap_nvfbc->tracking_width = XWidthOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.egl->x11.dpy));
    cap_nvfbc->tracking_height = XHeightOfScreen(DefaultScreenOfDisplay(cap_nvfbc->params.egl->x11.dpy));
    cap_nvfbc->tracking_type = strcmp(cap_nvfbc->params.display_to_capture, "screen") == 0 ? NVFBC_TRACKING_SCREEN : NVFBC_TRACKING_OUTPUT;
    if(cap_nvfbc->tracking_type == NVFBC_TRACKING_OUTPUT) {
        if(!status_params.bXRandRAvailable) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the xrandr extension is not available\n");
            goto error_cleanup;
        }

        if(status_params.bInModeset) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: the x server is in modeset, unable to record\n");
            goto error_cleanup;
        }

        cap_nvfbc->output_id = get_output_id_from_display_name(status_params.outputs, status_params.dwOutputNum, cap_nvfbc->params.display_to_capture, &cap_nvfbc->tracking_width, &cap_nvfbc->tracking_height);
        if(cap_nvfbc->output_id == 0) {
            fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: display '%s' not found\n", cap_nvfbc->params.display_to_capture);
            goto error_cleanup;
        }
    }

    return 0;

    error_cleanup:
    gsr_capture_nvfbc_destroy_session_and_handle(cap_nvfbc);
    return -1;
}

static int gsr_capture_nvfbc_setup_session(gsr_capture_nvfbc *cap_nvfbc) {
    NVFBC_CREATE_CAPTURE_SESSION_PARAMS create_capture_params;
    memset(&create_capture_params, 0, sizeof(create_capture_params));
    create_capture_params.dwVersion = NVFBC_CREATE_CAPTURE_SESSION_PARAMS_VER;
    create_capture_params.eCaptureType = NVFBC_CAPTURE_TO_GL;
    create_capture_params.bWithCursor = (!cap_nvfbc->params.direct_capture || cap_nvfbc->supports_direct_cursor) ? NVFBC_TRUE : NVFBC_FALSE;
    if(!cap_nvfbc->params.record_cursor)
        create_capture_params.bWithCursor = false;
    if(cap_nvfbc->capture_region)
        create_capture_params.captureBox = (NVFBC_BOX){ cap_nvfbc->x, cap_nvfbc->y, cap_nvfbc->width, cap_nvfbc->height };
    create_capture_params.eTrackingType = cap_nvfbc->tracking_type;
    create_capture_params.dwSamplingRateMs = (uint32_t)ceilf(1000.0f / (float)cap_nvfbc->params.fps);
    create_capture_params.bAllowDirectCapture = cap_nvfbc->params.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bPushModel = cap_nvfbc->params.direct_capture ? NVFBC_TRUE : NVFBC_FALSE;
    create_capture_params.bDisableAutoModesetRecovery = true;
    if(cap_nvfbc->tracking_type == NVFBC_TRACKING_OUTPUT)
        create_capture_params.dwOutputId = cap_nvfbc->output_id;

    NVFBCSTATUS status = cap_nvfbc->nv_fbc_function_list.nvFBCCreateCaptureSession(cap_nvfbc->nv_fbc_handle, &create_capture_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        return -1;
    }
    cap_nvfbc->capture_session_created = true;

    memset(&cap_nvfbc->setup_params, 0, sizeof(cap_nvfbc->setup_params));
    cap_nvfbc->setup_params.dwVersion = NVFBC_TOGL_SETUP_PARAMS_VER;
    cap_nvfbc->setup_params.eBufferFormat = NVFBC_BUFFER_FORMAT_BGRA;

    status = cap_nvfbc->nv_fbc_function_list.nvFBCToGLSetUp(cap_nvfbc->nv_fbc_handle, &cap_nvfbc->setup_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_start failed: %s\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle));
        gsr_capture_nvfbc_destroy_session(cap_nvfbc);
        return -1;
    }

    return 0;
}

static void gsr_capture_nvfbc_stop(gsr_capture_nvfbc *cap_nvfbc) {
    gsr_capture_nvfbc_destroy_session_and_handle(cap_nvfbc);
    if(cap_nvfbc->library) {
        dlclose(cap_nvfbc->library);
        cap_nvfbc->library = NULL;
    }
    if(cap_nvfbc->params.display_to_capture) {
        free((void*)cap_nvfbc->params.display_to_capture);
        cap_nvfbc->params.display_to_capture = NULL;
    }
}

static int gsr_capture_nvfbc_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    if(!gsr_capture_nvfbc_load_library(cap))
        return -1;

    cap_nvfbc->x = max_int(cap_nvfbc->params.pos.x, 0);
    cap_nvfbc->y = max_int(cap_nvfbc->params.pos.y, 0);
    cap_nvfbc->width = max_int(cap_nvfbc->params.size.x, 0);
    cap_nvfbc->height = max_int(cap_nvfbc->params.size.y, 0);

    cap_nvfbc->capture_region = (cap_nvfbc->x > 0 || cap_nvfbc->y > 0 || cap_nvfbc->width > 0 || cap_nvfbc->height > 0);

    cap_nvfbc->supports_direct_cursor = false;
    int driver_major_version = 0;
    int driver_minor_version = 0;
    if(cap_nvfbc->params.direct_capture && get_driver_version(&driver_major_version, &driver_minor_version)) {
        fprintf(stderr, "Info: detected nvidia version: %d.%d\n", driver_major_version, driver_minor_version);

        // TODO:
        if(version_at_least(driver_major_version, driver_minor_version, 515, 57) && version_less_than(driver_major_version, driver_minor_version, 520, 56)) {
            cap_nvfbc->params.direct_capture = false;
            fprintf(stderr, "Warning: \"screen-direct\" has temporary been disabled as it causes stuttering with driver versions >= 515.57 and < 520.56. Please update your driver if possible. Capturing \"screen\" instead.\n");
        }

        // TODO:
        // Cursor capture disabled because moving the cursor doesn't update capture rate to monitor hz and instead captures at 10-30 hz
        /*
        if(direct_capture) {
            if(version_at_least(driver_major_version, driver_minor_version, 515, 57))
                cap_nvfbc->supports_direct_cursor = true;
            else
                fprintf(stderr, "Info: capturing \"screen-direct\" but driver version appears to be less than 515.57. Disabling capture of cursor. Please update your driver if you want to capture your cursor or record \"screen\" instead.\n");
        }
        */
    }

    if(gsr_capture_nvfbc_setup_handle(cap_nvfbc) != 0) {
        goto error_cleanup;
    }

    if(gsr_capture_nvfbc_setup_session(cap_nvfbc) != 0) {
        goto error_cleanup;
    }

    if(cap_nvfbc->capture_region) {
        video_codec_context->width = FFALIGN(cap_nvfbc->width, 2);
        video_codec_context->height = FFALIGN(cap_nvfbc->height, 2);
    } else {
        video_codec_context->width = FFALIGN(cap_nvfbc->tracking_width, 2);
        video_codec_context->height = FFALIGN(cap_nvfbc->tracking_height, 2);
    }

    frame->width = video_codec_context->width;
    frame->height = video_codec_context->height;

    /* Disable vsync */
    set_vertical_sync_enabled(cap_nvfbc->params.egl, 0);

    return 0;

    error_cleanup:
    gsr_capture_nvfbc_stop(cap_nvfbc);
    return -1;
}

static int gsr_capture_nvfbc_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion) {
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;

    const double nvfbc_recreate_retry_time_seconds = 1.0;
    if(cap_nvfbc->nvfbc_needs_recreate) {
        const double now = clock_get_monotonic_seconds();
        if(now - cap_nvfbc->nvfbc_dead_start >= nvfbc_recreate_retry_time_seconds) {
            cap_nvfbc->nvfbc_dead_start = now;
            gsr_capture_nvfbc_destroy_session_and_handle(cap_nvfbc);

            if(gsr_capture_nvfbc_setup_handle(cap_nvfbc) != 0) {
                fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed to recreate nvfbc handle, trying again in %f second(s)\n", nvfbc_recreate_retry_time_seconds);
                return -1;
            }
            
            if(gsr_capture_nvfbc_setup_session(cap_nvfbc) != 0) {
                fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed to recreate nvfbc session, trying again in %f second(s)\n", nvfbc_recreate_retry_time_seconds);
                return -1;
            }

            cap_nvfbc->nvfbc_needs_recreate = false;
        } else {
            return 0;
        }
    }

    NVFBC_FRAME_GRAB_INFO frame_info;
    memset(&frame_info, 0, sizeof(frame_info));

    NVFBC_TOGL_GRAB_FRAME_PARAMS grab_params;
    memset(&grab_params, 0, sizeof(grab_params));
    grab_params.dwVersion = NVFBC_TOGL_GRAB_FRAME_PARAMS_VER;
    grab_params.dwFlags = NVFBC_TOGL_GRAB_FLAGS_NOWAIT | NVFBC_TOGL_GRAB_FLAGS_FORCE_REFRESH; // TODO: Remove NVFBC_TOGL_GRAB_FLAGS_FORCE_REFRESH
    grab_params.pFrameGrabInfo = &frame_info;
    grab_params.dwTimeoutMs = 0;

    NVFBCSTATUS status = cap_nvfbc->nv_fbc_function_list.nvFBCToGLGrabFrame(cap_nvfbc->nv_fbc_handle, &grab_params);
    if(status != NVFBC_SUCCESS) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_capture failed: %s (%d), recreating session after %f second(s)\n", cap_nvfbc->nv_fbc_function_list.nvFBCGetLastErrorStr(cap_nvfbc->nv_fbc_handle), status, nvfbc_recreate_retry_time_seconds);
        cap_nvfbc->nvfbc_needs_recreate = true;
        cap_nvfbc->nvfbc_dead_start = clock_get_monotonic_seconds();
        return 0;
    }

    cap_nvfbc->params.egl->glFlush();
    cap_nvfbc->params.egl->glFinish();

    gsr_color_conversion_draw(color_conversion, cap_nvfbc->setup_params.dwTextures[grab_params.dwTextureIndex],
        (vec2i){0, 0}, (vec2i){frame->width, frame->height},
        (vec2i){0, 0}, (vec2i){frame->width, frame->height},
        0.0f, false);

    cap_nvfbc->params.egl->glFlush();
    cap_nvfbc->params.egl->glFinish();

    return 0;
}

static gsr_source_color gsr_capture_nvfbc_get_source_color(gsr_capture *cap) {
    (void)cap;
    return GSR_SOURCE_COLOR_BGR;
}

static void gsr_capture_nvfbc_destroy(gsr_capture *cap, AVCodecContext *video_codec_context) {
    (void)video_codec_context;
    gsr_capture_nvfbc *cap_nvfbc = cap->priv;
    gsr_capture_nvfbc_stop(cap_nvfbc);
    free(cap);
}

gsr_capture* gsr_capture_nvfbc_create(const gsr_capture_nvfbc_params *params) {
    if(!params) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params is NULL\n");
        return NULL;
    }

    if(!params->display_to_capture) {
        fprintf(stderr, "gsr error: gsr_capture_nvfbc_create params.display_to_capture is NULL\n");
        return NULL;
    }

    gsr_capture *cap = calloc(1, sizeof(gsr_capture));
    if(!cap)
        return NULL;

    gsr_capture_nvfbc *cap_nvfbc = calloc(1, sizeof(gsr_capture_nvfbc));
    if(!cap_nvfbc) {
        free(cap);
        return NULL;
    }

    const char *display_to_capture = strdup(params->display_to_capture);
    if(!display_to_capture) {
        free(cap);
        free(cap_nvfbc);
        return NULL;
    }

    cap_nvfbc->params = *params;
    cap_nvfbc->params.display_to_capture = display_to_capture;
    cap_nvfbc->params.fps = max_int(cap_nvfbc->params.fps, 1);
    
    *cap = (gsr_capture) {
        .start = gsr_capture_nvfbc_start,
        .tick = NULL,
        .should_stop = NULL,
        .capture = gsr_capture_nvfbc_capture,
        .get_source_color = gsr_capture_nvfbc_get_source_color,
        .uses_external_image = NULL,
        .destroy = gsr_capture_nvfbc_destroy,
        .priv = cap_nvfbc
    };

    return cap;
}
