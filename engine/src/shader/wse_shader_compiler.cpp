// Clean-room C++20 SPIR-V 1.0 compiler backend backed by Khronos glslang.
// `<glslang/...>` and `<SPIRV/...>` remain private to this translation unit so
// portable headers stay strictly STL-only.

#include "wse/asset/shader_compiler.h"

#include <SPIRV/GlslangToSpv.h>
#include <SPIRV/Logger.h>
#include <glslang/Include/Types.h>
#include <glslang/Public/ResourceLimits.h>
#include <glslang/Public/ShaderLang.h>
#include <glslang/build_info.h>

#include <climits>
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

// Serializes only process-wide `InitializeProcess` / `FinalizeProcess` (reference
// counted by glslang). Compiles run concurrently: glslang keeps per-thread pools and
// guards its shared built-in symbol tables itself.
[[nodiscard]] std::mutex& process_init_mutex() {
    static std::mutex m;
    return m;
}

// Bytes one value of `t` occupies in a `std140` block, ignoring array dimensions (so
// an array type yields its element size). Struct-typed values report 0: glslang
// reflects their leaf members separately.
[[nodiscard]] std::uint32_t std140_value_bytes(const glslang::TType& t,
                                               std::uint32_t* out_matrix_stride) {
    std::uint32_t scalar = 4U;
    switch (t.getBasicType()) {
        case glslang::EbtDouble:
        case glslang::EbtInt64:
        case glslang::EbtUint64:
            scalar = 8U;
            break;
        case glslang::EbtStruct:
        case glslang::EbtBlock:
            return 0U;
        default:
            break;
    }
    if (t.isMatrix()) {
        const bool row_major = t.getQualifier().layoutMatrix == glslang::ElmRowMajor;
        const std::uint32_t vectors =
            static_cast<std::uint32_t>(row_major ? t.getMatrixRows() : t.getMatrixCols());
        const std::uint32_t vec_len =
            static_cast<std::uint32_t>(row_major ? t.getMatrixCols() : t.getMatrixRows());
        // std140: every matrix column (or row) is rounded up to a vec4.
        const std::uint32_t stride = ((vec_len * scalar + 15U) / 16U) * 16U;
        if (out_matrix_stride != nullptr) {
            *out_matrix_stride = stride;
        }
        return vectors * stride;
    }
    if (t.isVector()) {
        return static_cast<std::uint32_t>(t.getVectorSize()) * scalar;
    }
    return scalar;
}

class GlslangSpirvCompilerBackend final : public SpirvCompilerBackend {
public:
    GlslangSpirvCompilerBackend() {
        std::lock_guard<std::mutex> lock(process_init_mutex());
        initialized_ = glslang::InitializeProcess();
    }

    ~GlslangSpirvCompilerBackend() override {
        if (initialized_) {
            std::lock_guard<std::mutex> lock(process_init_mutex());
            glslang::FinalizeProcess();
        }
    }

    GlslangSpirvCompilerBackend(const GlslangSpirvCompilerBackend&) = delete;
    GlslangSpirvCompilerBackend& operator=(const GlslangSpirvCompilerBackend&) = delete;

    [[nodiscard]] const char* backend_name() const noexcept override {
        return "glslang";
    }

    [[nodiscard]] std::string cache_identity() const override {
        return "glslang-" + std::to_string(GLSLANG_VERSION_MAJOR) + "." +
               std::to_string(GLSLANG_VERSION_MINOR) + "." +
               std::to_string(GLSLANG_VERSION_PATCH) + GLSLANG_VERSION_FLAVOR;
    }

    [[nodiscard]] bool supports_preprocessing() const noexcept override {
        return true;
    }

    [[nodiscard]] bool preprocess_legacy_glsl(std::string_view glsl_source,
                                              SpirvStage stage,
                                              std::string& out_source,
                                              std::string* out_info_log) override {
        out_source.clear();
        if (out_info_log != nullptr) {
            out_info_log->clear();
        }
        EShLanguage esh_stage = EShLangVertex;
        if (!check_ready(stage, glsl_source, esh_stage, out_info_log)) {
            return false;
        }
        const char* src_ptr = glsl_source.data();
        const int src_len = static_cast<int>(glsl_source.size());
        // Default (OpenGL, no SPIR-V target) environment: legacy `#version 110/120`
        // sources are only preprocessed here, never compiled.
        glslang::TShader shader(esh_stage);
        shader.setStringsWithLengths(&src_ptr, &src_len, 1);
        glslang::TShader::ForbidIncluder includer;
        if (!shader.preprocess(GetDefaultResources(), 110, ENoProfile, false, false,
                               EShMsgDefault, &out_source, includer)) {
            if (out_info_log != nullptr) {
                *out_info_log = shader.getInfoLog();
            }
            out_source.clear();
            return false;
        }
        return true;
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
        EShLanguage esh_stage = EShLangVertex;
        if (!check_ready(stage, vulkan_glsl_source, esh_stage, out_info_log)) {
            return false;
        }

        const std::string entry_str(entry_point.empty() ? std::string_view("main") : entry_point);
        const char* src_ptr = vulkan_glsl_source.data();
        const int src_len = static_cast<int>(vulkan_glsl_source.size());

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
                if (!program.buildReflection(EShReflectionIntermediateIO |
                                             EShReflectionAllBlockVariables)) {
                    if (out_info_log != nullptr) {
                        *out_info_log = "glslang reflection failed";
                    }
                    return false;
                }
                fill_reflection(program, *out_reflection);
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
            spv::SpvBuildLogger logger;
            std::vector<unsigned int> raw_spv;
            glslang::GlslangToSpv(*intermediate, raw_spv, &logger, &spv_opts);
            out_spirv_words.assign(raw_spv.begin(), raw_spv.end());
            if (out_info_log != nullptr) {
                // Parse warnings live in the shader log; link and SPIR-V builder notes
                // in the program log and the logger.
                *out_info_log = shader.getInfoLog();
                *out_info_log += program.getInfoLog();
                *out_info_log += logger.getAllMessages();
            }
        }
        if (out_spirv_words.size() <= 5U || out_spirv_words[0] != 0x07230203U) {
            if (out_info_log != nullptr && out_info_log->empty()) {
                *out_info_log = "glslang emitted no SPIR-V";
            }
            return false;
        }
        return true;
    }

private:
    [[nodiscard]] bool check_ready(SpirvStage stage,
                                   std::string_view source,
                                   EShLanguage& esh_stage,
                                   std::string* out_info_log) const {
        const char* error = nullptr;
        if (!initialized_) {
            error = "glslang::InitializeProcess() failed";
        } else if (!to_esh_language(stage, esh_stage)) {
            error = "unsupported shader stage";
        } else if (source.size() > static_cast<std::size_t>(INT_MAX)) {
            error = "GLSL source too large";
        }
        if (error != nullptr && out_info_log != nullptr) {
            *out_info_log = error;
        }
        return error == nullptr;
    }

    static void fill_reflection(const glslang::TProgram& program, ShaderStageReflection& out) {
        const int num_blocks = program.getNumUniformBlocks();
        out.uniform_blocks.reserve(static_cast<std::size_t>(num_blocks));
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
            out.uniform_blocks.push_back(std::move(blk));
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
                samp.binding = u.getBinding() >= 0 ? static_cast<std::uint32_t>(u.getBinding()) : 0U;
                out.samplers.push_back(std::move(samp));
            } else if (u.index >= 0 &&
                       static_cast<std::size_t>(u.index) < out.uniform_blocks.size()) {
                ShaderUniformMemberInfo mem{};
                mem.name = u.name;
                mem.offset_bytes = u.offset >= 0 ? static_cast<std::uint32_t>(u.offset) : 0U;
                if (ty != nullptr) {
                    if (ty->isArray()) {
                        // For a non-block object `size` is the array length, not bytes.
                        mem.array_size = u.size > 0 ? static_cast<std::uint32_t>(u.size) : 0U;
                        mem.array_stride_bytes =
                            u.arrayStride > 0 ? static_cast<std::uint32_t>(u.arrayStride) : 0U;
                        // Element shape only: the helper ignores array dimensions.
                        (void)std140_value_bytes(*ty, &mem.matrix_stride_bytes);
                        mem.size_bytes = mem.array_stride_bytes * mem.array_size;
                    } else {
                        mem.size_bytes = std140_value_bytes(*ty, &mem.matrix_stride_bytes);
                    }
                }
                out.uniform_blocks[static_cast<std::size_t>(u.index)].members.push_back(
                    std::move(mem));
            }
        }

        const int num_inputs = program.getNumPipeInputs();
        out.inputs.reserve(static_cast<std::size_t>(num_inputs));
        for (int ii = 0; ii < num_inputs; ++ii) {
            const glslang::TObjectReflection& in = program.getPipeInput(ii);
            if (in.name.starts_with("gl_")) {
                continue;
            }
            ShaderInputAttributeInfo attr{};
            attr.name = in.name;
            attr.location = in.layoutLocation();
            out.inputs.push_back(std::move(attr));
        }
    }

    bool initialized_ = false;
};

}  // namespace

std::shared_ptr<SpirvCompilerBackend> create_glslang_shader_compiler() {
    return std::make_shared<GlslangSpirvCompilerBackend>();
}

}  // namespace wse::asset
