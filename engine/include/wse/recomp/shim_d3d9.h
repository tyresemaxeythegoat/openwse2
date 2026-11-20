// The recording D3D9 device, its D3DX texture and effect functions, and DirectInput 8 (Phase 5 R4,
// `docs/RECOMP_PLAN.md` "R4 design": "R4 stops at a recording stub D3D9 device: Direct3DCreate9 and
// D3DX return COM objects whose methods succeed and are logged, so startup reaches the main loop and
// frames tick headless. R5 replaces the stub with the Vulkan-backed device"):
//
//   d3d9      Direct3DCreate9
//   d3dx9_43  D3DXCreateTexture D3DXCreateTextureFromFileExA D3DXCreateTextureFromFileInMemoryEx
//             D3DXCreateCubeTextureFromFileExA D3DXFilterTexture D3DXSaveSurfaceToFileA
//             D3DXCreateEffectFromFileA
//   DINPUT8   DirectInput8Create
//
// Objects are guest memory whose first word is a vtable of callable addresses
// (`Runtime::add_callable`); the method order comes from `com_vtables.h` (generated from the
// mingw-w64 headers). Each method is one of:
//   implemented  adapter and mode queries, caps, device and resource creation (textures, surfaces,
//                buffers with real memory to lock), state the device stores and returns, D3DX
//                texture/effect creation (sizes from DDS headers when the file can be read),
//                DirectInput devices with empty state
//   recorded     a method that only changes state, draws or presents (`Set*`, `Begin*`, `End*`,
//                `Draw*`, `Clear`, `Present`, ...): succeeds and is counted
//   missing      anything else: faults with its name ("IDirect3DDevice9::GetLight"), the next
//                thing to implement, as a missing import does
// Nothing is drawn. Header-only.

#pragma once

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <vector>

#include "wse/recomp/com_vtables.h"
#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kD3dOk = 0, kD3dErrInvalidCall = 0x8876086CU, kD3dErrNotAvailable = 0x8876086AU,
                               kENoInterface = 0x80004002U, kD3dxErrInvalidData = 0x88760B59U;
inline constexpr std::uint32_t kD3dFmtA8R8G8B8 = 21, kD3dFmtX8R8G8B8 = 22, kD3dFmtD24S8 = 75;
inline constexpr std::uint32_t kD3dxDefault = 0xFFFFFFFFU;

namespace d3d9_detail {
// A resource's shape: textures (levels of `width` x `height`, `depth` for volumes), surfaces,
// buffers (`size` bytes).
struct Desc {
    std::uint32_t width = 0, height = 0, depth = 1, levels = 1, format = 0, usage = 0, pool = 0, size = 0, fvf = 0;
};
}  // namespace d3d9_detail

struct D3d9Config {
    int width = 1920;
    int height = 1080;
    int refresh = 60;
    std::uint64_t texture_memory = 1ULL << 30;
};

class D3d9 {
public:
    enum class Kind { d3d, device, swapchain, surface, texture, cube, volume, vbuffer, ibuffer, decl, vshader, pshader,
                      stateblock, query, effect, effect_pool, buffer, dinput, didevice };

    D3d9(Runtime& rt, D3d9Config cfg = {}) : rt_(rt), cfg_(cfg) {}
    D3d9(const D3d9&) = delete;
    D3d9& operator=(const D3d9&) = delete;

    // Reads a file the game names (D3DX texture headers); unset: sizes default to 256 x 256.
    std::function<bool(const std::string& path, std::vector<std::uint8_t>& bytes)> read_file;
    // The first time each recorded method is called (bring-up log).
    std::function<void(const std::string& method)> on_first_call;

    [[nodiscard]] std::uint64_t frames() const noexcept { return frames_; }
    [[nodiscard]] std::size_t objects() const {
        const std::lock_guard<std::mutex> lock(m_);
        return objs_.size();
    }

    std::size_t install() {
        build_vtables();
        std::size_t n = 0;
        using crt_detail::put;
        n += rt_.set_import("d3d9.dll", "Direct3DCreate9", [this](CpuState& s) {
            win64_return(s, create(Kind::d3d));
        });
        // DirectInput8Create(hinst, dwVersion, riidltf, ppvOut, punkOuter)
        n += rt_.set_import("DINPUT8.dll", "DirectInput8Create", [this](CpuState& s) {
            const std::uint64_t riid = win64_arg(s, 2);
            const std::uint64_t out = win64_arg(s, 3);
            if (out == 0) return win64_return(s, kD3dErrInvalidCall);
            const bool a = riid != 0 && iid_is(riid, com::kIDirectInput8A);
            const bool w = riid != 0 && iid_is(riid, com::kIDirectInput8W);
            if (!a && !w) {
                put<std::uint64_t>(out, 0);
                return win64_return(s, kENoInterface);
            }
            const std::uint64_t obj = create(Kind::dinput, w ? 1 : 0);
            put<std::uint64_t>(out, obj);
            win64_return(s, 0);
        });
        auto dx = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("d3dx9_43.dll", name, shim); };
        // D3DXCreateTexture(device, w, h, mips, usage, fmt, pool, ppTex)
        dx("D3DXCreateTexture", [this](CpuState& s) {
            const std::uint64_t out = win64_arg(s, 7);
            const auto fmt = static_cast<std::uint32_t>(win64_arg(s, 5));
            Desc d = texture_desc(size_or(win64_arg(s, 1), 256), size_or(win64_arg(s, 2), 256), 1,
                                  static_cast<std::uint32_t>(win64_arg(s, 3)), fmt == 0 || fmt == kD3dxDefault ? kD3dFmtA8R8G8B8 : fmt,
                                  static_cast<std::uint32_t>(win64_arg(s, 4)), static_cast<std::uint32_t>(win64_arg(s, 6)));
            win64_return(s, out_object(out, Kind::texture, d));
        });
        // D3DXCreateTextureFromFileExA(dev, file, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, ppTex)
        dx("D3DXCreateTextureFromFileExA", [this](CpuState& s) {
            std::vector<std::uint8_t> bytes;
            const std::string file = crt_detail::guest_string(win64_arg(s, 1));
            const bool have = read_file && read_file(file, bytes);
            if (read_file && !have) return win64_return(s, kD3dxErrInvalidData);
            texture_from_image(s, bytes, 2, 11, 13, Kind::texture);
        });
        // D3DXCreateTextureFromFileInMemoryEx(dev, data, size, w, h, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, ppTex)
        dx("D3DXCreateTextureFromFileInMemoryEx", [this](CpuState& s) {
            const std::uint64_t data = win64_arg(s, 1);
            const auto size = static_cast<std::size_t>(win64_arg(s, 2));
            std::vector<std::uint8_t> bytes;
            if (data != 0) {
                bytes.assign(reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data)),
                             reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(data)) + size);
            }
            texture_from_image(s, bytes, 3, 12, 14, Kind::texture);
        });
        // D3DXCreateCubeTextureFromFileExA(dev, file, size, mips, usage, fmt, pool, filter, mipfilter, key, info, pal, ppCube)
        dx("D3DXCreateCubeTextureFromFileExA", [this](CpuState& s) {
            std::vector<std::uint8_t> bytes;
            const std::string file = crt_detail::guest_string(win64_arg(s, 1));
            if (read_file && !read_file(file, bytes)) return win64_return(s, kD3dxErrInvalidData);
            Image img = parse_image(bytes);
            const auto edge = size_or(win64_arg(s, 2), img.width);
            const auto fmt = static_cast<std::uint32_t>(win64_arg(s, 5));
            Desc d = texture_desc(edge, edge, 1, static_cast<std::uint32_t>(win64_arg(s, 3)) == kD3dxDefault ? 0 : static_cast<std::uint32_t>(win64_arg(s, 3)),
                                  fmt == 0 || fmt == kD3dxDefault ? img.format : fmt, static_cast<std::uint32_t>(win64_arg(s, 4)),
                                  static_cast<std::uint32_t>(win64_arg(s, 6)));
            put_image_info(win64_arg(s, 10), img);
            win64_return(s, out_object(win64_arg(s, 12), Kind::cube, d));
        });
        dx("D3DXFilterTexture", [](CpuState& s) { win64_return(s, 0); });
        dx("D3DXSaveSurfaceToFileA", [](CpuState& s) { win64_return(s, 0); });  // nothing is written
        // D3DXCreateEffectFromFileA(dev, file, defines, include, flags, pool, ppEffect, ppErrors)
        dx("D3DXCreateEffectFromFileA", [this](CpuState& s) {
            if (win64_arg(s, 7) != 0) put<std::uint64_t>(win64_arg(s, 7), 0);
            if (win64_arg(s, 6) == 0) return win64_return(s, kD3dErrInvalidCall);
            put<std::uint64_t>(win64_arg(s, 6), create(Kind::effect));
            win64_return(s, 0);
        });
        return n;
    }

private:
    using Desc = d3d9_detail::Desc;
    struct Obj {
        Kind kind = Kind::d3d;
        int variant = 0;  // DirectInput: 0 = A, 1 = W
        std::uint32_t refs = 1;
        Desc desc;
        std::vector<std::size_t> sizes;                  // per level (x6 faces for a cube) or the buffer
        std::vector<std::vector<std::uint8_t>> memory;   // allocated on first lock (`bytes`)
        bool child = false;                              // a texture level's surface: memory is the parent's
        std::map<std::uint64_t, std::uint64_t> children;  // level/face -> surface object
        std::uint64_t parent = 0;   // a surface's texture, an object's device
        std::uint64_t memory_index = 0;  // a surface: which entry of its parent's memory
    };
    struct Image {
        std::uint32_t width = 256, height = 256, depth = 1, levels = 1, format = kD3dFmtA8R8G8B8, type = 3, file_format = 1;
    };
    struct DeviceState {
        std::map<std::uint32_t, std::uint32_t> render, sampler, stage;
        std::map<std::uint32_t, std::array<float, 16>> transforms;
        std::map<std::uint32_t, std::uint64_t> textures;
        std::uint64_t decl = 0, vs = 0, ps = 0, indices = 0, fvf = 0;
        std::map<std::uint32_t, std::array<std::uint64_t, 3>> streams;  // buffer, offset, stride
        std::array<std::uint32_t, 6> viewport{};  // X Y Width Height and MinZ MaxZ as bits
        std::array<std::int32_t, 4> scissor{};
        std::uint64_t backbuffer = 0, depth = 0, swapchain = 0, d3d = 0, focus = 0;
        std::uint32_t behavior = 0;
        std::vector<std::uint8_t> present;  // D3DPRESENT_PARAMETERS
    };
    using Method = std::function<void(CpuState&, Obj&, std::uint64_t self)>;

    static constexpr std::uint32_t kD3dPool = 0;

    // --- helpers ---------------------------------------------------------------------------
    static std::uint32_t size_or(std::uint64_t v, std::uint32_t fallback) {
        const auto x = static_cast<std::uint32_t>(v);
        return x == 0 || x == kD3dxDefault || x == 0xFFFFFFFEU ? fallback : x;  // D3DX_DEFAULT, D3DX_DEFAULT_NONPOW2
    }
    static bool iid_is(std::uint64_t riid, const com::Interface& i) {
        return std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(riid)), i.iid, 16) == 0;
    }
    static bool is_dxt(std::uint32_t f) { return f >= 0x31545844U && f <= 0x35545844U && (f & 0xFFFFFF) == 0x545844; }
    static std::uint32_t bytes_per_pixel(std::uint32_t f) {
        switch (f) {
        case 28: case 50: case 41: return 1;                               // A8 L8 P8
        case 23: case 24: case 25: case 26: case 51: case 80: case 111: return 2;  // 565 555 1555 4444 A8L8 D16 R16F
        case 113: case 115: return 8;                                       // A16B16G16R16F G32R32F
        case 116: return 16;                                               // A32B32G32R32F
        default: return 4;
        }
    }
    static std::uint32_t pitch_of(std::uint32_t f, std::uint32_t w) {
        if (is_dxt(f)) return std::max<std::uint32_t>(1, (w + 3) / 4) * (f == 0x31545844U ? 8 : 16);
        return std::max<std::uint32_t>(1, w) * bytes_per_pixel(f);
    }
    static std::uint32_t rows_of(std::uint32_t f, std::uint32_t h) { return is_dxt(f) ? std::max<std::uint32_t>(1, (h + 3) / 4) : std::max<std::uint32_t>(1, h); }
    static std::uint32_t level_dim(std::uint32_t v, std::uint32_t level) { return std::max<std::uint32_t>(1, v >> level); }
    static Desc texture_desc(std::uint32_t w, std::uint32_t h, std::uint32_t depth, std::uint32_t levels, std::uint32_t fmt,
                             std::uint32_t usage, std::uint32_t pool) {
        Desc d;
        d.width = std::max<std::uint32_t>(1, w);
        d.height = std::max<std::uint32_t>(1, h);
        d.depth = std::max<std::uint32_t>(1, depth);
        std::uint32_t full = 1;
        for (std::uint32_t m = std::max(std::max(d.width, d.height), d.depth); m > 1; m >>= 1) ++full;
        d.levels = levels == 0 || levels == kD3dxDefault || levels > full ? full : levels;
        d.format = fmt;
        d.usage = usage;
        d.pool = pool;
        return d;
    }

    void first_call(const std::string& name) {
        bool first = false;
        {
            const std::lock_guard<std::mutex> lock(m_);
            first = seen_.insert(name).second;
        }
        if (first && on_first_call) on_first_call(name);
    }

    // A guest object: 64 bytes whose first word is the vtable of `kind`.
    std::uint64_t create(Kind kind, int variant = 0, Desc desc = {}, std::uint64_t parent = 0, bool child = false) {
        auto mem = std::make_unique<std::uint8_t[]>(64);
        std::memset(mem.get(), 0, 64);
        const auto addr = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mem.get()));
        const std::uint64_t vt = vtable_of(kind, variant);
        std::memcpy(mem.get(), &vt, 8);
        auto o = std::make_unique<Obj>();
        o->kind = kind;
        o->variant = variant;
        o->desc = desc;
        o->parent = parent;
        o->child = child;
        if (kind == Kind::texture || kind == Kind::cube || kind == Kind::volume) {
            const std::uint32_t faces = kind == Kind::cube ? 6 : 1;
            for (std::uint32_t f = 0; f < faces; ++f) {
                for (std::uint32_t l = 0; l < desc.levels; ++l) {
                    const std::uint32_t w = level_dim(desc.width, l), h = level_dim(desc.height, l), dd = level_dim(desc.depth, l);
                    o->sizes.push_back(static_cast<std::size_t>(pitch_of(desc.format, w)) * rows_of(desc.format, h) * dd);
                }
            }
        } else if (kind == Kind::surface && !child) {
            o->sizes.push_back(static_cast<std::size_t>(pitch_of(desc.format, desc.width)) * rows_of(desc.format, desc.height));
        } else if (kind == Kind::vbuffer || kind == Kind::ibuffer) {
            o->sizes.push_back(desc.size);
        }
        o->memory.resize(o->sizes.size());
        if (kind == Kind::device) devices_[addr] = std::make_unique<DeviceState>();
        const std::lock_guard<std::mutex> lock(m_);
        blocks_.push_back(std::move(mem));
        objs_[addr] = std::move(o);
        return addr;
    }
    Obj* obj(std::uint64_t addr) {
        const std::lock_guard<std::mutex> lock(m_);
        const auto it = objs_.find(addr);
        return it == objs_.end() ? nullptr : it->second.get();
    }
    std::uint32_t out_object(std::uint64_t out, Kind kind, const Desc& d, std::uint64_t parent = 0) {
        if (out == 0) return kD3dErrInvalidCall;
        crt_detail::put<std::uint64_t>(out, create(kind, 0, d, parent));
        return kD3dOk;
    }
    void add_ref(std::uint64_t addr) {
        if (Obj* o = obj(addr)) ++o->refs;
    }
    // Hands `addr` out through `out` with a reference, as Get* methods do.
    static void put_ref(D3d9& self, std::uint64_t out, std::uint64_t addr) {
        if (out == 0) return;
        if (addr != 0) self.add_ref(addr);
        crt_detail::put<std::uint64_t>(out, addr);
    }
    // A surface of `parent` for memory entry `index` (made once).
    std::uint64_t child_surface(std::uint64_t parent, std::uint64_t index) {
        Obj* p = obj(parent);
        if (p == nullptr || index >= p->sizes.size()) return 0;
        {
            const std::lock_guard<std::mutex> lock(m_);
            const auto it = p->children.find(index);
            if (it != p->children.end()) return it->second;
        }
        const std::uint32_t level = static_cast<std::uint32_t>(index % p->desc.levels);
        Desc d = p->desc;
        d.width = level_dim(p->desc.width, level);
        d.height = level_dim(p->desc.height, level);
        d.levels = 1;
        const std::uint64_t s = create(Kind::surface, 0, d, parent, true);
        Obj* so = obj(s);
        so->memory_index = index;
        const std::lock_guard<std::mutex> lock(m_);
        p->children[index] = s;
        return s;
    }
    // Memory entry `i` of `o`, allocated on first use (a texture's levels cost memory only once locked).
    std::vector<std::uint8_t>* bytes(Obj& o, std::size_t i) {
        const std::lock_guard<std::mutex> lock(m_);
        if (i >= o.sizes.size()) return nullptr;
        if (o.memory[i].size() != o.sizes[i]) o.memory[i].assign(o.sizes[i], 0);
        return &o.memory[i];
    }
    std::vector<std::uint8_t>* surface_memory(Obj& o) {
        if (o.child) {
            Obj* p = obj(o.parent);
            return p != nullptr ? bytes(*p, o.memory_index) : nullptr;
        }
        return bytes(o, 0);
    }
    // The back buffer, the automatic depth surface and the swap chain, from D3DPRESENT_PARAMETERS
    // (BackBufferWidth Height Format Count ... EnableAutoDepthStencil@44 AutoDepthStencilFormat@48).
    void reset_buffers(std::uint64_t device, DeviceState& st) {
        auto u32 = [&](std::size_t off) {
            std::uint32_t v = 0;
            if (off + 4 <= st.present.size()) std::memcpy(&v, st.present.data() + off, 4);
            return v;
        };
        Desc d;
        d.width = u32(0) != 0 ? u32(0) : static_cast<std::uint32_t>(cfg_.width);
        d.height = u32(4) != 0 ? u32(4) : static_cast<std::uint32_t>(cfg_.height);
        d.format = u32(8) != 0 ? u32(8) : kD3dFmtX8R8G8B8;
        d.usage = 1;  // D3DUSAGE_RENDERTARGET
        st.backbuffer = create(Kind::surface, 0, d, device);
        if (u32(44) != 0) {
            Desc z = d;
            z.format = u32(48);
            z.usage = 2;  // D3DUSAGE_DEPTHSTENCIL
            st.depth = create(Kind::surface, 0, z, device);
        }
        if (st.swapchain == 0) st.swapchain = create(Kind::swapchain, 0, {}, device);
        const float zr[2] = {0.0F, 1.0F};
        st.viewport = {0, 0, d.width, d.height, 0, 0};
        std::memcpy(&st.viewport[4], zr, 8);
        st.scissor = {0, 0, static_cast<std::int32_t>(d.width), static_cast<std::int32_t>(d.height)};
    }
    // CreateRenderTarget / CreateDepthStencilSurface (w, h, fmt, ms, msq, flag, pp, sh) and
    // CreateOffscreenPlainSurface (w, h, fmt, pool, pp, sh); `which`: 0 offscreen, 1 target, 2 depth.
    std::uint32_t plain_surface(CpuState& s, std::uint64_t device, int out, int which) {
        Desc d;
        d.width = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(win64_arg(s, 1)));
        d.height = std::max<std::uint32_t>(1, static_cast<std::uint32_t>(win64_arg(s, 2)));
        d.format = static_cast<std::uint32_t>(win64_arg(s, 3));
        d.usage = which == 1 ? 1U : which == 2 ? 2U : 0U;
        d.pool = which == 0 ? static_cast<std::uint32_t>(win64_arg(s, 4)) : 0U;
        return out_object(win64_arg(s, out), Kind::surface, d, device);
    }
    // D3DSURFACE_DESC: Format Type Usage Pool MultiSampleType MultiSampleQuality Width Height.
    static void put_surface_desc(std::uint64_t p, const Desc& d, std::uint32_t type) {
        if (p == 0) return;
        const std::uint32_t v[8] = {d.format, type, d.usage, d.pool, 0, 0, d.width, d.height};
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), v, sizeof(v));
    }
    // D3DLOCKED_RECT { INT Pitch; void* pBits } at a rectangle's top-left.
    static std::uint32_t lock_rect(std::uint64_t locked, std::vector<std::uint8_t>* mem, std::uint32_t fmt, std::uint32_t w,
                                   std::uint64_t rect) {
        if (locked == 0 || mem == nullptr) return kD3dErrInvalidCall;
        const std::uint32_t pitch = pitch_of(fmt, w);
        std::size_t off = 0;
        if (rect != 0) {
            const auto l = static_cast<std::uint32_t>(crt_detail::get<std::int32_t>(rect));
            const auto t = static_cast<std::uint32_t>(crt_detail::get<std::int32_t>(rect + 4));
            off = is_dxt(fmt) ? static_cast<std::size_t>(t / 4) * pitch + (l / 4) * (fmt == 0x31545844U ? 8 : 16)
                              : static_cast<std::size_t>(t) * pitch + static_cast<std::size_t>(l) * bytes_per_pixel(fmt);
            if (off >= mem->size()) off = 0;
        }
        crt_detail::put<std::int32_t>(locked, static_cast<std::int32_t>(pitch));
        crt_detail::put<std::uint64_t>(locked + 8, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mem->data() + off)));
        return kD3dOk;
    }

    // The header of a DDS file (size, format, levels); other formats default to 256 x 256 ARGB.
    static Image parse_image(const std::vector<std::uint8_t>& b) {
        Image img;
        if (b.size() >= 128 && std::memcmp(b.data(), "DDS ", 4) == 0) {
            auto u32 = [&](std::size_t off) {
                std::uint32_t v = 0;
                std::memcpy(&v, b.data() + off, 4);
                return v;
            };
            img.height = std::max<std::uint32_t>(1, u32(12));
            img.width = std::max<std::uint32_t>(1, u32(16));
            img.depth = (u32(8) & 0x800000U) != 0 ? std::max<std::uint32_t>(1, u32(24)) : 1;  // DDSD_DEPTH
            img.levels = std::max<std::uint32_t>(1, u32(28));
            const std::uint32_t pf_flags = u32(80);
            if ((pf_flags & 4) != 0) {  // DDPF_FOURCC
                img.format = u32(84);
            } else {
                const std::uint32_t bits = u32(88);
                img.format = bits == 32 ? ((pf_flags & 1) != 0 ? kD3dFmtA8R8G8B8 : kD3dFmtX8R8G8B8)
                             : bits == 16 ? 23 : bits == 8 ? 50 : kD3dFmtA8R8G8B8;
            }
            img.file_format = 4;  // D3DXIFF_DDS
            if ((u32(112) & 0x200U) != 0) img.type = 5;  // DDSCAPS2_CUBEMAP -> D3DRTYPE_CUBETEXTURE
            if (img.depth > 1) img.type = 4;
        }
        return img;
    }
    // D3DXIMAGE_INFO: Width Height Depth MipLevels Format ResourceType ImageFileFormat.
    static void put_image_info(std::uint64_t p, const Image& img) {
        if (p == 0) return;
        const std::uint32_t v[7] = {img.width, img.height, img.depth, img.levels, img.format, img.type, img.file_format};
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), v, sizeof(v));
    }
    // Shared by the two FromFile(InMemory)Ex texture functions: `first` is the index of the width
    // argument, `info` of pSrcInfo, `out` of ppTexture.
    void texture_from_image(CpuState& s, const std::vector<std::uint8_t>& bytes, int first, int info, int out, Kind kind) {
        const Image img = parse_image(bytes);
        const auto mips = static_cast<std::uint32_t>(win64_arg(s, first + 2));
        const auto fmt = static_cast<std::uint32_t>(win64_arg(s, first + 4));
        Desc d = texture_desc(size_or(win64_arg(s, first), img.width), size_or(win64_arg(s, first + 1), img.height), 1,
                              mips == kD3dxDefault ? 0 : (mips == 0xFFFFFFFEU ? img.levels : mips), // D3DX_FROM_FILE
                              fmt == 0 || fmt == kD3dxDefault ? img.format : fmt, static_cast<std::uint32_t>(win64_arg(s, first + 3)),
                              static_cast<std::uint32_t>(win64_arg(s, first + 5)));
        put_image_info(win64_arg(s, info), img);
        win64_return(s, out_object(win64_arg(s, out), kind, d));
    }

    // --- vtables ----------------------------------------------------------------------------
    struct Table {
        const com::Interface* iface = nullptr;
        std::unique_ptr<std::uint64_t[]> slots;  // the vtable the guest reads
        std::unique_ptr<std::uint8_t[]> thunks;  // one byte per method: its callable address
    };
    std::uint64_t vtable_of(Kind k, int variant) const {
        const auto it = tables_.find(static_cast<int>(k) * 2 + variant);
        return it == tables_.end() ? 0 : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(it->second.slots.get()));
    }
    static bool recorded(const std::string& m) {
        static const char* const kPrefixes[] = {"Set", "Begin", "End", "Clear", "Present", "Draw", "Evict", "Light", "Multiply",
                                                "Process", "Update", "Stretch", "Color", "Commit", "On", "Acquire", "Unacquire",
                                                "Poll", "Delete", "Apply", "Capture", "Issue", "Register", "PreLoad", "Generate",
                                                "Unlock", "Free", "Show", "Add", "Send", "Run", "Initialize", "Escape"};
        for (const char* p : kPrefixes) {
            if (m.rfind(p, 0) == 0) return true;
        }
        return false;
    }
    void make_table(Kind k, int variant, const com::Interface& iface, std::map<std::string, Method> impl) {
        Table t;
        t.iface = &iface;
        t.slots = std::make_unique<std::uint64_t[]>(iface.count);
        t.thunks = std::make_unique<std::uint8_t[]>(iface.count);
        for (std::size_t i = 0; i < iface.count; ++i) {
            const std::uint64_t va = static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(t.thunks.get() + i));
            t.slots[i] = va;
            const std::string name = iface.methods[i];
            const std::string full = std::string(iface.name) + "::" + name;
            Method m;
            if (const auto it = impl.find(name); it != impl.end()) {
                m = it->second;
            } else if (name == "QueryInterface") {
                m = [this, &iface](CpuState& s, Obj&, std::uint64_t self) {
                    const std::uint64_t riid = win64_arg(s, 1), out = win64_arg(s, 2);
                    static constexpr unsigned char kUnknown[16] = {0, 0, 0, 0, 0, 0, 0, 0, 0xC0, 0, 0, 0, 0, 0, 0, 0x46};
                    const bool ok = riid != 0 && (iid_is(riid, iface) ||
                                                  std::memcmp(reinterpret_cast<const void*>(static_cast<std::uintptr_t>(riid)), kUnknown, 16) == 0);
                    if (out != 0) crt_detail::put<std::uint64_t>(out, ok ? self : 0);
                    if (ok) add_ref(self);
                    win64_return(s, ok ? 0 : kENoInterface);
                };
            } else if (name == "AddRef") {
                m = [](CpuState& s, Obj& o, std::uint64_t) { win64_return(s, ++o.refs); };
            } else if (name == "Release") {
                m = [](CpuState& s, Obj& o, std::uint64_t) { win64_return(s, o.refs > 0 ? --o.refs : 0); };  // kept: never freed
            } else if (recorded(name)) {
                m = [this, full](CpuState& s, Obj&, std::uint64_t) {
                    first_call(full);
                    win64_return(s, 0);
                };
            } else {
                m = [full](CpuState& s, Obj&, std::uint64_t) {
                    s.hooks->unsupported(&s, crt_detail::get<std::uint64_t>(s.gpr[4]),
                                         (full + " (not in the recording device yet)").c_str());
                };
            }
            rt_.add_callable(va, [this, m](CpuState& s) {
                const std::uint64_t self = s.gpr[1];
                Obj* o = obj(self);
                if (o == nullptr) {
                    s.hooks->unsupported(&s, crt_detail::get<std::uint64_t>(s.gpr[4]), "COM call on an unknown object");
                    return;
                }
                m(s, *o, self);
            });
        }
        tables_[static_cast<int>(k) * 2 + variant] = std::move(t);
    }

    DeviceState* dev(std::uint64_t addr) {
        const auto it = devices_.find(addr);
        return it == devices_.end() ? nullptr : it->second.get();
    }
    // The device an object belongs to (resources keep it as their parent's parent).
    std::uint64_t device_of(Obj& o) {
        if (o.kind == Kind::device) return 0;
        Obj* p = o.parent != 0 ? obj(o.parent) : nullptr;
        if (p != nullptr && p->kind != Kind::device) return p->parent;
        return o.parent;
    }

    void build_vtables();

    Runtime& rt_;
    D3d9Config cfg_;
    mutable std::mutex m_;
    std::map<int, Table> tables_;
    std::map<std::uint64_t, std::unique_ptr<Obj>> objs_;
    std::map<std::uint64_t, std::unique_ptr<DeviceState>> devices_;
    std::vector<std::unique_ptr<std::uint8_t[]>> blocks_;
    std::set<std::string> seen_;
    std::uint64_t frames_ = 0;
    std::uint64_t next_handle_ = 0x7E000000;
    // Effect parameter handles: the name each was looked up by (stable storage: the guest reads
    // D3DXPARAMETER_DESC::Name through a pointer into it).
    std::map<std::uint64_t, std::string> handle_names_;
};

// The implemented methods, interface by interface.
inline void D3d9::build_vtables() {
    using crt_detail::get;
    using crt_detail::put;
    auto ret = [](CpuState& s, std::uint64_t v) { win64_return(s, v); };
    auto get_device = [this](CpuState& s, Obj& o, std::uint64_t) {
        put_ref(*this, win64_arg(s, 1), device_of(o));
        win64_return(s, 0);
    };
    auto mode = [this](std::uint64_t p, int w, int h) {
        if (p == 0) return;
        const std::uint32_t v[4] = {static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h), static_cast<std::uint32_t>(cfg_.refresh), kD3dFmtX8R8G8B8};
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), v, sizeof(v));
    };
    static constexpr int kModes[][2] = {{640, 480}, {800, 600}, {1024, 768}, {1280, 720}, {1280, 1024}, {1366, 768}, {1600, 900}, {1920, 1080}};
    static constexpr std::uint32_t kModeCount = sizeof(kModes) / sizeof(kModes[0]);
    // D3DCAPS9 (304 bytes) exactly as a shader-model-3.0 D3D9 HAL device reports it
    // (GetDeviceCaps(0, D3DDEVTYPE_HAL) on Windows 10.0.19045). The engine's device check
    // (VertexShaderVersion and PixelShaderVersion >= 2.0, MaxStreams >= 16,
    // DevCaps2 & D3DDEVCAPS2_STREAMOFFSET, else rglError "HLSL support is required") passes.
    // Notable fields: DevCaps2 0x51 (STREAMOFFSET | CAN_STRETCHRECT_FROM_TEXTURES |
    // VERTEXELEMENTSCANSHARESTREAMOFFSET), NumberOfAdaptersInGroup 1, 8192 textures, 16 streams.
    static constexpr std::uint32_t kCaps[76] = {
        0x00000001, 0x00000000, 0x00020000, 0xe0020000, 0x00000320, 0x8000000f, 0x00000001, 0x0019ae50,
        0x002fccf2, 0x0f736191, 0x000000ff, 0x00003fff, 0x000027ff, 0x000000ff, 0x00084208, 0x0021ec45,
        0x07030700, 0x07030700, 0x07030700, 0x0000003f, 0x0000003f, 0x0000001f, 0x00002000, 0x00002000,
        0x00000800, 0x00002000, 0x00002000, 0x00000010, 0x501502f9, 0xc7000000, 0xc7000000, 0x47000000,
        0x47000000, 0x00000000, 0x000001ff, 0x00180008, 0x03ffffff, 0x00000008, 0x00000008, 0x0000017b,
        0x00000008, 0x00000006, 0x00000004, 0x00000000, 0x43800000, 0x007fffff, 0x00ffffff, 0x00000010,
        0x000000ff, 0xfffe0300, 0x00000100, 0xffff0300, 0x7f7fffff, 0x00000051, 0x00000000, 0x00000000,
        0x00000000, 0x00000000, 0x00000001, 0x0000037f, 0x00000004, 0x03000300, 0x00000001, 0x00000018,
        0x00000020, 0x00000004, 0x0000001f, 0x00000018, 0x00000020, 0x00000004, 0x00000200, 0x03030300,
        0xffffffff, 0xffffffff, 0x00008000, 0x00008000};
    auto caps = [](std::uint64_t p) {
        if (p == 0) return;
        std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), kCaps, sizeof(kCaps));
    };

    // IDirect3D9
    make_table(Kind::d3d, 0, com::kIDirect3D9, {
        {"GetAdapterCount", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 1); }},
        // GetAdapterIdentifier(adapter, flags, D3DADAPTER_IDENTIFIER9*)
        {"GetAdapterIdentifier", [ret](CpuState& s, Obj&, std::uint64_t) {
             const std::uint64_t p = win64_arg(s, 3);
             if (win64_arg(s, 1) != 0 || p == 0) return ret(s, kD3dErrInvalidCall);
             auto* c = reinterpret_cast<char*>(static_cast<std::uintptr_t>(p));
             std::memset(c, 0, 1104);
             std::strcpy(c, "wse2rec.dll");
             std::strcpy(c + 512, "WSE2 Android recording device");
             std::strcpy(c + 1024, "\\\\.\\DISPLAY1");
             const std::uint32_t vendor = 0x13B5;  // Arm
             std::memcpy(c + 1064, &vendor, 4);
             ret(s, 0);
         }},
        {"GetAdapterModeCount", [ret](CpuState& s, Obj&, std::uint64_t) {
             ret(s, static_cast<std::uint32_t>(win64_arg(s, 2)) == kD3dFmtX8R8G8B8 ? kModeCount : 0);
         }},
        // EnumAdapterModes(adapter, format, mode, D3DDISPLAYMODE*)
        {"EnumAdapterModes", [ret, mode](CpuState& s, Obj&, std::uint64_t) {
             const auto i = static_cast<std::uint32_t>(win64_arg(s, 3));
             if (static_cast<std::uint32_t>(win64_arg(s, 2)) != kD3dFmtX8R8G8B8 || i >= kModeCount) return ret(s, kD3dErrInvalidCall);
             mode(win64_arg(s, 4), kModes[i][0], kModes[i][1]);
             ret(s, 0);
         }},
        {"GetAdapterDisplayMode", [this, ret, mode](CpuState& s, Obj&, std::uint64_t) {
             mode(win64_arg(s, 2), cfg_.width, cfg_.height);
             ret(s, 0);
         }},
        {"CheckDeviceType", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"CheckDeviceFormat", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        // CheckDeviceMultiSampleType(adapter, type, fmt, windowed, msType, pQualityLevels)
        {"CheckDeviceMultiSampleType", [ret](CpuState& s, Obj&, std::uint64_t) {
             const auto ms = static_cast<std::uint32_t>(win64_arg(s, 5));
             if (win64_arg(s, 6) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 6), ms == 0 ? 1 : 0);
             ret(s, ms == 0 ? 0 : kD3dErrNotAvailable);
         }},
        {"CheckDepthStencilMatch", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"CheckDeviceFormatConversion", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"GetDeviceCaps", [ret, caps](CpuState& s, Obj&, std::uint64_t) {
             caps(win64_arg(s, 3));
             ret(s, 0);
         }},
        {"GetAdapterMonitor", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0x10001); }},
        // CreateDevice(adapter, type, hFocusWindow, behavior, D3DPRESENT_PARAMETERS*, ppDevice)
        {"CreateDevice", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const std::uint64_t pp = win64_arg(s, 5), out = win64_arg(s, 6);
             if (pp == 0 || out == 0) return ret(s, kD3dErrInvalidCall);
             const std::uint64_t d = create(Kind::device);
             DeviceState* st = dev(d);
             st->d3d = self;
             add_ref(self);
             st->focus = win64_arg(s, 3);
             st->behavior = static_cast<std::uint32_t>(win64_arg(s, 4));
             st->present.assign(reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pp)),
                                reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pp)) + 64);
             reset_buffers(d, *st);
             put<std::uint64_t>(out, d);
             ret(s, 0);
         }},
    });

    // IDirect3DDevice9
    make_table(Kind::device, 0, com::kIDirect3DDevice9, {
        {"TestCooperativeLevel", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"GetAvailableTextureMem", [this, ret](CpuState& s, Obj&, std::uint64_t) {
             ret(s, static_cast<std::uint32_t>(std::min<std::uint64_t>(cfg_.texture_memory, 0xFFF00000ULL)));
         }},
        {"GetDirect3D", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 1), dev(self)->d3d);
             ret(s, 0);
         }},
        {"GetDeviceCaps", [ret, caps](CpuState& s, Obj&, std::uint64_t) {
             caps(win64_arg(s, 1));
             ret(s, 0);
         }},
        {"GetDisplayMode", [this, ret, mode](CpuState& s, Obj&, std::uint64_t) {
             mode(win64_arg(s, 2), cfg_.width, cfg_.height);
             ret(s, 0);
         }},
        // D3DDEVICE_CREATION_PARAMETERS: AdapterOrdinal DeviceType hFocusWindow BehaviorFlags
        {"GetCreationParameters", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const std::uint64_t p = win64_arg(s, 1);
             if (p == 0) return ret(s, kD3dErrInvalidCall);
             put<std::uint32_t>(p, 0);
             put<std::uint32_t>(p + 4, 1);
             put<std::uint64_t>(p + 8, dev(self)->focus);
             put<std::uint32_t>(p + 16, dev(self)->behavior);
             ret(s, 0);
         }},
        {"Reset", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const std::uint64_t pp = win64_arg(s, 1);
             if (pp == 0) return ret(s, kD3dErrInvalidCall);
             DeviceState* st = dev(self);
             st->present.assign(reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pp)),
                                reinterpret_cast<const std::uint8_t*>(static_cast<std::uintptr_t>(pp)) + 64);
             reset_buffers(self, *st);
             ret(s, 0);
         }},
        {"Present", [this, ret](CpuState& s, Obj&, std::uint64_t) {
             ++frames_;
             ret(s, 0);
         }},
        // GetBackBuffer(iSwapChain, iBackBuffer, Type, ppBackBuffer)
        {"GetBackBuffer", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 4), dev(self)->backbuffer);
             ret(s, 0);
         }},
        {"GetSwapChain", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 2), dev(self)->swapchain);
             ret(s, 0);
         }},
        {"GetNumberOfSwapChains", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 1); }},
        // GetRasterStatus(iSwapChain, D3DRASTER_STATUS{BOOL InVBlank; UINT ScanLine})
        {"GetRasterStatus", [ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 2) != 0) crt_detail::put<std::uint64_t>(win64_arg(s, 2), 0);
             ret(s, 0);
         }},
        {"GetGammaRamp", [ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 2) != 0) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 2))), 0, 3 * 256 * 2);
             ret(s, 0);
         }},
        // CreateTexture(w, h, levels, usage, fmt, pool, ppTexture, pSharedHandle)
        {"CreateTexture", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const Desc d = texture_desc(static_cast<std::uint32_t>(win64_arg(s, 1)), static_cast<std::uint32_t>(win64_arg(s, 2)), 1,
                                         static_cast<std::uint32_t>(win64_arg(s, 3)), static_cast<std::uint32_t>(win64_arg(s, 5)),
                                         static_cast<std::uint32_t>(win64_arg(s, 4)), static_cast<std::uint32_t>(win64_arg(s, 6)));
             ret(s, out_object(win64_arg(s, 7), Kind::texture, d, self));
         }},
        // CreateVolumeTexture(w, h, depth, levels, usage, fmt, pool, pp, sh)
        {"CreateVolumeTexture", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const Desc d = texture_desc(static_cast<std::uint32_t>(win64_arg(s, 1)), static_cast<std::uint32_t>(win64_arg(s, 2)),
                                         static_cast<std::uint32_t>(win64_arg(s, 3)), static_cast<std::uint32_t>(win64_arg(s, 4)),
                                         static_cast<std::uint32_t>(win64_arg(s, 6)), static_cast<std::uint32_t>(win64_arg(s, 5)),
                                         static_cast<std::uint32_t>(win64_arg(s, 7)));
             ret(s, out_object(win64_arg(s, 8), Kind::volume, d, self));
         }},
        // CreateCubeTexture(edge, levels, usage, fmt, pool, pp, sh)
        {"CreateCubeTexture", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const auto e = static_cast<std::uint32_t>(win64_arg(s, 1));
             const Desc d = texture_desc(e, e, 1, static_cast<std::uint32_t>(win64_arg(s, 2)), static_cast<std::uint32_t>(win64_arg(s, 4)),
                                         static_cast<std::uint32_t>(win64_arg(s, 3)), static_cast<std::uint32_t>(win64_arg(s, 5)));
             ret(s, out_object(win64_arg(s, 6), Kind::cube, d, self));
         }},
        // CreateVertexBuffer(length, usage, fvf, pool, pp, sh)
        {"CreateVertexBuffer", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             Desc d;
             d.size = static_cast<std::uint32_t>(win64_arg(s, 1));
             d.usage = static_cast<std::uint32_t>(win64_arg(s, 2));
             d.fvf = static_cast<std::uint32_t>(win64_arg(s, 3));
             d.pool = static_cast<std::uint32_t>(win64_arg(s, 4));
             d.format = 100;  // D3DFMT_VERTEXDATA
             ret(s, out_object(win64_arg(s, 5), Kind::vbuffer, d, self));
         }},
        // CreateIndexBuffer(length, usage, fmt, pool, pp, sh)
        {"CreateIndexBuffer", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             Desc d;
             d.size = static_cast<std::uint32_t>(win64_arg(s, 1));
             d.usage = static_cast<std::uint32_t>(win64_arg(s, 2));
             d.format = static_cast<std::uint32_t>(win64_arg(s, 3));
             d.pool = static_cast<std::uint32_t>(win64_arg(s, 4));
             ret(s, out_object(win64_arg(s, 5), Kind::ibuffer, d, self));
         }},
        // CreateRenderTarget(w, h, fmt, ms, msq, lockable, pp, sh) / CreateDepthStencilSurface(w, h, fmt, ms, msq, discard, pp, sh)
        {"CreateRenderTarget", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, plain_surface(s, self, 7, 1)); }},
        {"CreateDepthStencilSurface", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, plain_surface(s, self, 7, 2)); }},
        // CreateOffscreenPlainSurface(w, h, fmt, pool, pp, sh)
        {"CreateOffscreenPlainSurface", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, plain_surface(s, self, 5, 0)); }},
        {"GetRenderTargetData", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"GetFrontBufferData", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"GetRenderTarget", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 2), win64_arg(s, 1) == 0 ? dev(self)->backbuffer : 0);
             ret(s, win64_arg(s, 1) == 0 ? 0 : kD3dErrNotAvailable);
         }},
        {"GetDepthStencilSurface", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 1), dev(self)->depth);
             ret(s, dev(self)->depth != 0 ? 0 : kD3dErrNotAvailable);
         }},
        {"SetRenderState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             dev(self)->render[static_cast<std::uint32_t>(win64_arg(s, 1))] = static_cast<std::uint32_t>(win64_arg(s, 2));
             ret(s, 0);
         }},
        {"GetRenderState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
             crt_detail::put<std::uint32_t>(win64_arg(s, 2), dev(self)->render[static_cast<std::uint32_t>(win64_arg(s, 1))]);
             ret(s, 0);
         }},
        {"SetSamplerState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             dev(self)->sampler[static_cast<std::uint32_t>(win64_arg(s, 1)) * 64 + static_cast<std::uint32_t>(win64_arg(s, 2))] =
                 static_cast<std::uint32_t>(win64_arg(s, 3));
             ret(s, 0);
         }},
        {"GetSamplerState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 3) == 0) return ret(s, kD3dErrInvalidCall);
             crt_detail::put<std::uint32_t>(win64_arg(s, 3), dev(self)->sampler[static_cast<std::uint32_t>(win64_arg(s, 1)) * 64 +
                                                                                 static_cast<std::uint32_t>(win64_arg(s, 2))]);
             ret(s, 0);
         }},
        {"SetTextureStageState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             dev(self)->stage[static_cast<std::uint32_t>(win64_arg(s, 1)) * 64 + static_cast<std::uint32_t>(win64_arg(s, 2))] =
                 static_cast<std::uint32_t>(win64_arg(s, 3));
             ret(s, 0);
         }},
        {"GetTextureStageState", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 3) == 0) return ret(s, kD3dErrInvalidCall);
             crt_detail::put<std::uint32_t>(win64_arg(s, 3), dev(self)->stage[static_cast<std::uint32_t>(win64_arg(s, 1)) * 64 +
                                                                               static_cast<std::uint32_t>(win64_arg(s, 2))]);
             ret(s, 0);
         }},
        {"SetTransform", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
             std::array<float, 16> m{};
             std::memcpy(m.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win64_arg(s, 2))), 64);
             dev(self)->transforms[static_cast<std::uint32_t>(win64_arg(s, 1))] = m;
             ret(s, 0);
         }},
        {"GetTransform", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
             auto& t = dev(self)->transforms;
             const auto it = t.find(static_cast<std::uint32_t>(win64_arg(s, 1)));
             std::array<float, 16> m{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1};
             if (it != t.end()) m = it->second;
             std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 2))), m.data(), 64);
             ret(s, 0);
         }},
        // D3DVIEWPORT9: X Y Width Height MinZ MaxZ
        {"SetViewport", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 1) == 0) return ret(s, kD3dErrInvalidCall);
             std::memcpy(dev(self)->viewport.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), 24);
             ret(s, 0);
         }},
        {"GetViewport", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 1) == 0) return ret(s, kD3dErrInvalidCall);
             std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), dev(self)->viewport.data(), 24);
             ret(s, 0);
         }},
        {"SetScissorRect", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 1) != 0) std::memcpy(dev(self)->scissor.data(), reinterpret_cast<const void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), 16);
             ret(s, 0);
         }},
        {"GetScissorRect", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 1) == 0) return ret(s, kD3dErrInvalidCall);
             std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), dev(self)->scissor.data(), 16);
             ret(s, 0);
         }},
        {"SetTexture", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             dev(self)->textures[static_cast<std::uint32_t>(win64_arg(s, 1))] = win64_arg(s, 2);
             ret(s, 0);
         }},
        {"GetTexture", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             put_ref(*this, win64_arg(s, 2), dev(self)->textures[static_cast<std::uint32_t>(win64_arg(s, 1))]);
             ret(s, 0);
         }},
        {"SetVertexDeclaration", [this, ret](CpuState& s, Obj&, std::uint64_t self) { dev(self)->decl = win64_arg(s, 1); ret(s, 0); }},
        {"GetVertexDeclaration", [this, ret](CpuState& s, Obj&, std::uint64_t self) { put_ref(*this, win64_arg(s, 1), dev(self)->decl); ret(s, 0); }},
        {"SetFVF", [this, ret](CpuState& s, Obj&, std::uint64_t self) { dev(self)->fvf = win64_arg(s, 1); ret(s, 0); }},
        {"GetFVF", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), static_cast<std::uint32_t>(dev(self)->fvf));
             ret(s, 0);
         }},
        {"SetVertexShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { dev(self)->vs = win64_arg(s, 1); ret(s, 0); }},
        {"GetVertexShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { put_ref(*this, win64_arg(s, 1), dev(self)->vs); ret(s, 0); }},
        {"SetPixelShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { dev(self)->ps = win64_arg(s, 1); ret(s, 0); }},
        {"GetPixelShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { put_ref(*this, win64_arg(s, 1), dev(self)->ps); ret(s, 0); }},
        {"SetIndices", [this, ret](CpuState& s, Obj&, std::uint64_t self) { dev(self)->indices = win64_arg(s, 1); ret(s, 0); }},
        {"GetIndices", [this, ret](CpuState& s, Obj&, std::uint64_t self) { put_ref(*this, win64_arg(s, 1), dev(self)->indices); ret(s, 0); }},
        // SetStreamSource(stream, vb, offset, stride) / GetStreamSource(stream, ppVB, pOffset, pStride)
        {"SetStreamSource", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             dev(self)->streams[static_cast<std::uint32_t>(win64_arg(s, 1))] = {win64_arg(s, 2), win64_arg(s, 3) & 0xFFFFFFFF, win64_arg(s, 4) & 0xFFFFFFFF};
             ret(s, 0);
         }},
        {"GetStreamSource", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             const auto st = dev(self)->streams[static_cast<std::uint32_t>(win64_arg(s, 1))];
             put_ref(*this, win64_arg(s, 2), st[0]);
             if (win64_arg(s, 3) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 3), static_cast<std::uint32_t>(st[1]));
             if (win64_arg(s, 4) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 4), static_cast<std::uint32_t>(st[2]));
             ret(s, 0);
         }},
        {"CreateVertexDeclaration", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, out_object(win64_arg(s, 2), Kind::decl, {}, self)); }},
        {"CreateVertexShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, out_object(win64_arg(s, 2), Kind::vshader, {}, self)); }},
        {"CreatePixelShader", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, out_object(win64_arg(s, 2), Kind::pshader, {}, self)); }},
        {"CreateStateBlock", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, out_object(win64_arg(s, 2), Kind::stateblock, {}, self)); }},
        {"EndStateBlock", [this, ret](CpuState& s, Obj&, std::uint64_t self) { ret(s, out_object(win64_arg(s, 1), Kind::stateblock, {}, self)); }},
        // CreateQuery(type, ppQuery): a NULL ppQuery asks whether the type is supported.
        {"CreateQuery", [this, ret](CpuState& s, Obj&, std::uint64_t self) {
             if (win64_arg(s, 2) == 0) return ret(s, 0);
             Desc d;
             d.format = static_cast<std::uint32_t>(win64_arg(s, 1));
             ret(s, out_object(win64_arg(s, 2), Kind::query, d, self));
         }},
        {"ValidateDevice", [ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), 1);
             ret(s, 0);
         }},
        {"GetSoftwareVertexProcessing", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
    });

    // IDirect3DSwapChain9
    make_table(Kind::swapchain, 0, com::kIDirect3DSwapChain9, {
        {"Present", [this, ret](CpuState& s, Obj&, std::uint64_t) { ++frames_; ret(s, 0); }},
        {"GetBackBuffer", [this, ret](CpuState& s, Obj& o, std::uint64_t) { put_ref(*this, win64_arg(s, 3), dev(o.parent)->backbuffer); ret(s, 0); }},
        {"GetDisplayMode", [this, ret, mode](CpuState& s, Obj&, std::uint64_t) { mode(win64_arg(s, 1), cfg_.width, cfg_.height); ret(s, 0); }},
        {"GetPresentParameters", [this, ret](CpuState& s, Obj& o, std::uint64_t) {
             if (win64_arg(s, 1) == 0) return ret(s, kD3dErrInvalidCall);
             const auto& p = dev(o.parent)->present;
             std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 1))), p.data(), p.size());
             ret(s, 0);
         }},
        {"GetDevice", get_device},
    });

    // Surfaces and textures
    make_table(Kind::surface, 0, com::kIDirect3DSurface9, {
        {"GetDevice", get_device},
        {"GetType", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 1); }},  // D3DRTYPE_SURFACE
        {"GetDesc", [ret](CpuState& s, Obj& o, std::uint64_t) { put_surface_desc(win64_arg(s, 1), o.desc, 1); ret(s, 0); }},
        // LockRect(D3DLOCKED_RECT*, RECT*, flags)
        {"LockRect", [this, ret](CpuState& s, Obj& o, std::uint64_t) {
             ret(s, lock_rect(win64_arg(s, 1), surface_memory(o), o.desc.format, o.desc.width, win64_arg(s, 2)));
         }},
        {"GetContainer", [this, ret](CpuState& s, Obj& o, std::uint64_t) {
             if (o.parent == 0 || obj(o.parent)->kind == Kind::device) return ret(s, kENoInterface);
             put_ref(*this, win64_arg(s, 2), o.parent);
             ret(s, 0);
         }},
    });
    auto tex_common = [&](Kind k) {
        std::map<std::string, Method> m = {
            {"GetDevice", get_device},
            {"GetType", [ret, k](CpuState& s, Obj&, std::uint64_t) { ret(s, k == Kind::cube ? 5 : k == Kind::volume ? 4 : 3); }},
            {"GetLevelCount", [ret](CpuState& s, Obj& o, std::uint64_t) { ret(s, o.desc.levels); }},
            {"GetLOD", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
            {"GetAutoGenFilterType", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 2); }},  // D3DTEXF_LINEAR
            {"GetPriority", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        };
        return m;
    };
    {
        auto m = tex_common(Kind::texture);
        m["GetLevelDesc"] = [ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (l >= o.desc.levels) return ret(s, kD3dErrInvalidCall);
            Desc d = o.desc;
            d.width = level_dim(o.desc.width, l);
            d.height = level_dim(o.desc.height, l);
            put_surface_desc(win64_arg(s, 2), d, 1);
            ret(s, 0);
        };
        m["GetSurfaceLevel"] = [this, ret](CpuState& s, Obj& o, std::uint64_t self) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (l >= o.desc.levels || win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
            put_ref(*this, win64_arg(s, 2), child_surface(self, l));
            ret(s, 0);
        };
        // LockRect(level, D3DLOCKED_RECT*, RECT*, flags)
        m["LockRect"] = [this, ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (l >= o.desc.levels) return ret(s, kD3dErrInvalidCall);
            ret(s, lock_rect(win64_arg(s, 2), bytes(o, l), o.desc.format, level_dim(o.desc.width, l), win64_arg(s, 3)));
        };
        make_table(Kind::texture, 0, com::kIDirect3DTexture9, m);
    }
    {
        auto m = tex_common(Kind::cube);
        m["GetLevelDesc"] = [ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (l >= o.desc.levels) return ret(s, kD3dErrInvalidCall);
            Desc d = o.desc;
            d.width = d.height = level_dim(o.desc.width, l);
            put_surface_desc(win64_arg(s, 2), d, 1);
            ret(s, 0);
        };
        // GetCubeMapSurface(face, level, pp) / LockRect(face, level, locked, rect, flags)
        m["GetCubeMapSurface"] = [this, ret](CpuState& s, Obj& o, std::uint64_t self) {
            const auto f = static_cast<std::uint32_t>(win64_arg(s, 1)), l = static_cast<std::uint32_t>(win64_arg(s, 2));
            if (f >= 6 || l >= o.desc.levels || win64_arg(s, 3) == 0) return ret(s, kD3dErrInvalidCall);
            put_ref(*this, win64_arg(s, 3), child_surface(self, f * o.desc.levels + l));
            ret(s, 0);
        };
        m["LockRect"] = [this, ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto f = static_cast<std::uint32_t>(win64_arg(s, 1)), l = static_cast<std::uint32_t>(win64_arg(s, 2));
            if (f >= 6 || l >= o.desc.levels) return ret(s, kD3dErrInvalidCall);
            ret(s, lock_rect(win64_arg(s, 3), bytes(o, f * o.desc.levels + l), o.desc.format, level_dim(o.desc.width, l), win64_arg(s, 4)));
        };
        make_table(Kind::cube, 0, com::kIDirect3DCubeTexture9, m);
    }
    {
        auto m = tex_common(Kind::volume);
        // D3DVOLUME_DESC: Format Type Usage Pool Width Height Depth
        m["GetLevelDesc"] = [ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            if (l >= o.desc.levels || win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
            const std::uint32_t v[7] = {o.desc.format, 2, o.desc.usage, o.desc.pool, level_dim(o.desc.width, l),
                                        level_dim(o.desc.height, l), level_dim(o.desc.depth, l)};
            std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 2))), v, sizeof(v));
            ret(s, 0);
        };
        // LockBox(level, D3DLOCKED_BOX{RowPitch, SlicePitch, pBits}, box, flags)
        m["LockBox"] = [this, ret](CpuState& s, Obj& o, std::uint64_t) {
            const auto l = static_cast<std::uint32_t>(win64_arg(s, 1));
            const std::uint64_t p = win64_arg(s, 2);
            if (l >= o.desc.levels || p == 0) return ret(s, kD3dErrInvalidCall);
            const std::uint32_t row = pitch_of(o.desc.format, level_dim(o.desc.width, l));
            crt_detail::put<std::int32_t>(p, static_cast<std::int32_t>(row));
            crt_detail::put<std::int32_t>(p + 4, static_cast<std::int32_t>(row * rows_of(o.desc.format, level_dim(o.desc.height, l))));
            crt_detail::put<std::uint64_t>(p + 8, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(bytes(o, l)->data())));
            ret(s, 0);
        };
        make_table(Kind::volume, 0, com::kIDirect3DVolumeTexture9, m);
    }

    // Buffers: Lock(offset, size, void**, flags), GetDesc
    auto buffer = [&](Kind k, const com::Interface& iface) {
        make_table(k, 0, iface, {
            {"GetDevice", get_device},
            {"GetType", [ret, k](CpuState& s, Obj&, std::uint64_t) { ret(s, k == Kind::vbuffer ? 6 : 7); }},
            {"Lock", [this, ret](CpuState& s, Obj& o, std::uint64_t) {
                 const auto off = static_cast<std::uint32_t>(win64_arg(s, 1));
                 std::vector<std::uint8_t>* mem = bytes(o, 0);
                 if (win64_arg(s, 3) == 0 || mem == nullptr || (off >= mem->size() && !mem->empty())) return ret(s, kD3dErrInvalidCall);
                 crt_detail::put<std::uint64_t>(win64_arg(s, 3), static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mem->data() + off)));
                 ret(s, 0);
             }},
            {"GetDesc", [ret, k](CpuState& s, Obj& o, std::uint64_t) {
                 const std::uint64_t p = win64_arg(s, 1);
                 if (p == 0) return ret(s, kD3dErrInvalidCall);
                 const std::uint32_t v[6] = {o.desc.format, k == Kind::vbuffer ? 6U : 7U, o.desc.usage, o.desc.pool, o.desc.size, o.desc.fvf};
                 std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), v, k == Kind::vbuffer ? 24 : 20);
                 ret(s, 0);
             }},
        });
    };
    buffer(Kind::vbuffer, com::kIDirect3DVertexBuffer9);
    buffer(Kind::ibuffer, com::kIDirect3DIndexBuffer9);
    make_table(Kind::decl, 0, com::kIDirect3DVertexDeclaration9, {{"GetDevice", get_device}});
    make_table(Kind::vshader, 0, com::kIDirect3DVertexShader9, {{"GetDevice", get_device}});
    make_table(Kind::pshader, 0, com::kIDirect3DPixelShader9, {{"GetDevice", get_device}});
    make_table(Kind::stateblock, 0, com::kIDirect3DStateBlock9, {{"GetDevice", get_device}});
    make_table(Kind::query, 0, com::kIDirect3DQuery9, {
        {"GetDevice", get_device},
        {"GetType", [ret](CpuState& s, Obj& o, std::uint64_t) { ret(s, o.desc.format); }},
        {"GetDataSize", [ret](CpuState& s, Obj& o, std::uint64_t) { ret(s, o.desc.format == 8 ? 4 : 8); }},  // EVENT: BOOL
        // GetData(pData, size, flags): finished at once; an event query reads TRUE, others zeros.
        {"GetData", [ret](CpuState& s, Obj& o, std::uint64_t) {
             const std::uint64_t p = win64_arg(s, 1);
             const auto n = static_cast<std::uint32_t>(win64_arg(s, 2));
             if (p != 0 && n != 0) {
                 std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(p)), 0, n);
                 if (o.desc.format == 8 && n >= 4) crt_detail::put<std::uint32_t>(p, 1);
             }
             ret(s, 0);
         }},
    });

    // ID3DXEffect: parameters and techniques are opaque handles; setting succeeds.
    auto handle = [this, ret](CpuState& s, Obj&, std::uint64_t) {
        const std::lock_guard<std::mutex> lock(m_);
        next_handle_ += 0x10;
        ret(s, next_handle_);
    };
    // GetParameterByName / BySemantic(hParent, name): a new handle that remembers the name.
    auto named_handle = [this, ret](CpuState& s, Obj&, std::uint64_t) {
        const std::uint64_t name = win64_arg(s, 2);
        const std::lock_guard<std::mutex> lock(m_);
        next_handle_ += 0x10;
        handle_names_[next_handle_] = name != 0 ? crt_detail::guest_string(name) : std::string();
        ret(s, next_handle_);
    };
    make_table(Kind::effect, 0, com::kID3DXEffect, {
        {"GetDevice", get_device},
        {"GetParameterByName", named_handle},
        {"GetParameterBySemantic", named_handle},
        // GetParameterDesc(handle, D3DXPARAMETER_DESC*): the stub knows no parameter's class, type
        // or size (the .fxo is not read before R5), so the 56-byte desc is zero apart from Name;
        // Elements 0 makes array-sized engine settings (lights per shader) 0. A handle this
        // device never returned is D3DERR_INVALIDCALL.
        {"GetParameterDesc", [this, ret](CpuState& s, Obj&, std::uint64_t) {
             const std::uint64_t h = win64_arg(s, 1), desc = win64_arg(s, 2);
             const std::lock_guard<std::mutex> lock(m_);
             const auto it = handle_names_.find(h);
             if (desc == 0 || it == handle_names_.end()) return ret(s, kD3dErrInvalidCall);
             std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(desc)), 0, 56);
             crt_detail::put<std::uint64_t>(desc, static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(it->second.c_str())));
             ret(s, 0);
         }},
        {"GetParameter", handle},
        {"GetTechniqueByName", handle},
        {"GetTechnique", handle},
        {"GetPass", handle},
        {"GetPassByName", handle},
        {"GetCurrentTechnique", handle},
        {"GetPool", [this, ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 1) != 0) crt_detail::put<std::uint64_t>(win64_arg(s, 1), create(Kind::effect_pool));
             ret(s, 0);
         }},
        {"ValidateTechnique", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        {"FindNextValidTechnique", [ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 2) != 0) crt_detail::put<std::uint64_t>(win64_arg(s, 2), win64_arg(s, 1) != 0 ? 0 : 0x7E000010ULL);
             ret(s, 0);
         }},
        // Begin(UINT* pPasses, flags): one pass.
        {"Begin", [ret](CpuState& s, Obj&, std::uint64_t) {
             if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), 1);
             ret(s, 0);
         }},
        {"IsParameterUsed", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 1); }},
    });
    make_table(Kind::effect_pool, 0, com::kID3DXEffectPool, {});
    make_table(Kind::buffer, 0, com::kID3DXBuffer, {
        {"GetBufferPointer", [this, ret](CpuState& s, Obj& o, std::uint64_t) {
             std::vector<std::uint8_t>* mem = bytes(o, 0);
             ret(s, mem == nullptr ? 0 : static_cast<std::uint64_t>(reinterpret_cast<std::uintptr_t>(mem->data())));
         }},
        {"GetBufferSize", [ret](CpuState& s, Obj& o, std::uint64_t) { ret(s, o.sizes.empty() ? 0 : o.sizes[0]); }},
    });

    // DirectInput 8 (A and W): devices with nothing pressed.
    for (int v = 0; v < 2; ++v) {
        make_table(Kind::dinput, v, v == 0 ? com::kIDirectInput8A : com::kIDirectInput8W, {
            // CreateDevice(rguid, ppDevice, pUnkOuter)
            {"CreateDevice", [this, ret, v](CpuState& s, Obj&, std::uint64_t self) {
                 if (win64_arg(s, 2) == 0) return ret(s, kD3dErrInvalidCall);
                 crt_detail::put<std::uint64_t>(win64_arg(s, 2), create(Kind::didevice, v, {}, self));
                 ret(s, 0);
             }},
            {"EnumDevices", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},  // nothing beyond the system keyboard and mouse
            {"GetDeviceStatus", [ret](CpuState& s, Obj&, std::uint64_t) { ret(s, 0); }},
        });
        make_table(Kind::didevice, v, v == 0 ? com::kIDirectInputDevice8A : com::kIDirectInputDevice8W, {
            // GetDeviceState(cbData, lpvData): nothing pressed.
            {"GetDeviceState", [ret](CpuState& s, Obj&, std::uint64_t) {
                 if (win64_arg(s, 2) != 0) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 2))), 0, static_cast<std::uint32_t>(win64_arg(s, 1)));
                 ret(s, 0);
             }},
            // GetDeviceData(cbObjectData, rgdod, pdwInOut, dwFlags): no events.
            {"GetDeviceData", [ret](CpuState& s, Obj&, std::uint64_t) {
                 if (win64_arg(s, 3) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 3), 0);
                 ret(s, 0);
             }},
            // GetCapabilities(DIDEVCAPS*): attached.
            {"GetCapabilities", [ret](CpuState& s, Obj&, std::uint64_t) {
                 const std::uint64_t p = win64_arg(s, 1);
                 if (p == 0) return ret(s, kD3dErrInvalidCall);
                 crt_detail::put<std::uint32_t>(p + 4, 1);  // dwFlags: DIDC_ATTACHED
                 ret(s, 0);
             }},
        });
    }
}

}  // namespace wse::recomp::shim
