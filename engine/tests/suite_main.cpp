// Entry point for one host test executable in the CMake build. The suite
// itself is compiled once, into the wse_selftest library; this file only
// selects which suite to run (WSE_SUITE_FN, e.g. run_operand_suite).

#include "wse_selftest.h"

#ifndef WSE_SUITE_FN
#error "define WSE_SUITE_FN to a wse::selftest::run_<name>_suite function"
#endif

int main(int argc, char** argv) {
    return wse::selftest::WSE_SUITE_FN(argc > 1 ? argv[1] : nullptr);
}
