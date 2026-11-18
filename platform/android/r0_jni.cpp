// JNI entry for the launcher's Advanced > "R0 spikes" row (`R0Spikes.kt`): runs
// `wse::recomp::run_r0_spikes` and returns its plain-text report. Built into `libwse_r0.so`,
// which embeds LLVM + LLD and is loaded only when the row is used.

#include <jni.h>

#include <android/log.h>

#include <cstdint>
#include <string>
#include <vector>

#include "../../engine/src/recomp/r0_spikes.h"

namespace {

std::string to_string(JNIEnv* env, jstring s) {
    if (s == nullptr) {
        return {};
    }
    const char* chars = env->GetStringUTFChars(s, nullptr);
    std::string out = chars != nullptr ? chars : "";
    if (chars != nullptr) {
        env->ReleaseStringUTFChars(s, chars);
    }
    return out;
}

}  // namespace

extern "C" JNIEXPORT jstring JNICALL
Java_org_wse2_android_launcher_R0Spikes_nativeRun(JNIEnv* env, jclass /*clazz*/, jstring work_dir,
                                                   jbyteArray exe, jstring exe_sha256,
                                                   jstring exe_error, jbyteArray pdb,
                                                   jstring pdb_error, jlong apk_bytes) {
    wse::recomp::R0SpikeInput input;
    input.work_dir = to_string(env, work_dir);
    input.exe_sha256 = to_string(env, exe_sha256);
    input.exe_error = to_string(env, exe_error);
    input.apk_bytes = apk_bytes > 0 ? static_cast<std::uint64_t>(apk_bytes) : 0U;
    if (exe != nullptr) {
        const jsize n = env->GetArrayLength(exe);
        input.exe.resize(static_cast<std::size_t>(n));
        env->GetByteArrayRegion(exe, 0, n, reinterpret_cast<jbyte*>(input.exe.data()));
    }
    input.pdb_error = to_string(env, pdb_error);
    if (pdb != nullptr) {
        const jsize n = env->GetArrayLength(pdb);
        input.pdb.resize(static_cast<std::size_t>(n));
        env->GetByteArrayRegion(pdb, 0, n, reinterpret_cast<jbyte*>(input.pdb.data()));
    }
    const wse::recomp::R0SpikeResult result = wse::recomp::run_r0_spikes(input);
    __android_log_write(ANDROID_LOG_INFO, "WSE2-R0", result.report.c_str());
    return env->NewStringUTF(result.report.c_str());
}
