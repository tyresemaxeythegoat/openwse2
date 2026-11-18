// SAF-picked Warband install for the in-app module shell (Android only).
//
// `WseActivity` creates a `WseSafBridge` for the tree the launcher picked and registers it
// (`WseSafBridge.nativeRegisterGameTree`, a JNI global reference). The SDL thread then opens
// a `SafVfs` over it bound to its own `JNIEnv`: the launcher's SAF cache binds a JNI session
// per call on the UI thread, which the game thread cannot borrow.

#pragma once

#include <memory>
#include <string>

#include "wse/platform/vfs.h"

namespace wse::platform::android_saf {

class RegisteredGameTree {
public:
    // `jni_env` is the calling thread's `JNIEnv*` (`SDL_GetAndroidJNIEnv()`); the tree may be
    // used only on that thread. Indexes the whole tree before returning (one SAF listing per
    // directory). Null with `error_out` set when no tree was registered or indexing failed.
    [[nodiscard]] static std::unique_ptr<RegisteredGameTree> open(void* jni_env,
                                                                  std::string* error_out);

    ~RegisteredGameTree();
    RegisteredGameTree(const RegisteredGameTree&) = delete;
    RegisteredGameTree& operator=(const RegisteredGameTree&) = delete;

    [[nodiscard]] Vfs& vfs() noexcept;
    [[nodiscard]] std::size_t file_count() const noexcept;

private:
    struct Impl;
    explicit RegisteredGameTree(std::unique_ptr<Impl> impl) noexcept;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wse::platform::android_saf
