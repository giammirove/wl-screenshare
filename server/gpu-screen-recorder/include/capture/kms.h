#ifndef GSR_CAPTURE_KMS_H
#define GSR_CAPTURE_KMS_H

#include "capture.h"

typedef struct {
    gsr_egl *egl;
    const char *display_to_capture; /* if this is "screen", then the first monitor is captured. A copy is made of this */
    gsr_color_depth color_depth;
    gsr_color_range color_range;
    bool hdr;
    bool record_cursor;
    int fps;
} gsr_capture_kms_params;

gsr_capture* gsr_capture_kms_create(const gsr_capture_kms_params *params);

#endif /* GSR_CAPTURE_KMS_H */
