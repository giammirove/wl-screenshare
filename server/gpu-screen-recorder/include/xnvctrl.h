#ifndef GSR_XNVCTRL_H
#define GSR_XNVCTRL_H

#include <stdbool.h>
#include <stdint.h>

#define NV_CTRL_GPU_NVCLOCK_OFFSET                                      409
#define NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET                            410
#define NV_CTRL_GPU_NVCLOCK_OFFSET_ALL_PERFORMANCE_LEVELS               424
#define NV_CTRL_GPU_MEM_TRANSFER_RATE_OFFSET_ALL_PERFORMANCE_LEVELS     425

#define NV_CTRL_TARGET_TYPE_GPU                                         1

#define NV_CTRL_STRING_PERFORMANCE_MODES                                29

typedef struct _XDisplay Display;

typedef struct {
    int type;
    union {
        struct {
            int64_t min;
            int64_t max;
        } range;
        struct {
            unsigned int ints;
        } bits;
    } u;
    unsigned int permissions;
} NVCTRLAttributeValidValuesRec;

typedef struct {
    Display *display;
    void *library;
    
    int (*XNVCTRLQueryExtension)(Display *dpy, int *event_basep, int *error_basep);
    int (*XNVCTRLSetTargetAttributeAndGetStatus)(Display *dpy, int target_type, int target_id, unsigned int display_mask, unsigned int attribute, int value);
    int (*XNVCTRLQueryValidTargetAttributeValues)(Display *dpy, int target_type, int target_id, unsigned int display_mask, unsigned int attribute, NVCTRLAttributeValidValuesRec *values);
    int (*XNVCTRLQueryTargetStringAttribute)(Display *dpy, int target_type, int target_id, unsigned int display_mask, unsigned int attribute, char **ptr);
} gsr_xnvctrl;

bool gsr_xnvctrl_load(gsr_xnvctrl *self, Display *display);
void gsr_xnvctrl_unload(gsr_xnvctrl *self);

#endif /* GSR_XNVCTRL_H */
