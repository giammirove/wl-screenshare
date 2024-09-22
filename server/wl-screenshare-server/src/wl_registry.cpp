#include "src/wl_registry.hpp"
#include "src/xdg_output.hpp"
#include "src/zwlr_screencopy.hpp"
#include "src/zwp_linux_buffer.hpp"

#include <string.h>

static void handle_global(void *, struct wl_registry *registry, uint32_t name,
                          const char *interface, uint32_t) {

  if (strcmp(interface, wl_output_interface.name) == 0) {
    auto output =
        (wl_output *)wl_registry_bind(registry, name, &wl_output_interface, 1);
    wf_recorder_output wro;
    wro.output = output;
    available_outputs.push_back(wro);
  } else if (strcmp(interface, zwlr_screencopy_manager_v1_interface.name) ==
             0) {
    screencopy_manager = (zwlr_screencopy_manager_v1 *)wl_registry_bind(
        registry, name, &zwlr_screencopy_manager_v1_interface, 3);
  } else if (strcmp(interface, zxdg_output_manager_v1_interface.name) == 0) {
    xdg_output_manager = (zxdg_output_manager_v1 *)wl_registry_bind(
        registry, name, &zxdg_output_manager_v1_interface,
        2); // version 2 for name & description, if available
  } else if (strcmp(interface, zwp_linux_dmabuf_v1_interface.name) == 0) {
    dmabuf = (zwp_linux_dmabuf_v1 *)wl_registry_bind(
        registry, name, &zwp_linux_dmabuf_v1_interface, 4);
    if (dmabuf) {
      struct zwp_linux_dmabuf_feedback_v1 *feedback =
          zwp_linux_dmabuf_v1_get_default_feedback(dmabuf);
      zwp_linux_dmabuf_feedback_v1_add_listener(
          feedback, &dmabuf_feedback_listener, NULL);
    }
  }
}

static void handle_global_remove(void *, struct wl_registry *, uint32_t) {}

const struct wl_registry_listener registry_listener = {
    .global = handle_global,
    .global_remove = handle_global_remove,
};
