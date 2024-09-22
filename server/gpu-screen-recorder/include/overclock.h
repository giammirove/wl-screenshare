#ifndef GSR_OVERCLOCK_H
#define GSR_OVERCLOCK_H

#include "xnvctrl.h"

typedef struct {
    gsr_xnvctrl xnvctrl;
    int num_performance_levels;
} gsr_overclock;

bool gsr_overclock_load(gsr_overclock *self, Display *display);
void gsr_overclock_unload(gsr_overclock *self);

bool gsr_overclock_start(gsr_overclock *self);
void gsr_overclock_stop(gsr_overclock *self);

#endif /* GSR_OVERCLOCK_H */
