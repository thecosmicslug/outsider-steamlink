/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "platform/platform.h"
#include "input/input_manager.h"
#include "rendering/gl_loader.h"

#include <SDL.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct Platform {
    SDL_Window *window;
    SDL_GLContext gl_ctx;
    SDL_GameController *controllers[INPUT_MAX_GAMEPADS];
    SDL_JoystickID controller_ids[INPUT_MAX_GAMEPADS];
    bool resize_pending;
    int resize_w;
    int resize_h;
};

static int gamecontroller_slot(const Platform *p, SDL_JoystickID id)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (p->controllers[i] && p->controller_ids[i] == id) return i;
    }
    return -1;
}

static int free_controller_slot(const Platform *p)
{
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (!p->controllers[i]) return i;
    }
    return -1;
}

static void open_gamecontroller(Platform *p, int device_index)
{
    if (!SDL_IsGameController(device_index)) return;
    SDL_GameController *controller = SDL_GameControllerOpen(device_index);
    if (!controller) {
        fprintf(stderr, "SDL_GameControllerOpen failed: %s\n", SDL_GetError());
        return;
    }

    SDL_Joystick *joystick = SDL_GameControllerGetJoystick(controller);
    SDL_JoystickID id = SDL_JoystickInstanceID(joystick);
    if (gamecontroller_slot(p, id) >= 0) {
        SDL_GameControllerClose(controller);
        return;
    }
    int slot = free_controller_slot(p);
    if (slot < 0) {
        SDL_GameControllerClose(controller);
        return;
    }

    p->controllers[slot] = controller;
    p->controller_ids[slot] = id;
    GamepadState state;
    memset(&state, 0, sizeof(state));
    state.connected = true;
    state.index = slot;
    const char *name = SDL_GameControllerName(controller);
    snprintf(state.id, sizeof(state.id), "%s", name ? name : "Unknown Controller");
    input_manager_set_gamepad(slot, &state);
}

static void close_gamecontroller(Platform *p, SDL_JoystickID id)
{
    int slot = gamecontroller_slot(p, id);
    if (slot < 0) return;
    SDL_GameControllerClose(p->controllers[slot]);
    p->controllers[slot] = NULL;
    p->controller_ids[slot] = 0;
    input_manager_clear_gamepad(slot);
}

static void poll_gamecontrollers(Platform *p)
{
    static const SDL_GameControllerButton buttons[INPUT_GAMEPAD_BUTTONS] = {
        SDL_CONTROLLER_BUTTON_A, SDL_CONTROLLER_BUTTON_B,
        SDL_CONTROLLER_BUTTON_X, SDL_CONTROLLER_BUTTON_Y,
        SDL_CONTROLLER_BUTTON_LEFTSHOULDER, SDL_CONTROLLER_BUTTON_RIGHTSHOULDER,
        SDL_CONTROLLER_BUTTON_INVALID, SDL_CONTROLLER_BUTTON_INVALID,
        SDL_CONTROLLER_BUTTON_BACK, SDL_CONTROLLER_BUTTON_START,
        SDL_CONTROLLER_BUTTON_LEFTSTICK, SDL_CONTROLLER_BUTTON_RIGHTSTICK,
        SDL_CONTROLLER_BUTTON_DPAD_UP, SDL_CONTROLLER_BUTTON_DPAD_DOWN,
        SDL_CONTROLLER_BUTTON_DPAD_LEFT, SDL_CONTROLLER_BUTTON_DPAD_RIGHT,
        SDL_CONTROLLER_BUTTON_GUIDE
    };
    static const SDL_GameControllerAxis axes[INPUT_GAMEPAD_AXES] = {
        SDL_CONTROLLER_AXIS_LEFTX, SDL_CONTROLLER_AXIS_LEFTY,
        SDL_CONTROLLER_AXIS_RIGHTX, SDL_CONTROLLER_AXIS_RIGHTY
    };

    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        SDL_GameController *controller = p->controllers[i];
        if (!controller) continue;

        GamepadState state;
        memset(&state, 0, sizeof(state));
        state.connected = true;
        state.index = i;
        const char *name = SDL_GameControllerName(controller);
        snprintf(state.id, sizeof(state.id), "%s", name ? name : "Unknown Controller");

        for (int b = 0; b < INPUT_GAMEPAD_BUTTONS; b++) {
            if (buttons[b] == SDL_CONTROLLER_BUTTON_INVALID) continue;
            bool pressed = SDL_GameControllerGetButton(controller, buttons[b]) != 0;
            state.buttons[b].pressed = pressed;
            state.buttons[b].value = pressed ? 1.0 : 0.0;
        }
        for (int a = 0; a < INPUT_GAMEPAD_AXES; a++) {
            double value = (double)SDL_GameControllerGetAxis(controller, axes[a]) / 32767.0;
            state.axes[a] = value < -1.0 ? -1.0 : value > 1.0 ? 1.0 : value;
        }

        const double left_trigger =
            ((double)SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERLEFT) + 32768.0) / 65535.0;
        const double right_trigger =
            ((double)SDL_GameControllerGetAxis(controller, SDL_CONTROLLER_AXIS_TRIGGERRIGHT) + 32768.0) / 65535.0;
        state.buttons[6].value = left_trigger;
        state.buttons[6].pressed = state.buttons[6].value > 0.5;
        state.buttons[7].value = right_trigger;
        state.buttons[7].pressed = state.buttons[7].value > 0.5;
        input_manager_set_gamepad(i, &state);
    }
}

static void push_key_event(const SDL_KeyboardEvent *key, InputEventType type)
{
    InputEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.keyCode = (int)key->keysym.sym;
    event.repeat = key->repeat != 0;
    event.shiftKey = (key->keysym.mod & KMOD_SHIFT) != 0;
    event.ctrlKey = (key->keysym.mod & KMOD_CTRL) != 0;
    event.altKey = (key->keysym.mod & KMOD_ALT) != 0;
    event.metaKey = (key->keysym.mod & KMOD_GUI) != 0;

    SDL_Keycode sym = key->keysym.sym;
    SDL_Scancode sc = key->keysym.scancode;
    if (sym >= SDLK_a && sym <= SDLK_z) {
        event.key[0] = (char)((event.shiftKey ? 'A' : 'a') + (sym - SDLK_a));
        snprintf(event.code, sizeof(event.code), "Key%c", 'A' + (sym - SDLK_a));
    } else if (sym >= SDLK_0 && sym <= SDLK_9) {
        event.key[0] = (char)sym;
        snprintf(event.code, sizeof(event.code), "Digit%c", (char)sym);
    } else if (sym >= SDLK_F1 && sym <= SDLK_F12) {
        snprintf(event.key, sizeof(event.key), "F%d", (int)(sym - SDLK_F1) + 1);
        snprintf(event.code, sizeof(event.code), "F%d", (int)(sym - SDLK_F1) + 1);
        event.keyCode = 112 + (int)(sym - SDLK_F1);
    } else {
        const char *name = SDL_GetKeyName(sym);
        const char *code = SDL_GetScancodeName(sc);
        if (name) snprintf(event.key, sizeof(event.key), "%s", name);
        if (code) snprintf(event.code, sizeof(event.code), "%s", code);
        switch (sym) {
            case SDLK_RETURN: event.keyCode = 13; snprintf(event.key, sizeof(event.key), "Enter"); break;
            case SDLK_ESCAPE: event.keyCode = 27; snprintf(event.key, sizeof(event.key), "Escape"); break;
            case SDLK_BACKSPACE: event.keyCode = 8; snprintf(event.key, sizeof(event.key), "Backspace"); break;
            case SDLK_TAB: event.keyCode = 9; snprintf(event.key, sizeof(event.key), "Tab"); break;
            case SDLK_SPACE: event.keyCode = 32; snprintf(event.key, sizeof(event.key), " "); break;
            case SDLK_LEFT: event.keyCode = 37; snprintf(event.key, sizeof(event.key), "ArrowLeft"); break;
            case SDLK_UP: event.keyCode = 38; snprintf(event.key, sizeof(event.key), "ArrowUp"); break;
            case SDLK_RIGHT: event.keyCode = 39; snprintf(event.key, sizeof(event.key), "ArrowRight"); break;
            case SDLK_DOWN: event.keyCode = 40; snprintf(event.key, sizeof(event.key), "ArrowDown"); break;
            default: break;
        }
    }
    input_manager_push_event(&event);
}

static int dom_mouse_button(uint8_t button)
{
    if (button == SDL_BUTTON_LEFT) return 0;
    if (button == SDL_BUTTON_MIDDLE) return 1;
    if (button == SDL_BUTTON_RIGHT) return 2;
    return (int)button - 1;
}

static void push_mouse_event(InputEventType type, int x, int y, int button)
{
    InputEvent event;
    memset(&event, 0, sizeof(event));
    event.type = type;
    event.mouseX = x;
    event.mouseY = y;
    event.button = button;
    input_manager_push_event(&event);
}

Platform *platform_init(const char *title, int width, int height)
{
    if (SDL_Init(SDL_INIT_VIDEO) != 0) {
        fprintf(stderr, "SDL_Init failed: %s\n", SDL_GetError());
        return NULL;
    }
    if (SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
        fprintf(stderr, "Warning: audio init failed: %s\n", SDL_GetError());
    }
    if (SDL_InitSubSystem(SDL_INIT_GAMECONTROLLER) != 0) {
        fprintf(stderr, "Warning: game controller init failed: %s\n", SDL_GetError());
    }

    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);

    Uint32 window_flags =
        SDL_WINDOW_OPENGL | SDL_WINDOW_SHOWN | SDL_WINDOW_RESIZABLE;
#ifdef RMMZ_STEAMLINK
    /* The Steam Link is a TV appliance, so launch borderless fullscreen. */
    window_flags |= SDL_WINDOW_FULLSCREEN_DESKTOP;
#endif
    SDL_Window *window = SDL_CreateWindow(title, SDL_WINDOWPOS_CENTERED,
        SDL_WINDOWPOS_CENTERED, width, height, window_flags);
    if (!window) {
        fprintf(stderr, "SDL_CreateWindow failed: %s\n", SDL_GetError());
        SDL_Quit();
        return NULL;
    }
    if (SDL_ShowCursor(SDL_DISABLE) < 0) {
        fprintf(stderr, "Warning: unable to hide mouse cursor: %s\n", SDL_GetError());
    }

    SDL_GLContext gl_ctx = SDL_GL_CreateContext(window);
    if (!gl_ctx) {
        fprintf(stderr, "SDL_GL_CreateContext failed: %s\n", SDL_GetError());
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
    if (gl_loader_init() != 0) {
        fprintf(stderr, "OpenGL ES 2.0 initialization failed\n");
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
    SDL_GL_SetSwapInterval(1);
    SDL_StartTextInput();

    Platform *p = calloc(1, sizeof(*p));
    if (!p) {
        SDL_GL_DeleteContext(gl_ctx);
        SDL_DestroyWindow(window);
        SDL_Quit();
        return NULL;
    }
    p->window = window;
    p->gl_ctx = gl_ctx;

    for (int i = 0; i < SDL_NumJoysticks(); i++) open_gamecontroller(p, i);
    return p;
}

void platform_shutdown(Platform *p)
{
    if (!p) return;
    for (int i = 0; i < INPUT_MAX_GAMEPADS; i++) {
        if (p->controllers[i]) SDL_GameControllerClose(p->controllers[i]);
    }
    SDL_StopTextInput();
    SDL_GL_DeleteContext(p->gl_ctx);
    SDL_DestroyWindow(p->window);
    SDL_Quit();
    free(p);
}

bool platform_poll_events(Platform *p)
{
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
        switch (event.type) {
            case SDL_QUIT:
                return false;
            case SDL_KEYDOWN:
                push_key_event(&event.key, INPUT_KEY_DOWN);
                break;
            case SDL_KEYUP:
                push_key_event(&event.key, INPUT_KEY_UP);
                break;
            case SDL_TEXTINPUT: {
                InputEvent input;
                memset(&input, 0, sizeof(input));
                input.type = INPUT_TEXT;
                snprintf(input.key, sizeof(input.key), "%s", event.text.text);
                input_manager_push_event(&input);
                break;
            }
            case SDL_MOUSEBUTTONDOWN:
            case SDL_MOUSEBUTTONUP:
                push_mouse_event(event.type == SDL_MOUSEBUTTONDOWN ? INPUT_MOUSE_DOWN : INPUT_MOUSE_UP,
                    event.button.x, event.button.y, dom_mouse_button(event.button.button));
                break;
            case SDL_MOUSEMOTION:
                push_mouse_event(INPUT_MOUSE_MOVE, event.motion.x, event.motion.y, 0);
                break;
            case SDL_MOUSEWHEEL: {
                InputEvent input;
                memset(&input, 0, sizeof(input));
                input.type = INPUT_WHEEL;
                input.wheelDeltaX = event.wheel.x;
                input.wheelDeltaY = -event.wheel.y;
                if (event.wheel.direction == SDL_MOUSEWHEEL_FLIPPED) {
                    input.wheelDeltaX = -input.wheelDeltaX;
                    input.wheelDeltaY = -input.wheelDeltaY;
                }
                SDL_GetMouseState(&input.mouseX, &input.mouseY);
                input_manager_push_event(&input);
                break;
            }
            case SDL_WINDOWEVENT:
                if (event.window.event == SDL_WINDOWEVENT_RESIZED ||
                    event.window.event == SDL_WINDOWEVENT_SIZE_CHANGED) {
                    p->resize_pending = true;
                    p->resize_w = event.window.data1;
                    p->resize_h = event.window.data2;
                }
                break;
            case SDL_CONTROLLERDEVICEADDED:
                open_gamecontroller(p, event.cdevice.which);
                break;
            case SDL_CONTROLLERDEVICEREMOVED:
                close_gamecontroller(p, event.cdevice.which);
                break;
            default:
                break;
        }
    }
    SDL_GameControllerUpdate();
    poll_gamecontrollers(p);
    return true;
}

void platform_swap_buffers(Platform *p)
{
    if (p && p->window) SDL_GL_SwapWindow(p->window);
}

void platform_set_swap_interval(Platform *p, bool enabled)
{
    if (p && p->gl_ctx) SDL_GL_SetSwapInterval(enabled ? 1 : 0);
}

void platform_get_window_size(Platform *p, int *w, int *h)
{
    if (p && p->window) SDL_GetWindowSize(p->window, w, h);
}

void platform_get_drawable_size(Platform *p, int *w, int *h)
{
    if (p && p->window) SDL_GL_GetDrawableSize(p->window, w, h);
}

void platform_set_window_size(Platform *p, int w, int h)
{
    if (p && p->window && w > 0 && h > 0) {
        SDL_SetWindowSize(p->window, w, h);
        SDL_SetWindowPosition(p->window, SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED);
    }
}

void platform_set_window_title(Platform *p, const char *title)
{
    if (p && p->window) SDL_SetWindowTitle(p->window, title ? title : "");
}

void platform_set_fullscreen(Platform *p, bool fullscreen)
{
    if (p && p->window) {
        SDL_SetWindowFullscreen(p->window,
            fullscreen ? SDL_WINDOW_FULLSCREEN_DESKTOP : 0);
    }
}

bool platform_is_fullscreen(Platform *p)
{
    return p && p->window &&
        (SDL_GetWindowFlags(p->window) & SDL_WINDOW_FULLSCREEN) != 0;
}

void platform_get_display_size(Platform *p, int *w, int *h)
{
    int display = p && p->window ? SDL_GetWindowDisplayIndex(p->window) : 0;
    SDL_DisplayMode mode;
    if (SDL_GetDesktopDisplayMode(display, &mode) == 0) {
        if (w) *w = mode.w;
        if (h) *h = mode.h;
    } else {
        if (w) *w = 1920;
        if (h) *h = 1080;
    }
}

float platform_get_display_scale(Platform *p)
{
    (void)p;
    return 1.0f;
}

float platform_get_desktop_scale(Platform *p)
{
    (void)p;
    return 1.0f;
}

bool platform_check_resize(Platform *p, int *w, int *h)
{
    if (!p || !p->resize_pending) return false;
    p->resize_pending = false;
    if (w) *w = p->resize_w;
    if (h) *h = p->resize_h;
    return true;
}
