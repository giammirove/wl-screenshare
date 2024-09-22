#include <list>
#include <string>

#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

struct wf_recorder_output {
  wl_output *output;
  zxdg_output_v1 *zxdg_output;
  std::string name, description;
  int32_t x, y, width, height;
};

extern std::list<wf_recorder_output> available_outputs;
extern const zxdg_output_v1_listener xdg_output_implementation;
extern struct zxdg_output_manager_v1 *xdg_output_manager;
