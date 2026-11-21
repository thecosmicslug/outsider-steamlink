/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/renderer.h"
#include "rendering/filters.h"
#ifdef RMMZ_GLES2
#include "rendering/tilemap.h"
#endif

#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

#include <stdio.h> /* for fprintf in FBO error path */
#include <math.h>  /* for floorf, ceilf in scissor transform */

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"
#endif

static float s_render_scale = 1.0f;

static int scaled_dimension(int dimension)
{
    if (dimension <= 0) return dimension;
    int scaled = (int)floorf(dimension * s_render_scale + 0.5f);
    return scaled > 0 ? scaled : 1;
}

bool renderer_set_render_scale(float scale)
{
    if (!(scale >= 0.5f && scale <= 1.0f)) return false;
    s_render_scale = scale;
    return true;
}

float renderer_get_render_scale(void)
{
    return s_render_scale;
}

struct Renderer {
    SpriteBatch *batch;
    int          width;       /* game logical width (projection) */
    int          height;      /* game logical height (projection) */
    int          vp_x;        /* screen viewport X offset (letterbox) */
    int          vp_y;        /* screen viewport Y offset (letterbox) */
    int          vp_w;        /* screen viewport width in pixels */
    int          vp_h;        /* screen viewport height in pixels */
    uint32_t     bound_fbo;   /* currently bound FBO, 0 = default */

    /* The frame is composed here and blitted to the window on present, so a
       screenshot reads the same pixels on every backend and rendering does
       not depend on the window being visible. */
    uint32_t     screen_fbo;
    uint32_t     screen_tex;
    int          out_w;
    int          out_h;
#ifdef RMMZ_GLES2
    uint32_t     present_program;
#endif
};

Renderer *renderer_create(int width, int height)
{
    Renderer *r = calloc(1, sizeof(Renderer));
    if (!r) return NULL;

    r->width = width;
    r->height = height;
    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = scaled_dimension(width);
    r->vp_h = scaled_dimension(height);

    r->batch = sprite_batch_create(4096);
    if (!r->batch) {
        free(r);
        return NULL;
    }

    sprite_batch_set_projection(r->batch, (float)width, (float)height);

#ifdef RMMZ_HAS_GL
    glViewport(0, 0, r->vp_w, r->vp_h);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);

    fprintf(stderr, "[GL] Version: %s\n", glGetString(GL_VERSION));
    fprintf(stderr, "[GL] Renderer: %s\n", glGetString(GL_RENDERER));
    fprintf(stderr, "[GL] Vendor: %s\n", glGetString(GL_VENDOR));
    fprintf(stderr, "[GL] GLSL: %s\n", glGetString(GL_SHADING_LANGUAGE_VERSION));
#endif
#ifdef RMMZ_GLES2
    filters_init();
    r->present_program = filter_compile_shader(NULL, filter_copy_frag_src());
    if (!r->present_program) {
        sprite_batch_destroy(r->batch);
        free(r);
        return NULL;
    }
#endif

    return r;
}

void renderer_destroy(Renderer *r)
{
    if (!r) return;
#ifdef RMMZ_GLES2
    tilemap_renderer_shutdown();
#endif
#ifdef RMMZ_HAS_GL
    if (r->screen_fbo) renderer_delete_fbo(r->screen_fbo, r->screen_tex);
#endif
#ifdef RMMZ_GLES2
    filter_delete_shader(r->present_program);
#endif
    sprite_batch_destroy(r->batch);
    free(r);
}

#ifdef RMMZ_HAS_GL
static void ensure_screen_fbo(Renderer *r)
{
    if (r->out_w <= 0 || r->out_h <= 0) return;
    if (r->screen_fbo) return;
    r->screen_fbo = renderer_create_fbo(r->out_w, r->out_h, &r->screen_tex);
    if (r->screen_tex) renderer_set_texture_filter(r->screen_tex, true);
}
#endif

void renderer_set_output_size(Renderer *r, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return;
    if (r->out_w == width && r->out_h == height) return;
    r->out_w = width;
    r->out_h = height;
#ifdef RMMZ_HAS_GL
    if (r->screen_fbo) {
        renderer_delete_fbo(r->screen_fbo, r->screen_tex);
        r->screen_fbo = 0;
        r->screen_tex = 0;
    }
    ensure_screen_fbo(r);
#endif
}

void renderer_resize(Renderer *r, int width, int height)
{
    if (!r) return;
    r->width = width;
    r->height = height;
    sprite_batch_set_projection(r->batch, (float)width, (float)height);

    /* Reset to a full viewport; the JS side re-applies letterboxing afterwards. */
    r->vp_x = 0;
    r->vp_y = 0;
    r->vp_w = scaled_dimension(width);
    r->vp_h = scaled_dimension(height);

#ifdef RMMZ_HAS_GL
    glViewport(0, 0, r->vp_w, r->vp_h);
#endif
}

void renderer_get_size(const Renderer *r, int *out_w, int *out_h)
{
    if (!r) return;
    if (out_w) *out_w = r->width;
    if (out_h) *out_h = r->height;
}

void renderer_set_screen_viewport(Renderer *r, int x, int y, int w, int h)
{
    if (!r) return;
    r->vp_x = scaled_dimension(x);
    r->vp_y = scaled_dimension(y);
    r->vp_w = scaled_dimension(w);
    r->vp_h = scaled_dimension(h);

    /* Apply immediately if we're rendering to the default framebuffer. */
    if (r->bound_fbo == 0) {
#ifdef RMMZ_HAS_GL
        glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
#endif
    }
}

#ifdef RMMZ_HAS_GL
/* Clear the whole target even while a scissor is active. A filter pass may
   start inside a scissor-clipped window; if its (pooled) framebuffer were
   only cleared within the clip, stale pixels from an earlier pass would be
   composited back over the screen. */
static void clear_unscissored(void)
{
    GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);
    if (scissored) glDisable(GL_SCISSOR_TEST);
    glClear(GL_COLOR_BUFFER_BIT);
    if (scissored) glEnable(GL_SCISSOR_TEST);
}
#endif

void renderer_begin_frame(Renderer *r)
{
    if (!r) return;

#ifdef RMMZ_HAS_GL
    /* When an FBO is bound, renderer_bind_fbo already set its viewport. */
    if (r->bound_fbo == 0) {
        ensure_screen_fbo(r);
        glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
        glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    }
    clear_unscissored();
#endif

    sprite_batch_begin(r->batch);
}

void renderer_begin_frame_transparent(Renderer *r)
{
    if (!r) return;

#ifdef RMMZ_HAS_GL
    if (r->bound_fbo == 0) {
        ensure_screen_fbo(r);
        glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
        glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
    }
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    clear_unscissored();
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
#endif

    sprite_batch_begin(r->batch);
}

#ifdef RMMZ_GLES2
void renderer_begin_frame_no_clear(Renderer *r)
{
    if (!r) return;
    sprite_batch_begin(r->batch);
}
#endif

void renderer_end_frame(Renderer *r)
{
    if (!r) return;
    sprite_batch_end(r->batch);

}

void renderer_present(Renderer *r)
{
    if (!r) return;
#ifdef RMMZ_HAS_GL
    if (!r->screen_fbo || r->out_w <= 0 || r->out_h <= 0) return;
#ifdef RMMZ_GLES2
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glViewport(0, 0, r->out_w, r->out_h);
    glDisable(GL_SCISSOR_TEST);
    filters_init();
    filter_begin(r->present_program, r->screen_tex, r->out_w, r->out_h);
    glDisable(GL_BLEND);
    filter_draw_quad();
    filter_end();
#else
    /* Preserve the letterboxing baked into the offscreen surface while
       scaling it to the full window. */
    glBindFramebuffer(GL_READ_FRAMEBUFFER, r->screen_fbo);
    glBindFramebuffer(GL_DRAW_FRAMEBUFFER, 0);
    glDisable(GL_SCISSOR_TEST);
    glBlitFramebuffer(0, 0, scaled_dimension(r->out_w), scaled_dimension(r->out_h),
                      0, 0, r->out_w, r->out_h,
                      GL_COLOR_BUFFER_BIT, GL_LINEAR);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
#endif
#endif
}

SpriteBatch *renderer_get_batch(Renderer *r)
{
    return r ? r->batch : NULL;
}

/* Texture management */

uint32_t renderer_create_texture(int width, int height)
{
#ifdef RMMZ_HAS_GL
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
    return (uint32_t)tex;
#else
    (void)width; (void)height;
    return 0;
#endif
}

void renderer_update_texture(uint32_t texture, int width, int height,
                             const uint8_t *pixels)
{
#ifdef RMMZ_HAS_GL
    if (!texture) return;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, texture);
    glTexSubImage2D(GL_TEXTURE_2D, 0, 0, 0, width, height,
                    GL_RGBA, GL_UNSIGNED_BYTE, pixels);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
#else
    (void)texture; (void)width; (void)height; (void)pixels;
#endif
}

void renderer_set_texture_filter(uint32_t texture, bool linear)
{
#ifdef RMMZ_HAS_GL
    if (texture == 0) return;
    GLint f = linear ? GL_LINEAR : GL_NEAREST;
    GLint previous = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous);
    glBindTexture(GL_TEXTURE_2D, (GLuint)texture);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, f);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, f);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous);
#else
    (void)texture; (void)linear;
#endif
}

void renderer_delete_texture(uint32_t texture)
{
#ifdef RMMZ_HAS_GL
    if (texture) {
        GLuint t = texture;
        glDeleteTextures(1, &t);
    }
#else
    (void)texture;
#endif
}

/* Framebuffer objects */

uint32_t renderer_create_fbo(int width, int height, uint32_t *out_texture)
{
#ifdef RMMZ_HAS_GL
    int target_width = scaled_dimension(width);
    int target_height = scaled_dimension(height);
    GLint previous_texture = 0, previous_fbo = 0;
    glGetIntegerv(GL_TEXTURE_BINDING_2D, &previous_texture);
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &previous_fbo);
    GLuint tex = 0, fbo = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, target_width, target_height, 0,
                 GL_RGBA, GL_UNSIGNED_BYTE, NULL);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glGenFramebuffers(1, &fbo);
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);

    GLenum status = glCheckFramebufferStatus(GL_FRAMEBUFFER);
    if (status != GL_FRAMEBUFFER_COMPLETE) {
        fprintf(stderr, "renderer: FBO incomplete (status 0x%X)\n", status);
        glDeleteFramebuffers(1, &fbo);
        glDeleteTextures(1, &tex);
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
        glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);
        return 0;
    }

    GLboolean scissored = glIsEnabled(GL_SCISSOR_TEST);
    if (scissored) glDisable(GL_SCISSOR_TEST);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);
    glClear(GL_COLOR_BUFFER_BIT);
    glClearColor(0.0f, 0.0f, 0.0f, 1.0f);
    if (scissored) glEnable(GL_SCISSOR_TEST);
    glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)previous_fbo);
    glBindTexture(GL_TEXTURE_2D, (GLuint)previous_texture);

    if (out_texture) *out_texture = (uint32_t)tex;
    return (uint32_t)fbo;
#else
    (void)width; (void)height;
    if (out_texture) *out_texture = 0;
    return 0;
#endif
}

void renderer_delete_fbo(uint32_t fbo, uint32_t texture)
{
#ifdef RMMZ_HAS_GL
    if (fbo) {
        GLuint f = fbo;
        glDeleteFramebuffers(1, &f);
    }
    if (texture) {
        GLuint t = texture;
        glDeleteTextures(1, &t);
    }
#else
    (void)fbo; (void)texture;
#endif
}

void renderer_bind_fbo(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r) return;

    /* Flush any pending draws before switching targets. */
    sprite_batch_flush(r->batch);
    r->bound_fbo = fbo;

#ifdef RMMZ_HAS_GL
    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, scaled_dimension(width), scaled_dimension(height));
#else
    (void)width; (void)height;
#endif

    sprite_batch_set_projection(r->batch, (float)width, (float)height);
}

void renderer_unbind_fbo(Renderer *r)
{
    if (!r) return;

    sprite_batch_flush(r->batch);
    r->bound_fbo = 0;

#ifdef RMMZ_HAS_GL
    glBindFramebuffer(GL_FRAMEBUFFER, r->screen_fbo);
    glViewport(r->vp_x, r->vp_y, r->vp_w, r->vp_h);
#endif

    sprite_batch_set_projection(r->batch, (float)r->width, (float)r->height);
}

uint8_t *renderer_read_pixels(Renderer *r, uint32_t fbo, int width, int height)
{
    if (!r || width <= 0 || height <= 0) return NULL;

    /* Guard against size_t overflow on 32-bit targets. */
    uint64_t alloc_size = (uint64_t)width * (uint64_t)height * 4;
    if (alloc_size > SIZE_MAX) return NULL;

    uint8_t *pixels = malloc((size_t)alloc_size);
    if (!pixels) return NULL;

#ifdef RMMZ_HAS_GL
    /* Flush pending draws before reading. */
    sprite_batch_flush(r->batch);

    /* 0 means "the screen", which is the offscreen surface the frame was
       composed into rather than the window itself. */
    if (fbo == 0 && r->screen_fbo) fbo = r->screen_fbo;

    GLint prev_fbo = 0;
    glGetIntegerv(GL_FRAMEBUFFER_BINDING, &prev_fbo);

    int read_width = scaled_dimension(width);
    int read_height = scaled_dimension(height);
    uint64_t read_size = (uint64_t)read_width * (uint64_t)read_height * 4;
    if (read_size > SIZE_MAX) {
        free(pixels);
        return NULL;
    }
    uint8_t *readback = (read_width == width && read_height == height)
        ? pixels : malloc((size_t)read_size);
    if (!readback) {
        free(pixels);
        return NULL;
    }

    if ((uint32_t)prev_fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    }

    glReadPixels(0, 0, read_width, read_height, GL_RGBA, GL_UNSIGNED_BYTE, readback);

    /* OpenGL reads bottom-up; flip to top-down. */
    size_t row_bytes = (size_t)read_width * 4;
    uint8_t *tmp = malloc(row_bytes);
    if (!tmp) {
        if (readback != pixels) free(readback);
        free(pixels);
        if ((uint32_t)prev_fbo != fbo) {
            glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
        }
        return NULL;
    }
    for (int y = 0; y < read_height / 2; y++) {
        uint8_t *top = readback + y * row_bytes;
        uint8_t *bot = readback + (read_height - 1 - y) * row_bytes;
        memcpy(tmp, top, row_bytes);
        memcpy(top, bot, row_bytes);
        memcpy(bot, tmp, row_bytes);
    }
    free(tmp);

    if (readback != pixels) {
        for (int y = 0; y < height; y++) {
            float source_y = ((y + 0.5f) * read_height / height) - 0.5f;
            if (source_y < 0.0f) source_y = 0.0f;
            int y0 = (int)source_y;
            int y1 = y0 + 1 < read_height ? y0 + 1 : y0;
            float fy = source_y - y0;
            for (int x = 0; x < width; x++) {
                float source_x = ((x + 0.5f) * read_width / width) - 0.5f;
                if (source_x < 0.0f) source_x = 0.0f;
                int x0 = (int)source_x;
                int x1 = x0 + 1 < read_width ? x0 + 1 : x0;
                float fx = source_x - x0;
                const uint8_t *p00 = readback + ((size_t)y0 * read_width + x0) * 4;
                const uint8_t *p10 = readback + ((size_t)y0 * read_width + x1) * 4;
                const uint8_t *p01 = readback + ((size_t)y1 * read_width + x0) * 4;
                const uint8_t *p11 = readback + ((size_t)y1 * read_width + x1) * 4;
                uint8_t *out = pixels + ((size_t)y * width + x) * 4;
                for (int c = 0; c < 4; c++) {
                    float top = p00[c] + (p10[c] - p00[c]) * fx;
                    float bottom = p01[c] + (p11[c] - p01[c]) * fx;
                    out[c] = (uint8_t)(top + (bottom - top) * fy + 0.5f);
                }
            }
        }
        free(readback);
    }

    /* Restore previous FBO. */
    if ((uint32_t)prev_fbo != fbo) {
        glBindFramebuffer(GL_FRAMEBUFFER, (GLuint)prev_fbo);
    }
#else
    memset(pixels, 0, (size_t)width * (size_t)height * 4);
#endif

    return pixels;
}

void renderer_rebind_batch(Renderer *r)
{
    if (r && r->batch) sprite_batch_rebind(r->batch);
}

void renderer_set_scissor(Renderer *r, int x, int y, int w, int h)
{
#ifdef RMMZ_HAS_GL
    if (!r) return;

    /* On the default framebuffer, map game-space to the letterbox viewport.
       Floor the origin and ceil the far edge so fractional scaling never
       clips boundary pixels. */
    if (r->bound_fbo == 0 && r->width > 0 && r->height > 0) {
        float sx = (float)r->vp_w / (float)r->width;
        float sy = (float)r->vp_h / (float)r->height;
        int x1 = (int)floorf(x * sx + r->vp_x);
        int y1 = (int)floorf(y * sy + r->vp_y);
        int x2 = (int)ceilf((x + w) * sx + r->vp_x);
        int y2 = (int)ceilf((y + h) * sy + r->vp_y);
        x = x1;
        y = y1;
        w = x2 - x1;
        h = y2 - y1;
    } else if (r->bound_fbo != 0) {
        int x1 = (int)floorf(x * s_render_scale);
        int y1 = (int)floorf(y * s_render_scale);
        int x2 = (int)ceilf((x + w) * s_render_scale);
        int y2 = (int)ceilf((y + h) * s_render_scale);
        x = x1;
        y = y1;
        w = x2 - x1;
        h = y2 - y1;
    }

    glEnable(GL_SCISSOR_TEST);
    glScissor(x, y, w, h);
#else
    (void)r; (void)x; (void)y; (void)w; (void)h;
#endif
}

void renderer_clear_scissor(Renderer *r)
{
#ifdef RMMZ_HAS_GL
    (void)r;
    glDisable(GL_SCISSOR_TEST);
#else
    (void)r;
#endif
}
