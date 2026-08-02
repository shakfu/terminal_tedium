/*
 * Minimal zero-dependency test harness.
 *
 * Tests self-register via constructors, so adding a test file to the build is
 * the only wiring required. Assertions long-jump out of the failing test so a
 * failure never cascades into a crash that hides the message.
 *
 *     TT_TEST(ring, push_then_read)
 *     {
 *         TT_ASSERT_EQ_INT(1, 1);
 *     }
 */

#ifndef TT_TEST_H
#define TT_TEST_H

#include <setjmp.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>

typedef void (*tt_test_fn)(void);

void tt_test_register(const char *suite, const char *name, tt_test_fn fn);
void tt_test_fail(const char *file, int line, const char *fmt, ...);

/* Set by the runner before each test; assertions jump here on failure. */
extern jmp_buf tt_test_jmp;

#define TT_TEST(suite, name)                                                  \
    static void tt_tc_##suite##_##name(void);                                 \
    __attribute__((constructor))                                              \
    static void tt_reg_##suite##_##name(void)                                 \
    {                                                                         \
        tt_test_register(#suite, #name, tt_tc_##suite##_##name);              \
    }                                                                         \
    static void tt_tc_##suite##_##name(void)

#define TT_ASSERT(cond)                                                       \
    do {                                                                      \
        if (!(cond))                                                          \
            tt_test_fail(__FILE__, __LINE__, "assertion failed: %s", #cond);  \
    } while (0)

#define TT_ASSERT_EQ_INT(want, got)                                           \
    do {                                                                      \
        long long _w = (long long)(want), _g = (long long)(got);              \
        if (_w != _g)                                                         \
            tt_test_fail(__FILE__, __LINE__,                                  \
                         "%s: want %lld, got %lld", #got, _w, _g);            \
    } while (0)

#define TT_ASSERT_EQ_U64(want, got)                                           \
    do {                                                                      \
        unsigned long long _w = (unsigned long long)(want);                   \
        unsigned long long _g = (unsigned long long)(got);                    \
        if (_w != _g)                                                         \
            tt_test_fail(__FILE__, __LINE__,                                  \
                         "%s: want %llu, got %llu", #got, _w, _g);            \
    } while (0)

#define TT_ASSERT_NEAR(want, got, eps)                                        \
    do {                                                                      \
        double _w = (double)(want), _g = (double)(got), _e = (double)(eps);   \
        if (!(fabs(_w - _g) <= _e))                                           \
            tt_test_fail(__FILE__, __LINE__,                                  \
                         "%s: want %g +/- %g, got %g (delta %g)",             \
                         #got, _w, _e, _g, fabs(_w - _g));                    \
    } while (0)

#define TT_ASSERT_STR_EQ(want, got)                                           \
    do {                                                                      \
        const char *_w = (want), *_g = (got);                                 \
        if (!_g || strcmp(_w, _g) != 0)                                       \
            tt_test_fail(__FILE__, __LINE__,                                  \
                         "%s: want \"%s\", got \"%s\"", #got, _w,             \
                         _g ? _g : "(null)");                                 \
    } while (0)

#define TT_ASSERT_NOT_NULL(p)                                                 \
    do {                                                                      \
        if ((p) == NULL)                                                      \
            tt_test_fail(__FILE__, __LINE__, "%s is NULL", #p);               \
    } while (0)

#define TT_FAIL(...) tt_test_fail(__FILE__, __LINE__, __VA_ARGS__)

#endif /* TT_TEST_H */
