// Platform seam: Android JNI backend and launcher bridge for SafVfs
// (docs/PLATFORM_PLAN.md, slice P2).
//
// Compiled into libwse2.so only when WSE_BUILD_ANDROID_APP is ON.

#include <android/api-level.h>
#include <jni.h>

#include <unistd.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "driver_loader.h"
#include "saf_game_tree.h"
#include "wse/asset/bmp.h"
#include "wse/core/json.h"
#include "wse/game/module_loader_common.h"
#include "wse/platform/clock.h"
#include "wse/platform/performance.h"
#include "wse/platform/driver_package.h"
#include "wse/platform/rhi.h"
#include "wse/platform/saf_vfs.h"
#include "wse/platform/ui_game_shell.h"

namespace {

[[nodiscard]] bool clear_if_exception(JNIEnv* env) noexcept {
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
[[nodiscard]] std::string jstring_to_utf8(JNIEnv* env, jstring jstr) {
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

[[nodiscard]] jstring utf8_to_jstring(JNIEnv* env, std::string_view sv) {
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

[[nodiscard]] bool parse_child_record(std::string_view line,
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

struct ActiveJniBinding {
    JNIEnv* env = nullptr;
    jobject bridge = nullptr;
    jmethodID mid_tree_key = nullptr;
    jmethodID mid_root = nullptr;
    jmethodID mid_list = nullptr;
    jmethodID mid_read = nullptr;
    jmethodID mid_write = nullptr;
    jmethodID mid_mkdir = nullptr;
    jmethodID mid_delete = nullptr;
    jmethodID mid_rename = nullptr;
    jmethodID mid_move = nullptr;
};

class AndroidJniSafBackend final : public wse::platform::SafTreeBackend {
public:
    void set_binding(const ActiveJniBinding* binding) noexcept { binding_ = binding; }

    [[nodiscard]] std::string root_document_id() const override {
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_root == nullptr) {
            return {};
        }
        auto jroot = static_cast<jstring>(
            binding_->env->CallObjectMethod(binding_->bridge, binding_->mid_root));
        if (clear_if_exception(binding_->env) || jroot == nullptr) {
            return {};
        }
        std::string out = jstring_to_utf8(binding_->env, jroot);
        binding_->env->DeleteLocalRef(jroot);
        return out;
    }

    [[nodiscard]] bool list_children(std::string_view parent_document_id,
                                     std::vector<wse::platform::SafChildEntry>& out_children) override {
        out_children.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_list == nullptr) {
            return false;
        }
        jstring jparent = utf8_to_jstring(binding_->env, parent_document_id);
        if (jparent == nullptr) {
            return false;
        }
        auto jarr = static_cast<jobjectArray>(
            binding_->env->CallObjectMethod(binding_->bridge, binding_->mid_list, jparent));
        binding_->env->DeleteLocalRef(jparent);
        if (clear_if_exception(binding_->env) || jarr == nullptr) {
            return false;
        }

        const jsize count = binding_->env->GetArrayLength(jarr);
        out_children.reserve(static_cast<std::size_t>(count));
        for (jsize i = 0; i < count; ++i) {
            auto jelem = static_cast<jstring>(binding_->env->GetObjectArrayElement(jarr, i));
            if (clear_if_exception(binding_->env)) {
                binding_->env->DeleteLocalRef(jarr);
                out_children.clear();
                return false;
            }
            if (jelem != nullptr) {
                const std::string line = jstring_to_utf8(binding_->env, jelem);
                binding_->env->DeleteLocalRef(jelem);
                wse::platform::SafChildEntry entry{};
                if (parse_child_record(line, entry)) {
                    out_children.push_back(std::move(entry));
                }
            }
        }
        binding_->env->DeleteLocalRef(jarr);
        return true;
    }

    [[nodiscard]] bool read_document(std::string_view document_id,
                                     std::vector<std::uint8_t>& out) override {
        out.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_read == nullptr) {
            return false;
        }
        jstring jdoc = utf8_to_jstring(binding_->env, document_id);
        if (jdoc == nullptr) {
            return false;
        }
        auto jbytes = static_cast<jbyteArray>(
            binding_->env->CallObjectMethod(binding_->bridge, binding_->mid_read, jdoc));
        binding_->env->DeleteLocalRef(jdoc);
        if (clear_if_exception(binding_->env) || jbytes == nullptr) {
            return false;
        }

        const jsize len = binding_->env->GetArrayLength(jbytes);
        if (len > 0) {
            out.resize(static_cast<std::size_t>(len));
            binding_->env->GetByteArrayRegion(
                jbytes, 0, len, reinterpret_cast<jbyte*>(out.data()));
        }
        binding_->env->DeleteLocalRef(jbytes);
        if (clear_if_exception(binding_->env)) {
            out.clear();
            return false;
        }
        return true;
    }

    [[nodiscard]] bool write_document(std::string_view parent_document_id,
                                      std::string_view existing_document_id,
                                      std::string_view display_name,
                                      std::span<const std::uint8_t> data,
                                      std::string& out_document_id) override {
        out_document_id.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_write == nullptr) {
            return false;
        }
        if (data.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max())) {
            return false;
        }

        jstring jparent = utf8_to_jstring(binding_->env, parent_document_id);
        jstring jexisting = utf8_to_jstring(binding_->env, existing_document_id);
        jstring jname = utf8_to_jstring(binding_->env, display_name);
        const jsize len = static_cast<jsize>(data.size());
        jbyteArray jdata = binding_->env->NewByteArray(len);
        if (jparent == nullptr || jexisting == nullptr || jname == nullptr || jdata == nullptr ||
            clear_if_exception(binding_->env)) {
            if (jparent != nullptr) binding_->env->DeleteLocalRef(jparent);
            if (jexisting != nullptr) binding_->env->DeleteLocalRef(jexisting);
            if (jname != nullptr) binding_->env->DeleteLocalRef(jname);
            if (jdata != nullptr) binding_->env->DeleteLocalRef(jdata);
            return false;
        }
        if (len > 0) {
            binding_->env->SetByteArrayRegion(
                jdata, 0, len, reinterpret_cast<const jbyte*>(data.data()));
            if (clear_if_exception(binding_->env)) {
                binding_->env->DeleteLocalRef(jparent);
                binding_->env->DeleteLocalRef(jexisting);
                binding_->env->DeleteLocalRef(jname);
                binding_->env->DeleteLocalRef(jdata);
                return false;
            }
        }

        auto jres = static_cast<jstring>(binding_->env->CallObjectMethod(
            binding_->bridge, binding_->mid_write, jparent, jexisting, jname, jdata));
        binding_->env->DeleteLocalRef(jparent);
        binding_->env->DeleteLocalRef(jexisting);
        binding_->env->DeleteLocalRef(jname);
        binding_->env->DeleteLocalRef(jdata);
        if (clear_if_exception(binding_->env) || jres == nullptr) {
            return false;
        }
        out_document_id = jstring_to_utf8(binding_->env, jres);
        binding_->env->DeleteLocalRef(jres);
        return !out_document_id.empty();
    }

    [[nodiscard]] bool create_directory(std::string_view parent_document_id,
                                        std::string_view display_name,
                                        std::string& out_document_id) override {
        out_document_id.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_mkdir == nullptr) {
            return false;
        }
        jstring jparent = utf8_to_jstring(binding_->env, parent_document_id);
        jstring jname = utf8_to_jstring(binding_->env, display_name);
        if (jparent == nullptr || jname == nullptr) {
            if (jparent != nullptr) binding_->env->DeleteLocalRef(jparent);
            if (jname != nullptr) binding_->env->DeleteLocalRef(jname);
            return false;
        }
        auto jres = static_cast<jstring>(binding_->env->CallObjectMethod(
            binding_->bridge, binding_->mid_mkdir, jparent, jname));
        binding_->env->DeleteLocalRef(jparent);
        binding_->env->DeleteLocalRef(jname);
        if (clear_if_exception(binding_->env) || jres == nullptr) {
            return false;
        }
        out_document_id = jstring_to_utf8(binding_->env, jres);
        binding_->env->DeleteLocalRef(jres);
        return !out_document_id.empty();
    }

    [[nodiscard]] bool delete_document(std::string_view document_id) override {
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_delete == nullptr) {
            return false;
        }
        jstring jdoc = utf8_to_jstring(binding_->env, document_id);
        if (jdoc == nullptr) {
            return false;
        }
        const jboolean ok = binding_->env->CallBooleanMethod(
            binding_->bridge, binding_->mid_delete, jdoc);
        binding_->env->DeleteLocalRef(jdoc);
        if (clear_if_exception(binding_->env)) {
            return false;
        }
        return ok == JNI_TRUE;
    }

    // `DocumentsContract.renameDocument` through `WseSafBridge.renameDocument`.
    [[nodiscard]] bool rename_document(std::string_view document_id,
                                       std::string_view new_name,
                                       std::string& out_document_id) override {
        out_document_id.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_rename == nullptr) {
            return false;
        }
        JNIEnv* env = binding_->env;
        jstring jdoc = utf8_to_jstring(env, document_id);
        jstring jname = utf8_to_jstring(env, new_name);
        if (jdoc == nullptr || jname == nullptr) {
            if (jdoc != nullptr) env->DeleteLocalRef(jdoc);
            if (jname != nullptr) env->DeleteLocalRef(jname);
            return false;
        }
        auto jres = static_cast<jstring>(
            env->CallObjectMethod(binding_->bridge, binding_->mid_rename, jdoc, jname));
        env->DeleteLocalRef(jdoc);
        env->DeleteLocalRef(jname);
        if (clear_if_exception(env) || jres == nullptr) {
            return false;
        }
        out_document_id = jstring_to_utf8(env, jres);
        env->DeleteLocalRef(jres);
        return !out_document_id.empty();
    }

    // `DocumentsContract.moveDocument` through `WseSafBridge.moveDocument`.
    [[nodiscard]] bool move_document(std::string_view document_id,
                                     std::string_view source_parent_id,
                                     std::string_view target_parent_id,
                                     std::string& out_document_id) override {
        out_document_id.clear();
        if (binding_ == nullptr || binding_->env == nullptr || binding_->bridge == nullptr ||
            binding_->mid_move == nullptr) {
            return false;
        }
        JNIEnv* env = binding_->env;
        jstring jdoc = utf8_to_jstring(env, document_id);
        jstring jsrc = utf8_to_jstring(env, source_parent_id);
        jstring jdst = utf8_to_jstring(env, target_parent_id);
        if (jdoc == nullptr || jsrc == nullptr || jdst == nullptr) {
            if (jdoc != nullptr) env->DeleteLocalRef(jdoc);
            if (jsrc != nullptr) env->DeleteLocalRef(jsrc);
            if (jdst != nullptr) env->DeleteLocalRef(jdst);
            return false;
        }
        auto jres = static_cast<jstring>(
            env->CallObjectMethod(binding_->bridge, binding_->mid_move, jdoc, jsrc, jdst));
        env->DeleteLocalRef(jdoc);
        env->DeleteLocalRef(jsrc);
        env->DeleteLocalRef(jdst);
        if (clear_if_exception(env) || jres == nullptr) {
            return false;
        }
        out_document_id = jstring_to_utf8(env, jres);
        env->DeleteLocalRef(jres);
        return !out_document_id.empty();
    }

private:
    const ActiveJniBinding* binding_ = nullptr;
};

struct SafCacheState {
    std::mutex mutex;
    std::string tree_key;
    std::shared_ptr<AndroidJniSafBackend> backend;
    std::unique_ptr<wse::platform::SafVfs> vfs;
};

SafCacheState& get_saf_cache() {
    static SafCacheState state{};
    return state;
}

class ScopedBridgeSession {
public:
    ScopedBridgeSession(JNIEnv* env, jobject thiz) {
        if (env == nullptr || thiz == nullptr) {
            return;
        }
        jclass cls = env->GetObjectClass(thiz);
        if (clear_if_exception(env) || cls == nullptr) {
            return;
        }
        binding_.env = env;
        binding_.bridge = thiz;
        binding_.mid_tree_key = env->GetMethodID(cls, "treeUriString", "()Ljava/lang/String;");
        binding_.mid_root = env->GetMethodID(cls, "rootDocumentId", "()Ljava/lang/String;");
        binding_.mid_list =
            env->GetMethodID(cls, "listChildren", "(Ljava/lang/String;)[Ljava/lang/String;");
        binding_.mid_read = env->GetMethodID(cls, "readDocument", "(Ljava/lang/String;)[B");
        binding_.mid_write = env->GetMethodID(
            cls, "writeDocument",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Ljava/lang/String;");
        binding_.mid_mkdir = env->GetMethodID(
            cls, "createDirectory", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        binding_.mid_delete = env->GetMethodID(cls, "deleteDocument", "(Ljava/lang/String;)Z");
        binding_.mid_rename = env->GetMethodID(
            cls, "renameDocument", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        binding_.mid_move = env->GetMethodID(
            cls, "moveDocument",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        // Release the local class ref immediately so sessions held by `RegisteredGameTree`
        // carry no thread-affine local JNI references across `rebind_thread`.
        env->DeleteLocalRef(cls);
        if (!binding_.mid_delete || !binding_.mid_rename || !binding_.mid_move) {
            (void)clear_if_exception(env);
        }
        if (clear_if_exception(env) || !binding_.mid_tree_key || !binding_.mid_root ||
            !binding_.mid_list || !binding_.mid_read || !binding_.mid_write ||
            !binding_.mid_mkdir || !binding_.mid_delete) {
            return;
        }
        auto jkey = static_cast<jstring>(env->CallObjectMethod(thiz, binding_.mid_tree_key));
        if (clear_if_exception(env) || jkey == nullptr) {
            return;
        }
        tree_key_ = jstring_to_utf8(env, jkey);
        env->DeleteLocalRef(jkey);
        valid_ = !tree_key_.empty();
    }

    ~ScopedBridgeSession() {
        if (bound_backend_ != nullptr) {
            bound_backend_->set_binding(nullptr);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& tree_key() const noexcept { return tree_key_; }

    void set_env(JNIEnv* env) noexcept { binding_.env = env; }

    void attach(std::shared_ptr<AndroidJniSafBackend>& backend) {
        bound_backend_ = backend.get();
        if (bound_backend_ != nullptr) {
            bound_backend_->set_binding(&binding_);
        }
    }

private:
    ActiveJniBinding binding_{};
    std::string tree_key_{};
    AndroidJniSafBackend* bound_backend_ = nullptr;
    bool valid_ = false;
};

[[nodiscard]] std::string format_scan_error(std::string_view message) {
    std::string out = "{\"ok\":false,\"error\":";
    wse::core::wse_append_json_string(out, message);
    out.push_back('}');
    return out;
}

// Attaches `session` to the cached `SafVfs` for its tree URI, rebuilding the index on a miss.
// Caller must hold `cache.mutex`.
[[nodiscard]] bool ensure_cached_tree_vfs(SafCacheState& cache,
                                          ScopedBridgeSession& session,
                                          std::string& error_out,
                                          bool* out_cache_hit = nullptr,
                                          std::int64_t* out_index_ms = nullptr) {
    if (!session.valid()) {
        error_out = "failed to bind WseSafBridge JNI methods";
        return false;
    }
    const bool cache_hit = cache.vfs && cache.backend &&
                           cache.tree_key == session.tree_key() &&
                           cache.vfs->index_complete();
    std::int64_t index_ms = 0;
    if (!cache_hit) {
        cache.tree_key = session.tree_key();
        cache.backend = std::make_shared<AndroidJniSafBackend>();
        cache.vfs = std::make_unique<wse::platform::SafVfs>(cache.backend, false);
        session.attach(cache.backend);
        wse::platform::SteadyClock clock;
        const std::int64_t i0 = clock.ticks();
        const bool ok = cache.vfs->rebuild_index();
        const std::int64_t i1 = clock.ticks();
        index_ms = ((i1 - i0) * 1000) / wse::platform::kTicksPerSecond;
        if (!ok) {
            error_out = "failed indexing SAF document tree";
            return false;
        }
    } else {
        session.attach(cache.backend);
    }
    if (out_cache_hit != nullptr) {
        *out_cache_hit = cache_hit;
    }
    if (out_index_ms != nullptr) {
        *out_index_ms = index_ms;
    }
    return true;
}

}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeScanTree(JNIEnv* env, jobject thiz) {
    SafCacheState& cache = get_saf_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);

    ScopedBridgeSession session(env, thiz);
    if (!session.valid()) {
        return utf8_to_jstring(env, format_scan_error("failed to bind WseSafBridge JNI methods"));
    }

    if (!cache.backend || cache.tree_key != session.tree_key() || !cache.vfs) {
        cache.tree_key = session.tree_key();
        cache.backend = std::make_shared<AndroidJniSafBackend>();
        cache.vfs = std::make_unique<wse::platform::SafVfs>(cache.backend, false);
    }
    session.attach(cache.backend);

    wse::platform::SteadyClock clock;
    const std::int64_t t0 = clock.ticks();
    const bool indexed = cache.vfs->rebuild_index();
    const std::int64_t t1 = clock.ticks();
    const std::int64_t index_ms = ((t1 - t0) * 1000) / wse::platform::kTicksPerSecond;

    if (!indexed) {
        return utf8_to_jstring(env, format_scan_error("failed indexing SAF document tree"));
    }

    const std::vector<wse::platform::WarbandModuleInfo> modules =
        wse::platform::scan_warband_modules(*cache.vfs);
    const std::vector<std::string> languages =
        wse::platform::list_warband_languages(*cache.vfs, "");

    std::string json = "{\"ok\":true,\"index_ms\":";
    json.append(std::to_string(index_ms));
    json.append(",\"files\":");
    json.append(std::to_string(cache.vfs->index_file_count()));
    json.append(",\"dirs\":");
    json.append(std::to_string(cache.vfs->index_dir_count()));
    json.append(",\"modules\":[");
    for (std::size_t i = 0; i < modules.size(); ++i) {
        if (i != 0) {
            json.push_back(',');
        }
        const auto& m = modules[i];
        json.append("{\"folder_name\":");
        wse::core::wse_append_json_string(json, m.folder_name);
        json.append(",\"module_path\":");
        wse::core::wse_append_json_string(json, m.module_path);
        json.append(",\"display_name\":");
        wse::core::wse_append_json_string(json, m.display_name);
        json.append(",\"has_scripts_txt\":");
        json.append(m.has_scripts_txt ? "true" : "false");
        json.append(",\"load_resource_count\":");
        json.append(std::to_string(m.load_resource_count));
        json.append(",\"load_mod_resource_count\":");
        json.append(std::to_string(m.load_mod_resource_count));
        json.append(",\"compatible_with_warband\":");
        wse::core::wse_append_json_string(json, m.compatible_with_warband);
        json.push_back('}');
    }
    json.append("],\"languages\":[");
    for (std::size_t i = 0; i < languages.size(); ++i) {
        if (i != 0U) {
            json.push_back(',');
        }
        json.push_back('"');
        json.append(wse::platform::json_escape(languages[i]));
        json.push_back('"');
    }
    json.append("]}");
    return utf8_to_jstring(env, json);
}

JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeVerifyModule(JNIEnv* env,
                                                      jobject thiz,
                                                      jstring module_rel_dir) {
    const std::string module_dir = jstring_to_utf8(env, module_rel_dir);
    if (module_dir.empty()) {
        return utf8_to_jstring(env, "ERROR: empty module directory");
    }

    SafCacheState& cache = get_saf_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);

    ScopedBridgeSession session(env, thiz);
    std::string vfs_error;
    bool cache_hit = false;
    std::int64_t index_ms = 0;
    if (!ensure_cached_tree_vfs(cache, session, vfs_error, &cache_hit, &index_ms)) {
        return utf8_to_jstring(env, "ERROR: " + vfs_error);
    }

    wse::platform::SteadyClock clock;
    const std::int64_t p0 = clock.ticks();
    const wse::platform::ModuleScriptsCheckResult res =
        wse::platform::verify_module_scripts(*cache.vfs, module_dir);
    const std::int64_t p1 = clock.ticks();
    const std::int64_t parse_ms = ((p1 - p0) * 1000) / wse::platform::kTicksPerSecond;

    if (!res.ok) {
        return utf8_to_jstring(env, "ERROR: " + res.error_message);
    }

    std::string summary = "OK: ";
    summary.append(std::to_string(res.script_count));
    summary.append(" scripts, ");
    summary.append(std::to_string(res.statement_count));
    summary.append(" statements, ");
    summary.append(std::to_string(res.mapped_script_count));
    summary.append("/65 mapped (");
    summary.append(cache_hit ? "cached index, " : ("index " + std::to_string(index_ms) + " ms, "));
    summary.append("parse " + std::to_string(parse_ms) + " ms)");
    return utf8_to_jstring(env, summary);
}

// Pre-play install check for the launcher (P10.3): returns `install_check_json` JSON.
JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeCheckModuleInstall(JNIEnv* env,
                                                            jobject thiz,
                                                            jstring module_rel_dir) {
    const auto error_json = [env](std::string_view reason) -> jstring {
        return utf8_to_jstring(
            env, wse::platform::install_check_json({std::string(reason)}));
    };

    const std::string module_dir = jstring_to_utf8(env, module_rel_dir);
    if (module_dir.empty()) {
        return error_json("empty module directory");
    }

    SafCacheState& cache = get_saf_cache();
    std::lock_guard<std::mutex> lock(cache.mutex);

    ScopedBridgeSession session(env, thiz);
    std::string vfs_error;
    if (!ensure_cached_tree_vfs(cache, session, vfs_error)) {
        return error_json(vfs_error);
    }

    return utf8_to_jstring(
        env, wse::platform::install_check_json(
                 wse::platform::find_missing_warband_files(*cache.vfs, "", module_dir)));
}

// Decodes `<rel_path>/main.bmp` (or root `main.bmp` when `rel_path` is empty) through the
// cached SAF tree VFS, downscales to `max_side` clamped to [16, 2048], computes the HSV seed
// colour, and returns `[w, h, seed, pixels...]` or null on any failure (P12.2).
JNIEXPORT jintArray JNICALL
Java_org_wse2_android_WseSafBridge_nativeLoadModuleArt(JNIEnv* env,
                                                       jobject thiz,
                                                       jstring rel_path_jstr,
                                                       jint max_side_jint) {
    if (env == nullptr || thiz == nullptr || rel_path_jstr == nullptr) {
        (void)clear_if_exception(env);
        return nullptr;
    }

    const jsize rel_len = env->GetStringLength(rel_path_jstr);
    if (clear_if_exception(env)) {
        return nullptr;
    }
    const std::string rel_path = jstring_to_utf8(env, rel_path_jstr);
    if (rel_path.empty() && rel_len != 0) {
        (void)clear_if_exception(env);
        return nullptr;
    }

    const std::string bmp_rel_path = wse::game::vfs_join_path(rel_path, "main.bmp");

    std::vector<std::uint8_t> bmp_bytes;
    {
        SafCacheState& cache = get_saf_cache();
        std::lock_guard<std::mutex> lock(cache.mutex);

        ScopedBridgeSession session(env, thiz);
        std::string vfs_error;
        if (!ensure_cached_tree_vfs(cache, session, vfs_error)) {
            (void)clear_if_exception(env);
            return nullptr;
        }
        if (!cache.vfs->read_file(bmp_rel_path, bmp_bytes)) {
            (void)clear_if_exception(env);
            return nullptr;
        }
    }

    const std::uint32_t max_side = static_cast<std::uint32_t>(
        std::clamp<std::int32_t>(static_cast<std::int32_t>(max_side_jint), 16, 2048));

    wse::asset::DecodedBmp scaled{};
    {
        wse::asset::DecodedBmp decoded{};
        if (!wse::asset::decode_bmp(bmp_bytes, decoded)) {
            return nullptr;
        }
        std::vector<std::uint8_t>{}.swap(bmp_bytes);
        scaled = wse::asset::downscale_to_fit(decoded, max_side);
    }

    if (scaled.width == 0U || scaled.height == 0U ||
        static_cast<std::uint64_t>(scaled.width) * scaled.height != scaled.argb.size()) {
        return nullptr;
    }

    if (scaled.argb.size() > static_cast<std::size_t>(std::numeric_limits<jsize>::max()) - 3U) {
        return nullptr;
    }

    const std::uint32_t seed = wse::asset::seed_color_argb(scaled);
    const jsize total_len = static_cast<jsize>(std::size_t{3} + scaled.argb.size());
    jintArray result = env->NewIntArray(total_len);
    if (clear_if_exception(env) || result == nullptr) {
        if (result != nullptr) {
            env->DeleteLocalRef(result);
        }
        return nullptr;
    }

    const std::array<jint, 3> header{
        static_cast<jint>(scaled.width),
        static_cast<jint>(scaled.height),
        static_cast<jint>(seed),
    };
    env->SetIntArrayRegion(result, 0, 3, header.data());
    if (clear_if_exception(env)) {
        env->DeleteLocalRef(result);
        return nullptr;
    }

    std::array<jint, 4096> chunk{};
    std::size_t offset = 0;
    while (offset < scaled.argb.size()) {
        const std::size_t n = std::min(chunk.size(), scaled.argb.size() - offset);
        for (std::size_t i = 0; i < n; ++i) {
            chunk[i] = static_cast<jint>(scaled.argb[offset + i]);
        }
        env->SetIntArrayRegion(result, static_cast<jsize>(std::size_t{3} + offset),
                               static_cast<jsize>(n), chunk.data());
        if (clear_if_exception(env)) {
            env->DeleteLocalRef(result);
            return nullptr;
        }
        offset += n;
    }

    return result;
}

// `config/performance.json` under the app's private files directory (`configRoot`) is the one
// saved copy of the performance and launcher settings (`STATUS.md` §5 P8 review item 4).

// Returns the settings as JSON (the defaults when the file is missing or corrupt).
JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeLoadPerformanceSettings(JNIEnv* env,
                                                                 jclass clazz,
                                                                 jstring config_root) {
    (void)clazz;
    wse::platform::HostVfs vfs{std::filesystem::path(jstring_to_utf8(env, config_root))};
    wse::platform::PerformanceSettings s{};
    (void)wse::platform::load_performance_settings_or_default(vfs, s);
    return utf8_to_jstring(env, wse::platform::serialize_performance_settings_json(s));
}

// Applies the launcher's choices on top of the stored file and saves it.
JNIEXPORT jboolean JNICALL
Java_org_wse2_android_WseSafBridge_nativeSavePerformanceSettings(JNIEnv* env,
                                                                 jclass clazz,
                                                                 jstring config_root,
                                                                 jint frame_rate_cap_hz,
                                                                 jstring present_mode,
                                                                 jboolean thermal_throttling_enabled,
                                                                 jboolean dynamic_resolution_enabled,
                                                                 jboolean lod_bias_scaling_enabled,
                                                                 jboolean gyro_aim_enabled,
                                                                 jboolean pinch_zoom_enabled) {
    (void)clazz;
    wse::platform::HostVfs vfs{std::filesystem::path(jstring_to_utf8(env, config_root))};
    wse::platform::LauncherPerformanceChoices c{};
    c.frame_rate_cap =
        wse::platform::frame_rate_cap_from_hz(static_cast<std::int32_t>(frame_rate_cap_hz));
    if (!wse::platform::parse_present_mode(jstring_to_utf8(env, present_mode), c.present_mode)) {
        c.present_mode = wse::platform::PresentModePreference::Fifo;
    }
    c.thermal_throttling_enabled = (thermal_throttling_enabled == JNI_TRUE);
    c.dynamic_resolution_enabled = (dynamic_resolution_enabled == JNI_TRUE);
    c.lod_bias_scaling_enabled = (lod_bias_scaling_enabled == JNI_TRUE);
    c.gyro_aim_enabled = (gyro_aim_enabled == JNI_TRUE);
    c.pinch_zoom_enabled = (pinch_zoom_enabled == JNI_TRUE);
    return wse::platform::save_launcher_choices(vfs, c) ? JNI_TRUE : JNI_FALSE;
}

// Merges a partial JSON object of launcher choices onto `config/performance.json` and saves it.
JNIEXPORT jboolean JNICALL
Java_org_wse2_android_WseSafBridge_nativeSaveLauncherSettings(JNIEnv* env,
                                                              jclass clazz,
                                                              jstring config_root,
                                                              jstring choices_json) {
    (void)clazz;
    const std::string root = jstring_to_utf8(env, config_root);
    if (root.empty()) {
        return JNI_FALSE;
    }
    wse::platform::HostVfs vfs{std::filesystem::path(root)};
    const std::string choices = jstring_to_utf8(env, choices_json);
    return wse::platform::save_launcher_settings_json(vfs, choices) ? JNI_TRUE : JNI_FALSE;
}

// Ensures `config/performance.json` exists and is valid (seeding first-launch defaults when
// missing or corrupt), then returns its serialized JSON.
JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeEnsureFirstLaunchSettings(JNIEnv* env,
                                                                   jclass clazz,
                                                                   jstring config_root) {
    (void)clazz;
    const std::string root = jstring_to_utf8(env, config_root);
    wse::platform::PerformanceSettings s{};
    bool ok = !root.empty();
    if (ok) {
        wse::platform::HostVfs vfs{std::filesystem::path(root)};
        ok = wse::platform::ensure_first_launch_settings(vfs, s);
    }
    if (!ok) {
        s = wse::platform::PerformanceSettings{};
        wse::platform::apply_quality_preset(s, wse::platform::kFirstLaunchQualityPreset);
        wse::platform::sanitize_performance_settings(s);
    }
    return utf8_to_jstring(env, wse::platform::serialize_performance_settings_json(s));
}

// Loads the file into `RuntimePerformanceHub` for the SDL frame loop (called by `WseActivity`).
JNIEXPORT void JNICALL
Java_org_wse2_android_WseSafBridge_nativeApplyPerformanceSettings(JNIEnv* env,
                                                                  jclass clazz,
                                                                  jstring config_root) {
    (void)clazz;
    wse::platform::HostVfs vfs{std::filesystem::path(jstring_to_utf8(env, config_root))};
    wse::platform::RuntimeLaunchPerfConfig cfg{};
    (void)wse::platform::load_performance_settings_or_default(vfs, cfg.settings);
    wse::platform::RuntimePerformanceHub::global().set_launch_config(cfg);
}

JNIEXPORT void JNICALL
Java_org_wse2_android_WseSafBridge_nativeUpdateThermalState(JNIEnv* env,
                                                            jclass clazz,
                                                            jint thermal_status,
                                                            jfloat thermal_headroom) {
    (void)env;
    (void)clazz;
    wse::platform::RuntimePerformanceHub::global().update_thermal_state(
        static_cast<std::int32_t>(thermal_status), static_cast<float>(thermal_headroom));
}

// Stages an AdrenoTools-style driver package (meta.json + ICD bytes, already
// unzipped by Kotlin) under `stage_dir` on the app's own storage: validates
// meta.json, rejects a non-ELF `.so`, rewrites `libhardware.so` ->
// `libpanvk_hw.so`, and writes both files. dlopen needs a real filesystem path,
// so a SAF `content://` document cannot be loaded in place.
// `requested_mode` is the launcher's selected driver id: a Turnip/PanVK request
// refuses a package of the other kind, and a package whose `minApi` exceeds the
// device API level is refused.
// Returns `OK\t<mode>\t<absolute ICD path>\t<package name>\t<patched refs>` or
// `ERROR: <reason>`.
JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeStageDriverPackage(JNIEnv* env,
                                                           jobject thiz,
                                                           jstring meta_json_jstr,
                                                           jbyteArray icd_bytes_jarr,
                                                           jstring stage_dir_jstr,
                                                           jstring requested_mode_jstr) {
    (void)thiz;
    const std::string meta_json = jstring_to_utf8(env, meta_json_jstr);
    const std::string stage_dir = jstring_to_utf8(env, stage_dir_jstr);
    const std::string requested_str = jstring_to_utf8(env, requested_mode_jstr);
    wse::platform::DriverMode requested = wse::platform::DriverMode::system;
    if (!wse::platform::parse_driver_mode(requested_str, requested)) {
        return utf8_to_jstring(env, "ERROR: unknown GPU driver selection \"" + requested_str + "\"");
    }
    if (stage_dir.empty() || icd_bytes_jarr == nullptr) {
        return utf8_to_jstring(env, "ERROR: missing stage directory or driver bytes");
    }
    const jsize len = env->GetArrayLength(icd_bytes_jarr);
    std::vector<std::uint8_t> icd(static_cast<std::size_t>(len));
    if (len > 0) {
        env->GetByteArrayRegion(icd_bytes_jarr, 0, len, reinterpret_cast<jbyte*>(icd.data()));
        if (clear_if_exception(env)) {
            return utf8_to_jstring(env, "ERROR: failed reading driver bytes");
        }
    }

    wse::platform::DriverPackageMeta meta{};
    std::string err;
    if (!wse::platform::parse_driver_meta_json(meta_json, meta, &err) ||
        !wse::platform::check_driver_package_compat(meta, android_get_device_api_level(), requested,
                                                    &err)) {
        return utf8_to_jstring(env, "ERROR: " + err);
    }

    std::error_code ec;
    std::filesystem::create_directories(stage_dir, ec);
    wse::platform::HostVfs vfs(stage_dir);
    std::string staged_rel;
    std::size_t patched = 0;
    // The bytes are patched in place (no second multi-megabyte copy).
    if (!wse::platform::stage_driver_package_to_vfs(vfs, "", meta_json, std::move(icd), meta,
                                                    staged_rel, &patched, &err)) {
        return utf8_to_jstring(env, "ERROR: " + err);
    }
    std::string abs = stage_dir;
    if (abs.back() != '/') {
        abs.push_back('/');
    }
    abs.append(staged_rel);

    std::string display_name = meta.name.empty() ? meta.library_name : meta.name;
    for (char& ch : display_name) {
        const unsigned char uch = static_cast<unsigned char>(ch);
        if (uch < 0x20U || uch == 0x7FU) {
            ch = ' ';
        }
    }

    std::string out = "OK\t";
    out.append(wse::platform::driver_mode_name(meta.inferred_mode));
    out.push_back('\t');
    out.append(abs);
    out.push_back('\t');
    out.append(display_name);
    out.push_back('\t');
    out.append(std::to_string(patched));
    return utf8_to_jstring(env, out);
}

JNIEXPORT jstring JNICALL
Java_org_wse2_android_WseSafBridge_nativeProbeVulkanAndRenderMesh(JNIEnv* env,
                                                                  jobject thiz,
                                                                  jstring driver_mode_jstr,
                                                                  jstring custom_icd_path_jstr,
                                                                  jstring native_lib_dir_jstr,
                                                                  jstring shader_cache_dir_jstr,
                                                                  jboolean allow_mock_fallback) {
    (void)thiz;
    const std::string mode_str = jstring_to_utf8(env, driver_mode_jstr);
    const std::string custom_icd = jstring_to_utf8(env, custom_icd_path_jstr);
    const std::string native_lib_dir = jstring_to_utf8(env, native_lib_dir_jstr);
    const std::string shader_cache_dir = jstring_to_utf8(env, shader_cache_dir_jstr);

    wse::platform::DriverMode mode = wse::platform::DriverMode::system;
    if (!wse::platform::parse_driver_mode(mode_str, mode)) {
        return utf8_to_jstring(env, "ERROR: unknown GPU driver selection \"" + mode_str + "\"");
    }

    // Mesa drivers (Turnip and PanVK alike) link libhardware.so, which staging
    // rewrote to libpanvk_hw.so; preload the bundled stub for every custom driver.
    // Fall back to the bare soname (linker namespace search path) when the APK
    // keeps native libraries uncompressed and nativeLibraryDir has no file.
    std::string hw_stub_path;
    if (mode != wse::platform::DriverMode::system) {
        hw_stub_path = "libpanvk_hw.so";
        if (!native_lib_dir.empty()) {
            std::string candidate = native_lib_dir;
            if (candidate.back() != '/') {
                candidate.push_back('/');
            }
            candidate.append("libpanvk_hw.so");
            if (::access(candidate.c_str(), R_OK) == 0) {
                hw_stub_path = std::move(candidate);
            }
        }
        (void)wse::platform::apply_mesa_driver_environment(mode, shader_cache_dir);
    }

    std::uint64_t frame_hash = 0;
    std::string err;
    const wse::platform::VulkanDriverProbeResult probe =
        wse::platform::run_vulkan_mesh_smoke_test(mode, custom_icd, hw_stub_path,
                                                  allow_mock_fallback == JNI_TRUE, frame_hash,
                                                  &err);

    if (!probe.ok) {
        return utf8_to_jstring(env, "ERROR: " + (probe.error.empty() ? err : probe.error));
    }

    char hex_buf[24]{};
    std::snprintf(hex_buf, sizeof(hex_buf), "0x%016llx",
                  static_cast<unsigned long long>(frame_hash));

    std::string summary;
    if (probe.fell_back_to_mock) {
        summary = "MOCK ICD (real driver unavailable: " + probe.fallback_reason + ")\n";
    }
    summary.append("VK ");
    summary.append(wse::platform::driver_mode_name(probe.mode));
    if (probe.is_mock) {
        summary.append(" (mock)");
    } else if (probe.used_hal_module) {
        summary.append(" (hwvulkan HAL)");
    } else if (probe.used_direct_icd) {
        summary.append(" (direct ICD v");
        summary.append(std::to_string(probe.negotiated_icd_version));
        summary.push_back(')');
    }
    summary.append(": ");
    summary.append(probe.device_name);
    summary.append("\nAPI device ");
    summary.append(wse::platform::format_vulkan_version(probe.api_version));
    summary.append(" / instance ");
    summary.append(wse::platform::format_vulkan_version(probe.instance_api_version));
    summary.append(" | BC textures: ");
    summary.append(probe.texture_compression_bc ? "yes" : "no (CPU decode)");
    summary.append(" | max 2D ");
    summary.append(std::to_string(probe.max_image_dimension_2d));
    summary.append("\n64x64 CPU reference frame hash=");
    summary.append(hex_buf);
    return utf8_to_jstring(env, summary);
}

}  // extern "C"

// ---------------------------------------------------------------------------
// Registered game tree for the in-app module shell (`saf_game_tree.h`).
// ---------------------------------------------------------------------------

namespace {

struct GameTreeRegistry {
    std::mutex mutex;
    jobject bridge = nullptr;  // global reference to a `WseSafBridge`
};

GameTreeRegistry& game_tree_registry() {
    static GameTreeRegistry registry{};
    return registry;
}

}  // namespace

extern "C" {

// `WseActivity` registers the bridge for the launcher's tree before SDL starts the game
// thread; a later call replaces it, and a null `thiz` cannot occur (instance method).
JNIEXPORT void JNICALL
Java_org_wse2_android_WseSafBridge_nativeRegisterGameTree(JNIEnv* env, jobject thiz) {
    if (env == nullptr || thiz == nullptr) {
        return;
    }
    GameTreeRegistry& reg = game_tree_registry();
    std::lock_guard<std::mutex> lock(reg.mutex);
    jobject global = env->NewGlobalRef(thiz);
    if (clear_if_exception(env) || global == nullptr) {
        return;
    }
    if (reg.bridge != nullptr) {
        env->DeleteGlobalRef(reg.bridge);
    }
    reg.bridge = global;
}

}  // extern "C"

namespace wse::platform::android_saf {

struct RegisteredGameTree::Impl {
    JavaVM* vm = nullptr;
    jobject bridge = nullptr;  // own global reference, released with the tree
    JNIEnv* env = nullptr;
    std::unique_ptr<ScopedBridgeSession> session{};
    std::shared_ptr<AndroidJniSafBackend> backend{};
    std::unique_ptr<SafVfs> vfs{};

    ~Impl() {
        vfs.reset();
        if (backend) {
            backend->set_binding(nullptr);
        }
        session.reset();
        if (bridge != nullptr) {
            JNIEnv* del_env = env;
            void* raw_env = nullptr;
            if (vm != nullptr &&
                vm->GetEnv(&raw_env, JNI_VERSION_1_6) == JNI_OK &&
                raw_env != nullptr) {
                del_env = static_cast<JNIEnv*>(raw_env);
            }
            if (del_env != nullptr) {
                del_env->DeleteGlobalRef(bridge);
            }
        }
    }
};

RegisteredGameTree::RegisteredGameTree(std::unique_ptr<Impl> impl) noexcept
    : impl_(std::move(impl)) {}

RegisteredGameTree::~RegisteredGameTree() = default;

void RegisteredGameTree::rebind_thread(void* jni_env) noexcept {
    if (!impl_) {
        return;
    }
    auto* env = static_cast<JNIEnv*>(jni_env);
    impl_->env = env;
    if (impl_->session) {
        impl_->session->set_env(env);
    }
}

Vfs& RegisteredGameTree::vfs() noexcept { return *impl_->vfs; }

std::size_t RegisteredGameTree::file_count() const noexcept {
    return impl_->vfs->index_file_count();
}

std::unique_ptr<RegisteredGameTree> RegisteredGameTree::open(void* jni_env,
                                                             std::string* error_out) {
    const auto fail = [error_out](const char* message) -> std::unique_ptr<RegisteredGameTree> {
        if (error_out != nullptr) {
            *error_out = message;
        }
        return nullptr;
    };
    auto* env = static_cast<JNIEnv*>(jni_env);
    if (env == nullptr) {
        return fail("no JNIEnv for the calling thread");
    }
    auto impl = std::make_unique<Impl>();
    if (env->GetJavaVM(&impl->vm) != JNI_OK) {
        impl->vm = nullptr;
    }
    {
        GameTreeRegistry& reg = game_tree_registry();
        std::lock_guard<std::mutex> lock(reg.mutex);
        if (reg.bridge == nullptr) {
            return fail("no SAF tree registered (launch from WseLauncherActivity)");
        }
        impl->bridge = env->NewGlobalRef(reg.bridge);
    }
    impl->env = env;
    if (clear_if_exception(env) || impl->bridge == nullptr) {
        impl->bridge = nullptr;
        return fail("could not reference the registered SAF bridge");
    }
    impl->session = std::make_unique<ScopedBridgeSession>(env, impl->bridge);
    if (!impl->session->valid()) {
        return fail("the registered SAF bridge has no tree URI");
    }
    impl->backend = std::make_shared<AndroidJniSafBackend>();
    impl->session->attach(impl->backend);
    impl->vfs = std::make_unique<SafVfs>(impl->backend, false);
    if (!impl->vfs->rebuild_index()) {
        return fail("indexing the SAF tree failed (permission revoked or tree removed?)");
    }
    return std::unique_ptr<RegisteredGameTree>(new RegisteredGameTree(std::move(impl)));
}

}  // namespace wse::platform::android_saf
