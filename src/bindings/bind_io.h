/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_BIND_IO_H
#define RMMZ_BIND_IO_H

#include "engine/error_handler.h"

#include <stdbool.h>
#include <quickjs.h>

/* Register the __native_io global object exposing synchronous file I/O to JS.
   All paths are resolved relative to the game root. */
void bind_io_register(JSContext *ctx);

/* Log the time and size of native file reads when enabled. */
void bind_io_set_load_profile(bool enabled, LogCallback callback, void *userdata);

/* Set the logger used by native I/O helpers exposed to JavaScript. */
void bind_io_set_log_callback(LogCallback callback, void *userdata);

#endif /* RMMZ_BIND_IO_H */
