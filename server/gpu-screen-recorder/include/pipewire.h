#ifndef GSR_PIPEWIRE_H
#define GSR_PIPEWIRE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

#include <spa/utils/hook.h>
#include <spa/param/video/format.h>

#define GSR_PIPEWIRE_MAX_MODIFIERS 1024
#define GSR_PIPEWIRE_NUM_VIDEO_FORMATS 6
#define GSR_PIPEWIRE_DMABUF_MAX_PLANES 4

typedef struct gsr_egl gsr_egl;

typedef struct {
    int major;
    int minor;
    int micro;
} gsr_pipewire_data_version;

typedef struct {
    uint32_t fps_num;
    uint32_t fps_den;
} gsr_pipewire_video_info;

typedef struct {
    int fd;
    uint32_t offset;
    int32_t stride;
} gsr_pipewire_dmabuf_data;

typedef struct {
    int x, y;
    int width, height;
} gsr_pipewire_region;

typedef struct {
    enum spa_video_format format;
    size_t modifiers_index;
    size_t modifiers_size;
} gsr_video_format;

typedef struct {
    unsigned int texture_id;
    unsigned int external_texture_id;
    unsigned int cursor_texture_id;
} gsr_texture_map;

typedef struct {
    gsr_egl *egl;
    int fd;
    uint32_t node;
    pthread_mutex_t mutex;
    bool mutex_initialized;

    struct pw_thread_loop *thread_loop;
    struct pw_context *context;
    struct pw_core *core;
    struct spa_hook core_listener;
    struct pw_stream *stream;
    struct spa_hook stream_listener;
    struct spa_source *reneg;
    struct spa_video_info format;
    int server_version_sync;
    bool negotiated;
    bool damaged;

    struct {
        bool visible;
        bool valid;
        uint8_t *data;
        int x, y;
        int hotspot_x, hotspot_y;
        int width, height;
    } cursor;

    struct {
        bool valid;
        int x, y;
        uint32_t width, height;
    } crop;

    gsr_video_format supported_video_formats[GSR_PIPEWIRE_NUM_VIDEO_FORMATS];

    gsr_pipewire_data_version server_version;
    gsr_pipewire_video_info video_info;
    gsr_pipewire_dmabuf_data dmabuf_data[GSR_PIPEWIRE_DMABUF_MAX_PLANES];
    size_t dmabuf_num_planes;

    bool no_modifiers_fallback;
    bool external_texture_fallback;

    uint64_t modifiers[GSR_PIPEWIRE_MAX_MODIFIERS];
    size_t num_modifiers;
} gsr_pipewire;

/*
    |capture_cursor| only applies to when capturing a window or region.
    In other cases |pipewire_node|'s setup will determine if the cursor is included.
    Note that the cursor is not guaranteed to be shown even if set to true, it depends on the wayland compositor.
*/
bool gsr_pipewire_init(gsr_pipewire *self, int pipewire_fd, uint32_t pipewire_node, int fps, bool capture_cursor, gsr_egl *egl);
void gsr_pipewire_deinit(gsr_pipewire *self);

/* |dmabuf_data| should be at least GSR_PIPEWIRE_DMABUF_MAX_PLANES in size */
bool gsr_pipewire_map_texture(gsr_pipewire *self, gsr_texture_map texture_map, gsr_pipewire_region *region, gsr_pipewire_region *cursor_region, gsr_pipewire_dmabuf_data *dmabuf_data, int *num_dmabuf_data, uint32_t *fourcc, uint64_t *modifiers, bool *using_external_image);
bool gsr_pipewire_is_damaged(gsr_pipewire *self);
void gsr_pipewire_clear_damage(gsr_pipewire *self);

#endif /* GSR_PIPEWIRE_H */
