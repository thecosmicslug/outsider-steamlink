/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/sprite_batch.h"

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"
#endif
#ifdef RMMZ_STEAMLINK
#include "platform/sdl.h"
#endif

#ifdef RMMZ_STEAMLINK
static bool s_profile_gpu_sync;
static SpriteBatchProfileGroupStats s_profile_gpu_sprites;
static SpriteBatchProfileGroupStats s_profile_gpu_premult_composites;
static unsigned int s_profile_gpu_blend_disabled_batches;
static unsigned int s_profile_gpu_full_screen_clear_batches;
#endif

typedef struct {
    float x, y;       /* position */
    float u, v;       /* texcoord */
    float r, g, b, a; /* color: tint RGB [0..1] and alpha (NOT premultiplied) */
} Vertex;

/* Indexed rendering: 4 unique vertices and 6 indices per quad. */
#define VERTS_PER_QUAD   4
#define INDICES_PER_QUAD 6

/* Shader sources */

#ifdef RMMZ_HAS_GL

#ifdef RMMZ_GLES2
static const char *VERT_SRC =
    "#version 100\n"
    "attribute vec2 a_position;\n"
    "attribute vec2 a_texcoord;\n"
    "attribute vec4 a_color;\n"
    "uniform mat4 u_projection;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = a_color;\n"
    "}\n";

static const char *FRAG_SRC =
    "#version 100\n"
    "precision mediump float;\n"
    "varying vec2 v_texcoord;\n"
    "varying vec4 v_color;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_premultiplied;\n"
    "void main() {\n"
    "    vec4 c = texture2D(u_texture, v_texcoord) * v_color;\n"
    "    gl_FragColor = vec4(c.rgb * mix(c.a, 1.0, u_premultiplied), c.a);\n"
    "}\n";
#else
static const char *VERT_SRC =
    "#version 450 core\n"
    "layout(location = 0) in vec2 a_position;\n"
    "layout(location = 1) in vec2 a_texcoord;\n"
    "layout(location = 2) in vec4 a_color;\n"
    "uniform mat4 u_projection;\n"
    "out vec2 v_texcoord;\n"
    "out vec4 v_color;\n"
    "void main() {\n"
    "    gl_Position = u_projection * vec4(a_position, 0.0, 1.0);\n"
    "    v_texcoord = a_texcoord;\n"
    "    v_color = a_color;\n"
    "}\n";

/* Emits premultiplied colour, like PIXI. Textures are stored straight, so rgb
   is multiplied by the final alpha unless u_premultiplied marks an already
   premultiplied source (an FBO produced by this batch). This is what makes
   the MULTIPLY / SCREEN blend functions below correct. */
static const char *FRAG_SRC =
    "#version 450 core\n"
    "in vec2 v_texcoord;\n"
    "in vec4 v_color;\n"
    "uniform sampler2D u_texture;\n"
    "uniform float u_premultiplied;\n"
    "out vec4 fragColor;\n"
    "void main() {\n"
    "    vec4 c = texture(u_texture, v_texcoord) * v_color;\n"
    "    fragColor = vec4(c.rgb * mix(c.a, 1.0, u_premultiplied), c.a);\n"
    "}\n";
#endif

#endif /* RMMZ_HAS_GL */

struct SpriteBatch {
    int       max_quads;
    int       quad_count;      /* quads queued in current batch */
    int       frame_quads;     /* total quads this frame */
    int       frame_culled;    /* quads culled (off-screen) this frame */
    int       draw_calls;      /* draw calls this frame */
    uint32_t  current_texture; /* GL texture currently batched */
    int       current_blend;   /* current blend mode */
    Vertex   *vertices;        /* CPU-side vertex buffer */

    /* Viewport bounds for frustum culling */
    float     viewport_w;
    float     viewport_h;

#ifdef RMMZ_HAS_GL
#ifndef RMMZ_GLES2
    GLuint    vao;
#endif
    GLuint    vbo;
    GLuint    ebo;             /* element (index) buffer */
    GLuint    shader;
    GLint     u_projection;
    GLint     u_texture;
    GLint     u_premultiplied;
    GLuint    white_texture;   /* 1x1 white pixel for untextured draws */
#endif

    float     proj[16];        /* orthographic projection matrix */
};

#ifdef RMMZ_STEAMLINK
static void profile_batch(SpriteBatchProfileGroupStats *profile,
                          const SpriteBatch *sb, double wait_ms)
{
    profile->wait_ms += wait_ms;
    profile->batch_count++;
    if (wait_ms <= profile->slowest_wait_ms) return;

    profile->slowest_wait_ms = wait_ms;
    profile->slowest_quads = (unsigned int)sb->quad_count;
    profile->slowest_texture = sb->current_texture;
    profile->slowest_blend = sb->current_blend;
    const Vertex *vertices = sb->vertices;
    profile->slowest_r = vertices[0].r;
    profile->slowest_g = vertices[0].g;
    profile->slowest_b = vertices[0].b;
    profile->slowest_a = vertices[0].a;
#ifdef RMMZ_HAS_GL
    profile->slowest_white_texture = sb->current_texture == sb->white_texture;
#endif
    float min_x = vertices[0].x;
    float max_x = min_x;
    float min_y = vertices[0].y;
    float max_y = min_y;
    size_t vertex_count = (size_t)sb->quad_count * VERTS_PER_QUAD;
    for (size_t i = 1; i < vertex_count; i++) {
        if (vertices[i].x < min_x) min_x = vertices[i].x;
        if (vertices[i].x > max_x) max_x = vertices[i].x;
        if (vertices[i].y < min_y) min_y = vertices[i].y;
        if (vertices[i].y > max_y) max_y = vertices[i].y;
    }
    if (min_x < 0.0f) min_x = 0.0f;
    if (min_y < 0.0f) min_y = 0.0f;
    if (max_x > sb->viewport_w) max_x = sb->viewport_w;
    if (max_y > sb->viewport_h) max_y = sb->viewport_h;
    profile->slowest_x = min_x;
    profile->slowest_y = min_y;
    profile->slowest_width = max_x > min_x ? max_x - min_x : 0.0f;
    profile->slowest_height = max_y > min_y ? max_y - min_y : 0.0f;
}
#endif

#ifdef RMMZ_HAS_GL
static bool batch_is_opaque_white(const SpriteBatch *sb)
{
    if (sb->current_blend != BLEND_MODE_NORMAL ||
        sb->current_texture != sb->white_texture) {
        return false;
    }

    size_t vertex_count = (size_t)sb->quad_count * VERTS_PER_QUAD;
    for (size_t i = 0; i < vertex_count; i++) {
        if (sb->vertices[i].a != 1.0f) return false;
    }
    return true;
}

static bool clear_fullscreen_black_batch(const SpriteBatch *sb)
{
    if (sb->quad_count != 1 ||
        sb->current_blend != BLEND_MODE_NORMAL ||
        sb->current_texture != sb->white_texture) {
        return false;
    }

    const Vertex *v = sb->vertices;
    for (int i = 0; i < VERTS_PER_QUAD; i++) {
        if (v[i].r != 0.0f || v[i].g != 0.0f ||
            v[i].b != 0.0f || v[i].a != 1.0f) {
            return false;
        }
    }
    if (v[0].y != v[1].y || v[2].y != v[3].y ||
        v[0].x != v[2].x || v[1].x != v[3].x) {
        return false;
    }

    float min_x = v[0].x < v[1].x ? v[0].x : v[1].x;
    float max_x = v[0].x > v[1].x ? v[0].x : v[1].x;
    float min_y = v[0].y < v[2].y ? v[0].y : v[2].y;
    float max_y = v[0].y > v[2].y ? v[0].y : v[2].y;
    if (min_x > 0.0f || min_y > 0.0f ||
        max_x < sb->viewport_w || max_y < sb->viewport_h) {
        return false;
    }

    GLint viewport[4];
    GLint old_scissor[4];
    GLfloat old_clear_color[4];
    glGetIntegerv(GL_VIEWPORT, viewport);
    if (viewport[2] <= 0 || viewport[3] <= 0) return false;
    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    if (scissor_enabled) glGetIntegerv(GL_SCISSOR_BOX, old_scissor);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, old_clear_color);

    GLint clear_x = viewport[0];
    GLint clear_y = viewport[1];
    GLint clear_right = viewport[0] + viewport[2];
    GLint clear_top = viewport[1] + viewport[3];
    if (scissor_enabled) {
        if (old_scissor[0] > clear_x) clear_x = old_scissor[0];
        if (old_scissor[1] > clear_y) clear_y = old_scissor[1];
        GLint scissor_right = old_scissor[0] + old_scissor[2];
        GLint scissor_top = old_scissor[1] + old_scissor[3];
        if (scissor_right < clear_right) clear_right = scissor_right;
        if (scissor_top < clear_top) clear_top = scissor_top;
    } else {
        glEnable(GL_SCISSOR_TEST);
    }

    if (clear_right > clear_x && clear_top > clear_y) {
        glScissor(clear_x, clear_y, clear_right - clear_x, clear_top - clear_y);
        glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
    }

    glClearColor(old_clear_color[0], old_clear_color[1],
                 old_clear_color[2], old_clear_color[3]);
    if (scissor_enabled) {
        glScissor(old_scissor[0], old_scissor[1], old_scissor[2], old_scissor[3]);
    } else {
        glDisable(GL_SCISSOR_TEST);
    }
    return true;
}
#endif

/* GL helpers */

#ifdef RMMZ_HAS_GL

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint s = glCreateShader(type);
    glShaderSource(s, 1, &src, NULL);
    glCompileShader(s);

    GLint ok;
    glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetShaderInfoLog(s, sizeof(log), NULL, log);
        fprintf(stderr, "sprite_batch: shader compile error: %s\n", log);
        glDeleteShader(s);
        return 0;
    }
    return s;
}

static GLuint create_program(const char *vert_src, const char *frag_src)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, vert_src);
    if (!vs) return 0;

    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, frag_src);
    if (!fs) { glDeleteShader(vs); return 0; }

    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glBindAttribLocation(prog, 0, "a_position");
    glBindAttribLocation(prog, 1, "a_texcoord");
    glBindAttribLocation(prog, 2, "a_color");
    glLinkProgram(prog);

    GLint ok;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[512];
        glGetProgramInfoLog(prog, sizeof(log), NULL, log);
        fprintf(stderr, "sprite_batch: program link error: %s\n", log);
        glDeleteProgram(prog);
        prog = 0;
    }

    glDeleteShader(vs);
    glDeleteShader(fs);
    return prog;
}

static GLuint create_white_texture(void)
{
    GLuint tex = 0;
    uint8_t white[4] = { 255, 255, 255, 255 };
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, 1, 1, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, white);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

/* Generate a static index buffer for indexed quad rendering.
   Pattern: for quad i, indices are 4i+0, 4i+1, 4i+2, 4i+1, 4i+3, 4i+2 */
static GLuint create_index_buffer(int max_quads)
{
    size_t count = (size_t)max_quads * INDICES_PER_QUAD;
    uint16_t *indices = malloc(count * sizeof(uint16_t));
    if (!indices) return 0;

    for (int i = 0; i < max_quads; i++) {
        int vi = i * 4;  /* vertex base index */
        int ii = i * 6;  /* index base */
        /* Triangle 1: TL, TR, BL */
        indices[ii + 0] = (uint16_t)(vi + 0);
        indices[ii + 1] = (uint16_t)(vi + 1);
        indices[ii + 2] = (uint16_t)(vi + 2);
        /* Triangle 2: TR, BR, BL */
        indices[ii + 3] = (uint16_t)(vi + 1);
        indices[ii + 4] = (uint16_t)(vi + 3);
        indices[ii + 5] = (uint16_t)(vi + 2);
    }

    GLuint ebo = 0;
    glGenBuffers(1, &ebo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, ebo);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)(count * sizeof(uint16_t)), indices, GL_STATIC_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);

    free(indices);
    return ebo;
}

static void bind_vertex_attributes(SpriteBatch *sb)
{
#ifndef RMMZ_GLES2
    glBindVertexArray(sb->vao);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, sb->vbo);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sb->ebo);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, x));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, u));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
                          (const void *)offsetof(Vertex, r));
}

/* 1.0 when the source texture is already premultiplied (see FRAG_SRC). */
static float premultiplied_flag(int mode)
{
    return (mode == BLEND_MODE_NORMAL_PREMULT) ? 1.0f : 0.0f;
}

/* PIXI's premultiplied-alpha blend table; exact because the shader always
   emits premultiplied colour. Alpha accumulates as coverage
   (ONE, ONE_MINUS_SRC_ALPHA) so a transparent FBO holds premultiplied (rgb*a, a). */
static void apply_blend_mode(int mode)
{
    switch (mode) {
    case BLEND_MODE_ADD:
        glBlendFuncSeparate(GL_ONE, GL_ONE,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BLEND_MODE_MULTIPLY:
        glBlendFuncSeparate(GL_DST_COLOR, GL_ONE_MINUS_SRC_ALPHA,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BLEND_MODE_SCREEN:
        glBlendFuncSeparate(GL_ONE, GL_ONE_MINUS_SRC_COLOR,
                            GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    case BLEND_MODE_NORMAL_PREMULT:
    case BLEND_MODE_NORMAL:
    default:
        glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
        break;
    }
}

#endif /* RMMZ_HAS_GL */

/* Column-major orthographic matrix mapping (0,0)-(w,h) to NDC with Y down. */
static void ortho_projection(float *m, float w, float h)
{
    memset(m, 0, 16 * sizeof(float));
    m[0]  =  2.0f / w;      /* sx */
    m[5]  = -2.0f / h;      /* sy (flip Y) */
    m[10] = -1.0f;           /* sz */
    m[12] = -1.0f;           /* tx */
    m[13] =  1.0f;           /* ty */
    m[15] =  1.0f;           /* w  */
}

SpriteBatch *sprite_batch_create(int max_quads)
{
    if (max_quads <= 0) max_quads = 4096;

    /* 16-bit indices: at most 65536 / 4 quads. */
    if (max_quads > 16384) max_quads = 16384;

    SpriteBatch *sb = calloc(1, sizeof(SpriteBatch));
    if (!sb) return NULL;

    sb->max_quads = max_quads;
    sb->viewport_w = 816.0f;
    sb->viewport_h = 624.0f;
    sb->vertices = malloc(sizeof(Vertex) * (size_t)max_quads * VERTS_PER_QUAD);
    if (!sb->vertices) {
        free(sb);
        return NULL;
    }

    ortho_projection(sb->proj, 816.0f, 624.0f);

#ifdef RMMZ_HAS_GL
    sb->shader = create_program(VERT_SRC, FRAG_SRC);
    if (!sb->shader) {
        free(sb->vertices);
        free(sb);
        return NULL;
    }

    sb->u_projection = glGetUniformLocation(sb->shader, "u_projection");
    sb->u_texture = glGetUniformLocation(sb->shader, "u_texture");
    sb->u_premultiplied = glGetUniformLocation(sb->shader, "u_premultiplied");

#ifndef RMMZ_GLES2
    glGenVertexArrays(1, &sb->vao);
    glBindVertexArray(sb->vao);
#endif
    glGenBuffers(1, &sb->vbo);

    /* Vertex buffer is re-uploaded every flush. */
    size_t buf_size = sizeof(Vertex) * (size_t)max_quads * VERTS_PER_QUAD;
    glBindBuffer(GL_ARRAY_BUFFER, sb->vbo);
    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)buf_size, NULL, GL_DYNAMIC_DRAW);
    glBindBuffer(GL_ARRAY_BUFFER, 0);

    sb->ebo = create_index_buffer(max_quads);
    if (!sb->ebo) {
        /* Without an element buffer glDrawElements would read a null binding. */
        sprite_batch_destroy(sb);
        return NULL;
    }
    sb->white_texture = create_white_texture();
    bind_vertex_attributes(sb);
#endif

    return sb;
}

void sprite_batch_destroy(SpriteBatch *sb)
{
    if (!sb) return;

#ifdef RMMZ_HAS_GL
    if (sb->white_texture) glDeleteTextures(1, &sb->white_texture);
    if (sb->ebo) glDeleteBuffers(1, &sb->ebo);
    if (sb->vbo) glDeleteBuffers(1, &sb->vbo);
#ifndef RMMZ_GLES2
    if (sb->vao) glDeleteVertexArrays(1, &sb->vao);
#endif
    if (sb->shader) glDeleteProgram(sb->shader);
#endif

    free(sb->vertices);
    free(sb);
}

void sprite_batch_set_projection(SpriteBatch *sb, float width, float height)
{
    if (!sb) return;
    ortho_projection(sb->proj, width, height);
    sb->viewport_w = width;
    sb->viewport_h = height;
}

void sprite_batch_begin(SpriteBatch *sb)
{
    if (!sb) return;
    sb->quad_count = 0;
    sb->frame_quads = 0;
    sb->frame_culled = 0;
    sb->draw_calls = 0;
    sb->current_texture = 0;
    sb->current_blend = BLEND_MODE_NORMAL;

#ifdef RMMZ_HAS_GL
    glDisable(GL_DEPTH_TEST);
    glDisable(GL_CULL_FACE);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_STENCIL_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glDepthMask(GL_FALSE);

    glEnable(GL_BLEND);
    apply_blend_mode(BLEND_MODE_NORMAL);

    glUseProgram(sb->shader);
    glUniformMatrix4fv(sb->u_projection, 1, GL_FALSE, sb->proj);
    glUniform1i(sb->u_texture, 0);
    glUniform1f(sb->u_premultiplied, premultiplied_flag(sb->current_blend));

    glActiveTexture(GL_TEXTURE0);
    bind_vertex_attributes(sb);
#endif
}

void sprite_batch_draw(SpriteBatch *sb, uint32_t texture,
                       float x, float y, float w, float h,
                       float u0, float v0, float u1, float v1,
                       uint32_t tint, float alpha)
{
    /* Axis-aligned quad: expand to four corners (TL, TR, BR, BL) and delegate. */
    sprite_batch_draw_verts(sb, texture,
                            x,     y,          /* TL */
                            x + w, y,          /* TR */
                            x + w, y + h,      /* BR */
                            x,     y + h,      /* BL */
                            u0, v0, u1, v1, tint, alpha);
}

void sprite_batch_draw_verts(SpriteBatch *sb, uint32_t texture,
                             float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float u0, float v0, float u1, float v1,
                             uint32_t tint, float alpha)
{
    if (!sb) return;

    /* Frustum culling from the axis-aligned bounds of the four corners. */
    float min_x = x0, max_x = x0, min_y = y0, max_y = y0;
    float xs[3] = { x1, x2, x3 };
    float ys[3] = { y1, y2, y3 };
    for (int i = 0; i < 3; i++) {
        if (xs[i] < min_x) min_x = xs[i];
        if (xs[i] > max_x) max_x = xs[i];
        if (ys[i] < min_y) min_y = ys[i];
        if (ys[i] > max_y) max_y = ys[i];
    }
    if (max_x < 0.0f || min_x > sb->viewport_w ||
        max_y < 0.0f || min_y > sb->viewport_h) {
        sb->frame_culled++;
        return;
    }

    /* Skip fully transparent quads. */
    if (alpha <= 0.0f) {
        sb->frame_culled++;
        return;
    }

    /* Use white texture for untextured draws. */
#ifdef RMMZ_HAS_GL
    if (texture == 0) texture = sb->white_texture;
#endif

    /* Flush if texture changed or batch full. */
    if (sb->quad_count > 0 &&
        (texture != sb->current_texture || sb->quad_count >= sb->max_quads)) {
        sprite_batch_flush(sb);
    }
    sb->current_texture = texture;

    /* Extract tint color components (ARGB format). */
    float tr = (float)((tint >> 16) & 0xFF) / 255.0f;
    float tg = (float)((tint >> 8) & 0xFF) / 255.0f;
    float tb = (float)(tint & 0xFF) / 255.0f;
    float ta = alpha;

    /* Vertex slots are TL, TR, BL, BR; the index buffer assembles
       triangles (0,1,2) and (1,3,2). */
    Vertex *v = &sb->vertices[sb->quad_count * VERTS_PER_QUAD];

    v[0] = (Vertex){ x0, y0, u0, v0, tr, tg, tb, ta };
    v[1] = (Vertex){ x1, y1, u1, v0, tr, tg, tb, ta };
    v[2] = (Vertex){ x3, y3, u0, v1, tr, tg, tb, ta };
    v[3] = (Vertex){ x2, y2, u1, v1, tr, tg, tb, ta };
    sb->quad_count++;
}

void sprite_batch_draw_verts_uv(SpriteBatch *sb, uint32_t texture,
                                const float xy[8], const float uv[8],
                                uint32_t tint, float alpha)
{
    if (!sb || alpha <= 0.0f) return;

    float min_x = xy[0], max_x = xy[0], min_y = xy[1], max_y = xy[1];
    for (int i = 1; i < 4; i++) {
        if (xy[i * 2] < min_x) min_x = xy[i * 2];
        if (xy[i * 2] > max_x) max_x = xy[i * 2];
        if (xy[i * 2 + 1] < min_y) min_y = xy[i * 2 + 1];
        if (xy[i * 2 + 1] > max_y) max_y = xy[i * 2 + 1];
    }
    if (max_x < 0.0f || min_x > sb->viewport_w ||
        max_y < 0.0f || min_y > sb->viewport_h) {
        sb->frame_culled++;
        return;
    }

#ifdef RMMZ_HAS_GL
    if (texture == 0) texture = sb->white_texture;
#endif
    if (sb->quad_count > 0 &&
        (texture != sb->current_texture || sb->quad_count >= sb->max_quads)) {
        sprite_batch_flush(sb);
    }
    sb->current_texture = texture;

    float tr = (float)((tint >> 16) & 0xFF) / 255.0f;
    float tg = (float)((tint >> 8) & 0xFF) / 255.0f;
    float tb = (float)(tint & 0xFF) / 255.0f;

    /* Slots TL, TR, BL, BR (see sprite_batch_draw_verts). */
    Vertex *v = &sb->vertices[sb->quad_count * VERTS_PER_QUAD];
    v[0] = (Vertex){ xy[0], xy[1], uv[0], uv[1], tr, tg, tb, alpha };
    v[1] = (Vertex){ xy[2], xy[3], uv[2], uv[3], tr, tg, tb, alpha };
    v[2] = (Vertex){ xy[6], xy[7], uv[6], uv[7], tr, tg, tb, alpha };
    v[3] = (Vertex){ xy[4], xy[5], uv[4], uv[5], tr, tg, tb, alpha };
    sb->quad_count++;
}

void sprite_batch_flush(SpriteBatch *sb)
{
    if (!sb || sb->quad_count == 0) return;

#ifdef RMMZ_HAS_GL
    bool cleared = clear_fullscreen_black_batch(sb);
    bool blend_disabled = false;
    if (cleared) {
#ifdef RMMZ_STEAMLINK
        if (s_profile_gpu_sync) s_profile_gpu_full_screen_clear_batches++;
#endif
    } else {
        size_t data_size = sizeof(Vertex) * (size_t)sb->quad_count * VERTS_PER_QUAD;
        glBindBuffer(GL_ARRAY_BUFFER, sb->vbo);
        glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)data_size, sb->vertices, GL_STREAM_DRAW);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, sb->ebo);

        glBindTexture(GL_TEXTURE_2D, sb->current_texture);

        int index_count = sb->quad_count * INDICES_PER_QUAD;
        blend_disabled = batch_is_opaque_white(sb);
        if (blend_disabled) glDisable(GL_BLEND);
        glDrawElements(GL_TRIANGLES, index_count, GL_UNSIGNED_SHORT, 0);
        if (blend_disabled) {
            glEnable(GL_BLEND);
#ifdef RMMZ_STEAMLINK
            if (s_profile_gpu_sync) s_profile_gpu_blend_disabled_batches++;
#endif
        }
    }
#ifdef RMMZ_STEAMLINK
    if (s_profile_gpu_sync) {
        uint64_t start = SDL_GetPerformanceCounter();
        glFinish();
        uint64_t elapsed = SDL_GetPerformanceCounter() - start;
        double wait_ms =
            (double)elapsed * 1000.0 / (double)SDL_GetPerformanceFrequency();
        SpriteBatchProfileGroupStats *profile =
            sb->current_blend == BLEND_MODE_NORMAL_PREMULT
                ? &s_profile_gpu_premult_composites
                : &s_profile_gpu_sprites;
        profile_batch(profile, sb, wait_ms);
    }
#endif
#endif

    sb->frame_quads += sb->quad_count;
    sb->draw_calls++;
    sb->quad_count = 0;
}

void sprite_batch_end(SpriteBatch *sb)
{
    if (!sb) return;
    sprite_batch_flush(sb);

#ifdef RMMZ_HAS_GL
    glBindBuffer(GL_ARRAY_BUFFER, 0);
#ifdef RMMZ_GLES2
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, 0);
#else
    glBindVertexArray(0);
#endif
    glUseProgram(0);
    glDisable(GL_BLEND);
#endif
}

void sprite_batch_set_blend_mode(SpriteBatch *sb, int mode)
{
    if (!sb) return;
    if (mode == sb->current_blend) return;

    sprite_batch_flush(sb);
    sb->current_blend = mode;

#ifdef RMMZ_HAS_GL
    apply_blend_mode(mode);
    /* The premultiplied flag rides along with the blend mode. */
    glUseProgram(sb->shader);
    glUniform1f(sb->u_premultiplied, premultiplied_flag(mode));
#endif
}

int sprite_batch_get_draw_calls(const SpriteBatch *sb)
{
    return sb ? sb->draw_calls : 0;
}

int sprite_batch_get_quad_count(const SpriteBatch *sb)
{
    return sb ? sb->frame_quads : 0;
}

int sprite_batch_get_culled_count(const SpriteBatch *sb)
{
    return sb ? sb->frame_culled : 0;
}

void sprite_batch_record_external_draw(SpriteBatch *sb, int quad_count)
{
    if (!sb || quad_count <= 0) return;
    sb->frame_quads += quad_count;
    sb->draw_calls++;
}

void sprite_batch_rebind(SpriteBatch *sb)
{
    if (!sb) return;

#ifdef RMMZ_HAS_GL
    glEnable(GL_BLEND);
    apply_blend_mode(sb->current_blend);
    glUseProgram(sb->shader);
    glUniformMatrix4fv(sb->u_projection, 1, GL_FALSE, sb->proj);
    glUniform1i(sb->u_texture, 0);
    glUniform1f(sb->u_premultiplied, premultiplied_flag(sb->current_blend));
    glActiveTexture(GL_TEXTURE0);
    bind_vertex_attributes(sb);
#endif
}

#ifdef RMMZ_STEAMLINK
void sprite_batch_set_profile_gpu_sync(bool enabled)
{
    s_profile_gpu_sync = enabled;
    if (!enabled) {
        memset(&s_profile_gpu_sprites, 0, sizeof(s_profile_gpu_sprites));
        memset(&s_profile_gpu_premult_composites, 0,
               sizeof(s_profile_gpu_premult_composites));
        s_profile_gpu_blend_disabled_batches = 0;
        s_profile_gpu_full_screen_clear_batches = 0;
    }
}

void sprite_batch_take_profile_gpu_stats(SpriteBatchProfileStats *stats)
{
    if (!stats) return;
    stats->sprites = s_profile_gpu_sprites;
    stats->premult_composites = s_profile_gpu_premult_composites;
    stats->blend_disabled_batches = s_profile_gpu_blend_disabled_batches;
    stats->full_screen_clear_batches = s_profile_gpu_full_screen_clear_batches;
    memset(&s_profile_gpu_sprites, 0, sizeof(s_profile_gpu_sprites));
    memset(&s_profile_gpu_premult_composites, 0,
           sizeof(s_profile_gpu_premult_composites));
    s_profile_gpu_blend_disabled_batches = 0;
    s_profile_gpu_full_screen_clear_batches = 0;
}
#endif
