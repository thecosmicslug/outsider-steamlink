/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_SDL_H
#define RMMZ_SDL_H

#ifdef RMMZ_STEAMLINK
#include <SDL.h>
typedef SDL_mutex SDL_Mutex;
#define SDL_EVENT_QUIT SDL_QUIT
#else
#include <SDL3/SDL.h>
#endif

#endif
