// SPDX-FileCopyrightText: Copyright 2024 shadPS4 Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <SDL3/SDL_audio.h>
#include <cubeb/cubeb.h>
#include "common/config.h"

typedef struct cubeb cubeb;

namespace Libraries::AudioOut {

extern SDL_AudioStream* currentSDLStream;
extern cubeb_stream* currentCubebStream;

inline void AdjustVolume(float vol) {
    if (!currentSDLStream && !currentCubebStream) {
        return;
    }
    if (Config::getAudioBackend() == "sdl") {
        SDL_SetAudioStreamGain(currentSDLStream, vol);
    } else if (Config::getAudioBackend() == "cubeb") {
        cubeb_stream_set_volume(currentCubebStream, vol);
    }
}

struct PortOut;

class PortBackend {
public:
    virtual ~PortBackend() = default;

    virtual void Output(void* ptr, size_t size) = 0;
    virtual void SetVolume(const std::array<int, 8>& ch_volumes) = 0;
};

class AudioOutBackend {
public:
    AudioOutBackend() = default;
    virtual ~AudioOutBackend() = default;

    virtual std::unique_ptr<PortBackend> Open(PortOut& port) = 0;
};

class CubebAudioOut final : public AudioOutBackend {
public:
    CubebAudioOut();
    ~CubebAudioOut() override;

    std::unique_ptr<PortBackend> Open(PortOut& port) override;

private:
    cubeb* ctx = nullptr;
#ifdef _WIN32
    bool owns_com = false;
#endif
};

class SDLAudioOut final : public AudioOutBackend {
public:
    std::unique_ptr<PortBackend> Open(PortOut& port) override;
};

} // namespace Libraries::AudioOut
