/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "engine/boot_console.h"

#include "platform/platform.h"
#include "engine/script_loader.h"

#include <ctype.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef RMMZ_HAS_GL
#include "rendering/gl_loader.h"

#define BOOT_LINE_COUNT 28
#define BOOT_LINE_LENGTH 512
#define BOOT_MAX_CHARS 160
#define BOOT_POINTS_PER_CHAR 35

typedef struct {
    char text[BOOT_LINE_LENGTH];
    LogLevel level;
} BootLine;

static const uint8_t s_glyphs[128][7] = {
    ['!'] = {4, 4, 4, 4, 4, 0, 4},
    ['"'] = {10, 10, 10, 0, 0, 0, 0},
    ['#'] = {10, 31, 10, 10, 31, 10, 0},
    ['\''] = {4, 4, 2, 0, 0, 0, 0},
    ['('] = {2, 4, 8, 8, 8, 4, 2},
    [')'] = {8, 4, 2, 2, 2, 4, 8},
    ['*'] = {0, 21, 14, 31, 14, 21, 0},
    ['+'] = {0, 4, 4, 31, 4, 4, 0},
    [','] = {0, 0, 0, 0, 0, 6, 4},
    ['-'] = {0, 0, 0, 31, 0, 0, 0},
    ['.'] = {0, 0, 0, 0, 0, 6, 6},
    ['/'] = {1, 2, 2, 4, 8, 8, 16},
    ['0'] = {14, 17, 19, 21, 25, 17, 14},
    ['1'] = {4, 12, 4, 4, 4, 4, 14},
    ['2'] = {14, 17, 1, 2, 4, 8, 31},
    ['3'] = {30, 1, 1, 14, 1, 1, 30},
    ['4'] = {2, 6, 10, 18, 31, 2, 2},
    ['5'] = {31, 16, 16, 30, 1, 1, 30},
    ['6'] = {14, 16, 16, 30, 17, 17, 14},
    ['7'] = {31, 1, 2, 4, 8, 8, 8},
    ['8'] = {14, 17, 17, 14, 17, 17, 14},
    ['9'] = {14, 17, 17, 15, 1, 1, 14},
    [':'] = {0, 4, 4, 0, 4, 4, 0},
    [';'] = {0, 4, 4, 0, 4, 4, 8},
    ['<'] = {2, 4, 8, 16, 8, 4, 2},
    ['='] = {0, 31, 0, 31, 0, 0, 0},
    ['>'] = {8, 4, 2, 1, 2, 4, 8},
    ['?'] = {14, 17, 1, 2, 4, 0, 4},
    ['A'] = {14, 17, 17, 31, 17, 17, 17},
    ['B'] = {30, 17, 17, 30, 17, 17, 30},
    ['C'] = {14, 17, 16, 16, 16, 17, 14},
    ['D'] = {30, 17, 17, 17, 17, 17, 30},
    ['E'] = {31, 16, 16, 30, 16, 16, 31},
    ['F'] = {31, 16, 16, 30, 16, 16, 16},
    ['G'] = {14, 17, 16, 23, 17, 17, 15},
    ['H'] = {17, 17, 17, 31, 17, 17, 17},
    ['I'] = {14, 4, 4, 4, 4, 4, 14},
    ['J'] = {7, 2, 2, 2, 18, 18, 12},
    ['K'] = {17, 18, 20, 24, 20, 18, 17},
    ['L'] = {16, 16, 16, 16, 16, 16, 31},
    ['M'] = {17, 27, 21, 21, 17, 17, 17},
    ['N'] = {17, 25, 25, 21, 19, 19, 17},
    ['O'] = {14, 17, 17, 17, 17, 17, 14},
    ['P'] = {30, 17, 17, 30, 16, 16, 16},
    ['Q'] = {14, 17, 17, 17, 21, 18, 13},
    ['R'] = {30, 17, 17, 30, 20, 18, 17},
    ['S'] = {15, 16, 16, 14, 1, 1, 30},
    ['T'] = {31, 4, 4, 4, 4, 4, 4},
    ['U'] = {17, 17, 17, 17, 17, 17, 14},
    ['V'] = {17, 17, 17, 17, 17, 10, 4},
    ['W'] = {17, 17, 17, 21, 21, 21, 10},
    ['X'] = {17, 17, 10, 4, 10, 17, 17},
    ['Y'] = {17, 17, 10, 4, 4, 4, 4},
    ['Z'] = {31, 1, 2, 4, 8, 16, 31},
    ['['] = {14, 8, 8, 8, 8, 8, 14},
    ['\\'] = {16, 8, 8, 4, 2, 2, 1},
    [']'] = {14, 2, 2, 2, 2, 2, 14},
    ['_'] = {0, 0, 0, 0, 0, 0, 31},
    ['|'] = {4, 4, 4, 4, 4, 4, 4},
};

static const char *s_vertex_shader =
    "attribute vec2 a_position;\n"
    "uniform vec2 u_resolution;\n"
    "uniform float u_point_size;\n"
    "void main() {\n"
    "  vec2 p = a_position / u_resolution;\n"
    "  gl_Position = vec4(p.x * 2.0 - 1.0, 1.0 - p.y * 2.0, 0.0, 1.0);\n"
    "  gl_PointSize = u_point_size;\n"
    "}\n";

static const char *s_fragment_shader =
    "precision mediump float;\n"
    "uniform vec4 u_color;\n"
    "void main() { gl_FragColor = u_color; }\n";

static Platform *s_platform;
static BootLine s_lines[BOOT_LINE_COUNT];
static size_t s_line_count;
static GLuint s_program;
static GLuint s_buffer;
#ifndef RMMZ_GLES2
static GLuint s_vertex_array;
#endif
static GLint s_resolution_uniform;
static GLint s_point_size_uniform;
static GLint s_color_uniform;
static bool s_active;
static bool s_deferred;
static bool s_dirty;

static GLuint compile_shader(GLenum type, const char *source)
{
    GLuint shader = glCreateShader(type);
    if (!shader) return 0;

    glShaderSource(shader, 1, &source, NULL);
    glCompileShader(shader);

    GLint compiled = GL_FALSE;
    glGetShaderiv(shader, GL_COMPILE_STATUS, &compiled);
    if (!compiled) {
        char message[512] = {0};
        glGetShaderInfoLog(shader, sizeof(message), NULL, message);
        fprintf(stderr, "boot_console: shader compilation failed: %s\n", message);
        glDeleteShader(shader);
        return 0;
    }
    return shader;
}

static bool create_program(void)
{
    GLuint vertex = compile_shader(GL_VERTEX_SHADER, s_vertex_shader);
    GLuint fragment = compile_shader(GL_FRAGMENT_SHADER, s_fragment_shader);
    if (!vertex || !fragment) {
        if (vertex) glDeleteShader(vertex);
        if (fragment) glDeleteShader(fragment);
        return false;
    }

    s_program = glCreateProgram();
    if (!s_program) {
        glDeleteShader(vertex);
        glDeleteShader(fragment);
        return false;
    }
    glAttachShader(s_program, vertex);
    glAttachShader(s_program, fragment);
    glBindAttribLocation(s_program, 0, "a_position");
    glLinkProgram(s_program);
    glDeleteShader(vertex);
    glDeleteShader(fragment);

    GLint linked = GL_FALSE;
    glGetProgramiv(s_program, GL_LINK_STATUS, &linked);
    if (!linked) {
        char message[512] = {0};
        glGetProgramInfoLog(s_program, sizeof(message), NULL, message);
        fprintf(stderr, "boot_console: program link failed: %s\n", message);
        glDeleteProgram(s_program);
        s_program = 0;
        return false;
    }

    s_resolution_uniform = glGetUniformLocation(s_program, "u_resolution");
    s_point_size_uniform = glGetUniformLocation(s_program, "u_point_size");
    s_color_uniform = glGetUniformLocation(s_program, "u_color");
    if (s_resolution_uniform < 0 || s_point_size_uniform < 0 || s_color_uniform < 0) {
        glDeleteProgram(s_program);
        s_program = 0;
        return false;
    }
    return true;
}

static void release_graphics(void)
{
    if (s_buffer) glDeleteBuffers(1, &s_buffer);
    s_buffer = 0;
#ifndef RMMZ_GLES2
    if (s_vertex_array) glDeleteVertexArrays(1, &s_vertex_array);
    s_vertex_array = 0;
#endif
    if (s_program) glDeleteProgram(s_program);
    s_program = 0;
}

static void append_line(LogLevel level, const char *text)
{
    if (s_line_count == BOOT_LINE_COUNT) {
        memmove(&s_lines[0], &s_lines[1],
                sizeof(s_lines[0]) * (BOOT_LINE_COUNT - 1));
        s_line_count--;
    }

    BootLine *line = &s_lines[s_line_count++];
    line->level = level;
    snprintf(line->text, sizeof(line->text), "[%s] %s",
             error_handler_level_name(level), text ? text : "");
}

static void color_for_level(LogLevel level, GLfloat color[4])
{
    color[3] = 1.0f;
    switch (level) {
        case LOG_DEBUG:
            color[0] = 0.55f; color[1] = 0.70f; color[2] = 0.85f;
            break;
        case LOG_WARN:
            color[0] = 1.00f; color[1] = 0.78f; color[2] = 0.30f;
            break;
        case LOG_ERROR:
            color[0] = 1.00f; color[1] = 0.38f; color[2] = 0.35f;
            break;
        default:
            color[0] = 0.88f; color[1] = 0.92f; color[2] = 0.98f;
            break;
    }
}

static int build_line_vertices(const char *text, int x, int y, int scale,
                               int max_chars, GLfloat *vertices)
{
    int count = 0;
    for (int i = 0; text[i] && i < max_chars; i++) {
        unsigned char ch = (unsigned char)text[i];
        if (islower(ch)) ch = (unsigned char)toupper(ch);
        if (ch < 32) ch = ' ';
        if (ch >= 128) ch = '?';

        for (int row = 0; row < 7; row++) {
            uint8_t bits = s_glyphs[ch][row];
            for (int col = 0; col < 5; col++) {
                if (bits & (1u << (4 - col))) {
                    vertices[count++] = (GLfloat)(x + (i * 6 + col) * scale) +
                                        (GLfloat)scale * 0.5f;
                    vertices[count++] = (GLfloat)(y + row * scale) +
                                        (GLfloat)scale * 0.5f;
                }
            }
        }
    }
    return count / 2;
}

static void draw_line(const char *text, LogLevel level, int x, int y,
                      int scale, int max_chars, GLfloat *vertices)
{
    int count = build_line_vertices(text, x, y, scale, max_chars, vertices);
    if (count == 0) return;

    glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(count * 2 * sizeof(GLfloat)),
                 vertices, GL_STREAM_DRAW);
    GLfloat color[4];
    color_for_level(level, color);
    glUniform4f(s_color_uniform, color[0], color[1], color[2], color[3]);
    glDrawArrays(GL_POINTS, 0, count);
}

static void draw_console(void)
{
    if (!s_active || !s_platform || !s_program || !s_buffer) return;

    int width = 0;
    int height = 0;
    platform_get_drawable_size(s_platform, &width, &height);
    if (width <= 0 || height <= 0) return;

    int scale = height / 240;
    if (scale < 2) scale = 2;
    if (scale > 4) scale = 4;
    int margin = scale * 4;
    int line_step = scale * 9;
    int max_chars = (width - margin * 2) / (scale * 6);
    if (max_chars > BOOT_MAX_CHARS) max_chars = BOOT_MAX_CHARS;
    if (max_chars <= 0) return;

    int visible = (height - margin * 2) / line_step - 1;
    if (visible < 0) visible = 0;
    if ((size_t)visible > s_line_count) visible = (int)s_line_count;
    size_t first = s_line_count - (size_t)visible;

    GLfloat vertices[BOOT_MAX_CHARS * BOOT_POINTS_PER_CHAR * 2];

    glViewport(0, 0, width, height);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    glDisable(GL_BLEND);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_DEPTH_TEST);
    glClearColor(0.018f, 0.026f, 0.045f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(s_program);
    glUniform2f(s_resolution_uniform, (GLfloat)width, (GLfloat)height);
    glUniform1f(s_point_size_uniform, (GLfloat)scale);
#ifndef RMMZ_GLES2
    glEnable(GL_PROGRAM_POINT_SIZE);
    glBindVertexArray(s_vertex_array);
#endif
    glBindBuffer(GL_ARRAY_BUFFER, s_buffer);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 0, NULL);

    const GLfloat title_color[4] = {0.35f, 0.78f, 1.0f, 1.0f};
    glUniform4f(s_color_uniform, title_color[0], title_color[1],
                title_color[2], title_color[3]);
    int y = margin;
    int title_count = build_line_vertices("OUTSIDER - STARTING", margin, y,
                                          scale, max_chars, vertices);
    if (title_count > 0) {
        glBufferData(GL_ARRAY_BUFFER,
                     (GLsizeiptr)(title_count * 2 * sizeof(GLfloat)),
                     vertices, GL_STREAM_DRAW);
        glDrawArrays(GL_POINTS, 0, title_count);
    }

    y += line_step;
    for (size_t i = first; i < s_line_count; i++, y += line_step) {
        draw_line(s_lines[i].text, s_lines[i].level, margin, y, scale,
                  max_chars, vertices);
    }

    glBindBuffer(GL_ARRAY_BUFFER, 0);
    s_dirty = false;
    platform_swap_buffers(s_platform);
}

bool boot_console_init(Platform *platform)
{
    if (!platform) return false;
    s_platform = platform;
    s_line_count = 0;
    if (!create_program()) {
        s_platform = NULL;
        return false;
    }

    glGenBuffers(1, &s_buffer);
    if (!s_buffer) {
        release_graphics();
        s_platform = NULL;
        return false;
    }
#ifndef RMMZ_GLES2
    glGenVertexArrays(1, &s_vertex_array);
    if (!s_vertex_array) {
        release_graphics();
        s_platform = NULL;
        return false;
    }
#endif

    s_active = true;
    s_deferred = false;
    s_dirty = true;
    platform_set_swap_interval(platform, false);
    draw_console();
    return true;
}

bool boot_console_is_active(void)
{
    return s_active;
}

void boot_console_defer_updates(void)
{
    s_deferred = true;
}

void boot_console_present_pending(void)
{
    if (s_active && s_dirty) draw_console();
}

void boot_console_log_callback(LogLevel level, const char *message, void *userdata)
{
    (void)userdata;
    if (!s_active) return;
    append_line(level, message);
    s_dirty = true;
    if (!s_deferred) draw_console();
}

void boot_console_hide(void)
{
    if (!s_active) return;
    s_active = false;
    s_deferred = false;
    error_handler_set_callback(NULL, NULL);
    script_loader_set_log_callback(NULL, NULL);
    release_graphics();
    platform_set_swap_interval(s_platform, true);
    s_platform = NULL;
}

void boot_console_shutdown(void)
{
    boot_console_hide();
}

#else

bool boot_console_init(Platform *platform)
{
    (void)platform;
    return false;
}

bool boot_console_is_active(void)
{
    return false;
}

void boot_console_defer_updates(void)
{
}

void boot_console_present_pending(void)
{
}

void boot_console_log_callback(LogLevel level, const char *message, void *userdata)
{
    (void)level;
    (void)message;
    (void)userdata;
}

void boot_console_hide(void)
{
}

void boot_console_shutdown(void)
{
}

#endif
