#ifndef GSR_CAPTURE_CAPTURE_H
#define GSR_CAPTURE_CAPTURE_H

#include "../color_conversion.h"
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct AVCodecContext AVCodecContext;
typedef struct AVStream AVStream;
typedef struct AVFrame AVFrame;
typedef struct gsr_capture gsr_capture;
typedef struct AVMasteringDisplayMetadata AVMasteringDisplayMetadata;
typedef struct AVContentLightMetadata AVContentLightMetadata;

struct gsr_capture {
    /* These methods should not be called manually. Call gsr_capture_* instead */
    int (*start)(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame);
    void (*on_event)(gsr_capture *cap, gsr_egl *egl); /* can be NULL */
    void (*tick)(gsr_capture *cap); /* can be NULL. If there is an event then |on_event| is called before this */
    bool (*should_stop)(gsr_capture *cap, bool *err); /* can be NULL. If NULL, return false */
    int (*capture)(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion);
    gsr_source_color (*get_source_color)(gsr_capture *cap);
    bool (*uses_external_image)(gsr_capture *cap); /* can be NULL. If NULL, return false */
    bool (*set_hdr_metadata)(gsr_capture *cap, AVMasteringDisplayMetadata *mastering_display_metadata, AVContentLightMetadata *light_metadata); /* can be NULL. If NULL, return false */
    uint64_t (*get_window_id)(gsr_capture *cap); /* can be NULL. Returns 0 if unknown */
    bool (*is_damaged)(gsr_capture *cap); /* can be NULL */
    void (*clear_damage)(gsr_capture *cap); /* can be NULL */
    void (*destroy)(gsr_capture *cap, AVCodecContext *video_codec_context);

    void *priv; /* can be NULL */
    bool started;
};

int gsr_capture_start(gsr_capture *cap, AVCodecContext *video_codec_context, AVFrame *frame);
void gsr_capture_on_event(gsr_capture *cap, gsr_egl *egl);
void gsr_capture_tick(gsr_capture *cap);
bool gsr_capture_should_stop(gsr_capture *cap, bool *err);
int gsr_capture_capture(gsr_capture *cap, AVFrame *frame, gsr_color_conversion *color_conversion);
gsr_source_color gsr_capture_get_source_color(gsr_capture *cap);
bool gsr_capture_uses_external_image(gsr_capture *cap);
bool gsr_capture_set_hdr_metadata(gsr_capture *cap, AVMasteringDisplayMetadata *mastering_display_metadata, AVContentLightMetadata *light_metadata);
void gsr_capture_destroy(gsr_capture *cap, AVCodecContext *video_codec_context);

#endif /* GSR_CAPTURE_CAPTURE_H */
