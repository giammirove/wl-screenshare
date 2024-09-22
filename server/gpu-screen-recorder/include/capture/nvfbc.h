#ifndef GSR_CAPTURE_NVFBC_H
#define GSR_CAPTURE_NVFBC_H

#include "capture.h"
#include "../vec2.h"

typedef struct {
    gsr_egl *egl;
    const char *display_to_capture; /* if this is "screen", then the entire x11 screen is captured (all displays). A copy is made of this */
    int fps;
    vec2i pos;
    vec2i size;
    bool direct_capture;
    gsr_color_depth color_depth;
    gsr_color_range color_range;
    bool record_cursor;
    bool use_software_video_encoder;
} gsr_capture_nvfbc_params;

gsr_capture* gsr_capture_nvfbc_create(const gsr_capture_nvfbc_params *params);

#endif /* GSR_CAPTURE_NVFBC_H */
