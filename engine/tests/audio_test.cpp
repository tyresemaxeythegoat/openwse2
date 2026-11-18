// Host and Android unit + retail corpus tests for Slice P6 audio decoders,
// 2D/3D stereo mixer, music scheduler, and VM/world audio integration.
//
// Build/run: powershell -NoProfile -ExecutionPolicy Bypass -File engine/build_host.ps1

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <initializer_list>
#include <limits>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <thread>
#include <vector>

#include "wse/asset/audio.h"
#include "wse/asset/audio_codecs.h"
#include "wse/core/math.h"
#include "wse/game/module_asset_loaders.h"
#include "wse/game/world_audio.h"
#include "wse/platform/audio.h"
#include "wse/platform/audio_worker.h"
#include "wse/platform/vfs.h"
#include "wse/vm/block_vm.h"

namespace {

int g_failures = 0;

void expect(bool condition, const char* what) {
    if (!condition) {
        ++g_failures;
        std::printf("FAIL  %s\n", what);
    }
}

[[nodiscard]] bool close(float a, float b, float eps = 1e-3f) noexcept {
    return std::fabs(a - b) <= eps;
}

constexpr std::uint64_t tagged(std::uint8_t tag, std::uint64_t payload) noexcept {
    return (static_cast<std::uint64_t>(tag & 0x7FU) << 56) |
           (payload & wse::vm::kPayloadMask);
}

constexpr std::int64_t reg(std::int32_t idx) noexcept {
    return static_cast<std::int64_t>(tagged(1, static_cast<std::uint64_t>(idx)));
}

wse::vm::Statement make_stmt(std::uint32_t opcode,
                             std::initializer_list<std::int64_t> args) {
    wse::vm::Statement stmt{};
    stmt.opcode = opcode;
    stmt.numOperands = static_cast<std::int32_t>(args.size());
    std::size_t idx = 0;
    for (std::int64_t arg : args) {
        if (idx < wse::vm::kMaxOperands) {
            stmt.operands[idx++] = arg;
        }
    }
    return stmt;
}

bool run_stmt(wse::vm::VmContext& ctx,
              std::uint32_t opcode,
              std::initializer_list<std::int64_t> args,
              std::int32_t caller = 0) {
    std::vector<std::int64_t> locals(32, 0);
    const auto stmt = make_stmt(opcode, args);
    return wse::vm::execute_statement(stmt, caller, ctx, locals);
}

void push_u16_le(std::vector<std::uint8_t>& dst, std::uint16_t v) {
    dst.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    dst.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
}

void push_u32_le(std::vector<std::uint8_t>& dst, std::uint32_t v) {
    dst.push_back(static_cast<std::uint8_t>(v & 0xFFU));
    dst.push_back(static_cast<std::uint8_t>((v >> 8U) & 0xFFU));
    dst.push_back(static_cast<std::uint8_t>((v >> 16U) & 0xFFU));
    dst.push_back(static_cast<std::uint8_t>((v >> 24U) & 0xFFU));
}

void push_ascii4(std::vector<std::uint8_t>& dst, const char* s) {
    for (std::size_t i = 0; i < 4; ++i) {
        dst.push_back(static_cast<std::uint8_t>(s[i]));
    }
}

std::vector<std::uint8_t> make_wav_pcm(std::uint16_t format_tag,
                                       std::uint16_t channels,
                                       std::uint32_t sample_rate,
                                       std::uint16_t bits_per_sample,
                                       std::size_t frame_count,
                                       float freq_hz = 440.0f,
                                       bool odd_junk_chunk = false,
                                       bool extensible = false) {
    std::vector<std::uint8_t> data;
    const double two_pi = 6.283185307179586;
    for (std::size_t f = 0; f < frame_count; ++f) {
        const float phase = static_cast<float>(
            two_pi * static_cast<double>(freq_hz) * static_cast<double>(f) /
            static_cast<double>(sample_rate));
        const float s0 = 0.5f * std::sin(phase);
        for (std::uint16_t ch = 0; ch < channels; ++ch) {
            const float val = (ch == 0U) ? s0 : -s0;
            if (format_tag == 3U && bits_per_sample == 32U) {
                std::uint32_t u = 0;
                std::memcpy(&u, &val, sizeof(u));
                push_u32_le(data, u);
            } else if (bits_per_sample == 8U) {
                const int u8 = std::clamp(static_cast<int>(std::lround(val * 127.0f + 128.0f)),
                                          0, 255);
                data.push_back(static_cast<std::uint8_t>(u8));
            } else if (bits_per_sample == 16U) {
                const std::int32_t s16 = std::clamp(
                    static_cast<std::int32_t>(std::lround(val * 32767.0f)), -32767, 32767);
                push_u16_le(data, static_cast<std::uint16_t>(static_cast<std::int16_t>(s16)));
            } else if (bits_per_sample == 24U) {
                const std::int32_t s24 = std::clamp(
                    static_cast<std::int32_t>(std::lround(val * 8388607.0f)), -8388607, 8388607);
                const std::uint32_t u = static_cast<std::uint32_t>(s24);
                data.push_back(static_cast<std::uint8_t>(u & 0xFFU));
                data.push_back(static_cast<std::uint8_t>((u >> 8U) & 0xFFU));
                data.push_back(static_cast<std::uint8_t>((u >> 16U) & 0xFFU));
            } else if (bits_per_sample == 32U) {
                const std::int32_t s32 = static_cast<std::int32_t>(
                    static_cast<double>(val) * 2147483000.0);
                push_u32_le(data, static_cast<std::uint32_t>(s32));
            }
        }
    }

    std::vector<std::uint8_t> fmt;
    const std::uint16_t block_align = static_cast<std::uint16_t>(channels * (bits_per_sample / 8U));
    const std::uint32_t byte_rate = sample_rate * static_cast<std::uint32_t>(block_align);
    push_u16_le(fmt, extensible ? 0xFFFEU : format_tag);
    push_u16_le(fmt, channels);
    push_u32_le(fmt, sample_rate);
    push_u32_le(fmt, byte_rate);
    push_u16_le(fmt, block_align);
    push_u16_le(fmt, bits_per_sample);
    if (extensible) {
        push_u16_le(fmt, 22U);  // cbSize
        push_u16_le(fmt, bits_per_sample);
        push_u32_le(fmt, 0U);  // channel mask
        push_u16_le(fmt, format_tag);
        push_u16_le(fmt, 0U);
        static constexpr std::uint8_t kGuidTail[12] = {
            0x00, 0x00, 0x10, 0x00, 0x80, 0x00, 0x00, 0xAA, 0x00, 0x38, 0x9B, 0x71};
        fmt.insert(fmt.end(), std::begin(kGuidTail), std::end(kGuidTail));
    }

    std::vector<std::uint8_t> out;
    push_ascii4(out, "RIFF");
    push_u32_le(out, 0U);  // patched below
    push_ascii4(out, "WAVE");
    if (odd_junk_chunk) {
        push_ascii4(out, "JUNK");
        push_u32_le(out, 3U);
        out.push_back('a');
        out.push_back('b');
        out.push_back('c');
        out.push_back(0U);  // pad byte for odd chunk size
    }
    push_ascii4(out, "fmt ");
    push_u32_le(out, static_cast<std::uint32_t>(fmt.size()));
    out.insert(out.end(), fmt.begin(), fmt.end());
    push_ascii4(out, "data");
    push_u32_le(out, static_cast<std::uint32_t>(data.size()));
    out.insert(out.end(), data.begin(), data.end());

    const std::uint32_t riff_sz = static_cast<std::uint32_t>(out.size() - 8U);
    out[4] = static_cast<std::uint8_t>(riff_sz & 0xFFU);
    out[5] = static_cast<std::uint8_t>((riff_sz >> 8U) & 0xFFU);
    out[6] = static_cast<std::uint8_t>((riff_sz >> 16U) & 0xFFU);
    out[7] = static_cast<std::uint8_t>((riff_sz >> 24U) & 0xFFU);
    return out;
}

std::shared_ptr<wse::asset::AudioPcmBuffer> make_sine_pcm(std::uint32_t sr,
                                                          std::size_t frames,
                                                          float freq_hz = 440.0f,
                                                          float amp = 0.5f) {
    auto buf = std::make_shared<wse::asset::AudioPcmBuffer>();
    buf->sample_rate = sr;
    buf->channels = 1U;
    buf->samples.resize(frames);
    constexpr double kTwoPi = 6.283185307179586;
    for (std::size_t i = 0; i < frames; ++i) {
        buf->samples[i] = amp * static_cast<float>(std::sin(
                                    kTwoPi * static_cast<double>(freq_hz) *
                                    static_cast<double>(i) / static_cast<double>(sr)));
    }
    return buf;
}

// Clean-room synthetic Vorbis I mono 44100 Hz stream (6 overlapping blocks -> 640 frames).
inline constexpr std::array<std::uint8_t, 326> kSyntheticOggBytes = {
    0x4f, 0x67, 0x67, 0x53, 0x00, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x56,
    0x34, 0x12, 0x00, 0x00, 0x00, 0x00, 0xf7, 0xd9, 0x03, 0xc2, 0x01, 0x1e, 0x01, 0x76, 0x6f, 0x72,
    0x62, 0x69, 0x73, 0x00, 0x00, 0x00, 0x00, 0x01, 0x44, 0xac, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x88, 0x01, 0x4f, 0x67, 0x67, 0x53, 0x00, 0x00,
    0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12, 0x01, 0x00, 0x00, 0x00,
    0x29, 0x99, 0xf3, 0x61, 0x02, 0x13, 0x49, 0x03, 0x76, 0x6f, 0x72, 0x62, 0x69, 0x73, 0x03, 0x00,
    0x00, 0x00, 0x57, 0x53, 0x45, 0x00, 0x00, 0x00, 0x00, 0x01, 0x05, 0x76, 0x6f, 0x72, 0x62, 0x69,
    0x73, 0x01, 0x42, 0x43, 0x56, 0x01, 0x00, 0x02, 0x00, 0x00, 0x81, 0xa1, 0x99, 0x99, 0xfb, 0xad,
    0x99, 0x99, 0xfd, 0x05, 0x14, 0x1a, 0xb2, 0x0a, 0x00, 0x10, 0x00, 0x00, 0x08, 0x04, 0x00, 0x00,
    0x00, 0x08, 0x00, 0x08, 0x00, 0x04, 0x38, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x08, 0x00,
    0xf0, 0x01, 0x00, 0x00, 0x04, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x00, 0x80, 0x4f, 0x67, 0x67, 0x53, 0x00, 0x04, 0x80, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0x00, 0x78, 0x56, 0x34, 0x12, 0x02, 0x00, 0x00, 0x00, 0x37, 0x30, 0x33, 0x80, 0x06, 0x13, 0x13,
    0x13, 0x13, 0x13, 0x13, 0x22, 0x23, 0x53, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x05, 0x22, 0x23, 0xab, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x02, 0x22, 0x23, 0x53, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x05, 0x22, 0x23, 0xab,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x02,
    0x22, 0x23, 0x53, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55, 0x55,
    0x55, 0x55, 0x05, 0x22, 0x23, 0xab, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0xaa,
    0xaa, 0xaa, 0xaa, 0xaa, 0xaa, 0x02
};

// Generates N clean-room MPEG-1 Layer III 44100 Hz 128 kbps mono frames (417 bytes/frame, 1152 samples/frame).
std::vector<std::uint8_t> make_synthetic_mp3(std::size_t frame_count) {
    static constexpr std::array<std::uint8_t, 4> kFrameHead = {0xFFU, 0xFBU, 0x90U, 0xC4U};
    std::array<std::uint8_t, 17> si{};
    std::size_t bitpos = 0;
    auto put_bits = [&](std::uint32_t val, int n) {
        for (int i = n - 1; i >= 0; --i) {
            if (((val >> i) & 1U) != 0U) {
                si[bitpos >> 3U] |= static_cast<std::uint8_t>(1U << (7U - (bitpos & 7U)));
            }
            ++bitpos;
        }
    };
    put_bits(0U, 9);
    put_bits(0U, 5);
    put_bits(0U, 4);
    for (int g = 0; g < 2; ++g) {
        put_bits(24U, 12);
        put_bits(8U, 9);
        put_bits(185U, 8);
        put_bits(0U, 4);
        put_bits(0U, 1);
        put_bits(1U, 5);
        put_bits(1U, 5);
        put_bits(1U, 5);
        put_bits(3U, 4);
        put_bits(2U, 3);
        put_bits(0U, 1);
        put_bits(0U, 1);
        put_bits(0U, 1);
    }

    std::vector<std::uint8_t> out;
    out.reserve(frame_count * 417U);
    for (std::size_t f = 0; f < frame_count; ++f) {
        out.push_back(kFrameHead[0]);
        out.push_back(kFrameHead[1]);
        out.push_back(kFrameHead[2]);
        out.push_back(kFrameHead[3]);
        out.insert(out.end(), si.begin(), si.end());
        out.resize(out.size() + 396U, 0U);
    }
    return out;
}

[[nodiscard]] double buffer_rms(const wse::asset::AudioPcmBuffer& buf) noexcept {
    if (buf.samples.empty()) {
        return 0.0;
    }
    double sum_sq = 0.0;
    for (float s : buf.samples) {
        sum_sq += static_cast<double>(s) * static_cast<double>(s);
    }
    return std::sqrt(sum_sq / static_cast<double>(buf.samples.size()));
}

void test_wav_decoding_and_path_resolution() {
    using namespace wse::asset;

    // 1. 8-bit unsigned mono, 16-bit LE stereo, 24-bit mono, 32-bit PCM, 32-bit float + odd chunk + extensible 4ch
    {
        AudioPcmBuffer pcm{};
        std::string err;
        const auto wav8 = make_wav_pcm(1U, 1U, 22050U, 8U, 512U);
        expect(decode_wav_pcm(wav8, pcm, &err) && pcm.sample_rate == 22050U && pcm.channels == 1U &&
                   pcm.frame_count() == 512U && buffer_rms(pcm) > 0.1,
               "WAV 8-bit unsigned mono decodes with non-zero RMS");

        const auto wav16 = make_wav_pcm(1U, 2U, 44100U, 16U, 1024U, 440.0f, true);
        expect(decode_wav_pcm(wav16, pcm, &err) && pcm.sample_rate == 44100U && pcm.channels == 2U &&
                   pcm.frame_count() == 1024U && buffer_rms(pcm) > 0.1,
               "WAV 16-bit stereo with odd-sized JUNK chunk decodes cleanly");

        const auto wav24 = make_wav_pcm(1U, 1U, 48000U, 24U, 400U);
        expect(decode_wav_pcm(wav24, pcm, &err) && pcm.sample_rate == 48000U && pcm.channels == 1U &&
                   pcm.frame_count() == 400U && buffer_rms(pcm) > 0.1,
               "WAV 24-bit integer mono decodes cleanly");

        const auto wav32 = make_wav_pcm(1U, 1U, 44100U, 32U, 400U);
        expect(decode_wav_pcm(wav32, pcm, &err) && pcm.sample_rate == 44100U &&
                   pcm.frame_count() == 400U && buffer_rms(pcm) > 0.1,
               "WAV 32-bit integer mono decodes cleanly");

        const auto wavf32 = make_wav_pcm(3U, 2U, 48000U, 32U, 400U);
        expect(decode_wav_pcm(wavf32, pcm, &err) && pcm.sample_rate == 48000U && pcm.channels == 2U &&
                   pcm.frame_count() == 400U && buffer_rms(pcm) > 0.1,
               "WAV IEEE 32-bit float stereo decodes cleanly");

        const auto wav_ext = make_wav_pcm(1U, 4U, 44100U, 16U, 256U, 440.0f, false, true);
        expect(decode_wav_pcm(wav_ext, pcm, &err) && pcm.channels == 2U && pcm.frame_count() == 256U,
               "WAVE_FORMAT_EXTENSIBLE 4-channel downmixes to stereo");

        // PcmBufferStreamDecoder chunked read + seek_start
        PcmBufferStreamDecoder stream(pcm);
        expect(stream.sample_rate() == 44100U && stream.channels() == 2U && stream.total_frames() == 256U,
               "PcmBufferStreamDecoder metadata matches");
        std::vector<float> chunk;
        std::size_t total = 0U;
        while (true) {
            const std::size_t n = stream.read_frames(100U, chunk);
            if (n == 0U) {
                break;
            }
            total += n;
        }
        expect(total == 256U && stream.cursor_frame() == 256U,
               "PcmBufferStreamDecoder reads all frames to EOF");
        expect(stream.seek_start() && stream.cursor_frame() == 0U && stream.read_frames(64U, chunk) == 64U,
               "PcmBufferStreamDecoder seek_start rewinds cursor cleanly");

        // WavStreamDecoder converts from the file bytes on demand and matches a full decode.
        auto wav_stream = WavStreamDecoder::open(std::vector<std::uint8_t>(wav_ext.begin(), wav_ext.end()));
        std::vector<float> streamed;
        if (wav_stream != nullptr) {
            while (wav_stream->read_frames(100U, chunk) > 0U) {
                streamed.insert(streamed.end(), chunk.begin(), chunk.end());
            }
        }
        expect(wav_stream != nullptr && wav_stream->channels() == 2U &&
                   wav_stream->total_frames() == 256U && streamed == pcm.samples,
               "WavStreamDecoder streams 4-channel WAV identically to decode_wav_pcm");
    }

    // 2. Hardening against truncated/corrupt headers
    {
        AudioPcmBuffer pcm{};
        std::string err;
        const std::array<std::uint8_t, 8> too_small = {'R', 'I', 'F', 'F', 0, 0, 0, 0};
        expect(!decode_wav_pcm(too_small, pcm, &err) && !pcm.valid(), "rejects truncated header");

        auto bad = make_wav_pcm(1U, 1U, 44100U, 16U, 64U);
        bad[0] = 'X';
        expect(!decode_wav_pcm(bad, pcm, &err) && !pcm.valid(), "rejects non-RIFF header");

        // Safety deviation: a short data chunk plays the frames that are present.
        auto trunc = make_wav_pcm(1U, 1U, 44100U, 16U, 64U);
        trunc.resize(trunc.size() - 20U);
        expect(decode_wav_pcm(trunc, pcm, &err) && pcm.frame_count() == 54U,
               "truncated data chunk decodes the frames present");
    }

    // 3. Path resolution precedence + sound/music flags
    {
        wse::platform::MemoryVfs vfs;
        const std::vector<std::uint8_t> dummy = {1U, 2U, 3U};
        vfs.write_file("Sounds/sword.wav", dummy);
        expect(resolve_sound_sample_path(vfs, "Modules/Native", "en", "sword.wav") == "Sounds/sword.wav",
               "sound resolution falls back to base Sounds/");

        vfs.write_file("Sounds/languages/en/sword.wav", dummy);
        expect(resolve_sound_sample_path(vfs, "Modules/Native", "en", "sword.wav") ==
                   "Sounds/languages/en/sword.wav",
               "sound resolution prefers base Sounds/languages/<lang> over base Sounds/");

        vfs.write_file("Modules/Native/Sounds/sword.wav", dummy);
        expect(resolve_sound_sample_path(vfs, "Modules/Native", "en", "sword.wav") ==
                   "Modules/Native/Sounds/sword.wav",
               "sound resolution prefers module Sounds/ over base Sounds/languages/");

        vfs.write_file("Modules/Native/Sounds/languages/en/sword.wav", dummy);
        expect(resolve_sound_sample_path(vfs, "Modules/Native", "en", "sword.wav") ==
                   "Modules/Native/Sounds/languages/en/sword.wav",
               "sound resolution prefers module Sounds/languages/<lang> first");

        vfs.write_file("Music/calm.ogg", dummy);
        expect(resolve_music_track_path(vfs, "Modules/Native", "calm.ogg") == "Music/calm.ogg",
               "music resolution falls back to Music/");
        vfs.write_file("Modules/Native/Music/calm.ogg", dummy);
        expect(resolve_music_track_path(vfs, "Modules/Native", "calm.ogg") ==
                   "Modules/Native/Music/calm.ogg",
               "music resolution prefers <module>/Music/ over Music/");

        expect(decode_sound_priority_nibble(0x00U) == 5 && decode_sound_priority_nibble(0xA0U) == 10,
               "sound priority nibble defaults to 5 when 0");
        expect(close(decode_sound_volume(0x000U), 10.0f / 15.0f) &&
                   close(decode_sound_volume(0xF00U), 1.0f),
               "sound volume nibble defaults to 10/15 and scales / 15.0f");
        const auto nf0 = normalize_music_track_flags(0x10U, 0x00U);
        expect(nf0.continue_flags == 0x10U && nf0.culture_flags == 0x10U &&
                   nf0.continue_culture_flags == 0x10U,
               "music flags merge into continue_flags");
        const auto nf_all = normalize_music_track_flags(0x400U, 0x800U);
        expect(nf_all.culture_flags == 0x3FU && nf_all.continue_culture_flags == 0x3FU &&
                   nf_all.continue_flags == 0xC00U,
               "music zero culture mask expands to 0x3F");
    }
}

void test_ogg_and_mp3_codecs_and_streaming() {
    using namespace wse::asset;

    auto backend = create_audio_codec_backend();
    expect(backend != nullptr && backend->supports(AudioCodecKind::ogg) &&
               backend->supports(AudioCodecKind::mp3) &&
               !backend->supports(AudioCodecKind::wav),
           "compressed codec backend supports OGG and MP3");

    expect(detect_audio_codec(kSyntheticOggBytes, "") == AudioCodecKind::ogg,
           "detects OggS magic header without extension");
    const auto mp3_bytes = make_synthetic_mp3(3U);
    expect(detect_audio_codec(mp3_bytes, "") == AudioCodecKind::mp3,
           "detects MP3 frame sync without extension");

    AudioPcmBuffer ogg_pcm{};
    std::string err;
    expect(decode_audio_bytes(kSyntheticOggBytes, "", backend.get(), ogg_pcm, &err) &&
               ogg_pcm.valid() && ogg_pcm.sample_rate == 44100U && ogg_pcm.channels == 1U &&
               ogg_pcm.frame_count() == 640U && buffer_rms(ogg_pcm) > 0.01,
           "decodes Ogg Vorbis memory payload with non-zero RMS");

    auto ogg_stream = open_audio_stream(
        std::vector<std::uint8_t>(kSyntheticOggBytes.begin(), kSyntheticOggBytes.end()), "sfx.ogg",
        backend.get(), &err);
    expect(ogg_stream != nullptr && ogg_stream->sample_rate() == 44100U &&
               ogg_stream->channels() == 1U,
           "opens Ogg Vorbis stream decoder");
    if (ogg_stream != nullptr) {
        std::vector<float> chunk;
        std::size_t total = 0;
        while (true) {
            const std::size_t got = ogg_stream->read_frames(200U, chunk);
            if (got == 0U) {
                break;
            }
            total += got;
        }
        expect(total == 640U && ogg_stream->seek_start() &&
                   ogg_stream->read_frames(128U, chunk) == 128U,
               "Ogg Vorbis stream reads to EOF and re-seeks cleanly");
    }

    AudioPcmBuffer mp3_pcm{};
    expect(decode_audio_bytes(mp3_bytes, "track.MP3", backend.get(), mp3_pcm, &err) &&
               mp3_pcm.valid() && mp3_pcm.sample_rate == 44100U && mp3_pcm.channels == 1U &&
               mp3_pcm.frame_count() == 3456U && buffer_rms(mp3_pcm) > 0.01,
           "decodes MP3 memory payload with non-zero RMS");

    auto mp3_stream = open_audio_stream(mp3_bytes, "track.mp3", backend.get(), &err);
    expect(mp3_stream != nullptr && mp3_stream->sample_rate() == 44100U &&
               mp3_stream->channels() == 1U,
           "opens MP3 stream decoder");
    if (mp3_stream != nullptr) {
        std::vector<float> chunk;
        std::size_t total = 0;
        while (true) {
            const std::size_t got = mp3_stream->read_frames(512U, chunk);
            if (got == 0U) {
                break;
            }
            total += got;
        }
        expect(total == 3456U && mp3_stream->seek_start() &&
                   mp3_stream->read_frames(256U, chunk) == 256U,
               "MP3 stream reads to EOF and re-seeks cleanly");
    }

    // Corrupt OGG / MP3 rejection
    const std::vector<std::uint8_t> corrupt = {'O', 'g', 'g', 'S', 0, 1, 2, 3};
    AudioPcmBuffer bad_out{};
    expect(!decode_audio_bytes(corrupt, "bad.ogg", backend.get(), bad_out, &err) && !bad_out.valid(),
           "rejects corrupt Ogg Vorbis payload safely");
}

void test_mixer_3d_panning_lod_priority_occlusion_and_determinism() {
    using namespace wse::asset;
    using namespace wse::platform;

    // 1. v1.1.5.5 default config + listener_from_camera_transform (-u -> forward, +f -> up, +s -> right)
    {
        AudioConfig cfg{};
        expect(cfg.sound_enabled && cfg.music_enabled && close(cfg.sound_volume, 1.0f) &&
                   close(cfg.music_volume, 0.4f) && cfg.max_channels == 64 &&
                   cfg.max_virtual_channels == 100 && close(cfg.distance_priority_factor, 0.47f) &&
                   close(cfg.occlusion_max_distance, 80.0f) && close(cfg.min_distance_3d, 2.0f) &&
                   close(cfg.max_distance_3d, 80.0f),
               "AudioConfig defaults match v1.1.5.5 rglConfig::Sound");

        wse::core::WseTransform cam{};
        cam.s = {1.0f, 0.0f, 0.0f, 0.0f};
        cam.f = {0.0f, 0.0f, 1.0f, 0.0f};
        cam.u = {0.0f, -1.0f, 0.0f, 0.0f};
        cam.o = {10.0f, 20.0f, 2.0f, 0.0f};
        const AudioListener lis = listener_from_camera_transform(cam);
        expect(close(lis.position.x, 10.0f) && close(lis.forward.y, 1.0f) &&
                   close(lis.up.z, 1.0f) && close(lis.right.x, 1.0f),
               "listener_from_camera_transform maps -u->forward, +f->up, +s->right");
    }

    auto tone_22k = make_sine_pcm(22050U, 22050U, 440.0f, 0.5f);  // 1s tone at 22050 Hz

    // 2. Stereo panning (right vs left), 2D centering, and distance attenuation
    {
        WseAudioMixer mixer;
        CaptureAudioDevice cap_r;
        expect(cap_r.open(AudioDeviceSpec{48000U, 2U}), "open capture device");
        mixer.load_sound_bank({MixerSoundSample{0U, tone_22k}},
                              {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});

        const std::int32_t ch_r =
            mixer.play_sound_at_position(0, wse::core::WseVec4{10.0f, 0.0f, 0.0f, 0.0f});
        expect(ch_r >= 0 && mixer.pump(cap_r, 4800U), "play right-panned 3D sound");
        expect(cap_r.rms_right() > cap_r.rms_left() * 5.0 && cap_r.rms_right() > 0.01,
               "3D panning places +X sound into right channel");

        mixer.stop_all_sounds(true);
        CaptureAudioDevice cap_l;
        cap_l.open(AudioDeviceSpec{48000U, 2U});
        mixer.play_sound_at_position(0, wse::core::WseVec4{-10.0f, 0.0f, 0.0f, 0.0f});
        mixer.pump(cap_l, 4800U);
        expect(cap_l.rms_left() > cap_l.rms_right() * 5.0 && cap_l.rms_left() > 0.01,
               "3D panning places -X sound into left channel");

        mixer.stop_all_sounds(true);
        CaptureAudioDevice cap_far;
        cap_far.open(AudioDeviceSpec{48000U, 2U});
        mixer.play_sound_at_position(0, wse::core::WseVec4{30.0f, 0.0f, 0.0f, 0.0f});
        mixer.pump(cap_far, 4800U);
        expect(cap_far.rms_right() < cap_r.rms_right() * 0.5,
               "distance attenuation reduces gain cleanly with distance");

        mixer.stop_all_sounds(true);
        CaptureAudioDevice cap_2d;
        cap_2d.open(AudioDeviceSpec{48000U, 2U});
        mixer.play_sound_2d(0);
        mixer.pump(cap_2d, 4800U);
        expect(close(static_cast<float>(cap_2d.rms_left()), static_cast<float>(cap_2d.rms_right()),
                     1e-4f) &&
                   cap_2d.rms_left() > 0.1,
               "2D playback delivers balanced L==R stereo output and resamples 22k->48k");
    }

    // 3. LOD fallback (`0x1000000`) & culture variant filter
    {
        WseAudioMixer mixer;
        mixer.set_seed(42U);
        auto sample_near = make_sine_pcm(48000U, 4800U, 440.0f, 0.6f);
        auto sample_far = make_sine_pcm(48000U, 4800U, 220.0f, 0.6f);
        mixer.load_sound_bank(
            {MixerSoundSample{0U, sample_near}, MixerSoundSample{0U, sample_far}},
            {MixerSoundDef{kSoundFlagLodFallback, {MixerSoundVariant{0, 1U}}},
             MixerSoundDef{0U, {MixerSoundVariant{1, 0U}}}});
        // Near + wrong culture 2 -> rejected by variant culture filter
        expect(mixer.play_sound_at_position(0, wse::core::WseVec4{2.0f, 0.0f, 0.0f, 0.0f}, {}, 0U,
                                            1.0f, 2) == -1,
               "culture filter suppresses mismatched non-universal variant");
        // Far > 15m -> LOD steps from sound 0 -> sound 1 (sample 1)
        const std::int32_t lod_ch =
            mixer.play_sound_at_position(0, wse::core::WseVec4{25.0f, 0.0f, 0.0f, 0.0f});
        expect(lod_ch >= 0 && mixer.is_channel_playing(lod_ch),
               "LOD fallback resolves distant sound to fallback variant");
    }

    // 4. Virtual channel priority eviction, occlusion hysteresis, low-pass filter, & determinism
    {
        AudioConfig cfg{};
        cfg.max_virtual_channels = 2;
        cfg.max_channels = 2;
        WseAudioMixer mixer(cfg);
        std::vector<std::int32_t> ended;
        mixer.set_channel_end_callback([&](std::int32_t ch) { ended.push_back(ch); });
        auto short_pcm = make_sine_pcm(48000U, 4800U, 440.0f, 0.5f);
        mixer.load_sound_bank({MixerSoundSample{0U, short_pcm}},
                              {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});

        const std::int32_t c2d = mixer.play_sound_2d(0);  // prio 0
        const std::int32_t cfar =
            mixer.play_sound_at_position(0, wse::core::WseVec4{50.0f, 0.0f, 0.0f, 0.0f});
        expect(c2d >= 0 && cfar >= 0 && c2d != cfar, "occupies 2 virtual channels");
        const std::int32_t cnear =
            mixer.play_sound_at_position(0, wse::core::WseVec4{1.0f, 0.0f, 0.0f, 0.0f});
        expect(cnear == cfar && ended.size() == 1U && ended[0] == cfar,
               "higher priority near sound evicts lowest-importance far channel");

        // Occlusion hysteresis around 80m (enter > 88m, exit <= 72m)
        mixer.stop_all_sounds(true);
        const std::int32_t cocc =
            mixer.play_sound_at_position(0, wse::core::WseVec4{85.0f, 0.0f, 0.0f, 0.0f});
        expect(cocc >= 0 && mixer.is_channel_ignoring_geometry(cocc),
               "beyond occlusion_max_distance starts ignore_geometry=true");
        mixer.set_channel_position(cocc, wse::core::WseVec4{75.0f, 0.0f, 0.0f, 0.0f});
        mixer.update_occlusion_distances();
        expect(mixer.is_channel_ignoring_geometry(cocc),
               "occlusion hysteresis holds ignore_geometry at 75m (> 72m)");
        mixer.set_channel_position(cocc, wse::core::WseVec4{70.0f, 0.0f, 0.0f, 0.0f});
        mixer.update_occlusion_distances();
        expect(!mixer.is_channel_ignoring_geometry(cocc),
               "occlusion hysteresis clears ignore_geometry inside 72m");

        // Deterministic mix hash across two independent runs
        auto run_deterministic = [&]() -> std::uint64_t {
            WseAudioMixer m(cfg);
            m.set_seed(0xBEEFU);
            m.load_sound_bank({MixerSoundSample{kSoundFlagRandomStartPos, short_pcm}},
                              {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
            m.play_sound_at_position(0, wse::core::WseVec4{3.0f, 1.0f, 0.0f, 0.0f});
            CaptureAudioDevice cap;
            cap.open(AudioDeviceSpec{48000U, 2U});
            m.pump(cap, 2400U);
            return cap.hash_pcm_i16();
        };
        const std::uint64_t h1 = run_deterministic();
        const std::uint64_t h2 = run_deterministic();
        expect(h1 != 0U && h1 == h2, "deterministic CaptureAudioDevice hash matches across runs");
    }
}

void test_mixer_music_scheduler_and_streaming() {
    using namespace wse::asset;
    using namespace wse::platform;

    auto backend = create_audio_codec_backend();
    const auto long_mp3 = make_synthetic_mp3(48U);  // 48 * 1152 = 55296 samples (~1.25s)

    WseAudioMixer mixer;
    mixer.set_seed(77U);
    mixer.set_music_stream_opener([&](std::size_t) -> std::unique_ptr<AudioStreamDecoder> {
        return open_audio_stream(long_mp3, "music.mp3", backend.get());
    });
    mixer.load_music_tracks({
        MixerMusicTrack{"m0.mp3", normalize_music_track_flags(0x01U, 0x01U), nullptr},
        MixerMusicTrack{"m1.mp3", normalize_music_track_flags(0x02U | kMusicFlagPersist, 0x02U),
                        nullptr},
        MixerMusicTrack{"cue.mp3", normalize_music_track_flags(0x00U, 0x00U), nullptr},
    });

    CaptureAudioDevice cap;
    cap.open(AudioDeviceSpec{48000U, 2U});

    // Situation 0x01 auto-picks track 0 on first 0.5s tick
    mixer.set_music_situation(0x01U);
    mixer.update(0.0f);
    expect(mixer.active_music_track() == 0, "music scheduler auto-picks matching situation track 0");
    mixer.pump(cap, 2400U);
    expect(cap.rms_left() > 0.001, "streaming MP3 music produces non-zero mixed stereo output");

    // Request track 2 with option 1 -> starts 5.0s fadeout on slot 0 and swaps after 5.0s
    mixer.play_track(2, 1);
    mixer.update(0.6f);
    expect(mixer.is_music_fading() && mixer.active_music_track() == 0,
           "play_track(..., 1) triggers 5.0s fadeout before switching");
    mixer.update(6.0f);
    expect(!mixer.is_music_fading() && mixer.active_music_track() == 2,
           "after > 5.0s fadeout, scheduler transitions cleanly to requested track");

    // play_cue_track runs concurrently in slot 1, and option 2 switches immediately to persist track 1
    mixer.play_cue_track(2);
    mixer.play_track(1, 2);
    mixer.update(6.6f);
    expect(mixer.active_music_track() == 1 && mixer.active_cue_track() == 2,
           "play_track(..., 2) switches immediately and play_cue_track populates slot 1");
}

void test_vm_and_world_audio_bridge_integration() {
    using namespace wse::asset;
    using namespace wse::game;
    using namespace wse::platform;

    MemoryVfs vfs;
    const auto wav_short = make_wav_pcm(1U, 1U, 48000U, 16U, 4800U);     // 0.1s
    const auto wav_loop = make_wav_pcm(1U, 1U, 48000U, 16U, 2400U);      // 0.05s
    const auto mp3_track = make_synthetic_mp3(12U);
    vfs.write_file("Modules/TestMod/Sounds/hit.wav", wav_short);
    vfs.write_file("Modules/TestMod/Sounds/hum.wav", wav_loop);
    vfs.write_file("Modules/TestMod/Music/theme.mp3", mp3_track);

    LoadedSoundBank bank{};
    bank.version = 3;
    bank.samples = {
        LoadedSoundSample{"hit.wav", 0U},
        LoadedSoundSample{"hum.wav", kSoundFlagLooping},
    };
    bank.sounds = {
        LoadedSound{"snd_hit", 0U, {LoadedSoundRef{0, 0U}}},
        LoadedSound{"snd_hum", kSoundFlagLooping, {LoadedSoundRef{1, 0U}}},
    };
    std::vector<LoadedMusicTrack> tracks = {
        LoadedMusicTrack{"theme.mp3", 0x41U, 0x41U},
    };

    wse::vm::VmContext ctx{};
    auto& w = ctx.world;
    w.has_game = true;
    w.m5.num_sounds = 2;
    w.misc.num_music_tracks = 1;
    w.misc.num_sound_channels = 64;
    w.agents.resize(2);
    w.agents[0].active = true;
    w.agents[0].position.o = {1.0f, 0.0f, 0.0f, 0.0f};
    w.mission_objects.resize(2);
    w.mission_objects[0].active = true;
    w.mission_objects[0].position.o = {2.0f, 0.0f, 0.0f, 0.0f};
    ctx.positions[1].o = {0.0f, 0.0f, 0.0f, 0.0f};

    auto codecs = create_audio_codec_backend();
    WorldAudioBridge bridge;
    const WorldAudioStats st = bridge.initialize(
        vfs, w, bank, tracks, "Modules/TestMod", "", codecs.get(), false);
    expect(st.sample_paths_resolved == 2U && st.sample_paths_missing == 0U &&
               st.music_paths_resolved == 1U && w.audio_runtime == &bridge,
           "WorldAudioBridge resolves module assets and registers runtime hook");

    // 1. prop_instance_play_sound (1881) + prop_instance_get_sound_progress (3618) -> advances -> -1 at EOF
    expect(run_stmt(ctx, 1881U, {0, 0}), "1881 prop_instance_play_sound succeeds");
    const std::int32_t prop_ch = w.mission_objects[0].m5.sound_channel;
    expect(prop_ch >= 0 && run_stmt(ctx, 615U, {reg(0)}) && ctx.registers[0] == prop_ch,
           "615 store_last_sound_channel matches prop channel");
    expect(run_stmt(ctx, 3618U, {reg(1), 0}) && ctx.registers[1] == 0,
           "3618 prop_instance_get_sound_progress starts at 0");

    CaptureAudioDevice cap;
    cap.open(AudioDeviceSpec{48000U, 2U});
    bridge.mixer().pump(cap, 2400U);  // 50% of 4800 frames
    expect(run_stmt(ctx, 3618U, {reg(2), 0}) && ctx.registers[2] >= 45 && ctx.registers[2] <= 55,
           "3618 prop_instance_get_sound_progress advances to ~50%");

    bridge.mixer().pump(cap, 3000U);  // Past EOF -> end callback clears prop channel
    expect(w.mission_objects[0].m5.sound_channel == -1 && run_stmt(ctx, 3618U, {reg(3), 0}) &&
               ctx.registers[3] == -1,
           "3618 returns -1 and clears mission object sound state after EOF");

    // 2. agent_play_sound (1750) + agent_stop_sound (1808) + stop_all_sounds (609)
    expect(run_stmt(ctx, 1750U, {0, 0}) && w.agents[0].m5.sound_channel >= 0,
           "1750 agent_play_sound assigns active sound channel to agent");
    expect(run_stmt(ctx, 1808U, {0}) && w.agents[0].m5.sound_channel == -1,
           "1808 agent_stop_sound stops and clears agent sound channel");

    expect(run_stmt(ctx, 599U, {0, 1, 0}) && run_stmt(ctx, 600U, {1, 0}),
           "599 play_sound_at_position and 600 play_sound execute");
    const std::int32_t ch_2d = w.music.last_sound_channel;
    expect(ch_2d >= 0 && bridge.mixer().active_sound_channel_count() == 2U,
           "both 3D hit and 2D looping hum channels are active");

    // stop_all_sounds(0) stops looping channels ONLY; stop_sound_channel(ch) stops specific channel
    expect(run_stmt(ctx, 609U, {0}) && bridge.mixer().active_sound_channel_count() == 1U &&
               !bridge.mixer().is_channel_playing(ch_2d),
           "609 stop_all_sounds(0) stops looping channel while leaving one-shot alive");
    expect(run_stmt(ctx, 600U, {1, 0}) && w.music.last_sound_channel >= 0,
           "replay looping hum");
    const std::int32_t stop_target = w.music.last_sound_channel;
    expect(run_stmt(ctx, 616U, {stop_target}) && !bridge.mixer().is_channel_playing(stop_target) &&
               w.music.stopped_channels[static_cast<std::size_t>(stop_target)],
           "616 stop_sound_channel stops channel and marks world stopped_channels");
    expect(run_stmt(ctx, 609U, {1}) && bridge.mixer().active_sound_channel_count() == 0U,
           "609 stop_all_sounds(1) stops all active channels");

    // 3. play_track (601), music_set_situation (603), music_set_culture (604), play_cue_track (602)
    expect(run_stmt(ctx, 603U, {0x01}) && run_stmt(ctx, 604U, {0x01}) &&
               run_stmt(ctx, 601U, {0, 2}) && run_stmt(ctx, 602U, {0}),
           "music opcodes 601..604 execute cleanly");
    bridge.mixer().update(0.6f);  // Advance past 0.5s music manager frameMove cadence (`0x140290da0`).
    bridge.mixer().pump(cap, 2400U);
    expect(bridge.mixer().active_music_track() == 0 && bridge.mixer().active_cue_track() == 0 &&
               cap.rms_left() > 0.005,
           "WorldAudioBridge plays and mixes streamed MP3 track + cue through CaptureAudioDevice");
}

void test_retail_warband_audio_gate() {
    using namespace wse::asset;
    using namespace wse::game;
    using namespace wse::platform;

    std::error_code ec;
    const char* root_env = std::getenv("WSE_WARBAND_ROOT");
    const std::filesystem::path retail_root(
        (root_env != nullptr && root_env[0] != '\0') ? root_env
                                                      : "C:/GOG Games/Mount and Blade - Warband");
    if (!std::filesystem::exists(retail_root / "Modules/Native/module.ini", ec)) {
        return;
    }

    HostVfs vfs(retail_root);
    auto codecs = create_audio_codec_backend();
    expect(codecs != nullptr, "retail gate has compressed codec backend");

    // Native module: .wav + .ogg sounds and .ogg music
    {
        WseWorldState world{};
        LoadedSoundBank bank{};
        std::vector<LoadedMusicTrack> tracks{};
        const auto s_err = load_sounds(vfs, "Modules/Native/sounds.txt", world, &bank);
        const auto m_err = load_music(vfs, "Modules/Native/music.txt", world, &tracks);
        expect(s_err.ok && m_err.ok && bank.samples.size() == 366U && bank.sounds.size() == 200U &&
                   tracks.size() == 74U,
               "retail Native sounds.txt (366/200) and music.txt (74) load cleanly");

        WorldAudioBridge bridge;
        const WorldAudioStats st = bridge.initialize(
            vfs, world, bank, tracks, "Modules/Native", "", codecs.get(), false);
        expect(st.sample_paths_resolved >= 357U && st.music_paths_resolved >= 73U,
               "retail Native resolves >=357 sound samples and >=73 music tracks");

        std::int32_t wav_snd = -1;
        std::int32_t ogg_snd = -1;
        for (std::size_t si = 0; si < bank.sounds.size(); ++si) {
            for (const auto& r : bank.sounds[si].refs) {
                if (r.sample_no < 0 ||
                    static_cast<std::size_t>(r.sample_no) >= bank.samples.size()) {
                    continue;
                }
                const std::string resolved = resolve_sound_sample_path(
                    vfs, "Modules/Native", "", bank.samples[static_cast<std::size_t>(r.sample_no)].file);
                if (resolved.empty()) {
                    continue;
                }
                if (wav_snd < 0 && audio_detail::ends_with_ci(resolved, ".wav")) {
                    wav_snd = static_cast<std::int32_t>(si);
                } else if (ogg_snd < 0 && audio_detail::ends_with_ci(resolved, ".ogg")) {
                    ogg_snd = static_cast<std::int32_t>(si);
                }
            }
            if (wav_snd >= 0 && ogg_snd >= 0) {
                break;
            }
        }
        std::int32_t ogg_trk = -1;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            if (!resolve_music_track_path(vfs, "Modules/Native", tracks[ti].file).empty()) {
                ogg_trk = static_cast<std::int32_t>(ti);
                break;
            }
        }
        expect(wav_snd >= 0 && ogg_snd >= 0 && ogg_trk >= 0,
               "located retail Native WAV SFX, OGG SFX, and OGG music track");

        const std::int32_t ch_wav = bridge.play_sound_2d(wav_snd, 0U);
        const std::int32_t ch_ogg = bridge.play_sound_2d(ogg_snd, 0U);
        bridge.play_track(ogg_trk, 2);
        CaptureAudioDevice cap;
        cap.open(AudioDeviceSpec{48000U, 2U});
        bridge.mixer().pump(cap, 24000U);
        expect(ch_wav >= 0 && ch_ogg >= 0 && bridge.mixer().active_music_track() == ogg_trk &&
                   cap.rms_left() > 0.001 && cap.rms_right() > 0.001,
               "retail Native WAV+OGG SFX and OGG music stream mix to non-zero stereo output");
    }

    // Phantasy Calradia module (when present): includes retail .mp3 SFX & music alongside .wav/.ogg
    if (std::filesystem::exists(retail_root / "Modules/Phantasy Calradia/module.ini", ec)) {
        WseWorldState world{};
        LoadedSoundBank bank{};
        std::vector<LoadedMusicTrack> tracks{};
        const auto s_err =
            load_sounds(vfs, "Modules/Phantasy Calradia/sounds.txt", world, &bank);
        const auto m_err =
            load_music(vfs, "Modules/Phantasy Calradia/music.txt", world, &tracks);
        expect(s_err.ok && m_err.ok && bank.samples.size() >= 1000U && tracks.size() >= 79U,
               "retail Phantasy Calradia sounds.txt and music.txt load cleanly");

        WorldAudioBridge bridge;
        const WorldAudioStats st = bridge.initialize(
            vfs, world, bank, tracks, "Modules/Phantasy Calradia", "", codecs.get(), false);
        expect(st.sample_paths_resolved >= 1000U && st.music_paths_resolved >= 78U,
               "retail Phantasy Calradia resolves >=1000 sound samples and >=78 music tracks");

        std::int32_t mp3_snd = -1;
        for (std::size_t si = 0; si < bank.sounds.size() && mp3_snd < 0; ++si) {
            for (const auto& r : bank.sounds[si].refs) {
                if (r.sample_no < 0 ||
                    static_cast<std::size_t>(r.sample_no) >= bank.samples.size()) {
                    continue;
                }
                const std::string resolved = resolve_sound_sample_path(
                    vfs, "Modules/Phantasy Calradia", "",
                    bank.samples[static_cast<std::size_t>(r.sample_no)].file);
                if (!resolved.empty() && audio_detail::ends_with_ci(resolved, ".mp3")) {
                    mp3_snd = static_cast<std::int32_t>(si);
                    break;
                }
            }
        }
        std::int32_t mp3_trk = -1;
        for (std::size_t ti = 0; ti < tracks.size(); ++ti) {
            const std::string resolved =
                resolve_music_track_path(vfs, "Modules/Phantasy Calradia", tracks[ti].file);
            if (!resolved.empty() && audio_detail::ends_with_ci(resolved, ".mp3")) {
                mp3_trk = static_cast<std::int32_t>(ti);
                break;
            }
        }
        expect(mp3_snd >= 0 && mp3_trk >= 0,
               "located retail Phantasy Calradia MP3 SFX and MP3 music track");

        const std::int32_t ch_mp3 = bridge.play_sound_2d(mp3_snd, 0U);
        bridge.play_track(mp3_trk, 2);
        CaptureAudioDevice cap;
        cap.open(AudioDeviceSpec{48000U, 2U});
        bridge.mixer().pump(cap, 24000U);
        expect(ch_mp3 >= 0 && bridge.mixer().active_music_track() == mp3_trk &&
                   cap.rms_left() > 0.001 && cap.rms_right() > 0.001,
               "retail Phantasy Calradia MP3 SFX and MP3 music stream mix cleanly");
    }
}


// Builds a 16-bit PCM WAV from explicit interleaved samples; `data_size_field`
// overrides the declared data chunk size (0 = actual size).
std::vector<std::uint8_t> make_wav_i16(std::uint16_t channels, std::uint32_t sample_rate,
                                       const std::vector<std::int16_t>& interleaved,
                                       std::uint32_t data_size_field = 0U) {
    std::vector<std::uint8_t> out;
    push_ascii4(out, "RIFF");
    push_u32_le(out, 0xFFFFFFFFU);
    push_ascii4(out, "WAVE");
    push_ascii4(out, "fmt ");
    push_u32_le(out, 16U);
    push_u16_le(out, 1U);
    push_u16_le(out, channels);
    push_u32_le(out, sample_rate);
    push_u32_le(out, sample_rate * 2U * channels);
    push_u16_le(out, static_cast<std::uint16_t>(2U * channels));
    push_u16_le(out, 16U);
    push_ascii4(out, "data");
    const auto actual = static_cast<std::uint32_t>(interleaved.size() * 2U);
    push_u32_le(out, data_size_field != 0U ? data_size_field : actual);
    for (std::int16_t v : interleaved) {
        push_u16_le(out, static_cast<std::uint16_t>(v));
    }
    return out;
}

std::shared_ptr<wse::asset::AudioPcmBuffer> make_const_pcm(std::uint32_t sr, std::size_t frames,
                                                           float value) {
    auto buf = std::make_shared<wse::asset::AudioPcmBuffer>();
    buf->sample_rate = sr;
    buf->channels = 1U;
    buf->samples.assign(frames, value);
    return buf;
}

[[nodiscard]] float peak_abs(const std::vector<float>& v) noexcept {
    float p = 0.0f;
    for (float s : v) {
        p = std::max(p, std::fabs(s));
    }
    return p;
}

// Regression tests for the P6 review findings (crash, UB, scheduler, decoder fixes).
void test_review_regressions() {
    using namespace wse::asset;
    using namespace wse::platform;

    // 1. An end callback that stops every channel mid-mix must not leave the mix
    //    loop dereferencing a released sample (was a null shared_ptr deref).
    {
        AudioConfig cfg{};
        cfg.max_virtual_channels = 4;
        WseAudioMixer m(cfg);
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 10U, 0.1f)},
                           MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 48000U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}},
                           MixerSoundDef{0U, {MixerSoundVariant{1, 0U}}}});
        m.play_sound_2d(0);
        m.play_sound_2d(1);
        m.play_sound_2d(1);
        int ends = 0;
        m.set_channel_end_callback([&](std::int32_t) {
            ++ends;
            m.stop_all_sounds(true);
        });
        std::vector<float> out;
        m.mix_frames(256U, out);
        expect(ends == 3 && m.active_sound_channel_count() == 0U,
               "end callback may stop all sounds during a mix without a crash");
    }

    // 2. Eviction: the callback runs after the new sound owns the evicted slot.
    {
        AudioConfig cfg{};
        cfg.max_virtual_channels = 1;
        WseAudioMixer m(cfg);
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 4800U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        const std::int32_t first = m.play_sound_2d(0);
        bool owner_visible = false;
        m.set_channel_end_callback([&](std::int32_t ch) {
            owner_visible = m.is_channel_playing(ch);
        });
        const std::int32_t second = m.play_sound_2d(0);
        expect(first == 0 && second == 0 && owner_visible,
               "eviction callback fires after the new sound owns the slot");
    }

    // 3. Changing volume keeps sounds playing; shrinking the channel pool ends
    //    the dropped channels through the callback.
    {
        WseAudioMixer m;
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 480000U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        std::vector<std::int32_t> ended;
        m.set_channel_end_callback([&](std::int32_t ch) { ended.push_back(ch); });
        const std::int32_t a = m.play_sound_2d(0);
        const std::int32_t b = m.play_sound_2d(0);
        AudioConfig cfg = m.config();
        cfg.sound_volume = 0.8f;
        m.set_config(cfg);
        expect(m.is_channel_playing(a) && m.is_channel_playing(b) && ended.empty(),
               "set_config volume change keeps playing sounds alive");
        cfg.max_virtual_channels = 1;
        m.set_config(cfg);
        expect(m.is_channel_playing(a) && !m.is_channel_playing(b) && ended.size() == 1U &&
                   ended[0] == b,
               "shrinking max_virtual_channels ends dropped channels via callback");
    }

    // 4. Voice pitch hash (`0x140057170`): wrapping arithmetic on extreme inputs,
    //    exact `0.3f` scale and asymmetric `1/(1-delta)` negative branch, and deadband.
    {
        const float hi = compute_agent_voice_pitch(std::numeric_limits<std::int32_t>::max(),
                                                   std::numeric_limits<std::int32_t>::max(),
                                                   std::numeric_limits<std::int32_t>::max());
        const float lo = compute_agent_voice_pitch(std::numeric_limits<std::int32_t>::min(), -1,
                                                   std::numeric_limits<std::int32_t>::min());
        const float typical = compute_agent_voice_pitch(5, 100, 12345);
        const float negative_branch = compute_agent_voice_pitch(0, 1, -1);
        // Negative signed sum produces negative `%` remainder (acc = -31, delta ~= -0.1607);
        // retail 0x140057170 has no +-0.15f clamp, giving ~0.8615659 rather than 1/1.15.
        const float negative_unclamped = compute_agent_voice_pitch(-29, 0, -1);
        expect(close(hi, 1.0f, 1e-6f) && close(lo, 0.9795872f, 1e-5f) &&
                   close(typical, 1.1093571f, 1e-5f) &&
                   close(negative_branch, 0.8890023f, 1e-5f) &&
                   close(negative_unclamped, 0.8615659f, 1e-5f),
               "compute_agent_voice_pitch matches retail 0.3f / 1/(1-delta) pitch curve");
    }

    // 5. Random start position reaches the whole sample, not just frame < 32767.
    {
        WseAudioMixer m;
        m.set_seed(9U);
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 960000U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        std::int32_t best = 0;
        for (int i = 0; i < 200; ++i) {
            m.stop_all_sounds(true);
            best = std::max(best, m.get_sound_progress(m.play_sound_2d(0, kSoundFlagRandomStartPos)));
        }
        expect(best >= 90, "random start position spans the whole 20 s sample");
    }

    // 6. Requesting the track that is already playing keeps it playing.
    for (const std::int32_t option : {1, 2}) {
        AudioConfig cfg{};
        cfg.music_volume = 1.0f;
        WseAudioMixer m(cfg);
        m.load_music_tracks({MixerMusicTrack{"a", normalize_music_track_flags(0U, 0U),
                                             make_const_pcm(48000U, 48000U * 60U, 0.25f)}});
        m.play_track(0, 0);
        m.update(0.0);
        m.play_track(0, option);
        m.update(1.0);
        m.update(7.0);
        std::vector<float> out;
        m.mix_frames(480U, out);
        expect(m.active_music_track() == 0 && !m.is_music_fading() && peak_abs(out) > 0.2f,
               option == 1 ? "play_track(current, 1) keeps the current track playing"
                           : "play_track(current, 2) keeps the current track playing");
    }

    // 7. Streamed music interpolates across chunk boundaries exactly like the
    //    predecoded path (44.1 kHz -> 48 kHz, several 2048-frame refills).
    {
        auto pcm = std::make_shared<AudioPcmBuffer>();
        pcm->sample_rate = 44100U;
        pcm->channels = 1U;
        for (int i = 0; i < 44100; ++i) {
            pcm->samples.push_back(0.5f * std::sin(static_cast<float>(i) * 0.05f));
        }
        const auto render = [&](bool stream) {
            AudioConfig cfg{};
            cfg.music_volume = 1.0f;
            WseAudioMixer m(cfg);
            m.load_music_tracks({MixerMusicTrack{"t", normalize_music_track_flags(0U, 0U),
                                                 stream ? nullptr : pcm}});
            m.set_music_stream_opener([&](std::size_t) -> std::unique_ptr<AudioStreamDecoder> {
                return std::make_unique<PcmBufferStreamDecoder>(*pcm);
            });
            m.play_track(0, 0);
            m.update(0.0);
            std::vector<float> all;
            std::vector<float> block;
            for (int k = 0; k < 20; ++k) {
                m.mix_frames(480U, block);
                all.insert(all.end(), block.begin(), block.end());
            }
            return all;
        };
        const auto a = render(false);
        const auto b = render(true);
        float max_diff = 0.0f;
        for (std::size_t i = 0; i < a.size() && i < b.size(); ++i) {
            max_diff = std::max(max_diff, std::fabs(a[i] - b[i]));
        }
        expect(a.size() == b.size() && max_diff < 1e-5f,
               "streamed music matches predecoded output across chunk boundaries");
    }

    // 8. Looping samples interpolate across the loop point (frame 1 -> frame 0).
    {
        auto two = std::make_shared<AudioPcmBuffer>();
        two->sample_rate = 24000U;  // step 0.5 at 48 kHz
        two->channels = 1U;
        two->samples = {0.6f, -0.6f};
        WseAudioMixer m;
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D | kSoundFlagLooping, two}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        m.play_sound_2d(0);
        std::vector<float> out;
        m.mix_frames(4U, out);
        const float g = decode_sound_volume(0U);
        expect(close(out[0], 0.6f * g, 1e-5f) && close(out[2], 0.0f, 1e-5f) &&
                   close(out[4], -0.6f * g, 1e-5f) && close(out[6], 0.0f, 1e-5f),
               "looping sample wraps interpolation from the last frame to frame 0");
    }

    // 9. Surround fold: centre to both sides, LFE dropped, symmetric 3-channel.
    {
        AudioPcmBuffer pcm{};
        std::vector<std::int16_t> centre;
        std::vector<std::int16_t> lfe;
        std::vector<std::int16_t> three;
        for (int f = 0; f < 16; ++f) {
            for (int c = 0; c < 6; ++c) {
                centre.push_back(c == 2 ? 16384 : 0);
                lfe.push_back(c == 3 ? 16384 : 0);
            }
            three.insert(three.end(), {9000, 9000, 9000});
        }
        const bool c_ok = decode_wav_pcm(make_wav_i16(6U, 44100U, centre), pcm) &&
                          pcm.channels == 2U && pcm.samples[0] > 0.1f &&
                          close(pcm.samples[0], pcm.samples[1], 1e-6f);
        const bool lfe_ok = decode_wav_pcm(make_wav_i16(6U, 44100U, lfe), pcm) &&
                            peak_abs(pcm.samples) == 0.0f;
        const bool three_ok = decode_wav_pcm(make_wav_i16(3U, 44100U, three), pcm) &&
                              close(pcm.samples[0], pcm.samples[1], 1e-6f);
        expect(c_ok && lfe_ok && three_ok,
               "5.1 / 3.0 fold sends centre to both sides and drops LFE");
    }

    // 10. Streamed WAV writers leave the data size at 0xFFFFFFFF.
    {
        AudioPcmBuffer pcm{};
        std::string err;
        const std::vector<std::int16_t> mono(1000, 1000);
        expect(decode_wav_pcm(make_wav_i16(1U, 44100U, mono, 0xFFFFFFFFU), pcm, &err) &&
                   pcm.frame_count() == 1000U,
               "WAV with unpatched 0xFFFFFFFF data size decodes the frames present");
    }

    // 11. Invalid distances from config are sanitised (std::clamp lo > hi was UB).
    {
        AudioConfig cfg{};
        cfg.min_distance_3d = 50.0f;
        cfg.max_distance_3d = 10.0f;
        cfg.sound_volume = std::numeric_limits<float>::quiet_NaN();
        WseAudioMixer m(cfg);
        m.load_sound_bank({MixerSoundSample{0U, make_const_pcm(48000U, 4800U, 0.3f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        m.play_sound_at_position(0, wse::core::WseVec4{20.0f, 0.0f, 0.0f, 0.0f});
        std::vector<float> out;
        m.mix_frames(64U, out);
        expect(m.config().max_distance_3d >= m.config().min_distance_3d &&
                   std::isfinite(m.config().sound_volume) && std::isfinite(out[1]),
               "min > max 3D distance and NaN volume are sanitised");
    }

    // 12. A clock that jumps backwards (new mission timer) does not stall music.
    {
        WseAudioMixer m;
        m.load_music_tracks({
            MixerMusicTrack{"a", normalize_music_track_flags(0U, 0U),
                            make_const_pcm(48000U, 48000U * 60U, 0.2f)},
            MixerMusicTrack{"b", normalize_music_track_flags(0U, 0U),
                            make_const_pcm(48000U, 48000U * 60U, 0.2f)},
        });
        m.play_track(0, 0);
        m.update(100.0);
        m.update(0.0);  // clock reset
        m.play_track(1, 1);
        m.update(0.6);
        const bool fading = m.is_music_fading();
        m.update(6.0);
        expect(fading && m.active_music_track() == 1,
               "music scheduler keeps running after the clock jumps backwards");
    }

    // 13. last_sound_channel reports -1 after a failed play.
    {
        WseAudioMixer m;
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 480U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        const std::int32_t ok = m.play_sound_2d(0);
        const bool ok_reported = m.last_sound_channel() == ok;
        m.play_sound_2d(7);
        expect(ok >= 0 && ok_reported && m.last_sound_channel() == -1,
               "last_sound_channel is -1 after a failed play");
    }

    // 14. Soft clipper, pacing helper, and MP3 / AAC sync disambiguation.
    {
        expect(soft_clip_sample(0.5f) == 0.5f && soft_clip_sample(4.0f) <= 1.0f &&
                   soft_clip_sample(0.9f) < 0.9f && soft_clip_sample(0.9f) > 0.85f &&
                   soft_clip_sample(-4.0f) >= -1.0f &&
                   soft_clip_sample(std::numeric_limits<float>::quiet_NaN()) == 0.0f,
               "soft clipper is identity below the knee, bounded above, NaN-safe");
        WseAudioMixer m;
        NullAudioDevice dev;
        dev.open(AudioDeviceSpec{48000U, 2U});
        expect(m.pump_to_latency(dev, 1500U, 512U) == 1500U && dev.submitted_frames() == 1500U,
               "pump_to_latency tops the queue up in bounded blocks");
        const std::array<std::uint8_t, 4> adts = {0xFFU, 0xF1U, 0x50U, 0x80U};
        const std::array<std::uint8_t, 4> mpeg = {0xFFU, 0xFBU, 0x90U, 0xC4U};
        expect(detect_audio_codec(adts, "") == AudioCodecKind::unknown &&
                   detect_audio_codec(mpeg, "") == AudioCodecKind::mp3,
               "AAC ADTS sync word is not mistaken for MP3");
    }

    // 15. Re-entrant callback starting a sound into a sibling slot that ended in
    //     the same flush must cancel the sibling slot's stale queued end notification.
    {
        AudioConfig cfg{};
        cfg.max_virtual_channels = 2;
        WseAudioMixer m(cfg);
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 16U, 0.1f)},
                           MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 48000U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}},
                           MixerSoundDef{0U, {MixerSoundVariant{1, 0U}}}});
        const std::int32_t c0 = m.play_sound_2d(0);
        const std::int32_t c1 = m.play_sound_2d(0);
        expect(c0 == 0 && c1 == 1, "two sibling channels start");
        std::vector<std::int32_t> fired;
        std::int32_t restarted_ch = -1;
        bool stale_wipe_happened = false;
        m.set_channel_end_callback([&](std::int32_t ch) {
            fired.push_back(ch);
            if (ch == c0 && restarted_ch < 0) {
                // Both short sounds ended in this mix, so ends for c0 and c1 are queued.
                // While c0's end is delivered, refill slot 0 and then slot 1 (still
                // inactive, its end not yet delivered): c1's queued end must be dropped.
                (void)m.play_sound_2d(1);           // claims slot 0
                restarted_ch = m.play_sound_2d(1);  // claims slot 1
            } else if (ch == restarted_ch) {
                stale_wipe_happened = true;
            }
        });
        std::vector<float> out;
        m.mix_frames(256U, out);
        expect(restarted_ch == c1 && fired.size() == 1U && fired[0] == c0 &&
                   !stale_wipe_happened && m.is_channel_playing(c1) &&
                   m.active_sound_channel_count() == 2U,
               "re-entrant play into ended sibling slot suppresses stale queued end callback");
    }

    // 16. A sound started from an end callback never evicts: otherwise the evicted
    //     sound's queued end (channel number only) would be delivered after the
    //     caller had recorded that channel as its own.
    {
        AudioConfig cfg{};
        cfg.max_virtual_channels = 2;
        WseAudioMixer m(cfg);
        m.load_sound_bank({MixerSoundSample{kSoundFlag2D, make_const_pcm(48000U, 48000U, 0.1f)}},
                          {MixerSoundDef{0U, {MixerSoundVariant{0, 0U}}}});
        const std::int32_t c0 = m.play_sound_2d(0);
        const std::int32_t c1 = m.play_sound_2d(0);
        std::vector<std::int32_t> fired;
        std::int32_t refill = -2;
        std::int32_t evict_attempt = -2;
        m.set_channel_end_callback([&](std::int32_t ch) {
            fired.push_back(ch);
            if (ch == c0 && refill == -2) {
                refill = m.play_sound_2d(0);         // free slot 0: allowed
                evict_attempt = m.play_sound_2d(0);  // all busy: must not evict c1
            }
        });
        m.stop_sound(c0);
        expect(c0 == 0 && c1 == 1 && refill == c0 && evict_attempt == -1 &&
                   fired.size() == 1U && m.is_channel_playing(c1) &&
                   m.active_sound_channel_count() == 2U,
               "play from a channel-end callback takes free slots only and never evicts");

        // Outside a callback, eviction still works as before.
        const std::int32_t outside = m.play_sound_2d(0);
        expect(outside >= 0 && fired.size() == 2U, "eviction outside callbacks is unchanged");
    }
}

// Opcode-level regression tests: replayed prop / looping agent sounds do not
// leak channels, and agent sounds follow the agent.
void test_review_regressions_vm() {
    using namespace wse::asset;
    using namespace wse::game;
    using namespace wse::platform;

    MemoryVfs vfs;
    std::vector<std::int16_t> tone;
    for (int i = 0; i < 48000; ++i) {
        tone.push_back(static_cast<std::int16_t>(8000.0f * std::sin(static_cast<float>(i) * 0.06f)));
    }
    vfs.write_file("Modules/T/Sounds/hum.wav", make_wav_i16(1U, 48000U, tone));
    vfs.write_file("Modules/T/Sounds/bad.wav", std::vector<std::uint8_t>{'R', 'I', 'F', 'F'});
    LoadedSoundBank bank{};
    bank.version = 3;
    bank.samples = {LoadedSoundSample{"hum.wav", kSoundFlagLooping},
                    LoadedSoundSample{"bad.wav", 0U}};
    bank.sounds = {LoadedSound{"snd_hum", kSoundFlagLooping, {LoadedSoundRef{0, 0U}}},
                   LoadedSound{"snd_bad", 0U, {LoadedSoundRef{1, 0U}}}};

    wse::vm::VmContext ctx{};
    auto& w = ctx.world;
    w.has_game = true;
    w.m5.num_sounds = 2;
    w.misc.num_sound_channels = 64;
    w.agents.resize(1);
    w.agents[0].active = true;
    w.agents[0].troop_no = 3;
    w.agents[0].position.o = {10.0f, 0.0f, 0.0f, 0.0f};
    w.mission_objects.resize(1);
    w.mission_objects[0].active = true;

    WorldAudioBridge bridge;
    (void)bridge.initialize(vfs, w, bank, {}, "Modules/T", "", nullptr, false);

    expect(run_stmt(ctx, 1881U, {0, 0}) && run_stmt(ctx, 1881U, {0, 0}) &&
               bridge.mixer().active_sound_channel_count() == 1U &&
               w.mission_objects[0].m5.sound_channel >= 0 &&
               w.mission_objects[0].m5.active_sound_no == 0 &&
               w.mission_objects[0].m5.sound_playing,
           "1881 replaying a looping prop sound replaces its channel instead of leaking it");
    expect(run_stmt(ctx, 1882U, {0}) && bridge.mixer().active_sound_channel_count() == 0U,
           "1882 stops the prop's only channel");

    expect(run_stmt(ctx, 1750U, {0, 0}) && run_stmt(ctx, 1750U, {0, 0}) &&
               bridge.mixer().active_sound_channel_count() == 1U,
           "1750 replaying a looping agent sound does not orphan the previous channel");

    // Agent sound follows the agent from +X (right) to -X (left).
    CaptureAudioDevice right;
    right.open(AudioDeviceSpec{48000U, 2U});
    bridge.mixer().pump(right, 4800U);
    w.agents[0].position.o = {-10.0f, 0.0f, 0.0f, 0.0f};
    bridge.update(0.1);
    CaptureAudioDevice left;
    left.open(AudioDeviceSpec{48000U, 2U});
    bridge.mixer().pump(left, 4800U);  // first block ramps from the old pan
    left.clear();
    bridge.mixer().pump(left, 4800U);
    expect(right.rms_right() > right.rms_left() * 3.0 && left.rms_left() > left.rms_right() * 3.0,
           "WorldAudioBridge::update moves an agent's channel with the agent");

    // A sample that fails to decode is reported as unplayable on every attempt.
    expect(run_stmt(ctx, 600U, {1, 0}) && w.music.last_sound_channel == -1 &&
               run_stmt(ctx, 600U, {1, 0}) && w.music.last_sound_channel == -1 &&
               run_stmt(ctx, 615U, {reg(0)}) && ctx.registers[0] == -1,
           "undecodable sample fails consistently and 615 reports -1");
}

// P6 review item 2: the mixer runs on a worker thread behind one mutex.
void test_audio_mixer_worker_thread() {
    using wse::platform::AudioMixerWorker;
    // A device that drains as the test "plays" frames (thread-safe counters).
    class DrainingDevice final : public wse::platform::AudioDevice {
    public:
        bool open(const wse::platform::AudioDeviceSpec& s) override {
            spec_ = s;
            open_ = true;
            return true;
        }
        void close() override { open_ = false; }
        [[nodiscard]] bool is_open() const noexcept override { return open_; }
        [[nodiscard]] wse::platform::AudioDeviceSpec spec() const noexcept override { return spec_; }
        bool submit_frames(std::span<const float> pcm) override {
            submitted_.fetch_add(pcm.size() / 2U);
            return true;
        }
        [[nodiscard]] std::size_t queued_frames() const noexcept override {
            const std::size_t sub = submitted_.load();
            const std::size_t played = played_.load();
            return sub > played ? sub - played : 0U;
        }
        void clear() override { played_.store(submitted_.load()); }
        void set_paused(bool p) override { paused_.store(p); }
        [[nodiscard]] const char* backend_name() const noexcept override { return "draining"; }
        void play(std::size_t frames) { played_.fetch_add(frames); }
        [[nodiscard]] bool paused() const noexcept { return paused_.load(); }

    private:
        wse::platform::AudioDeviceSpec spec_{};
        bool open_ = false;
        std::atomic<std::size_t> submitted_{0U};
        std::atomic<std::size_t> played_{0U};
        std::atomic<bool> paused_{false};
    };

    wse::platform::WseAudioMixer mixer{};
    DrainingDevice dev{};
    AudioMixerWorker::Options opt{};
    opt.target_queued_frames = 1024U;
    opt.max_block_frames = 256U;
    opt.period = std::chrono::microseconds{500};
    {
        AudioMixerWorker closed(mixer, dev, opt);
        expect(!closed.start() && !closed.running(), "the worker does not start on a closed device");
    }
    expect(dev.open(wse::platform::AudioDeviceSpec{48000U, 2U}), "draining device opens");
    AudioMixerWorker worker(mixer, dev, opt);
    expect(worker.pump_once() == 1024U && worker.pump_once() == 0U && dev.queued_frames() == 1024U,
           "pump_once tops the queue up to the target latency and no further");

    worker.set_paused(true);
    dev.play(512U);
    expect(dev.paused() && worker.paused() && worker.pump_once() == 0U,
           "a paused worker pauses the device and submits nothing");
    worker.set_paused(false);
    expect(!dev.paused() && worker.pump_once() == 512U, "resuming tops the queue up again");

    expect(worker.start() && worker.running() && worker.start(), "the worker thread starts (idempotent)");
    const std::uint64_t base = worker.frames_submitted();
    std::size_t game_calls = 0U;
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    while (worker.frames_submitted() < base + 8192U && std::chrono::steady_clock::now() < deadline) {
        dev.play(256U);
        // Game-thread mixer calls under the worker's mutex while it mixes.
        game_calls += worker.with_mixer([](wse::platform::WseAudioMixer& m) {
            return m.output_sample_rate() == 48000U ? 1U : 0U;
        });
        std::this_thread::sleep_for(std::chrono::microseconds(200));
    }
    worker.stop();
    expect(!worker.running() && worker.frames_submitted() >= base + 8192U && game_calls > 0U,
           "the worker thread keeps the draining device topped up while the game thread uses the mixer");
    expect(dev.queued_frames() <= 1024U, "the worker never queues past the target latency");
    worker.stop();
    expect(!worker.running(), "stop is idempotent");
}

}  // namespace

namespace wse::selftest {

int run_audio_suite(const char* fixture_path) {
    (void)fixture_path;
    g_failures = 0;
    test_wav_decoding_and_path_resolution();
    test_ogg_and_mp3_codecs_and_streaming();
    test_mixer_3d_panning_lod_priority_occlusion_and_determinism();
    test_mixer_music_scheduler_and_streaming();
    test_vm_and_world_audio_bridge_integration();
    test_review_regressions();
    test_review_regressions_vm();
    test_audio_mixer_worker_thread();
    test_retail_warband_audio_gate();

    if (g_failures == 0) {
        std::printf("audio tests: PASS\n");
        return 0;
    }
    std::printf("audio tests: FAIL (%d)\n", g_failures);
    return 1;
}

}  // namespace wse::selftest

#ifndef WSE_SELF_TEST_LIBRARY
int main() {
    return wse::selftest::run_audio_suite(nullptr);
}
#endif
