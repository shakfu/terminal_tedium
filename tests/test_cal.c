#include "tt_test.h"
#include "tedium/tedium.h"

#include <stdio.h>
#include <unistd.h>

static const char *tmp_path(void)
{
    static char buf[512];
    const char *dir = getenv("TMPDIR");
    if (!dir || !*dir) dir = "/tmp";
    snprintf(buf, sizeof(buf), "%s/tt_cal_test_%d.json", dir, (int)getpid());
    return buf;
}

static void write_text(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TT_ASSERT_NOT_NULL(f);
    fputs(text, f);
    fclose(f);
}

TT_TEST(cal, nominal_matches_hardware_transfer)
{
    tt_cal c;
    tt_cal_nominal(&c, TT_BOARD_WM8731);

    /* Code 2048 is the centre of a +/-5 V input, and the front end inverts,
     * so the scale must be negative. */
    TT_ASSERT_NEAR(2048.0, c.ch[0].offset, 1e-6);
    TT_ASSERT_NEAR(-10.0 / 4096.0, c.ch[0].scale, 1e-9);

    /* Round-trip the three points that matter. */
    TT_ASSERT_NEAR(0.0, (0.0 + (2048.0 - c.ch[0].offset) * c.ch[0].scale), 1e-6);
    TT_ASSERT_NEAR(5.0, (0.0 - c.ch[0].offset) * c.ch[0].scale, 1e-4);
    TT_ASSERT_NEAR(-4.9976, (4095.0 - c.ch[0].offset) * c.ch[0].scale, 1e-3);
}

TT_TEST(cal, save_load_roundtrip)
{
    tt_cal a, b;
    const char *path = tmp_path();
    int i;

    tt_cal_nominal(&a, TT_BOARD_PCM5102A);
    for (i = 0; i < TT_MAX_CV; i++) {
        a.ch[i].offset = 2000.0f + (float)i;
        a.ch[i].scale  = -0.00244f - (float)i * 1e-6f;
    }

    TT_ASSERT_EQ_INT(0, tt_cal_save_file(&a, path));

    tt_cal_nominal(&b, TT_BOARD_PCM5102A);
    TT_ASSERT_EQ_INT(0, tt_cal_load_file(&b, path));

    for (i = 0; i < TT_MAX_CV; i++) {
        TT_ASSERT_NEAR(a.ch[i].offset, b.ch[i].offset, 1e-3);
        TT_ASSERT_NEAR(a.ch[i].scale, b.ch[i].scale, 1e-9);
    }
    remove(path);
}

TT_TEST(cal, missing_file_reports_failure)
{
    tt_cal c;
    tt_cal_nominal(&c, TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(-1, tt_cal_load_file(&c, "/nonexistent/nowhere/cal.json"));
    /* The struct must be left usable. */
    TT_ASSERT_NEAR(2048.0, c.ch[0].offset, 1e-6);
}

TT_TEST(cal, partial_file_keeps_nominal_for_rest)
{
    tt_cal c;
    const char *path = tmp_path();

    /* Only two channels present, and the second omits "scale". */
    write_text(path,
        "{\n"
        "  \"version\": 1,\n"
        "  \"channels\": [\n"
        "    { \"offset\": 1000.5, \"scale\": -0.002 },\n"
        "    { \"offset\": 1234.0 }\n"
        "  ]\n"
        "}\n");

    tt_cal_nominal(&c, TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(0, tt_cal_load_file(&c, path));

    TT_ASSERT_NEAR(1000.5, c.ch[0].offset, 1e-4);
    TT_ASSERT_NEAR(-0.002, c.ch[0].scale, 1e-9);

    TT_ASSERT_NEAR(1234.0, c.ch[1].offset, 1e-4);
    TT_ASSERT_NEAR(-10.0 / 4096.0, c.ch[1].scale, 1e-9);   /* untouched */

    TT_ASSERT_NEAR(2048.0, c.ch[2].offset, 1e-4);          /* untouched */
    remove(path);
}

TT_TEST(cal, malformed_file_is_rejected)
{
    tt_cal c;
    const char *path = tmp_path();

    write_text(path, "{ \"version\": 1, \"nothing\": true }\n");
    tt_cal_nominal(&c, TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(-1, tt_cal_load_file(&c, path));
    TT_ASSERT_NEAR(2048.0, c.ch[0].offset, 1e-6);
    remove(path);
}

TT_TEST(cal, default_path_is_under_config_home)
{
    char buf[512];
    size_t n = tt_cal_default_path(buf, sizeof(buf));
    TT_ASSERT(n > 0);
    TT_ASSERT(strstr(buf, "terminal_tedium/cal.json") != NULL);
}

TT_TEST(cal, save_creates_missing_directories)
{
    tt_cal c;
    char dir[512], path[600];
    const char *tmpd = getenv("TMPDIR");
    if (!tmpd || !*tmpd) tmpd = "/tmp";

    snprintf(dir, sizeof(dir), "%s/tt_caldir_%d/a/b", tmpd, (int)getpid());
    snprintf(path, sizeof(path), "%s/cal.json", dir);

    tt_cal_nominal(&c, TT_BOARD_WM8731);
    TT_ASSERT_EQ_INT(0, tt_cal_save_file(&c, path));
    TT_ASSERT_EQ_INT(0, tt_cal_load_file(&c, path));

    remove(path);
}
