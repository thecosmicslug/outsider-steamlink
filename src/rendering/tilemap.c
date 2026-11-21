/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/tilemap.h"

#include <stdio.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Fields per element in the packed float array. */
#define TILE_FIELDS 7

#if defined(RMMZ_GLES2)
#include "rendering/gl_loader.h"

#define TILEMAP_ATLAS_COUNT 3
#define TILEMAP_ATLAS_SIZE 2048
#define TILEMAP_SLOT_SIZE 1024
#define TILEMAP_MAX_SOURCES (TILEMAP_ATLAS_COUNT * 4)
#define TILEMAP_MAX_QUADS 16384

typedef struct {
    float texture_id;
    float source[2];
    float dest[2];
} TilemapVertex;

typedef struct {
    GLuint program;
    GLint projection_uniform;
    GLint sampler_uniforms[TILEMAP_ATLAS_COUNT];
    GLuint vertex_buffer;
    GLuint index_buffer;
    GLuint atlases[TILEMAP_ATLAS_COUNT];
    TilemapVertex *vertices;
    GLushort *indices;
    int capacity;
    uint32_t *source_ids;
    int *source_widths;
    int *source_heights;
    int source_count;
    uint32_t cache_token;
    bool key_valid;
    bool atlas_valid;
    bool shader_failed;
    bool profile_gpu_sync;
} TilemapRenderer;

static TilemapRenderer s_tilemap;

static const char tilemap_vertex_shader[] =
    "attribute float aTextureId;"
    "attribute vec2 aSource;"
    "attribute vec2 aDest;"
    "uniform mat4 uProjectionMatrix;"
    "varying vec2 vTextureCoord;"
    "varying float vTextureId;"
    "void main(void) {"
    "  gl_Position = uProjectionMatrix * vec4(aDest, 0.0, 1.0);"
    "  vTextureCoord = aSource;"
    "  vTextureId = aTextureId;"
    "}";

static const char tilemap_fragment_shader[] =
    "precision mediump float;"
    "varying vec2 vTextureCoord;"
    "varying float vTextureId;"
    "uniform sampler2D uSampler0;"
    "uniform sampler2D uSampler1;"
    "uniform sampler2D uSampler2;"
    "void main(void) {"
    "  vec2 uv = vTextureCoord / 2048.0;"
    "  vec4 color;"
    "  if (vTextureId < 0.0) {"
    "    color = vec4(0.0, 0.0, 0.0, 0.5);"
    "  } else if (vTextureId < 0.5) {"
    "    color = texture2D(uSampler0, uv);"
    "  } else if (vTextureId < 1.5) {"
    "    color = texture2D(uSampler1, uv);"
    "  } else {"
    "    color = texture2D(uSampler2, uv);"
    "  }"
    "  color.rgb *= color.a;"
    "  gl_FragColor = color;"
    "}";

static GLuint compile_tilemap_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char log[1024];
        GLsizei length = 0;
        glGetShaderInfoLog(shader, sizeof(log), &length, log);
        fprintf(stderr, "[tilemap] shader compilation failed: %.*s\n",
                (int)length, log);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool ensure_tilemap_program(void)
{
    if (s_tilemap.program) return true;
    if (s_tilemap.shader_failed) return false;

    GLuint vertex = compile_tilemap_shader(GL_VERTEX_SHADER, tilemap_vertex_shader);
    GLuint fragment = compile_tilemap_shader(GL_FRAGMENT_SHADER, tilemap_fragment_shader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        s_tilemap.shader_failed = true;
        return false;
    }

    GLuint program = glCreateProgram();
    if (!program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        s_tilemap.shader_failed = true;
        return false;
    }
    glAttachShader(program, vertex);
    glAttachShader(program, fragment);
    glBindAttribLocation(program, 0, "aTextureId");
    glBindAttribLocation(program, 1, "aSource");
    glBindAttribLocation(program, 2, "aDest");
    glLinkProgram(program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char log[1024];
        GLsizei length = 0;
        glGetProgramInfoLog(program, sizeof(log), &length, log);
        fprintf(stderr, "[tilemap] shader link failed: %.*s\n",
                (int)length, log);
        glDeleteProgram(program);
        s_tilemap.shader_failed = true;
        return false;
    }

    s_tilemap.projection_uniform =
        glGetUniformLocation(program, "uProjectionMatrix");
    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        char name[16];
        snprintf(name, sizeof(name), "uSampler%d", i);
        s_tilemap.sampler_uniforms[i] = glGetUniformLocation(program, name);
        if (s_tilemap.sampler_uniforms[i] < 0) {
            glDeleteProgram(program);
            s_tilemap.shader_failed = true;
            fprintf(stderr, "[tilemap] shader is missing %s\n", name);
            return false;
        }
    }
    if (s_tilemap.projection_uniform < 0) {
        glDeleteProgram(program);
        s_tilemap.shader_failed = true;
        fprintf(stderr, "[tilemap] shader is missing its projection uniform\n");
        return false;
    }

    s_tilemap.program = program;
    return true;
}

static void delete_atlases(void)
{
    if (s_tilemap.atlases[0] || s_tilemap.atlases[1] || s_tilemap.atlases[2]) {
        glDeleteTextures(TILEMAP_ATLAS_COUNT, s_tilemap.atlases);
        memset(s_tilemap.atlases, 0, sizeof(s_tilemap.atlases));
    }
    s_tilemap.atlas_valid = false;
}

static bool cache_key_matches(const TilesetInfo *tilesets, int count,
                              uint32_t cache_token)
{
    if (!s_tilemap.key_valid || s_tilemap.source_count != count ||
        s_tilemap.cache_token != cache_token) {
        return false;
    }
    for (int i = 0; i < count; i++) {
        if (s_tilemap.source_ids[i] != tilesets[i].gl_texture ||
            s_tilemap.source_widths[i] != tilesets[i].width ||
            s_tilemap.source_heights[i] != tilesets[i].height) {
            return false;
        }
    }
    return true;
}

static bool store_cache_key(const TilesetInfo *tilesets, int count,
                            uint32_t cache_token)
{
    uint32_t *ids = count ? malloc((size_t)count * sizeof(*ids)) : NULL;
    int *widths = count ? malloc((size_t)count * sizeof(*widths)) : NULL;
    int *heights = count ? malloc((size_t)count * sizeof(*heights)) : NULL;
    if (count && (!ids || !widths || !heights)) {
        free(ids);
        free(widths);
        free(heights);
        return false;
    }
    for (int i = 0; i < count; i++) {
        ids[i] = tilesets[i].gl_texture;
        widths[i] = tilesets[i].width;
        heights[i] = tilesets[i].height;
    }

    free(s_tilemap.source_ids);
    free(s_tilemap.source_widths);
    free(s_tilemap.source_heights);
    s_tilemap.source_ids = ids;
    s_tilemap.source_widths = widths;
    s_tilemap.source_heights = heights;
    s_tilemap.source_count = count;
    s_tilemap.cache_token = cache_token;
    s_tilemap.key_valid = true;
    return true;
}

static void restore_atlas_build_state(GLint framebuffer, const GLint viewport[4],
                                      GLint active_texture, GLint texture0,
                                      GLboolean scissor_enabled,
                                      const GLfloat clear_color[4],
                                      const GLboolean color_mask[4])
{
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)framebuffer);
    glViewport(viewport[0], viewport[1], viewport[2], viewport[3]);
    if (scissor_enabled) glEnable(GL_SCISSOR_TEST);
    else glDisable(GL_SCISSOR_TEST);
    glClearColor(clear_color[0], clear_color[1], clear_color[2], clear_color[3]);
    glColorMask(color_mask[0], color_mask[1], color_mask[2], color_mask[3]);
    glActiveTexture(GL_TEXTURE0);
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture0);
    glActiveTexture((GLenum)active_texture);
}

static bool build_atlases(const TilesetInfo *tilesets, int count)
{
    if (count > TILEMAP_MAX_SOURCES) {
        fprintf(stderr, "[tilemap] %d source images exceed the atlas layout\n", count);
        return false;
    }

    GLint max_texture_size = 0;
    glGetIntegerv(GL_MAX_TEXTURE_SIZE, &max_texture_size);
    if (max_texture_size < TILEMAP_ATLAS_SIZE) {
        fprintf(stderr, "[tilemap] GL_MAX_TEXTURE_SIZE %d is below %d\n",
                max_texture_size, TILEMAP_ATLAS_SIZE);
        return false;
    }

    GLint framebuffer = 0, viewport[4] = {0}, active_texture = GL_TEXTURE0;
    GLint texture0 = 0;
    GLfloat clear_color[4] = {0};
    GLboolean color_mask[4] = {GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE};
    GLboolean scissor_enabled = glIsEnabled(GL_SCISSOR_TEST);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &framebuffer);
    glGetIntegerv(GL_VIEWPORT, viewport);
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    glGetFloatv(GL_COLOR_CLEAR_VALUE, clear_color);
    glGetBooleanv(GL_COLOR_WRITEMASK, color_mask);
    glActiveTexture(GL_TEXTURE0);
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture0);

    GLuint atlases[TILEMAP_ATLAS_COUNT] = {0};
    GLuint framebuffer_object = 0;
    bool ok = false;

    glGenTextures(TILEMAP_ATLAS_COUNT, atlases);
    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        glBindTexture(GL_TEXTURE_2D, atlases[i]);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, TILEMAP_ATLAS_SIZE,
                     TILEMAP_ATLAS_SIZE, 0, GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    }

    glGenFramebuffers(1, &framebuffer_object);
    if (!framebuffer_object) goto cleanup;

    glBindFramebuffer(GL_FRAMEBUFFER, framebuffer_object);
    glDisable(GL_SCISSOR_TEST);
    glColorMask(GL_TRUE, GL_TRUE, GL_TRUE, GL_TRUE);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glViewport(0, 0, TILEMAP_ATLAS_SIZE, TILEMAP_ATLAS_SIZE);
    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, atlases[i], 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[tilemap] atlas framebuffer is incomplete\n");
            goto cleanup;
        }
        glClear(GL_COLOR_BUFFER_BIT);
    }

    for (int i = 0; i < count; i++) {
        const TilesetInfo *source = &tilesets[i];
        if (!source->gl_texture || source->width <= 0 || source->height <= 0) {
            continue;
        }
        if (source->width > TILEMAP_SLOT_SIZE ||
            source->height > TILEMAP_SLOT_SIZE) {
            fprintf(stderr, "[tilemap] source image %d exceeds its atlas slot (%dx%d)\n",
                    i, source->width, source->height);
            goto cleanup;
        }

        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
                               GL_TEXTURE_2D, source->gl_texture, 0);
        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            fprintf(stderr, "[tilemap] source image %d is not framebuffer-readable\n", i);
            goto cleanup;
        }

        int atlas_index = i >> 2;
        int dest_x = TILEMAP_SLOT_SIZE * (i & 1);
        int dest_y = TILEMAP_SLOT_SIZE * ((i >> 1) & 1);
        glBindTexture(GL_TEXTURE_2D, atlases[atlas_index]);
        glCopyTexSubImage2D(GL_TEXTURE_2D, 0, dest_x, dest_y, 0, 0,
                            source->width, source->height);
    }

    ok = true;

cleanup:
    restore_atlas_build_state(framebuffer, viewport, active_texture, texture0,
                              scissor_enabled, clear_color, color_mask);
    if (framebuffer_object) glDeleteFramebuffers(1, &framebuffer_object);

    if (ok) {
        memcpy(s_tilemap.atlases, atlases, sizeof(atlases));
        s_tilemap.atlas_valid = true;
    } else {
        glDeleteTextures(TILEMAP_ATLAS_COUNT, atlases);
    }
    return ok;
}

static bool ensure_atlases(const TilesetInfo *tilesets, int count,
                           uint32_t cache_token)
{
    if (cache_key_matches(tilesets, count, cache_token)) {
        return s_tilemap.atlas_valid;
    }

    delete_atlases();
    if (!store_cache_key(tilesets, count, cache_token)) {
        fprintf(stderr, "[tilemap] unable to allocate tileset cache key\n");
        return false;
    }
    if (!ensure_tilemap_program() || !build_atlases(tilesets, count)) {
        fprintf(stderr, "[tilemap] using single-texture fallback\n");
        return false;
    }
    return true;
}

static bool ensure_geometry_capacity(int required)
{
    if (required > TILEMAP_MAX_QUADS) return false;
    if (required <= s_tilemap.capacity) return true;

    int capacity = s_tilemap.capacity ? s_tilemap.capacity : 256;
    while (capacity < required) capacity *= 2;
    if (capacity > TILEMAP_MAX_QUADS) capacity = TILEMAP_MAX_QUADS;

    TilemapVertex *vertices = malloc((size_t)capacity * 4 * sizeof(*vertices));
    GLushort *indices = malloc((size_t)capacity * 6 * sizeof(*indices));
    if (!vertices || !indices) {
        free(vertices);
        free(indices);
        return false;
    }
    for (int i = 0; i < capacity; i++) {
        GLushort base = (GLushort)(i * 4);
        GLushort *quad = indices + i * 6;
        quad[0] = base;
        quad[1] = (GLushort)(base + 1);
        quad[2] = (GLushort)(base + 2);
        quad[3] = base;
        quad[4] = (GLushort)(base + 2);
        quad[5] = (GLushort)(base + 3);
    }

    free(s_tilemap.vertices);
    free(s_tilemap.indices);
    s_tilemap.vertices = vertices;
    s_tilemap.indices = indices;
    s_tilemap.capacity = capacity;

    if (!s_tilemap.vertex_buffer) glGenBuffers(1, &s_tilemap.vertex_buffer);
    if (!s_tilemap.index_buffer) glGenBuffers(1, &s_tilemap.index_buffer);
    if (!s_tilemap.vertex_buffer || !s_tilemap.index_buffer) return false;

    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_tilemap.index_buffer);
    glBufferData(GL_ELEMENT_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)capacity * 6 * sizeof(*indices)),
                 indices, GL_STATIC_DRAW);
    return true;
}

static void set_vertex(TilemapVertex *vertex, float texture_id,
                       float source_x, float source_y, float dest_x, float dest_y)
{
    vertex->texture_id = texture_id;
    vertex->source[0] = source_x;
    vertex->source[1] = source_y;
    vertex->dest[0] = dest_x;
    vertex->dest[1] = dest_y;
}

static int draw_tiles_atlas(const float *elements, int count,
                            const TilesetInfo *tilesets, int num_tilesets,
                            float offset_x, float offset_y,
                            float viewport_width, float viewport_height,
                            uint32_t cache_token, SpriteBatch *batch)
{
    if (!ensure_atlases(tilesets, num_tilesets, cache_token)) return -1;

    if (!ensure_geometry_capacity(count)) {
        fprintf(stderr, "[tilemap] unable to allocate tile geometry for %d elements\n",
                count);
        return -1;
    }

    int visible = 0;
    for (int i = 0; i < count; i++) {
        const float *e = elements + i * TILE_FIELDS;
        int set_number = (int)e[0];
        float sx = e[1];
        float sy = e[2];
        float dx = e[3] + offset_x;
        float dy = e[4] + offset_y;
        float w = e[5];
        float h = e[6];
        if (w <= 0.0f || h <= 0.0f) continue;

        float texture_id = -1.0f;
        if (set_number >= 0) {
            if (set_number >= num_tilesets) continue;
            const TilesetInfo *source = &tilesets[set_number];
            if (!source->gl_texture || source->width <= 0 || source->height <= 0) {
                continue;
            }
            int atlas_x = TILEMAP_SLOT_SIZE * (set_number & 1);
            int atlas_y = TILEMAP_SLOT_SIZE * ((set_number >> 1) & 1);
            sx += (float)atlas_x;
            sy += (float)atlas_y;
            texture_id = (float)(set_number >> 2);
        }

        if (dx + w < 0.0f || dx > viewport_width ||
            dy + h < 0.0f || dy > viewport_height) {
            continue;
        }

        TilemapVertex *v = s_tilemap.vertices + visible * 4;
        set_vertex(&v[0], texture_id, sx, sy, dx, dy);
        set_vertex(&v[1], texture_id, sx + w, sy, dx + w, dy);
        set_vertex(&v[2], texture_id, sx + w, sy + h, dx + w, dy + h);
        set_vertex(&v[3], texture_id, sx, sy + h, dx, dy + h);
        visible++;
    }

    if (visible == 0) return 0;

    sprite_batch_rebind(batch);
    sprite_batch_set_blend_mode(batch, BLEND_MODE_NORMAL);
    sprite_batch_flush(batch);

    GLint active_texture = GL_TEXTURE0;
    GLint texture_bindings[TILEMAP_ATLAS_COUNT] = {0};
    glGetIntegerv(GL_ACTIVE_TEXTURE, &active_texture);
    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        glActiveTexture((GLenum)(GL_TEXTURE0 + i));
        glGetIntegerv(GL_TEXTURE_BINDING_2D, &texture_bindings[i]);
        glBindTexture(GL_TEXTURE_2D, s_tilemap.atlases[i]);
    }

    glUseProgram(s_tilemap.program);
    float projection[16] = {
        2.0f / viewport_width, 0, 0, 0,
        0, -2.0f / viewport_height, 0, 0,
        0, 0, -1.0f, 0,
        -1.0f, 1.0f, 0, 1.0f
    };
    glUniformMatrix4fv(s_tilemap.projection_uniform, 1, GL_FALSE, projection);
    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        glUniform1i(s_tilemap.sampler_uniforms[i], i);
    }

    glBindBuffer(GL_ARRAY_BUFFER, s_tilemap.vertex_buffer);
    glBufferData(GL_ARRAY_BUFFER,
                 (GLsizeiptr)((size_t)visible * 4 * sizeof(*s_tilemap.vertices)),
                 s_tilemap.vertices, GL_STREAM_DRAW);
    glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, s_tilemap.index_buffer);
    GLsizei stride = (GLsizei)sizeof(TilemapVertex);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 1, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(TilemapVertex, texture_id));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(TilemapVertex, source));
    glEnableVertexAttribArray(2);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, stride,
                          (const void *)offsetof(TilemapVertex, dest));
    glDrawElements(GL_TRIANGLES, visible * 6, GL_UNSIGNED_SHORT, NULL);
#ifdef RMMZ_STEAMLINK
    if (s_tilemap.profile_gpu_sync) glFinish();
#endif

    for (int i = 0; i < TILEMAP_ATLAS_COUNT; i++) {
        glActiveTexture((GLenum)(GL_TEXTURE0 + i));
        glBindTexture(GL_TEXTURE_2D, (GLuint)texture_bindings[i]);
    }
    glActiveTexture((GLenum)active_texture);
    sprite_batch_rebind(batch);
    sprite_batch_record_external_draw(batch, visible);
    return visible;
}

#endif /* RMMZ_GLES2 */

static int draw_tiles_single_texture(const float *elements, int count,
                                     const TilesetInfo *tilesets, int num_tilesets,
                                     float offset_x, float offset_y,
                                     SpriteBatch *batch)
{
    int culled_before = sprite_batch_get_culled_count(batch);
    int candidates = 0;
    for (int i = 0; i < count; i++) {
        const float *e = elements + i * TILE_FIELDS;
        int set_number = (int)e[0];
        float sx = e[1];
        float sy = e[2];
        float dx = e[3] + offset_x;
        float dy = e[4] + offset_y;
        float w = e[5];
        float h = e[6];

        if (w <= 0.0f || h <= 0.0f) continue;

        if (set_number < 0) {
            candidates++;
            sprite_batch_draw(batch, 0, dx, dy, w, h,
                              0.0f, 0.0f, 1.0f, 1.0f,
                              0xFF000000, 0.5f);
            continue;
        }

        if (set_number >= num_tilesets) continue;

        const TilesetInfo *ts = &tilesets[set_number];
        if (ts->gl_texture == 0 || ts->width <= 0 || ts->height <= 0) continue;

        float inv_w = 1.0f / (float)ts->width;
        float inv_h = 1.0f / (float)ts->height;
        float u0 = sx * inv_w;
        float v0 = sy * inv_h;
        float u1 = (sx + w) * inv_w;
        float v1 = (sy + h) * inv_h;

        candidates++;
        sprite_batch_draw(batch, ts->gl_texture, dx, dy, w, h,
                          u0, v0, u1, v1, 0xFFFFFFFF, 1.0f);
    }
    return candidates - (sprite_batch_get_culled_count(batch) - culled_before);
}

int tilemap_draw_tiles_viewport(const float *elements, int count,
                                const TilesetInfo *tilesets, int num_tilesets,
                                float offset_x, float offset_y,
                                float viewport_width, float viewport_height,
                                uint32_t cache_token, SpriteBatch *batch)
{
    if (!elements || count <= 0 || !batch) return 0;

#if defined(RMMZ_GLES2)
    if (num_tilesets > 0 && tilesets &&
        viewport_width > 0.0f && viewport_height > 0.0f) {
        int result = draw_tiles_atlas(elements, count, tilesets, num_tilesets,
                                      offset_x, offset_y, viewport_width,
                                      viewport_height, cache_token, batch);
        if (result >= 0) return result;
    }
#else
    (void)viewport_width;
    (void)viewport_height;
    (void)cache_token;
#endif

    return draw_tiles_single_texture(elements, count, tilesets, num_tilesets,
                                     offset_x, offset_y, batch);
}

int tilemap_draw_tiles(const float *elements, int count,
                       const TilesetInfo *tilesets, int num_tilesets,
                       float offset_x, float offset_y,
                       SpriteBatch *batch)
{
    return tilemap_draw_tiles_viewport(elements, count, tilesets, num_tilesets,
                                       offset_x, offset_y, 816.0f, 624.0f,
                                       0, batch);
}

void tilemap_renderer_shutdown(void)
{
#if defined(RMMZ_GLES2)
    delete_atlases();
    if (s_tilemap.program) glDeleteProgram(s_tilemap.program);
    if (s_tilemap.vertex_buffer) glDeleteBuffers(1, &s_tilemap.vertex_buffer);
    if (s_tilemap.index_buffer) glDeleteBuffers(1, &s_tilemap.index_buffer);
    free(s_tilemap.vertices);
    free(s_tilemap.indices);
    free(s_tilemap.source_ids);
    free(s_tilemap.source_widths);
    free(s_tilemap.source_heights);
    memset(&s_tilemap, 0, sizeof(s_tilemap));
#endif
}

#ifdef RMMZ_STEAMLINK
void tilemap_set_profile_gpu_sync(bool enabled)
{
    s_tilemap.profile_gpu_sync = enabled;
}
#endif
