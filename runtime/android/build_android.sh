#!/bin/sh
# Build the native PanVK loader, the libhardware stub, and the on-device probe.
#
# Requires an Android NDK (r26 or newer). The NDK is only used as a compiler
# sysroot; nothing is installed system-wide.
#
# Usage:
#   API=29 ./build_android.sh
#   ANDROID_NDK_HOME=/opt/android-ndk ./build_android.sh

set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
api=${API:-28}
out="$here/build/android/arm64-v8a"
mkdir -p "$out"

ndk=${ANDROID_NDK_HOME:-${ANDROID_NDK_ROOT:-${NDK_HOME:-}}}
if [ -z "$ndk" ]; then
    for root in "$HOME/Android/Sdk/ndk" "$HOME/Library/Android/sdk/ndk" "$HOME/android-sdk/ndk"; do
        if [ -d "$root" ]; then
            ndk=$(ls -1d "$root"/* 2>/dev/null | sort -r | head -n 1 || true)
            [ -n "$ndk" ] && break
        fi
    done
fi

if [ -z "$ndk" ] || [ ! -d "$ndk/toolchains/llvm" ]; then
    echo "Android NDK not found." >&2
    echo "Set ANDROID_NDK_HOME or install the NDK under the SDK's ndk/ folder." >&2
    echo "Download: https://developer.android.com/ndk/downloads" >&2
    exit 1
fi

case "$(uname -s)" in
    Darwin) host_tag=darwin-x86_64 ;;
    Linux) host_tag=linux-x86_64 ;;
    *)
        echo "unsupported build host: $(uname -s)" >&2
        exit 1
        ;;
esac

cc="$ndk/toolchains/llvm/prebuilt/$host_tag/bin/aarch64-linux-android${api}-clang"
if [ ! -x "$cc" ]; then
    echo "compiler not found: $cc" >&2
    exit 1
fi

includes="-I$here/third_party -I$here/src"

echo "NDK          $ndk"
echo "compiler     $cc"
echo "API          $api"
echo "output       $out"
echo

# Freestanding stub: no crt, no libc, no DT_NEEDED entries.
"$cc" -O2 -fPIC -shared -nostdlib -Wl,-soname,libpanvk_hw.so \
    -o "$out/libpanvk_hw.so" "$here/src/hardware_stub.c"

"$cc" -O2 -g $includes -o "$out/panvk_probe" \
    "$here/src/vk_panvk_loader.c" "$here/src/panvk_probe.c" -llog

"$cc" -O2 -g -fPIC -shared $includes -Wl,-soname,libwse2_panvk.so \
    -o "$out/libwse2_panvk.so" "$here/src/vk_panvk_loader.c" -llog

ls -l "$out"

cat <<EOF

Next steps (see runtime/android/README.md):
  1. python tools/patch_panvk.py <PanVK>/libvulkan_panfrost.so -o $out/libvulkan_panfrost.so
  2. python tools/check_panvk.py $out/libvulkan_panfrost.so --stub $out/libpanvk_hw.so
  3. Package libvulkan_panfrost.so, libpanvk_hw.so and panvk_probe under jniLibs/arm64-v8a/.
EOF
