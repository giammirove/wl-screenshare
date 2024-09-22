#include "src/xdg_output.hpp"

std::list<wf_recorder_output> available_outputs;
struct zxdg_output_manager_v1 *xdg_output_manager = NULL;

static void handle_xdg_output_logical_position(void *,
                                               zxdg_output_v1 *zxdg_output,
                                               int32_t x, int32_t y) {
  for (auto &wo : available_outputs) {
    if (wo.zxdg_output == zxdg_output) {
      wo.x = x;
      wo.y = y;
    }
  }
}

static void handle_xdg_output_logical_size(void *, zxdg_output_v1 *zxdg_output,
                                           int32_t w, int32_t h) {
  for (auto &wo : available_outputs) {
    if (wo.zxdg_output == zxdg_output) {
      wo.width = w;
      wo.height = h;
    }
  }
}

static void handle_xdg_output_done(void *, zxdg_output_v1 *) {}

static void handle_xdg_output_name(void *, zxdg_output_v1 *zxdg_output_v1,
                                   const char *name) {
  for (auto &wo : available_outputs) {
    if (wo.zxdg_output == zxdg_output_v1)
      wo.name = name;
  }
}

static void handle_xdg_output_description(void *,
                                          zxdg_output_v1 *zxdg_output_v1,
                                          const char *description) {
  for (auto &wo : available_outputs) {
    if (wo.zxdg_output == zxdg_output_v1)
      wo.description = description;
  }
}

const zxdg_output_v1_listener xdg_output_implementation = {
    .logical_position = handle_xdg_output_logical_position,
    .logical_size = handle_xdg_output_logical_size,
    .done = handle_xdg_output_done,
    .name = handle_xdg_output_name,
    .description = handle_xdg_output_description};
