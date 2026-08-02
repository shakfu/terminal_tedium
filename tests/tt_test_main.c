#include "tt_test.h"

#include <stdarg.h>

#define TT_MAX_TESTS 512

typedef struct {
    const char *suite;
    const char *name;
    tt_test_fn  fn;
} tt_test_case;

static tt_test_case g_tests[TT_MAX_TESTS];
static int          g_ntests;
static const char  *g_cur_file;
static int          g_failed;

jmp_buf tt_test_jmp;

void tt_test_register(const char *suite, const char *name, tt_test_fn fn)
{
    if (g_ntests >= TT_MAX_TESTS) {
        fprintf(stderr, "tt_test: too many tests, raise TT_MAX_TESTS\n");
        abort();
    }
    g_tests[g_ntests].suite = suite;
    g_tests[g_ntests].name  = name;
    g_tests[g_ntests].fn    = fn;
    g_ntests++;
}

void tt_test_fail(const char *file, int line, const char *fmt, ...)
{
    va_list ap;
    fprintf(stderr, "\n    FAIL %s:%d\n      ", file, line);
    va_start(ap, fmt);
    vfprintf(stderr, fmt, ap);
    va_end(ap);
    fputc('\n', stderr);
    g_failed = 1;
    longjmp(tt_test_jmp, 1);
}

/* Registration order is link order, which is not stable enough to rely on for
 * readable output; sort by suite then name. */
static int cmp_tests(const void *a, const void *b)
{
    const tt_test_case *x = (const tt_test_case *)a;
    const tt_test_case *y = (const tt_test_case *)b;
    int c = strcmp(x->suite, y->suite);
    if (c) return c;
    return strcmp(x->name, y->name);
}

int main(int argc, char **argv)
{
    const char *filter = (argc > 1) ? argv[1] : NULL;
    int passed = 0, failed = 0, skipped = 0;
    const char *last_suite = NULL;
    int i;

    qsort(g_tests, (size_t)g_ntests, sizeof(g_tests[0]), cmp_tests);

    for (i = 0; i < g_ntests; i++) {
        tt_test_case *tc = &g_tests[i];
        char full[256];

        snprintf(full, sizeof(full), "%s.%s", tc->suite, tc->name);
        if (filter && !strstr(full, filter)) { skipped++; continue; }

        if (!last_suite || strcmp(last_suite, tc->suite) != 0) {
            printf("\n%s\n", tc->suite);
            last_suite = tc->suite;
        }

        printf("  %-44s", tc->name);
        fflush(stdout);

        g_failed = 0;
        g_cur_file = NULL;
        (void)g_cur_file;

        if (setjmp(tt_test_jmp) == 0) {
            tc->fn();
        }

        if (g_failed) { failed++; printf("    FAILED\n"); }
        else          { passed++; printf("ok\n"); }
    }

    printf("\n--------------------------------------------------\n");
    printf("%d passed, %d failed", passed, failed);
    if (skipped) printf(", %d filtered out", skipped);
    printf("\n");

    return failed ? 1 : 0;
}
