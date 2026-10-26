#pragma once

#include <cstdio>

#define TEST_EXPECT(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, \
                         __LINE__, #expr);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

// The suite's soft check, shared by the tests that keep going past a failed expectation: a false
// `ok` prints "FAIL: <what>" to stderr and counts into test_expect::failures; the value passes
// through, so `if (!expect(...)) return false;` chains as it reads. A test that counts its own
// failures binds its counter to test_expect::failures.
namespace test_expect {
inline int failures = 0;
inline bool expect(bool ok, const char *what) {
    if (!ok) {
        std::fprintf(stderr, "FAIL: %s\n", what);
        ++failures;
    }
    return ok;
}
} // namespace test_expect
