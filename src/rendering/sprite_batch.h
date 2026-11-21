/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_SPRITE_BATCH_H
#define RMMZ_SPRITE_BATCH_H

#include <stdbool.h>
#include <stdint.h>

/* Blend mode constants matching PIXI.BLEND_MODES. */
enum {
    BLEND_MODE_NORMAL   = 0,
    BLEND_MODE_ADD      = 1,
    BLEND_MODE_MULTIPLY = 2,
    BLEND_MODE_SCREEN   = 3,
    /* Internal: source already has premultiplied alpha (an FBO produced by
       the NORMAL mode above). Used to composite filter output back. */
    BLEND_MODE_NORMAL_PREMULT = 4,
};

/* Batched quad renderer that groups quads by texture and flushes them in
   minimal indexed draw calls, culling quads that fall outside the viewport. */
typedef struct SpriteBatch SpriteBatch;

/* Create a sprite batch with the given maximum number of quads per batch.
   Typically 4096. Returns NULL on failure. */
SpriteBatch *sprite_batch_create(int max_quads);

/* Destroy the sprite batch and free all resources. */
void sprite_batch_destroy(SpriteBatch *sb);

/* Set the orthographic projection matrix (top-left origin).
   Also sets the viewport bounds for frustum culling. */
void sprite_batch_set_projection(SpriteBatch *sb, float width, float height);

/* Begin a new batch frame. Must be called before draw calls. */
void sprite_batch_begin(SpriteBatch *sb);

/* Queue an axis-aligned textured quad (texture 0 = untextured white).
   UVs are normalized; tint is ARGB multiplied with the texture. */
void sprite_batch_draw(SpriteBatch *sb, uint32_t texture,
                       float x, float y, float w, float h,
                       float u0, float v0, float u1, float v1,
                       uint32_t tint, float alpha);

/* Queue a quad from four transformed corners in TL, TR, BR, BL order, which
   preserves rotation, skew and mirroring. UVs map TL=(u0,v0) .. BR=(u1,v1). */
void sprite_batch_draw_verts(SpriteBatch *sb, uint32_t texture,
                             float x0, float y0, float x1, float y1,
                             float x2, float y2, float x3, float y3,
                             float u0, float v0, float u1, float v1,
                             uint32_t tint, float alpha);

/* Like sprite_batch_draw_verts, but with a UV per corner (same TL, TR, BR,
   BL order), for textured Graphics fills whose texture space is not
   axis-aligned with the shape. */
void sprite_batch_draw_verts_uv(SpriteBatch *sb, uint32_t texture,
                                const float xy[8], const float uv[8],
                                uint32_t tint, float alpha);

/* Flush any queued quads to the GPU. Called automatically on texture
   change, batch full, or end of frame. */
void sprite_batch_flush(SpriteBatch *sb);

/* End the batch frame. Flushes remaining quads. */
void sprite_batch_end(SpriteBatch *sb);

/* Change blend mode. Flushes current batch if mode differs. */
void sprite_batch_set_blend_mode(SpriteBatch *sb, int mode);

/* Get the number of draw calls in the current/last frame. */
int sprite_batch_get_draw_calls(const SpriteBatch *sb);

/* Get the number of quads drawn in the current/last frame. */
int sprite_batch_get_quad_count(const SpriteBatch *sb);

/* Get the number of quads culled (off-screen) in the current/last frame. */
int sprite_batch_get_culled_count(const SpriteBatch *sb);

/* Include an externally-issued draw in this frame's statistics. */
void sprite_batch_record_external_draw(SpriteBatch *sb, int quad_count);

/* Re-bind shader, VAO, and projection after external GL state changes
   (e.g., after filter rendering uses a different shader). */
void sprite_batch_rebind(SpriteBatch *sb);

#ifdef RMMZ_STEAMLINK
typedef struct {
    double wait_ms;
    unsigned int batch_count;
    double slowest_wait_ms;
    unsigned int slowest_quads;
    uint32_t slowest_texture;
    int slowest_blend;
    float slowest_x;
    float slowest_y;
    float slowest_width;
    float slowest_height;
    bool slowest_white_texture;
    float slowest_r;
    float slowest_g;
    float slowest_b;
    float slowest_a;
} SpriteBatchProfileGroupStats;

typedef struct {
    SpriteBatchProfileGroupStats sprites;
    SpriteBatchProfileGroupStats premult_composites;
    unsigned int blend_disabled_batches;
    unsigned int full_screen_clear_batches;
} SpriteBatchProfileStats;

void sprite_batch_set_profile_gpu_sync(bool enabled);
void sprite_batch_take_profile_gpu_stats(SpriteBatchProfileStats *stats);
#endif

#endif /* RMMZ_SPRITE_BATCH_H */
