#include "wse_selftest.h"

#include <cstdio>

int main(int argc, char** argv) {
    const char* fixture = argc > 1 ? argv[1] : "engine/fixtures/scripts.txt";
    const wse::selftest::SuiteSummary summary =
        wse::selftest::run_all_suites(fixture);

    for (const auto& res : summary.results) {
        std::printf("[%s] %-16s (%lld ms)\n", res.passed ? "PASS" : "FAIL",
                    res.name, static_cast<long long>(res.duration_ms));
    }
    if (!summary.all_passed()) {
        std::printf("selftest_host tests: FAIL (%d/%d passed)\n",
                    summary.passed_count, summary.total_count);
        return 1;
    }
    std::printf("selftest_host tests: PASS (%d/%d, %lld ms)\n",
                summary.passed_count, summary.total_count,
                static_cast<long long>(summary.total_ms));
    return 0;
}
