// Platform seam: maps `wse::asset` shader/material state (`wse/asset/shader.h`)
// onto the minimal RHI (`wse/platform/rhi.h`), slice P5.
//
// Port infrastructure, not a reconstruction: mirrors no retail v1.1.5.5 layout
// and carries no PE/PDB citations of its own (the decoded state it consumes does).

#pragma once

#include <algorithm>

#include "wse/asset/shader.h"
#include "wse/platform/rhi.h"

namespace wse::platform {

[[nodiscard]] inline constexpr RhiShaderStage to_rhi_shader_stage(
    wse::asset::SpirvStage stage) noexcept {
    return (stage == wse::asset::SpirvStage::Fragment) ? RhiShaderStage::fragment
                                                       : RhiShaderStage::vertex;
}

[[nodiscard]] inline constexpr wse::asset::SpirvStage to_spirv_stage(RhiShaderStage stage) noexcept {
    return (stage == RhiShaderStage::fragment) ? wse::asset::SpirvStage::Fragment
                                               : wse::asset::SpirvStage::Vertex;
}

[[nodiscard]] inline constexpr BlendMode to_rhi_blend_mode(
    wse::asset::MaterialBlendMode mode) noexcept {
    switch (mode) {
        case wse::asset::MaterialBlendMode::Opaque:
            return BlendMode::opaque;
        case wse::asset::MaterialBlendMode::AlphaBlend:
            return BlendMode::alpha;
        case wse::asset::MaterialBlendMode::AdditiveAlpha:
            return BlendMode::additive_alpha;
        case wse::asset::MaterialBlendMode::Multiply:
            return BlendMode::multiply;
        case wse::asset::MaterialBlendMode::Additive:
            return BlendMode::additive;
    }
    return BlendMode::opaque;
}

// Builds the pipeline state for one decoded material. Shader handles are left 0
// for the caller to fill; Warband `.brf` meshes are D3D9-authored, so front faces
// are clockwise and the D3D9 half-pixel offset is compensated.
[[nodiscard]] inline PipelineDesc make_pipeline_desc(const wse::asset::MaterialRenderState& state,
                                                     bool srgb_framebuffer = true) noexcept {
    PipelineDesc desc{};
    desc.depth_test = state.depth_test;
    desc.depth_write = state.depth_write;
    desc.cull_mode = CullMode::back;
    desc.front_face = FrontFace::clockwise;
    desc.blend_mode = to_rhi_blend_mode(state.blend_mode);
    desc.alpha_test = state.alpha_ref_u8 != 0U;
    desc.alpha_ref = state.alpha_ref_u8;
    desc.srgb_texture = state.srgb_diffuse;
    desc.srgb_framebuffer = srgb_framebuffer;
    desc.enable_lighting = true;
    desc.use_normal_map = state.uses_normal_map;
    desc.use_specular_map = !state.specular_map.empty();
    desc.compensate_d3d9_half_pixel = true;
    // `BrfMaterial::specular_coeff` is rglMaterial's specular power; the specular
    // colour's brightest channel scales the highlight (1 for a map-only material).
    const float rgb_max = std::max({state.specular_rgb.x, state.specular_rgb.y, state.specular_rgb.z});
    desc.specular_coefficient =
        state.uses_specular ? ((rgb_max > 0.0f) ? rgb_max : 1.0f) : 0.0f;
    desc.specular_power = (state.specular_coeff >= 1.0f) ? state.specular_coeff : 16.0f;
    return desc;
}

}  // namespace wse::platform
