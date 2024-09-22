#include "src/server.hpp"
extern "C" {
#include "../include/capture/kms.h"
#include "../include/capture/nvfbc.h"
#include "../include/capture/xcomposite.h"
#ifdef GSR_PORTAL
#include "../include/capture/portal.h"
#include "../include/dbus.h"
#endif
#include "../include/color_conversion.h"
#include "../include/damage.h"
#include "../include/egl.h"
#include "../include/encoder/video/cuda.h"
#include "../include/encoder/video/software.h"
#include "../include/encoder/video/vaapi.h"
#include "../include/utils.h"
}

#include <assert.h>
#include <libgen.h>
#include <map>
#include <mutex>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <sys/stat.h>
#include <sys/wait.h>
#include <thread>
#include <unistd.h>
#include <unordered_map>
#include <vector>

#include "../include/sound.hpp"

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavfilter/avfilter.h>
#include <libavfilter/buffersink.h>
#include <libavfilter/buffersrc.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libavutil/mastering_display_metadata.h>
#include <libavutil/opt.h>
#include <libavutil/pixfmt.h>
#include <libavutil/time.h>
#include <libswresample/swresample.h>
}

#include <deque>
#include <future>

#ifndef GSR_VERSION
#define GSR_VERSION "unknown"
#endif

// TODO: If options are not supported then they are returned (allocated) in the
// options. This should be free'd.

// TODO: Remove LIBAVUTIL_VERSION_MAJOR checks in the future when ubuntu, pop os
// LTS etc update ffmpeg to >= 5.0

static const int AUDIO_SAMPLE_RATE = 48000;

static const int VIDEO_STREAM_INDEX = 0;

static Server server;

static thread_local char av_error_buffer[AV_ERROR_MAX_STRING_SIZE];

static void monitor_output_callback_print(const gsr_monitor *monitor,
                                          void *userdata) {
  (void)userdata;
  fprintf(stderr, "    \"%.*s\"    (%dx%d+%d+%d)\n", monitor->name_len,
          monitor->name, monitor->size.x, monitor->size.y, monitor->pos.x,
          monitor->pos.y);
}

typedef struct {
  const char *output_name;
} FirstOutputCallback;

static void get_first_output(const gsr_monitor *monitor, void *userdata) {
  FirstOutputCallback *first_output = (FirstOutputCallback *)userdata;
  if (!first_output->output_name)
    first_output->output_name = strndup(monitor->name, monitor->name_len + 1);
}

static char *av_error_to_string(int err) {
  if (av_strerror(err, av_error_buffer, sizeof(av_error_buffer)) < 0)
    strcpy(av_error_buffer, "Unknown error");
  return av_error_buffer;
}

enum class VideoQuality { MEDIUM, HIGH, VERY_HIGH, ULTRA };

enum class VideoCodec {
  H264,
  HEVC,
  HEVC_HDR,
  HEVC_10BIT,
  AV1,
  AV1_HDR,
  AV1_10BIT,
  VP8,
  VP9
};

enum class AudioCodec { AAC, OPUS, FLAC };

enum class PixelFormat { YUV420, YUV444 };

enum class FramerateMode { CONSTANT, VARIABLE, CONTENT };

enum class BitrateMode { QP, VBR };

static int x11_error_handler(Display *, XErrorEvent *) { return 0; }

static int x11_io_error_handler(Display *) { return 0; }

static bool video_codec_is_hdr(VideoCodec video_codec) {
  switch (video_codec) {
  case VideoCodec::HEVC_HDR:
  case VideoCodec::AV1_HDR:
    return true;
  default:
    return false;
  }
}

static VideoCodec hdr_video_codec_to_sdr_video_codec(VideoCodec video_codec) {
  switch (video_codec) {
  case VideoCodec::HEVC_HDR:
    return VideoCodec::HEVC;
  case VideoCodec::AV1_HDR:
    return VideoCodec::AV1;
  default:
    return video_codec;
  }
}

static gsr_color_depth video_codec_to_bit_depth(VideoCodec video_codec) {
  switch (video_codec) {
  case VideoCodec::HEVC_HDR:
  case VideoCodec::HEVC_10BIT:
  case VideoCodec::AV1_HDR:
  case VideoCodec::AV1_10BIT:
    return GSR_COLOR_DEPTH_10_BITS;
  default:
    return GSR_COLOR_DEPTH_8_BITS;
  }
}

// static bool video_codec_is_hevc(VideoCodec video_codec) {
//     switch(video_codec) {
//         case VideoCodec::HEVC:
//         case VideoCodec::HEVC_HDR:
//         case VideoCodec::HEVC_10BIT:
//             return true;
//         default:
//             return false;
//     }
// }

static bool video_codec_is_av1(VideoCodec video_codec) {
  switch (video_codec) {
  case VideoCodec::AV1:
  case VideoCodec::AV1_HDR:
  case VideoCodec::AV1_10BIT:
    return true;
  default:
    return false;
  }
}

struct PacketData {
  PacketData() {}
  PacketData(const PacketData &) = delete;
  PacketData &operator=(const PacketData &) = delete;

  ~PacketData() { av_free(data.data); }

  AVPacket data;
};

// |stream| is only required for non-replay mode
static void
receive_frames(AVCodecContext *av_codec_context, int stream_index,
               AVStream *stream, int64_t pts,
               AVFormatContext *av_format_context, double replay_start_time,
               std::deque<std::shared_ptr<PacketData>> &frame_data_queue,
               int replay_buffer_size_secs, bool &frames_erased,
               std::mutex &write_output_mutex, double paused_time_offset) {
  for (;;) {
    AVPacket *av_packet = av_packet_alloc();
    if (!av_packet)
      break;

    av_packet->data = NULL;
    av_packet->size = 0;
    int res = avcodec_receive_packet(av_codec_context, av_packet);
    if (res == 0) { // we have a packet, send the packet to the muxer
      av_packet->stream_index = stream_index;
      av_packet->pts = pts;
      av_packet->dts = pts;

      std::lock_guard<std::mutex> lock(write_output_mutex);
      if (replay_buffer_size_secs != -1) {
        // TODO: Preallocate all frames data and use those instead.
        // Why are we doing this you ask? there is a new ffmpeg bug that causes
        // cpu usage to increase over time when you have packets that are not
        // being free'd until later. So we copy the packet data, free the packet
        // and then reconstruct the packet later on when we need it, to keep
        // packets alive only for a short period.
        auto new_packet = std::make_shared<PacketData>();
        new_packet->data = *av_packet;
        new_packet->data.data = (uint8_t *)av_malloc(av_packet->size);
        memcpy(new_packet->data.data, av_packet->data, av_packet->size);

        double time_now = clock_get_monotonic_seconds() - paused_time_offset;
        double replay_time_elapsed = time_now - replay_start_time;

        frame_data_queue.push_back(std::move(new_packet));
        if (replay_time_elapsed >= replay_buffer_size_secs) {
          frame_data_queue.pop_front();
          frames_erased = true;
        }
      } else {
        // send data -giammi
        server.send_data(av_packet->data, av_packet->size);

        av_packet_rescale_ts(av_packet, av_codec_context->time_base,
                             stream->time_base);
        av_packet->stream_index = stream->index;
        // TODO: Is av_interleaved_write_frame needed?. Answer: might be needed
        // for mkv but dont use it! it causes frames to be inconsistent,
        // skipping frames and duplicating frames
        int ret = av_write_frame(av_format_context, av_packet);
        if (ret < 0) {
          fprintf(stderr,
                  "Error: Failed to write frame index %d to muxer, reason: %s "
                  "(%d)\n",
                  av_packet->stream_index, av_error_to_string(ret), ret);
        }
      }
      av_packet_free(&av_packet);
    } else if (res == AVERROR(EAGAIN)) { // we have no packet
                                         // fprintf(stderr, "No packet!\n");
      av_packet_free(&av_packet);
      break;
    } else if (res == AVERROR_EOF) { // this is the end of the stream
      av_packet_free(&av_packet);
      fprintf(stderr, "End of stream!\n");
      break;
    } else {
      av_packet_free(&av_packet);
      fprintf(stderr, "Unexpected error: %d\n", res);
      break;
    }
  }
}

static const char *audio_codec_get_name(AudioCodec audio_codec) {
  switch (audio_codec) {
  case AudioCodec::AAC:
    return "aac";
  case AudioCodec::OPUS:
    return "opus";
  case AudioCodec::FLAC:
    return "flac";
  }
  assert(false);
  return "";
}

static AVCodecID audio_codec_get_id(AudioCodec audio_codec) {
  switch (audio_codec) {
  case AudioCodec::AAC:
    return AV_CODEC_ID_AAC;
  case AudioCodec::OPUS:
    return AV_CODEC_ID_OPUS;
  case AudioCodec::FLAC:
    return AV_CODEC_ID_FLAC;
  }
  assert(false);
  return AV_CODEC_ID_AAC;
}

static AVSampleFormat
audio_codec_get_sample_format(AVCodecContext *audio_codec_context,
                              AudioCodec audio_codec, const AVCodec *codec,
                              bool mix_audio) {
  (void)audio_codec_context;
  switch (audio_codec) {
  case AudioCodec::AAC: {
    return AV_SAMPLE_FMT_FLTP;
  }
  case AudioCodec::OPUS: {
    bool supports_s16 = false;
    bool supports_flt = false;

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 15, 0)
    for (size_t i = 0;
         codec->sample_fmts && codec->sample_fmts[i] != AV_SAMPLE_FMT_NONE;
         ++i) {
      if (codec->sample_fmts[i] == AV_SAMPLE_FMT_S16) {
        supports_s16 = true;
      } else if (codec->sample_fmts[i] == AV_SAMPLE_FMT_FLT) {
        supports_flt = true;
      }
    }
#else
    const enum AVSampleFormat *sample_fmts = NULL;
    if (avcodec_get_supported_config(audio_codec_context, codec,
                                     AV_CODEC_CONFIG_SAMPLE_FORMAT, 0,
                                     (const void **)&sample_fmts, NULL) >= 0) {
      if (sample_fmts) {
        for (size_t i = 0; sample_fmts[i] != AV_SAMPLE_FMT_NONE; ++i) {
          if (sample_fmts[i] == AV_SAMPLE_FMT_S16) {
            supports_s16 = true;
          } else if (sample_fmts[i] == AV_SAMPLE_FMT_FLT) {
            supports_flt = true;
          }
        }
      } else {
        // What a dumb API. It returns NULL if all formats are supported
        supports_s16 = true;
        supports_flt = true;
      }
    }
#endif

    // Amix only works with float audio
    if (mix_audio)
      supports_s16 = false;

    if (!supports_s16 && !supports_flt) {
      fprintf(stderr, "Warning: opus audio codec is chosen but your ffmpeg "
                      "version does not support s16/flt sample format and "
                      "performance might be slightly worse.\n");
      fprintf(stderr,
              "  You can either rebuild ffmpeg with libopus instead of the "
              "built-in opus, use the flatpak version of gpu screen recorder "
              "or record with aac audio codec instead (-ac aac).\n");
      fprintf(stderr, "  Falling back to fltp audio sample format instead.\n");
    }

    if (supports_s16)
      return AV_SAMPLE_FMT_S16;
    else if (supports_flt)
      return AV_SAMPLE_FMT_FLT;
    else
      return AV_SAMPLE_FMT_FLTP;
  }
  case AudioCodec::FLAC: {
    return AV_SAMPLE_FMT_S32;
  }
  }
  assert(false);
  return AV_SAMPLE_FMT_FLTP;
}

static int64_t audio_codec_get_get_bitrate(AudioCodec audio_codec) {
  switch (audio_codec) {
  case AudioCodec::AAC:
    return 160000;
  case AudioCodec::OPUS:
    return 128000;
  case AudioCodec::FLAC:
    return 128000;
  }
  assert(false);
  return 128000;
}

static AudioFormat audio_codec_context_get_audio_format(
    const AVCodecContext *audio_codec_context) {
  switch (audio_codec_context->sample_fmt) {
  case AV_SAMPLE_FMT_FLT:
    return F32;
  case AV_SAMPLE_FMT_FLTP:
    return S32;
  case AV_SAMPLE_FMT_S16:
    return S16;
  case AV_SAMPLE_FMT_S32:
    return S32;
  default:
    return S16;
  }
}

static AVSampleFormat
audio_format_to_sample_format(const AudioFormat audio_format) {
  switch (audio_format) {
  case S16:
    return AV_SAMPLE_FMT_S16;
  case S32:
    return AV_SAMPLE_FMT_S32;
  case F32:
    return AV_SAMPLE_FMT_FLT;
  }
  assert(false);
  return AV_SAMPLE_FMT_S16;
}

static AVCodecContext *create_audio_codec_context(int fps,
                                                  AudioCodec audio_codec,
                                                  bool mix_audio,
                                                  int audio_bitrate) {
  (void)fps;
  const AVCodec *codec = avcodec_find_encoder(audio_codec_get_id(audio_codec));
  if (!codec) {
    fprintf(stderr, "Error: Could not find %s audio encoder\n",
            audio_codec_get_name(audio_codec));
    _exit(1);
  }

  AVCodecContext *codec_context = avcodec_alloc_context3(codec);

  assert(codec->type == AVMEDIA_TYPE_AUDIO);
  codec_context->codec_id = codec->id;
  codec_context->sample_fmt = audio_codec_get_sample_format(
      codec_context, audio_codec, codec, mix_audio);
  codec_context->bit_rate = audio_bitrate == 0
                                ? audio_codec_get_get_bitrate(audio_codec)
                                : audio_bitrate;
  codec_context->sample_rate = AUDIO_SAMPLE_RATE;
  if (audio_codec == AudioCodec::AAC)
    codec_context->profile = FF_PROFILE_AAC_LOW;
#if LIBAVCODEC_VERSION_MAJOR < 60
  codec_context->channel_layout = AV_CH_LAYOUT_STEREO;
  codec_context->channels = 2;
#else
  av_channel_layout_default(&codec_context->ch_layout, 2);
#endif

  codec_context->time_base.num = 1;
  codec_context->time_base.den = codec_context->sample_rate;
  codec_context->thread_count = 1;
  codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  return codec_context;
}

static int vbr_get_quality_parameter(AVCodecContext *codec_context,
                                     VideoQuality video_quality, bool hdr) {
  // 8 bit / 10 bit = 80%
  const float qp_multiply = hdr ? 8.0f / 10.0f : 1.0f;
  if (codec_context->codec_id == AV_CODEC_ID_AV1) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      return 160 * qp_multiply;
    case VideoQuality::HIGH:
      return 130 * qp_multiply;
    case VideoQuality::VERY_HIGH:
      return 110 * qp_multiply;
    case VideoQuality::ULTRA:
      return 90 * qp_multiply;
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_H264) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      return 35 * qp_multiply;
    case VideoQuality::HIGH:
      return 30 * qp_multiply;
    case VideoQuality::VERY_HIGH:
      return 25 * qp_multiply;
    case VideoQuality::ULTRA:
      return 22 * qp_multiply;
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      return 35 * qp_multiply;
    case VideoQuality::HIGH:
      return 30 * qp_multiply;
    case VideoQuality::VERY_HIGH:
      return 25 * qp_multiply;
    case VideoQuality::ULTRA:
      return 22 * qp_multiply;
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_VP8 ||
             codec_context->codec_id == AV_CODEC_ID_VP9) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      return 35 * qp_multiply;
    case VideoQuality::HIGH:
      return 30 * qp_multiply;
    case VideoQuality::VERY_HIGH:
      return 25 * qp_multiply;
    case VideoQuality::ULTRA:
      return 22 * qp_multiply;
    }
  }
  assert(false);
  return 22 * qp_multiply;
}

static AVCodecContext *create_video_codec_context(
    AVPixelFormat pix_fmt, VideoQuality video_quality, int fps,
    const AVCodec *codec, bool low_latency, gsr_gpu_vendor vendor,
    FramerateMode framerate_mode, bool hdr, gsr_color_range color_range,
    float keyint, bool use_software_video_encoder, BitrateMode bitrate_mode) {

  AVCodecContext *codec_context = avcodec_alloc_context3(codec);

  // double fps_ratio = (double)fps / 30.0;

  assert(codec->type == AVMEDIA_TYPE_VIDEO);
  codec_context->codec_id = codec->id;
  // Timebase: This is the fundamental unit of time (in seconds) in terms
  // of which frame timestamps are represented. For fixed-fps content,
  // timebase should be 1/framerate and timestamp increments should be
  // identical to 1
  codec_context->time_base.num = 1;
  codec_context->time_base.den =
      framerate_mode == FramerateMode::CONSTANT ? fps : AV_TIME_BASE;
  codec_context->framerate.num = fps;
  codec_context->framerate.den = 1;
  codec_context->sample_aspect_ratio.num = 0;
  codec_context->sample_aspect_ratio.den = 0;
  if (low_latency) {
    codec_context->flags |=
        (AV_CODEC_FLAG_CLOSED_GOP | AV_CODEC_FLAG_LOW_DELAY);
    codec_context->flags2 |= AV_CODEC_FLAG2_FAST;
    // codec_context->gop_size = std::numeric_limits<int>::max();
    // codec_context->keyint_min = std::numeric_limits<int>::max();
    codec_context->gop_size = fps * keyint;
  } else {
    // High values reduce file size but increases time it takes to seek
    codec_context->gop_size = fps * keyint;
  }
  codec_context->max_b_frames = 0;
  codec_context->pix_fmt = pix_fmt;
  codec_context->color_range = color_range == GSR_COLOR_RANGE_LIMITED
                                   ? AVCOL_RANGE_MPEG
                                   : AVCOL_RANGE_JPEG;
  if (hdr) {
    codec_context->color_primaries = AVCOL_PRI_BT2020;
    codec_context->color_trc = AVCOL_TRC_SMPTE2084;
    codec_context->colorspace = AVCOL_SPC_BT2020_NCL;
  } else {
    codec_context->color_primaries = AVCOL_PRI_BT709;
    codec_context->color_trc = AVCOL_TRC_BT709;
    codec_context->colorspace = AVCOL_SPC_BT709;
  }
  // codec_context->chroma_sample_location = AVCHROMA_LOC_CENTER;
  if (codec->id == AV_CODEC_ID_HEVC)
    codec_context->codec_tag = MKTAG(
        'h', 'v', 'c',
        '1'); // QuickTime on MacOS requires this or the video wont be playable

  if (bitrate_mode == BitrateMode::VBR) {
    const int quality =
        vbr_get_quality_parameter(codec_context, video_quality, hdr);
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      codec_context->qmin = quality;
      codec_context->qmax = quality;
      codec_context->bit_rate = 100000; // 4500000 + (codec_context->width *
                                        // codec_context->height)*0.75;
      break;
    case VideoQuality::HIGH:
      codec_context->qmin = quality;
      codec_context->qmax = quality;
      codec_context->bit_rate =
          100000; // 10000000-9000000 + (codec_context->width *
                  // codec_context->height)*0.75;
      break;
    case VideoQuality::VERY_HIGH:
      codec_context->qmin = quality;
      codec_context->qmax = quality;
      codec_context->bit_rate =
          100000; // 10000000-9000000 + (codec_context->width *
                  // codec_context->height)*0.75;
      break;
    case VideoQuality::ULTRA:
      codec_context->qmin = quality;
      codec_context->qmax = quality;
      codec_context->bit_rate =
          100000; // 10000000-9000000 + (codec_context->width *
                  // codec_context->height)*0.75;
      break;
    }

    codec_context->rc_max_rate = codec_context->bit_rate;
    codec_context->rc_min_rate = codec_context->bit_rate;
    codec_context->rc_buffer_size =
        codec_context->bit_rate; // codec_context->bit_rate / 10;
    codec_context->rc_initial_buffer_occupancy =
        100000; // codec_context->bit_rate * 1000;
  }
  // codec_context->profile = FF_PROFILE_H264_MAIN;
  if (codec_context->codec_id == AV_CODEC_ID_MPEG1VIDEO)
    codec_context->mb_decision = 2;

  if (!use_software_video_encoder && vendor != GSR_GPU_VENDOR_NVIDIA) {
    // 8 bit / 10 bit = 80%, and increase it even more
    const float quality_multiply = hdr ? (8.0f / 10.0f * 0.7f) : 1.0f;
    if (codec_context->codec_id == AV_CODEC_ID_AV1 ||
        codec_context->codec_id == AV_CODEC_ID_H264 ||
        codec_context->codec_id == AV_CODEC_ID_HEVC) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        codec_context->global_quality = 150 * quality_multiply;
        break;
      case VideoQuality::HIGH:
        codec_context->global_quality = 120 * quality_multiply;
        break;
      case VideoQuality::VERY_HIGH:
        codec_context->global_quality = 100 * quality_multiply;
        break;
      case VideoQuality::ULTRA:
        codec_context->global_quality = 90 * quality_multiply;
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_VP8) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        codec_context->global_quality = 35 * quality_multiply;
        break;
      case VideoQuality::HIGH:
        codec_context->global_quality = 30 * quality_multiply;
        break;
      case VideoQuality::VERY_HIGH:
        codec_context->global_quality = 20 * quality_multiply;
        break;
      case VideoQuality::ULTRA:
        codec_context->global_quality = 10 * quality_multiply;
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_VP9) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        codec_context->global_quality = 35 * quality_multiply;
        break;
      case VideoQuality::HIGH:
        codec_context->global_quality = 30 * quality_multiply;
        break;
      case VideoQuality::VERY_HIGH:
        codec_context->global_quality = 20 * quality_multiply;
        break;
      case VideoQuality::ULTRA:
        codec_context->global_quality = 10 * quality_multiply;
        break;
      }
    }
  }

  av_opt_set_int(codec_context->priv_data, "b_ref_mode", 0, 0);
  // av_opt_set_int(codec_context->priv_data, "cbr", true, 0);

  if (vendor != GSR_GPU_VENDOR_NVIDIA) {
    // TODO: More options, better options
    // codec_context->bit_rate = codec_context->width * codec_context->height;
    if (bitrate_mode == BitrateMode::QP)
      av_opt_set(codec_context->priv_data, "rc_mode", "CQP", 0);
    else
      av_opt_set(codec_context->priv_data, "rc_mode", "VBR", 0);
    // codec_context->global_quality = 4;
    // codec_context->compression_level = 2;
  }

  // av_opt_set(codec_context->priv_data, "bsf",
  // "hevc_metadata=colour_primaries=9:transfer_characteristics=16:matrix_coefficients=9",
  // 0);

  codec_context->flags |= AV_CODEC_FLAG_GLOBAL_HEADER;

  return codec_context;
}

static void open_audio(AVCodecContext *audio_codec_context) {
  AVDictionary *options = nullptr;
  av_dict_set(&options, "strict", "experimental", 0);

  int ret;
  ret =
      avcodec_open2(audio_codec_context, audio_codec_context->codec, &options);
  if (ret < 0) {
    fprintf(stderr, "failed to open codec, reason: %s\n",
            av_error_to_string(ret));
    _exit(1);
  }
}

static AVFrame *create_audio_frame(AVCodecContext *audio_codec_context) {
  AVFrame *frame = av_frame_alloc();
  if (!frame) {
    fprintf(stderr, "failed to allocate audio frame\n");
    _exit(1);
  }

  frame->sample_rate = audio_codec_context->sample_rate;
  frame->nb_samples = audio_codec_context->frame_size;
  frame->format = audio_codec_context->sample_fmt;
#if LIBAVCODEC_VERSION_MAJOR < 60
  frame->channels = audio_codec_context->channels;
  frame->channel_layout = audio_codec_context->channel_layout;
#else
  av_channel_layout_copy(&frame->ch_layout, &audio_codec_context->ch_layout);
#endif

  int ret = av_frame_get_buffer(frame, 0);
  if (ret < 0) {
    fprintf(stderr, "failed to allocate audio data buffers, reason: %s\n",
            av_error_to_string(ret));
    _exit(1);
  }

  return frame;
}

static void dict_set_profile(AVCodecContext *codec_context,
                             gsr_gpu_vendor vendor, gsr_color_depth color_depth,
                             AVDictionary **options) {
#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(61, 17, 100)
  if (codec_context->codec_id == AV_CODEC_ID_H264) {
    // TODO: Only for vaapi
    // if(color_depth == GSR_COLOR_DEPTH_10_BITS)
    //    av_dict_set(options, "profile", "high10", 0);
    // else
    av_dict_set(options, "profile", "high", 0);
  } else if (codec_context->codec_id == AV_CODEC_ID_AV1) {
    if (vendor == GSR_GPU_VENDOR_NVIDIA) {
      if (color_depth == GSR_COLOR_DEPTH_10_BITS)
        av_dict_set_int(options, "highbitdepth", 1, 0);
    } else {
      av_dict_set(options, "profile", "main",
                  0); // TODO: use professional instead?
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
    if (color_depth == GSR_COLOR_DEPTH_10_BITS)
      av_dict_set(options, "profile", "main10", 0);
    else
      av_dict_set(options, "profile", "main", 0);
  }
#else
  if (codec_context->codec_id == AV_CODEC_ID_H264) {
    // TODO: Only for vaapi
    // if(color_depth == GSR_COLOR_DEPTH_10_BITS)
    //    av_dict_set_int(options, "profile", AV_PROFILE_H264_HIGH_10, 0);
    // else
    av_dict_set_int(options, "profile", AV_PROFILE_H264_HIGH, 0);
  } else if (codec_context->codec_id == AV_CODEC_ID_AV1) {
    if (vendor == GSR_GPU_VENDOR_NVIDIA) {
      if (color_depth == GSR_COLOR_DEPTH_10_BITS)
        av_dict_set_int(options, "highbitdepth", 1, 0);
    } else {
      av_dict_set_int(options, "profile", AV_PROFILE_AV1_MAIN,
                      0); // TODO: use professional instead?
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
    if (color_depth == GSR_COLOR_DEPTH_10_BITS)
      av_dict_set_int(options, "profile", AV_PROFILE_HEVC_MAIN_10, 0);
    else
      av_dict_set_int(options, "profile", AV_PROFILE_HEVC_MAIN, 0);
  }
#endif
}

static void video_software_set_qp(AVCodecContext *codec_context,
                                  VideoQuality video_quality, bool hdr,
                                  AVDictionary **options) {
  // 8 bit / 10 bit = 80%
  const float qp_multiply = hdr ? 8.0f / 10.0f : 1.0f;
  if (codec_context->codec_id == AV_CODEC_ID_AV1) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
      break;
    case VideoQuality::HIGH:
      av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
      break;
    case VideoQuality::VERY_HIGH:
      av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
      break;
    case VideoQuality::ULTRA:
      av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
      break;
    }
  } else if (codec_context->codec_id == AV_CODEC_ID_H264) {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      av_dict_set_int(options, "qp", 34 * qp_multiply, 0);
      break;
    case VideoQuality::HIGH:
      av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
      break;
    case VideoQuality::VERY_HIGH:
      av_dict_set_int(options, "qp", 23 * qp_multiply, 0);
      break;
    case VideoQuality::ULTRA:
      av_dict_set_int(options, "qp", 20 * qp_multiply, 0);
      break;
    }
  } else {
    switch (video_quality) {
    case VideoQuality::MEDIUM:
      av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
      break;
    case VideoQuality::HIGH:
      av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
      break;
    case VideoQuality::VERY_HIGH:
      av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
      break;
    case VideoQuality::ULTRA:
      av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
      break;
    }
  }
}

static void open_video_software(AVCodecContext *codec_context,
                                VideoQuality video_quality,
                                PixelFormat pixel_format, bool hdr,
                                gsr_color_depth color_depth,
                                BitrateMode bitrate_mode) {
  (void)pixel_format; // TODO:
  AVDictionary *options = nullptr;

  if (bitrate_mode == BitrateMode::QP)
    video_software_set_qp(codec_context, video_quality, hdr, &options);

  av_dict_set(&options, "preset", "medium", 0);
  dict_set_profile(codec_context, GSR_GPU_VENDOR_INTEL, color_depth, &options);
  // TODO: If streaming or piping output set this to zerolatency
  av_dict_set(&options, "tune", "fastdecode", 0);

  if (codec_context->codec_id == AV_CODEC_ID_H264) {
    av_dict_set(&options, "coder", "cabac",
                0); // TODO: cavlc is faster than cabac but worse compression.
                    // Which to use?
  }

  av_dict_set(&options, "strict", "experimental", 0);

  int ret = avcodec_open2(codec_context, codec_context->codec, &options);
  if (ret < 0) {
    fprintf(stderr, "Error: Could not open video codec: %s\n",
            av_error_to_string(ret));
    _exit(1);
  }
}

static void video_hardware_set_qp(AVCodecContext *codec_context,
                                  VideoQuality video_quality,
                                  gsr_gpu_vendor vendor, bool hdr,
                                  AVDictionary **options) {
  // 8 bit / 10 bit = 80%
  const float qp_multiply = hdr ? 8.0f / 10.0f : 1.0f;
  if (vendor == GSR_GPU_VENDOR_NVIDIA) {
    // TODO: Test if these should be in the same range as vaapi
    if (codec_context->codec_id == AV_CODEC_ID_AV1) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_H264) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 34 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 23 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 20 * qp_multiply, 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_VP8 ||
               codec_context->codec_id == AV_CODEC_ID_VP9) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
        break;
      }
    }

    av_dict_set(options, "rc", "constqp", 0);
  } else {
    if (codec_context->codec_id == AV_CODEC_ID_AV1) {
      // Using global_quality option
    } else if (codec_context->codec_id == AV_CODEC_ID_H264) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 34 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 23 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 20 * qp_multiply, 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_VP8 ||
               codec_context->codec_id == AV_CODEC_ID_VP9) {
      switch (video_quality) {
      case VideoQuality::MEDIUM:
        av_dict_set_int(options, "qp", 35 * qp_multiply, 0);
        break;
      case VideoQuality::HIGH:
        av_dict_set_int(options, "qp", 30 * qp_multiply, 0);
        break;
      case VideoQuality::VERY_HIGH:
        av_dict_set_int(options, "qp", 25 * qp_multiply, 0);
        break;
      case VideoQuality::ULTRA:
        av_dict_set_int(options, "qp", 22 * qp_multiply, 0);
        break;
      }
    }

    av_dict_set(options, "rc_mode", "CQP", 0);
  }
}

static void open_video_hardware(AVCodecContext *codec_context,
                                VideoQuality video_quality, bool very_old_gpu,
                                gsr_gpu_vendor vendor, PixelFormat pixel_format,
                                bool hdr, gsr_color_depth color_depth,
                                BitrateMode bitrate_mode) {
  (void)very_old_gpu;
  AVDictionary *options = nullptr;

  if (bitrate_mode == BitrateMode::QP) {
    video_hardware_set_qp(codec_context, video_quality, vendor, hdr, &options);
  } else {
    if (vendor == GSR_GPU_VENDOR_NVIDIA) {
      av_dict_set(&options, "rc", "vbr", 0);
    } else {
      av_dict_set(&options, "rc_mode", "VBR", 0);
    }
  }

  // TODO: Enable multipass

  if (vendor == GSR_GPU_VENDOR_NVIDIA) {
    av_dict_set(&options, "tune", "hq", 0);

    dict_set_profile(codec_context, vendor, color_depth, &options);

    if (codec_context->codec_id == AV_CODEC_ID_H264) {
      // TODO: h264 10bit?
      // TODO:
      // switch(pixel_format) {
      //     case PixelFormat::YUV420:
      //         av_dict_set_int(&options, "profile", AV_PROFILE_H264_HIGH, 0);
      //         break;
      //     case PixelFormat::YUV444:
      //         av_dict_set_int(&options, "profile", AV_PROFILE_H264_HIGH_444,
      //         0); break;
      // }
    } else if (codec_context->codec_id == AV_CODEC_ID_AV1) {
      switch (pixel_format) {
      case PixelFormat::YUV420:
        av_dict_set(&options, "rgb_mode", "yuv420", 0);
        break;
      case PixelFormat::YUV444:
        av_dict_set(&options, "rgb_mode", "yuv444", 0);
        break;
      }
    } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
      // av_dict_set(&options, "pix_fmt", "yuv420p16le", 0);
    }
  } else {
    // TODO: More quality options
    // av_dict_set_int(&options, "low_power", 1, 0);
    // Improves performance but increases vram
    // av_dict_set_int(&options, "async_depth", 8, 0);

    if (codec_context->codec_id == AV_CODEC_ID_H264) {
      // Removed because it causes stutter in games for some people
      // av_dict_set_int(&options, "quality", 5, 0); // quality preset
    } else if (codec_context->codec_id == AV_CODEC_ID_AV1) {
      av_dict_set(&options, "tier", "main", 0);
    } else if (codec_context->codec_id == AV_CODEC_ID_HEVC) {
      if (hdr)
        av_dict_set(&options, "sei", "hdr", 0);
    }

    // TODO: vp8/vp9 10bit
  }

  if (codec_context->codec_id == AV_CODEC_ID_H264) {
    av_dict_set(&options, "coder", "cabac",
                0); // TODO: cavlc is faster than cabac but worse compression.
                    // Which to use?
  }

  av_dict_set(&options, "strict", "experimental", 0);

  int ret = avcodec_open2(codec_context, codec_context->codec, &options);
  if (ret < 0) {
    fprintf(stderr, "Error: Could not open video codec: %s\n",
            av_error_to_string(ret));
    _exit(1);
  }
}

static void usage_header() {
  const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
  const char *program_name = inside_flatpak
                                 ? "flatpak run --command=gpu-screen-recorder "
                                   "com.dec05eba.gpu_screen_recorder"
                                 : "gpu-screen-recorder";
  fprintf(
      stderr,
      "usage: %s -w <window_id|monitor|focused|portal> [-c <container_format>] "
      "[-s WxH] -f <fps> [-a <audio_input>] [-q <quality>] [-r "
      "<replay_buffer_size_sec>] [-k "
      "h264|hevc|av1|vp8|vp9|hevc_hdr|av1_hdr|hevc_10bit|av1_10bit] [-ac "
      "aac|opus|flac] [-ab <bitrate>] [-oc yes|no] [-fm cfr|vfr|content] [-bm "
      "auto|qp|vbr] [-cr limited|full] [-df yes|no] [-sc <script_path>] "
      "[-cursor yes|no] [-keyint <value>] [-restore-portal-session yes|no] "
      "[-portal-session-token-filepath filepath] [-encoder gpu|cpu] [-o "
      "<output_file>] [-v yes|no] [--version] [-h|--help]\n",
      program_name);
}

// TODO: Update with portal info
static void usage_full() {
  const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;
  const char *program_name = inside_flatpak
                                 ? "flatpak run --command=gpu-screen-recorder "
                                   "com.dec05eba.gpu_screen_recorder"
                                 : "gpu-screen-recorder";
  usage_header();
  fprintf(stderr, "\n");
  fprintf(stderr, "OPTIONS:\n");
  fprintf(stderr,
          "  -w    Window id to record, a display (monitor name), \"screen\", "
          "\"screen-direct-force\", \"focused\" or \"portal\".\n");
  fprintf(
      stderr,
      "        If this is \"portal\" then xdg desktop screencast portal with "
      "pipewire will be used. Portal option is only available on Wayland.\n");
  fprintf(stderr, "        If you select to save the session (token) in the "
                  "desktop portal capture popup then the session will be saved "
                  "for the next time you use \"portal\",\n");
  fprintf(stderr,
          "        but the session will be ignored unless you run GPU Screen "
          "Recorder with the '-restore-portal-session yes' option.\n");
  fprintf(stderr,
          "        If this is \"screen\" or \"screen-direct-force\" then all "
          "monitors are recorded on Nvidia X11. On AMD/Intel or wayland "
          "\"screen\" will record the first monitor found.\n");
  fprintf(stderr,
          "        \"screen-direct-force\" is not recommended unless you use a "
          "VRR (G-SYNC) monitor on Nvidia X11 and you are aware that using "
          "this option can cause games to freeze/crash or other issues because "
          "of Nvidia driver issues.\n");
  fprintf(stderr,
          "        \"screen-direct-force\" option is only available on Nvidia "
          "X11. VRR works without this option on other systems.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -c    Container format for output file, for example mp4, "
                  "or flv. Only required if no output file is specified or if "
                  "recording in replay buffer mode.\n");
  fprintf(
      stderr,
      "        If an output file is specified and -c is not used then the "
      "container format is determined from the output filename extension.\n");
  fprintf(stderr, "        Only containers that support h264, hevc, av1, vp8 "
                  "or vp9 are supported, which means that only mp4, mkv, flv, "
                  "webm (and some others) are supported.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -s    The size (area) to record at in the format WxH, for "
                  "example 1920x1080. This option is only supported (and "
                  "required) when -w is \"focused\".\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -f    Frame rate to record at. Recording will only "
                  "capture frames at this target frame rate.\n");
  fprintf(stderr,
          "        For constant frame rate mode this option is the frame rate "
          "every frame will be captured at and if the capture frame rate is "
          "below this target frame rate then the frames will be duplicated.\n");
  fprintf(stderr, "        For variable frame rate mode this option is the max "
                  "frame rate and if the capture frame rate is below this "
                  "target frame rate then frames will not be duplicated.\n");
  fprintf(stderr, "        Content frame rate is similar to variable frame "
                  "rate mode, except the frame rate will match the frame rate "
                  "of the captured content when possible, but not capturing "
                  "above the frame rate set in this -f option.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -a    Audio device to record from (pulse audio device). Can be "
          "specified multiple times. Each time this is specified a new audio "
          "track is added for the specified audio device.\n");
  fprintf(stderr,
          "        A name can be given to the audio input device by prefixing "
          "the audio input with <name>/, for example "
          "\"dummy/alsa_output.pci-0000_00_1b.0.analog-stereo.monitor\".\n");
  fprintf(stderr, "        Multiple audio devices can be merged into one audio "
                  "track by using \"|\" as a separator into one -a argument, "
                  "for example: -a \"alsa_output1|alsa_output2\".\n");
  fprintf(stderr,
          "        The audio device can also be \"default_output\" in which "
          "case the default output device is used, or \"default_input\" in "
          "which case the default input device is used.\n");
  fprintf(stderr, "        If the audio device is an empty string then the "
                  "audio device is ignored.\n");
  fprintf(stderr, "        Optional, no audio track is added by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -q    Video quality. Should be either 'medium', 'high', "
                  "'very_high' or 'ultra'. 'high' is the recommended option "
                  "when live streaming or when you have a slower harddrive.\n");
  fprintf(stderr, "        Optional, set to 'very_high' be default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -r    Replay buffer size in seconds. If this is set, then only "
          "the last seconds as set by this option will be stored\n");
  fprintf(stderr, "        and the video will only be saved when the "
                  "gpu-screen-recorder is closed. This feature is similar to "
                  "Nvidia's instant replay feature.\n");
  fprintf(stderr, "        This option has be between 5 and 1200. Note that "
                  "the replay buffer size will not always be precise, because "
                  "of keyframes. Optional, disabled by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -k    Video codec to use. Should be either 'auto', "
                  "'h264', 'hevc', 'av1', 'vp8', 'vp9', 'hevc_hdr', 'av1_hdr', "
                  "'hevc_10bit' or 'av1_10bit'. Optional, set to 'auto' by "
                  "default which defaults to 'h264'.\n");
  fprintf(stderr, "        Forcefully set to 'h264' if the file container type "
                  "is 'flv'.\n");
  fprintf(stderr, "        'hevc_hdr' and 'av1_hdr' option is not available on "
                  "X11 nor when using the portal capture option.\n");
  fprintf(
      stderr,
      "        'hevc_10bit' and 'av1_10bit' options allow you to select 10 bit "
      "color depth which can reduce banding and improve quality in darker "
      "areas, but not all video players support 10 bit color depth\n");
  fprintf(stderr, "        and if you upload the video to a website the "
                  "website might reduce 10 bit to 8 bit.\n");
  fprintf(stderr, "        Note that when using 'hevc_hdr' or 'av1_hdr' the "
                  "color depth is also 10 bits.\n");
  fprintf(stderr, "\n");
  fprintf(
      stderr,
      "  -ac   Audio codec to use. Should be either 'aac', 'opus' or 'flac'. "
      "Optional, set to 'opus' for .mp4/.mkv files, otherwise set to 'aac'.\n");
  fprintf(
      stderr,
      "        'opus' and 'flac' is only supported by .mp4/.mkv files. 'opus' "
      "is recommended for best performance and smallest audio size.\n");
  fprintf(stderr, "        Flac audio codec is option is disable at the moment "
                  "because of a temporary issue.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -ab   Audio bitrate to use. If this is set to 0 then it's "
                  "the same as if it's absent, in which case the bitrate is "
                  "determined automatically depending on the audio codec.\n");
  fprintf(stderr, "        Optional, by default the bitrate is 128000 for opus "
                  "and flac and 160000 for aac.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -oc   Overclock memory transfer rate to the maximum performance "
          "level. This only applies to NVIDIA on X11 and exists to overcome a "
          "bug in NVIDIA driver where performance level\n");
  fprintf(stderr,
          "        is dropped when you record a game. Only needed if you are "
          "recording a game that is bottlenecked by GPU. The same issue exists "
          "on Wayland but overclocking is not possible on Wayland.\n");
  fprintf(stderr,
          "        Works only if your have \"Coolbits\" set to \"12\" in "
          "NVIDIA X settings, see README for more information. Note! use at "
          "your own risk! Optional, disabled by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -fm   Framerate mode. Should be either 'cfr' (constant "
                  "frame rate), 'vfr' (variable frame rate) or 'content'. "
                  "Optional, set to 'vfr' by default.\n");
  fprintf(stderr, "        'vfr' is recommended for recording for less issue "
                  "with very high system load but some applications such as "
                  "video editors may not support it properly.\n");
  fprintf(stderr,
          "        'content' is currently only supported on X11 or when using "
          "portal capture option. The 'content' option matches the recording "
          "frame rate to the captured content.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -bm   Bitrate mode. Should be either 'auto', 'qp' (constant "
          "quality) or 'vbr' (variable bitrate). Optional, set to 'auto' by "
          "default which defaults to 'qp' on all devices\n");
  fprintf(stderr, "        except steam deck that has broken drivers and "
                  "doesn't support qp.\n");
  fprintf(stderr, "        'vbr' option is not supported when using '-encoder "
                  "cpu' option.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -cr   Color range. Should be either 'limited' (aka mpeg) or "
          "'full' (aka jpeg). Optional, set to 'limited' by default.\n");
  fprintf(stderr, "        Limited color range means that colors are in range "
                  "16-235 (4112-60395 for hdr) while full color range means "
                  "that colors are in range 0-255 (0-65535 for hdr).\n");
  fprintf(stderr, "        Note that some buggy video players (such as vlc) "
                  "are unable to correctly display videos in full color range "
                  "and when upload the video to websites the website\n");
  fprintf(stderr, "        might re-encoder the video to make the video "
                  "limited color range.\n");
  fprintf(stderr, "\n");
  fprintf(stderr,
          "  -df   Organise replays in folders based on the current date.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -sc   Run a script on the saved video file "
                  "(asynchronously). The first argument to the script is the "
                  "filepath to the saved video file and the second argument is "
                  "the recording type (either \"regular\" or \"replay\").\n");
  fprintf(stderr, "        Not applicable for live streams.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -cursor\n");
  fprintf(stderr,
          "        Record cursor. Optional, set to 'yes' by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -keyint\n");
  fprintf(stderr, "        Specifies the keyframe interval in seconds, the max "
                  "amount of time to wait to generate a keyframe. Keyframes "
                  "can be generated more often than this.\n");
  fprintf(stderr,
          "        This also affects seeking in the video and may affect how "
          "the replay video is cut. If this is set to 10 for example then you "
          "can only seek in 10-second chunks in the video.\n");
  fprintf(stderr,
          "        Setting this to a higher value reduces the video file size "
          "if you are ok with the previously described downside. This option "
          "is expected to be a floating point number.\n");
  fprintf(stderr, "        By default this value is set to 2.0.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -restore-portal-session\n");
  fprintf(stderr, "        If GPU Screen Recorder should use the same capture "
                  "option as the last time. Using this option removes the "
                  "popup asking what you want to record the next time you "
                  "record with '-w portal' if you selected the option to save "
                  "session (token) in the desktop portal screencast popup.\n");
  fprintf(stderr,
          "        This option may not have any effect on your Wayland "
          "compositor and your systems desktop portal needs to support "
          "ScreenCast version 5 or later. Optional, set to 'no' by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -portal-session-token-filepath\n");
  fprintf(stderr, "        This option is used together with "
                  "-restore-portal-session option to specify the file path to "
                  "save/restore the portal session token to/from.\n");
  fprintf(
      stderr,
      "        This can be used to remember different portal capture options "
      "depending on different recording option (such as recording/replay).\n");
  fprintf(stderr,
          "        Optional, set to "
          "\"$XDG_CONFIG_HOME/gpu-screen-recorder/restore_token\" by default "
          "($XDG_CONFIG_HOME defaults to \"$HOME/.config\").\n");
  fprintf(stderr, "        Note: the directory to the portal session token "
                  "file is created automatically if it doesn't exist.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -encoder\n");
  fprintf(stderr, "        Which device should be used for video encoding. "
                  "Should either be 'gpu' or 'cpu'. Does currently only work "
                  "with h264 codec option (-k).\n");
  fprintf(stderr, "        Optional, set to 'gpu' by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  --info\n");
  fprintf(
      stderr,
      "        List info about the system (for use by GPU Screen Recorder UI). "
      "Lists the following information (prints them to stdout and exits):\n");
  fprintf(
      stderr,
      "        Supported video codecs (h264, h264_software, hevc, hevc_hdr, "
      "hevc_10bit, av1, av1_hdr, av1_10bit, vp8, vp9, (if supported)).\n");
  fprintf(stderr, "        Supported capture options (window, focused, screen, "
                  "monitors and portal, if supported by the system).\n");
  fprintf(stderr, "        If opengl initialization fails then the program "
                  "exits with 22, if no usable drm device is found then it "
                  "exits with 23. On success it exits with 0.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  --list-audio-devices\n");
  fprintf(stderr, "        List audio devices (for use by GPU Screen Recorder "
                  "UI). Lists audio devices in the following format (prints "
                  "them to stdout and exits):\n");
  fprintf(stderr,
          "          "
          "<audio_device_name>|<audio_device_name_in_human_readable_format>\n");
  fprintf(stderr, "        For example:\n");
  fprintf(stderr, "          bluez_input.88:C9:E8:66:A2:27|WH-1000XM4\n");
  fprintf(stderr, "        The <audio_device_name> is the name to pass to GPU "
                  "Screen Recorder in a -a option.\n");
  fprintf(stderr, "  --version\n");
  fprintf(stderr, "        Print version (%s) and exit\n", GSR_VERSION);
  fprintf(stderr, "\n");
  // fprintf(stderr, "  -pixfmt  The pixel format to use for the output video.
  // yuv420 is the most common format and is best supported, but the color is
  // compressed, so colors can look washed out and certain colors of text can
  // look bad. Use yuv444 for no color compression, but the video may not work
  // everywhere and it may not work with hardware video decoding. Optional, set
  // to 'yuv420' by default\n");
  fprintf(stderr,
          "  -o    The output file path. If omitted then the encoded data is "
          "sent to stdout. Required in replay mode (when using -r).\n");
  fprintf(
      stderr,
      "        In replay mode this has to be a directory instead of a file.\n");
  fprintf(stderr, "        Note: the directory to the file is created "
                  "automatically if it doesn't already exist.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -v    Prints per second, fps updates. Optional, set to "
                  "'yes' by default.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "  -h, --help\n");
  fprintf(stderr, "        Show this help.\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "NOTES:\n");
  fprintf(stderr,
          "  Send signal SIGINT to gpu-screen-recorder (Ctrl+C, or killall "
          "-SIGINT gpu-screen-recorder) to stop and save the recording. When "
          "in replay mode this stops recording without saving.\n");
  fprintf(stderr,
          "  Send signal SIGUSR1 to gpu-screen-recorder (killall -SIGUSR1 "
          "gpu-screen-recorder) to save a replay (when in replay mode).\n");
  fprintf(stderr,
          "  Send signal SIGUSR2 to gpu-screen-recorder (killall -SIGUSR2 "
          "gpu-screen-recorder) to pause/unpause recording. Only applicable "
          "and useful when recording (not streaming nor replay).\n");
  fprintf(stderr, "\n");
  fprintf(stderr, "EXAMPLES:\n");
  fprintf(
      stderr,
      "  %s -w screen -f 60 -a default_output -o \"$HOME/Videos/video.mp4\"\n",
      program_name);
  fprintf(stderr,
          "  %s -w screen -f 60 -a \"default_output|default_input\" -o "
          "\"$HOME/Videos/video.mp4\"\n",
          program_name);
  fprintf(stderr,
          "  %s -w screen -f 60 -a default_output -c mkv -r 60 -o "
          "\"$HOME/Videos\"\n",
          program_name);
  fprintf(stderr,
          "  %s -w screen -f 60 -a default_output -c mkv -sc script.sh -r 60 "
          "-o \"$HOME/Videos\"\n",
          program_name);
  fprintf(stderr,
          "  %s -w portal -f 60 -a default_output -restore-portal-session yes "
          "-o \"$HOME/Videos/video.mp4\"\n",
          program_name);
  // fprintf(stderr, "  gpu-screen-recorder -w screen -f 60 -q ultra -pixfmt
  // yuv444 -o video.mp4\n");
  _exit(1);
}

static void usage() {
  usage_header();
  _exit(1);
}

static sig_atomic_t running = 1;
static sig_atomic_t save_replay = 0;
static sig_atomic_t toggle_pause = 0;

static void stop_handler(int) { running = 0; }

static void save_replay_handler(int) { save_replay = 1; }

static void toggle_pause_handler(int) { toggle_pause = 1; }

static bool is_hex_num(char c) {
  return (c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f') ||
         (c >= '0' && c <= '9');
}

static bool contains_non_hex_number(const char *str) {
  bool hex_start = false;
  size_t len = strlen(str);
  if (len >= 2 && memcmp(str, "0x", 2) == 0) {
    str += 2;
    len -= 2;
    hex_start = true;
  }

  bool is_hex = false;
  for (size_t i = 0; i < len; ++i) {
    char c = str[i];
    if (c == '\0')
      return false;
    if (!is_hex_num(c))
      return true;
    if ((c >= 'A' && c <= 'F') || (c >= 'a' && c <= 'f'))
      is_hex = true;
  }

  return is_hex && !hex_start;
}

static std::string get_date_str() {
  char str[128];
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(str, sizeof(str) - 1, "%Y-%m-%d_%H-%M-%S", t);
  return str;
}

static std::string get_date_only_str() {
  char str[128];
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(str, sizeof(str) - 1, "%Y-%m-%d", t);
  return str;
}

static std::string get_time_only_str() {
  char str[128];
  time_t now = time(NULL);
  struct tm *t = localtime(&now);
  strftime(str, sizeof(str) - 1, "%H-%M-%S", t);
  return str;
}

static AVStream *create_stream(AVFormatContext *av_format_context,
                               AVCodecContext *codec_context) {
  AVStream *stream = avformat_new_stream(av_format_context, nullptr);
  if (!stream) {
    fprintf(stderr, "Error: Could not allocate stream\n");
    _exit(1);
  }
  stream->id = av_format_context->nb_streams - 1;
  stream->time_base = codec_context->time_base;
  stream->avg_frame_rate = codec_context->framerate;
  return stream;
}

static void run_recording_saved_script_async(const char *script_file,
                                             const char *video_file,
                                             const char *type) {
  char script_file_full[PATH_MAX];
  script_file_full[0] = '\0';
  if (!realpath(script_file, script_file_full)) {
    fprintf(stderr, "Error: script file not found: %s\n", script_file);
    return;
  }

  const char *args[6];
  const bool inside_flatpak = getenv("FLATPAK_ID") != NULL;

  if (inside_flatpak) {
    args[0] = "flatpak-spawn";
    args[1] = "--host";
    args[2] = script_file_full;
    args[3] = video_file;
    args[4] = type;
    args[5] = NULL;
  } else {
    args[0] = script_file_full;
    args[1] = video_file;
    args[2] = type;
    args[3] = NULL;
  }

  pid_t pid = fork();
  if (pid == -1) {
    perror(script_file_full);
    return;
  } else if (pid == 0) { // child
    setsid();
    signal(SIGHUP, SIG_IGN);

    pid_t second_child = fork();
    if (second_child == 0) { // child
      execvp(args[0], (char *const *)args);
      perror(script_file_full);
      _exit(127);
    } else if (second_child != -1) { // parent
      _exit(0);
    }
  } else { // parent
    waitpid(pid, NULL, 0);
  }
}

static double audio_codec_get_desired_delay(AudioCodec audio_codec, int fps) {
  const double fps_inv = 1.0 / (double)fps;
  const double base = 0.01 + 1.0 / 165.0;
  switch (audio_codec) {
  case AudioCodec::OPUS:
    return std::max(0.0, base - fps_inv);
  case AudioCodec::AAC:
    return std::max(0.0, (base + 0.008) * 2.0 - fps_inv);
  case AudioCodec::FLAC:
    // TODO: Test
    return std::max(0.0, base - fps_inv);
  }
  assert(false);
  return std::max(0.0, base - fps_inv);
}

struct AudioDevice {
  SoundDevice sound_device;
  AudioInput audio_input;
  AVFilterContext *src_filter_ctx = nullptr;
  AVFrame *frame = nullptr;
  std::thread
      thread; // TODO: Instead of having a thread for each track, have one
              // thread for all threads and read the data with non-blocking read
};

// TODO: Cleanup
struct AudioTrack {
  AVCodecContext *codec_context = nullptr;
  AVStream *stream = nullptr;

  std::vector<AudioDevice> audio_devices;
  AVFilterGraph *graph = nullptr;
  AVFilterContext *sink = nullptr;
  int stream_index = 0;
  int64_t pts = 0;
};

static bool add_hdr_metadata_to_video_stream(gsr_capture *cap,
                                             AVStream *video_stream) {
  size_t light_metadata_size = 0;
  size_t mastering_display_metadata_size = 0;
  AVContentLightMetadata *light_metadata =
      av_content_light_metadata_alloc(&light_metadata_size);
#if LIBAVUTIL_VERSION_INT < AV_VERSION_INT(59, 37, 100)
  AVMasteringDisplayMetadata *mastering_display_metadata =
      av_mastering_display_metadata_alloc();
  mastering_display_metadata_size = sizeof(*mastering_display_metadata);
#else
  AVMasteringDisplayMetadata *mastering_display_metadata =
      av_mastering_display_metadata_alloc_size(
          &mastering_display_metadata_size);
#endif

  if (!light_metadata || !mastering_display_metadata) {
    if (light_metadata)
      av_freep(light_metadata);

    if (mastering_display_metadata)
      av_freep(mastering_display_metadata);

    return false;
  }

  if (!gsr_capture_set_hdr_metadata(cap, mastering_display_metadata,
                                    light_metadata)) {
    av_freep(light_metadata);
    av_freep(mastering_display_metadata);
    return false;
  }

  // TODO: More error checking

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
  av_stream_add_side_data(video_stream, AV_PKT_DATA_CONTENT_LIGHT_LEVEL,
                          (uint8_t *)light_metadata, light_metadata_size);
#else
  av_packet_side_data_add(&video_stream->codecpar->coded_side_data,
                          &video_stream->codecpar->nb_coded_side_data,
                          AV_PKT_DATA_CONTENT_LIGHT_LEVEL, light_metadata,
                          light_metadata_size, 0);
#endif

#if LIBAVCODEC_VERSION_INT < AV_VERSION_INT(60, 31, 102)
  av_stream_add_side_data(video_stream, AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                          (uint8_t *)mastering_display_metadata,
                          mastering_display_metadata_size);
#else
  av_packet_side_data_add(&video_stream->codecpar->coded_side_data,
                          &video_stream->codecpar->nb_coded_side_data,
                          AV_PKT_DATA_MASTERING_DISPLAY_METADATA,
                          mastering_display_metadata,
                          mastering_display_metadata_size, 0);
#endif

  return true;
}

static std::future<void> save_replay_thread;
static std::vector<std::shared_ptr<PacketData>> save_replay_packets;
static std::string save_replay_output_filepath;

static void save_replay_async(
    AVCodecContext *video_codec_context, int video_stream_index,
    std::vector<AudioTrack> &audio_tracks,
    std::deque<std::shared_ptr<PacketData>> &frame_data_queue,
    bool frames_erased, std::string output_dir, const char *container_format,
    const std::string &file_extension, std::mutex &write_output_mutex,
    bool date_folders, bool hdr, gsr_capture *capture) {
  if (save_replay_thread.valid())
    return;

  size_t start_index = (size_t)-1;
  int64_t video_pts_offset = 0;
  int64_t audio_pts_offset = 0;

  {
    std::lock_guard<std::mutex> lock(write_output_mutex);
    start_index = (size_t)-1;
    for (size_t i = 0; i < frame_data_queue.size(); ++i) {
      const AVPacket &av_packet = frame_data_queue[i]->data;
      if ((av_packet.flags & AV_PKT_FLAG_KEY) &&
          av_packet.stream_index == video_stream_index) {
        start_index = i;
        break;
      }
    }

    if (start_index == (size_t)-1)
      return;

    if (frames_erased) {
      video_pts_offset = frame_data_queue[start_index]->data.pts;

      // Find the next audio packet to use as audio pts offset
      for (size_t i = start_index; i < frame_data_queue.size(); ++i) {
        const AVPacket &av_packet = frame_data_queue[i]->data;
        if (av_packet.stream_index != video_stream_index) {
          audio_pts_offset = av_packet.pts;
          break;
        }
      }
    } else {
      start_index = 0;
    }

    save_replay_packets.resize(frame_data_queue.size());
    for (size_t i = 0; i < frame_data_queue.size(); ++i) {
      save_replay_packets[i] = frame_data_queue[i];
    }
  }

  if (date_folders) {
    std::string output_folder = output_dir + '/' + get_date_only_str();
    create_directory_recursive(&output_folder[0]);
    save_replay_output_filepath =
        output_folder + "/Replay_" + get_time_only_str() + "." + file_extension;
  } else {
    create_directory_recursive(&output_dir[0]);
    save_replay_output_filepath =
        output_dir + "/Replay_" + get_date_str() + "." + file_extension;
  }

  AVFormatContext *av_format_context;
  avformat_alloc_output_context2(&av_format_context, nullptr, container_format,
                                 nullptr);

  AVStream *video_stream =
      create_stream(av_format_context, video_codec_context);
  avcodec_parameters_from_context(video_stream->codecpar, video_codec_context);

  std::unordered_map<int, AudioTrack *> stream_index_to_audio_track_map;
  for (AudioTrack &audio_track : audio_tracks) {
    stream_index_to_audio_track_map[audio_track.stream_index] = &audio_track;
    AVStream *audio_stream =
        create_stream(av_format_context, audio_track.codec_context);
    avcodec_parameters_from_context(audio_stream->codecpar,
                                    audio_track.codec_context);
    audio_track.stream = audio_stream;
  }

  const int open_ret =
      avio_open(&av_format_context->pb, save_replay_output_filepath.c_str(),
                AVIO_FLAG_WRITE);
  if (open_ret < 0) {
    fprintf(stderr,
            "Error: Could not open '%s': %s. Make sure %s is an existing "
            "directory with write access\n",
            save_replay_output_filepath.c_str(), av_error_to_string(open_ret),
            save_replay_output_filepath.c_str());
    return;
  }

  AVDictionary *options = nullptr;
  av_dict_set(&options, "strict", "experimental", 0);

  const int header_write_ret =
      avformat_write_header(av_format_context, &options);
  if (header_write_ret < 0) {
    fprintf(stderr, "Error occurred when writing header to output file: %s\n",
            av_error_to_string(header_write_ret));
    avio_close(av_format_context->pb);
    avformat_free_context(av_format_context);
    av_dict_free(&options);
    return;
  }

  if (hdr)
    add_hdr_metadata_to_video_stream(capture, video_stream);

  save_replay_thread = std::async(
      std::launch::async,
      [video_stream_index, video_stream, start_index, video_pts_offset,
       audio_pts_offset, video_codec_context, &audio_tracks,
       stream_index_to_audio_track_map, av_format_context, options]() mutable {
        for (size_t i = start_index; i < save_replay_packets.size(); ++i) {
          // TODO: Check if successful
          AVPacket av_packet;
          memset(&av_packet, 0, sizeof(av_packet));
          // av_packet_from_data(av_packet, save_replay_packets[i]->data.data,
          // save_replay_packets[i]->data.size);
          av_packet.data = save_replay_packets[i]->data.data;
          av_packet.size = save_replay_packets[i]->data.size;
          av_packet.stream_index = save_replay_packets[i]->data.stream_index;
          av_packet.pts = save_replay_packets[i]->data.pts;
          av_packet.dts = save_replay_packets[i]->data.pts;
          av_packet.flags = save_replay_packets[i]->data.flags;
          // av_packet.duration = save_replay_packets[i]->data.duration;

          AVStream *stream = video_stream;
          AVCodecContext *codec_context = video_codec_context;

          if (av_packet.stream_index == video_stream_index) {
            av_packet.pts -= video_pts_offset;
            av_packet.dts -= video_pts_offset;
          } else {
            AudioTrack *audio_track =
                stream_index_to_audio_track_map[av_packet.stream_index];
            stream = audio_track->stream;
            codec_context = audio_track->codec_context;

            av_packet.pts -= audio_pts_offset;
            av_packet.dts -= audio_pts_offset;
          }

          av_packet.stream_index = stream->index;
          av_packet_rescale_ts(&av_packet, codec_context->time_base,
                               stream->time_base);

          const int ret = av_write_frame(av_format_context, &av_packet);
          if (ret < 0)
            fprintf(stderr,
                    "Error: Failed to write frame index %d to muxer, reason: "
                    "%s (%d)\n",
                    stream->index, av_error_to_string(ret), ret);

          // av_packet_free(&av_packet);
        }

        if (av_write_trailer(av_format_context) != 0)
          fprintf(stderr, "Failed to write trailer\n");

        avio_close(av_format_context->pb);
        avformat_free_context(av_format_context);
        av_dict_free(&options);

        for (AudioTrack &audio_track : audio_tracks) {
          audio_track.stream = nullptr;
        }
      });
}

static void split_string(const std::string &str, char delimiter,
                         std::function<bool(const char *, size_t)> callback) {
  size_t index = 0;
  while (index < str.size()) {
    size_t end_index = str.find(delimiter, index);
    if (end_index == std::string::npos)
      end_index = str.size();

    if (!callback(&str[index], end_index - index))
      break;

    index = end_index + 1;
  }
}

static std::vector<AudioInput> parse_audio_input_arg(const char *str) {
  std::vector<AudioInput> audio_inputs;
  split_string(str, '|', [&audio_inputs](const char *sub, size_t size) {
    AudioInput audio_input;
    audio_input.name.assign(sub, size);
    const size_t index = audio_input.name.find('/');
    if (index != std::string::npos) {
      audio_input.description = audio_input.name.substr(0, index);
      audio_input.name.erase(audio_input.name.begin(),
                             audio_input.name.begin() + index + 1);
    }
    audio_inputs.push_back(std::move(audio_input));
    return true;
  });
  return audio_inputs;
}

// TODO: Does this match all livestreaming cases?
static bool is_livestream_path(const char *str) {
  const int len = strlen(str);
  if ((len >= 7 && memcmp(str, "http://", 7) == 0) ||
      (len >= 8 && memcmp(str, "https://", 8) == 0))
    return true;
  else if ((len >= 7 && memcmp(str, "rtmp://", 7) == 0) ||
           (len >= 8 && memcmp(str, "rtmps://", 8) == 0))
    return true;
  else if ((len >= 7 && memcmp(str, "rtsp://", 7) == 0))
    return true;
  else if ((len >= 6 && memcmp(str, "srt://", 6) == 0))
    return true;
  else if ((len >= 6 && memcmp(str, "tcp://", 6) == 0))
    return true;
  else if ((len >= 6 && memcmp(str, "udp://", 6) == 0))
    return true;
  else
    return false;
}

// TODO: Proper cleanup
static int init_filter_graph(AVCodecContext *audio_codec_context,
                             AVFilterGraph **graph, AVFilterContext **sink,
                             std::vector<AVFilterContext *> &src_filter_ctx,
                             size_t num_sources) {
  char ch_layout[64];
  int err = 0;
  ch_layout[0] = '\0';

  AVFilterGraph *filter_graph = avfilter_graph_alloc();
  if (!filter_graph) {
    fprintf(stderr, "Unable to create filter graph.\n");
    return AVERROR(ENOMEM);
  }

  for (size_t i = 0; i < num_sources; ++i) {
    const AVFilter *abuffer = avfilter_get_by_name("abuffer");
    if (!abuffer) {
      fprintf(stderr, "Could not find the abuffer filter.\n");
      return AVERROR_FILTER_NOT_FOUND;
    }

    AVFilterContext *abuffer_ctx =
        avfilter_graph_alloc_filter(filter_graph, abuffer, NULL);
    if (!abuffer_ctx) {
      fprintf(stderr, "Could not allocate the abuffer instance.\n");
      return AVERROR(ENOMEM);
    }

#if LIBAVCODEC_VERSION_MAJOR < 60
    av_get_channel_layout_string(ch_layout, sizeof(ch_layout), 0,
                                 AV_CH_LAYOUT_STEREO);
#else
    av_channel_layout_describe(&audio_codec_context->ch_layout, ch_layout,
                               sizeof(ch_layout));
#endif
    av_opt_set(abuffer_ctx, "channel_layout", ch_layout,
               AV_OPT_SEARCH_CHILDREN);
    av_opt_set(abuffer_ctx, "sample_fmt",
               av_get_sample_fmt_name(audio_codec_context->sample_fmt),
               AV_OPT_SEARCH_CHILDREN);
    av_opt_set_q(abuffer_ctx, "time_base", audio_codec_context->time_base,
                 AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(abuffer_ctx, "sample_rate", audio_codec_context->sample_rate,
                   AV_OPT_SEARCH_CHILDREN);
    av_opt_set_int(abuffer_ctx, "bit_rate", audio_codec_context->bit_rate,
                   AV_OPT_SEARCH_CHILDREN);

    err = avfilter_init_str(abuffer_ctx, NULL);
    if (err < 0) {
      fprintf(stderr, "Could not initialize the abuffer filter.\n");
      return err;
    }

    src_filter_ctx.push_back(abuffer_ctx);
  }

  const AVFilter *mix_filter = avfilter_get_by_name("amix");
  if (!mix_filter) {
    av_log(NULL, AV_LOG_ERROR, "Could not find the mix filter.\n");
    return AVERROR_FILTER_NOT_FOUND;
  }

  char args[512];
  snprintf(args, sizeof(args), "inputs=%d", (int)num_sources);

  AVFilterContext *mix_ctx;
  err = avfilter_graph_create_filter(&mix_ctx, mix_filter, "amix", args, NULL,
                                     filter_graph);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "Cannot create audio amix filter\n");
    return err;
  }

  const AVFilter *abuffersink = avfilter_get_by_name("abuffersink");
  if (!abuffersink) {
    fprintf(stderr, "Could not find the abuffersink filter.\n");
    return AVERROR_FILTER_NOT_FOUND;
  }

  AVFilterContext *abuffersink_ctx =
      avfilter_graph_alloc_filter(filter_graph, abuffersink, "sink");
  if (!abuffersink_ctx) {
    fprintf(stderr, "Could not allocate the abuffersink instance.\n");
    return AVERROR(ENOMEM);
  }

  err = avfilter_init_str(abuffersink_ctx, NULL);
  if (err < 0) {
    fprintf(stderr, "Could not initialize the abuffersink instance.\n");
    return err;
  }

  err = 0;
  for (size_t i = 0; i < src_filter_ctx.size(); ++i) {
    AVFilterContext *src_ctx = src_filter_ctx[i];
    if (err >= 0)
      err = avfilter_link(src_ctx, 0, mix_ctx, i);
  }
  if (err >= 0)
    err = avfilter_link(mix_ctx, 0, abuffersink_ctx, 0);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error connecting filters\n");
    return err;
  }

  err = avfilter_graph_config(filter_graph, NULL);
  if (err < 0) {
    av_log(NULL, AV_LOG_ERROR, "Error configuring the filter graph\n");
    return err;
  }

  *graph = filter_graph;
  *sink = abuffersink_ctx;

  return 0;
}

static gsr_video_encoder *
create_video_encoder(gsr_egl *egl, bool overclock, gsr_color_depth color_depth,
                     bool use_software_video_encoder) {
  gsr_video_encoder *video_encoder = nullptr;

  if (use_software_video_encoder) {
    gsr_video_encoder_software_params params;
    params.egl = egl;
    params.color_depth = color_depth;
    video_encoder = gsr_video_encoder_software_create(&params);
    return video_encoder;
  }

  switch (egl->gpu_info.vendor) {
  case GSR_GPU_VENDOR_AMD:
  case GSR_GPU_VENDOR_INTEL: {
    gsr_video_encoder_vaapi_params params;
    params.egl = egl;
    params.color_depth = color_depth;
    video_encoder = gsr_video_encoder_vaapi_create(&params);
    break;
  }
  case GSR_GPU_VENDOR_NVIDIA: {
    gsr_video_encoder_cuda_params params;
    params.egl = egl;
    params.overclock = overclock;
    params.color_depth = color_depth;
    video_encoder = gsr_video_encoder_cuda_create(&params);
    break;
  }
  }

  return video_encoder;
}

static void xwayland_check_callback(const gsr_monitor *monitor,
                                    void *userdata) {
  bool *xwayland_found = (bool *)userdata;
  if (monitor->name_len >= 8 && strncmp(monitor->name, "XWAYLAND", 8) == 0)
    *xwayland_found = true;
  else if (memmem(monitor->name, monitor->name_len, "X11", 3))
    *xwayland_found = true;
}

static bool is_xwayland(Display *display) {
  int opcode, event, error;
  if (XQueryExtension(display, "XWAYLAND", &opcode, &event, &error))
    return true;

  bool xwayland_found = false;
  for_each_active_monitor_output_x11_not_cached(
      display, xwayland_check_callback, &xwayland_found);
  return xwayland_found;
}

static bool is_using_prime_run() {
  const char *prime_render_offload = getenv("__NV_PRIME_RENDER_OFFLOAD");
  return prime_render_offload && strcmp(prime_render_offload, "1") == 0;
}

static void disable_prime_run() {
  unsetenv("__NV_PRIME_RENDER_OFFLOAD");
  unsetenv("__NV_PRIME_RENDER_OFFLOAD_PROVIDER");
  unsetenv("__GLX_VENDOR_LIBRARY_NAME");
  unsetenv("__VK_LAYER_NV_optimus");
}

static void list_system_info(bool wayland) {
  printf("display_server|%s\n", wayland ? "wayland" : "x11");
}

static void list_gpu_info(gsr_egl *egl) {
  switch (egl->gpu_info.vendor) {
  case GSR_GPU_VENDOR_AMD:
    printf("vendor|amd\n");
    break;
  case GSR_GPU_VENDOR_INTEL:
    printf("vendor|intel\n");
    break;
  case GSR_GPU_VENDOR_NVIDIA:
    printf("vendor|nvidia\n");
    break;
  }
}

static const AVCodec *get_ffmpeg_video_codec(VideoCodec video_codec,
                                             gsr_gpu_vendor vendor) {
  switch (video_codec) {
  case VideoCodec::H264:
    return avcodec_find_encoder_by_name(
        vendor == GSR_GPU_VENDOR_NVIDIA ? "h264_nvenc" : "h264_vaapi");
  case VideoCodec::HEVC:
  case VideoCodec::HEVC_HDR:
  case VideoCodec::HEVC_10BIT:
    return avcodec_find_encoder_by_name(
        vendor == GSR_GPU_VENDOR_NVIDIA ? "hevc_nvenc" : "hevc_vaapi");
  case VideoCodec::AV1:
  case VideoCodec::AV1_HDR:
  case VideoCodec::AV1_10BIT:
    return avcodec_find_encoder_by_name(
        vendor == GSR_GPU_VENDOR_NVIDIA ? "av1_nvenc" : "av1_vaapi");
  case VideoCodec::VP8:
    return avcodec_find_encoder_by_name(
        vendor == GSR_GPU_VENDOR_NVIDIA ? "vp8_nvenc" : "vp8_vaapi");
  case VideoCodec::VP9:
    return avcodec_find_encoder_by_name(
        vendor == GSR_GPU_VENDOR_NVIDIA ? "vp9_nvenc" : "vp9_vaapi");
  }
  return nullptr;
}

static void set_supported_video_codecs_ffmpeg(
    gsr_supported_video_codecs *supported_video_codecs, gsr_gpu_vendor vendor) {
  if (!get_ffmpeg_video_codec(VideoCodec::H264, vendor)) {
    supported_video_codecs->h264 = false;
  }

  if (!get_ffmpeg_video_codec(VideoCodec::HEVC, vendor)) {
    supported_video_codecs->hevc = false;
    supported_video_codecs->hevc_hdr = false;
    supported_video_codecs->hevc_10bit = false;
  }

  if (!get_ffmpeg_video_codec(VideoCodec::AV1, vendor)) {
    supported_video_codecs->av1 = false;
    supported_video_codecs->av1_hdr = false;
    supported_video_codecs->av1_10bit = false;
  }

  if (!get_ffmpeg_video_codec(VideoCodec::VP8, vendor)) {
    supported_video_codecs->vp8 = false;
  }

  if (!get_ffmpeg_video_codec(VideoCodec::VP9, vendor)) {
    supported_video_codecs->vp9 = false;
  }
}

static void list_supported_video_codecs(gsr_egl *egl, bool wayland) {
  // Dont clean it up on purpose to increase shutdown speed
  gsr_video_encoder *video_encoder =
      create_video_encoder(egl, false, GSR_COLOR_DEPTH_8_BITS, false);
  if (!video_encoder)
    return;

  gsr_supported_video_codecs supported_video_codecs =
      gsr_video_encoder_get_supported_codecs(video_encoder, false);
  set_supported_video_codecs_ffmpeg(&supported_video_codecs,
                                    egl->gpu_info.vendor);

  if (supported_video_codecs.h264)
    puts("h264");
  if (avcodec_find_encoder_by_name("libx264"))
    puts("h264_software");
  if (supported_video_codecs.hevc)
    puts("hevc");
  if (supported_video_codecs.hevc_hdr && wayland)
    puts("hevc_hdr");
  if (supported_video_codecs.hevc_10bit)
    puts("hevc_10bit");
  if (supported_video_codecs.av1)
    puts("av1");
  if (supported_video_codecs.av1_hdr && wayland)
    puts("av1_hdr");
  if (supported_video_codecs.av1_10bit)
    puts("av1_10bit");
  if (supported_video_codecs.vp8)
    puts("vp8");
  if (supported_video_codecs.vp9)
    puts("vp9");
}

static bool monitor_capture_use_drm(gsr_egl *egl, bool wayland) {
  return wayland || egl->gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA;
}

typedef struct {
  bool wayland;
  gsr_egl *egl;
} capture_options_callback;

static void output_monitor_info(const gsr_monitor *monitor, void *userdata) {
  const capture_options_callback *options =
      (capture_options_callback *)userdata;
  if (options->wayland &&
      monitor_capture_use_drm(options->egl, options->wayland)) {
    vec2i monitor_size = monitor->size;
    const gsr_monitor_rotation rot =
        drm_monitor_get_display_server_rotation(options->egl, monitor);
    if (rot == GSR_MONITOR_ROT_90 || rot == GSR_MONITOR_ROT_270)
      std::swap(monitor_size.x, monitor_size.y);
    printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor_size.x,
           monitor_size.y);
  } else {
    printf("%.*s|%dx%d\n", monitor->name_len, monitor->name, monitor->size.x,
           monitor->size.y);
  }
}

static void list_supported_capture_options(gsr_egl *egl, bool wayland) {
  if (!wayland) {
    puts("window");
    puts("focused");
  }

  capture_options_callback options;
  options.wayland = wayland;
  options.egl = egl;
  if (monitor_capture_use_drm(egl, wayland)) {
    const bool is_x11 =
        gsr_egl_get_display_server(egl) == GSR_DISPLAY_SERVER_X11;
    const gsr_connection_type connection_type =
        is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;
    for_each_active_monitor_output(egl, connection_type, output_monitor_info,
                                   &options);
  } else {
    puts("screen"); // All monitors in one, only available on Nvidia X11
    for_each_active_monitor_output(egl, GSR_CONNECTION_X11, output_monitor_info,
                                   &options);
  }

#ifdef GSR_PORTAL
  // Desktop portal capture on x11 doesn't seem to be hardware accelerated
  if (!wayland)
    return;

  gsr_dbus dbus;
  if (!gsr_dbus_init(&dbus, NULL))
    return;

  char *session_handle = NULL;
  if (gsr_dbus_screencast_create_session(&dbus, &session_handle) == 0) {
    free(session_handle);
    puts("portal");
  }
  gsr_dbus_deinit(&dbus);
#endif
}

static void info_command() {
  bool wayland = false;
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    wayland = true;
    fprintf(stderr, "Warning: failed to connect to the X server. Assuming "
                    "wayland is running without Xwayland\n");
  }

  XSetErrorHandler(x11_error_handler);
  XSetIOErrorHandler(x11_io_error_handler);

  if (!wayland)
    wayland = is_xwayland(dpy);

  if (!wayland && is_using_prime_run()) {
    // Disable prime-run and similar options as it doesn't work, the monitor to
    // capture has to be run on the same device. This is fine on wayland since
    // nvidia uses drm interface there and the monitor query checks the monitors
    // connected to the drm device.
    fprintf(stderr, "Warning: use of prime-run on X11 is not supported. "
                    "Disabling prime-run\n");
    disable_prime_run();
  }

  gsr_egl egl;
  if (!gsr_egl_load(&egl, dpy, wayland, false)) {
    fprintf(stderr, "gsr error: failed to load opengl\n");
    _exit(22);
  }

  egl.card_path[0] = '\0';
  if (monitor_capture_use_drm(&egl, wayland)) {
    // TODO: Allow specifying another card, and in other places
    if (!gsr_get_valid_card_path(&egl, egl.card_path, false)) {
      fprintf(stderr, "Error: no /dev/dri/cardX device found. Make sure that "
                      "you have at least one monitor connected\n");
      _exit(23);
    }
  }

  av_log_set_level(AV_LOG_FATAL);

  puts("section=system_info");
  list_system_info(wayland);
  if (egl.gpu_info.is_steam_deck)
    puts("is_steam_deck|yes");
  else
    puts("is_steam_deck|no");
  puts("section=gpu_info");
  list_gpu_info(&egl);
  puts("section=video_codecs");
  list_supported_video_codecs(&egl, wayland);
  puts("section=capture_options");
  list_supported_capture_options(&egl, wayland);

  fflush(stdout);

  // Not needed as this will just slow down shutdown
  // gsr_egl_unload(&egl);
  // if(dpy)
  //    XCloseDisplay(dpy);

  _exit(0);
}

static void list_audio_devices_command() {
  const AudioDevices audio_devices = get_pulseaudio_inputs();

  if (!audio_devices.default_output.empty())
    puts("default_output|Default output");

  if (!audio_devices.default_input.empty())
    puts("default_input|Default input");

  for (const auto &audio_input : audio_devices.audio_inputs) {
    printf("%s|%s\n", audio_input.name.c_str(),
           audio_input.description.c_str());
  }

  fflush(stdout);
  _exit(0);
}

static gsr_capture *create_capture_impl(
    std::string &window_str, const char *screen_region, bool wayland,
    gsr_egl *egl, int fps, VideoCodec video_codec, gsr_color_range color_range,
    bool record_cursor, bool use_software_video_encoder,
    bool restore_portal_session, const char *portal_session_token_filepath,
    gsr_color_depth color_depth) {
  vec2i region_size = {0, 0};
  Window src_window_id = None;
  bool follow_focused = false;

  gsr_capture *capture = nullptr;
  if (strcmp(window_str.c_str(), "focused") == 0) {
    if (wayland) {
      fprintf(stderr, "Error: GPU Screen Recorder window capture only works in "
                      "a pure X11 session. Xwayland is not supported. You can "
                      "record a monitor instead on wayland\n");
      _exit(2);
    }

    if (!screen_region) {
      fprintf(stderr, "Error: option -s is required when using -w focused\n");
      usage();
    }

    if (sscanf(screen_region, "%dx%d", &region_size.x, &region_size.y) != 2) {
      fprintf(stderr,
              "Error: invalid value for option -s '%s', expected a value in "
              "format WxH\n",
              screen_region);
      usage();
    }

    if (region_size.x <= 0 || region_size.y <= 0) {
      fprintf(stderr,
              "Error: invalud value for option -s '%s', expected width and "
              "height to be greater than 0\n",
              screen_region);
      usage();
    }

    follow_focused = true;
  } else if (strcmp(window_str.c_str(), "portal") == 0) {
#ifdef GSR_PORTAL
    // Desktop portal capture on x11 doesn't seem to be hardware accelerated
    if (!wayland) {
      fprintf(stderr,
              "Error: desktop portal capture is not supported on X11\n");
      _exit(1);
    }

    gsr_capture_portal_params portal_params;
    portal_params.egl = egl;
    portal_params.color_depth = color_depth;
    portal_params.color_range = color_range;
    portal_params.record_cursor = record_cursor;
    portal_params.restore_portal_session = restore_portal_session;
    portal_params.portal_session_token_filepath = portal_session_token_filepath;
    capture = gsr_capture_portal_create(&portal_params);
    if (!capture)
      _exit(1);
#else
    fprintf(stderr, "Error: option '-w portal' used but GPU Screen Recorder "
                    "was compiled without desktop portal support\n");
    _exit(2);
#endif
  } else if (contains_non_hex_number(window_str.c_str())) {
    if (monitor_capture_use_drm(egl, wayland)) {
      const bool is_x11 =
          gsr_egl_get_display_server(egl) == GSR_DISPLAY_SERVER_X11;
      const gsr_connection_type connection_type =
          is_x11 ? GSR_CONNECTION_X11 : GSR_CONNECTION_DRM;

      if (strcmp(window_str.c_str(), "screen") == 0) {
        FirstOutputCallback first_output;
        first_output.output_name = NULL;
        for_each_active_monitor_output(egl, connection_type, get_first_output,
                                       &first_output);

        if (first_output.output_name) {
          window_str = first_output.output_name;
        } else {
          fprintf(stderr, "Error: no usable output found\n");
          _exit(1);
        }
      } else {
        gsr_monitor gmon;
        if (!get_monitor_by_name(egl, connection_type, window_str.c_str(),
                                 &gmon)) {
          fprintf(stderr,
                  "gsr error: display \"%s\" not found, expected one of:\n",
                  window_str.c_str());
          fprintf(stderr, "    \"screen\"\n");
          for_each_active_monitor_output(egl, connection_type,
                                         monitor_output_callback_print, NULL);
          _exit(1);
        }
      }
    } else {
      if (strcmp(window_str.c_str(), "screen") != 0 &&
          strcmp(window_str.c_str(), "screen-direct") != 0 &&
          strcmp(window_str.c_str(), "screen-direct-force") != 0) {
        gsr_monitor gmon;
        if (!get_monitor_by_name(egl, GSR_CONNECTION_X11, window_str.c_str(),
                                 &gmon)) {
          const int screens_width =
              XWidthOfScreen(DefaultScreenOfDisplay(egl->x11.dpy));
          const int screens_height =
              XWidthOfScreen(DefaultScreenOfDisplay(egl->x11.dpy));
          fprintf(stderr,
                  "gsr error: display \"%s\" not found, expected one of:\n",
                  window_str.c_str());
          fprintf(stderr, "    \"screen\"    (%dx%d+%d+%d)\n", screens_width,
                  screens_height, 0, 0);
          fprintf(stderr, "    \"screen-direct\"    (%dx%d+%d+%d)\n",
                  screens_width, screens_height, 0, 0);
          fprintf(stderr, "    \"screen-direct-force\"    (%dx%d+%d+%d)\n",
                  screens_width, screens_height, 0, 0);
          for_each_active_monitor_output(egl, GSR_CONNECTION_X11,
                                         monitor_output_callback_print, NULL);
          _exit(1);
        }
      }
    }

    if (egl->gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && !wayland) {
      const char *capture_target = window_str.c_str();
      bool direct_capture = strcmp(window_str.c_str(), "screen-direct") == 0;
      if (direct_capture) {
        capture_target = "screen";
        // TODO: Temporary disable direct capture because push model causes
        // stuttering when it's direct capturing. This might be a nvfbc bug.
        // This does not happen when using a compositor.
        direct_capture = false;
        fprintf(stderr, "Warning: screen-direct has temporary been disabled as "
                        "it causes stuttering. This is likely a NvFBC bug. "
                        "Falling back to \"screen\".\n");
      }

      if (strcmp(window_str.c_str(), "screen-direct-force") == 0) {
        direct_capture = true;
        capture_target = "screen";
      }

      gsr_capture_nvfbc_params nvfbc_params;
      nvfbc_params.egl = egl;
      nvfbc_params.display_to_capture = capture_target;
      nvfbc_params.fps = fps;
      nvfbc_params.pos = {0, 0};
      nvfbc_params.size = {0, 0};
      nvfbc_params.direct_capture = direct_capture;
      nvfbc_params.color_depth = color_depth;
      nvfbc_params.color_range = color_range;
      nvfbc_params.record_cursor = record_cursor;
      nvfbc_params.use_software_video_encoder = use_software_video_encoder;
      capture = gsr_capture_nvfbc_create(&nvfbc_params);
      if (!capture)
        _exit(1);
    } else {
      gsr_capture_kms_params kms_params;
      kms_params.egl = egl;
      kms_params.display_to_capture = window_str.c_str();
      kms_params.color_depth = color_depth;
      kms_params.color_range = color_range;
      kms_params.record_cursor = record_cursor;
      kms_params.hdr = video_codec_is_hdr(video_codec);
      kms_params.fps = fps;
      capture = gsr_capture_kms_create(&kms_params);
      if (!capture)
        _exit(1);
    }
  } else {
    if (wayland) {
      fprintf(stderr, "Error: GPU Screen Recorder window capture only works in "
                      "a pure X11 session. Xwayland is not supported. You can "
                      "record a monitor instead on wayland\n");
      _exit(2);
    }

    errno = 0;
    src_window_id = strtol(window_str.c_str(), nullptr, 0);
    if (src_window_id == None || errno == EINVAL) {
      fprintf(stderr, "Invalid window number %s\n", window_str.c_str());
      usage();
    }
  }

  if (!capture) {
    gsr_capture_xcomposite_params xcomposite_params;
    xcomposite_params.egl = egl;
    xcomposite_params.window = src_window_id;
    xcomposite_params.follow_focused = follow_focused;
    xcomposite_params.region_size = region_size;
    xcomposite_params.color_range = color_range;
    xcomposite_params.record_cursor = record_cursor;
    xcomposite_params.color_depth = color_depth;
    capture = gsr_capture_xcomposite_create(&xcomposite_params);
    if (!capture)
      _exit(1);
  }

  return capture;
}

static AVPixelFormat get_pixel_format(gsr_gpu_vendor vendor,
                                      bool use_software_video_encoder) {
  if (use_software_video_encoder) {
    return AV_PIX_FMT_NV12;
  } else {
    return vendor == GSR_GPU_VENDOR_NVIDIA ? AV_PIX_FMT_CUDA : AV_PIX_FMT_VAAPI;
  }
}

struct Arg {
  std::vector<const char *> values;
  bool optional = false;
  bool list = false;

  const char *value() const {
    if (values.empty())
      return nullptr;
    return values.front();
  }
};

// Manually check if the audio inputs we give exist. This is only needed for
// pipewire, not pulseaudio. Pipewire instead DEFAULTS TO THE DEFAULT AUDIO
// INPUT. THAT'S RETARDED. OH, YOU MISSPELLED THE AUDIO INPUT? FUCK YOU
static std::vector<MergedAudioInputs>
parse_audio_inputs(const AudioDevices &audio_devices,
                   const Arg &audio_input_arg, bool &uses_amix) {
  std::vector<MergedAudioInputs> requested_audio_inputs;
  uses_amix = false;

  for (const char *audio_input : audio_input_arg.values) {
    if (!audio_input || audio_input[0] == '\0')
      continue;

    requested_audio_inputs.push_back({parse_audio_input_arg(audio_input)});
    if (requested_audio_inputs.back().audio_inputs.size() > 1)
      uses_amix = true;

    for (AudioInput &request_audio_input :
         requested_audio_inputs.back().audio_inputs) {
      bool match = false;

      if (!audio_devices.default_output.empty() &&
          request_audio_input.name == "default_output") {
        request_audio_input.name = audio_devices.default_output;
        if (request_audio_input.description.empty())
          request_audio_input.description = "gsr-Default output";
        match = true;
      }

      if (!audio_devices.default_input.empty() &&
          request_audio_input.name == "default_input") {
        request_audio_input.name = audio_devices.default_input;
        if (request_audio_input.description.empty())
          request_audio_input.description = "gsr-Default input";
        match = true;
      }

      for (const auto &existing_audio_input : audio_devices.audio_inputs) {
        if (request_audio_input.name == existing_audio_input.name) {
          if (request_audio_input.description.empty())
            request_audio_input.description =
                "gsr-" + existing_audio_input.description;

          match = true;
          break;
        }
      }

      if (!match) {
        fprintf(stderr,
                "Error: Audio input device '%s' is not a valid audio device, "
                "expected one of:\n",
                request_audio_input.name.c_str());
        if (!audio_devices.default_output.empty())
          fprintf(stderr, "    default_output (Default output)\n");
        if (!audio_devices.default_input.empty())
          fprintf(stderr, "    default_input (Default input)\n");
        for (const auto &existing_audio_input : audio_devices.audio_inputs) {
          fprintf(stderr, "    %s (%s)\n", existing_audio_input.name.c_str(),
                  existing_audio_input.description.c_str());
        }
        _exit(2);
      }
    }
  }

  return requested_audio_inputs;
}

static AudioCodec select_audio_codec_with_fallback(
    AudioCodec audio_codec, const std::string &file_extension, bool uses_amix) {
  switch (audio_codec) {
  case AudioCodec::AAC: {
    if (file_extension == "webm") {
      // audio_codec_to_use = "opus";
      audio_codec = AudioCodec::OPUS;
      fprintf(stderr, "Warning: .webm files only support opus audio codec, "
                      "changing audio codec from aac to opus\n");
    }
    break;
  }
  case AudioCodec::OPUS: {
    // TODO: Also check mpegts?
    if (file_extension != "mp4" && file_extension != "mkv" &&
        file_extension != "webm") {
      // audio_codec_to_use = "aac";
      audio_codec = AudioCodec::AAC;
      fprintf(stderr, "Warning: opus audio codec is only supported by .mp4, "
                      ".mkv and .webm files, falling back to aac instead\n");
    }
    break;
  }
  case AudioCodec::FLAC: {
    // TODO: Also check mpegts?
    if (file_extension == "webm") {
      // audio_codec_to_use = "opus";
      audio_codec = AudioCodec::OPUS;
      fprintf(stderr, "Warning: .webm files only support opus audio codec, "
                      "changing audio codec from flac to opus\n");
    } else if (file_extension != "mp4" && file_extension != "mkv") {
      // audio_codec_to_use = "aac";
      audio_codec = AudioCodec::AAC;
      fprintf(stderr, "Warning: flac audio codec is only supported by .mp4 and "
                      ".mkv files, falling back to aac instead\n");
    } else if (uses_amix) {
      // TODO: remove this? is it true anymore?
      // audio_codec_to_use = "opus";
      audio_codec = AudioCodec::OPUS;
      fprintf(stderr, "Warning: flac audio codec is not supported when mixing "
                      "audio sources, falling back to opus instead\n");
    }
    break;
  }
  }
  return audio_codec;
}

static const char *video_codec_to_string(VideoCodec video_codec) {
  switch (video_codec) {
  case VideoCodec::H264:
    return "h264";
  case VideoCodec::HEVC:
    return "hevc";
  case VideoCodec::HEVC_HDR:
    return "hevc_hdr";
  case VideoCodec::HEVC_10BIT:
    return "hevc_10bit";
  case VideoCodec::AV1:
    return "av1";
  case VideoCodec::AV1_HDR:
    return "av1_hdr";
  case VideoCodec::AV1_10BIT:
    return "av1_10bit";
  case VideoCodec::VP8:
    return "vp8";
  case VideoCodec::VP9:
    return "vp9";
  }
  return "";
}

static const AVCodec *pick_video_codec(VideoCodec *video_codec, gsr_egl *egl,
                                       bool use_software_video_encoder,
                                       bool video_codec_auto,
                                       const char *video_codec_to_use,
                                       bool is_flv) {
  // TODO: software encoder for hevc, av1, vp8 and vp9

  gsr_video_encoder *video_encoder = create_video_encoder(
      egl, false, GSR_COLOR_DEPTH_8_BITS, use_software_video_encoder);
  if (!video_encoder) {
    fprintf(stderr, "Error: failed to create video encoder\n");
    _exit(1);
  }

  const gsr_supported_video_codecs supported_video_codecs =
      gsr_video_encoder_get_supported_codecs(video_encoder, true);
  const AVCodec *video_codec_f = nullptr;

  // TODO: Cleanup
  // gsr_video_encoder_destroy

  switch (*video_codec) {
  case VideoCodec::H264: {
    if (use_software_video_encoder)
      video_codec_f = avcodec_find_encoder_by_name("libx264");
    else if (supported_video_codecs.h264)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::HEVC: {
    if (supported_video_codecs.hevc)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::HEVC_HDR: {
    if (supported_video_codecs.hevc_hdr)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::HEVC_10BIT: {
    if (supported_video_codecs.hevc_10bit)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::AV1: {
    if (supported_video_codecs.av1)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::AV1_HDR: {
    if (supported_video_codecs.av1_hdr)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::AV1_10BIT: {
    if (supported_video_codecs.av1_10bit)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::VP8: {
    if (supported_video_codecs.vp8)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  case VideoCodec::VP9: {
    if (supported_video_codecs.vp9)
      video_codec_f =
          get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
    break;
  }
  }

  if (!video_codec_auto && !video_codec_f && !is_flv) {
    switch (*video_codec) {
    case VideoCodec::H264: {
      fprintf(stderr, "Warning: selected video codec h264 is not supported, "
                      "trying hevc instead\n");
      video_codec_to_use = "hevc";
      if (supported_video_codecs.hevc)
        video_codec_f =
            get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
      break;
    }
    case VideoCodec::HEVC:
    case VideoCodec::HEVC_HDR:
    case VideoCodec::HEVC_10BIT: {
      fprintf(stderr, "Warning: selected video codec hevc is not supported, "
                      "trying h264 instead\n");
      video_codec_to_use = "h264";
      *video_codec = VideoCodec::H264;
      if (supported_video_codecs.h264)
        video_codec_f =
            get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
      break;
    }
    case VideoCodec::AV1:
    case VideoCodec::AV1_HDR:
    case VideoCodec::AV1_10BIT: {
      fprintf(stderr, "Warning: selected video codec av1 is not supported, "
                      "trying h264 instead\n");
      video_codec_to_use = "h264";
      *video_codec = VideoCodec::H264;
      if (supported_video_codecs.h264)
        video_codec_f =
            get_ffmpeg_video_codec(*video_codec, egl->gpu_info.vendor);
      break;
    }
    case VideoCodec::VP8:
    case VideoCodec::VP9:
      // TODO: Cant fallback to other codec because webm only supports vp8/vp9
      break;
    }
  }

  (void)video_codec_to_use;

  if (!video_codec_f) {
    const char *video_codec_name = video_codec_to_string(*video_codec);
    fprintf(
        stderr,
        "Error: your gpu does not support '%s' video codec. If you are sure "
        "that your gpu does support '%s' video encoding and you are using an "
        "AMD/Intel GPU,\n"
        "  then make sure you have installed the GPU specific vaapi packages "
        "(intel-media-driver, libva-intel-driver, libva-mesa-driver and "
        "linux-firmware).\n"
        "  It's also possible that your distro has disabled hardware "
        "accelerated video encoding for '%s' video codec.\n"
        "  This may be the case on corporate distros such as Manjaro, Fedora "
        "or OpenSUSE.\n"
        "  You can test this by running 'vainfo | grep VAEntrypointEncSlice' "
        "to see if it matches any H264/HEVC/AV1/VP8/VP9 profile.\n"
        "  On such distros, you need to manually install mesa from source to "
        "enable H264/HEVC hardware acceleration, or use a more user friendly "
        "distro. Alternatively record with AV1 if supported by your GPU.\n"
        "  You can alternatively use the flatpak version of GPU Screen "
        "Recorder (https://flathub.org/apps/com.dec05eba.gpu_screen_recorder) "
        "which bypasses system issues with patented H264/HEVC codecs.\n"
        "  Make sure you have mesa-extra freedesktop runtime installed when "
        "using the flatpak (this should be the default), which can be "
        "installed with this command:\n"
        "  flatpak install --system "
        "org.freedesktop.Platform.GL.default//23.08-extra\n"
        "  If your GPU doesn't support hardware accelerated video encoding "
        "then you can use '-encoder cpu' option to encode with your cpu "
        "instead.\n",
        video_codec_name, video_codec_name, video_codec_name);
    _exit(2);
  }

  return video_codec_f;
}

static const AVCodec *select_video_codec_with_fallback(
    VideoCodec *video_codec, const char *video_codec_to_use,
    const char *file_extension, bool use_software_video_encoder, gsr_egl *egl) {
  const bool video_codec_auto = strcmp(video_codec_to_use, "auto") == 0;
  if (video_codec_auto) {
    if (strcmp(file_extension, "webm") == 0) {
      fprintf(stderr, "Info: using vp8 encoder because a codec was not "
                      "specified and the file extension is .webm\n");
      video_codec_to_use = "vp8";
      *video_codec = VideoCodec::VP8;
    } else {
      fprintf(stderr,
              "Info: using h264 encoder because a codec was not specified\n");
      video_codec_to_use = "h264";
      *video_codec = VideoCodec::H264;
    }
  }

  // TODO: Allow hevc, vp9 and av1 in (enhanced) flv (supported since
  // ffmpeg 6.1)
  const bool is_flv = strcmp(file_extension, "flv") == 0;
  if (is_flv) {
    if (*video_codec != VideoCodec::H264) {
      video_codec_to_use = "h264";
      *video_codec = VideoCodec::H264;
      fprintf(stderr, "Warning: hevc/av1 is not compatible with flv, falling "
                      "back to h264 instead.\n");
    }

    // if(audio_codec != AudioCodec::AAC) {
    //     audio_codec_to_use = "aac";
    //     audio_codec = AudioCodec::AAC;
    //     fprintf(stderr, "Warning: flv only supports aac, falling back to aac
    //     instead.\n");
    // }
  }

  const bool is_hls = strcmp(file_extension, "m3u8") == 0;
  if (is_hls) {
    if (video_codec_is_av1(*video_codec)) {
      video_codec_to_use = "hevc";
      *video_codec = VideoCodec::HEVC;
      fprintf(stderr, "Warning: av1 is not compatible with hls (m3u8), falling "
                      "back to hevc instead.\n");
    }

    // if(audio_codec != AudioCodec::AAC) {
    //     audio_codec_to_use = "aac";
    //     audio_codec = AudioCodec::AAC;
    //     fprintf(stderr, "Warning: hls (m3u8) only supports aac, falling back
    //     to aac instead.\n");
    // }
  }

  if (use_software_video_encoder && *video_codec != VideoCodec::H264) {
    fprintf(stderr, "Error: \"-encoder cpu\" option is currently only "
                    "available when using h264 codec option (-k)\n");
    usage();
  }

  return pick_video_codec(video_codec, egl, use_software_video_encoder,
                          video_codec_auto, video_codec_to_use, is_flv);
}

int main(int argc, char **argv) {
  signal(SIGINT, stop_handler);
  signal(SIGUSR1, save_replay_handler);
  signal(SIGUSR2, toggle_pause_handler);

  // Stop nvidia driver from buffering frames
  setenv("__GL_MaxFramesAllowed", "1", true);
  // If this is set to 1 then cuGraphicsGLRegisterImage will fail for egl
  // context with error: invalid OpenGL or DirectX context, so we overwrite it
  setenv("__GL_THREADED_OPTIMIZATIONS", "0", true);
  // Some people set this to nvidia (for nvdec) or vdpau (for nvidia vdpau),
  // which breaks gpu screen recorder since nvidia doesn't support vaapi and
  // nvidia-vaapi-driver doesn't support encoding yet. Let vaapi find the match
  // vaapi driver instead of forcing a specific one.
  unsetenv("LIBVA_DRIVER_NAME");
  // Some people set this to force all applications to vsync on nvidia, but this
  // makes eglSwapBuffers never return.
  unsetenv("__GL_SYNC_TO_VBLANK");
  // Same as above, but for amd/intel
  unsetenv("vblank_mode");

  if (argc <= 1)
    usage_full();

  if (argc == 2 &&
      (strcmp(argv[1], "-h") == 0 || strcmp(argv[1], "--help") == 0))
    usage_full();

  if (argc == 2 && strcmp(argv[1], "--info") == 0) {
    info_command();
    _exit(0);
  }

  if (argc == 2 && strcmp(argv[1], "--list-audio-devices") == 0) {
    list_audio_devices_command();
    _exit(0);
  }

  if (argc == 2 && strcmp(argv[1], "--version") == 0) {
    puts(GSR_VERSION);
    _exit(0);
  }

  // av_log_set_level(AV_LOG_TRACE);

  std::map<std::string, Arg> args = {
      {"-w", Arg{{}, false, false}},
      {"-c", Arg{{}, true, false}},
      {"-f", Arg{{}, false, false}},
      {"-s", Arg{{}, true, false}},
      {"-a", Arg{{}, true, true}},
      {"-q", Arg{{}, true, false}},
      {"-o", Arg{{}, true, false}},
      {"-r", Arg{{}, true, false}},
      {"-k", Arg{{}, true, false}},
      {"-ac", Arg{{}, true, false}},
      {"-ab", Arg{{}, true, false}},
      {"-oc", Arg{{}, true, false}},
      {"-fm", Arg{{}, true, false}},
      {"-bm", Arg{{}, true, false}},
      {"-pixfmt", Arg{{}, true, false}},
      {"-v", Arg{{}, true, false}},
      {"-mf",
       Arg{{}, true, false}}, // TODO: Remove, this exists for backwards
                              // compatibility. -df should be used instead
      {"-df", Arg{{}, true, false}},
      {"-sc", Arg{{}, true, false}},
      {"-cr", Arg{{}, true, false}},
      {"-cursor", Arg{{}, true, false}},
      {"-keyint", Arg{{}, true, false}},
      {"-restore-portal-session", Arg{{}, true, false}},
      {"-portal-session-token-filepath", Arg{{}, true, false}},
      {"-encoder", Arg{{}, true, false}},
  };

  for (int i = 1; i < argc; i += 2) {
    auto it = args.find(argv[i]);
    if (it == args.end()) {
      fprintf(stderr, "Invalid argument '%s'\n", argv[i]);
      usage();
    }

    if (!it->second.values.empty() && !it->second.list) {
      fprintf(stderr, "Expected argument '%s' to only be specified once\n",
              argv[i]);
      usage();
    }

    if (i + 1 >= argc) {
      fprintf(stderr, "Missing value for argument '%s'\n", argv[i]);
      usage();
    }

    it->second.values.push_back(argv[i + 1]);
  }

  for (auto &it : args) {
    if (!it.second.optional && !it.second.value()) {
      fprintf(stderr, "Missing argument '%s'\n", it.first.c_str());
      usage();
    }
  }

  VideoCodec video_codec = VideoCodec::H264;
  const char *video_codec_to_use = args["-k"].value();
  if (!video_codec_to_use)
    video_codec_to_use = "auto";

  if (strcmp(video_codec_to_use, "h264") == 0) {
    video_codec = VideoCodec::H264;
  } else if (strcmp(video_codec_to_use, "h265") == 0 ||
             strcmp(video_codec_to_use, "hevc") == 0) {
    video_codec = VideoCodec::HEVC;
  } else if (strcmp(video_codec_to_use, "hevc_hdr") == 0) {
    video_codec = VideoCodec::HEVC_HDR;
  } else if (strcmp(video_codec_to_use, "hevc_10bit") == 0) {
    video_codec = VideoCodec::HEVC_10BIT;
  } else if (strcmp(video_codec_to_use, "av1") == 0) {
    video_codec = VideoCodec::AV1;
  } else if (strcmp(video_codec_to_use, "av1_hdr") == 0) {
    video_codec = VideoCodec::AV1_HDR;
  } else if (strcmp(video_codec_to_use, "av1_10bit") == 0) {
    video_codec = VideoCodec::AV1_10BIT;
  } else if (strcmp(video_codec_to_use, "vp8") == 0) {
    video_codec = VideoCodec::VP8;
  } else if (strcmp(video_codec_to_use, "vp9") == 0) {
    video_codec = VideoCodec::VP9;
  } else if (strcmp(video_codec_to_use, "auto") != 0) {
    fprintf(stderr,
            "Error: -k should either be either 'auto', 'h264', 'hevc', 'av1', "
            "'vp8', 'vp9', 'hevc_hdr', 'av1_hdr', 'hevc_10bit' or 'av1_10bit', "
            "got: '%s'\n",
            video_codec_to_use);
    usage();
  }

  AudioCodec audio_codec = AudioCodec::OPUS;
  const char *audio_codec_to_use = args["-ac"].value();
  if (!audio_codec_to_use)
    audio_codec_to_use = "opus";

  if (strcmp(audio_codec_to_use, "aac") == 0) {
    audio_codec = AudioCodec::AAC;
  } else if (strcmp(audio_codec_to_use, "opus") == 0) {
    audio_codec = AudioCodec::OPUS;
  } else if (strcmp(audio_codec_to_use, "flac") == 0) {
    audio_codec = AudioCodec::FLAC;
  } else {
    fprintf(stderr,
            "Error: -ac should either be either 'aac', 'opus' or 'flac', got: "
            "'%s'\n",
            audio_codec_to_use);
    usage();
  }

  if (audio_codec == AudioCodec::FLAC) {
    fprintf(stderr, "Warning: flac audio codec is temporary disabled, using "
                    "opus audio codec instead\n");
    audio_codec_to_use = "opus";
    audio_codec = AudioCodec::OPUS;
  }

  int audio_bitrate = 0;
  const char *audio_bitrate_str = args["-ab"].value();
  if (audio_bitrate_str) {
    if (sscanf(audio_bitrate_str, "%d", &audio_bitrate) != 1) {
      fprintf(stderr, "Error: -ab argument \"%s\" is not an integer\n",
              audio_bitrate_str);
      usage();
    }
  }

  float keyint = 2.0;
  const char *keyint_str = args["-keyint"].value();
  if (keyint_str) {
    if (sscanf(keyint_str, "%f", &keyint) != 1) {
      fprintf(stderr,
              "Error: -keyint argument \"%s\" is not a floating point number\n",
              keyint_str);
      usage();
    }

    if (keyint < 0) {
      fprintf(stderr, "Error: -keyint is expected to be 0 or larger\n");
      usage();
    }
  }

  bool use_software_video_encoder = false;
  const char *encoder_str = args["-encoder"].value();
  if (encoder_str) {
    if (strcmp(encoder_str, "gpu") == 0) {
      use_software_video_encoder = false;
    } else if (strcmp(encoder_str, "cpu") == 0) {
      use_software_video_encoder = true;
    } else {
      fprintf(stderr,
              "Error: -encoder is expected to be 'gpu' or 'cpu', was '%s'\n",
              encoder_str);
      usage();
    }
  }

  bool overclock = false;
  const char *overclock_str = args["-oc"].value();
  if (!overclock_str)
    overclock_str = "no";

  if (strcmp(overclock_str, "yes") == 0) {
    overclock = true;
  } else if (strcmp(overclock_str, "no") == 0) {
    overclock = false;
  } else {
    fprintf(stderr,
            "Error: -oc should either be either 'yes' or 'no', got: '%s'\n",
            overclock_str);
    usage();
  }

  bool verbose = true;
  const char *verbose_str = args["-v"].value();
  if (!verbose_str)
    verbose_str = "yes";

  if (strcmp(verbose_str, "yes") == 0) {
    verbose = true;
  } else if (strcmp(verbose_str, "no") == 0) {
    verbose = false;
  } else {
    fprintf(stderr,
            "Error: -v should either be either 'yes' or 'no', got: '%s'\n",
            verbose_str);
    usage();
  }

  bool record_cursor = true;
  const char *record_cursor_str = args["-cursor"].value();
  if (!record_cursor_str)
    record_cursor_str = "yes";

  if (strcmp(record_cursor_str, "yes") == 0) {
    record_cursor = true;
  } else if (strcmp(record_cursor_str, "no") == 0) {
    record_cursor = false;
  } else {
    fprintf(stderr,
            "Error: -cursor should either be either 'yes' or 'no', got: '%s'\n",
            record_cursor_str);
    usage();
  }

  bool date_folders = false;
  const char *date_folders_str = args["-df"].value();
  if (!date_folders_str) {
    date_folders_str = args["-mf"].value();
    if (date_folders_str)
      fprintf(stderr, "Warning: -mf is deprecated, use -df instead\n");
  }
  if (!date_folders_str)
    date_folders_str = "no";

  if (strcmp(date_folders_str, "yes") == 0) {
    date_folders = true;
  } else if (strcmp(date_folders_str, "no") == 0) {
    date_folders = false;
  } else {
    fprintf(stderr,
            "Error: -df should either be either 'yes' or 'no', got: '%s'\n",
            date_folders_str);
    usage();
  }

  bool restore_portal_session = false;
  const char *restore_portal_session_str =
      args["-restore-portal-session"].value();
  if (!restore_portal_session_str)
    restore_portal_session_str = "no";

  if (strcmp(restore_portal_session_str, "yes") == 0) {
    restore_portal_session = true;
  } else if (strcmp(restore_portal_session_str, "no") == 0) {
    restore_portal_session = false;
  } else {
    fprintf(stderr,
            "Error: -restore-portal-session should either be either 'yes' or "
            "'no', got: '%s'\n",
            restore_portal_session_str);
    usage();
  }

  const char *portal_session_token_filepath =
      args["-portal-session-token-filepath"].value();
  if (portal_session_token_filepath) {
    int len = strlen(portal_session_token_filepath);
    if (len > 0 && portal_session_token_filepath[len - 1] == '/') {
      fprintf(stderr,
              "Error: -portal-session-token-filepath should be a path to a "
              "file but it ends with a /: %s\n",
              portal_session_token_filepath);
      _exit(1);
    }
  }

  const char *recording_saved_script = args["-sc"].value();
  if (recording_saved_script) {
    struct stat buf;
    if (stat(recording_saved_script, &buf) == -1 || !S_ISREG(buf.st_mode)) {
      fprintf(stderr,
              "Error: Script \"%s\" either doesn't exist or it's not a file\n",
              recording_saved_script);
      usage();
    }

    if (!(buf.st_mode & S_IXUSR)) {
      fprintf(stderr, "Error: Script \"%s\" is not executable\n",
              recording_saved_script);
      usage();
    }
  }

  PixelFormat pixel_format = PixelFormat::YUV420;
  const char *pixfmt = args["-pixfmt"].value();
  if (!pixfmt)
    pixfmt = "yuv420";

  if (strcmp(pixfmt, "yuv420") == 0) {
    pixel_format = PixelFormat::YUV420;
  } else if (strcmp(pixfmt, "yuv444") == 0) {
    pixel_format = PixelFormat::YUV444;
  } else {
    fprintf(stderr,
            "Error: -pixfmt should either be either 'yuv420', or 'yuv444', "
            "got: '%s'\n",
            pixfmt);
    usage();
  }

  const Arg &audio_input_arg = args["-a"];
  AudioDevices audio_devices;
  if (!audio_input_arg.values.empty())
    audio_devices = get_pulseaudio_inputs();

  bool uses_amix = false;
  std::vector<MergedAudioInputs> requested_audio_inputs =
      parse_audio_inputs(audio_devices, audio_input_arg, uses_amix);

  const char *container_format = args["-c"].value();
  if (container_format && strcmp(container_format, "mkv") == 0)
    container_format = "matroska";

  int fps = atoi(args["-f"].value());
  if (fps == 0) {
    fprintf(stderr, "Invalid fps argument: %s\n", args["-f"].value());
    _exit(1);
  }
  if (fps < 1)
    fps = 1;

  VideoQuality quality = VideoQuality::VERY_HIGH;
  const char *quality_str = args["-q"].value();
  if (!quality_str)
    quality_str = "very_high";

  if (strcmp(quality_str, "medium") == 0) {
    quality = VideoQuality::MEDIUM;
  } else if (strcmp(quality_str, "high") == 0) {
    quality = VideoQuality::HIGH;
  } else if (strcmp(quality_str, "very_high") == 0) {
    quality = VideoQuality::VERY_HIGH;
  } else if (strcmp(quality_str, "ultra") == 0) {
    quality = VideoQuality::ULTRA;
  } else {
    fprintf(stderr,
            "Error: -q should either be either 'medium', 'high', 'very_high' "
            "or 'ultra', got: '%s'\n",
            quality_str);
    usage();
  }

  int replay_buffer_size_secs = -1;
  const char *replay_buffer_size_secs_str = args["-r"].value();
  if (replay_buffer_size_secs_str) {
    replay_buffer_size_secs = atoi(replay_buffer_size_secs_str);
    if (replay_buffer_size_secs < 5 || replay_buffer_size_secs > 1200) {
      fprintf(stderr,
              "Error: option -r has to be between 5 and 1200, was: %s\n",
              replay_buffer_size_secs_str);
      _exit(1);
    }
    replay_buffer_size_secs +=
        std::ceil(keyint); // Add a few seconds to account of lost packets
                           // because of non-keyframe packets skipped
  }

  std::string window_str = args["-w"].value();
  const bool is_portal_capture = strcmp(window_str.c_str(), "portal") == 0;

  if (!restore_portal_session && is_portal_capture) {
    fprintf(stderr, "gsr info: option '-w portal' was used without "
                    "'-restore-portal-session yes'. The previous screencast "
                    "session will be ignored\n");
  }

  bool wayland = false;
  Display *dpy = XOpenDisplay(nullptr);
  if (!dpy) {
    wayland = true;
    fprintf(stderr, "Warning: failed to connect to the X server. Assuming "
                    "wayland is running without Xwayland\n");
  }

  XSetErrorHandler(x11_error_handler);
  XSetIOErrorHandler(x11_io_error_handler);

  if (!wayland)
    wayland = is_xwayland(dpy);

  if (!wayland && is_using_prime_run()) {
    // Disable prime-run and similar options as it doesn't work, the monitor to
    // capture has to be run on the same device. This is fine on wayland since
    // nvidia uses drm interface there and the monitor query checks the monitors
    // connected to the drm device.
    fprintf(stderr, "Warning: use of prime-run on X11 is not supported. "
                    "Disabling prime-run\n");
    disable_prime_run();
  }

  if (is_portal_capture && is_using_prime_run()) {
    fprintf(stderr, "Warning: use of prime-run with -w portal option is "
                    "currently not supported. Disabling prime-run\n");
    disable_prime_run();
  }

  if (video_codec_is_hdr(video_codec) && !wayland) {
    fprintf(stderr,
            "Error: hdr video codec option %s is not available on X11\n",
            video_codec_to_use);
    _exit(1);
  }

  const bool is_monitor_capture = strcmp(window_str.c_str(), "focused") != 0 &&
                                  !is_portal_capture &&
                                  contains_non_hex_number(window_str.c_str());
  gsr_egl egl;
  if (!gsr_egl_load(&egl, dpy, wayland, is_monitor_capture)) {
    fprintf(stderr, "gsr error: failed to load opengl\n");
    _exit(1);
  }

  if (egl.gpu_info.is_steam_deck) {
    fprintf(stderr, "gsr warning: steam deck has multiple driver issues. One "
                    "of them has been reported here: "
                    "https://github.com/ValveSoftware/SteamOS/issues/1609\n"
                    "If you have issues with GPU Screen Recorder on steam deck "
                    "that you don't have on a desktop computer then report the "
                    "issue to Valve and/or AMD.\n");
  }

  bool very_old_gpu = false;

  if (egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA &&
      egl.gpu_info.gpu_version != 0 && egl.gpu_info.gpu_version < 900) {
    fprintf(stderr, "Info: your gpu appears to be very old (older than maxwell "
                    "architecture). Switching to lower preset\n");
    very_old_gpu = true;
  }

  if (egl.gpu_info.vendor != GSR_GPU_VENDOR_NVIDIA && overclock) {
    fprintf(
        stderr,
        "Info: overclock option has no effect on amd/intel, ignoring option\n");
    overclock = false;
  }

  if (egl.gpu_info.vendor == GSR_GPU_VENDOR_NVIDIA && overclock && wayland) {
    fprintf(stderr, "Info: overclocking is not possible on nvidia on wayland, "
                    "ignoring option\n");
    overclock = false;
  }

  egl.card_path[0] = '\0';
  if (monitor_capture_use_drm(&egl, wayland)) {
    // TODO: Allow specifying another card, and in other places
    if (!gsr_get_valid_card_path(&egl, egl.card_path, is_monitor_capture)) {
      fprintf(stderr,
              "Error: no /dev/dri/cardX device found. Make sure that you have "
              "at least one monitor connected or record a single window "
              "instead on X11 or record with the -w portal option\n");
      _exit(2);
    }
  }

  // if(wayland && is_monitor_capture) {
  //     fprintf(stderr, "gsr warning: it's not possible to sync video to
  //     recorded monitor exactly on wayland when recording a monitor."
  //         " If you experience stutter in the video then record with portal
  //         capture option instead (-w portal) or use X11 instead\n");
  // }

  // TODO: Fix constant framerate not working properly on amd/intel because
  // capture framerate gets locked to the same framerate as game framerate,
  // which doesn't work well when you need to encode multiple duplicate frames
  // (AMD/Intel is slow at encoding!). It also appears to skip audio frames on
  // nvidia wayland? why? that should be fine, but it causes video stuttering
  // because of audio/video sync.
  FramerateMode framerate_mode = FramerateMode::VARIABLE;
  const char *framerate_mode_str = args["-fm"].value();
  if (!framerate_mode_str)
    framerate_mode_str = "vfr";

  if (strcmp(framerate_mode_str, "cfr") == 0) {
    framerate_mode = FramerateMode::CONSTANT;
  } else if (strcmp(framerate_mode_str, "vfr") == 0) {
    framerate_mode = FramerateMode::VARIABLE;
  } else if (strcmp(framerate_mode_str, "content") == 0) {
    framerate_mode = FramerateMode::CONTENT;
  } else {
    fprintf(stderr,
            "Error: -fm should either be either 'cfr', 'vfr' or 'content', "
            "got: '%s'\n",
            framerate_mode_str);
    usage();
  }

  if (framerate_mode == FramerateMode::CONTENT && wayland &&
      !is_portal_capture) {
    fprintf(stderr, "Error: -fm 'content' is currently only supported on X11 "
                    "or when using portal capture option\n");
    usage();
  }

  BitrateMode bitrate_mode = BitrateMode::QP;
  const char *bitrate_mode_str = args["-bm"].value();
  if (!bitrate_mode_str)
    bitrate_mode_str = "auto";

  if (strcmp(bitrate_mode_str, "qp") == 0) {
    bitrate_mode = BitrateMode::QP;
  } else if (strcmp(bitrate_mode_str, "vbr") == 0) {
    bitrate_mode = BitrateMode::VBR;
  } else if (strcmp(bitrate_mode_str, "auto") != 0) {
    fprintf(
        stderr,
        "Error: -bm should either be either 'auto', 'qp', 'vbr', got: '%s'\n",
        bitrate_mode_str);
    usage();
  }

  if (strcmp(bitrate_mode_str, "auto") == 0) {
    // QP is broken on steam deck, see
    // https://github.com/ValveSoftware/SteamOS/issues/1609
    bitrate_mode =
        egl.gpu_info.is_steam_deck ? BitrateMode::VBR : BitrateMode::QP;
  }

  if (use_software_video_encoder && bitrate_mode != BitrateMode::QP) {
    fprintf(stderr,
            "Warning: bitrate mode has been forcefully set to qp because "
            "software encoding option doesn't support vbr option\n");
    bitrate_mode = BitrateMode::QP;
  }

  gsr_color_range color_range = GSR_COLOR_RANGE_LIMITED;
  const char *color_range_str = args["-cr"].value();
  if (!color_range_str)
    color_range_str = "limited";

  if (strcmp(color_range_str, "limited") == 0) {
    color_range = GSR_COLOR_RANGE_LIMITED;
  } else if (strcmp(color_range_str, "full") == 0) {
    color_range = GSR_COLOR_RANGE_FULL;
  } else {
    fprintf(
        stderr,
        "Error: -cr should either be either 'limited' or 'full', got: '%s'\n",
        color_range_str);
    usage();
  }

  const char *screen_region = args["-s"].value();

  if (screen_region && strcmp(window_str.c_str(), "focused") != 0) {
    fprintf(stderr,
            "Error: option -s is only available when using -w focused\n");
    usage();
  }

  bool is_livestream = false;
  const char *filename = args["-o"].value();
  if (filename) {
    is_livestream = is_livestream_path(filename);
    if (is_livestream) {
      if (replay_buffer_size_secs != -1) {
        fprintf(stderr,
                "Error: replay mode is not applicable to live streaming\n");
        _exit(1);
      }
    } else {
      if (replay_buffer_size_secs == -1) {
        char directory_buf[PATH_MAX];
        snprintf(directory_buf, sizeof(directory_buf), "%s", filename);
        char *directory = dirname(directory_buf);
        if (strcmp(directory, ".") != 0 && strcmp(directory, "/") != 0) {
          if (create_directory_recursive(directory) != 0) {
            fprintf(stderr,
                    "Error: failed to create directory for output file: %s\n",
                    filename);
            _exit(1);
          }
        }
      } else {
        if (!container_format) {
          fprintf(stderr,
                  "Error: option -c is required when using option -r\n");
          usage();
        }

        struct stat buf;
        if (stat(filename, &buf) != -1 && !S_ISDIR(buf.st_mode)) {
          fprintf(stderr,
                  "Error: File \"%s\" exists but it's not a directory\n",
                  filename);
          usage();
        }
      }
    }
  } else {
    if (replay_buffer_size_secs == -1) {
      filename = "/dev/stdout";
    } else {
      fprintf(stderr, "Error: Option -o is required when using option -r\n");
      usage();
    }

    if (!container_format) {
      fprintf(stderr,
              "Error: option -c is required when not using option -o\n");
      usage();
    }
  }

  const bool is_output_piped = strcmp(filename, "/dev/stdout") == 0;

  AVFormatContext *av_format_context;
  // The output format is automatically guessed by the file extension
  avformat_alloc_output_context2(&av_format_context, nullptr, container_format,
                                 filename);
  if (!av_format_context) {
    if (container_format) {
      fprintf(stderr,
              "Error: Container format '%s' (argument -c) is not valid\n",
              container_format);
    } else {
      fprintf(stderr,
              "Error: Failed to deduce container format from file extension. "
              "Use the '-c' option to specify container format\n");
      usage();
    }
    _exit(1);
  }

  const AVOutputFormat *output_format = av_format_context->oformat;

  std::string file_extension = output_format->extensions;
  {
    size_t comma_index = file_extension.find(',');
    if (comma_index != std::string::npos)
      file_extension = file_extension.substr(0, comma_index);
  }

  const bool force_no_audio_offset =
      is_livestream || is_output_piped ||
      (file_extension != "mp4" && file_extension != "mkv" &&
       file_extension != "webm");
  const double target_fps = 1.0 / (double)fps;

  if (video_codec_is_hdr(video_codec) && is_portal_capture) {
    fprintf(
        stderr,
        "Warning: portal capture option doesn't support hdr yet (pipewire "
        "doesn't support hdr), the video will be tonemapped from hdr to sdr\n");
    video_codec = hdr_video_codec_to_sdr_video_codec(video_codec);
  }

  audio_codec =
      select_audio_codec_with_fallback(audio_codec, file_extension, uses_amix);
  const AVCodec *video_codec_f = select_video_codec_with_fallback(
      &video_codec, video_codec_to_use, file_extension.c_str(),
      use_software_video_encoder, &egl);

  const gsr_color_depth color_depth = video_codec_to_bit_depth(video_codec);
  gsr_capture *capture = create_capture_impl(
      window_str, screen_region, wayland, &egl, fps, video_codec, color_range,
      record_cursor, use_software_video_encoder, restore_portal_session,
      portal_session_token_filepath, color_depth);

  // (Some?) livestreaming services require at least one audio track to work.
  // If not audio is provided then create one silent audio track.
  if (is_livestream && requested_audio_inputs.empty()) {
    fprintf(stderr, "Info: live streaming but no audio track was added. Adding "
                    "a silent audio track\n");
    MergedAudioInputs mai;
    mai.audio_inputs.push_back({"", "gsr-silent"});
    requested_audio_inputs.push_back(std::move(mai));
  }

  if (is_livestream && recording_saved_script) {
    fprintf(stderr, "Warning: live stream detected, -sc script is ignored\n");
    recording_saved_script = nullptr;
  }

  AVStream *video_stream = nullptr;
  std::vector<AudioTrack> audio_tracks;
  const bool hdr = video_codec_is_hdr(video_codec);
  const bool low_latency_recording = is_livestream || is_output_piped;

  AVCodecContext *video_codec_context = create_video_codec_context(
      get_pixel_format(egl.gpu_info.vendor, use_software_video_encoder),
      quality, fps, video_codec_f, low_latency_recording, egl.gpu_info.vendor,
      framerate_mode, hdr, color_range, keyint, use_software_video_encoder,
      bitrate_mode);
  if (replay_buffer_size_secs == -1)
    video_stream = create_stream(av_format_context, video_codec_context);

  AVFrame *video_frame = av_frame_alloc();
  if (!video_frame) {
    fprintf(stderr, "Error: Failed to allocate video frame\n");
    _exit(1);
  }
  video_frame->format = video_codec_context->pix_fmt;
  video_frame->width = video_codec_context->width;
  video_frame->height = video_codec_context->height;
  video_frame->color_range = video_codec_context->color_range;
  video_frame->color_primaries = video_codec_context->color_primaries;
  video_frame->color_trc = video_codec_context->color_trc;
  video_frame->colorspace = video_codec_context->colorspace;
  video_frame->chroma_location = video_codec_context->chroma_sample_location;

  int capture_result =
      gsr_capture_start(capture, video_codec_context, video_frame);
  if (capture_result != 0) {
    fprintf(stderr, "gsr error: gsr_capture_start failed\n");
    _exit(capture_result);
  }

  gsr_video_encoder *video_encoder = create_video_encoder(
      &egl, overclock, color_depth, use_software_video_encoder);
  if (!video_encoder) {
    fprintf(stderr, "Error: failed to create video encoder\n");
    _exit(1);
  }

  if (!gsr_video_encoder_start(video_encoder, video_codec_context,
                               video_frame)) {
    fprintf(stderr, "Error: failed to start video encoder\n");
    _exit(1);
  }

  gsr_color_conversion_params color_conversion_params;
  memset(&color_conversion_params, 0, sizeof(color_conversion_params));
  color_conversion_params.color_range = color_range;
  color_conversion_params.egl = &egl;
  color_conversion_params.source_color = gsr_capture_get_source_color(capture);
  color_conversion_params.load_external_image_shader =
      gsr_capture_uses_external_image(capture);
  gsr_video_encoder_get_textures(
      video_encoder, color_conversion_params.destination_textures,
      &color_conversion_params.num_destination_textures,
      &color_conversion_params.destination_color);

  gsr_color_conversion color_conversion;
  if (gsr_color_conversion_init(&color_conversion, &color_conversion_params) !=
      0) {
    fprintf(stderr, "gsr error: gsr_capture_kms_setup_vaapi_textures: failed "
                    "to create color conversion\n");
    _exit(1);
  }

  gsr_color_conversion_clear(&color_conversion);

  if (use_software_video_encoder) {
    open_video_software(video_codec_context, quality, pixel_format, hdr,
                        color_depth, bitrate_mode);
  } else {
    open_video_hardware(video_codec_context, quality, very_old_gpu,
                        egl.gpu_info.vendor, pixel_format, hdr, color_depth,
                        bitrate_mode);
  }
  if (video_stream)
    avcodec_parameters_from_context(video_stream->codecpar,
                                    video_codec_context);

  int audio_max_frame_size = 1024;
  int audio_stream_index = VIDEO_STREAM_INDEX + 1;
  for (const MergedAudioInputs &merged_audio_inputs : requested_audio_inputs) {
    const bool use_amix = merged_audio_inputs.audio_inputs.size() > 1;
    AVCodecContext *audio_codec_context =
        create_audio_codec_context(fps, audio_codec, use_amix, audio_bitrate);

    AVStream *audio_stream = nullptr;
    if (replay_buffer_size_secs == -1)
      audio_stream = create_stream(av_format_context, audio_codec_context);

    open_audio(audio_codec_context);
    if (audio_stream)
      avcodec_parameters_from_context(audio_stream->codecpar,
                                      audio_codec_context);

#if LIBAVCODEC_VERSION_MAJOR < 60
    const int num_channels = audio_codec_context->channels;
#else
    const int num_channels = audio_codec_context->ch_layout.nb_channels;
#endif

    // audio_frame->sample_rate = audio_codec_context->sample_rate;

    std::vector<AVFilterContext *> src_filter_ctx;
    AVFilterGraph *graph = nullptr;
    AVFilterContext *sink = nullptr;
    if (use_amix) {
      int err =
          init_filter_graph(audio_codec_context, &graph, &sink, src_filter_ctx,
                            merged_audio_inputs.audio_inputs.size());
      if (err < 0) {
        fprintf(stderr, "Error: failed to create audio filter\n");
        _exit(1);
      }
    }

    // TODO: Cleanup above

    const double audio_fps = (double)audio_codec_context->sample_rate /
                             (double)audio_codec_context->frame_size;
    const double timeout_sec = 1000.0 / audio_fps / 1000.0;

    const double audio_startup_time_seconds =
        force_no_audio_offset
            ? 0
            : audio_codec_get_desired_delay(
                  audio_codec,
                  fps); // * ((double)audio_codec_context->frame_size / 1024.0);
    const double num_audio_frames_shift =
        audio_startup_time_seconds / timeout_sec;

    std::vector<AudioDevice> audio_track_audio_devices;
    for (size_t i = 0; i < merged_audio_inputs.audio_inputs.size(); ++i) {
      auto &audio_input = merged_audio_inputs.audio_inputs[i];
      AVFilterContext *src_ctx = nullptr;
      if (use_amix)
        src_ctx = src_filter_ctx[i];

      AudioDevice audio_device;
      audio_device.audio_input = audio_input;
      audio_device.src_filter_ctx = src_ctx;

      if (audio_input.name.empty()) {
        audio_device.sound_device.handle = NULL;
        audio_device.sound_device.frames = 0;
      } else {
        if (sound_device_get_by_name(
                &audio_device.sound_device, audio_input.name.c_str(),
                audio_input.description.c_str(), num_channels,
                audio_codec_context->frame_size,
                audio_codec_context_get_audio_format(audio_codec_context)) !=
            0) {
          fprintf(stderr, "Error: failed to get \"%s\" sound device\n",
                  audio_input.name.c_str());
          _exit(1);
        }
      }

      audio_device.frame = create_audio_frame(audio_codec_context);
      audio_device.frame->pts =
          -audio_codec_context->frame_size * num_audio_frames_shift;

      audio_track_audio_devices.push_back(std::move(audio_device));
    }

    AudioTrack audio_track;
    audio_track.codec_context = audio_codec_context;
    audio_track.stream = audio_stream;
    audio_track.audio_devices = std::move(audio_track_audio_devices);
    audio_track.graph = graph;
    audio_track.sink = sink;
    audio_track.stream_index = audio_stream_index;
    audio_track.pts = -audio_codec_context->frame_size * num_audio_frames_shift;
    audio_tracks.push_back(std::move(audio_track));
    ++audio_stream_index;

    audio_max_frame_size =
        std::max(audio_max_frame_size, audio_codec_context->frame_size);
  }

  // av_dump_format(av_format_context, 0, filename, 1);

  if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE)) {
    int ret = avio_open(&av_format_context->pb, filename, AVIO_FLAG_WRITE);
    if (ret < 0) {
      fprintf(stderr, "Error: Could not open '%s': %s\n", filename,
              av_error_to_string(ret));
      _exit(1);
    }
  }

  if (replay_buffer_size_secs == -1) {
    AVDictionary *options = nullptr;
    av_dict_set(&options, "strict", "experimental", 0);
    // av_dict_set_int(&av_format_context->metadata, "video_full_range_flag", 1,
    // 0);

    int ret = avformat_write_header(av_format_context, &options);
    if (ret < 0) {
      fprintf(stderr, "Error occurred when writing header to output file: %s\n",
              av_error_to_string(ret));
      _exit(1);
    }

    av_dict_free(&options);
  }

  double fps_start_time = clock_get_monotonic_seconds();
  double frame_timer_start = fps_start_time;
  int fps_counter = 0;
  int damage_fps_counter = 0;

  bool paused = false;
  double paused_time_offset = 0.0;
  double paused_time_start = 0.0;

  std::mutex write_output_mutex;
  std::mutex audio_filter_mutex;

  const double record_start_time = clock_get_monotonic_seconds();
  std::deque<std::shared_ptr<PacketData>> frame_data_queue;
  bool frames_erased = false;

  const size_t audio_buffer_size =
      audio_max_frame_size * 4 * 2; // max 4 bytes/sample, 2 channels
  uint8_t *empty_audio = (uint8_t *)malloc(audio_buffer_size);
  if (!empty_audio) {
    fprintf(stderr, "Error: failed to create empty audio\n");
    _exit(1);
  }
  memset(empty_audio, 0, audio_buffer_size);

  for (AudioTrack &audio_track : audio_tracks) {
    for (AudioDevice &audio_device : audio_track.audio_devices) {
      audio_device.thread = std::thread([&]() mutable {
        const AVSampleFormat sound_device_sample_format =
            audio_format_to_sample_format(audio_codec_context_get_audio_format(
                audio_track.codec_context));
        // TODO: Always do conversion for now. This fixes issue with stuttering
        // audio on pulseaudio with opus + multiple audio sources merged
        const bool needs_audio_conversion =
            true; // audio_track.codec_context->sample_fmt !=
                  // sound_device_sample_format;
        SwrContext *swr = nullptr;
        if (needs_audio_conversion) {
          swr = swr_alloc();
          if (!swr) {
            fprintf(stderr, "Failed to create SwrContext\n");
            _exit(1);
          }
#if LIBAVUTIL_VERSION_MAJOR <= 56
          av_opt_set_channel_layout(swr, "in_channel_layout",
                                    AV_CH_LAYOUT_STEREO, 0);
          av_opt_set_channel_layout(swr, "out_channel_layout",
                                    AV_CH_LAYOUT_STEREO, 0);
#elif LIBAVUTIL_VERSION_MAJOR >= 59
          av_opt_set_chlayout(swr, "in_chlayout",
                              &audio_track.codec_context->ch_layout, 0);
          av_opt_set_chlayout(swr, "out_chlayout",
                              &audio_track.codec_context->ch_layout, 0);
#else
          av_opt_set_chlayout(swr, "in_channel_layout",
                              &audio_track.codec_context->ch_layout, 0);
          av_opt_set_chlayout(swr, "out_channel_layout",
                              &audio_track.codec_context->ch_layout, 0);
#endif
          av_opt_set_int(swr, "in_sample_rate",
                         audio_track.codec_context->sample_rate, 0);
          av_opt_set_int(swr, "out_sample_rate",
                         audio_track.codec_context->sample_rate, 0);
          av_opt_set_sample_fmt(swr, "in_sample_fmt",
                                sound_device_sample_format, 0);
          av_opt_set_sample_fmt(swr, "out_sample_fmt",
                                audio_track.codec_context->sample_fmt, 0);
          swr_init(swr);
        }

        const double audio_fps =
            (double)audio_track.codec_context->sample_rate /
            (double)audio_track.codec_context->frame_size;
        const int64_t timeout_ms = std::round(1000.0 / audio_fps);
        const double timeout_sec = 1000.0 / audio_fps / 1000.0;
        bool first_frame = true;
        int64_t num_received_frames = 0;

        while (running) {
          void *sound_buffer;
          int sound_buffer_size = -1;
          // const double time_before_read_seconds =
          // clock_get_monotonic_seconds();
          if (audio_device.sound_device.handle) {
            // TODO: use this instead of calculating time to read. But this can
            // fluctuate and we dont want to go back in time, also it's 0.0 for
            // some users???
            double latency_seconds = 0.0;
            sound_buffer_size = sound_device_read_next_chunk(
                &audio_device.sound_device, &sound_buffer, timeout_sec * 2.0,
                &latency_seconds);
          }

          const bool got_audio_data = sound_buffer_size >= 0;
          // fprintf(stderr, "got audio data: %s\n", got_audio_data ? "yes" :
          // "no"); const double time_after_read_seconds =
          // clock_get_monotonic_seconds(); const double time_to_read_seconds =
          // time_after_read_seconds - time_before_read_seconds; fprintf(stderr,
          // "time to read: %f, %s, %f\n", time_to_read_seconds, got_audio_data
          // ? "yes" : "no", timeout_sec);
          const double this_audio_frame_time =
              clock_get_monotonic_seconds() - paused_time_offset;

          if (paused) {
            if (!audio_device.sound_device.handle)
              av_usleep(timeout_ms * 1000);

            continue;
          }

          int ret = av_frame_make_writable(audio_device.frame);
          if (ret < 0) {
            fprintf(stderr, "Failed to make audio frame writable\n");
            break;
          }

          // TODO: Is this |received_audio_time| really correct?
          const int64_t num_expected_frames = std::round(
              (this_audio_frame_time - record_start_time) / timeout_sec);
          int64_t num_missing_frames =
              std::max((int64_t)0LL, num_expected_frames - num_received_frames);

          if (got_audio_data)
            num_missing_frames = std::max((int64_t)0LL, num_missing_frames - 1);

          if (!audio_device.sound_device.handle)
            num_missing_frames = std::max((int64_t)1, num_missing_frames);

          // Fucking hell is there a better way to do this? I JUST WANT TO KEEP
          // VIDEO AND AUDIO SYNCED HOLY FUCK I WANT TO KILL MYSELF NOW. THIS
          // PIECE OF SHIT WANTS EMPTY FRAMES OTHERWISE VIDEO PLAYS TOO FAST TO
          // KEEP UP WITH AUDIO OR THE AUDIO PLAYS TOO EARLY. BUT WE CANT USE
          // DELAYS TO GIVE DUMMY DATA BECAUSE PULSEAUDIO MIGHT GIVE AUDIO A BIG
          // DELAYED!!! This garbage is needed because we want to produce
          // constant frame rate videos instead of variable frame rate videos
          // because bad software such as video editing software and VLC do not
          // support variable frame rate software, despite nvidia shadowplay and
          // xbox game bar producing variable frame rate videos. So we have to
          // make sure we produce frames at the same relative rate as the video.
          if ((num_missing_frames >= 1 && got_audio_data) ||
              num_missing_frames >= 5 || !audio_device.sound_device.handle) {
            // TODO:
            // audio_track.frame->data[0] = empty_audio;
            if (first_frame || num_missing_frames >= 5) {
              if (needs_audio_conversion)
                swr_convert(swr, &audio_device.frame->data[0],
                            audio_track.codec_context->frame_size,
                            (const uint8_t **)&empty_audio,
                            audio_track.codec_context->frame_size);
              else
                audio_device.frame->data[0] = empty_audio;
            }
            first_frame = false;

            // TODO: Check if duplicate frame can be saved just by writing it
            // with a different pts instead of sending it again
            std::lock_guard<std::mutex> lock(audio_filter_mutex);
            for (int i = 0; i < num_missing_frames; ++i) {
              if (audio_track.graph) {
                // TODO: av_buffersrc_add_frame
                if (av_buffersrc_write_frame(audio_device.src_filter_ctx,
                                             audio_device.frame) < 0) {
                  fprintf(stderr,
                          "Error: failed to add audio frame to filter\n");
                }
              } else {
                ret = avcodec_send_frame(audio_track.codec_context,
                                         audio_device.frame);
                if (ret >= 0) {
                  // TODO: Move to separate thread because this could write to
                  // network (for example when livestreaming)
                  receive_frames(audio_track.codec_context,
                                 audio_track.stream_index, audio_track.stream,
                                 audio_device.frame->pts, av_format_context,
                                 record_start_time, frame_data_queue,
                                 replay_buffer_size_secs, frames_erased,
                                 write_output_mutex, paused_time_offset);
                } else {
                  fprintf(stderr, "Failed to encode audio!\n");
                }
              }

              audio_device.frame->pts += audio_track.codec_context->frame_size;
              num_received_frames++;
            }
          }

          if (!audio_device.sound_device.handle)
            av_usleep(timeout_ms * 1000);

          if (got_audio_data) {
            // TODO: Instead of converting audio, get float audio from alsa. Or
            // does alsa do conversion internally to get this format?
            if (needs_audio_conversion)
              swr_convert(swr, &audio_device.frame->data[0],
                          audio_track.codec_context->frame_size,
                          (const uint8_t **)&sound_buffer,
                          audio_track.codec_context->frame_size);
            else
              audio_device.frame->data[0] = (uint8_t *)sound_buffer;
            first_frame = false;

            if (audio_track.graph) {
              std::lock_guard<std::mutex> lock(audio_filter_mutex);
              // TODO: av_buffersrc_add_frame
              if (av_buffersrc_write_frame(audio_device.src_filter_ctx,
                                           audio_device.frame) < 0) {
                fprintf(stderr, "Error: failed to add audio frame to filter\n");
              }
            } else {
              ret = avcodec_send_frame(audio_track.codec_context,
                                       audio_device.frame);
              if (ret >= 0) {
                // TODO: Move to separate thread because this could write to
                // network (for example when livestreaming)
                receive_frames(audio_track.codec_context,
                               audio_track.stream_index, audio_track.stream,
                               audio_device.frame->pts, av_format_context,
                               record_start_time, frame_data_queue,
                               replay_buffer_size_secs, frames_erased,
                               write_output_mutex, paused_time_offset);
              } else {
                fprintf(stderr, "Failed to encode audio!\n");
              }
            }

            audio_device.frame->pts += audio_track.codec_context->frame_size;
            num_received_frames++;
          }
        }

        if (swr)
          swr_free(&swr);
      });
    }
  }

  std::thread amix_thread;
  if (uses_amix) {
    amix_thread = std::thread([&]() {
      AVFrame *aframe = av_frame_alloc();
      while (running) {
        {
          std::lock_guard<std::mutex> lock(audio_filter_mutex);
          for (AudioTrack &audio_track : audio_tracks) {
            if (!audio_track.sink)
              continue;

            int err = 0;
            while ((err = av_buffersink_get_frame(audio_track.sink, aframe)) >=
                   0) {
              aframe->pts = audio_track.pts;
              err = avcodec_send_frame(audio_track.codec_context, aframe);
              if (err >= 0) {
                // TODO: Move to separate thread because this could write to
                // network (for example when livestreaming)
                receive_frames(audio_track.codec_context,
                               audio_track.stream_index, audio_track.stream,
                               aframe->pts, av_format_context,
                               record_start_time, frame_data_queue,
                               replay_buffer_size_secs, frames_erased,
                               write_output_mutex, paused_time_offset);
              } else {
                fprintf(stderr, "Failed to encode audio!\n");
              }
              av_frame_unref(aframe);
              audio_track.pts += audio_track.codec_context->frame_size;
            }
          }
        }
        av_usleep(5 * 1000); // 5 milliseconds
      }
      av_frame_free(&aframe);
    });
  }

  // Set update_fps to 24 to test if duplicate/delayed frames cause video/audio
  // desync or too fast/slow video.
  const double update_fps = fps + 190;
  bool should_stop_error = false;

  int64_t video_pts_counter = 0;
  int64_t video_prev_pts = 0;

  bool hdr_metadata_set = false;

  double damage_timeout_seconds =
      framerate_mode == FramerateMode::CONTENT ? 0.5 : 0.1;
  damage_timeout_seconds = std::max(damage_timeout_seconds, target_fps);

  bool use_damage_tracking = false;
  gsr_damage damage;
  memset(&damage, 0, sizeof(damage));
  if (gsr_egl_get_display_server(&egl) == GSR_DISPLAY_SERVER_X11) {
    gsr_damage_init(&damage, &egl, record_cursor);
    use_damage_tracking = true;
  }

  if (is_monitor_capture)
    gsr_damage_set_target_monitor(&damage, window_str.c_str());

  while (running) {
    const double frame_start = clock_get_monotonic_seconds();

    while (gsr_egl_process_event(&egl)) {
      gsr_damage_on_event(&damage, gsr_egl_get_event_data(&egl));
      gsr_capture_on_event(capture, &egl);
    }
    gsr_damage_tick(&damage);
    gsr_capture_tick(capture);

    if (!is_monitor_capture) {
      Window damage_target_window = 0;
      if (capture->get_window_id)
        damage_target_window = capture->get_window_id(capture);

      if (damage_target_window != 0)
        gsr_damage_set_target_window(&damage, damage_target_window);
    }

    should_stop_error = false;
    if (gsr_capture_should_stop(capture, &should_stop_error)) {
      running = 0;
      break;
    }

    bool damaged = false;
    if (use_damage_tracking)
      damaged = gsr_damage_is_damaged(&damage);
    else if (capture->is_damaged)
      damaged = capture->is_damaged(capture);
    else
      damaged = true;

    // TODO: Readd wayland sync warning when removing this
    if (framerate_mode != FramerateMode::CONTENT)
      damaged = true;

    if (damaged)
      ++damage_fps_counter;

    ++fps_counter;
    const double time_now = clock_get_monotonic_seconds();
    const double frame_timer_elapsed = time_now - frame_timer_start;
    const double elapsed = time_now - fps_start_time;
    if (elapsed >= 1.0) {
      if (verbose) {
        fprintf(stderr, "update fps: %d, damage fps: %d\n", fps_counter,
                damage_fps_counter);
      }
      fps_start_time = time_now;
      fps_counter = 0;
      damage_fps_counter = 0;
    }

    double frame_time_overflow = frame_timer_elapsed - target_fps;
    if ((frame_time_overflow >= 0.0 || video_pts_counter == 0) && damaged) {
      gsr_damage_clear(&damage);
      if (capture->clear_damage)
        capture->clear_damage(capture);
      frame_time_overflow =
          std::min(std::max(0.0, frame_time_overflow), target_fps);
      frame_timer_start = time_now - frame_time_overflow;

      const double this_video_frame_time =
          clock_get_monotonic_seconds() - paused_time_offset;
      const int64_t expected_frames =
          std::round((this_video_frame_time - record_start_time) / target_fps);
      const int num_frames =
          framerate_mode == FramerateMode::CONSTANT
              ? std::max((int64_t)0LL, expected_frames - video_pts_counter)
              : 1;

      if (num_frames > 0 && !paused) {
        egl.glClear(0);
        gsr_capture_capture(capture, video_frame, &color_conversion);
        gsr_egl_swap_buffers(&egl);

        gsr_video_encoder_copy_textures_to_frame(video_encoder, video_frame);

        if (hdr && !hdr_metadata_set && replay_buffer_size_secs == -1 &&
            add_hdr_metadata_to_video_stream(capture, video_stream))
          hdr_metadata_set = true;

        // TODO: Check if duplicate frame can be saved just by writing it with a
        // different pts instead of sending it again
        for (int i = 0; i < num_frames; ++i) {
          if (framerate_mode == FramerateMode::CONSTANT) {
            video_frame->pts = video_pts_counter + i;
          } else {
            video_frame->pts = (this_video_frame_time - record_start_time) *
                               (double)AV_TIME_BASE;
            const bool same_pts = video_frame->pts == video_prev_pts;
            video_prev_pts = video_frame->pts;
            if (same_pts)
              continue;
          }

          int ret = avcodec_send_frame(video_codec_context, video_frame);
          if (ret == 0) {
            // TODO: Move to separate thread because this could write to network
            // (for example when livestreaming)
            receive_frames(video_codec_context, VIDEO_STREAM_INDEX,
                           video_stream, video_frame->pts, av_format_context,
                           record_start_time, frame_data_queue,
                           replay_buffer_size_secs, frames_erased,
                           write_output_mutex, paused_time_offset);
          } else {
            fprintf(stderr, "Error: avcodec_send_frame failed, error: %s\n",
                    av_error_to_string(ret));
          }
        }

        video_pts_counter += num_frames;
      }
    }

    if (toggle_pause == 1) {
      const bool new_paused_state = !paused;
      if (new_paused_state) {
        paused_time_start = clock_get_monotonic_seconds();
        fprintf(stderr, "Paused\n");
      } else {
        paused_time_offset +=
            (clock_get_monotonic_seconds() - paused_time_start);
        fprintf(stderr, "Unpaused\n");
      }

      toggle_pause = 0;
      paused = !paused;
    }

    if (save_replay_thread.valid() &&
        save_replay_thread.wait_for(std::chrono::seconds(0)) ==
            std::future_status::ready) {
      save_replay_thread.get();
      puts(save_replay_output_filepath.c_str());
      fflush(stdout);
      if (recording_saved_script)
        run_recording_saved_script_async(recording_saved_script,
                                         save_replay_output_filepath.c_str(),
                                         "replay");
      std::lock_guard<std::mutex> lock(write_output_mutex);
      save_replay_packets.clear();
    }

    if (save_replay == 1 && !save_replay_thread.valid() &&
        replay_buffer_size_secs != -1) {
      save_replay = 0;
      save_replay_async(video_codec_context, VIDEO_STREAM_INDEX, audio_tracks,
                        frame_data_queue, frames_erased, filename,
                        container_format, file_extension, write_output_mutex,
                        date_folders, hdr, capture);
    }

    double frame_end = clock_get_monotonic_seconds();
    double frame_sleep_fps = 1.0 / update_fps;
    double sleep_time = frame_sleep_fps - (frame_end - frame_start);
    if (sleep_time > 0.0)
      av_usleep(sleep_time * 1000.0 * 1000.0);
  }

  running = 0;

  if (save_replay_thread.valid()) {
    save_replay_thread.get();
    puts(save_replay_output_filepath.c_str());
    fflush(stdout);
    if (recording_saved_script)
      run_recording_saved_script_async(recording_saved_script,
                                       save_replay_output_filepath.c_str(),
                                       "replay");
    std::lock_guard<std::mutex> lock(write_output_mutex);
    save_replay_packets.clear();
  }

  for (AudioTrack &audio_track : audio_tracks) {
    for (AudioDevice &audio_device : audio_track.audio_devices) {
      audio_device.thread.join();
      sound_device_close(&audio_device.sound_device);
    }
  }

  if (amix_thread.joinable())
    amix_thread.join();

  if (replay_buffer_size_secs == -1 &&
      av_write_trailer(av_format_context) != 0) {
    fprintf(stderr, "Failed to write trailer\n");
  }

  if (replay_buffer_size_secs == -1 && !(output_format->flags & AVFMT_NOFILE))
    avio_close(av_format_context->pb);

  gsr_damage_deinit(&damage);
  gsr_color_conversion_deinit(&color_conversion);
  gsr_video_encoder_destroy(video_encoder, video_codec_context);
  gsr_capture_destroy(capture, video_codec_context);

  if (replay_buffer_size_secs == -1 && recording_saved_script)
    run_recording_saved_script_async(recording_saved_script, filename,
                                     "regular");

  if (dpy) {
    // TODO: This causes a crash, why? maybe some other library dlclose xlib and
    // that also happened to unload this???
    // XCloseDisplay(dpy);
  }

  // av_frame_free(&video_frame);
  free(empty_audio);
  // We do an _exit here because cuda uses at_exit to do _something_ that causes
  // the program to freeze, but only on some nvidia driver versions on some gpus
  // (RTX?), and _exit exits the program without calling the at_exit registered
  // functions. Cuda (cuvid library in this case) seems to be waiting for a
  // thread that never finishes execution. Maybe this happens because we dont
  // clean up all ffmpeg resources?
  // TODO: Investigate this.
  _exit(should_stop_error ? 3 : 0);
}
