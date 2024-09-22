#ifndef GSR_DEFS_H
#define GSR_DEFS_H

#include <stdbool.h>

typedef enum {
    GSR_GPU_VENDOR_AMD,
    GSR_GPU_VENDOR_INTEL,
    GSR_GPU_VENDOR_NVIDIA
} gsr_gpu_vendor;

typedef struct {
    gsr_gpu_vendor vendor;
    int gpu_version; /* 0 if unknown */
    bool is_steam_deck;
} gsr_gpu_info;

typedef enum {
    GSR_MONITOR_ROT_0,
    GSR_MONITOR_ROT_90,
    GSR_MONITOR_ROT_180,
    GSR_MONITOR_ROT_270
} gsr_monitor_rotation;

typedef enum {
    GSR_CONNECTION_X11,
    GSR_CONNECTION_WAYLAND,
    GSR_CONNECTION_DRM
} gsr_connection_type;

#endif /* GSR_DEFS_H */
