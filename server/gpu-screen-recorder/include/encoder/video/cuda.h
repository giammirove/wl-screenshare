#ifndef GSR_ENCODER_VIDEO_CUDA_H
#define GSR_ENCODER_VIDEO_CUDA_H

#include "video.h"

typedef struct gsr_egl gsr_egl;

typedef struct {
    gsr_egl *egl;
    bool overclock;
    gsr_color_depth color_depth;
} gsr_video_encoder_cuda_params;

gsr_video_encoder* gsr_video_encoder_cuda_create(const gsr_video_encoder_cuda_params *params);

#endif /* GSR_ENCODER_VIDEO_CUDA_H */
