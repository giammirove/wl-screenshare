#include "src/zwp_linux_buffer.hpp"
#include "src/zwlr_screencopy.hpp"

#include <fcntl.h>
#include <gbm.h>
#include <iostream>
#include <stdlib.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

std::string drm_device_name;

static void
dmabuf_feedback_done(void *, struct zwp_linux_dmabuf_feedback_v1 *feedback) {
  zwp_linux_dmabuf_feedback_v1_destroy(feedback);
}

static void dmabuf_feedback_format_table(void *,
                                         struct zwp_linux_dmabuf_feedback_v1 *,
                                         int32_t fd, uint32_t) {
  close(fd);
}

static void dmabuf_feedback_main_device(void *,
                                        struct zwp_linux_dmabuf_feedback_v1 *,
                                        struct wl_array *device) {
  dev_t dev_id;
  memcpy(&dev_id, device->data, device->size);

  drmDevice *dev = NULL;
  if (drmGetDeviceFromDevId(dev_id, 0, &dev) != 0) {
    std::cerr << "Failed to get DRM device from dev id " << strerror(errno)
              << std::endl;
    return;
  }

  if (dev->available_nodes & (1 << DRM_NODE_RENDER)) {
    drm_device_name = dev->nodes[DRM_NODE_RENDER];
  } else if (dev->available_nodes & (1 << DRM_NODE_PRIMARY)) {
    drm_device_name = dev->nodes[DRM_NODE_PRIMARY];
  }

  drmFreeDevice(&dev);
}

static void
dmabuf_feedback_tranche_done(void *, struct zwp_linux_dmabuf_feedback_v1 *) {}

static void dmabuf_feedback_tranche_target_device(
    void *, struct zwp_linux_dmabuf_feedback_v1 *, struct wl_array *) {}

static void
dmabuf_feedback_tranche_formats(void *, struct zwp_linux_dmabuf_feedback_v1 *,
                                struct wl_array *) {}

static void dmabuf_feedback_tranche_flags(void *,
                                          struct zwp_linux_dmabuf_feedback_v1 *,
                                          uint32_t) {}

const struct zwp_linux_dmabuf_feedback_v1_listener dmabuf_feedback_listener = {
    .done = dmabuf_feedback_done,
    .format_table = dmabuf_feedback_format_table,
    .main_device = dmabuf_feedback_main_device,
    .tranche_done = dmabuf_feedback_tranche_done,
    .tranche_target_device = dmabuf_feedback_tranche_target_device,
    .tranche_formats = dmabuf_feedback_tranche_formats,
    .tranche_flags = dmabuf_feedback_tranche_flags,
};

static void dmabuf_created(void *data, struct zwp_linux_buffer_params_v1 *,
                           struct wl_buffer *wl_buffer) {

  frame_data_t *frame_data = (frame_data_t *)data;
  wf_buffer *buffer = frame_data->buffer;
  buffer->wl_buffer = wl_buffer;
  buffer->bo_fd = frame_data->bo_fd;

  zwlr_screencopy_frame_v1 *frame = frame_data->frame;

  free(frame_data);
  if (use_damage) {
    zwlr_screencopy_frame_v1_copy_with_damage(frame, buffer->wl_buffer);
  } else {
    zwlr_screencopy_frame_v1_copy(frame, buffer->wl_buffer);
  }
}

static void dmabuf_failed(void *, struct zwp_linux_buffer_params_v1 *) {}

const struct zwp_linux_buffer_params_v1_listener params_listener = {
    .created = dmabuf_created,
    .failed = dmabuf_failed,
};
