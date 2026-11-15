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
    )
    set_target_properties(wse_selftest PROPERTIES POSITION_INDEPENDENT_CODE ON)
    target_compile_definitions(wse_selftest PRIVATE WSE_SELF_TEST_LIBRARY=1)
    target_include_directories(wse_selftest PUBLIC
        "${CMAKE_CURRENT_SOURCE_DIR}/engine/tests"
    )
    target_link_libraries(wse_selftest PUBLIC wse_engine wse_lua)
    wse_apply_strict_warnings(wse_selftest)
endif()

if(WSE_BUILD_TESTS AND BUILD_TESTING)
    function(wse_add_host_test name source)
        set(target "${name}_test")
        add_executable("${target}" "${source}")
        target_link_libraries("${target}" PRIVATE wse_engine)
        wse_apply_strict_warnings("${target}")
        add_test(NAME "${name}"
                 COMMAND "${target}" ${ARGN}
                 WORKING_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}")
    endfunction()

    wse_add_host_test(operand  engine/tests/operand_test.cpp)
    wse_add_host_test(trigger  engine/tests/trigger_test.cpp)
    wse_add_host_test(savegame engine/tests/savegame_test.cpp)
    wse_add_host_test(script   engine/tests/script_test.cpp
                      "${CMAKE_CURRENT_SOURCE_DIR}/engine/fixtures/scripts.txt")
    wse_add_host_test(wse_bitwise_fp engine/tests/wse_bitwise_fp_test.cpp)
    wse_add_host_test(wse_system     engine/tests/wse_system_test.cpp)
    wse_add_host_test(wse_string     engine/tests/wse_string_test.cpp)
    wse_add_host_test(wse_dict       engine/tests/wse_dict_test.cpp)
    wse_add_host_test(wse_array      engine/tests/wse_array_test.cpp)
    wse_add_host_test(wse_world      engine/tests/wse_world_test.cpp)
    wse_add_host_test(block_vm       engine/tests/block_vm_test.cpp)
    wse_add_host_test(native_world   engine/tests/native_world_test.cpp)
    wse_add_host_test(native_m5      engine/tests/native_m5_test.cpp)
    wse_add_host_test(wse_m6         engine/tests/wse_m6_test.cpp)
    wse_add_host_test(platform       engine/tests/platform_test.cpp)
    wse_add_host_test(module_loaders engine/tests/module_loaders_test.cpp)
    wse_add_host_test(headless       engine/tests/headless_test.cpp)
    wse_add_host_test(lua            engine/tests/lua_test.cpp)
    target_link_libraries(lua_test PRIVATE wse_lua)

    wse_add_host_test(selftest_host  engine/tests/selftest_host_test.cpp
                      "${CMAKE_CURRENT_SOURCE_DIR}/engine/fixtures/scripts.txt")
    target_link_libraries(selftest_host_test PRIVATE wse_selftest)

    set_tests_properties(wse_dict wse_array platform module_loaders headless lua selftest_host
                         PROPERTIES RUN_SERIAL TRUE)
endif()

if(WSE_BUILD_TOOLS)
    add_executable(wse_headless engine/tools/wse_headless.cpp)
    target_link_libraries(wse_headless PRIVATE wse_engine wse_lua)
    wse_apply_strict_warnings(wse_headless)
endif()

if(WSE_BUILD_ANDROID_APP)
    if(NOT ANDROID)
        message(FATAL_ERROR "WSE_BUILD_ANDROID_APP=ON requires an Android toolchain build")
    endif()
    find_package(SDL3 REQUIRED CONFIG)
    add_library(wse2 SHARED
        platform/sdl/sdl_selftest_main.cpp
    )
    target_link_libraries(wse2 PRIVATE
        wse_selftest
        SDL3::SDL3
        android
        log
    )
    wse_apply_strict_warnings(wse2)
endif()
