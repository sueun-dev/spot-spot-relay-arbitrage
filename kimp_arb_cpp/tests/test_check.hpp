#pragma once

// NDEBUG-proof assertion for test binaries.
//
// The project builds tests in Release (-DNDEBUG), which strips assert(), turning
// assert-only tests into no-ops that "pass" without checking anything. KIMP_CHECK
// performs the same check in every build type: on failure it prints the failed
// expression with file/line and exits non-zero so CTest / shell runners see it.

#include <cstdio>
#include <cstdlib>

#define KIMP_CHECK(cond)                                                       \
    do {                                                                       \
        if (!(cond)) {                                                         \
            std::fprintf(stderr, "CHECK failed: %s\n  at %s:%d\n", #cond,      \
                         __FILE__, __LINE__);                                  \
            std::exit(1);                                                      \
        }                                                                      \
    } while (0)
