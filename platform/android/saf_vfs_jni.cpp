// Platform seam: Android JNI backend and launcher bridge for SafVfs
// (docs/PLATFORM_PLAN.md, slice P2).
//
// Compiled into libwse2.so only when WSE_BUILD_ANDROID_APP is ON.

#include <jni.h>

#include <charconv>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wse/core/json.h"
#include "wse/platform/clock.h"
#include "wse/platform/saf_vfs.h"

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

[[nodiscard]] std::string jstring_to_utf8(JNIEnv* env, jstring jstr) {
    if (env == nullptr || jstr == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(jstr, nullptr);
    if (chars == nullptr) {
        (void)clear_if_exception(env);
        return {};
    }
    std::string out(chars);
    env->ReleaseStringUTFChars(jstr, chars);
    return out;
}

[[nodiscard]] jstring utf8_to_jstring(JNIEnv* env, std::string_view sv) {
    if (env == nullptr) {
        return nullptr;
    }
    const std::string tmp(sv);
    jstring result = env->NewStringUTF(tmp.c_str());
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
        cls_ = env->GetObjectClass(thiz);
        if (clear_if_exception(env) || cls_ == nullptr) {
            return;
        }
        binding_.env = env;
        binding_.bridge = thiz;
        binding_.mid_tree_key = env->GetMethodID(cls_, "treeUriString", "()Ljava/lang/String;");
        binding_.mid_root = env->GetMethodID(cls_, "rootDocumentId", "()Ljava/lang/String;");
        binding_.mid_list =
            env->GetMethodID(cls_, "listChildren", "(Ljava/lang/String;)[Ljava/lang/String;");
        binding_.mid_read = env->GetMethodID(cls_, "readDocument", "(Ljava/lang/String;)[B");
        binding_.mid_write = env->GetMethodID(
            cls_, "writeDocument",
            "(Ljava/lang/String;Ljava/lang/String;Ljava/lang/String;[B)Ljava/lang/String;");
        binding_.mid_mkdir = env->GetMethodID(
            cls_, "createDirectory", "(Ljava/lang/String;Ljava/lang/String;)Ljava/lang/String;");
        binding_.mid_delete = env->GetMethodID(cls_, "deleteDocument", "(Ljava/lang/String;)Z");
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
        if (binding_.env != nullptr && cls_ != nullptr) {
            binding_.env->DeleteLocalRef(cls_);
        }
    }

    [[nodiscard]] bool valid() const noexcept { return valid_; }
    [[nodiscard]] const std::string& tree_key() const noexcept { return tree_key_; }

    void attach(std::shared_ptr<AndroidJniSafBackend>& backend) {
        bound_backend_ = backend.get();
        if (bound_backend_ != nullptr) {
            bound_backend_->set_binding(&binding_);
        }
    }

private:
    jclass cls_ = nullptr;
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
    if (!session.valid()) {
        return utf8_to_jstring(env, "ERROR: failed to bind WseSafBridge JNI methods");
    }

    wse::platform::SteadyClock clock;
    std::int64_t index_ms = 0;

    const bool cache_hit = cache.vfs && cache.backend &&
                           cache.tree_key == session.tree_key() &&
                           cache.vfs->index_complete();
    if (!cache_hit) {
        cache.tree_key = session.tree_key();
        cache.backend = std::make_shared<AndroidJniSafBackend>();
        cache.vfs = std::make_unique<wse::platform::SafVfs>(cache.backend, false);
        session.attach(cache.backend);
        const std::int64_t i0 = clock.ticks();
        const bool ok = cache.vfs->rebuild_index();
        const std::int64_t i1 = clock.ticks();
        index_ms = ((i1 - i0) * 1000) / wse::platform::kTicksPerSecond;
        if (!ok) {
            return utf8_to_jstring(env, "ERROR: failed indexing SAF document tree");
        }
    } else {
        session.attach(cache.backend);
    }

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

}  // extern "C"
