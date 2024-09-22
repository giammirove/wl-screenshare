#include "../include/color_conversion.h"
#include "../include/egl.h"
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <assert.h>

/* TODO: highp instead of mediump? */

#define MAX_SHADERS 4
#define MAX_FRAMEBUFFERS 2

static float abs_f(float v) {
    return v >= 0.0f ? v : -v;
}

#define ROTATE_Z   "mat4 rotate_z(in float angle) {\n"                        \
                   "    return mat4(cos(angle), -sin(angle), 0.0, 0.0,\n"     \
                   "                sin(angle),  cos(angle), 0.0, 0.0,\n"     \
                   "                0.0,           0.0,      1.0, 0.0,\n"     \
                   "                0.0,           0.0,      0.0, 1.0);\n"    \
                   "}\n"

/* https://en.wikipedia.org/wiki/YCbCr, see study/color_space_transform_matrix.png */

/* ITU-R BT2020, full */
/* https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.2020-2-201510-I!!PDF-E.pdf */
#define RGB_TO_P010_FULL "const mat4 RGBtoYUV = mat4(0.262700, -0.139630,  0.500000, 0.000000,\n" \
                         "                           0.678000, -0.360370, -0.459786, 0.000000,\n" \
                         "                           0.059300,  0.500000, -0.040214, 0.000000,\n" \
                         "                           0.000000,  0.500000,  0.500000, 1.000000);"

/* ITU-R BT2020, limited (full multiplied by (235-16)/255, adding 16/255 to luma) */
#define RGB_TO_P010_LIMITED "const mat4 RGBtoYUV = mat4(0.225613, -0.119918,  0.429412, 0.000000,\n" \
                            "                           0.582282, -0.309494, -0.394875, 0.000000,\n" \
                            "                           0.050928,  0.429412, -0.034537, 0.000000,\n" \
                            "                           0.062745,  0.500000,  0.500000, 1.000000);"

/* ITU-R BT709, full, custom values: 0.2110 0.7110 0.0710 */
/* https://www.itu.int/dms_pubrec/itu-r/rec/bt/R-REC-BT.709-6-201506-I!!PDF-E.pdf */
#define RGB_TO_NV12_FULL "const mat4 RGBtoYUV = mat4(0.211000, -0.113563,  0.500000, 0.000000,\n" \
                         "                           0.711000, -0.382670, -0.450570, 0.000000,\n" \
                         "                           0.071000,  0.500000, -0.044994, 0.000000,\n" \
                         "                           0.000000,  0.500000,  0.500000, 1.000000);"

/* ITU-R BT709, limited, custom values: 0.2100 0.7100 0.0700 (full multiplied by (235-16)/255, adding 16/255 to luma) */
#define RGB_TO_NV12_LIMITED "const mat4 RGBtoYUV = mat4(0.180353, -0.096964,  0.429412, 0.000000,\n" \
                            "                           0.609765, -0.327830, -0.385927, 0.000000,\n" \
                            "                           0.060118,  0.429412, -0.038049, 0.000000,\n" \
                            "                           0.062745,  0.500000,  0.500000, 1.000000);"

static const char* color_format_range_get_transform_matrix(gsr_destination_color color_format, gsr_color_range color_range) {
    switch(color_format) {
        case GSR_DESTINATION_COLOR_NV12: {
            switch(color_range) {
                case GSR_COLOR_RANGE_LIMITED:
                    return RGB_TO_NV12_LIMITED;
                case GSR_COLOR_RANGE_FULL:
                    return RGB_TO_NV12_FULL;
            }
            break;
        }
        case GSR_DESTINATION_COLOR_P010: {
            switch(color_range) {
                case GSR_COLOR_RANGE_LIMITED:
                    return RGB_TO_P010_LIMITED;
                case GSR_COLOR_RANGE_FULL:
                    return RGB_TO_P010_FULL;
            }
            break;
        }
        default:
            return NULL;
    }
    return NULL;
}

static int load_shader_y(gsr_shader *shader, gsr_egl *egl, gsr_color_uniforms *uniforms, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                   \n"
        "in vec2 pos;                                      \n"
        "in vec2 texcoords;                                \n"
        "out vec2 texcoords_out;                           \n"
        "uniform vec2 offset;                              \n"
        "uniform float rotation;                           \n"
        ROTATE_Z
        "void main()                                       \n"
        "{                                                 \n"
        "  texcoords_out = (vec4(texcoords.x - 0.5, texcoords.y - 0.5, 0.0, 0.0) * rotate_z(rotation)).xy + vec2(0.5, 0.5);  \n"
        "  gl_Position = vec4(offset.x, offset.y, 0.0, 0.0) + vec4(pos.x, pos.y, 0.0, 1.0);    \n"
        "}                                                 \n");

    char fragment_shader[2048];
    if(external_texture) {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                 \n"
            "#extension GL_OES_EGL_image_external : enable                                   \n"
            "#extension GL_OES_EGL_image_external_essl3 : require                            \n"
            "precision mediump float;                                                        \n"
            "in vec2 texcoords_out;                                                          \n"
            "uniform samplerExternalOES tex1;                                                \n"
            "out vec4 FragColor;                                                             \n"
            "%s"
            "void main()                                                                     \n"
            "{                                                                               \n"
            "  vec4 pixel = texture(tex1, texcoords_out);                                    \n"
            "  FragColor.x = (RGBtoYUV * vec4(pixel.rgb, 1.0)).x;                            \n"
            "  FragColor.w = pixel.a;                                                        \n"
            "}                                                                               \n", color_transform_matrix);
    } else {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                 \n"
            "precision mediump float;                                                        \n"
            "in vec2 texcoords_out;                                                          \n"
            "uniform sampler2D tex1;                                                         \n"
            "out vec4 FragColor;                                                             \n"
            "%s"
            "void main()                                                                     \n"
            "{                                                                               \n"
            "  vec4 pixel = texture(tex1, texcoords_out);                                    \n"
            "  FragColor.x = (RGBtoYUV * vec4(pixel.rgb, 1.0)).x;                            \n"
            "  FragColor.w = pixel.a;                                                        \n"
            "}                                                                               \n", color_transform_matrix);
    }

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    uniforms->offset = egl->glGetUniformLocation(shader->program_id, "offset");
    uniforms->rotation = egl->glGetUniformLocation(shader->program_id, "rotation");
    return 0;
}

static unsigned int load_shader_uv(gsr_shader *shader, gsr_egl *egl, gsr_color_uniforms *uniforms, gsr_destination_color color_format, gsr_color_range color_range, bool external_texture) {
    const char *color_transform_matrix = color_format_range_get_transform_matrix(color_format, color_range);

    char vertex_shader[2048];
    snprintf(vertex_shader, sizeof(vertex_shader),
        "#version 300 es                                 \n"
        "in vec2 pos;                                    \n"
        "in vec2 texcoords;                              \n"
        "out vec2 texcoords_out;                         \n"
        "uniform vec2 offset;                              \n"
        "uniform float rotation;                         \n"
        ROTATE_Z
        "void main()                                     \n"
        "{                                               \n"
        "  texcoords_out = (vec4(texcoords.x - 0.5, texcoords.y - 0.5, 0.0, 0.0) * rotate_z(rotation)).xy + vec2(0.5, 0.5);                      \n"
        "  gl_Position = (vec4(offset.x, offset.y, 0.0, 0.0) + vec4(pos.x, pos.y, 0.0, 1.0)) * vec4(0.5, 0.5, 1.0, 1.0) - vec4(0.5, 0.5, 0.0, 0.0);   \n"
        "}                                               \n");

    char fragment_shader[2048];
    if(external_texture) {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "#extension GL_OES_EGL_image_external : enable                                         \n"
            "#extension GL_OES_EGL_image_external_essl3 : require                                  \n"
            "precision mediump float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform samplerExternalOES tex1;                                                      \n"
            "out vec4 FragColor;                                                                   \n"
            "%s"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "  vec4 pixel = texture(tex1, texcoords_out);                                          \n"
            "  FragColor.xy = (RGBtoYUV * vec4(pixel.rgb, 1.0)).yz;                                \n"
            "  FragColor.w = pixel.a;                                                              \n"
            "}                                                                                     \n", color_transform_matrix);
    } else {
        snprintf(fragment_shader, sizeof(fragment_shader),
            "#version 300 es                                                                       \n"
            "precision mediump float;                                                              \n"
            "in vec2 texcoords_out;                                                                \n"
            "uniform sampler2D tex1;                                                               \n"
            "out vec4 FragColor;                                                                   \n"
            "%s"
            "void main()                                                                           \n"
            "{                                                                                     \n"
            "  vec4 pixel = texture(tex1, texcoords_out);                                          \n"
            "  FragColor.xy = (RGBtoYUV * vec4(pixel.rgb, 1.0)).yz;                                \n"
            "  FragColor.w = pixel.a;                                                              \n"
            "}                                                                                     \n", color_transform_matrix);
    }

    if(gsr_shader_init(shader, egl, vertex_shader, fragment_shader) != 0)
        return -1;

    gsr_shader_bind_attribute_location(shader, "pos", 0);
    gsr_shader_bind_attribute_location(shader, "texcoords", 1);
    uniforms->offset = egl->glGetUniformLocation(shader->program_id, "offset");
    uniforms->rotation = egl->glGetUniformLocation(shader->program_id, "rotation");
    return 0;
}

static int load_framebuffers(gsr_color_conversion *self) {
    /* TODO: Only generate the necessary amount of framebuffers (self->params.num_destination_textures) */
    const unsigned int draw_buffer = GL_COLOR_ATTACHMENT0;
    self->params.egl->glGenFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[0], 0);
    self->params.egl->glDrawBuffers(1, &draw_buffer);
    if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for Y\n");
        goto err;
    }

    if(self->params.num_destination_textures > 1) {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        self->params.egl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, self->params.destination_textures[1], 0);
        self->params.egl->glDrawBuffers(1, &draw_buffer);
        if(self->params.egl->glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to create framebuffer for UV\n");
            goto err;
        }
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return 0;

    err:
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
    return -1;
}

static int create_vertices(gsr_color_conversion *self) {
    self->params.egl->glGenVertexArrays(1, &self->vertex_array_object_id);
    self->params.egl->glBindVertexArray(self->vertex_array_object_id);

    self->params.egl->glGenBuffers(1, &self->vertex_buffer_object_id);
    self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), NULL, GL_DYNAMIC_DRAW);

    self->params.egl->glEnableVertexAttribArray(0);
    self->params.egl->glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)0);

    self->params.egl->glEnableVertexAttribArray(1);
    self->params.egl->glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), (void*)(2 * sizeof(float)));

    self->params.egl->glBindVertexArray(0);
    return 0;
}

int gsr_color_conversion_init(gsr_color_conversion *self, const gsr_color_conversion_params *params) {
    assert(params);
    assert(params->egl);
    memset(self, 0, sizeof(*self));
    self->params.egl = params->egl;
    self->params = *params;

    switch(params->destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            if(self->params.num_destination_textures != 2) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: expected 2 destination textures for destination color NV12/P010, got %d destination texture(s)\n", self->params.num_destination_textures);
                return -1;
            }

            if(load_shader_y(&self->shaders[0], self->params.egl, &self->uniforms[0], params->destination_color, params->color_range, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y shader\n");
                goto err;
            }

            if(load_shader_uv(&self->shaders[1], self->params.egl, &self->uniforms[1], params->destination_color, params->color_range, false) != 0) {
                fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV shader\n");
                goto err;
            }

            if(self->params.load_external_image_shader) {
                if(load_shader_y(&self->shaders[2], self->params.egl, &self->uniforms[2], params->destination_color, params->color_range, true) != 0) {
                    fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load Y shader\n");
                    goto err;
                }

                if(load_shader_uv(&self->shaders[3], self->params.egl, &self->uniforms[3], params->destination_color, params->color_range, true) != 0) {
                    fprintf(stderr, "gsr error: gsr_color_conversion_init: failed to load UV shader\n");
                    goto err;
                }
            }
            break;
        }
    }

    if(load_framebuffers(self) != 0)
        goto err;

    if(create_vertices(self) != 0)
        goto err;

    return 0;

    err:
    gsr_color_conversion_deinit(self);
    return -1;
}

void gsr_color_conversion_deinit(gsr_color_conversion *self) {
    if(!self->params.egl)
        return;

    if(self->vertex_buffer_object_id) {
        self->params.egl->glDeleteBuffers(1, &self->vertex_buffer_object_id);
        self->vertex_buffer_object_id = 0;
    }

    if(self->vertex_array_object_id) {
        self->params.egl->glDeleteVertexArrays(1, &self->vertex_array_object_id);
        self->vertex_array_object_id = 0;
    }

    self->params.egl->glDeleteFramebuffers(MAX_FRAMEBUFFERS, self->framebuffers);
    for(int i = 0; i < MAX_FRAMEBUFFERS; ++i) {
        self->framebuffers[i] = 0;
    }

    for(int i = 0; i < MAX_SHADERS; ++i) {
        gsr_shader_deinit(&self->shaders[i]);
    }

    self->params.egl = NULL;
}

static void gsr_color_conversion_swizzle_texture_source(gsr_color_conversion *self) {
    if(self->params.source_color == GSR_SOURCE_COLOR_BGR) {
        const int swizzle_mask[] = { GL_BLUE, GL_GREEN, GL_RED, 1 };
        self->params.egl->glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
    }
}

static void gsr_color_conversion_swizzle_reset(gsr_color_conversion *self) {
    if(self->params.source_color == GSR_SOURCE_COLOR_BGR) {
        const int swizzle_mask[] = { GL_RED, GL_GREEN, GL_BLUE, GL_ALPHA };
        self->params.egl->glTexParameteriv(GL_TEXTURE_2D, GL_TEXTURE_SWIZZLE_RGBA, swizzle_mask);
    }
}

/* |source_pos| is in pixel coordinates and |source_size|  */
void gsr_color_conversion_draw(gsr_color_conversion *self, unsigned int texture_id, vec2i source_pos, vec2i source_size, vec2i texture_pos, vec2i texture_size, float rotation, bool external_texture) {
    // TODO: Remove this crap
    rotation = M_PI*2.0f - rotation;

    /* TODO: Do not call this every frame? */
    vec2i dest_texture_size = {0, 0};
    self->params.egl->glBindTexture(GL_TEXTURE_2D, self->params.destination_textures[0]);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &dest_texture_size.x);
    self->params.egl->glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &dest_texture_size.y);
    self->params.egl->glBindTexture(GL_TEXTURE_2D, 0);

    const int texture_target = external_texture ? GL_TEXTURE_EXTERNAL_OES : GL_TEXTURE_2D;

    self->params.egl->glBindTexture(texture_target, texture_id);

    vec2i source_texture_size = {0, 0};
    if(external_texture) {
        assert(self->params.load_external_image_shader);
        source_texture_size = source_size;
    } else {
        /* TODO: Do not call this every frame? */
        self->params.egl->glGetTexLevelParameteriv(texture_target, 0, GL_TEXTURE_WIDTH, &source_texture_size.x);
        self->params.egl->glGetTexLevelParameteriv(texture_target, 0, GL_TEXTURE_HEIGHT, &source_texture_size.y);
    }

    // TODO: Remove this crap
    if(abs_f(M_PI * 0.5f - rotation) <= 0.001f || abs_f(M_PI * 1.5f - rotation) <= 0.001f) {
        float tmp = source_texture_size.x;
        source_texture_size.x = source_texture_size.y;
        source_texture_size.y = tmp;
    }

    const vec2f pos_norm = {
        ((float)source_pos.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f,
        ((float)source_pos.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f,
    };

    const vec2f size_norm = {
        ((float)source_size.x / (dest_texture_size.x == 0 ? 1.0f : (float)dest_texture_size.x)) * 2.0f,
        ((float)source_size.y / (dest_texture_size.y == 0 ? 1.0f : (float)dest_texture_size.y)) * 2.0f,
    };

    const vec2f texture_pos_norm = {
        (float)texture_pos.x / (source_texture_size.x == 0 ? 1.0f : (float)source_texture_size.x),
        (float)texture_pos.y / (source_texture_size.y == 0 ? 1.0f : (float)source_texture_size.y),
    };

    const vec2f texture_size_norm = {
        (float)texture_size.x / (source_texture_size.x == 0 ? 1.0f : (float)source_texture_size.x),
        (float)texture_size.y / (source_texture_size.y == 0 ? 1.0f : (float)source_texture_size.y),
    };

    const float vertices[] = {
        -1.0f + 0.0f,               -1.0f + 0.0f + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + 0.0f,               -1.0f + 0.0f,               texture_pos_norm.x,                       texture_pos_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,

        -1.0f + 0.0f,               -1.0f + 0.0f + size_norm.y, texture_pos_norm.x,                       texture_pos_norm.y + texture_size_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f,               texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y,
        -1.0f + 0.0f + size_norm.x, -1.0f + 0.0f + size_norm.y, texture_pos_norm.x + texture_size_norm.x, texture_pos_norm.y + texture_size_norm.y
    };

    gsr_color_conversion_swizzle_texture_source(self);

    self->params.egl->glBindVertexArray(self->vertex_array_object_id);
    self->params.egl->glViewport(0, 0, dest_texture_size.x, dest_texture_size.y);

    /* TODO: this, also cleanup */
    //self->params.egl->glBindBuffer(GL_ARRAY_BUFFER, self->vertex_buffer_object_id);
    self->params.egl->glBufferSubData(GL_ARRAY_BUFFER, 0, 24 * sizeof(float), vertices);

    {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
        //cap_xcomp->params.egl->glClear(GL_COLOR_BUFFER_BIT); // TODO: Do this in a separate clear_ function. We want to do that when using multiple drm to create the final image (multiple monitors for example)

        const int shader_index = external_texture ? 2 : 0;
        gsr_shader_use(&self->shaders[shader_index]);
        self->params.egl->glUniform1f(self->uniforms[shader_index].rotation, rotation);
        self->params.egl->glUniform2f(self->uniforms[shader_index].offset, pos_norm.x, pos_norm.y);
        self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    if(self->params.num_destination_textures > 1) {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        //cap_xcomp->params.egl->glClear(GL_COLOR_BUFFER_BIT);

        const int shader_index = external_texture ? 3 : 1;
        gsr_shader_use(&self->shaders[shader_index]);
        self->params.egl->glUniform1f(self->uniforms[shader_index].rotation, rotation);
        self->params.egl->glUniform2f(self->uniforms[shader_index].offset, pos_norm.x, pos_norm.y);
        self->params.egl->glDrawArrays(GL_TRIANGLES, 0, 6);
    }

    self->params.egl->glBindVertexArray(0);
    gsr_shader_use_none(&self->shaders[0]);
    self->params.egl->glBindTexture(texture_target, 0);
    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);

    gsr_color_conversion_swizzle_reset(self);
}

void gsr_color_conversion_clear(gsr_color_conversion *self) {
    float color1[4] = {0.0f, 0.0f, 0.0f, 1.0f};
    float color2[4] = {0.0f, 0.0f, 0.0f, 1.0f};

    switch(self->params.destination_color) {
        case GSR_DESTINATION_COLOR_NV12:
        case GSR_DESTINATION_COLOR_P010: {
            color2[0] = 0.5f;
            color2[1] = 0.5f;
            color2[2] = 0.0f;
            color2[3] = 1.0f;
            break;
        }
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[0]);
    self->params.egl->glClearColor(color1[0], color1[1], color1[2], color1[3]);
    self->params.egl->glClear(GL_COLOR_BUFFER_BIT);

    if(self->params.num_destination_textures > 1) {
        self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, self->framebuffers[1]);
        self->params.egl->glClearColor(color2[0], color2[1], color2[2], color2[3]);
        self->params.egl->glClear(GL_COLOR_BUFFER_BIT);
    }

    self->params.egl->glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
