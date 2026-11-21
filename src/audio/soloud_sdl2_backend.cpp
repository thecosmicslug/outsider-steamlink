/*
 * Copyright (c) 2026 General Arcade (Pte. Ltd.)
 * SPDX-License-Identifier: GPL-3.0-or-later OR LicenseRef-GeneralArcade-Commercial
 */

#include "soloud.h"

#include <SDL.h>
#include <stdlib.h>
#include <string.h>

namespace SoLoud
{
    static SDL_AudioDeviceID gAudioDevice = 0;
    static float *gScratch = NULL;
    static unsigned int gChannels = 2;
    static unsigned int gBufferFrames = 2048;

    static void SDLCALL soloud_sdl2_audiomixer(void *userdata, Uint8 *stream, int length)
    {
        memset(stream, 0, (size_t)length);
        Soloud *soloud = (Soloud *)userdata;
        if (!soloud || !gScratch) return;

        const unsigned int frame_bytes = gChannels * sizeof(float);
        unsigned int remaining = (unsigned int)length / frame_bytes;
        float *out = (float *)stream;
        while (remaining > 0) {
            unsigned int frames = remaining < gBufferFrames ? remaining : gBufferFrames;
            soloud->mix(gScratch, frames);
            memcpy(out, gScratch, (size_t)frames * frame_bytes);
            out += (size_t)frames * gChannels;
            remaining -= frames;
        }
    }

    static void soloud_sdl2_deinit(Soloud *aSoloud)
    {
        (void)aSoloud;
        if (gAudioDevice) {
            SDL_CloseAudioDevice(gAudioDevice);
            gAudioDevice = 0;
        }
        free(gScratch);
        gScratch = NULL;
    }

    result sdl2static_init(Soloud *aSoloud, unsigned int aFlags, unsigned int aSamplerate,
                           unsigned int aBuffer, unsigned int aChannels)
    {
        if (!(SDL_WasInit(SDL_INIT_AUDIO) & SDL_INIT_AUDIO) &&
            SDL_InitSubSystem(SDL_INIT_AUDIO) != 0) {
            return UNKNOWN_ERROR;
        }

        if (aSamplerate == 0) aSamplerate = 44100;
        if (aBuffer == 0) aBuffer = 2048;
        if (aChannels == 0) aChannels = 2;
        if (aChannels > 8 || aBuffer > UINT16_MAX) return INVALID_PARAMETER;

        SDL_AudioSpec desired;
        SDL_zero(desired);
        desired.freq = (int)aSamplerate;
        desired.format = AUDIO_F32SYS;
        desired.channels = (Uint8)aChannels;
        desired.samples = (Uint16)aBuffer;
        desired.callback = soloud_sdl2_audiomixer;
        desired.userdata = aSoloud;

        gScratch = (float *)malloc((size_t)aBuffer * aChannels * sizeof(float));
        if (!gScratch) return OUT_OF_MEMORY;
        gChannels = aChannels;
        gBufferFrames = aBuffer;

        gAudioDevice = SDL_OpenAudioDevice(NULL, 0, &desired, NULL, 0);
        if (!gAudioDevice) {
            free(gScratch);
            gScratch = NULL;
            return UNKNOWN_ERROR;
        }

        aSoloud->postinit_internal(aSamplerate, aBuffer, aFlags, aChannels);
        aSoloud->mBackendCleanupFunc = soloud_sdl2_deinit;
        aSoloud->mBackendString = "SDL2";
        SDL_PauseAudioDevice(gAudioDevice, 0);
        return SO_NO_ERROR;
    }
}
