#include "src/zwlr_screencopy.hpp"
#include "src/zwp_linux_buffer.hpp"

#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

bool can_read_buffer = true;
bool use_dmabuf = false;
bool use_damage = true;
bool use_hwupload = false;
struct gbm_device *gbm_device = NULL;
struct zwp_linux_dmabuf_v1 *dmabuf = NULL;
atomic_queue<wf_buffer *> buffer_queue;
struct zwlr_screencopy_manager_v1 *screencopy_manager = NULL;

static wl_shm_format drm_to_wl_shm_format(uint32_t format) {
  if (format == GBM_FORMAT_ARGB8888) {
    return WL_SHM_FORMAT_ARGB8888;
  } else if (format == GBM_FORMAT_XRGB8888) {
    return WL_SHM_FORMAT_XRGB8888;
  } else {
    return (wl_shm_format)format;
  }
}

static uint32_t wl_shm_to_drm_format(uint32_t format) {
  if (format == WL_SHM_FORMAT_ARGB8888) {
    return GBM_FORMAT_ARGB8888;
  } else if (format == WL_SHM_FORMAT_XRGB8888) {
    return GBM_FORMAT_XRGB8888;
  } else {
    return format;
  }
}

static void free_shm_buffer(wf_buffer &buffer) {
  if (buffer.wl_buffer == NULL) {
    return;
  }

  munmap(buffer.data, buffer.size);
  wl_buffer_destroy(buffer.wl_buffer);
  buffer.wl_buffer = NULL;
}

InputFormat get_input_format(wf_buffer &buffer) {
  if (use_dmabuf && !use_hwupload) {
    return INPUT_FORMAT_DMABUF;
  }
  switch (buffer.format) {
  case WL_SHM_FORMAT_ARGB8888:
  case WL_SHM_FORMAT_XRGB8888:
    return INPUT_FORMAT_BGR0;
  case WL_SHM_FORMAT_XBGR8888:
  case WL_SHM_FORMAT_ABGR8888:
    return INPUT_FORMAT_RGB0;
  case WL_SHM_FORMAT_BGR888:
    return INPUT_FORMAT_BGR8;
  case WL_SHM_FORMAT_RGB565:
    return INPUT_FORMAT_RGB565;
  case WL_SHM_FORMAT_BGR565:
    return INPUT_FORMAT_BGR565;
  case WL_SHM_FORMAT_ARGB2101010:
  case WL_SHM_FORMAT_XRGB2101010:
    return INPUT_FORMAT_X2RGB10;
  case WL_SHM_FORMAT_ABGR2101010:
  case WL_SHM_FORMAT_XBGR2101010:
    return INPUT_FORMAT_X2BGR10;
  case WL_SHM_FORMAT_ABGR16161616:
  case WL_SHM_FORMAT_XBGR16161616:
    return INPUT_FORMAT_RGBX64;
  case WL_SHM_FORMAT_ARGB16161616:
  case WL_SHM_FORMAT_XRGB16161616:
    return INPUT_FORMAT_BGRX64;
  case WL_SHM_FORMAT_ABGR16161616F:
  case WL_SHM_FORMAT_XBGR16161616F:
    return INPUT_FORMAT_RGBX64F;
  default:
    fprintf(stderr, "Unsupported buffer format %d, exiting.", buffer.format);
    std::exit(0);
  }
}

static void frame_handle_buffer(void *data,
                                struct zwlr_screencopy_frame_v1 *frame,
                                uint32_t format, uint32_t width,
                                uint32_t height, uint32_t stride) {

  // DISABLED
  return;
}

static void frame_handle_flags(void *data, struct zwlr_screencopy_frame_v1 *,
                               uint32_t flags) {
  wf_buffer *buffer = (wf_buffer *)data;
  buffer->y_invert = flags & ZWLR_SCREENCOPY_FRAME_V1_FLAGS_Y_INVERT;
}

static void frame_handle_ready(void *data,
                               struct zwlr_screencopy_frame_v1 *frame,
                               uint32_t tv_sec_hi, uint32_t tv_sec_low,
                               uint32_t tv_nsec) {

  wf_buffer *buffer = (wf_buffer *)data;
  buffer->presented.tv_sec = ((1ll * tv_sec_hi) << 32ll) | tv_sec_low;
  buffer->presented.tv_nsec = tv_nsec;

  if (!frame_writer) {
    params.format = get_input_format(*buffer);
    params.drm_format = buffer->drm_format;
    params.width = buffer->width;
    params.height = buffer->height;
    params.stride = buffer->stride;
    frame_writer = std::unique_ptr<FrameWriter>(new FrameWriter(params));
  }

  buffer_queue.push(buffer);
  zwlr_screencopy_frame_v1_destroy(frame);
  can_read_buffer = true;
}

static void frame_handle_failed(void *, struct zwlr_screencopy_frame_v1 *) {}

static void frame_handle_damage(void *, struct zwlr_screencopy_frame_v1 *,
                                uint32_t, uint32_t, uint32_t, uint32_t) {}

static void frame_handle_linux_dmabuf(void *data,
                                      struct zwlr_screencopy_frame_v1 *frame,
                                      uint32_t format, uint32_t width,
                                      uint32_t height) {
  if (!use_dmabuf) {
    return;
  }

  wf_buffer *buffer = (wf_buffer *)data;

  buffer->format = drm_to_wl_shm_format(format);
  buffer->drm_format = format;
  buffer->width = width;
  buffer->height = height;

  const uint64_t modifier = 0; // DRM_FORMAT_MOD_LINEAR
  buffer->bo = gbm_bo_create_with_modifiers(
      gbm_device, buffer->width, buffer->height, format, &modifier, 1);
  if (buffer->bo == NULL) {
    buffer->bo =
        gbm_bo_create(gbm_device, buffer->width, buffer->height, format,
                      GBM_BO_USE_LINEAR | GBM_BO_USE_RENDERING);
  }
  if (buffer->bo == NULL) {
    std::cerr << "Failed to create gbm bo" << std::endl;
    /*exit_main_loop = true;*/
    return;
  }

  buffer->stride = gbm_bo_get_stride(buffer->bo);

  buffer->params = zwp_linux_dmabuf_v1_create_params(dmabuf);

  uint64_t mod = gbm_bo_get_modifier(buffer->bo);
  int fd = gbm_bo_get_fd(buffer->bo);
  zwp_linux_buffer_params_v1_add(
      buffer->params, fd, 0, gbm_bo_get_offset(buffer->bo, 0),
      gbm_bo_get_stride(buffer->bo), mod >> 32, mod & 0xffffffff);

  // params_listener will free fdata
  frame_data_t *fdata = new frame_data_t;
  fdata->frame = frame;
  fdata->buffer = buffer;
  fdata->bo_fd = fd;

  zwp_linux_buffer_params_v1_add_listener(buffer->params, &params_listener,
                                          fdata);
  zwp_linux_buffer_params_v1_create(buffer->params, buffer->width,
                                    buffer->height, format, 0);
}

static void frame_handle_buffer_done(void *,
                                     struct zwlr_screencopy_frame_v1 *) {}

const struct zwlr_screencopy_frame_v1_listener frame_listener = {
    .buffer = frame_handle_buffer,
    .flags = frame_handle_flags,
    .ready = frame_handle_ready,
    .failed = frame_handle_failed,
    .damage = frame_handle_damage,
    .linux_dmabuf = frame_handle_linux_dmabuf,
    .buffer_done = frame_handle_buffer_done,
};
