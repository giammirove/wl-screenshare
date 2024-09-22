#ifndef GSR_COLOR_CONVERSION_H
#define GSR_COLOR_CONVERSION_H

#include "shader.h"
#include "vec2.h"
#include <stdbool.h>

typedef enum {
    GSR_COLOR_RANGE_LIMITED,
    GSR_COLOR_RANGE_FULL
} gsr_color_range;

typedef enum {
    GSR_COLOR_DEPTH_8_BITS,
    GSR_COLOR_DEPTH_10_BITS
} gsr_color_depth;

typedef enum {
    GSR_SOURCE_COLOR_RGB,
    GSR_SOURCE_COLOR_BGR
} gsr_source_color;

typedef enum {
    GSR_DESTINATION_COLOR_NV12, /* YUV420, BT709, 8-bit */
    GSR_DESTINATION_COLOR_P010  /* YUV420, BT2020, 10-bit */
} gsr_destination_color;

typedef struct {
    int offset;
    int rotation;
} gsr_color_uniforms;

typedef struct {
    gsr_egl *egl;

    gsr_source_color source_color;
    gsr_destination_color destination_color;

    unsigned int destination_textures[2];
    int num_destination_textures;

    gsr_color_range color_range;
    bool load_external_image_shader;
} gsr_color_conversion_params;

typedef struct {
    gsr_color_conversion_params params;
    gsr_color_uniforms uniforms[4];
    gsr_shader shaders[4];

    unsigned int framebuffers[2];

    unsigned int vertex_array_object_id;
    unsigned int vertex_buffer_object_id;
} gsr_color_conversion;

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params);
void gsr_color_conversion_deinit(gsr_color_conversion *self);

void gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i source_pos, vec2i source_size, vec2i texture_pos, vec2i texture_size, float rotation, bool external_texture);
void gsr_color_conversion_clear(gsr_color_conversion *self);

#endif /* GSR_COLOR_CONVERSION_H */
