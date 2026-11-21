/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#ifndef RMMZ_BOOT_CONSOLE_H
#define RMMZ_BOOT_CONSOLE_H

#include "engine/error_handler.h"

#include <stdbool.h>

typedef struct Platform Platform;

bool boot_console_init(Platform *platform);
bool boot_console_is_active(void);
void boot_console_defer_updates(void);
void boot_console_present_pending(void);
void boot_console_log_callback(LogLevel level, const char *message, void *userdata);
void boot_console_hide(void);
void boot_console_shutdown(void);

#endif
