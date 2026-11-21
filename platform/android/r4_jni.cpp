// Phase 5 R4 startup run on the phone (`docs/RECOMP_PLAN.md` "R4 design", "Phone run"): the JNI
// entry `R4Native.nativeRun`, called by `R4StartupService` in its own `:recomp` process. It
// builds the install file system (the SAF folder through `WseSafBridge`, or a copy in app
// storage), the writable layer in app storage, and runs `wse::recomp::run_startup`, which ends
// the process; the launcher reads `<work>/r4/report.txt` (every line, flushed as written) and
// `<work>/r4/progress.txt` (translation progress).
//
// Compiled into libwse_r0.so (with the translator) only when WSE_ENABLE_LLVM is ON.

#include <android/log.h>
#include <dlfcn.h>
#include <jni.h>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../../engine/src/recomp/r4_startup.h"
#include "saf_jni_util.h"
#include "wse/platform/saf_vfs.h"
#include "wse/platform/vfs.h"

namespace {

using wse::platform::android_jni::clear_if_exception;
using wse::platform::android_jni::jstring_to_utf8;
using wse::platform::android_jni::parse_child_record;
using wse::platform::android_jni::utf8_to_jstring;

constexpr const char* kTag = "WSE2-R4";

// The install folder through `WseSafBridge`'s instance methods, from any thread: each call gets
// the calling thread's JNIEnv, attaching it to the JVM the first time (guest threads are host
// threads; the run ends the process, so none is detached). Read-only: the run never writes the
// install (writes go to the writable layer). Callers serialize (`LockedVfs`).
class ThreadJniSafBackend final : public wse::platform::SafTreeBackend {
public:
    ThreadJniSafBackend(JavaVM* vm, JNIEnv* env, jobject bridge) : vm_(vm) {
        bridge_ = env->NewGlobalRef(bridge);
        jclass cls = env->GetObjectClass(bridge);
        mid_root_ = env->GetMethodID(cls, "rootDocumentId", "()Ljava/lang/String;");
        mid_list_ = env->GetMethodID(cls, "listChildren", "(Ljava/lang/String;)[Ljava/lang/String;");
        mid_read_ = env->GetMethodID(cls, "readDocument", "(Ljava/lang/String;)[B");
        env->DeleteLocalRef(cls);
        if (clear_if_exception(env)) mid_root_ = mid_list_ = mid_read_ = nullptr;
    }
    [[nodiscard]] bool valid() const noexcept {
        return bridge_ != nullptr && mid_root_ != nullptr && mid_list_ != nullptr && mid_read_ != nullptr;
    }

    [[nodiscard]] std::string root_document_id() const override {
        JNIEnv* env = this_env();
        if (env == nullptr || !valid()) return {};
        auto jroot = static_cast<jstring>(env->CallObjectMethod(bridge_, mid_root_));
        if (clear_if_exception(env) || jroot == nullptr) return {};
        std::string out = jstring_to_utf8(env, jroot);
        env->DeleteLocalRef(jroot);
        return out;
    }

    [[nodiscard]] bool list_children(std::string_view parent, std::vector<wse::platform::SafChildEntry>& out) override {
        out.clear();
        JNIEnv* env = this_env();
        if (env == nullptr || !valid()) return false;
        jstring jparent = utf8_to_jstring(env, parent);
        if (jparent == nullptr) return false;
        auto jarr = static_cast<jobjectArray>(env->CallObjectMethod(bridge_, mid_list_, jparent));
        env->DeleteLocalRef(jparent);
        if (clear_if_exception(env) || jarr == nullptr) return false;
        const jsize n = env->GetArrayLength(jarr);
        out.reserve(static_cast<std::size_t>(n));
        for (jsize i = 0; i < n; ++i) {
            auto jelem = static_cast<jstring>(env->GetObjectArrayElement(jarr, i));
            if (clear_if_exception(env)) {
                env->DeleteLocalRef(jarr);
                out.clear();
                return false;
            }
            if (jelem == nullptr) continue;
            const std::string line = jstring_to_utf8(env, jelem);
            env->DeleteLocalRef(jelem);
            wse::platform::SafChildEntry e{};
            if (parse_child_record(line, e)) out.push_back(std::move(e));
        }
        env->DeleteLocalRef(jarr);
        return true;
    }

    [[nodiscard]] bool read_document(std::string_view id, std::vector<std::uint8_t>& out) override {
        out.clear();
        JNIEnv* env = this_env();
        if (env == nullptr || !valid()) return false;
        jstring jid = utf8_to_jstring(env, id);
        if (jid == nullptr) return false;
        auto jbytes = static_cast<jbyteArray>(env->CallObjectMethod(bridge_, mid_read_, jid));
        env->DeleteLocalRef(jid);
        if (clear_if_exception(env) || jbytes == nullptr) return false;
        const jsize len = env->GetArrayLength(jbytes);
        out.resize(static_cast<std::size_t>(len));
        if (len > 0) env->GetByteArrayRegion(jbytes, 0, len, reinterpret_cast<jbyte*>(out.data()));
        env->DeleteLocalRef(jbytes);
        if (clear_if_exception(env)) {
            out.clear();
            return false;
        }
        return true;
    }

    // The install is read-only for the run.
    [[nodiscard]] bool write_document(std::string_view, std::string_view, std::string_view, std::span<const std::uint8_t>,
                                      std::string& out_id) override {
        out_id.clear();
        return false;
    }
    [[nodiscard]] bool create_directory(std::string_view, std::string_view, std::string& out_id) override {
        out_id.clear();
        return false;
    }
    [[nodiscard]] bool delete_document(std::string_view) override { return false; }

private:
    [[nodiscard]] JNIEnv* this_env() const {
        void* raw = nullptr;
        if (vm_->GetEnv(&raw, JNI_VERSION_1_6) == JNI_OK && raw != nullptr) return static_cast<JNIEnv*>(raw);
        JNIEnv* env = nullptr;
        return vm_->AttachCurrentThread(&env, nullptr) == JNI_OK ? env : nullptr;
    }

    JavaVM* vm_ = nullptr;
    jobject bridge_ = nullptr;
    jmethodID mid_root_ = nullptr;
    jmethodID mid_list_ = nullptr;
    jmethodID mid_read_ = nullptr;
};

// Serializes every call of a Vfs that is not thread-safe (`SafVfs`): guest threads read files
// concurrently (resource loading threads).
class LockedVfs final : public wse::platform::Vfs {
public:
    explicit LockedVfs(wse::platform::Vfs& inner) : inner_(inner) {}
    bool read_file(std::string_view p, std::vector<std::uint8_t>& out) override { return locked([&] { return inner_.read_file(p, out); }); }
    bool write_file(std::string_view p, std::span<const std::uint8_t> d) override { return locked([&] { return inner_.write_file(p, d); }); }
    bool remove_file(std::string_view p) override { return locked([&] { return inner_.remove_file(p); }); }
    bool list_dir(std::string_view p, std::vector<wse::platform::VfsEntry>& out) override {
        return locked([&] { return inner_.list_dir(p, out); });
    }
    bool is_file(std::string_view p) override { return locked([&] { return inner_.is_file(p); }); }
    bool is_dir(std::string_view p) override { return locked([&] { return inner_.is_dir(p); }); }
    bool make_dir(std::string_view p) override { return locked([&] { return inner_.make_dir(p); }); }
    bool remove_dir(std::string_view p) override { return locked([&] { return inner_.remove_dir(p); }); }
    bool rename_file(std::string_view f, std::string_view t) override { return locked([&] { return inner_.rename_file(f, t); }); }
    bool file_size(std::string_view p, std::uint64_t& size) override { return locked([&] { return inner_.file_size(p, size); }); }
    bool read_text(std::string_view p, std::string& out) override { return locked([&] { return inner_.read_text(p, out); }); }

private:
    template <typename F>
    bool locked(F&& f) {
        const std::lock_guard<std::mutex> lock(m_);
        return f();
    }
    wse::platform::Vfs& inner_;
    std::mutex m_;
};

std::FILE* g_report = nullptr;
std::string g_progress_path;

void report(const std::string& text) {
    if (g_report != nullptr) {
        std::fputs(text.c_str(), g_report);
        std::fflush(g_report);
    }
    __android_log_write(ANDROID_LOG_INFO, kTag, text.c_str());
}

}  // namespace

extern "C" JNIEXPORT jint JNICALL Java_org_wse2_android_launcher_R4Native_nativeRun(JNIEnv* env, jclass, jobject bridge,
                                                                                    jstring jcopy_dir, jstring jwork_dir,
                                                                                    jstring jmodule, jint jobs, jlong frames) {
    namespace fs = std::filesystem;
    const fs::path work = fs::path(jstring_to_utf8(env, jwork_dir)) / "r4";
    std::error_code ec;
    fs::create_directories(work / "user", ec);
    g_report = std::fopen((work / "report.txt").c_str(), "w");
    g_progress_path = (work / "progress.txt").string();

    // The install: the SAF folder (through the launcher's bridge) or a copy in app storage.
    static std::unique_ptr<wse::platform::Vfs> install_base;
    static std::unique_ptr<LockedVfs> install;
    const std::string copy_dir = jstring_to_utf8(env, jcopy_dir);
    if (!copy_dir.empty()) {
        report("install: the copy in app storage (" + copy_dir + ")\n");
        install_base = std::make_unique<wse::platform::HostVfs>(fs::path(copy_dir));
    } else {
        JavaVM* vm = nullptr;
        if (bridge == nullptr || env->GetJavaVM(&vm) != JNI_OK) {
            report("setup: no install folder\nRESULT setup-error\n");
            return wse::recomp::kStartupSetupError;
        }
        auto backend = std::make_shared<ThreadJniSafBackend>(vm, env, bridge);
        if (!backend->valid()) {
            report("setup: the install folder bridge is missing a method\nRESULT setup-error\n");
            return wse::recomp::kStartupSetupError;
        }
        report("install: the SAF folder (indexing it)\n");
        auto saf = std::make_unique<wse::platform::SafVfs>(backend, false);
        if (!saf->rebuild_index()) {
            report("setup: indexing the install folder failed (permission revoked or folder removed?)\nRESULT setup-error\n");
            return wse::recomp::kStartupSetupError;
        }
        report("install: " + std::to_string(saf->index_file_count()) + " files indexed\n");
        install_base = std::move(saf);
    }
    install = std::make_unique<LockedVfs>(*install_base);
    static wse::platform::HostVfs user(work / "user");

    wse::recomp::StartupOptions o;
    if (!install->read_file("mb_warband_wse2_x64.exe", o.exe) || !install->read_file("lua5164.dll", o.lua) ||
        !install->read_file("mb_warband_wse2_x64.pdb", o.pdb)) {
        report("setup: mb_warband_wse2_x64.exe, lua5164.dll and mb_warband_wse2_x64.pdb must be in the install's root\n"
               "RESULT setup-error\n");
        return wse::recomp::kStartupSetupError;
    }
    o.install = install.get();
    o.user = &user;
    o.so_dir = (work / "so").string();
    o.module = jstring_to_utf8(env, jmodule);
    if (o.module.empty()) o.module = "Native";
    o.jobs = static_cast<unsigned>(jobs < 1 ? 1 : jobs);
    o.opt = 2;
    o.quiet = false;
    o.frame_limit = frames < 0 ? 0 : static_cast<std::uint64_t>(frames);
    o.log = report;
    o.progress = [](const std::string& image, std::size_t done, std::size_t total) {
        std::ofstream(g_progress_path) << image << " " << done << " " << total << "\n";
    };
    report(std::to_string(o.jobs) + " translation threads, -O2, module " + o.module +
           ", stop after " + std::to_string(o.frame_limit) + " frames\n");
    return wse::recomp::run_startup(o);  // returns only when nothing could be started
}
