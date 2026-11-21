/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_GL_LOADER_H
#define RMMZ_GL_LOADER_H

/*
 * OpenGL function loader. The renderer targets OpenGL 4.5 core, but on Windows
 * opengl32.dll only exports GL 1.1, so every newer entry point is resolved at
 * runtime via SDL_GL_GetProcAddress and mapped onto its standard gl* name.
 * gl_loader_init() must be called once after SDL_GL_CreateContext().
 */

#ifdef RMMZ_GLES2
#include <GLES2/gl2.h>
#else
#include <SDL3/SDL_opengl.h>

/* X-macro list of every function not exported by Windows' OpenGL 1.1 DLL. */
#define RMMZ_GL_FUNCS(X) \
    /* Shaders / programs (GL 2.0) */ \
    X(PFNGLBINDATTRIBLOCATIONPROC,         glBindAttribLocation) \
    X(PFNGLCREATESHADERPROC,               glCreateShader) \
    X(PFNGLSHADERSOURCEPROC,               glShaderSource) \
    X(PFNGLCOMPILESHADERPROC,              glCompileShader) \
    X(PFNGLGETSHADERIVPROC,                glGetShaderiv) \
    X(PFNGLGETSHADERINFOLOGPROC,           glGetShaderInfoLog) \
    X(PFNGLDELETESHADERPROC,               glDeleteShader) \
    X(PFNGLCREATEPROGRAMPROC,              glCreateProgram) \
    X(PFNGLATTACHSHADERPROC,               glAttachShader) \
    X(PFNGLLINKPROGRAMPROC,                glLinkProgram) \
    X(PFNGLGETPROGRAMIVPROC,               glGetProgramiv) \
    X(PFNGLGETPROGRAMINFOLOGPROC,          glGetProgramInfoLog) \
    X(PFNGLUSEPROGRAMPROC,                 glUseProgram) \
    X(PFNGLDELETEPROGRAMPROC,              glDeleteProgram) \
    X(PFNGLGETUNIFORMLOCATIONPROC,         glGetUniformLocation) \
    X(PFNGLUNIFORM1IPROC,                  glUniform1i) \
    X(PFNGLUNIFORM1FPROC,                  glUniform1f) \
    X(PFNGLUNIFORM2FPROC,                  glUniform2f) \
    X(PFNGLUNIFORM4FPROC,                  glUniform4f) \
    X(PFNGLUNIFORMMATRIX4FVPROC,           glUniformMatrix4fv) \
    /* Textures / blending (GL 1.3 - 1.4) */ \
    X(PFNGLACTIVETEXTUREPROC,              glActiveTexture) \
    X(PFNGLBLENDFUNCSEPARATEPROC,          glBlendFuncSeparate) \
    /* Buffers, vertex attributes and framebuffers */ \
    X(PFNGLGENBUFFERSPROC,                 glGenBuffers) \
    X(PFNGLBINDBUFFERPROC,                 glBindBuffer) \
    X(PFNGLBUFFERDATAPROC,                 glBufferData) \
    X(PFNGLBUFFERSUBDATAPROC,              glBufferSubData) \
    X(PFNGLENABLEVERTEXATTRIBARRAYPROC,    glEnableVertexAttribArray) \
    X(PFNGLVERTEXATTRIBPOINTERPROC,        glVertexAttribPointer) \
    X(PFNGLDELETEBUFFERSPROC,              glDeleteBuffers) \
    X(PFNGLGENVERTEXARRAYSPROC,            glGenVertexArrays) \
    X(PFNGLBINDVERTEXARRAYPROC,            glBindVertexArray) \
    X(PFNGLDELETEVERTEXARRAYSPROC,         glDeleteVertexArrays) \
    X(PFNGLGENFRAMEBUFFERSPROC,            glGenFramebuffers) \
    X(PFNGLBINDFRAMEBUFFERPROC,            glBindFramebuffer) \
    X(PFNGLDELETEFRAMEBUFFERSPROC,         glDeleteFramebuffers) \
    X(PFNGLFRAMEBUFFERTEXTURE2DPROC,       glFramebufferTexture2D) \
    X(PFNGLCHECKFRAMEBUFFERSTATUSPROC,     glCheckFramebufferStatus) \
    X(PFNGLBLITFRAMEBUFFERPROC,            glBlitFramebuffer) \
    /* glBlitFramebuffer is used only by the desktop GL backend. */

/* Declare one function pointer per entry point (rmmz_glFoo). */
#define RMMZ_GL_DECLARE(type, name) extern type rmmz_##name;
RMMZ_GL_FUNCS(RMMZ_GL_DECLARE)
#undef RMMZ_GL_DECLARE

/* Route the standard names through the loaded pointers. */
#define glBindAttribLocation           rmmz_glBindAttribLocation
#define glCreateShader                 rmmz_glCreateShader
#define glShaderSource                 rmmz_glShaderSource
#define glCompileShader                rmmz_glCompileShader
#define glGetShaderiv                  rmmz_glGetShaderiv
#define glGetShaderInfoLog             rmmz_glGetShaderInfoLog
#define glDeleteShader                 rmmz_glDeleteShader
#define glCreateProgram                rmmz_glCreateProgram
#define glAttachShader                 rmmz_glAttachShader
#define glLinkProgram                  rmmz_glLinkProgram
#define glGetProgramiv                 rmmz_glGetProgramiv
#define glGetProgramInfoLog            rmmz_glGetProgramInfoLog
#define glUseProgram                   rmmz_glUseProgram
#define glDeleteProgram                rmmz_glDeleteProgram
#define glGetUniformLocation           rmmz_glGetUniformLocation
#define glUniform1i                    rmmz_glUniform1i
#define glUniform1f                    rmmz_glUniform1f
#define glUniform2f                    rmmz_glUniform2f
#define glUniform4f                    rmmz_glUniform4f
#define glUniformMatrix4fv             rmmz_glUniformMatrix4fv
#define glActiveTexture                rmmz_glActiveTexture
#define glBlendFuncSeparate            rmmz_glBlendFuncSeparate
#define glGenBuffers                   rmmz_glGenBuffers
#define glBindBuffer                   rmmz_glBindBuffer
#define glBufferData                   rmmz_glBufferData
#define glBufferSubData                rmmz_glBufferSubData
#define glEnableVertexAttribArray      rmmz_glEnableVertexAttribArray
#define glVertexAttribPointer          rmmz_glVertexAttribPointer
#define glDeleteBuffers                rmmz_glDeleteBuffers
#define glGenVertexArrays              rmmz_glGenVertexArrays
#define glBindVertexArray              rmmz_glBindVertexArray
#define glDeleteVertexArrays           rmmz_glDeleteVertexArrays
#define glGenFramebuffers              rmmz_glGenFramebuffers
#define glBindFramebuffer              rmmz_glBindFramebuffer
#define glDeleteFramebuffers           rmmz_glDeleteFramebuffers
#define glFramebufferTexture2D         rmmz_glFramebufferTexture2D
#define glCheckFramebufferStatus       rmmz_glCheckFramebufferStatus
#define glBlitFramebuffer              rmmz_glBlitFramebuffer

#endif /* !RMMZ_GLES2 */

/* Initialize renderer entry points after a graphics context is current. */
int gl_loader_init(void);

/* Non-zero once gl_loader_init() has succeeded. */
int gl_loader_is_ready(void);

#endif /* RMMZ_GL_LOADER_H */
