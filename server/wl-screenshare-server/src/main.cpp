#define _XOPEN_SOURCE 700
#define _POSIX_C_SOURCE 199309L
#include <iostream>
#include <optional>

#include <assert.h>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <getopt.h>
#include <list>
#include <string>
#include <sys/time.h>
#include <thread>

#include <fcntl.h>
#include <gbm.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <wayland-client-protocol.h>
#include <xf86drm.h>

#include "frame-writer.hpp"
#include "linux-dmabuf-unstable-v1-client-protocol.h"
#include "wlr-screencopy-unstable-v1-client-protocol.h"
#include "xdg-output-unstable-v1-client-protocol.h"

#include "src/wl_registry.hpp"
#include "src/xdg_output.hpp"
#include "src/zwlr_screencopy.hpp"
#include "src/zwp_linux_buffer.hpp"

#include "config.h"

#define MAX_FRAME_FAILURES 16

static const int GRACEFUL_TERMINATION_SIGNALS[] = {SIGTERM, SIGINT, SIGHUP};

std::optional<uint64_t> first_frame_ts;

static int drm_fd = -1;

void request_next_frame();

static bool use_hwupload = false;

static void wf_buffer_destroy(wf_buffer *buffer) {
  zwp_linux_buffer_params_v1_destroy(buffer->params);
  gbm_bo_destroy(buffer->bo);
  munmap(buffer->data, buffer->size);
  wl_buffer_destroy(buffer->wl_buffer);
  buffer->wl_buffer = NULL;
  close(buffer->bo_fd);
  buffer->bo_fd = -1;
  free(buffer);
}

static void write_loop() {
  /* Ignore SIGTERM/SIGINT/SIGHUP, main loop is responsible for the
   * exit_main_loop signal */
  sigset_t sigset;
  sigemptyset(&sigset);
  for (auto signo : GRACEFUL_TERMINATION_SIGNALS) {
    sigaddset(&sigset, signo);
  }
  pthread_sigmask(SIG_BLOCK, &sigset, NULL);

  std::optional<uint64_t> first_frame_ts;

  while (!exit_main_loop) {
    if (exit_main_loop) {
      break;
    }

    if (buffer_queue.empty())
      continue;

    wf_buffer *buffer = buffer_queue.pop();

    uint64_t sync_timestamp = 0;
    if (first_frame_ts.has_value()) {
      sync_timestamp = buffer->base_usec - first_frame_ts.value();
    } else {
      sync_timestamp = 0;
      first_frame_ts = buffer->base_usec;
    }

    bool do_cont = frame_writer->add_frame(buffer->bo, buffer->bo_fd,
                                           sync_timestamp, buffer->y_invert);
    wf_buffer_destroy(buffer);

    if (!do_cont) {
      break;
    }
  }

  frame_writer = nullptr;
}

void handle_graceful_termination(int) { exit_main_loop = true; }

static bool user_specified_overwrite(std::string filename) {
  struct stat buffer;
  if (stat(filename.c_str(), &buffer) == 0 && !S_ISCHR(buffer.st_mode)) {
    std::string input;
    std::cerr << "Output file \"" << filename << "\" exists. Overwrite? Y/n: ";
    std::getline(std::cin, input);
    if (input.size() && input[0] != 'Y' && input[0] != 'y') {
      std::cerr << "Use -f to specify the file name." << std::endl;
      return false;
    }
  }

  return true;
}

static void check_has_protos() {
  if (screencopy_manager == NULL) {
    fprintf(stderr, "compositor doesn't support wlr-screencopy-unstable-v1\n");
    exit(EXIT_FAILURE);
  }

  if (xdg_output_manager == NULL) {
    fprintf(stderr, "compositor doesn't support xdg-output-unstable-v1\n");
    exit(EXIT_FAILURE);
  }

  if (use_dmabuf && dmabuf == NULL) {
    fprintf(stderr, "compositor doesn't support linux-dmabuf-unstable-v1\n");
    exit(EXIT_FAILURE);
  }

  if (available_outputs.empty()) {
    fprintf(stderr, "no outputs available\n");
    exit(EXIT_FAILURE);
  }
}

wl_display *display = NULL;
static void sync_wayland() {
  wl_display_dispatch(display);
  wl_display_roundtrip(display);
}

static void load_output_info() {
  for (auto &wo : available_outputs) {
    wo.zxdg_output =
        zxdg_output_manager_v1_get_xdg_output(xdg_output_manager, wo.output);
    zxdg_output_v1_add_listener(wo.zxdg_output, &xdg_output_implementation,
                                NULL);
  }

  sync_wayland();
}

static wf_recorder_output *choose_interactive() {
  fprintf(
      stdout,
      "Please select an output from the list to capture (enter output no.):\n");

  int i = 1;
  for (auto &wo : available_outputs) {
    printf("%d. Name: %s Description: %s\n", i++, wo.name.c_str(),
           wo.description.c_str());
  }

  printf("Enter output no.:");
  fflush(stdout);

  int choice = 1;
  if (scanf("%d", &choice) != 1 || choice > (int)available_outputs.size() ||
      choice <= 0)
    return nullptr;

  auto it = available_outputs.begin();
  std::advance(it, choice - 1);
  return &*it;
}

struct capture_region {
  int32_t x, y;
  int32_t width, height;

  capture_region() : capture_region(0, 0, 0, 0) {}

  capture_region(int32_t _x, int32_t _y, int32_t _width, int32_t _height)
      : x(_x), y(_y), width(_width), height(_height) {}

  void set_from_string(std::string geometry_string) {
    if (sscanf(geometry_string.c_str(), "%d,%d %dx%d", &x, &y, &width,
               &height) != 4) {
      fprintf(stderr, "Bad geometry: %s, capturing whole output instead.\n",
              geometry_string.c_str());
      x = y = width = height = 0;
      return;
    }
  }

  bool is_selected() { return width > 0 && height > 0; }

  bool contained_in(const capture_region &output) const {
    return output.x <= x && output.x + output.width >= x + width &&
           output.y <= y && output.y + output.height >= y + height;
  }
};

static wf_recorder_output *
detect_output_from_region(const capture_region &region) {
  for (auto &wo : available_outputs) {
    const capture_region output_region{wo.x, wo.y, wo.width, wo.height};
    if (region.contained_in(output_region)) {
      std::cerr << "Detected output based on geometry: " << wo.name
                << std::endl;
      return &wo;
    }
  }

  std::cerr << "Failed to detect output based on geometry (is your geometry "
               "overlapping outputs?)"
            << std::endl;
  return nullptr;
}

static void help() {
  printf(R"(Usage: wf-recorder [OPTION]... -f [FILE]...
Screen recording of wlroots-based compositors

With no FILE, start recording the current screen.

Use Ctrl+C to stop.)");
  printf(R"(

  -c, --codec               Specifies the codec of the video. These can be found by using:
                            ffmpeg -encoders
                            To modify codec parameters, use -p <option_name>=<option_value>
  
  -r, --framerate           Changes framerate to constant framerate with a given value.
  
  -d, --device              Selects the device to use when encoding the video
                            Some drivers report support for rgb0 data for vaapi input but
                            really only support yuv.

  --no-dmabuf               By default, wf-recorder will try to use only GPU buffers and copies if
                            using a GPU encoder. However, this can cause issues on some systems. In such
                            cases, this option will disable the GPU copy and force a CPU one.

  -D, --no-damage           By default, wf-recorder will request a new frame from the compositor
                            only when the screen updates. This results in a much smaller output
                            file, which however has a variable refresh rate. When this option is
                            on, wf-recorder does not use this optimization and continuously
                            records new frames, even if there are no updates on the screen.

  -f <filename>.ext         By using the -f option the output file will have the name :
                            filename.ext and the file format will be determined by provided
                            while extension .ext . If the extension .ext provided is not
                            recognized by your FFmpeg muxers, the command will fail.
                            You can check the muxers that your FFmpeg installation supports by
                            running: ffmpeg -muxers

  -m, --muxer               Set the output format to a specific muxer instead of detecting it
                            from the filename.

  -x, --pixel-format        Set the output pixel format. These can be found by running:
                            ffmpeg -pix_fmts

  -g, --geometry            Selects a specific part of the screen. The format is "x,y WxH".

  -h, --help                Prints this help screen.

  -v, --version             Prints the version of wf-recorder.

  -l, --log                 Generates a log on the current terminal. Debug purposes.

  -o, --output              Specify the output where the video is to be recorded.

  -p, --codec-param         Change the codec parameters.
                            -p <option_name>=<option_value>

  -F, --filter              Specify the ffmpeg filter string to use. For example,
                            -F scale_vaapi=format=nv12 is used for VAAPI.

  -b, --bframes             This option is used to set the maximum number of b-frames to be used.
                            If b-frames are not supported by your hardware, set this to 0.
    
  -B. --buffrate            This option is used to specify the buffers expected framerate. this 
                            may help when encoders are expecting specific or limited framerate.

  --audio-backend           Specifies the audio backend among the available backends, for ex.
                            --audio-backend=pipewire
  
  -C, --audio-codec         Specifies the codec of the audio. These can be found by running:
                            ffmpeg -encoders
                            To modify codec parameters, use -P <option_name>=<option_value>

  -X, --sample-format       Set the output audio sample format. These can be found by running: 
                            ffmpeg -sample_fmts
  
  -R, --sample-rate         Changes the audio sample rate in HZ. The default value is 48000.
  
  -P, --audio-codec-param   Change the audio codec parameters.
                            -P <option_name>=<option_value>
  
  -y, --overwrite           Force overwriting the output file without prompting.

Examples:)");
#ifdef HAVE_AUDIO
  printf(R"(

  Video Only:)");
#endif
  printf(R"(

  - wf-recorder                         Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -f <filename>.ext       Records the video. Use Ctrl+C to stop recording.
                                        The video file will be stored as <filename>.ext in the
                                        current working directory.)");
#ifdef HAVE_AUDIO
  printf(R"(

  Video and Audio:

  - wf-recorder -a                      Records the video and audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as recording.mp4 in the
                                        current working directory.

  - wf-recorder -a -f <filename>.ext    Records the video and audio. Use Ctrl+C to stop recording.
                                        The video file will be stored as <filename>.ext in the
                                        current working directory.)");
#endif
  printf(R"(

)"
         "\n");
  exit(EXIT_SUCCESS);
}

capture_region selected_region{};
wf_recorder_output *chosen_output = nullptr;

void request_next_frame() {
  zwlr_screencopy_frame_v1 *frame = NULL;

  /* Capture the whole output if the user hasn't provided a good geometry */
  if (!selected_region.is_selected()) {
    frame = zwlr_screencopy_manager_v1_capture_output(screencopy_manager, 1,
                                                      chosen_output->output);
  } else {
    frame = zwlr_screencopy_manager_v1_capture_output_region(
        screencopy_manager, 1, chosen_output->output,
        selected_region.x - chosen_output->x,
        selected_region.y - chosen_output->y, selected_region.width,
        selected_region.height);
  }

  wf_buffer *buffer = new wf_buffer;
  zwlr_screencopy_frame_v1_add_listener(frame, &frame_listener, buffer);
}

static void parse_codec_opts(std::map<std::string, std::string> &options,
                             const std::string param) {
  size_t pos;
  pos = param.find("=");
  if (pos != std::string::npos && pos != param.length() - 1) {
    auto optname = param.substr(0, pos);
    auto optvalue = param.substr(pos + 1, param.length() - pos - 1);
    options.insert(std::pair<std::string, std::string>(optname, optvalue));
  } else {
    std::cerr << "Invalid codec option " + param << std::endl;
  }
}

int main(int argc, char *argv[]) {
  params.file = "recording." + std::string(DEFAULT_CONTAINER_FORMAT);
  params.codec = DEFAULT_CODEC;
  params.pix_fmt = DEFAULT_PIX_FMT;
  params.audio_codec = DEFAULT_AUDIO_CODEC;
  params.enable_ffmpeg_debug_output = false;
  params.enable_audio = false;
  params.bframes = -1;

  constexpr const char *default_cmdline_output = "interactive";
  std::string cmdline_output = default_cmdline_output;
  bool force_no_dmabuf = false;
  bool force_overwrite = false;

  struct option opts[] = {{"output", required_argument, NULL, 'o'},
                          {"file", required_argument, NULL, 'f'},
                          {"muxer", required_argument, NULL, 'm'},
                          {"geometry", required_argument, NULL, 'g'},
                          {"codec", required_argument, NULL, 'c'},
                          {"codec-param", required_argument, NULL, 'p'},
                          {"framerate", required_argument, NULL, 'r'},
                          {"pixel-format", required_argument, NULL, 'x'},
                          {"audio-backend", required_argument, NULL, '*'},
                          {"audio-codec", required_argument, NULL, 'C'},
                          {"audio-codec-param", required_argument, NULL, 'P'},
                          {"sample-rate", required_argument, NULL, 'R'},
                          {"sample-format", required_argument, NULL, 'X'},
                          {"device", required_argument, NULL, 'd'},
                          {"no-dmabuf", no_argument, NULL, '&'},
                          {"filter", required_argument, NULL, 'F'},
                          {"log", no_argument, NULL, 'l'},
                          {"audio", optional_argument, NULL, 'a'},
                          {"help", no_argument, NULL, 'h'},
                          {"bframes", required_argument, NULL, 'b'},
                          {"buffrate", required_argument, NULL, 'B'},
                          {"version", no_argument, NULL, 'v'},
                          {"no-damage", no_argument, NULL, 'D'},
                          {"overwrite", no_argument, NULL, 'y'},
                          {0, 0, NULL, 0}};

  int c, i;
  while (
      (c = getopt_long(argc, argv, "o:f:m:g:c:p:r:x:C:P:R:X:d:b:B:la::hvDF:y",
                       opts, &i)) != -1) {
    switch (c) {
    case 'f':
      params.file = optarg;
      break;

    case 'F':
      params.video_filter = optarg;
      break;

    case 'o':
      cmdline_output = optarg;
      break;

    case 'm':
      params.muxer = optarg;
      break;

    case 'g':
      selected_region.set_from_string(optarg);
      break;

    case 'c':
      params.codec = optarg;
      break;

    case 'r':
      params.framerate = atoi(optarg);
      break;

    case 'x':
      params.pix_fmt = optarg;
      break;

    case 'C':
      params.audio_codec = optarg;
      break;

    case 'R':
      params.sample_rate = atoi(optarg);
      break;

    case 'X':
      params.sample_fmt = optarg;
      break;

    case 'd':
      params.hw_device = optarg;
      break;

    case 'b':
      params.bframes = atoi(optarg);
      break;

    case 'B':
      params.buffrate = atoi(optarg);
      break;

    case 'l':
      params.enable_ffmpeg_debug_output = true;
      break;

    case 'a':
      break;

    case 'h':
      help();
      break;

    case 'p':
      parse_codec_opts(params.codec_options, optarg);
      break;

    case 'v':
      printf("wf-recorder %s\n", WFRECORDER_VERSION);
      return 0;

    case 'D':
      use_damage = false;
      break;

    case 'P':
      parse_codec_opts(params.audio_codec_options, optarg);
      break;

    case '&':
      force_no_dmabuf = true;
      break;

    case 'y':
      force_overwrite = true;
      break;

    case '*':
      break;

    default:
      printf("Unsupported command line argument %s\n", optarg);
    }
  }

  if (!force_overwrite && !user_specified_overwrite(params.file)) {
    return EXIT_FAILURE;
  }

  display = wl_display_connect(NULL);
  if (display == NULL) {
    fprintf(stderr, "failed to create display: %m\n");
    return EXIT_FAILURE;
  }

  struct wl_registry *registry = wl_display_get_registry(display);
  wl_registry_add_listener(registry, &registry_listener, NULL);
  sync_wayland();

  if (params.codec.find("vaapi") != std::string::npos) {
    std::cerr << "using VA-API, trying to enable DMA-BUF capture..."
              << std::endl;

    // try compositor device if not explicitly set
    if (params.hw_device.empty()) {
      params.hw_device = drm_device_name;
    }

    // check we use same device as compositor
    if (!params.hw_device.empty() && params.hw_device == drm_device_name &&
        !force_no_dmabuf) {
      use_dmabuf = true;
    } else if (force_no_dmabuf) {
      std::cerr << "Disabling DMA-BUF as requested on command line"
                << std::endl;
    } else {
      std::cerr << "compositor running on different device, disabling DMA-BUF"
                << std::endl;
    }

    // region with dmabuf needs wlroots >= 0.17
    if (use_dmabuf && selected_region.is_selected()) {
      std::cerr << "region capture may not work with older wlroots, try "
                   "--no-dmabuf if it fails"
                << std::endl;
    }

    if (params.video_filter == "null") {
      params.video_filter = "scale_vaapi=format=nv12:out_range=full";
      if (!use_dmabuf) {
        params.video_filter.insert(0, "hwupload,");
      }
    }

    if (use_dmabuf) {
      std::cerr << "enabled DMA-BUF capture, device "
                << params.hw_device.c_str() << std::endl;

      drm_fd = open(params.hw_device.c_str(), O_RDWR);
      if (drm_fd < 0) {
        fprintf(stderr, "failed to open drm device: %m\n");
        return EXIT_FAILURE;
      }

      gbm_device = gbm_create_device(drm_fd);
      if (gbm_device == NULL) {
        fprintf(stderr, "failed to create gbm device: %m\n");
        return EXIT_FAILURE;
      }

      use_hwupload = params.video_filter.find("hwupload") != std::string::npos;
    }
  }

  printf("SO DMA BUF %d\n", use_dmabuf);

  check_has_protos();
  load_output_info();

  if (available_outputs.size() == 1) {
    chosen_output = &available_outputs.front();
    if (chosen_output->name != cmdline_output &&
        cmdline_output != default_cmdline_output) {
      std::cerr << "Couldn't find requested output " << cmdline_output
                << std::endl;
      return EXIT_FAILURE;
    }
  } else {
    for (auto &wo : available_outputs) {
      if (wo.name == cmdline_output)
        chosen_output = &wo;
    }

    if (chosen_output == NULL) {
      if (cmdline_output != default_cmdline_output) {
        std::cerr << "Couldn't find requested output " << cmdline_output.c_str()
                  << std::endl;
        return EXIT_FAILURE;
      }

      if (selected_region.is_selected()) {
        chosen_output = detect_output_from_region(selected_region);
      } else {
        chosen_output = choose_interactive();
      }
    }
  }

  if (chosen_output == nullptr) {
    fprintf(stderr, "Failed to select output, exiting\n");
    return EXIT_FAILURE;
  }

  if (selected_region.is_selected()) {
    if (!selected_region.contained_in({chosen_output->x, chosen_output->y,
                                       chosen_output->width,
                                       chosen_output->height})) {
      fprintf(stderr, "Invalid region to capture: must be completely "
                      "inside the output\n");
      selected_region = capture_region{};
    }
  }

  printf("selected region %d,%d %dx%d\n", selected_region.x, selected_region.y,
         selected_region.width, selected_region.height);

  std::thread writer_thread;

  for (auto signo : GRACEFUL_TERMINATION_SIGNALS) {
    signal(signo, handle_graceful_termination);
  }

  assert(use_dmabuf);

  writer_thread = std::thread([=]() { write_loop(); });

  int fps = 0;
  struct timeval tp;
  gettimeofday(&tp, NULL);
  long int prev_ms = tp.tv_sec * 1000 + tp.tv_usec / 1000;

  while (!exit_main_loop) {
    can_read_buffer = false;
    request_next_frame();

    while (!can_read_buffer && !exit_main_loop &&
           wl_display_dispatch(display) != -1) {
      std::this_thread::sleep_for(std::chrono::microseconds(10));
    }

    if (exit_main_loop) {
      break;
    }

    struct timeval tp;
    gettimeofday(&tp, NULL);
    long int ms = tp.tv_sec * 1000 + tp.tv_usec / 1000;

    fps++;

    if (ms - prev_ms >= 1000) {
      printf("FPS %d\n", fps);
      fps = 0;
      prev_ms = ms;
    }
  }

  if (writer_thread.joinable()) {
    writer_thread.join();
  }

  if (gbm_device) {
    gbm_device_destroy(gbm_device);
    close(drm_fd);
  }

  return EXIT_SUCCESS;
}
