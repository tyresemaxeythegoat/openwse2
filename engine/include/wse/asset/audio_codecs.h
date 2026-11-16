// Factory header for instantiating the compressed audio codec backend
// (Ogg Vorbis via stb_vorbis + MP3 via dr_mp3, isolated inside `wse_audio`).
// Separate from `wse/asset/audio.h` so header-only targets that only decode
// WAV/PCM incur zero external symbol dependencies (mirrors `wse/vm/lua_manager.h`).

#pragma once

#include <memory>

#include "wse/asset/audio.h"

namespace wse::asset {

[[nodiscard]] std::shared_ptr<AudioCodecBackend> create_audio_codec_backend();

}  // namespace wse::asset
