#ifndef OPENNOVA_TEST_EXPECT_H
#define OPENNOVA_TEST_EXPECT_H

#include <cstdio>

#define TEST_EXPECT(expr)                                                      \
    do {                                                                       \
        if (!(expr)) {                                                         \
            std::fprintf(stderr, "%s:%d: expectation failed: %s\n", __FILE__, \
                         __LINE__, #expr);                                    \
            return 1;                                                          \
        }                                                                      \
    } while (0)

#endif // OPENNOVA_TEST_EXPECT_H
