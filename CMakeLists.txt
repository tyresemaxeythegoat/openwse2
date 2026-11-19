cmake_minimum_required(VERSION 3.20)
project(wse2_android LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(wse_engine INTERFACE)
target_include_directories(wse_engine INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
)
target_compile_features(wse_engine INTERFACE cxx_std_20)

option(WSE_BUILD_TESTS "Build the engine C++20 unit test binaries" ON)
option(WSE_BUILD_TOOLS "Build portable engine CLI tools" ON)
option(WSE_BUILD_ANDROID_APP "Build the Android SDL3 shared library target wse2" OFF)
option(WSE_BUILD_HEADER_CHECKS "Compile every engine header standalone" OFF)
option(WSE_SANITIZE "Build tests with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

# The documented gate is -O2. Without a build type CMake passes no optimisation
# flag at all, so default single-config generators to RelWithDebInfo (-O2 -g).
get_property(_wse_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(NOT _wse_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

# Android Gradle builds the debug APK (the one CI uploads and testers install) with
# CMAKE_BUILD_TYPE=Debug, which the NDK maps to -O0: the VM, loaders and UI then run several
# times slower than on the -O2 host gate, so device timings would be meaningless. Keep the
# native code optimised in that APK; -g keeps it debuggable. Turn off to step through -O0 code.
option(WSE_ANDROID_OPTIMIZE_DEBUG "Compile Android Debug builds at -O2" ON)
if(ANDROID AND WSE_ANDROID_OPTIMIZE_DEBUG AND CMAKE_BUILD_TYPE STREQUAL "Debug")
    # Compile options follow CMAKE_<LANG>_FLAGS_DEBUG on the command line, so -O2 wins.
    add_compile_options(-O2 -g)
endif()

if(WSE_SANITIZE AND NOT MSVC)
    add_compile_options(-fsanitize=address,undefined -fno-sanitize-recover=undefined
                        -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

function(wse_apply_strict_warnings target)
    if(MSVC)
        target_compile_options("${target}" PRIVATE /W4 /WX /utf-8)
    else()
        target_compile_options("${target}" PRIVATE -Wall -Wextra -Werror)
    endif()
endfunction()

if(WSE_BUILD_HEADER_CHECKS)
    file(GLOB_RECURSE WSE_HEADERS CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/include/*.h"
    )
    set(_header_check_sources)
    foreach(_hdr IN LISTS WSE_HEADERS)
        file(RELATIVE_PATH _rel "${CMAKE_CURRENT_SOURCE_DIR}/engine/include" "${_hdr}")
        string(REGEX REPLACE "[^A-Za-z0-9_]" "_" _stem "${_rel}")
        set(_stub "${CMAKE_CURRENT_BINARY_DIR}/header_checks/${_stem}.cpp")
        file(WRITE "${_stub}" "#include \"${_rel}\"\n")
        list(APPEND _header_check_sources "${_stub}")
    endforeach()
    add_library(wse_header_checks OBJECT ${_header_check_sources})
    target_link_libraries(wse_header_checks PRIVATE wse_engine)
    wse_apply_strict_warnings(wse_header_checks)
endif()

if(NOT DEFINED WSE_LUA_SOURCE_DIR AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/lua-5.1.5/src/lua.h")
    set(WSE_LUA_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/lua-5.1.5")
endif()

if(DEFINED WSE_LUA_SOURCE_DIR)
    set(wse_lua51_src_dir "${WSE_LUA_SOURCE_DIR}/src")
else()
    if(POLICY CMP0135)
        cmake_policy(SET CMP0135 NEW)
    endif()
    include(FetchContent)
    FetchContent_Declare(
        lua51
        URL https://www.lua.org/ftp/lua-5.1.5.tar.gz
        URL_HASH SHA256=2640fc56a795f29d28ef15e13c34a47e223960b0240e8cb0a82d9b0738695333
        DOWNLOAD_EXTRACT_TIMESTAMP TRUE
    )
    FetchContent_MakeAvailable(lua51)
    set(wse_lua51_src_dir "${lua51_SOURCE_DIR}/src")
endif()

add_library(lua51 STATIC
    "${wse_lua51_src_dir}/lapi.c"
    "${wse_lua51_src_dir}/lauxlib.c"
    "${wse_lua51_src_dir}/lbaselib.c"
    "${wse_lua51_src_dir}/lcode.c"
    "${wse_lua51_src_dir}/ldblib.c"
    "${wse_lua51_src_dir}/ldebug.c"
    "${wse_lua51_src_dir}/ldo.c"
    "${wse_lua51_src_dir}/ldump.c"
    "${wse_lua51_src_dir}/lfunc.c"
    "${wse_lua51_src_dir}/lgc.c"
    "${wse_lua51_src_dir}/linit.c"
    "${wse_lua51_src_dir}/liolib.c"
    "${wse_lua51_src_dir}/llex.c"
    "${wse_lua51_src_dir}/lmathlib.c"
    "${wse_lua51_src_dir}/lmem.c"
    "${wse_lua51_src_dir}/loadlib.c"
    "${wse_lua51_src_dir}/lobject.c"
    "${wse_lua51_src_dir}/lopcodes.c"
    "${wse_lua51_src_dir}/loslib.c"
    "${wse_lua51_src_dir}/lparser.c"
    "${wse_lua51_src_dir}/lstate.c"
    "${wse_lua51_src_dir}/lstring.c"
    "${wse_lua51_src_dir}/lstrlib.c"
    "${wse_lua51_src_dir}/ltable.c"
    "${wse_lua51_src_dir}/ltablib.c"
    "${wse_lua51_src_dir}/ltm.c"
    "${wse_lua51_src_dir}/lundump.c"
    "${wse_lua51_src_dir}/lvm.c"
    "${wse_lua51_src_dir}/lzio.c"
)
# Stock Lua is compiled as C++ so LUAI_THROW/LUAI_TRY use exceptions (see
# luaconf.h): a Lua error raised inside a wse_lua callback then unwinds through
# C++ frames and runs their destructors instead of longjmp-ing over them.
get_target_property(wse_lua51_sources lua51 SOURCES)
set_source_files_properties(${wse_lua51_sources} PROPERTIES LANGUAGE CXX)
set_target_properties(lua51 PROPERTIES
    LINKER_LANGUAGE CXX
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(lua51 PRIVATE "${wse_lua51_src_dir}")
if(NOT WIN32)
    target_compile_definitions(lua51 PRIVATE LUA_USE_MKSTEMP LUA_USE_ISATTY)
    target_link_libraries(lua51 INTERFACE m)
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(lua51 PRIVATE -w)
endif()
if(WSE_SANITIZE AND NOT MSVC)
    # Stock Lua 5.1.5 casts arbitrary doubles to int when probing a table's
    # array part (lua_number2int in ltable.c), which Clang's UBSan reports as
    # float-cast-overflow for any number key outside the int range. Only that
    # one check is disabled, and only for the unmodified third-party sources.
    target_compile_options(lua51 PRIVATE -fno-sanitize=float-cast-overflow)
endif()

add_library(wse_lua STATIC
    engine/src/lua/lua_bitop.cpp
    engine/src/lua/wse_lua_callbacks.cpp
    engine/src/lua/wse_lua_manager.cpp
    engine/src/lua/wse_lua_ops.cpp
)
set_target_properties(wse_lua PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_lua PRIVATE "${wse_lua51_src_dir}")
target_link_libraries(wse_lua PRIVATE wse_engine lua51)
wse_apply_strict_warnings(wse_lua)

# Third-party public-domain / MIT-0 compressed audio decoders (slice P6):
# stb_vorbis.c (Ogg Vorbis) and dr_mp3.h (MP3). Cached under engine/build/third_party/audio/
# when built via build_host.ps1, or fetched per build directory with SHA-256 verification.
set(WSE_STB_VORBIS_URL
    "https://raw.githubusercontent.com/nothings/stb/9600522de53973336fa033a289858a4354cbb04c/stb_vorbis.c")
set(WSE_STB_VORBIS_SHA256
    "b52cf518355f7b2d181f18b97b812f361ffeb26299d935c8cf29e842401d60a6")
set(WSE_DR_MP3_URL
    "https://raw.githubusercontent.com/mackron/dr_libs/f12e197fd462e4c22873fbd70d11b1d2f2c1dc3c/dr_mp3.h")
set(WSE_DR_MP3_SHA256
    "7a6cc38b0afdb55cfc9eec991d9169d67beb8fd910be2995121510cdaa99c356")

# Every configure re-hashes the files actually compiled, wherever they came from:
# a mismatch in a user-supplied or source-tree cache is fatal; a stale copy in the
# build directory is deleted and fetched again.
function(wse_fetch_audio_vendor_file dir name url sha256 may_download)
    set(path "${dir}/${name}")
    if(EXISTS "${path}")
        file(SHA256 "${path}" actual)
        if(actual STREQUAL sha256)
            return()
        endif()
        if(NOT may_download)
            message(FATAL_ERROR
                "${path} SHA-256 mismatch: expected ${sha256}, got ${actual}")
        endif()
        message(STATUS "${name}: cached copy failed SHA-256 check, downloading again")
        file(REMOVE "${path}")
    elseif(NOT may_download)
        message(FATAL_ERROR "${path} not found (WSE_AUDIO_THIRD_PARTY_DIR)")
    endif()
    message(STATUS "Downloading ${name} (SHA-256 verified)...")
    file(DOWNLOAD "${url}" "${path}"
         EXPECTED_HASH "SHA256=${sha256}"
         TLS_VERIFY ON
         STATUS dl_status)
    list(GET dl_status 0 dl_code)
    if(NOT dl_code EQUAL 0)
        file(REMOVE "${path}")
        message(FATAL_ERROR "Failed to download ${name}: ${dl_status}")
    endif()
endfunction()

if(NOT DEFINED WSE_AUDIO_THIRD_PARTY_DIR AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/audio/stb_vorbis.c" AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/audio/dr_mp3.h")
    set(WSE_AUDIO_THIRD_PARTY_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/audio")
endif()

if(DEFINED WSE_AUDIO_THIRD_PARTY_DIR)
    set(wse_audio_third_party_dir "${WSE_AUDIO_THIRD_PARTY_DIR}")
    set(wse_audio_may_download OFF)
else()
    set(wse_audio_third_party_dir "${CMAKE_CURRENT_BINARY_DIR}/_third_party/audio")
    file(MAKE_DIRECTORY "${wse_audio_third_party_dir}")
    set(wse_audio_may_download ON)
endif()
wse_fetch_audio_vendor_file("${wse_audio_third_party_dir}" stb_vorbis.c
    "${WSE_STB_VORBIS_URL}" "${WSE_STB_VORBIS_SHA256}" ${wse_audio_may_download})
wse_fetch_audio_vendor_file("${wse_audio_third_party_dir}" dr_mp3.h
    "${WSE_DR_MP3_URL}" "${WSE_DR_MP3_SHA256}" ${wse_audio_may_download})

add_library(wse_audio_vendor STATIC
    engine/src/audio/wse_audio_vendor.cpp
)
set_target_properties(wse_audio_vendor PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_audio_vendor PRIVATE
    "${wse_audio_third_party_dir}"
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/audio"
)
target_link_libraries(wse_audio_vendor PRIVATE wse_engine)
if(NOT WIN32)
    target_link_libraries(wse_audio_vendor INTERFACE m)
endif()
if(MSVC)
    target_compile_options(wse_audio_vendor PRIVATE /w)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wse_audio_vendor PRIVATE -w)
endif()

add_library(wse_audio STATIC
    engine/src/audio/wse_audio_codecs.cpp
)
set_target_properties(wse_audio PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_audio PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/src/audio"
)
target_link_libraries(wse_audio PRIVATE wse_engine wse_audio_vendor)
wse_apply_strict_warnings(wse_audio)

# Third-party Khronos glslang 16.6.0 (GLSL -> Vulkan SPIR-V 1.0 compiler, slice P5).
# Cached under engine/build/third_party/glslang-16.6.0/ when built via build_host.ps1,
# or fetched per build directory with SHA-256 verification.
set(WSE_GLSLANG_URL
    "https://github.com/KhronosGroup/glslang/archive/refs/tags/16.6.0.tar.gz")
set(WSE_GLSLANG_SHA256
    "9c09b901149c729df745057dafa815278aaa101b84d2b6e14f16a42de52f97f2")

if(NOT DEFINED WSE_GLSLANG_SOURCE_DIR AND
   EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/glslang-16.6.0/glslang/Public/ShaderLang.h")
    set(WSE_GLSLANG_SOURCE_DIR
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/build/third_party/glslang-16.6.0")
endif()

if(DEFINED WSE_GLSLANG_SOURCE_DIR)
    set(wse_glslang_src_dir "${WSE_GLSLANG_SOURCE_DIR}")
else()
    set(wse_glslang_tp_dir "${CMAKE_CURRENT_BINARY_DIR}/_third_party/glslang")
    set(wse_glslang_tar "${wse_glslang_tp_dir}/glslang-16.6.0.tar.gz")
    set(wse_glslang_src_dir "${wse_glslang_tp_dir}/glslang-16.6.0")
    file(MAKE_DIRECTORY "${wse_glslang_tp_dir}")
    if(EXISTS "${wse_glslang_tar}")
        file(SHA256 "${wse_glslang_tar}" _wse_glslang_actual_sha)
        if(NOT _wse_glslang_actual_sha STREQUAL WSE_GLSLANG_SHA256)
            file(REMOVE "${wse_glslang_tar}")
            file(REMOVE_RECURSE "${wse_glslang_src_dir}")
        endif()
    endif()
    if(NOT EXISTS "${wse_glslang_tar}")
        message(STATUS "Downloading glslang-16.6.0.tar.gz (SHA-256 verified)...")
        file(DOWNLOAD "${WSE_GLSLANG_URL}" "${wse_glslang_tar}"
             EXPECTED_HASH "SHA256=${WSE_GLSLANG_SHA256}"
             TLS_VERIFY ON
             STATUS _wse_glslang_dl_status)
        list(GET _wse_glslang_dl_status 0 _wse_glslang_dl_code)
        if(NOT _wse_glslang_dl_code EQUAL 0)
            file(REMOVE "${wse_glslang_tar}")
            message(FATAL_ERROR "Failed to download glslang-16.6.0.tar.gz: ${_wse_glslang_dl_status}")
        endif()
    endif()
    if(NOT EXISTS "${wse_glslang_src_dir}/glslang/Public/ShaderLang.h")
        file(ARCHIVE_EXTRACT INPUT "${wse_glslang_tar}" DESTINATION "${wse_glslang_tp_dir}")
    endif()
endif()

if(NOT EXISTS "${wse_glslang_src_dir}/glslang/Public/ShaderLang.h")
    message(FATAL_ERROR "glslang source not found at ${wse_glslang_src_dir}")
endif()

set(wse_glslang_gen_dir "${CMAKE_CURRENT_BINARY_DIR}/_third_party/glslang_gen")
# build_info.h.tmpl expands @major@/@minor@/@patch@/@flavor@; a function keeps those
# generic names out of the directory scope.
function(wse_configure_glslang_build_info src_dir gen_dir)
    set(major 16)
    set(minor 6)
    set(patch 0)
    set(flavor "")
    configure_file("${src_dir}/build_info.h.tmpl" "${gen_dir}/glslang/build_info.h" @ONLY)
endfunction()
wse_configure_glslang_build_info("${wse_glslang_src_dir}" "${wse_glslang_gen_dir}")

if(WIN32)
    set(_wse_glslang_os_src "${wse_glslang_src_dir}/glslang/OSDependent/Windows/ossource.cpp")
else()
    set(_wse_glslang_os_src "${wse_glslang_src_dir}/glslang/OSDependent/Unix/ossource.cpp")
endif()

# Only the C++ API is used, so glslang's C interface sources are left out.
add_library(wse_glslang STATIC
    "${wse_glslang_src_dir}/glslang/GenericCodeGen/CodeGen.cpp"
    "${wse_glslang_src_dir}/glslang/GenericCodeGen/Link.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/glslang_tab.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/attribute.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/Constant.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/iomapper.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/InfoSink.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/Initialize.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/IntermTraverse.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/Intermediate.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/ParseContextBase.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/ParseHelper.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/PoolAlloc.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/RemoveTree.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/Scan.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/ShaderLang.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/SpirvIntrinsics.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/SymbolTable.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/Versions.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/intermOut.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/limits.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/linkValidate.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/parseConst.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/reflection.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/preprocessor/Pp.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/preprocessor/PpAtom.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/preprocessor/PpContext.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/preprocessor/PpScanner.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/preprocessor/PpTokens.cpp"
    "${wse_glslang_src_dir}/glslang/MachineIndependent/propagateNoContraction.cpp"
    "${wse_glslang_src_dir}/glslang/ResourceLimits/ResourceLimits.cpp"
    "${_wse_glslang_os_src}"
    "${wse_glslang_src_dir}/SPIRV/GlslangToSpv.cpp"
    "${wse_glslang_src_dir}/SPIRV/InReadableOrder.cpp"
    "${wse_glslang_src_dir}/SPIRV/Logger.cpp"
    "${wse_glslang_src_dir}/SPIRV/SpvBuilder.cpp"
    "${wse_glslang_src_dir}/SPIRV/SpvPostProcess.cpp"
    "${wse_glslang_src_dir}/SPIRV/doc.cpp"
    "${wse_glslang_src_dir}/SPIRV/SpvTools.cpp"
    "${wse_glslang_src_dir}/SPIRV/disassemble.cpp"
)
set_target_properties(wse_glslang PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    POSITION_INDEPENDENT_CODE ON
)
target_include_directories(wse_glslang PRIVATE
    "${wse_glslang_src_dir}"
    "${wse_glslang_gen_dir}"
)
target_compile_definitions(wse_glslang PRIVATE ENABLE_OPT=0)
if(MSVC)
    target_compile_options(wse_glslang PRIVATE /w)
elseif(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(wse_glslang PRIVATE -w)
endif()

add_library(wse_shader STATIC
    engine/src/shader/wse_shader_compiler.cpp
)
set_target_properties(wse_shader PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_shader PRIVATE
    "${wse_glslang_src_dir}"
    "${wse_glslang_gen_dir}"
)
target_link_libraries(wse_shader PRIVATE wse_engine wse_glslang)
wse_apply_strict_warnings(wse_shader)

# Vulkan runtime driver loader + VulkanRhiDevice (slice P4). Its <vulkan/vulkan.h>
# comes from the vendored headers and stays private: callers see only rhi.h.
add_library(wse_vulkan STATIC platform/vulkan/driver_loader.cpp)
set_target_properties(wse_vulkan PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_vulkan
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/platform/vulkan"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/android/third_party"
)
target_link_libraries(wse_vulkan PUBLIC wse_engine wse_shader)  # P4: built-in GLSL via glslang
if(NOT WIN32)
    target_link_libraries(wse_vulkan PUBLIC ${CMAKE_DL_LIBS})
endif()
wse_apply_strict_warnings(wse_vulkan)

include(CTest)
if((WSE_BUILD_TESTS AND BUILD_TESTING) OR WSE_BUILD_ANDROID_APP)
    add_library(wse_selftest STATIC
        engine/tests/wse_selftest.cpp
        engine/tests/operand_test.cpp
        engine/tests/trigger_test.cpp
        engine/tests/savegame_test.cpp
        engine/tests/script_test.cpp
        engine/tests/wse_bitwise_fp_test.cpp
        engine/tests/wse_system_test.cpp
        engine/tests/wse_string_test.cpp
        engine/tests/wse_dict_test.cpp
        engine/tests/wse_array_test.cpp
        engine/tests/wse_world_test.cpp
        engine/tests/block_vm_test.cpp
        engine/tests/native_world_test.cpp
        engine/tests/native_m5_test.cpp
        engine/tests/wse_m6_test.cpp
        engine/tests/platform_test.cpp
        engine/tests/module_loaders_test.cpp
        engine/tests/headless_test.cpp
        engine/tests/lua_test.cpp
        engine/tests/asset_test.cpp
        engine/tests/audio_test.cpp
        engine/tests/input_test.cpp
        engine/tests/scene_test.cpp
        engine/tests/ui_test.cpp
        engine/tests/recomp_test.cpp
    )
    set_target_properties(wse_selftest PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(wse_selftest PRIVATE WSE_SELF_TEST_LIBRARY=1)
    target_include_directories(wse_selftest PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests"
    )
    target_link_libraries(wse_selftest PUBLIC wse_engine wse_lua wse_vulkan wse_audio wse_shader)
    wse_apply_strict_warnings(wse_selftest)
endif()

if(WSE_BUILD_TESTS AND BUILD_TESTING)
    # Each suite is compiled once, into wse_selftest. A host test executable is
    # engine/tests/suite_main.cpp plus that library, so the linker pulls in just
    # the one suite it names. (The standalone `clang++ engine/tests/<name>_test.cpp`
    # gate in AGENTS.md still works: every suite keeps its own main().)
    function(wse_add_host_test name)
        set(target "${name}_test")
        add_executable("${target}" engine/tests/suite_main.cpp)
        target_compile_definitions("${target}" PRIVATE "WSE_SUITE_FN=run_${name}_suite")
        target_link_libraries("${target}" PRIVATE wse_selftest)
        wse_apply_strict_warnings("${target}")
        add_test(NAME "${name}"
                 COMMAND "${target}" ${ARGN}
                 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endfunction()

    wse_add_host_test(operand)
    wse_add_host_test(trigger)
    wse_add_host_test(savegame)
    wse_add_host_test(script "${CMAKE_CURRENT_SOURCE_DIR}/engine/fixtures/scripts.txt")
    wse_add_host_test(wse_bitwise_fp)
    wse_add_host_test(wse_system)
    wse_add_host_test(wse_string)
    wse_add_host_test(wse_dict)
    wse_add_host_test(wse_array)
    wse_add_host_test(wse_world)
    wse_add_host_test(block_vm)
    wse_add_host_test(native_world)
    wse_add_host_test(native_m5)
    wse_add_host_test(wse_m6)
    wse_add_host_test(platform)
    wse_add_host_test(module_loaders)
    wse_add_host_test(headless)
    wse_add_host_test(lua)
    wse_add_host_test(asset)
    wse_add_host_test(audio)
    wse_add_host_test(input)
    wse_add_host_test(scene)
    wse_add_host_test(ui)
    wse_add_host_test(recomp)

    add_executable(selftest_host_test engine/tests/selftest_host_test.cpp)
    target_link_libraries(selftest_host_test PRIVATE wse_selftest)
    wse_apply_strict_warnings(selftest_host_test)
    add_test(NAME selftest_host
             COMMAND selftest_host_test "${CMAKE_CURRENT_SOURCE_DIR}/engine/fixtures/scripts.txt"
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    set_tests_properties(wse_dict wse_array platform module_loaders headless lua selftest_host
                         PROPERTIES RUN_SERIAL TRUE)
endif()

# Phase 5 translator (`docs/RECOMP_PLAN.md`): x86-64 -> LLVM IR -> native code, linked by an
# embedded LLD. Off by default: it needs an LLVM 18 + LLD build (`-DLLVM_DIR=<prefix>/lib/cmake/llvm`).
# CI builds it on Linux against Ubuntu's `llvm-18-dev` / `liblld-18-dev` (`recomp-host`) and for
# Android against LLVM 18.1.8 built from source (`llvm-android`), where it becomes `libwse_r0.so`.
option(WSE_ENABLE_LLVM "Build the Phase 5 translator (wse_recomp) against LLVM + LLD 18" OFF)
if(WSE_ENABLE_LLVM)
    if(WIN32)
        message(FATAL_ERROR "WSE_ENABLE_LLVM is for Linux and Android builds")
    endif()
    # LLVMConfigVersion.cmake only accepts an exact major.minor request, so check the major here.
    find_package(LLVM REQUIRED CONFIG)
    if(NOT LLVM_VERSION_MAJOR EQUAL 18)
        message(FATAL_ERROR "wse_recomp needs LLVM 18 (found ${LLVM_PACKAGE_VERSION})")
    endif()
    find_package(LLD REQUIRED CONFIG HINTS "${LLVM_DIR}/../lld")
    message(STATUS "wse_recomp: LLVM ${LLVM_PACKAGE_VERSION} (${LLVM_DIR}), targets ${LLVM_TARGETS_TO_BUILD}")
    if(ANDROID)
        set(_wse_llvm_target_components aarch64codegen aarch64desc aarch64info aarch64utils)
    else()
        set(_wse_llvm_target_components nativecodegen)
    endif()
    llvm_map_components_to_libnames(_wse_llvm_libs
        core passes support target mc object option codegen ${_wse_llvm_target_components})
    separate_arguments(_wse_llvm_defs NATIVE_COMMAND "${LLVM_DEFINITIONS}")

    # Zydis 4.1.0 x86-64 decoder (MIT), the amalgamated single-file release, SHA-256 pinned
    # (`docs/RECOMP_PLAN.md` "R1 design"). Offline: -DWSE_ZYDIS_SOURCE_DIR=<dir with Zydis.c/.h>.
    set(WSE_ZYDIS_URL
        "https://github.com/zyantific/zydis/releases/download/v4.1.0/zydis-amalgamated.tar.gz")
    set(WSE_ZYDIS_SHA256 "aa9b82be3a37a2998bd8e16cf583bbf2b6c3d80e97dc20504169dc32ca1ced59")
    if(DEFINED WSE_ZYDIS_SOURCE_DIR)
        set(wse_zydis_src_dir "${WSE_ZYDIS_SOURCE_DIR}")
    else()
        set(wse_zydis_tp_dir "${CMAKE_CURRENT_BINARY_DIR}/_third_party/zydis")
        set(wse_zydis_tar "${wse_zydis_tp_dir}/zydis-amalgamated-4.1.0.tar.gz")
        set(wse_zydis_src_dir "${wse_zydis_tp_dir}/amalgamated-dist")
        file(MAKE_DIRECTORY "${wse_zydis_tp_dir}")
        if(EXISTS "${wse_zydis_tar}")
            file(SHA256 "${wse_zydis_tar}" _wse_zydis_actual_sha)
            if(NOT _wse_zydis_actual_sha STREQUAL WSE_ZYDIS_SHA256)
                file(REMOVE "${wse_zydis_tar}")
                file(REMOVE_RECURSE "${wse_zydis_src_dir}")
            endif()
        endif()
        if(NOT EXISTS "${wse_zydis_tar}")
            message(STATUS "Downloading zydis-amalgamated 4.1.0 (SHA-256 verified)...")
            file(DOWNLOAD "${WSE_ZYDIS_URL}" "${wse_zydis_tar}"
                 EXPECTED_HASH "SHA256=${WSE_ZYDIS_SHA256}" TLS_VERIFY ON
                 STATUS _wse_zydis_dl_status)
            list(GET _wse_zydis_dl_status 0 _wse_zydis_dl_code)
            if(NOT _wse_zydis_dl_code EQUAL 0)
                file(REMOVE "${wse_zydis_tar}")
                message(FATAL_ERROR "Failed to download Zydis: ${_wse_zydis_dl_status}")
            endif()
        endif()
        if(NOT EXISTS "${wse_zydis_src_dir}/Zydis.c")
            file(ARCHIVE_EXTRACT INPUT "${wse_zydis_tar}" DESTINATION "${wse_zydis_tp_dir}")
        endif()
    endif()
    if(NOT EXISTS "${wse_zydis_src_dir}/Zydis.c")
        message(FATAL_ERROR "Zydis source not found at ${wse_zydis_src_dir}")
    endif()
    add_library(wse_zydis STATIC "${wse_zydis_src_dir}/Zydis.c")
    set_target_properties(wse_zydis PROPERTIES POSITION_INDEPENDENT_CODE ON LINKER_LANGUAGE C)
    target_include_directories(wse_zydis SYSTEM PUBLIC "${wse_zydis_src_dir}")
    # Decoder only: the formatter is kept for diagnostics (instruction text in reports).
    target_compile_definitions(wse_zydis PUBLIC ZYDIS_STATIC_BUILD ZYCORE_STATIC_BUILD)
    if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
        target_compile_options(wse_zydis PRIVATE -w)
    endif()

    add_library(wse_recomp STATIC
        engine/src/recomp/lifter_llvm.cpp
        engine/src/recomp/r0_spikes.cpp
        engine/src/recomp/discover.cpp
        engine/src/recomp/lift_r1.cpp
        engine/src/recomp/lift_r1_int.cpp
        engine/src/recomp/lift_r1_sse.cpp
        engine/src/recomp/lift_r1_string.cpp
    )
    set_target_properties(wse_recomp PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_include_directories(wse_recomp SYSTEM PRIVATE ${LLVM_INCLUDE_DIRS} ${LLD_INCLUDE_DIRS})
    target_compile_definitions(wse_recomp PRIVATE ${_wse_llvm_defs})
    if(NOT LLVM_ENABLE_RTTI)
        # Every translator source includes LLVM headers (classes with RTTI-free vtables).
        target_compile_options(wse_recomp PRIVATE -fno-rtti)
    endif()
    target_link_libraries(wse_recomp PUBLIC wse_engine PRIVATE wse_zydis lldELF lldCommon ${_wse_llvm_libs} ${CMAKE_DL_LIBS})
    wse_apply_strict_warnings(wse_recomp)

    if(WSE_BUILD_TESTS AND BUILD_TESTING AND NOT ANDROID)
        add_executable(recomp_llvm_test engine/tests/recomp_llvm_test.cpp)
        target_link_libraries(recomp_llvm_test PRIVATE wse_recomp wse_zydis ${CMAKE_DL_LIBS})
        wse_apply_strict_warnings(recomp_llvm_test)
        add_test(NAME recomp_llvm COMMAND recomp_llvm_test)

        # R1 differential corpus: our own C compiled for Win64 into one PE at 0x140000000 with no
        # relocations or imports (`engine/tests/recomp_corpus/*_corpus.c`; `corpus_entry` is in
        # `int_corpus.c`). CI requires it
        # (-DWSE_REQUIRE_RECOMP_CORPUS=ON); elsewhere the test skips it when the tools are missing.
        option(WSE_REQUIRE_RECOMP_CORPUS "Fail configuration when the R1 Win64 corpus cannot be built" OFF)
        find_program(WSE_CORPUS_CLANG NAMES clang-18 clang HINTS "${LLVM_TOOLS_BINARY_DIR}")
        find_program(WSE_CORPUS_LLD_LINK NAMES lld-link-18 lld-link HINTS "${LLVM_TOOLS_BINARY_DIR}")
        find_program(WSE_CORPUS_DLLTOOL NAMES llvm-dlltool-18 llvm-dlltool HINTS "${LLVM_TOOLS_BINARY_DIR}")
        if(WSE_CORPUS_CLANG AND WSE_CORPUS_LLD_LINK AND WSE_CORPUS_DLLTOOL)
            set(_wse_corpus_dir "${CMAKE_CURRENT_BINARY_DIR}/recomp_corpus")
            set(_wse_corpus_exe "${_wse_corpus_dir}/corpus.exe")
            file(MAKE_DIRECTORY "${_wse_corpus_dir}")
            # -fasynchronous-unwind-tables: .pdata/.xdata for every function, as MSVC emits (the R3
            # exception tests unwind through C frames).
            set(_wse_corpus_common --target=x86_64-pc-windows-msvc -O2 -ffreestanding -fno-builtin -fasynchronous-unwind-tables
                -fno-stack-protector -Wall -Wextra -Werror)
            # Integer and string corpora: no SSE, so every instruction is integer or string.
            set(_wse_corpus_flags_int -mno-sse -mno-sse2 -mno-mmx -fno-vectorize -fno-slp-vectorize)
            set(_wse_corpus_flags_string ${_wse_corpus_flags_int})
            # R3 SEH corpus: integer code calling KERNEL32 through an import library.
            set(_wse_corpus_flags_seh ${_wse_corpus_flags_int})
            # R3 TEB corpus: static TLS, a TLS callback, and the KERNEL32 TLS/FLS calls.
            set(_wse_corpus_flags_teb ${_wse_corpus_flags_int})
            # R4 corpus: time, CRT support, system information, timer queues.
            set(_wse_corpus_flags_crt ${_wse_corpus_flags_int})
            # SSE corpus: SSE4.1 with the vectorizers on.
            set(_wse_corpus_flags_sse -msse4.1)
            set(_wse_corpus_objs)
            foreach(_unit int string sse seh teb crt)
                set(_src "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests/recomp_corpus/${_unit}_corpus.c")
                if(EXISTS "${_src}")
                    set(_obj "${_wse_corpus_dir}/${_unit}_corpus.obj")
                    add_custom_command(
                        OUTPUT "${_obj}"
                        COMMAND "${WSE_CORPUS_CLANG}" ${_wse_corpus_common} ${_wse_corpus_flags_${_unit}}
                                -c "${_src}" -o "${_obj}"
                        DEPENDS "${_src}"
                        COMMENT "R1 corpus: ${_unit}_corpus.c for Win64"
                        VERBATIM)
                    list(APPEND _wse_corpus_objs "${_obj}")
                endif()
            endforeach()
            add_custom_command(
                OUTPUT "${_wse_corpus_exe}"
                COMMAND "${WSE_CORPUS_LLD_LINK}" -nologo "-out:${_wse_corpus_exe}" -entry:corpus_entry
                        -subsystem:console -fixed -base:0x140000000 -nodefaultlib ${_wse_corpus_objs}
                        "${_wse_corpus_dir}/kernel32.lib"
                DEPENDS ${_wse_corpus_objs} "${_wse_corpus_dir}/kernel32.lib"
                COMMENT "R1 corpus: linking the Win64 PE"
                VERBATIM)
            add_custom_command(
                OUTPUT "${_wse_corpus_dir}/kernel32.lib"
                COMMAND "${WSE_CORPUS_DLLTOOL}" -m i386:x86-64
                        -d "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests/recomp_corpus/kernel32.def"
                        -l "${_wse_corpus_dir}/kernel32.lib"
                DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests/recomp_corpus/kernel32.def"
                COMMENT "R3 corpus: KERNEL32 import library"
                VERBATIM)
            add_custom_target(wse_recomp_corpus DEPENDS "${_wse_corpus_exe}")
            add_dependencies(recomp_llvm_test wse_recomp_corpus)
            target_compile_definitions(recomp_llvm_test PRIVATE "WSE_RECOMP_CORPUS=\"${_wse_corpus_exe}\"")
        elseif(WSE_REQUIRE_RECOMP_CORPUS)
            message(FATAL_ERROR "R1 corpus: clang (${WSE_CORPUS_CLANG}), lld-link (${WSE_CORPUS_LLD_LINK}) or llvm-dlltool (${WSE_CORPUS_DLLTOOL}) not found")
        endif()
    endif()

    if(WSE_BUILD_ANDROID_APP)
        add_library(wse_r0 SHARED platform/android/r0_jni.cpp)
        target_link_libraries(wse_r0 PRIVATE wse_recomp log)
        target_link_options(wse_r0 PRIVATE "-Wl,--gc-sections" "-Wl,--exclude-libs,ALL")
        wse_apply_strict_warnings(wse_r0)
    endif()
endif()

if(WSE_BUILD_TOOLS)
    add_executable(wse_headless engine/tools/wse_headless.cpp)
    target_link_libraries(wse_headless PRIVATE wse_engine wse_lua)
    wse_apply_strict_warnings(wse_headless)

    add_executable(wse_brf engine/tools/wse_brf.cpp)
    target_link_libraries(wse_brf PRIVATE wse_engine wse_shader)
    wse_apply_strict_warnings(wse_brf)
endif()

if(WSE_BUILD_ANDROID_APP)
    if(NOT ANDROID)
        message(FATAL_ERROR "WSE_BUILD_ANDROID_APP=ON requires an Android toolchain build")
    endif()
    find_package(SDL3 REQUIRED CONFIG)
    add_library(panvk_hw SHARED
        platform/android/panvk_hw_stub.c
    )
    wse_apply_strict_warnings(panvk_hw)

    add_library(wse2 SHARED
        platform/sdl/sdl_selftest_main.cpp
        platform/sdl/sdl_audio.cpp
        platform/sdl/sdl_input.cpp
        platform/android/perf_hint.cpp
        platform/android/saf_vfs_jni.cpp
    )
    target_link_libraries(wse2 PRIVATE
        wse_selftest
        SDL3::SDL3
        android
        log
    )
    wse_apply_strict_warnings(wse2)
endif()
