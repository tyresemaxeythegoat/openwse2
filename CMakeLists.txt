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

# Vulkan runtime driver loader + VulkanRhiDevice (slice P4). Its <vulkan/vulkan.h>
# comes from the vendored headers and stays private: callers see only rhi.h.
add_library(wse_vulkan STATIC platform/vulkan/driver_loader.cpp)
set_target_properties(wse_vulkan PROPERTIES POSITION_INDEPENDENT_CODE ON)
target_include_directories(wse_vulkan
    PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/platform/vulkan"
    PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/runtime/android/third_party"
)
target_link_libraries(wse_vulkan PUBLIC wse_engine)
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
    )
    set_target_properties(wse_selftest PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(wse_selftest PRIVATE WSE_SELF_TEST_LIBRARY=1)
    target_include_directories(wse_selftest PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests"
    )
    target_link_libraries(wse_selftest PUBLIC wse_engine wse_lua wse_vulkan wse_audio)
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

    add_executable(selftest_host_test engine/tests/selftest_host_test.cpp)
    target_link_libraries(selftest_host_test PRIVATE wse_selftest)
    wse_apply_strict_warnings(selftest_host_test)
    add_test(NAME selftest_host
             COMMAND selftest_host_test "${CMAKE_CURRENT_SOURCE_DIR}/engine/fixtures/scripts.txt"
             WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")

    set_tests_properties(wse_dict wse_array platform module_loaders headless lua selftest_host
                         PROPERTIES RUN_SERIAL TRUE)
endif()

if(WSE_BUILD_TOOLS)
    add_executable(wse_headless engine/tools/wse_headless.cpp)
    target_link_libraries(wse_headless PRIVATE wse_engine wse_lua)
    wse_apply_strict_warnings(wse_headless)

    add_executable(wse_brf engine/tools/wse_brf.cpp)
    target_link_libraries(wse_brf PRIVATE wse_engine)
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
