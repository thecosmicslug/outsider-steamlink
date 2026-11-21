/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/filters.h"
#include "rendering/renderer.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RMMZ_STEAMLINK
#include "platform/sdl.h"

static int s_profile_gpu_sync;
static double s_profile_gpu_wait_ms;
static unsigned int s_profile_gpu_draw_count;
#endif

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"
#endif

/* Built-in shader sources */

#ifdef RMMZ_GLES2
#define GLSL_FRAGMENT_VERSION "#version 100\nprecision mediump float;\n"
#define GLSL_INPUT "varying "
#define GLSL_TEXTURE_FN "texture2D"
#define GLSL_FRAGMENT_OUTPUT ""
#define GLSL_FRAGMENT_COLOR "gl_FragColor"
#else
#define GLSL_FRAGMENT_VERSION "#version 450 core\n"
#define GLSL_INPUT "in "
#define GLSL_TEXTURE_FN "texture"
#define GLSL_FRAGMENT_OUTPUT "out vec4 fragColor;\n"
#define GLSL_FRAGMENT_COLOR "fragColor"
#endif

#ifdef RMMZ_GLES2
static const char *DEFAULT_VERT_SRC =
    "#version 100\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "varying vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";
#else
static const char *DEFAULT_VERT_SRC =
    "#version 450 core\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "out vec2 v_texcoord;\n"
    "void main() {\n"
    "    gl_Position = vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "}\n";
#endif

static const char *COLOR_MATRIX_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform mat4 u_colorMatrix;\n"
    "uniform vec4 u_colorOffset;\n"
    GLSL_FRAGMENT_OUTPUT
    "void main() {\n"
    "    vec4 c = " GLSL_TEXTURE_FN "(u_texture, v_texcoord);\n"
    "    if (c.a > 0.0) {\n"
    "        c.rgb /= c.a;\n"
    "    }\n"
    "    vec4 result = u_colorMatrix * c + u_colorOffset;\n"
    "    result = clamp(result, 0.0, 1.0);\n"
    "    result.rgb *= result.a;\n"
    "    " GLSL_FRAGMENT_COLOR " = result;\n"
    "}\n";

static const char *BLUR_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform vec2 u_direction;\n"
    "uniform float u_strength;\n"
    GLSL_FRAGMENT_OUTPUT
    "void main() {\n"
    "    vec4 color = vec4(0.0);\n"
    "    vec2 off1 = u_direction * 1.3846153846 * u_strength;\n"
    "    vec2 off2 = u_direction * 3.2307692308 * u_strength;\n"
    "    color += " GLSL_TEXTURE_FN "(u_texture, v_texcoord) * 0.2270270270;\n"
    "    color += " GLSL_TEXTURE_FN "(u_texture, v_texcoord + off1) * 0.3162162162;\n"
    "    color += " GLSL_TEXTURE_FN "(u_texture, v_texcoord - off1) * 0.3162162162;\n"
    "    color += " GLSL_TEXTURE_FN "(u_texture, v_texcoord + off2) * 0.0702702703;\n"
    "    color += " GLSL_TEXTURE_FN "(u_texture, v_texcoord - off2) * 0.0702702703;\n"
    "    " GLSL_FRAGMENT_COLOR " = color;\n"
    "}\n";

/* Port of the ColorFilter shader from rmmz_core.js: hue rotation, color tone,
   blend color and brightness. */
static const char *COLOR_FILTER_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float hue;\n"
    "uniform vec4 colorTone;\n"
    "uniform vec4 blendColor;\n"
    "uniform float brightness;\n"
    GLSL_FRAGMENT_OUTPUT
    "vec3 rgbToHsl(vec3 rgb) {\n"
    "    float r = rgb.r;\n"
    "    float g = rgb.g;\n"
    "    float b = rgb.b;\n"
    "    float cmin = min(r, min(g, b));\n"
    "    float cmax = max(r, max(g, b));\n"
    "    float h = 0.0;\n"
    "    float s = 0.0;\n"
    "    float l = (cmin + cmax) / 2.0;\n"
    "    float delta = cmax - cmin;\n"
    "    if (delta > 0.0) {\n"
    "        if (r == cmax) {\n"
    "            h = mod((g - b) / delta + 6.0, 6.0) / 6.0;\n"
    "        } else if (g == cmax) {\n"
    "            h = ((b - r) / delta + 2.0) / 6.0;\n"
    "        } else {\n"
    "            h = ((r - g) / delta + 4.0) / 6.0;\n"
    "        }\n"
    "        if (l < 1.0) {\n"
    "            s = delta / (1.0 - abs(2.0 * l - 1.0));\n"
    "        }\n"
    "    }\n"
    "    return vec3(h, s, l);\n"
    "}\n"
    "vec3 hslToRgb(vec3 hsl) {\n"
    "    float h = hsl.x;\n"
    "    float s = hsl.y;\n"
    "    float l = hsl.z;\n"
    "    float c = (1.0 - abs(2.0 * l - 1.0)) * s;\n"
    "    float x = c * (1.0 - abs(mod(h * 6.0, 2.0) - 1.0));\n"
    "    float m = l - c / 2.0;\n"
    "    float cm = c + m;\n"
    "    float xm = x + m;\n"
    "    if (h < 1.0 / 6.0) {\n"
    "        return vec3(cm, xm, m);\n"
    "    } else if (h < 2.0 / 6.0) {\n"
    "        return vec3(xm, cm, m);\n"
    "    } else if (h < 3.0 / 6.0) {\n"
    "        return vec3(m, cm, xm);\n"
    "    } else if (h < 4.0 / 6.0) {\n"
    "        return vec3(m, xm, cm);\n"
    "    } else if (h < 5.0 / 6.0) {\n"
    "        return vec3(xm, m, cm);\n"
    "    } else {\n"
    "        return vec3(cm, m, xm);\n"
    "    }\n"
    "}\n"
    "void main() {\n"
    "    vec4 sample_ = " GLSL_TEXTURE_FN "(u_texture, v_texcoord);\n"
    "    float a = sample_.a;\n"
    "    if (a <= 0.0) { " GLSL_FRAGMENT_COLOR " = vec4(0.0); return; }\n"
    "    vec3 rgb = sample_.rgb;\n"
    "    if (hue != 0.0 || colorTone.a != 0.0) {\n"
    "        vec3 hsl = rgbToHsl(sample_.rgb);\n"
    "        hsl.x = mod(hsl.x + hue / 360.0, 1.0);\n"
    "        hsl.y = hsl.y * (1.0 - colorTone.a / 255.0);\n"
    "        rgb = hslToRgb(hsl);\n"
    "    }\n"
    "    float r = rgb.r;\n"
    "    float g = rgb.g;\n"
    "    float b = rgb.b;\n"
    "    float r2 = colorTone.r / 255.0;\n"
    "    float g2 = colorTone.g / 255.0;\n"
    "    float b2 = colorTone.b / 255.0;\n"
    "    float r3 = blendColor.r / 255.0;\n"
    "    float g3 = blendColor.g / 255.0;\n"
    "    float b3 = blendColor.b / 255.0;\n"
    "    float i3 = blendColor.a / 255.0;\n"
    "    float i1 = 1.0 - i3;\n"
    "    r = clamp(r + r2 * a, 0.0, 1.0);\n"
    "    g = clamp(g + g2 * a, 0.0, 1.0);\n"
    "    b = clamp(b + b2 * a, 0.0, 1.0);\n"
    "    r = clamp(r * i1 + r3 * i3 * a, 0.0, 1.0);\n"
    "    g = clamp(g * i1 + g3 * i3 * a, 0.0, 1.0);\n"
    "    b = clamp(b * i1 + b3 * i3 * a, 0.0, 1.0);\n"
    "    r = r * brightness / 255.0;\n"
    "    g = g * brightness / 255.0;\n"
    "    b = b * brightness / 255.0;\n"
    "    " GLSL_FRAGMENT_COLOR " = vec4(r, g, b, a);\n"
    "}\n";

static const char *ALPHA_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_alpha;\n"
    GLSL_FRAGMENT_OUTPUT
    "void main() {\n"
    "    " GLSL_FRAGMENT_COLOR " = " GLSL_TEXTURE_FN "(u_texture, v_texcoord) * u_alpha;\n"
    "}\n";

/* Sprite/Graphics masks: content multiplied by the mask's alpha, both
   rendered to screen-sized textures beforehand. */
static const char *MASK_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    "uniform sampler2D u_mask;\n"
    GLSL_FRAGMENT_OUTPUT
    "void main() {\n"
    "    " GLSL_FRAGMENT_COLOR " = " GLSL_TEXTURE_FN "(u_texture, v_texcoord) * "
        GLSL_TEXTURE_FN "(u_mask, v_texcoord).a;\n"
    "}\n";

static const char *COPY_FRAG_SRC =
    GLSL_FRAGMENT_VERSION
    GLSL_INPUT "vec2 v_texcoord;\n"
    "uniform sampler2D u_texture;\n"
    GLSL_FRAGMENT_OUTPUT
    "void main() {\n"
    "    " GLSL_FRAGMENT_COLOR " = " GLSL_TEXTURE_FN "(u_texture, v_texcoord);\n"
    "}\n";

static int s_initialized = 0;

#ifdef RMMZ_HAS_GL
#ifndef RMMZ_GLES2
static GLuint s_quad_vao = 0;
#endif
static GLuint s_quad_vbo = 0;
#endif

/* Fullscreen quad: position(2f) + texcoord(2f), 6 vertices (2 triangles). */
static const float QUAD_VERTICES[] = {
    -1.0f, -1.0f,  0.0f, 0.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
     1.0f, -1.0f,  1.0f, 0.0f,
     1.0f,  1.0f,  1.0f, 1.0f,
    -1.0f,  1.0f,  0.0f, 1.0f,
};

#ifdef RMMZ_HAS_GL

static GLuint compile_shader_src(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "filters: shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

#endif /* RMMZ_HAS_GL */

void filters_init(void)
{
    if (s_initialized) return;
    s_initialized = 1;

#ifdef RMMZ_HAS_GL
#ifndef RMMZ_GLES2
    glGenVertexArrays(1, &s_quad_vao);
    glBindVertexArray(s_quad_vao);
#endif
    glGenBuffers(1, &s_quad_vbo);
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glBufferData(GL_ARRAY_BUFFER, sizeof(QUAD_VERTICES), QUAD_VERTICES, GL_STATIC_DRAW);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#ifndef RMMZ_GLES2
    glBindVertexArray(0);
#endif
#endif
}

void filters_shutdown(void)
{
    if (!s_initialized) return;

#ifdef RMMZ_HAS_GL
    if (s_quad_vbo) { glDeleteBuffers(1, &s_quad_vbo); s_quad_vbo = 0; }
#ifndef RMMZ_GLES2
    if (s_quad_vao) { glDeleteVertexArrays(1, &s_quad_vao); s_quad_vao = 0; }
#endif
#endif

    s_initialized = 0;
}

uint32_t filter_compile_shader(const char *vert_src, const char *frag_src)
{
    if (!frag_src) return 0;
    if (!vert_src) vert_src = DEFAULT_VERT_SRC;

#ifdef RMMZ_HAS_GL
    GLuint vs = compile_shader_src(GL_VERTEX_SHADER, vert_src);
    if (!vs) return 0;

    GLuint fs = compile_shader_src(GL_FRAGMENT_SHADER, frag_src);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_texcoord");
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "filters: program link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return (uint32_t)prog;
#else
    (void)vert_src;
    return 0;
#endif
}

void filter_delete_shader(uint32_t program)
{
#ifdef RMMZ_HAS_GL
    if (program) {
        GLuint p = program;
        glDeleteProgram(p);
    }
#else
    (void)program;
#endif
}

static void filter_begin_internal(uint32_t program, uint32_t input_texture,
                                  int width, int height, int blend)
{
#ifdef RMMZ_HAS_GL
    glUseProgram(program);

    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, input_texture);

    GLint loc = glGetUniformLocation(program, "u_texture");
    if (loc >= 0) glUniform1i(loc, 0);

    loc = glGetUniformLocation(program, "u_resolution");
    if (loc >= 0) {
        float scale = renderer_get_render_scale();
        glUniform2f(loc, width * scale, height * scale);
    }

    if (blend) {
        glEnable(GL_BLEND);
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    } else {
        glDisable(GL_BLEND);
    }

#ifndef RMMZ_GLES2
    glBindVertexArray(s_quad_vao);
#else
    glBindBuffer(GL_ARRAY_BUFFER, s_quad_vbo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float), NULL);
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 4 * sizeof(float),
                          (const void *)(2 * sizeof(float)));
#endif
#else
    (void)program; (void)input_texture; (void)width; (void)height; (void)blend;
#endif
}

void filter_begin(uint32_t program, uint32_t input_texture, int width, int height)
{
    filter_begin_internal(program, input_texture, width, height, 1);
}

#ifdef RMMZ_GLES2
void filter_begin_overwrite(uint32_t program, uint32_t input_texture,
                            int width, int height)
{
    filter_begin_internal(program, input_texture, width, height, 0);
}
#endif

void filter_set_uniform_texture(uint32_t program, const char *name, uint32_t texture, int unit)
{
#ifdef RMMZ_HAS_GL
    GLint loc = glGetUniformLocation(program, name);
    if (loc < 0) return;
    glActiveTexture(GL_TEXTURE0 + unit);
    glBindTexture(GL_TEXTURE_2D, texture);
    glUniform1i(loc, unit);
    glActiveTexture(GL_TEXTURE0);
#else
    (void)program; (void)name; (void)texture; (void)unit;
#endif
}

void filter_set_uniform_1f(uint32_t program, const char *name, float value)
{
#ifdef RMMZ_HAS_GL
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform1f(loc, value);
#else
    (void)program; (void)name; (void)value;
#endif
}

void filter_set_uniform_2f(uint32_t program, const char *name, float x, float y)
{
#ifdef RMMZ_HAS_GL
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform2f(loc, x, y);
#else
    (void)program; (void)name; (void)x; (void)y;
#endif
}

void filter_set_uniform_4f(uint32_t program, const char *name,
                           float x, float y, float z, float w)
{
#ifdef RMMZ_HAS_GL
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniform4f(loc, x, y, z, w);
#else
    (void)program; (void)name; (void)x; (void)y; (void)z; (void)w;
#endif
}

void filter_set_uniform_mat4(uint32_t program, const char *name, const float *values)
{
#ifdef RMMZ_HAS_GL
    if (!values) return;
    GLint loc = glGetUniformLocation(program, name);
    if (loc >= 0) glUniformMatrix4fv(loc, 1, GL_FALSE, values);
#else
    (void)program; (void)name; (void)values;
#endif
}

void filter_draw_quad(void)
{
#ifdef RMMZ_HAS_GL
#ifdef RMMZ_STEAMLINK
    if (s_profile_gpu_sync) {
        /* Include the filter target clear queued before this fullscreen draw. */
        uint64_t start = SDL_GetPerformanceCounter();
        glFinish();
        glDrawArrays(GL_TRIANGLES, 0, 6);
        glFinish();
        uint64_t elapsed = SDL_GetPerformanceCounter() - start;
        s_profile_gpu_wait_ms +=
            (double)elapsed * 1000.0 / (double)SDL_GetPerformanceFrequency();
        s_profile_gpu_draw_count++;
        return;
    }
#endif
    glDrawArrays(GL_TRIANGLES, 0, 6);
#endif
}

#ifdef RMMZ_STEAMLINK
void filters_set_profile_gpu_sync(int enabled)
{
    s_profile_gpu_sync = enabled;
}

void filters_take_profile_gpu_stats(double *wait_ms, unsigned int *draw_count)
{
    if (wait_ms) *wait_ms = s_profile_gpu_wait_ms;
    if (draw_count) *draw_count = s_profile_gpu_draw_count;
    s_profile_gpu_wait_ms = 0.0;
    s_profile_gpu_draw_count = 0;
}
#endif

void filter_end(void)
{
#ifdef RMMZ_HAS_GL
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#ifndef RMMZ_GLES2
    glBindVertexArray(0);
#endif
    glUseProgram(0);
    glBindTexture(GL_TEXTURE_2D, 0);
#endif
}

const char *filter_default_vert_src(void)  { return DEFAULT_VERT_SRC; }
const char *filter_color_matrix_frag_src(void) { return COLOR_MATRIX_FRAG_SRC; }
const char *filter_blur_frag_src(void)     { return BLUR_FRAG_SRC; }
const char *filter_alpha_frag_src(void)    { return ALPHA_FRAG_SRC; }
const char *filter_color_filter_frag_src(void) { return COLOR_FILTER_FRAG_SRC; }
const char *filter_mask_frag_src(void) { return MASK_FRAG_SRC; }
const char *filter_copy_frag_src(void) { return COPY_FRAG_SRC; }
