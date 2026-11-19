#!/bin/bash
# The recompiler gate and every CTest suite in WSL Ubuntu on the user's PC (Phase 5 R4 slice 1,
# `docs/RECOMP_PLAN.md` "R4 design"), configured as CI job `recomp-host` configures it.
#
#   wsl -e bash tools/wsl_gate.sh            # from the checkout, through /mnt/c
#
# The sources stay in the Windows checkout; the build tree is in WSL's own filesystem
# (WSE_WSL_BUILD, default /root/wse/build-recomp). Third-party sources the Windows host gate cached
# under the main checkout's engine/build/third_party are reused when present (only Zydis is fetched).
# Two differences from the CI runner, both for Ubuntu 26.04 (AGENTS.md section 5): CC=clang-18 (no
# `cc` in a fresh install) and -Wno-#warnings (libstdc++ 15 warns on the <ciso646> LLVM 18 includes).
set -o pipefail
SRC="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TP="${WSE_THIRD_PARTY:-${SRC%%/.wt/*}/engine/build/third_party}"  # a worktree under repo/.wt/ uses the main checkout's
OUT="${WSE_WSL_BUILD:-/root/wse/build-recomp}"
JOBS="${WSE_WSL_JOBS:-2}"  # ~3 GB of RAM in WSL on this PC
mkdir -p "$OUT"
step() { echo "== $1 ($(date +%T))"; }
extra=()
[ -d "$TP/lua-5.1.5" ] && extra+=("-DWSE_LUA_SOURCE_DIR=$TP/lua-5.1.5")
[ -d "$TP/audio" ] && extra+=("-DWSE_AUDIO_THIRD_PARTY_DIR=$TP/audio")
[ -d "$TP/glslang-16.6.0" ] && extra+=("-DWSE_GLSLANG_SOURCE_DIR=$TP/glslang-16.6.0")
step configure
CC=clang-18 CXX=clang++-18 cmake -S "$SRC" -B "$OUT" -DWSE_ENABLE_LLVM=ON \
  -DLLVM_DIR=/usr/lib/llvm-18/lib/cmake/llvm -DLLD_DIR=/usr/lib/llvm-18/lib/cmake/lld \
  -DWSE_REQUIRE_RECOMP_CORPUS=ON "-DCMAKE_CXX_FLAGS=-Wno-#warnings" "${extra[@]}" \
  >"$OUT/configure.log" 2>&1 || { tail -30 "$OUT/configure.log"; exit 1; }
step build
cmake --build "$OUT" -j"$JOBS" >"$OUT/build.log" 2>&1 || { grep -E "error" "$OUT/build.log" | head -30; exit 1; }
step recomp_llvm_test
"$OUT/recomp_llvm_test" 2>&1 | grep -E "FAIL|MISMATCH|differential:|coverage:|recomp_llvm tests" | head -30
step ctest
(cd "$OUT" && ctest --output-on-failure -j"$JOBS" 2>&1 | grep -E "tests passed|tests failed|Failed|\*\*\*")
step done
df -h /mnt/c | tail -1
