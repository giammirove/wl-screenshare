#include "src/atomic_queue.hpp"
#include "src/frame-writer.hpp"

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct wf_buffer {
  struct gbm_bo *bo = nullptr;
  int bo_fd;
  zwp_linux_buffer_params_v1 *params = nullptr;
  struct wl_buffer *wl_buffer = nullptr;
  void *data = nullptr;
  size_t size = 0;
  enum wl_shm_format format;
  int drm_format;
  int width, height, stride;
  bool y_invert;

  timespec presented;
  uint64_t base_usec;
};
struct frame_data_t {
  zwlr_screencopy_frame_v1 *frame = NULL;
  wf_buffer *buffer;
  int bo_fd;
};

extern bool can_read_buffer;
extern bool use_dmabuf;
extern bool use_damage;
extern struct gbm_device *gbm_device;
extern struct zwp_linux_dmabuf_v1 *dmabuf;
extern atomic_queue<wf_buffer *> buffer_queue;

extern const struct zwlr_screencopy_frame_v1_listener frame_listener;
extern struct zwlr_screencopy_manager_v1 *screencopy_manager;

InputFormat get_input_format(wf_buffer &buffer);
