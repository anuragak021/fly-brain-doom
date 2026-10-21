/**
 * @file test_runner.h
 * @brief Minimal assert based test runner for host unit tests (no external dependency).
 *
 * Each test is a void function; CHECK records a failure and keeps going so one run reports
 * every broken expectation. main() returns nonzero if anything failed, which ctest reports.
 */
#ifndef TEST_RUNNER_H
#define TEST_RUNNER_H

#include <stdio.h>

static unsigned g_checks;
static unsigned g_failures;

#define CHECK(cond)                                                                                \
    do {                                                                                           \
        g_checks++;                                                                                \
        if (!(cond)) {                                                                             \
            g_failures++;                                                                          \
            printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond);                                 \
        }                                                                                          \
    } while (0)

#define CHECK_EQ_INT(a, b)                                                                         \
    do {                                                                                           \
        g_checks++;                                                                                \
        const long long va_ = (long long)(a);                                                      \
        const long long vb_ = (long long)(b);                                                      \
        if (va_ != vb_) {                                                                          \
            g_failures++;                                                                          \
            printf("FAIL %s:%d: %s == %s (%lld != %lld)\n", __FILE__, __LINE__, #a, #b, va_, vb_); \
        }                                                                                          \
    } while (0)

#define RUN(test)                                                                                  \
    do {                                                                                           \
        test();                                                                                    \
        printf("ran %s\n", #test);                                                                 \
    } while (0)

#define TEST_REPORT()                                                                              \
    (printf("%u checks, %u failures\n", g_checks, g_failures), (g_failures == 0U) ? 0 : 1)

#endif /* TEST_RUNNER_H */
