// Synthetic x86-64 test functions for the recompiler (no retail code).
//
// `make_synthetic_function(blocks)` emits a long function in the decoder subset, shaped
// like compiled game code: each 21-byte block loads from a buffer (`rcx`), mixes registers,
// compares and branches over one instruction. R0 (c) times translation of it; the host tests
// check the translated code against the interpreter on it.
//
//   function(const uint8_t* buf /*rcx, >= 256 readable bytes*/, uint64_t seed /*rdx*/)
//   returns rax

#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

namespace wse::recomp {

inline std::vector<std::uint8_t> make_synthetic_function(std::size_t blocks) {
    std::vector<std::uint8_t> code;
    code.reserve(blocks * 21 + 4);
    code.insert(code.end(), {0x31, 0xc0});  // xor eax, eax
    for (std::size_t i = 0; i < blocks; ++i) {
        const auto disp = static_cast<std::uint8_t>((i * 8U) & 0x78U);
        code.insert(code.end(), {
                                    0x48, 0x03, 0x41, disp,  // add rax, [rcx+disp8]
                                    0x48, 0x31, 0xc2,        // xor rdx, rax
                                    0x48, 0xff, 0xc2,        // inc rdx
                                    0x48, 0x39, 0xd0,        // cmp rax, rdx
                                    0x7c, 0x03,              // jl +3
                                    0x48, 0xff, 0xc0,        // inc rax
                                    0x48, 0x29, 0xd0,        // sub rax, rdx
                                });
    }
    code.push_back(0xc3);  // ret
    return code;
}

}  // namespace wse::recomp
