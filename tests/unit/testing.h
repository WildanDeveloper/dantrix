/*
 * testing.h - Minimal dependency-free test harness for the unit tests.
 *
 * A failed check is recorded, not fatal, so one run reports every failure.
 * The process exit code is non-zero when any check failed.
 */
#ifndef DX_TESTING_H
#define DX_TESTING_H

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int dx_test_count = 0;
static int dx_test_failed = 0;
static const char *dx_test_group = "";

static void dx_test_begin(const char *group)
{
    dx_test_group = group;
}

static int dx_test_summary(void)
{
    printf("%d checks, %d failed\n", dx_test_count, dx_test_failed);
    return (dx_test_failed == 0) ? 0 : 1;
}

#define CHECK(cond)                                                          \
    do {                                                                     \
        dx_test_count++;                                                     \
        if (!(cond)) {                                                       \
            dx_test_failed++;                                                \
            fprintf(stderr, "FAIL [%s] %s:%d: %s\n", dx_test_group,          \
                    __FILE__, __LINE__, #cond);                              \
        }                                                                    \
    } while (0)

/* Compare two values that promote to long long (sizes, ints, pointers as integers). */
#define CHECK_EQ(a, b)                                                       \
    do {                                                                     \
        long long va_ = (long long)(a);                                      \
        long long vb_ = (long long)(b);                                      \
        dx_test_count++;                                                     \
        if (va_ != vb_) {                                                    \
            dx_test_failed++;                                                \
            fprintf(stderr, "FAIL [%s] %s:%d: %s == %s (%lld != %lld)\n",    \
                    dx_test_group, __FILE__, __LINE__, #a, #b, va_, vb_);    \
        }                                                                    \
    } while (0)

#define CHECK_NE(a, b)                                                       \
    do {                                                                     \
        long long va_ = (long long)(a);                                      \
        long long vb_ = (long long)(b);                                      \
        dx_test_count++;                                                     \
        if (va_ == vb_) {                                                    \
            dx_test_failed++;                                                \
            fprintf(stderr, "FAIL [%s] %s:%d: %s != %s (%lld)\n",            \
                    dx_test_group, __FILE__, __LINE__, #a, va_);             \
        }                                                                    \
    } while (0)

#endif /* DX_TESTING_H */
