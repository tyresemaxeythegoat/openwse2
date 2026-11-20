// ADVAPI32 for translated code (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design": "registry (an
// in-memory hive persisted to the writable layer), `Crypt*` hashing (real MD5/SHA-1)"; v1.1.5.6
// imports these 10, 15 call sites):
//
//   RegCreateKeyExA RegQueryValueExA RegSetValueExA RegCloseKey
//   CryptAcquireContextA CryptReleaseContext CryptCreateHash CryptHashData CryptGetHashParam
//   CryptDestroyHash
//   SystemFunction036 (RtlGenRandom; lua5164 reaches it through GetProcAddress)
//
// The registry is one in-memory tree under the four predefined roots (HKCR, HKCU, HKLM, HKU),
// names compared case-insensitively. Every change is written to `NTUSER.DAT` at the root of the
// writable layer (the profile directory, where Windows keeps the user's hive; the format is our
// own text, and HKLM is kept there too), and read back when the object is built. The embedder may
// seed values (`Advapi::set_value`). Hashes: CALG_MD5, CALG_SHA1, CALG_SHA_256, computed here.
// Registry functions return their error (LSTATUS); the Crypt functions return BOOL and set the
// last error, as on Windows. Header-only.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <map>
#include <mutex>
#include <random>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "wse/platform/vfs.h"
#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"
#include "wse/recomp/teb.h"

namespace wse::recomp::shim {

// The predefined keys as a Win64 program passes them: `(HKEY)(LONG)0x8000000n`, sign-extended.
// Windows compares only the low 32 bits (checked on 10.0.19045: 0xFFFFFFFF80000001,
// 0x0000000080000001 and 0x0000000180000001 all open HKCU), so `is_root` does too.
inline constexpr std::uint64_t kHkeyClassesRoot = 0xFFFFFFFF80000000ULL, kHkeyCurrentUser = 0xFFFFFFFF80000001ULL,
                               kHkeyLocalMachine = 0xFFFFFFFF80000002ULL, kHkeyUsers = 0xFFFFFFFF80000003ULL;
inline constexpr std::uint32_t kRegSz = 1, kRegExpandSz = 2, kRegBinary = 3, kRegDword = 4;
inline constexpr std::uint32_t kRegCreatedNewKey = 1, kRegOpenedExistingKey = 2;
inline constexpr std::uint32_t kErrorMoreData = 234;
inline constexpr std::uint32_t kCalgMd5 = 0x8003, kCalgSha1 = 0x8004, kCalgSha256 = 0x800C;
inline constexpr std::uint32_t kHpHashVal = 2, kHpHashSize = 4, kHpAlgId = 1;
inline constexpr std::uint32_t kNteBadAlgid = 0x80090008U, kNteBadHash = 0x80090002U, kNteBadType = 0x8009000AU,
                               kNteBadUid = 0x80090001U;

// --- hashes ------------------------------------------------------------------------------------
namespace hash_detail {

inline std::uint32_t rol(std::uint32_t x, unsigned n) noexcept { return (x << n) | (x >> (32 - n)); }
inline std::uint32_t ror(std::uint32_t x, unsigned n) noexcept { return (x >> n) | (x << (32 - n)); }

// Merkle-Damgard padding: the message, 0x80, zeros, then the bit length (little- or big-endian).
inline std::vector<std::uint8_t> pad(const std::vector<std::uint8_t>& m, bool big_endian) {
    std::vector<std::uint8_t> p = m;
    const std::uint64_t bits = static_cast<std::uint64_t>(m.size()) * 8;
    p.push_back(0x80);
    while (p.size() % 64 != 56) p.push_back(0);
    for (int i = 0; i < 8; ++i) {
        p.push_back(static_cast<std::uint8_t>(bits >> (big_endian ? 56 - 8 * i : 8 * i)));
    }
    return p;
}
inline std::uint32_t be32(const std::uint8_t* p) noexcept {
    return (std::uint32_t{p[0]} << 24) | (std::uint32_t{p[1]} << 16) | (std::uint32_t{p[2]} << 8) | p[3];
}

}  // namespace hash_detail

inline std::vector<std::uint8_t> md5(const std::vector<std::uint8_t>& msg) {
    using namespace hash_detail;
    static constexpr std::uint32_t kS[64] = {7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22,
                                             5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20, 5, 9,  14, 20,
                                             4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23,
                                             6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21, 6, 10, 15, 21};
    std::uint32_t k[64];
    for (int i = 0; i < 64; ++i) {
        // floor(|sin(i + 1)| * 2^32), the RFC 1321 table
        static constexpr std::uint32_t kT[64] = {
            0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
            0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
            0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
            0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
            0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
            0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
            0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
            0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391};
        k[i] = kT[i];
    }
    std::uint32_t h[4] = {0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476};
    const std::vector<std::uint8_t> p = pad(msg, false);
    for (std::size_t off = 0; off < p.size(); off += 64) {
        std::uint32_t w[16];
        std::memcpy(w, p.data() + off, 64);  // little-endian hosts (x86-64, AArch64)
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3];
        for (unsigned i = 0; i < 64; ++i) {
            std::uint32_t f = 0;
            unsigned g = 0;
            if (i < 16) { f = (b & c) | (~b & d); g = i; }
            else if (i < 32) { f = (d & b) | (~d & c); g = (5 * i + 1) % 16; }
            else if (i < 48) { f = b ^ c ^ d; g = (3 * i + 5) % 16; }
            else { f = c ^ (b | ~d); g = (7 * i) % 16; }
            const std::uint32_t t = d;
            d = c;
            c = b;
            b = b + rol(a + f + k[i] + w[g], kS[i]);
            a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d;
    }
    std::vector<std::uint8_t> out(16);
    std::memcpy(out.data(), h, 16);
    return out;
}

inline std::vector<std::uint8_t> sha1(const std::vector<std::uint8_t>& msg) {
    using namespace hash_detail;
    std::uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
    const std::vector<std::uint8_t> p = pad(msg, true);
    for (std::size_t off = 0; off < p.size(); off += 64) {
        std::uint32_t w[80];
        for (int i = 0; i < 16; ++i) w[i] = be32(p.data() + off + 4 * i);
        for (int i = 16; i < 80; ++i) w[i] = rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
        for (int i = 0; i < 80; ++i) {
            std::uint32_t f = 0, k = 0;
            if (i < 20) { f = (b & c) | (~b & d); k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d; k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else { f = b ^ c ^ d; k = 0xCA62C1D6; }
            const std::uint32_t t = rol(a, 5) + f + e + k + w[i];
            e = d; d = c; c = rol(b, 30); b = a; a = t;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
    }
    std::vector<std::uint8_t> out;
    for (const std::uint32_t v : h) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
    }
    return out;
}

inline std::vector<std::uint8_t> sha256(const std::vector<std::uint8_t>& msg) {
    using namespace hash_detail;
    static constexpr std::uint32_t kK[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};
    std::uint32_t h[8] = {0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19};
    const std::vector<std::uint8_t> p = pad(msg, true);
    for (std::size_t off = 0; off < p.size(); off += 64) {
        std::uint32_t w[64];
        for (int i = 0; i < 16; ++i) w[i] = be32(p.data() + off + 4 * i);
        for (int i = 16; i < 64; ++i) {
            const std::uint32_t s0 = ror(w[i - 15], 7) ^ ror(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const std::uint32_t s1 = ror(w[i - 2], 17) ^ ror(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        std::uint32_t a = h[0], b = h[1], c = h[2], d = h[3], e = h[4], f = h[5], g = h[6], hh = h[7];
        for (int i = 0; i < 64; ++i) {
            const std::uint32_t t1 = hh + (ror(e, 6) ^ ror(e, 11) ^ ror(e, 25)) + ((e & f) ^ (~e & g)) + kK[i] + w[i];
            const std::uint32_t t2 = (ror(a, 2) ^ ror(a, 13) ^ ror(a, 22)) + ((a & b) ^ (a & c) ^ (b & c));
            hh = g; g = f; f = e; e = d + t1; d = c; c = b; b = a; a = t1 + t2;
        }
        h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e; h[5] += f; h[6] += g; h[7] += hh;
    }
    std::vector<std::uint8_t> out;
    for (const std::uint32_t v : h) {
        for (int s = 24; s >= 0; s -= 8) out.push_back(static_cast<std::uint8_t>(v >> s));
    }
    return out;
}

// --- ADVAPI32 ----------------------------------------------------------------------------------
class Advapi {
public:
    static constexpr const char* kHiveFile = "NTUSER.DAT";

    // `user` is the writable layer (may be null: the registry then lives only in memory).
    Advapi(Runtime& rt, platform::Vfs* user) : rt_(rt), user_(user) { load(); }
    Advapi(const Advapi&) = delete;
    Advapi& operator=(const Advapi&) = delete;

    // Seeds or replaces a value: `key` like "HKEY_CURRENT_USER\Software\X" (also HKCU/HKLM).
    void set_value(const std::string& key, const std::string& name, std::uint32_t type, std::vector<std::uint8_t> data) {
        const std::lock_guard<std::mutex> lock(m_);
        std::string path = root_name(key);
        Value& v = keys_[lower(path)].values[lower(name)];
        v.name = name;
        v.type = type;
        v.data = std::move(data);
        keys_[lower(path)].path = path;
        save();
    }
    // Reads a value back (tests, the embedder). False when absent.
    bool get_value(const std::string& key, const std::string& name, std::uint32_t& type, std::vector<std::uint8_t>& data) {
        const std::lock_guard<std::mutex> lock(m_);
        const auto k = keys_.find(lower(root_name(key)));
        if (k == keys_.end()) return false;
        const auto v = k->second.values.find(lower(name));
        if (v == k->second.values.end()) return false;
        type = v->second.type;
        data = v->second.data;
        return true;
    }

    std::size_t install() {
        std::size_t n = 0;
        auto bind = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("ADVAPI32.dll", name, shim); };
        using crt_detail::get;
        using crt_detail::put;
        // RegCreateKeyExA(hKey, lpSubKey, Reserved, lpClass, dwOptions, samDesired, lpSecAttr, phkResult, lpdwDisposition)
        bind("RegCreateKeyExA", [this](CpuState& s) {
            const std::uint64_t out = win64_arg(s, 7);
            const std::uint64_t disp = win64_arg(s, 8);
            const std::lock_guard<std::mutex> lock(m_);
            std::string parent;
            if (!path_of(win64_arg(s, 0), parent)) return win64_return(s, kErrorInvalidHandle);
            const std::string sub = crt_detail::guest_string(win64_arg(s, 1));
            if (out == 0) return win64_return(s, teb::kErrorInvalidParameter);
            std::string path = parent;
            bool created = false;
            for (const std::string& part : split(sub)) {
                path += "\\" + part;
                Key& k = keys_[lower(path)];
                if (k.path.empty()) {
                    k.path = path;
                    created = true;
                }
            }
            const std::uint64_t h = next_handle_;
            next_handle_ += 4;
            handles_[h] = path;
            put<std::uint64_t>(out, h);
            if (disp != 0) put<std::uint32_t>(disp, created ? kRegCreatedNewKey : kRegOpenedExistingKey);
            if (created) save();
            win64_return(s, 0);
        });
        // RegQueryValueExA(hKey, lpValueName, lpReserved, lpType, lpData, lpcbData)
        bind("RegQueryValueExA", [this](CpuState& s) {
            const std::uint64_t type_out = win64_arg(s, 3);
            const std::uint64_t data = win64_arg(s, 4);
            const std::uint64_t size_io = win64_arg(s, 5);
            const std::lock_guard<std::mutex> lock(m_);
            std::string path;
            if (!path_of(win64_arg(s, 0), path)) return win64_return(s, kErrorInvalidHandle);
            const auto k = keys_.find(lower(path));
            const std::string name = crt_detail::guest_string(win64_arg(s, 1));
            if (k == keys_.end()) return win64_return(s, kErrorFileNotFound);
            const auto v = k->second.values.find(lower(name));
            if (v == k->second.values.end()) return win64_return(s, kErrorFileNotFound);
            if (data != 0 && size_io == 0) return win64_return(s, teb::kErrorInvalidParameter);
            if (type_out != 0) put<std::uint32_t>(type_out, v->second.type);
            const auto need = static_cast<std::uint32_t>(v->second.data.size());
            if (size_io == 0) return win64_return(s, 0);
            const std::uint32_t have = get<std::uint32_t>(size_io);
            put<std::uint32_t>(size_io, need);
            if (data == 0) return win64_return(s, 0);
            if (have < need) return win64_return(s, kErrorMoreData);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(data)), v->second.data.data(), need);
            win64_return(s, 0);
        });
        // RegSetValueExA(hKey, lpValueName, Reserved, dwType, lpData, cbData)
        bind("RegSetValueExA", [this](CpuState& s) {
            const auto type = static_cast<std::uint32_t>(win64_arg(s, 3));
            const std::uint64_t data = win64_arg(s, 4);
            const auto size = static_cast<std::uint32_t>(win64_arg(s, 5));
            const std::lock_guard<std::mutex> lock(m_);
            std::string path;
            if (!path_of(win64_arg(s, 0), path)) return win64_return(s, kErrorInvalidHandle);
            keys_[lower(path)].path = path;  // a predefined root holds values too
            if (data == 0 && size != 0) return win64_return(s, kErrorNoAccess);
            const std::string name = crt_detail::guest_string(win64_arg(s, 1));
            Value& v = keys_[lower(path)].values[lower(name)];
            v.name = name;
            v.type = type;
            v.data.assign(reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data)),
                          reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data)) + size);
            save();
            win64_return(s, 0);
        });
        bind("RegCloseKey", [this](CpuState& s) {
            const std::uint64_t h = win64_arg(s, 0);
            const std::lock_guard<std::mutex> lock(m_);
            if (is_root(h)) return win64_return(s, 0);
            win64_return(s, handles_.erase(h) != 0 ? 0 : kErrorInvalidHandle);
        });

        // CryptAcquireContextA(phProv, szContainer, szProvider, dwProvType, dwFlags)
        // SystemFunction036 (RtlGenRandom)(buffer, length) -> BOOLEAN: lua5164 (LuaJIT 2.1) finds it
        // with LoadLibraryExA("advapi32.dll") + GetProcAddress to seed its PRNG, and luaL_newstate
        // returns NULL when it is missing. Filled from the host's random source; always TRUE.
        bind("SystemFunction036", [](CpuState& s) {
            const std::uint64_t buf = win64_arg(s, 0);
            const auto len = static_cast<std::uint32_t>(win64_arg(s, 1));
            std::random_device rd;
            for (std::uint32_t i = 0; i < len && buf != 0; i += 4) {
                const std::uint32_t r = rd();
                std::memcpy(reinterpret_cast<void*>(buf + i), &r, std::min<std::uint32_t>(4, len - i));
            }
            win64_return(s, 1);
        });
        bind("CryptAcquireContextA", [this](CpuState& s) {
            const std::uint64_t out = win64_arg(s, 0);
            if (out == 0) return fail(s, teb::kErrorInvalidParameter);
            const std::lock_guard<std::mutex> lock(m_);
            const std::uint64_t h = next_handle_;
            next_handle_ += 4;
            providers_.insert(h);
            put<std::uint64_t>(out, h);
            win64_return(s, 1);
        });
        bind("CryptReleaseContext", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (providers_.erase(win64_arg(s, 0)) == 0) return fail(s, kNteBadUid);
            win64_return(s, 1);
        });
        // CryptCreateHash(hProv, Algid, hKey, dwFlags, phHash)
        bind("CryptCreateHash", [this](CpuState& s) {
            const auto alg = static_cast<std::uint32_t>(win64_arg(s, 1));
            const std::uint64_t out = win64_arg(s, 4);
            const std::lock_guard<std::mutex> lock(m_);
            if (providers_.count(win64_arg(s, 0)) == 0) return fail(s, kNteBadUid);
            if (alg != kCalgMd5 && alg != kCalgSha1 && alg != kCalgSha256) return fail(s, kNteBadAlgid);
            if (out == 0) return fail(s, teb::kErrorInvalidParameter);
            const std::uint64_t h = next_handle_;
            next_handle_ += 4;
            hashes_[h] = Hash{alg, {}, false};
            put<std::uint64_t>(out, h);
            win64_return(s, 1);
        });
        // CryptHashData(hHash, pbData, dwDataLen, dwFlags)
        bind("CryptHashData", [this](CpuState& s) {
            const std::uint64_t data = win64_arg(s, 1);
            const auto len = static_cast<std::uint32_t>(win64_arg(s, 2));
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = hashes_.find(win64_arg(s, 0));
            if (it == hashes_.end()) return fail(s, kNteBadHash);
            if (it->second.finished) return fail(s, kNteBadHashState);
            const auto* p = reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data));
            it->second.data.insert(it->second.data.end(), p, p + len);
            win64_return(s, 1);
        });
        // CryptGetHashParam(hHash, dwParam, pbData, pdwDataLen, dwFlags)
        bind("CryptGetHashParam", [this](CpuState& s) {
            const auto param = static_cast<std::uint32_t>(win64_arg(s, 1));
            const std::uint64_t out = win64_arg(s, 2);
            const std::uint64_t len_io = win64_arg(s, 3);
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = hashes_.find(win64_arg(s, 0));
            if (it == hashes_.end()) return fail(s, kNteBadHash);
            if (len_io == 0) return fail(s, teb::kErrorInvalidParameter);
            Hash& h = it->second;
            std::vector<std::uint8_t> value;
            if (param == kHpHashVal) {
                value = h.alg == kCalgMd5 ? md5(h.data) : h.alg == kCalgSha1 ? sha1(h.data) : sha256(h.data);
            } else if (param == kHpHashSize || param == kHpAlgId) {
                const std::uint32_t v = param == kHpAlgId ? h.alg : (h.alg == kCalgMd5 ? 16U : h.alg == kCalgSha1 ? 20U : 32U);
                value.resize(4);
                std::memcpy(value.data(), &v, 4);
            } else {
                return fail(s, kNteBadType);
            }
            const std::uint32_t have = get<std::uint32_t>(len_io);
            put<std::uint32_t>(len_io, static_cast<std::uint32_t>(value.size()));
            if (out == 0) return win64_return(s, 1);
            if (have < value.size()) return fail(s, kErrorMoreData);
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(out)), value.data(), value.size());
            if (param == kHpHashVal) h.finished = true;
            win64_return(s, 1);
        });
        bind("CryptDestroyHash", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (hashes_.erase(win64_arg(s, 0)) == 0) return fail(s, kNteBadHash);
            win64_return(s, 1);
        });
        return n;
    }

private:
    static constexpr std::uint32_t kErrorFileNotFound = 2, kErrorInvalidHandle = 6, kErrorNoAccess = 998;
    static constexpr std::uint32_t kNteBadHashState = 0x8009000BU;

    struct Value {
        std::string name;  // as first written
        std::uint32_t type = 0;
        std::vector<std::uint8_t> data;
    };
    struct Key {
        std::string path;  // as first created, e.g. "HKEY_CURRENT_USER\Software\X"
        std::map<std::string, Value> values;  // by lower-case name
    };
    struct Hash {
        std::uint32_t alg = 0;
        std::vector<std::uint8_t> data;
        bool finished = false;
    };

    static void fail(CpuState& s, std::uint32_t error) {
        teb::set_error(s, error);
        win64_return(s, 0);
    }
    static std::string lower(std::string s) {
        for (char& c : s) c = static_cast<char>(c >= 'A' && c <= 'Z' ? c - 'A' + 'a' : c);
        return s;
    }
    static bool is_root(std::uint64_t h) {
        const std::uint32_t low = static_cast<std::uint32_t>(h);
        return low >= 0x80000000U && low <= 0x80000003U;
    }
    static std::vector<std::string> split(const std::string& sub) {
        std::vector<std::string> parts;
        std::string cur;
        for (const char c : sub) {
            if (c == '\\') {
                if (!cur.empty()) parts.push_back(cur);
                cur.clear();
            } else {
                cur.push_back(c);
            }
        }
        if (!cur.empty()) parts.push_back(cur);
        return parts;
    }
    // "HKCU\x" -> "HKEY_CURRENT_USER\x".
    static std::string root_name(const std::string& key) {
        static const std::pair<const char*, const char*> kShort[] = {{"hkcr", "HKEY_CLASSES_ROOT"},
                                                                     {"hkcu", "HKEY_CURRENT_USER"},
                                                                     {"hklm", "HKEY_LOCAL_MACHINE"},
                                                                     {"hku", "HKEY_USERS"}};
        const std::size_t slash = key.find('\\');
        const std::string first = lower(key.substr(0, slash));
        for (const auto& [s, l] : kShort) {
            if (first == s) return std::string(l) + (slash == std::string::npos ? "" : key.substr(slash));
        }
        return key;
    }
    bool path_of(std::uint64_t h, std::string& path) const {
        static const char* const kRoots[] = {"HKEY_CLASSES_ROOT", "HKEY_CURRENT_USER", "HKEY_LOCAL_MACHINE", "HKEY_USERS"};
        if (is_root(h)) {
            path = kRoots[static_cast<std::uint32_t>(h) - 0x80000000U];
            return true;
        }
        const auto it = handles_.find(h);
        if (it == handles_.end()) return false;
        path = it->second;
        return true;
    }

    // NTUSER.DAT: "K <path>" lines, each followed by "V <type> <hex data> <name>" lines.
    void load() {
        std::vector<std::uint8_t> bytes;
        if (user_ == nullptr || !user_->read_file(kHiveFile, bytes)) return;
        const std::string text(bytes.begin(), bytes.end());
        std::size_t pos = 0;
        std::string key;
        while (pos < text.size()) {
            std::size_t end = text.find('\n', pos);
            if (end == std::string::npos) end = text.size();
            const std::string line = text.substr(pos, end - pos);
            pos = end + 1;
            if (line.rfind("K ", 0) == 0) {
                key = line.substr(2);
                keys_[lower(key)].path = key;
            } else if (line.rfind("V ", 0) == 0 && !key.empty()) {
                const std::size_t a = line.find(' ', 2);
                const std::size_t b = a == std::string::npos ? a : line.find(' ', a + 1);
                if (b == std::string::npos) continue;
                Value v;
                v.type = static_cast<std::uint32_t>(std::strtoul(line.substr(2, a - 2).c_str(), nullptr, 10));
                const std::string hex = line.substr(a + 1, b - a - 1);
                for (std::size_t i = 0; i + 1 < hex.size(); i += 2) {
                    v.data.push_back(static_cast<std::uint8_t>(std::strtoul(hex.substr(i, 2).c_str(), nullptr, 16)));
                }
                v.name = line.substr(b + 1);
                keys_[lower(key)].values[lower(v.name)] = v;
            }
        }
    }
    void save() {
        if (user_ == nullptr) return;
        std::string text = "; WSE2 Android registry (shim_advapi.h): K <key>, V <type> <hex data> <name>\n";
        for (const auto& [lk, k] : keys_) {
            (void)lk;
            text += "K " + k.path + "\n";
            for (const auto& [ln, v] : k.values) {
                (void)ln;
                char t[16];
                std::snprintf(t, sizeof(t), "%u", v.type);
                std::string hex = v.data.empty() ? "-" : "";
                for (const std::uint8_t b : v.data) {
                    char x[3];
                    std::snprintf(x, sizeof(x), "%02x", b);
                    hex += x;
                }
                text += std::string("V ") + t + " " + hex + " " + v.name + "\n";
            }
        }
        (void)user_->write_file(kHiveFile, std::span<const std::uint8_t>(reinterpret_cast<const std::uint8_t*>(text.data()), text.size()));
    }

    Runtime& rt_;
    platform::Vfs* user_ = nullptr;
    std::mutex m_;
    std::map<std::string, Key> keys_;  // by lower-case path
    std::map<std::uint64_t, std::string> handles_;
    std::set<std::uint64_t> providers_;
    std::map<std::uint64_t, Hash> hashes_;
    std::uint64_t next_handle_ = 0x5000;
};

}  // namespace wse::recomp::shim
