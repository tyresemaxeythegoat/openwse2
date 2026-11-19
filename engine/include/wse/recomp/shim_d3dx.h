// D3DX9 math for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design": "d3dx9_43 ...
// math functions (D3DXMatrix*, D3DXVec*) real"; v1.1.5.6 imports 17 D3DX functions, these 10 are
// math):
//
//   D3DXMatrixMultiply D3DXMatrixTranspose D3DXMatrixInverse D3DXMatrixOrthoOffCenterRH
//   D3DXMatrixPerspectiveOffCenterRH D3DXPlaneNormalize D3DXVec3Normalize D3DXVec4Normalize
//   D3DXVec3Transform D3DXVec4Transform
//
// Matrices are D3DXMATRIX (16 row-major floats, row vectors: v' = v * M); every function returns
// its output pointer (D3DXMatrixInverse returns NULL for a singular matrix), and the output may
// alias an input. Computed in float as D3DX does, and compared with the system's d3dx9_43.dll on
// the user's PC (20,000 random cases each): all agree to 1e-4 relative except the inverse of
// ill-conditioned matrices, where both are float-accurate (same residual |M * inverse - I|) but
// round differently. The texture, surface and effect functions
// belong to the recording D3D9 device. Header-only.

#pragma once

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"

namespace wse::recomp::shim {

namespace d3dx {

using Mat = float[16];

inline void multiply(float* out, const float* a, const float* b) noexcept {
    float r[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            r[i * 4 + j] = a[i * 4 + 0] * b[0 * 4 + j] + a[i * 4 + 1] * b[1 * 4 + j] + a[i * 4 + 2] * b[2 * 4 + j] +
                           a[i * 4 + 3] * b[3 * 4 + j];
        }
    }
    std::memcpy(out, r, sizeof(r));
}
inline void transpose(float* out, const float* m) noexcept {
    float r[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) r[i * 4 + j] = m[j * 4 + i];
    }
    std::memcpy(out, r, sizeof(r));
}
// Cofactor inverse; false (output untouched) when the determinant is 0.
inline bool inverse(float* out, float* det_out, const float* m) noexcept {
    float inv[16];
    inv[0] = m[5] * m[10] * m[15] - m[5] * m[11] * m[14] - m[9] * m[6] * m[15] + m[9] * m[7] * m[14] + m[13] * m[6] * m[11] - m[13] * m[7] * m[10];
    inv[4] = -m[4] * m[10] * m[15] + m[4] * m[11] * m[14] + m[8] * m[6] * m[15] - m[8] * m[7] * m[14] - m[12] * m[6] * m[11] + m[12] * m[7] * m[10];
    inv[8] = m[4] * m[9] * m[15] - m[4] * m[11] * m[13] - m[8] * m[5] * m[15] + m[8] * m[7] * m[13] + m[12] * m[5] * m[11] - m[12] * m[7] * m[9];
    inv[12] = -m[4] * m[9] * m[14] + m[4] * m[10] * m[13] + m[8] * m[5] * m[14] - m[8] * m[6] * m[13] - m[12] * m[5] * m[10] + m[12] * m[6] * m[9];
    inv[1] = -m[1] * m[10] * m[15] + m[1] * m[11] * m[14] + m[9] * m[2] * m[15] - m[9] * m[3] * m[14] - m[13] * m[2] * m[11] + m[13] * m[3] * m[10];
    inv[5] = m[0] * m[10] * m[15] - m[0] * m[11] * m[14] - m[8] * m[2] * m[15] + m[8] * m[3] * m[14] + m[12] * m[2] * m[11] - m[12] * m[3] * m[10];
    inv[9] = -m[0] * m[9] * m[15] + m[0] * m[11] * m[13] + m[8] * m[1] * m[15] - m[8] * m[3] * m[13] - m[12] * m[1] * m[11] + m[12] * m[3] * m[9];
    inv[13] = m[0] * m[9] * m[14] - m[0] * m[10] * m[13] - m[8] * m[1] * m[14] + m[8] * m[2] * m[13] + m[12] * m[1] * m[10] - m[12] * m[2] * m[9];
    inv[2] = m[1] * m[6] * m[15] - m[1] * m[7] * m[14] - m[5] * m[2] * m[15] + m[5] * m[3] * m[14] + m[13] * m[2] * m[7] - m[13] * m[3] * m[6];
    inv[6] = -m[0] * m[6] * m[15] + m[0] * m[7] * m[14] + m[4] * m[2] * m[15] - m[4] * m[3] * m[14] - m[12] * m[2] * m[7] + m[12] * m[3] * m[6];
    inv[10] = m[0] * m[5] * m[15] - m[0] * m[7] * m[13] - m[4] * m[1] * m[15] + m[4] * m[3] * m[13] + m[12] * m[1] * m[7] - m[12] * m[3] * m[5];
    inv[14] = -m[0] * m[5] * m[14] + m[0] * m[6] * m[13] + m[4] * m[1] * m[14] - m[4] * m[2] * m[13] - m[12] * m[1] * m[6] + m[12] * m[2] * m[5];
    inv[3] = -m[1] * m[6] * m[11] + m[1] * m[7] * m[10] + m[5] * m[2] * m[11] - m[5] * m[3] * m[10] - m[9] * m[2] * m[7] + m[9] * m[3] * m[6];
    inv[7] = m[0] * m[6] * m[11] - m[0] * m[7] * m[10] - m[4] * m[2] * m[11] + m[4] * m[3] * m[10] + m[8] * m[2] * m[7] - m[8] * m[3] * m[6];
    inv[11] = -m[0] * m[5] * m[11] + m[0] * m[7] * m[9] + m[4] * m[1] * m[11] - m[4] * m[3] * m[9] - m[8] * m[1] * m[7] + m[8] * m[3] * m[5];
    inv[15] = m[0] * m[5] * m[10] - m[0] * m[6] * m[9] - m[4] * m[1] * m[10] + m[4] * m[2] * m[9] + m[8] * m[1] * m[6] - m[8] * m[2] * m[5];
    const float det = m[0] * inv[0] + m[1] * inv[4] + m[2] * inv[8] + m[3] * inv[12];
    if (det == 0.0F) return false;  // d3dx9_43 leaves *pDeterminant unwritten here (checked on Windows)
    if (det_out != nullptr) *det_out = det;
    const float k = 1.0F / det;
    for (float& v : inv) v *= k;
    std::memcpy(out, inv, sizeof(inv));
    return true;
}
inline void ortho_off_center_rh(float* out, float l, float r, float b, float t, float zn, float zf) noexcept {
    const float m[16] = {2.0F / (r - l), 0, 0, 0, 0, 2.0F / (t - b), 0, 0, 0, 0, 1.0F / (zn - zf), 0,
                         (l + r) / (l - r), (t + b) / (b - t), zn / (zn - zf), 1};
    std::memcpy(out, m, sizeof(m));
}
inline void perspective_off_center_rh(float* out, float l, float r, float b, float t, float zn, float zf) noexcept {
    const float m[16] = {2.0F * zn / (r - l), 0, 0, 0, 0, 2.0F * zn / (t - b), 0, 0,
                         (l + r) / (r - l), (t + b) / (t - b), zf / (zn - zf), -1, 0, 0, zn * zf / (zn - zf), 0};
    std::memcpy(out, m, sizeof(m));
}
// Normalizes the first `n` components (D3DX: a zero vector stays zero). For a plane, by |(a,b,c)|.
inline void normalize(float* out, const float* v, int n, int len_of) noexcept {
    float sq = 0;
    for (int i = 0; i < len_of; ++i) sq += v[i] * v[i];
    const float len = std::sqrt(sq);
    float r[4] = {};
    for (int i = 0; i < n; ++i) r[i] = len != 0.0F ? v[i] / len : 0.0F;
    std::memcpy(out, r, sizeof(float) * static_cast<std::size_t>(n));
}
// out(4) = (v[0..n), w) * M, w = 1 for a 3-vector.
inline void transform(float* out, const float* v, int n, const float* m) noexcept {
    const float in[4] = {v[0], v[1], v[2], n == 4 ? v[3] : 1.0F};
    float r[4];
    for (int j = 0; j < 4; ++j) r[j] = in[0] * m[j] + in[1] * m[4 + j] + in[2] * m[8 + j] + in[3] * m[12 + j];
    std::memcpy(out, r, sizeof(r));
}

}  // namespace d3dx

class D3dxMath {
public:
    explicit D3dxMath(Runtime& rt) : rt_(rt) {}

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("d3dx9_43.dll", name, shim); };
        bind("D3DXMatrixMultiply", [](CpuState& s) {
            d3dx::multiply(f(win64_arg(s, 0)), f(win64_arg(s, 1)), f(win64_arg(s, 2)));
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXMatrixTranspose", [](CpuState& s) {
            d3dx::transpose(f(win64_arg(s, 0)), f(win64_arg(s, 1)));
            win64_return(s, win64_arg(s, 0));
        });
        // D3DXMatrixInverse(pOut, pDeterminant, pM)
        bind("D3DXMatrixInverse", [](CpuState& s) {
            const bool ok = d3dx::inverse(f(win64_arg(s, 0)), win64_arg(s, 1) != 0 ? f(win64_arg(s, 1)) : nullptr, f(win64_arg(s, 2)));
            win64_return(s, ok ? win64_arg(s, 0) : 0);
        });
        // (pOut, l, r, b, t, zn, zf): l r b in xmm1-xmm3, t zn zf in the stack slots.
        bind("D3DXMatrixOrthoOffCenterRH", [](CpuState& s) {
            d3dx::ortho_off_center_rh(f(win64_arg(s, 0)), win64_arg_f32(s, 1), win64_arg_f32(s, 2), win64_arg_f32(s, 3),
                                      stack_f32(s, 4), stack_f32(s, 5), stack_f32(s, 6));
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXMatrixPerspectiveOffCenterRH", [](CpuState& s) {
            d3dx::perspective_off_center_rh(f(win64_arg(s, 0)), win64_arg_f32(s, 1), win64_arg_f32(s, 2), win64_arg_f32(s, 3),
                                            stack_f32(s, 4), stack_f32(s, 5), stack_f32(s, 6));
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXPlaneNormalize", [](CpuState& s) {
            d3dx::normalize(f(win64_arg(s, 0)), f(win64_arg(s, 1)), 4, 3);
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXVec3Normalize", [](CpuState& s) {
            d3dx::normalize(f(win64_arg(s, 0)), f(win64_arg(s, 1)), 3, 3);
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXVec4Normalize", [](CpuState& s) {
            d3dx::normalize(f(win64_arg(s, 0)), f(win64_arg(s, 1)), 4, 4);
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXVec3Transform", [](CpuState& s) {
            d3dx::transform(f(win64_arg(s, 0)), f(win64_arg(s, 1)), 3, f(win64_arg(s, 2)));
            win64_return(s, win64_arg(s, 0));
        });
        bind("D3DXVec4Transform", [](CpuState& s) {
            d3dx::transform(f(win64_arg(s, 0)), f(win64_arg(s, 1)), 4, f(win64_arg(s, 2)));
            win64_return(s, win64_arg(s, 0));
        });
        return n;
    }

private:
    static float* f(std::uint64_t va) noexcept { return reinterpret_cast<float*>(static_cast<std::uintptr_t>(va)); }
    // A float argument at position `i` >= 4: the low 4 bytes of its 8-byte stack slot.
    static float stack_f32(const CpuState& s, int i) noexcept {
        float v = 0;
        std::memcpy(&v, reinterpret_cast<const void*>(s.gpr[4] + 0x28 + 8 * static_cast<std::uint64_t>(i - 4)), 4);
        return v;
    }

    Runtime& rt_;
};

}  // namespace wse::recomp::shim
