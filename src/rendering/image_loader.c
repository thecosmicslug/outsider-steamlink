/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "rendering/image_loader.h"
#include "io/file_io.h"

#include <stdio.h>
#include <stdarg.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>   /* INT_MAX */
#include <time.h>

/* stb_image implementation lives in this translation unit. */
#define STB_IMAGE_IMPLEMENTATION
/* Browsers decode by content, not extension: games ship JPEG data under
   .png names now and then (Pocket Mirror's perlinNoise.png). */
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO   /* We handle file I/O ourselves. */
#include "stb_image.h"

#include "rendering/renderer.h"

typedef struct CacheEntry {
    char        *key;       /* File path or blob key (owned). */
    uint8_t     *pixels;    /* RGBA pixel data (stbi_image_free). */
    int          width;
    int          height;
    uint32_t     gl_texture; /* Renderer texture id, 0 if none */
    bool         reload_from_file;
    ImageHandle  handle;
    int          refcount;  /* Loads sharing this entry; freed when it hits 0. */
    struct CacheEntry *next;
} CacheEntry;

#define CACHE_BUCKETS 256
#define MAX_HANDLES   4096

static CacheEntry *s_cache[CACHE_BUCKETS];
static CacheEntry *s_handle_index[MAX_HANDLES]; /* Direct lookup by handle. */
static bool         s_gl_enabled  = false;
static bool         s_profile_loads = false;
static LogCallback  s_profile_log_callback;
static void        *s_profile_log_userdata;
static size_t       s_cache_count = 0;

void image_loader_set_load_profile(bool enabled, LogCallback callback, void *userdata)
{
    s_profile_loads = enabled;
    s_profile_log_callback = callback;
    s_profile_log_userdata = userdata;
}

static double profile_time_ms(void)
{
    struct timespec ts;
#ifdef _WIN32
    timespec_get(&ts, TIME_UTC);
#else
    clock_gettime(CLOCK_MONOTONIC, &ts);
#endif
    return (double)ts.tv_sec * 1000.0 + (double)ts.tv_nsec / 1000000.0;
}

static void profile_log(const char *fmt, ...)
{
    char message[1024];
    va_list args;
    va_start(args, fmt);
    vsnprintf(message, sizeof(message), fmt, args);
    va_end(args);

    if (s_profile_log_callback) {
        s_profile_log_callback(LOG_INFO, message, s_profile_log_userdata);
    } else {
        fprintf(stderr, "%s\n", message);
    }
}

/* FNV-1a hash of a cache key, reduced to a bucket index. */
static unsigned int hash_key(const char *key)
{
    unsigned int h = 2166136261u;
    for (const char *p = key; *p; p++) {
        h ^= (unsigned char)*p;
        h *= 16777619u;
    }
    return h % CACHE_BUCKETS;
}

static CacheEntry *cache_find_by_key(const char *key)
{
    unsigned int bucket = hash_key(key);
    for (CacheEntry *e = s_cache[bucket]; e; e = e->next) {
        if (strcmp(e->key, key) == 0) return e;
    }
    return NULL;
}

static CacheEntry *cache_find_by_handle(ImageHandle handle)
{
    if (handle == 0 || handle >= MAX_HANDLES) return NULL;
    return s_handle_index[handle];
}

/* Texture creation goes through the renderer so the image cache works with
   whichever rendering backend was built. */
static uint32_t upload_gl_texture(const uint8_t *pixels, int w, int h)
{
    uint32_t tex = renderer_create_texture(w, h);
    if (!tex) return 0;
    renderer_update_texture(tex, w, h, pixels);
    return tex;
}

uint32_t image_upload_rgba_texture(const uint8_t *rgba, int width, int height)
{
    if (!s_gl_enabled || !rgba || width <= 0 || height <= 0) return 0;
    return upload_gl_texture(rgba, width, height);
}

static void delete_gl_texture(uint32_t tex)
{
    if (tex) renderer_delete_texture(tex);
}

static CacheEntry *cache_insert(const char *key, uint8_t *pixels,
                                int w, int h, bool reload_from_file)
{
    CacheEntry *entry = calloc(1, sizeof(CacheEntry));
    if (!entry) return NULL;

    ImageHandle new_handle = 0;
    for (ImageHandle i = 1; i < MAX_HANDLES; i++) {
        if (s_handle_index[i] == NULL) {
            new_handle = i;
            break;
        }
    }
    if (new_handle == 0) {
        fprintf(stderr, "image_loader: handle limit exceeded\n");
        free(entry);
        return NULL;
    }

    entry->key    = strdup(key);
    if (!entry->key) {
        fprintf(stderr, "image_loader: out of memory duplicating cache key\n");
        free(entry);
        return NULL;
    }
    entry->pixels = pixels;
    entry->width  = w;
    entry->height = h;
    entry->reload_from_file = reload_from_file;
    entry->handle = new_handle;
    entry->refcount = 1;

    if (s_gl_enabled) {
        double upload_start = s_profile_loads ? profile_time_ms() : 0.0;
        entry->gl_texture = upload_gl_texture(pixels, w, h);
        if (s_profile_loads) {
            profile_log("[load] texture-upload key=%s size=%dx%d time=%.2fms",
                        key, w, h, profile_time_ms() - upload_start);
        }
        if (reload_from_file && entry->gl_texture) {
            stbi_image_free(entry->pixels);
            entry->pixels = NULL;
        }
    }

    unsigned int bucket = hash_key(key);
    entry->next = s_cache[bucket];
    s_cache[bucket] = entry;
    s_handle_index[entry->handle] = entry;
    s_cache_count++;

    return entry;
}

static void cache_remove(CacheEntry *entry)
{
    unsigned int bucket = hash_key(entry->key);
    CacheEntry **pp = &s_cache[bucket];
    while (*pp) {
        if (*pp == entry) {
            *pp = entry->next;
            break;
        }
        pp = &(*pp)->next;
    }

    if (entry->handle < MAX_HANDLES) {
        s_handle_index[entry->handle] = NULL;
    }
    delete_gl_texture(entry->gl_texture);
    stbi_image_free(entry->pixels);
    free(entry->key);
    free(entry);
    s_cache_count--;
}

void image_loader_init(bool enable_gl)
{
    s_gl_enabled = enable_gl;
    s_cache_count = 0;
    memset(s_cache, 0, sizeof(s_cache));
    memset(s_handle_index, 0, sizeof(s_handle_index));
}

void image_loader_shutdown(void)
{
    image_cache_clear();
}

ImageHandle image_load(const char *path)
{
    if (!path) return IMAGE_HANDLE_INVALID;

    CacheEntry *existing = cache_find_by_key(path);
    if (existing) {
        /* Each sharing loader must balance this with image_free. */
        existing->refcount++;
        if (s_profile_loads) {
            profile_log("[load] image-cache-hit key=%s refs=%d",
                        path, existing->refcount);
        }
        return existing->handle;
    }

    size_t file_size = 0;
    double read_start = s_profile_loads ? profile_time_ms() : 0.0;
    uint8_t *file_data = file_io_read_binary(path, &file_size);
    if (s_profile_loads && file_data) {
        profile_log("[load] image-read path=%s bytes=%zu time=%.2fms",
                    path, file_size, profile_time_ms() - read_start);
    }
    if (!file_data) {
        fprintf(stderr, "image_load: failed to read file: %s\n", path);
        return IMAGE_HANDLE_INVALID;
    }

    if (file_size > INT_MAX) {
        fprintf(stderr, "image_load: file too large: %s\n", path);
        free(file_data);
        return IMAGE_HANDLE_INVALID;
    }
    int w, h, channels;
    double decode_start = s_profile_loads ? profile_time_ms() : 0.0;
    uint8_t *pixels = stbi_load_from_memory(file_data, (int)file_size,
                                            &w, &h, &channels, 4);
    if (s_profile_loads && pixels) {
        profile_log("[load] image-decode path=%s size=%dx%d time=%.2fms",
                    path, w, h, profile_time_ms() - decode_start);
    }

    if (!pixels) {
        fprintf(stderr, "image_load: failed to decode image: %s (%s)\n",
                path, stbi_failure_reason());
        free(file_data);
        return IMAGE_HANDLE_INVALID;
    }

    CacheEntry *entry = cache_insert(path, pixels, w, h, true);
    free(file_data);
    if (!entry) {
        stbi_image_free(pixels);
        return IMAGE_HANDLE_INVALID;
    }

    return entry->handle;
}

ImageHandle image_load_from_memory(const char *key,
                                   const uint8_t *data, size_t size)
{
    if (!key) return IMAGE_HANDLE_INVALID;

    /* A cache hit is valid even when data is NULL. */
    CacheEntry *existing = cache_find_by_key(key);
    if (existing) {
        existing->refcount++;
        if (s_profile_loads) {
            profile_log("[load] image-cache-hit key=%s refs=%d",
                        key, existing->refcount);
        }
        return existing->handle;
    }

    if (!data || size == 0) return IMAGE_HANDLE_INVALID;
    if (size > INT_MAX) return IMAGE_HANDLE_INVALID;

    int w, h, channels;
    double decode_start = s_profile_loads ? profile_time_ms() : 0.0;
    uint8_t *pixels = stbi_load_from_memory(data, (int)size,
                                            &w, &h, &channels, 4);
    if (s_profile_loads && pixels) {
        profile_log("[load] image-decode key=%s size=%dx%d time=%.2fms",
                    key, w, h, profile_time_ms() - decode_start);
    }
    if (!pixels) {
        fprintf(stderr, "image_load_from_memory: failed to decode (%s)\n",
                stbi_failure_reason());
        return IMAGE_HANDLE_INVALID;
    }

    CacheEntry *entry = cache_insert(key, pixels, w, h, false);
    if (!entry) {
        stbi_image_free(pixels);
        return IMAGE_HANDLE_INVALID;
    }

    return entry->handle;
}

bool image_get_info(ImageHandle handle, ImageInfo *out)
{
    if (handle == IMAGE_HANDLE_INVALID || !out) return false;

    CacheEntry *e = cache_find_by_handle(handle);
    if (!e) return false;

    out->width      = e->width;
    out->height     = e->height;
    out->gl_texture = e->gl_texture;
    return true;
}

const uint8_t *image_get_pixels(ImageHandle handle)
{
    if (handle == IMAGE_HANDLE_INVALID) return NULL;
    CacheEntry *e = cache_find_by_handle(handle);
    if (!e) return NULL;
    if (!e->pixels && e->reload_from_file) {
        size_t file_size = 0;
        uint8_t *file_data = file_io_read_binary(e->key, &file_size);
        if (!file_data) {
            fprintf(stderr, "image_get_pixels: failed to reread image: %s\n", e->key);
            return NULL;
        }
        if (file_size > INT_MAX) {
            fprintf(stderr, "image_get_pixels: file too large: %s\n", e->key);
            free(file_data);
            return NULL;
        }
        int w, h, channels;
        uint8_t *pixels = stbi_load_from_memory(file_data, (int)file_size,
                                                &w, &h, &channels, 4);
        free(file_data);
        if (!pixels) {
            fprintf(stderr, "image_get_pixels: failed to decode image: %s (%s)\n",
                    e->key, stbi_failure_reason());
            return NULL;
        }
        if (w != e->width || h != e->height) {
            fprintf(stderr, "image_get_pixels: image dimensions changed: %s\n", e->key);
            stbi_image_free(pixels);
            return NULL;
        }
        e->pixels = pixels;
    }
    return e->pixels;
}

void image_release_pixels(ImageHandle handle)
{
    CacheEntry *e = cache_find_by_handle(handle);
    if (e && e->reload_from_file && e->pixels) {
        stbi_image_free(e->pixels);
        e->pixels = NULL;
    }
}

void image_free(ImageHandle handle)
{
    if (handle == IMAGE_HANDLE_INVALID) return;
    CacheEntry *e = cache_find_by_handle(handle);
    if (!e) return;
    /* Entries may be shared by several loads; pixels and the GL texture are
       released only when the last holder is gone. */
    if (--e->refcount <= 0) cache_remove(e);
}

size_t image_cache_count(void)
{
    return s_cache_count;
}

void image_cache_clear(void)
{
    for (int i = 0; i < CACHE_BUCKETS; i++) {
        CacheEntry *e = s_cache[i];
        while (e) {
            CacheEntry *next = e->next;
            delete_gl_texture(e->gl_texture);
            stbi_image_free(e->pixels);
            free(e->key);
            free(e);
            e = next;
        }
        s_cache[i] = NULL;
    }
    s_cache_count = 0;
    memset(s_handle_index, 0, sizeof(s_handle_index));
}
