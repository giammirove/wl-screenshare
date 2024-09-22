#ifndef GSR_EGL_H
#define GSR_EGL_H

/* OpenGL EGL library with a hidden window context (to allow using the opengl functions) */

#include <X11/X.h>
#include <X11/Xutil.h>
#include <stdbool.h>
#include <stdint.h>
#include "vec2.h"
#include "defs.h"

#ifdef _WIN64
typedef signed   long long int khronos_intptr_t;
typedef unsigned long long int khronos_uintptr_t;
typedef signed   long long int khronos_ssize_t;
typedef unsigned long long int khronos_usize_t;
#else
typedef signed   long  int     khronos_intptr_t;
typedef unsigned long  int     khronos_uintptr_t;
typedef signed   long  int     khronos_ssize_t;
typedef unsigned long  int     khronos_usize_t;
#endif

typedef void* EGLDisplay;
typedef void* EGLNativeDisplayType;
typedef uintptr_t EGLNativeWindowType;
typedef uintptr_t EGLNativePixmapType;
typedef void* EGLConfig;
typedef void* EGLSurface;
typedef void* EGLContext;
typedef void* EGLClientBuffer;
typedef void* EGLImage;
typedef void* EGLImageKHR;
typedef void *GLeglImageOES;
typedef void (*__eglMustCastToProperFunctionPointerType)(void);
typedef struct __GLXFBConfigRec *GLXFBConfig;
typedef struct __GLXcontextRec *GLXContext;
typedef XID GLXDrawable;
typedef void(*__GLXextFuncPtr)(void);

#define EGL_SUCCESS                             0x3000
#define EGL_BUFFER_SIZE                         0x3020
#define EGL_RENDERABLE_TYPE                     0x3040
#define EGL_OPENGL_API                          0x30A2
#define EGL_OPENGL_ES_API                       0x30A0
#define EGL_OPENGL_BIT                          0x0008
#define EGL_OPENGL_ES_BIT                       0x0001
#define EGL_NONE                                0x3038
#define EGL_CONTEXT_CLIENT_VERSION              0x3098
#define EGL_BACK_BUFFER                         0x3084
#define EGL_GL_TEXTURE_2D                       0x30B1
#define EGL_TRUE                                1
#define EGL_IMAGE_PRESERVED_KHR                 0x30D2
#define EGL_NATIVE_PIXMAP_KHR                   0x30B0
#define EGL_LINUX_DRM_FOURCC_EXT                0x3271
#define EGL_WIDTH                               0x3057
#define EGL_HEIGHT                              0x3056
#define EGL_DMA_BUF_PLANE0_FD_EXT               0x3272
#define EGL_DMA_BUF_PLANE0_OFFSET_EXT           0x3273
#define EGL_DMA_BUF_PLANE0_PITCH_EXT            0x3274
#define EGL_DMA_BUF_PLANE1_FD_EXT               0x3275
#define EGL_DMA_BUF_PLANE1_OFFSET_EXT           0x3276
#define EGL_DMA_BUF_PLANE1_PITCH_EXT            0x3277
#define EGL_DMA_BUF_PLANE2_FD_EXT               0x3278
#define EGL_DMA_BUF_PLANE2_OFFSET_EXT           0x3279
#define EGL_DMA_BUF_PLANE2_PITCH_EXT            0x327A
#define EGL_DMA_BUF_PLANE3_FD_EXT               0x3440
#define EGL_DMA_BUF_PLANE3_OFFSET_EXT           0x3441
#define EGL_DMA_BUF_PLANE3_PITCH_EXT            0x3442
#define EGL_DMA_BUF_PLANE0_MODIFIER_LO_EXT      0x3443
#define EGL_DMA_BUF_PLANE0_MODIFIER_HI_EXT      0x3444
#define EGL_DMA_BUF_PLANE1_MODIFIER_LO_EXT      0x3445
#define EGL_DMA_BUF_PLANE1_MODIFIER_HI_EXT      0x3446
#define EGL_DMA_BUF_PLANE2_MODIFIER_LO_EXT      0x3447
#define EGL_DMA_BUF_PLANE2_MODIFIER_HI_EXT      0x3448
#define EGL_DMA_BUF_PLANE3_MODIFIER_LO_EXT      0x3449
#define EGL_DMA_BUF_PLANE3_MODIFIER_HI_EXT      0x344A
#define EGL_LINUX_DMA_BUF_EXT                   0x3270
#define EGL_RED_SIZE                            0x3024
#define EGL_ALPHA_SIZE                          0x3021
#define EGL_BLUE_SIZE                           0x3022
#define EGL_GREEN_SIZE                          0x3023
#define EGL_CONTEXT_PRIORITY_LEVEL_IMG          0x3100
#define EGL_CONTEXT_PRIORITY_HIGH_IMG           0x3101
#define EGL_CONTEXT_PRIORITY_MEDIUM_IMG         0x3102
#define EGL_CONTEXT_PRIORITY_LOW_IMG            0x3103
#define EGL_DEVICE_EXT                          0x322C
#define EGL_DRM_DEVICE_FILE_EXT                 0x3233

#define GL_FLOAT                                0x1406
#define GL_FALSE                                0
#define GL_TRUE                                 1
#define GL_TRIANGLES                            0x0004
#define GL_TEXTURE_2D                           0x0DE1
#define GL_TEXTURE_EXTERNAL_OES                 0x8D65
#define GL_RED                                  0x1903
#define GL_GREEN                                0x1904
#define GL_BLUE                                    0x1905
#define GL_ALPHA                                0x1906
#define GL_TEXTURE_SWIZZLE_RGBA                 0x8E46
#define GL_RG                                   0x8227
#define GL_RGB                                  0x1907
#define GL_RGBA                                 0x1908
#define GL_RGBA8                                0x8058
#define GL_R8                                   0x8229
#define GL_RG8                                  0x822B
#define GL_R16                                  0x822A
#define GL_RG16                                 0x822C
#define GL_UNSIGNED_BYTE                        0x1401
#define GL_COLOR_BUFFER_BIT                     0x00004000
#define GL_TEXTURE_WRAP_S                       0x2802
#define GL_TEXTURE_WRAP_T                       0x2803
#define GL_TEXTURE_MAG_FILTER                   0x2800
#define GL_TEXTURE_MIN_FILTER                   0x2801
#define GL_TEXTURE_WIDTH                        0x1000
#define GL_TEXTURE_HEIGHT                       0x1001
#define GL_NEAREST                              0x2600
#define GL_CLAMP_TO_EDGE                        0x812F
#define GL_LINEAR                               0x2601
#define GL_FRAMEBUFFER                          0x8D40
#define GL_COLOR_ATTACHMENT0                    0x8CE0
#define GL_FRAMEBUFFER_COMPLETE                 0x8CD5
#define GL_DYNAMIC_DRAW                         0x88E8
#define GL_ARRAY_BUFFER                         0x8892
#define GL_BLEND                                0x0BE2
#define GL_SRC_ALPHA                            0x0302
#define GL_ONE_MINUS_SRC_ALPHA                  0x0303
#define GL_DEBUG_OUTPUT                         0x92E0
#define GL_SCISSOR_TEST                         0x0C11

#define GL_VENDOR                               0x1F00
#define GL_RENDERER                             0x1F01

#define GL_COMPILE_STATUS                       0x8B81
#define GL_INFO_LOG_LENGTH                      0x8B84
#define GL_FRAGMENT_SHADER                      0x8B30
#define GL_VERTEX_SHADER                        0x8B31
#define GL_COMPILE_STATUS                       0x8B81
#define GL_LINK_STATUS                          0x8B82

typedef unsigned int (*FUNC_eglExportDMABUFImageQueryMESA)(EGLDisplay dpy, EGLImageKHR image, int *fourcc, int *num_planes, uint64_t *modifiers);
typedef unsigned int (*FUNC_eglExportDMABUFImageMESA)(EGLDisplay dpy, EGLImageKHR image, int *fds, int32_t *strides, int32_t *offsets);
typedef void (*FUNC_glEGLImageTargetTexture2DOES)(unsigned int target, GLeglImageOES image);
typedef GLXContext (*FUNC_glXCreateContextAttribsARB)(Display *dpy, GLXFBConfig config, GLXContext share_context, Bool direct, const int *attrib_list);
typedef void (*FUNC_glXSwapIntervalEXT)(Display * dpy, GLXDrawable drawable, int interval);
typedef int (*FUNC_glXSwapIntervalMESA)(unsigned int interval);
typedef int (*FUNC_glXSwapIntervalSGI)(int interval);
typedef void (*GLDEBUGPROC)(unsigned int source, unsigned int type, unsigned int id, unsigned int severity, int length, const char *message, const void *userParam);
typedef int (*FUNC_eglQueryDisplayAttribEXT)(EGLDisplay dpy, int32_t attribute, intptr_t *value);
typedef const char* (*FUNC_eglQueryDeviceStringEXT)(void *device, int32_t name);
typedef int (*FUNC_eglQueryDmaBufModifiersEXT)(EGLDisplay dpy, int32_t format, int32_t max_modifiers, uint64_t *modifiers, int *external_only, int32_t *num_modifiers);

#define GSR_MAX_OUTPUTS 32

typedef struct {
    char *name;
    vec2i pos;
    vec2i size;
    uint32_t connector_id;
    gsr_monitor_rotation rotation;
    uint32_t monitor_identifier; /* crtc id */
} gsr_x11_output;

typedef struct {
    Display *dpy;
    Window window;
    gsr_x11_output outputs[GSR_MAX_OUTPUTS];
    int num_outputs;
    XEvent xev;
} gsr_x11;

typedef struct {
    uint32_t wl_name;
    void *output;
    vec2i pos;
    vec2i size;
    int32_t transform;
    char *name;
} gsr_wayland_output;

typedef struct {
    void *dpy;
    void *window;
    void *registry;
    void *surface;
    void *compositor;
    gsr_wayland_output outputs[GSR_MAX_OUTPUTS];
    int num_outputs;
} gsr_wayland;

typedef enum {
    GSR_GL_CONTEXT_TYPE_EGL,
    GSR_GL_CONTEXT_TYPE_GLX
} gsr_gl_context_type;

typedef enum {
    GSR_DISPLAY_SERVER_X11,
    GSR_DISPLAY_SERVER_WAYLAND
} gsr_display_server;

typedef struct gsr_egl gsr_egl;
struct gsr_egl {
    void *egl_library;
    void *glx_library;
    void *gl_library;

    gsr_gl_context_type context_type;

    EGLDisplay egl_display;
    EGLSurface egl_surface;
    EGLContext egl_context;
    const char *dri_card_path;

    void *glx_context;
    void *glx_fb_config;

    gsr_gpu_info gpu_info;

    gsr_x11 x11;
    gsr_wayland wayland;
    char card_path[128];

    int32_t (*eglGetError)(void);
    EGLDisplay (*eglGetDisplay)(EGLNativeDisplayType display_id);
    unsigned int (*eglInitialize)(EGLDisplay dpy, int32_t *major, int32_t *minor);
    unsigned int (*eglTerminate)(EGLDisplay dpy);
    unsigned int (*eglChooseConfig)(EGLDisplay dpy, const int32_t *attrib_list, EGLConfig *configs, int32_t config_size, int32_t *num_config);
    EGLSurface (*eglCreateWindowSurface)(EGLDisplay dpy, EGLConfig config, EGLNativeWindowType win, const int32_t *attrib_list);
    EGLContext (*eglCreateContext)(EGLDisplay dpy, EGLConfig config, EGLContext share_context, const int32_t *attrib_list);
    unsigned int (*eglMakeCurrent)(EGLDisplay dpy, EGLSurface draw, EGLSurface read, EGLContext ctx);
    EGLImage (*eglCreateImage)(EGLDisplay dpy, EGLContext ctx, unsigned int target, EGLClientBuffer buffer, const intptr_t *attrib_list);
    unsigned int (*eglDestroyContext)(EGLDisplay dpy, EGLContext ctx);
    unsigned int (*eglDestroySurface)(EGLDisplay dpy, EGLSurface surface);
    unsigned int (*eglDestroyImage)(EGLDisplay dpy, EGLImage image);
    unsigned int (*eglSwapInterval)(EGLDisplay dpy, int32_t interval);
    unsigned int (*eglSwapBuffers)(EGLDisplay dpy, EGLSurface surface);
    unsigned int (*eglBindAPI)(unsigned int api);
    __eglMustCastToProperFunctionPointerType (*eglGetProcAddress)(const char *procname);

    FUNC_eglExportDMABUFImageQueryMESA eglExportDMABUFImageQueryMESA;
    FUNC_eglExportDMABUFImageMESA eglExportDMABUFImageMESA;
    FUNC_glEGLImageTargetTexture2DOES glEGLImageTargetTexture2DOES;
    FUNC_eglQueryDisplayAttribEXT eglQueryDisplayAttribEXT;
    FUNC_eglQueryDeviceStringEXT eglQueryDeviceStringEXT;
    FUNC_eglQueryDmaBufModifiersEXT eglQueryDmaBufModifiersEXT;

    __GLXextFuncPtr (*glXGetProcAddress)(const unsigned char *procName);
    GLXFBConfig* (*glXChooseFBConfig)(Display *dpy, int screen, const int *attribList, int *nitems);
    Bool (*glXMakeContextCurrent)(Display *dpy, GLXDrawable draw, GLXDrawable read, GLXContext ctx);
    // TODO: Remove
    GLXContext (*glXCreateNewContext)(Display *dpy, GLXFBConfig config, int renderType, GLXContext shareList, Bool direct);
    void (*glXDestroyContext)(Display *dpy, GLXContext ctx);
    void (*glXSwapBuffers)(Display *dpy, GLXDrawable drawable);
    FUNC_glXCreateContextAttribsARB glXCreateContextAttribsARB;

    /* Optional */
    FUNC_glXSwapIntervalEXT glXSwapIntervalEXT;
    FUNC_glXSwapIntervalMESA glXSwapIntervalMESA;
    FUNC_glXSwapIntervalSGI glXSwapIntervalSGI;

    unsigned int (*glGetError)(void);
    const unsigned char* (*glGetString)(unsigned int name);
    void (*glFlush)(void);
    void (*glFinish)(void);
    void (*glClear)(unsigned int mask);
    void (*glClearColor)(float red, float green, float blue, float alpha);
    void (*glGenTextures)(int n, unsigned int *textures);
    void (*glDeleteTextures)(int n, const unsigned int *texture);
    void (*glBindTexture)(unsigned int target, unsigned int texture);
    void (*glTexParameteri)(unsigned int target, unsigned int pname, int param);
    void (*glTexParameteriv)(unsigned int target, unsigned int pname, const int *params);
    void (*glGetTexLevelParameteriv)(unsigned int target, int level, unsigned int pname, int *params);
    void (*glTexImage2D)(unsigned int target, int level, int internalFormat, int width, int height, int border, unsigned int format, unsigned int type, const void *pixels);
    void (*glGetTexImage)(unsigned int target, int level, unsigned int format, unsigned int type, void *pixels);
    void (*glGenFramebuffers)(int n, unsigned int *framebuffers);
    void (*glBindFramebuffer)(unsigned int target, unsigned int framebuffer);
    void (*glDeleteFramebuffers)(int n, const unsigned int *framebuffers);
    void (*glViewport)(int x, int y, int width, int height);
    void (*glFramebufferTexture2D)(unsigned int target, unsigned int attachment, unsigned int textarget, unsigned int texture, int level);
    void (*glDrawBuffers)(int n, const unsigned int *bufs);
    unsigned int (*glCheckFramebufferStatus)(unsigned int target);
    void (*glBindBuffer)(unsigned int target, unsigned int buffer);
    void (*glGenBuffers)(int n, unsigned int *buffers);
    void (*glBufferData)(unsigned int target, khronos_ssize_t size, const void *data, unsigned int usage);
    void (*glBufferSubData)(unsigned int target, khronos_intptr_t offset, khronos_ssize_t size, const void *data);
    void (*glDeleteBuffers)(int n, const unsigned int *buffers);
    void (*glGenVertexArrays)(int n, unsigned int *arrays);
    void (*glBindVertexArray)(unsigned int array);
    void (*glDeleteVertexArrays)(int n, const unsigned int *arrays);
    unsigned int (*glCreateProgram)(void);
    unsigned int (*glCreateShader)(unsigned int type);
    void (*glAttachShader)(unsigned int program, unsigned int shader);
    void (*glBindAttribLocation)(unsigned int program, unsigned int index, const char *name);
    void (*glCompileShader)(unsigned int shader);
    void (*glLinkProgram)(unsigned int program);
    void (*glShaderSource)(unsigned int shader, int count, const char *const*string, const int *length);
    void (*glUseProgram)(unsigned int program);
    void (*glGetProgramInfoLog)(unsigned int program, int bufSize, int *length, char *infoLog);
    void (*glGetShaderiv)(unsigned int shader, unsigned int pname, int *params);
    void (*glGetShaderInfoLog)(unsigned int shader, int bufSize, int *length, char *infoLog);
    void (*glDeleteProgram)(unsigned int program);
    void (*glDeleteShader)(unsigned int shader);
    void (*glGetProgramiv)(unsigned int program, unsigned int pname, int *params);
    void (*glVertexAttribPointer)(unsigned int index, int size, unsigned int type, unsigned char normalized, int stride, const void *pointer);
    void (*glEnableVertexAttribArray)(unsigned int index);
    void (*glDrawArrays)(unsigned int mode, int first, int count);
    void (*glEnable)(unsigned int cap);
    void (*glDisable)(unsigned int cap);
    void (*glBlendFunc)(unsigned int sfactor, unsigned int dfactor);
    int (*glGetUniformLocation)(unsigned int program, const char *name);
    void (*glUniform1f)(int location, float v0);
    void (*glUniform2f)(int location, float v0, float v1);
    void (*glDebugMessageCallback)(GLDEBUGPROC callback, const void *userParam);
    void (*glScissor)(int x, int y, int width, int height);
};

bool gsr_egl_load(gsr_egl *self, Display *dpy, bool wayland, bool is_monitor_capture);
void gsr_egl_unload(gsr_egl *self);

/* Returns true if an event is available */
bool gsr_egl_process_event(gsr_egl *self);
/* Does opengl swap with egl or glx, depending on which one is active */
void gsr_egl_swap_buffers(gsr_egl *self);

gsr_display_server gsr_egl_get_display_server(const gsr_egl *self);
XEvent* gsr_egl_get_event_data(gsr_egl *self);

#endif /* GSR_EGL_H */
