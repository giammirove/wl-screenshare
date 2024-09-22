#ifndef GSR_DAMAGE_H
#define GSR_DAMAGE_H

#include "cursor.h"
#include "utils.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct _XDisplay Display;
typedef union _XEvent XEvent;

typedef enum {
    GSR_DAMAGE_TRACK_NONE,
    GSR_DAMAGE_TRACK_WINDOW,
    GSR_DAMAGE_TRACK_MONITOR
} gsr_damage_track_type;

typedef struct {
    gsr_egl *egl;
    bool track_cursor;
    gsr_damage_track_type track_type;

    int damage_event;
    int damage_error;
    uint64_t damage;
    bool damaged;

    int randr_event;
    int randr_error;

    uint64_t window;
    //vec2i window_pos;
    vec2i window_size;

    gsr_cursor cursor; /* Relative to |window| */
    gsr_monitor monitor;
    char monitor_name[32];
} gsr_damage;

bool gsr_damage_init(gsr_damage *self, gsr_egl *egl, bool track_cursor);
void gsr_damage_deinit(gsr_damage *self);

bool gsr_damage_set_target_window(gsr_damage *self, uint64_t window);
bool gsr_damage_set_target_monitor(gsr_damage *self, const char *monitor_name);
void gsr_damage_on_event(gsr_damage *self, XEvent *xev);
void gsr_damage_tick(gsr_damage *self);
/* Also returns true if damage tracking is not available */
bool gsr_damage_is_damaged(gsr_damage *self);
void gsr_damage_clear(gsr_damage *self);

#endif /* GSR_DAMAGE_H */
