// Clean-room C++20 SPIR-V 1.0 compiler backend backed by Khronos glslang.
// `<glslang/...>` and `<SPIRV/...>` remain private to this translation unit so
// portable headers stay strictly STL-only.

#include "wse/asset/shader_compiler.h"

#include <SPIRV/GlslangToSpv.h>
#include <glslang/Include/Types.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>

#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace wse::asset {

namespace {

[[nodiscard]] bool to_esh_language(SpirvStage stage, EShLanguage& out) noexcept {
    switch (stage) {
        case SpirvStage::Vertex:
            out = EShLangVertex;
            return true;
        case SpirvStage::Fragment:
            out = EShLangFragment;
            return true;
    }
    return false;
}

[[nodiscard]] std::mutex& compiler_global_mutex() {
    static std::mutex m;
    return m;
}

class GlslangSpirvCompilerBackend final : public SpirvCompilerBackend {
public:
    GlslangSpirvCompilerBackend() {
        std::lock_guard<std::mutex> lock(compiler_global_mutex());
        initialized_ = glslang::InitializeProcess();
    }

    ~GlslangSpirvCompilerBackend() override {
        if (initialized_) {
            std::lock_guard<std::mutex> lock(compiler_global_mutex());
            glslang::FinalizeProcess();
        }
    }

    GlslangSpirvCompilerBackend(const GlslangSpirvCompilerBackend&) = delete;
    GlslangSpirvCompilerBackend& operator=(const GlslangSpirvCompilerBackend&) = delete;

    [[nodiscard]] const char* backend_name() const noexcept override {
        return "glslang";
    }

    [[nodiscard]] bool compile_vulkan_glsl_to_spirv(
        std::string_view vulkan_glsl_source,
        SpirvStage stage,
        std::string_view entry_point,
        std::vector<std::uint32_t>& out_spirv_words,
        ShaderStageReflection* out_reflection,
        std::string* out_info_log) override {
        out_spirv_words.clear();
        if (out_reflection != nullptr) {
            *out_reflection = {};
        }
        if (out_info_log != nullptr) {
            out_info_log->clear();
        }
        if (!initialized_) {
            if (out_info_log != nullptr) {
                *out_info_log = "glslang::InitializeProcess() failed";
            }
            return false;
        }
        EShLanguage esh_stage = EShLangVertex;
        if (!to_esh_language(stage, esh_stage)) {
            if (out_info_log != nullptr) {
                *out_info_log = "unsupported shader stage";
            }
            return false;
        }

        const std::string src_str(vulkan_glsl_source);
        const std::string entry_str(entry_point.empty() ? std::string_view("main") : entry_point);
        const char* src_ptr = src_str.c_str();
        const int src_len = static_cast<int>(src_str.size());

        // `glslang` tracks process-wide pool/symbol tables, so serialize all calls.
        std::lock_guard<std::mutex> lock(compiler_global_mutex());

        glslang::TShader shader(esh_stage);
        shader.setStringsWithLengths(&src_ptr, &src_len, 1);
        shader.setEntryPoint(entry_str.c_str());
        shader.setSourceEntryPoint(entry_str.c_str());
        shader.setEnvInput(glslang::EShSourceGlsl, esh_stage, glslang::EShClientVulkan, 100);
        shader.setEnvClient(glslang::EShClientVulkan, glslang::EShTargetVulkan_1_0);
        shader.setEnvTarget(glslang::EShTargetSpv, glslang::EShTargetSpv_1_0);

        const EShMessages messages = static_cast<EShMessages>(EShMsgSpvRules | EShMsgVulkanRules);
        if (!shader.parse(GetDefaultResources(), 450, false, messages)) {
            if (out_info_log != nullptr) {
                *out_info_log = shader.getInfoLog();
            }
            return false;
        }

        // Destroy `TProgram` before `TShader` leaves scope (required by glslang).
        {
            glslang::TProgram program;
            program.addShader(&shader);
            if (!program.link(messages)) {
                if (out_info_log != nullptr) {
                    *out_info_log = program.getInfoLog();
                }
                return false;
            }

            if (out_reflection != nullptr) {
                program.buildReflection(EShReflectionIntermediateIO | EShReflectionAllBlockVariables);
                const int num_blocks = program.getNumUniformBlocks();
                out_reflection->uniform_blocks.reserve(static_cast<std::size_t>(num_blocks));
                for (int bi = 0; bi < num_blocks; ++bi) {
                    const glslang::TObjectReflection& b = program.getUniformBlock(bi);
                    ShaderUniformBlockInfo blk{};
                    blk.name = b.name;
                    if (b.getType() != nullptr) {
                        const auto& qual = b.getType()->getQualifier();
                        blk.set = qual.hasSet() ? static_cast<std::uint32_t>(qual.layoutSet) : 0U;
                    }
                    blk.binding = b.getBinding() >= 0 ? static_cast<std::uint32_t>(b.getBinding()) : 0U;
                    blk.size_bytes = b.size >= 0 ? static_cast<std::uint32_t>(b.size) : 0U;
                    out_reflection->uniform_blocks.push_back(std::move(blk));
                }

                const int num_uniforms = program.getNumUniformVariables();
                for (int ui = 0; ui < num_uniforms; ++ui) {
                    const glslang::TObjectReflection& u = program.getUniform(ui);
                    const glslang::TType* ty = u.getType();
                    if (ty != nullptr && ty->getBasicType() == glslang::EbtSampler) {
                        ShaderSamplerBindingInfo samp{};
                        samp.name = u.name;
                        const auto& qual = ty->getQualifier();
                        samp.set = qual.hasSet() ? static_cast<std::uint32_t>(qual.layoutSet) : 0U;
                        samp.binding =
                            u.getBinding() >= 0 ? static_cast<std::uint32_t>(u.getBinding()) : 0U;
                        out_reflection->samplers.push_back(std::move(samp));
                    } else if (u.index >= 0 &&
                               static_cast<std::size_t>(u.index) <
                                   out_reflection->uniform_blocks.size()) {
                        ShaderUniformMemberInfo mem{};
                        mem.name = u.name;
                        mem.offset_bytes =
                            u.offset >= 0 ? static_cast<std::uint32_t>(u.offset) : 0U;
                        mem.size_bytes = u.size >= 0 ? static_cast<std::uint32_t>(u.size) : 0U;
                        out_reflection->uniform_blocks[static_cast<std::size_t>(u.index)]
                            .members.push_back(std::move(mem));
                    }
                }

                const int num_inputs = program.getNumPipeInputs();
                out_reflection->inputs.reserve(static_cast<std::size_t>(num_inputs));
                for (int ii = 0; ii < num_inputs; ++ii) {
                    const glslang::TObjectReflection& in = program.getPipeInput(ii);
                    if (in.name.starts_with("gl_")) {
                        continue;
                    }
                    ShaderInputAttributeInfo attr{};
                    attr.name = in.name;
                    attr.location = in.layoutLocation();
                    out_reflection->inputs.push_back(std::move(attr));
                }
            }

            const glslang::TIntermediate* intermediate = program.getIntermediate(esh_stage);
            if (intermediate == nullptr) {
                if (out_info_log != nullptr) {
                    *out_info_log = "glslang produced null intermediate";
                }
                return false;
            }

            glslang::SpvOptions spv_opts{};
            spv_opts.generateDebugInfo = false;
            spv_opts.stripDebugInfo = false;
            spv_opts.disableOptimizer = true;
            spv_opts.validate = false;
            std::vector<unsigned int> raw_spv;
            glslang::GlslangToSpv(*intermediate, raw_spv, &spv_opts);
            out_spirv_words.assign(raw_spv.begin(), raw_spv.end());
            if (out_info_log != nullptr) {
                *out_info_log = program.getInfoLog();
            }
        }
        return out_spirv_words.size() > 5U && out_spirv_words[0] == 0x07230203U;
    }

private:
    bool initialized_ = false;
};

}  // namespace

std::shared_ptr<SpirvCompilerBackend> create_glslang_shader_compiler() {
    return std::make_shared<GlslangSpirvCompilerBackend>();
}

}  // namespace wse::asset
