cmake_minimum_required(VERSION 3.20)
project(wse2_android LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(wse_engine INTERFACE)
target_include_directories(wse_engine INTERFACE
    "${CMAKE_CURRENT_SOURCE_DIR}/engine/include"
)
target_compile_features(wse_engine INTERFACE cxx_std_20)

option(WSE_BUILD_TESTS "Build the engine C++20 unit test binaries" ON)
option(WSE_BUILD_HEADER_CHECKS "Compile every engine header standalone" OFF)

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

include(CTest)
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
    wse_add_host_test(wse      engine/tests/wse_test.cpp)
    wse_add_host_test(block_vm engine/tests/block_vm_test.cpp)
    set_tests_properties(wse PROPERTIES RUN_SERIAL TRUE)
endif()
