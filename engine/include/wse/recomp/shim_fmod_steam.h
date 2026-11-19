// The two delay-loaded DLLs of v1.1.5.6 (Phase 5 R4, `docs/RECOMP_PLAN.md` "R4 design": "A silent
// FMOD-API stub (valid opaque handles, every call succeeds, nothing plays) until R6 translates
// fmodex64" and "steam_api64 behaves as absent"):
//
//   fmodex64.dll     the 54 FMOD Ex functions the exe imports: FMOD_System_Create and member
//                    functions of FMOD::System, Sound, Channel and Geometry (C++ names)
//   steam_api64.dll  the 10 SteamAPI_* / SteamInternal_* functions
//
// FMOD objects are opaque handles. Creating a sound, stream, geometry or channel hands one out;
// getters answer as for a machine with one silent output (one driver, FMOD Ex 4.44.64, sounds open
// and ready, 1,000 units long, channels never playing); a channel keeps its sound, user data, mode
// and volume so they read back. Every call returns FMOD_OK. Steam behaves as on a PC where Steam is
// not running: SteamAPI_Init returns false, the interface lookups return NULL, the rest do nothing.
// The exe's own delay-load helper loads both through `LoadLibraryExA` / `GetProcAddress`, which
// `shim_modules.h` answers from these shims. Header-only.

#pragma once

#include <algorithm>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <map>
#include <mutex>
#include <string>

#include "wse/recomp/cpu_state.h"
#include "wse/recomp/runtime.h"
#include "wse/recomp/shim_crt.h"

namespace wse::recomp::shim {

inline constexpr std::uint32_t kFmodVersion = 0x00044464U;  // FMOD Ex 4.44.64
inline constexpr std::uint32_t kFmodSpeakerModeStereo = 2;

class FmodSteam {
public:
    explicit FmodSteam(Runtime& rt) : rt_(rt) {}
    FmodSteam(const FmodSteam&) = delete;
    FmodSteam& operator=(const FmodSteam&) = delete;

    [[nodiscard]] std::size_t sounds_created() const noexcept { return sounds_; }

    std::size_t install() {
        std::size_t n = 0;
        using crt_detail::put;
        auto fmod = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("fmodex64.dll", name, shim); };
        auto ok = [](CpuState& s) { win64_return(s, 0); };
        auto out_handle = [this](std::uint64_t out) {
            if (out == 0) return std::uint64_t{0};
            const std::uint64_t h = next();
            crt_detail::put<std::uint64_t>(out, h);
            return h;
        };

        // FMOD_System_Create(FMOD::System**)
        fmod("FMOD_System_Create", [out_handle](CpuState& s) {
            out_handle(win64_arg(s, 0));
            win64_return(s, 0);
        });
        // System::createSound / createStream(this, name, mode, exinfo, Sound**)
        for (const char* name : {"?createSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z",
                                 "?createStream@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEBDIPEAUFMOD_CREATESOUNDEXINFO@@PEAPEAVSound@2@@Z"}) {
            fmod(name, [this, out_handle](CpuState& s) {
                out_handle(win64_arg(s, 4));
                ++sounds_;
                win64_return(s, 0);
            });
        }
        // System::createGeometry(this, maxpolygons, maxvertices, Geometry**)
        fmod("?createGeometry@System@FMOD@@QEAA?AW4FMOD_RESULT@@HHPEAPEAVGeometry@2@@Z", [out_handle](CpuState& s) {
            out_handle(win64_arg(s, 3));
            win64_return(s, 0);
        });
        // System::playSound(this, channelid, Sound*, paused, Channel**)
        fmod("?playSound@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_CHANNELINDEX@@PEAVSound@2@_NPEAPEAVChannel@2@@Z",
             [this, out_handle](CpuState& s) {
                 const std::uint64_t ch = out_handle(win64_arg(s, 4));
                 const std::lock_guard<std::mutex> lock(m_);
                 Channel& c = channels_[ch];
                 c.sound = win64_arg(s, 2);
                 c.index = static_cast<int>(channels_.size() - 1);
                 win64_return(s, 0);
             });
        // System::getChannel(this, channelid, Channel**)
        fmod("?getChannel@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAPEAVChannel@2@@Z", [this, out_handle](CpuState& s) {
            const std::uint64_t ch = out_handle(win64_arg(s, 2));
            const std::lock_guard<std::mutex> lock(m_);
            channels_[ch].index = static_cast<int>(win64_arg(s, 1));
            win64_return(s, 0);
        });
        fmod("?getVersion@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAI@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), kFmodVersion);
            win64_return(s, 0);
        });
        fmod("?getNumDrivers@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::int32_t>(win64_arg(s, 1), 1);
            win64_return(s, 0);
        });
        // System::getDriverInfo(this, id, char* name, namelen, FMOD_GUID*)
        fmod("?getDriverInfo@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEADHPEAUFMOD_GUID@@@Z", [](CpuState& s) {
            static constexpr char kName[] = "WSE2 silent output";
            const std::uint64_t name = win64_arg(s, 2);
            const auto len = static_cast<std::int32_t>(win64_arg(s, 3));
            if (name != 0 && len > 0) {
                const std::size_t k = std::min<std::size_t>(static_cast<std::size_t>(len) - 1, sizeof(kName) - 1);
                std::memcpy(reinterpret_cast<void*>(static_cast<std::uintptr_t>(name)), kName, k);
                crt_detail::put<char>(name + k, 0);
            }
            if (win64_arg(s, 4) != 0) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, 4))), 0, 16);
            win64_return(s, 0);
        });
        // System::getDriverCaps(this, id, unsigned* caps, int* controlpaneloutputrate, FMOD_SPEAKERMODE*)
        fmod("?getDriverCaps@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEAIPEAHPEAW4FMOD_SPEAKERMODE@@@Z", [](CpuState& s) {
            if (win64_arg(s, 2) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 2), 0);
            if (win64_arg(s, 3) != 0) crt_detail::put<std::int32_t>(win64_arg(s, 3), 48000);
            if (win64_arg(s, 4) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 4), kFmodSpeakerModeStereo);
            win64_return(s, 0);
        });
        fmod("?getChannelsPlaying@System@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::int32_t>(win64_arg(s, 1), 0);
            win64_return(s, 0);
        });
        // Sound
        // Sound::getOpenState(this, FMOD_OPENSTATE*, unsigned* percentbuffered, bool* starving, bool* diskbusy)
        fmod("?getOpenState@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAW4FMOD_OPENSTATE@@PEAIPEA_N2@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), 0);  // FMOD_OPENSTATE_READY
            if (win64_arg(s, 2) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 2), 100);
            if (win64_arg(s, 3) != 0) crt_detail::put<std::uint8_t>(win64_arg(s, 3), 0);
            if (win64_arg(s, 4) != 0) crt_detail::put<std::uint8_t>(win64_arg(s, 4), 0);
            win64_return(s, 0);
        });
        fmod("?getLength@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), 1000);
            win64_return(s, 0);
        });
        // Sound::getDefaults(this, float* frequency, float* volume, float* pan, int* priority)
        fmod("?getDefaults@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@PEAM00PEAH@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<float>(win64_arg(s, 1), 44100.0F);
            if (win64_arg(s, 2) != 0) crt_detail::put<float>(win64_arg(s, 2), 1.0F);
            if (win64_arg(s, 3) != 0) crt_detail::put<float>(win64_arg(s, 3), 0.0F);
            if (win64_arg(s, 4) != 0) crt_detail::put<std::int32_t>(win64_arg(s, 4), 128);
            win64_return(s, 0);
        });
        // Channel: what was set reads back.
        fmod("?isPlaying@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEA_N@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint8_t>(win64_arg(s, 1), 0);
            win64_return(s, 0);
        });
        fmod("?getPosition@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAII@Z", [](CpuState& s) {
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), 0);
            win64_return(s, 0);
        });
        fmod("?get3DAttributes@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAUFMOD_VECTOR@@0@Z", [](CpuState& s) {
            for (int i = 1; i <= 2; ++i) {
                if (win64_arg(s, i) != 0) std::memset(reinterpret_cast<void*>(static_cast<std::uintptr_t>(win64_arg(s, i))), 0, 12);
            }
            win64_return(s, 0);
        });
        fmod("?setUserData@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAX@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            channels_[win64_arg(s, 0)].user = win64_arg(s, 1);
            win64_return(s, 0);
        });
        fmod("?getUserData@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAX@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint64_t>(win64_arg(s, 1), channels_[win64_arg(s, 0)].user);
            win64_return(s, 0);
        });
        fmod("?getCurrentSound@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAPEAVSound@2@@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint64_t>(win64_arg(s, 1), channels_[win64_arg(s, 0)].sound);
            win64_return(s, 0);
        });
        fmod("?getIndex@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAH@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (win64_arg(s, 1) != 0) crt_detail::put<std::int32_t>(win64_arg(s, 1), channels_[win64_arg(s, 0)].index);
            win64_return(s, 0);
        });
        fmod("?setMode@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@I@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            channels_[win64_arg(s, 0)].mode = static_cast<std::uint32_t>(win64_arg(s, 1));
            win64_return(s, 0);
        });
        fmod("?getMode@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAI@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (win64_arg(s, 1) != 0) crt_detail::put<std::uint32_t>(win64_arg(s, 1), channels_[win64_arg(s, 0)].mode);
            win64_return(s, 0);
        });
        fmod("?setVolume@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            channels_[win64_arg(s, 0)].volume = win64_arg_f32(s, 1);
            win64_return(s, 0);
        });
        fmod("?getVolume@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEAM@Z", [this](CpuState& s) {
            const std::lock_guard<std::mutex> lock(m_);
            if (win64_arg(s, 1) != 0) crt_detail::put<float>(win64_arg(s, 1), channels_[win64_arg(s, 0)].volume);
            win64_return(s, 0);
        });
        // The rest only change state nothing reads back here: FMOD_OK.
        for (const char* name : {
                 "?setDefaults@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@MMMH@Z",
                 "?addPolygon@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@MM_NHPEBUFMOD_VECTOR@@PEAH@Z",
                 "?init@System@FMOD@@QEAA?AW4FMOD_RESULT@@HIPEAX@Z",
                 "?setOutput@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_OUTPUTTYPE@@@Z",
                 "?setSoftwareFormat@System@FMOD@@QEAA?AW4FMOD_RESULT@@HW4FMOD_SOUND_FORMAT@@HHW4FMOD_DSP_RESAMPLER@@@Z",
                 "?setDSPBufferSize@System@FMOD@@QEAA?AW4FMOD_RESULT@@IH@Z",
                 "?setSpeakerMode@System@FMOD@@QEAA?AW4FMOD_RESULT@@W4FMOD_SPEAKERMODE@@@Z",
                 "?close@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?setGeometrySettings@System@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z",
                 "?setPosition@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@PEBUFMOD_VECTOR@@@Z",
                 "?setRotation@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@PEBUFMOD_VECTOR@@0@Z",
                 "?setScale@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@PEBUFMOD_VECTOR@@@Z",
                 "?setActive@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z",
                 "?update@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?release@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?setCallback@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@P6A?AW43@PEAUFMOD_CHANNEL@@W4FMOD_CHANNEL_CALLBACKTYPE@@PEAX2@Z@Z",
                 "?setPriority@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z",
                 "?set3DMinMaxDistance@Sound@FMOD@@QEAA?AW4FMOD_RESULT@@MM@Z",
                 "?release@Geometry@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?set3DSettings@System@FMOD@@QEAA?AW4FMOD_RESULT@@MMM@Z",
                 "?release@System@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?stop@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@XZ",
                 "?set3DListenerAttributes@System@FMOD@@QEAA?AW4FMOD_RESULT@@HPEBUFMOD_VECTOR@@000@Z",
                 "?set3DAttributes@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@PEBUFMOD_VECTOR@@0@Z",
                 "?setPosition@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@II@Z",
                 "?setFrequency@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@M@Z",
                 "?setPaused@Channel@FMOD@@QEAA?AW4FMOD_RESULT@@_N@Z",
                 "?setHardwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z",
                 "?setSoftwareChannels@System@FMOD@@QEAA?AW4FMOD_RESULT@@H@Z"}) {
            fmod(name, ok);
        }

        // Steam, as on a PC where Steam is not running.
        auto steam = [&](const char* name, Runtime::ImportShim shim) { n += rt_.set_import("steam_api64.dll", name, shim); };
        steam("SteamAPI_Init", [](CpuState& s) { win64_return(s, 0); });
        steam("SteamAPI_GetHSteamUser", [](CpuState& s) { win64_return(s, 0); });
        steam("SteamInternal_FindOrCreateUserInterface", [](CpuState& s) { win64_return(s, 0); });
        // SteamInternal_ContextInit(CSteamAPIContextInitData*): the context inside it, left empty
        // (its interface pointers stay NULL: Steam never initialized).
        steam("SteamInternal_ContextInit", [](CpuState& s) {
            const std::uint64_t d = win64_arg(s, 0);
            win64_return(s, d != 0 ? d + 16 : 0);
        });
        for (const char* name : {"SteamAPI_RegisterCallResult", "SteamAPI_UnregisterCallResult", "SteamAPI_RunCallbacks",
                                 "SteamAPI_Shutdown", "SteamAPI_RegisterCallback", "SteamAPI_UnregisterCallback"}) {
            steam(name, ok);
        }
        return n;
    }

private:
    struct Channel {
        std::uint64_t sound = 0;
        std::uint64_t user = 0;
        std::uint32_t mode = 0;
        float volume = 1.0F;
        int index = 0;
    };
    std::uint64_t next() {
        const std::lock_guard<std::mutex> lock(m_);
        next_handle_ += 0x10;
        return next_handle_;
    }

    Runtime& rt_;
    std::mutex m_;
    std::map<std::uint64_t, Channel> channels_;
    std::uint64_t next_handle_ = 0x7F000000;
    std::atomic<std::size_t> sounds_{0};
};

}  // namespace wse::recomp::shim
