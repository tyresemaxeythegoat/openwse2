// Factory header for instantiating the Khronos glslang SPIR-V 1.0 shader
// compiler backend (isolated inside `wse_shader`).
// Separate from `wse/asset/shader.h` so header-only engine targets that only
// parse techniques, material state, or cached SPIR-V metadata incur zero external
// symbol dependencies (mirrors `wse/asset/audio_codecs.h` and `wse/vm/lua_manager.h`).

#pragma once

#include <memory>

#include "wse/asset/shader.h"

namespace wse::asset {

[[nodiscard]] std::shared_ptr<SpirvCompilerBackend> create_glslang_shader_compiler();

}  // namespace wse::asset
