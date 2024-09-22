#ifndef GSR_CAPTURE_PORTAL_H
#define GSR_CAPTURE_PORTAL_H

#include "capture.h"

typedef struct {
    gsr_egl *egl;
    gsr_color_depth color_depth;
    gsr_color_range color_range;
    bool record_cursor;
    bool restore_portal_session;
    /* If this is set to NULL then this defaults to $XDG_CONFIG_HOME/gpu-screen-recorder/restore_token ($XDG_CONFIG_HOME defaults to $HOME/.config) */
    const char *portal_session_token_filepath;
} gsr_capture_portal_params;

gsr_capture* gsr_capture_portal_create(const gsr_capture_portal_params *params);

#endif /* GSR_CAPTURE_PORTAL_H */
