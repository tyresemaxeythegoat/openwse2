// JNI helpers shared by the SAF backends (`saf_vfs_jni.cpp` in libwse2, `r4_jni.cpp` in
// libwse_r0): exception clearing, UTF-8 <-> jstring through UTF-16, and the child records
// `WseSafBridge.listChildren` returns ("dir\tsize\tdocumentId\tdisplayName").

#pragma once

#include <jni.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "wse/platform/saf_vfs.h"

namespace wse::platform::android_jni {

inline bool clear_if_exception(JNIEnv* env) noexcept {
    if (env == nullptr) {
        return true;
    }
    if (env->ExceptionCheck() == JNI_TRUE) {
        env->ExceptionClear();
        return true;
    }
    return false;
}

// JNI's *StringUTF* functions use "modified UTF-8" (NUL as C0 80, characters
// outside the BMP as two 3-byte surrogates), not UTF-8. SAF display names and
// paths can hold any Unicode, so convert through UTF-16 explicitly.
inline std::string jstring_to_utf8(JNIEnv* env, jstring jstr) {
    if (env == nullptr || jstr == nullptr) {
        return {};
    }
    const jsize len = env->GetStringLength(jstr);
    const jchar* chars = env->GetStringChars(jstr, nullptr);
    if (chars == nullptr) {
        (void)clear_if_exception(env);
        return {};
    }
    std::string out;
    out.reserve(static_cast<std::size_t>(len));
    const auto put = [&out](std::uint32_t cp) {
        if (cp < 0x80U) {
            out.push_back(static_cast<char>(cp));
        } else if (cp < 0x800U) {
            out.push_back(static_cast<char>(0xC0U | (cp >> 6U)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else if (cp < 0x10000U) {
            out.push_back(static_cast<char>(0xE0U | (cp >> 12U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        } else {
            out.push_back(static_cast<char>(0xF0U | (cp >> 18U)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 12U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | ((cp >> 6U) & 0x3FU)));
            out.push_back(static_cast<char>(0x80U | (cp & 0x3FU)));
        }
    };
    for (jsize i = 0; i < len; ++i) {
        std::uint32_t cp = chars[i];
        if (cp >= 0xD800U && cp <= 0xDBFFU && i + 1 < len && chars[i + 1] >= 0xDC00U &&
            chars[i + 1] <= 0xDFFFU) {
            cp = 0x10000U + ((cp - 0xD800U) << 10U) + (static_cast<std::uint32_t>(chars[i + 1]) - 0xDC00U);
            ++i;
        } else if (cp >= 0xD800U && cp <= 0xDFFFU) {
            cp = 0xFFFDU;  // Unpaired surrogate.
        }
        put(cp);
    }
    env->ReleaseStringChars(jstr, chars);
    return out;
}

inline jstring utf8_to_jstring(JNIEnv* env, std::string_view sv) {
    if (env == nullptr) {
        return nullptr;
    }
    std::u16string utf16;
    utf16.reserve(sv.size());
    for (std::size_t i = 0; i < sv.size();) {
        const auto b0 = static_cast<unsigned char>(sv[i]);
        std::uint32_t cp = 0xFFFDU;
        std::size_t n = 1;
        const auto cont = [&](std::size_t k) {
            return i + k < sv.size() && (static_cast<unsigned char>(sv[i + k]) & 0xC0U) == 0x80U;
        };
        const auto bits = [&](std::size_t k) {
            return static_cast<std::uint32_t>(static_cast<unsigned char>(sv[i + k]) & 0x3FU);
        };
        if (b0 < 0x80U) {
            cp = b0;
        } else if ((b0 & 0xE0U) == 0xC0U && cont(1)) {
            cp = ((b0 & 0x1FU) << 6U) | bits(1);
            n = 2;
            if (cp < 0x80U) cp = 0xFFFDU;
        } else if ((b0 & 0xF0U) == 0xE0U && cont(1) && cont(2)) {
            cp = ((b0 & 0x0FU) << 12U) | (bits(1) << 6U) | bits(2);
            n = 3;
            if (cp < 0x800U || (cp >= 0xD800U && cp <= 0xDFFFU)) cp = 0xFFFDU;
        } else if ((b0 & 0xF8U) == 0xF0U && cont(1) && cont(2) && cont(3)) {
            cp = ((b0 & 0x07U) << 18U) | (bits(1) << 12U) | (bits(2) << 6U) | bits(3);
            n = 4;
            if (cp < 0x10000U || cp > 0x10FFFFU) cp = 0xFFFDU;
        }
        i += n;
        if (cp >= 0x10000U) {
            cp -= 0x10000U;
            utf16.push_back(static_cast<char16_t>(0xD800U + (cp >> 10U)));
            utf16.push_back(static_cast<char16_t>(0xDC00U + (cp & 0x3FFU)));
        } else {
            utf16.push_back(static_cast<char16_t>(cp));
        }
    }
    static_assert(sizeof(char16_t) == sizeof(jchar), "jchar must be UTF-16");
    jstring result = env->NewString(reinterpret_cast<const jchar*>(utf16.data()),
                                    static_cast<jsize>(utf16.size()));
    if (clear_if_exception(env)) {
        return nullptr;
    }
    return result;
}

inline bool parse_child_record(std::string_view line,
                                      wse::platform::SafChildEntry& out) {
    const std::size_t t1 = line.find('\t');
    if (t1 == std::string_view::npos) {
        return false;
    }
    const std::size_t t2 = line.find('\t', t1 + 1U);
    if (t2 == std::string_view::npos) {
        return false;
    }
    const std::size_t t3 = line.find('\t', t2 + 1U);
    if (t3 == std::string_view::npos) {
        return false;
    }

    const std::string_view dir_sv = line.substr(0, t1);
    const std::string_view size_sv = line.substr(t1 + 1U, t2 - (t1 + 1U));
    const std::string_view id_sv = line.substr(t2 + 1U, t3 - (t2 + 1U));
    const std::string_view name_sv = line.substr(t3 + 1U);
    if (id_sv.empty() || name_sv.empty()) {
        return false;
    }

    out.is_directory = (dir_sv == "1");
    out.size_bytes = 0;
    std::uint64_t parsed_size = 0;
    const auto [ptr, ec] =
        std::from_chars(size_sv.data(), size_sv.data() + size_sv.size(), parsed_size);
    if (ec == std::errc{} && ptr == size_sv.data() + size_sv.size()) {
        out.size_bytes = parsed_size;
    }
    out.document_id = std::string(id_sv);
    out.display_name = std::string(name_sv);
    return true;
}

}  // namespace wse::platform::android_jni
