#include "tt_test.h"
#include "tedium/tedium.h"
#include "tt_board.h"

TT_TEST(board, channel_counts)
{
    TT_ASSERT_EQ_INT(6, tt_board_num_cv(TT_BOARD_WM8731));
    TT_ASSERT_EQ_INT(8, tt_board_num_cv(TT_BOARD_PCM5102A));
}

TT_TEST(board, names)
{
    TT_ASSERT_STR_EQ("wm8731", tt_board_name(TT_BOARD_WM8731));
    TT_ASSERT_STR_EQ("pcm5102a", tt_board_name(TT_BOARD_PCM5102A));
}

TT_TEST(board, unknown_id_falls_back)
{
    const tt_board_desc *d = tt_board_get((tt_board)99);
    TT_ASSERT_NOT_NULL(d);
    TT_ASSERT_STR_EQ("wm8731", d->name);
}

TT_TEST(board, wm8731_panel_map_is_identity)
{
    const tt_board_desc *d = tt_board_get(TT_BOARD_WM8731);
    int i;
    for (i = 0; i < d->num_cv; i++)
        TT_ASSERT_EQ_INT(i, d->panel_from_adc[i]);
}

TT_TEST(board, pcm5102a_panel_map_is_a_permutation)
{
    const tt_board_desc *d = tt_board_get(TT_BOARD_PCM5102A);
    int seen[TT_MAX_CV];
    int i;

    /* The map came from two independent legacy sources; a duplicated or
     * missing entry would silently swap two panel jacks. */
    memset(seen, 0, sizeof(seen));
    for (i = 0; i < d->num_cv; i++) {
        int a = d->panel_from_adc[i];
        TT_ASSERT(a >= 0 && a < d->num_cv);
        TT_ASSERT_EQ_INT(0, seen[a]);
        seen[a] = 1;
    }
}

TT_TEST(board, pcm5102a_panel_map_matches_legacy_outlets)
{
    /* terminal_tedium_adc.c mapped outlet N to a2dVal[] in this order. */
    static const int want[8] = { 5, 6, 1, 4, 7, 0, 3, 2 };
    const tt_board_desc *d = tt_board_get(TT_BOARD_PCM5102A);
    int i;
    for (i = 0; i < 8; i++)
        TT_ASSERT_EQ_INT(want[i], d->panel_from_adc[i]);
}

TT_TEST(board, gpio_assignments_differ_between_variants)
{
    const tt_board_desc *w = tt_board_get(TT_BOARD_WM8731);
    const tt_board_desc *p = tt_board_get(TT_BOARD_PCM5102A);

    /* Gates: wm8731 uses 12/16, pcm5102a uses 16/26. */
    TT_ASSERT_EQ_INT(12, w->gate_gpio[0]);
    TT_ASSERT_EQ_INT(16, w->gate_gpio[1]);
    TT_ASSERT_EQ_INT(16, p->gate_gpio[0]);
    TT_ASSERT_EQ_INT(26, p->gate_gpio[1]);

    /* Triggers: on a wm8731 board GPIO 2 and 3 are the I2C lines, so the
     * trigger inputs must not be assigned there. externals/ReadMe.md gets
     * this wrong; the OSC client gets it right. */
    TT_ASSERT_EQ_INT(14, w->trigger_gpio[2]);
    TT_ASSERT_EQ_INT(27, w->trigger_gpio[3]);
    TT_ASSERT_EQ_INT(2, p->trigger_gpio[2]);
    TT_ASSERT_EQ_INT(3, p->trigger_gpio[3]);
}

TT_TEST(board, no_gpio_is_used_twice_on_a_board)
{
    tt_board ids[2] = { TT_BOARD_WM8731, TT_BOARD_PCM5102A };
    int b;

    for (b = 0; b < 2; b++) {
        const tt_board_desc *d = tt_board_get(ids[b]);
        int used[64];
        int i;
        memset(used, 0, sizeof(used));

        for (i = 0; i < d->num_triggers; i++) {
            TT_ASSERT_EQ_INT(0, used[d->trigger_gpio[i]]);
            used[d->trigger_gpio[i]] = 1;
        }
        for (i = 0; i < d->num_gates; i++) {
            TT_ASSERT_EQ_INT(0, used[d->gate_gpio[i]]);
            used[d->gate_gpio[i]] = 1;
        }
        for (i = 0; i < d->num_buttons; i++) {
            TT_ASSERT_EQ_INT(0, used[d->button_gpio[i]]);
            used[d->button_gpio[i]] = 1;
        }
    }
}

TT_TEST(board, config_init_sets_usable_defaults)
{
    tt_config cfg;
    tt_config_init(&cfg, TT_BOARD_PCM5102A);

    TT_ASSERT_EQ_INT(TT_BOARD_PCM5102A, cfg.board);
    TT_ASSERT_EQ_INT(TT_HAL_AUTO, cfg.hal);
    TT_ASSERT_EQ_INT(4000, cfg.scan_rate_hz);
    TT_ASSERT_EQ_INT(4000000, cfg.spi_speed_hz);
    TT_ASSERT_EQ_INT(1024, cfg.ring_scans);
    TT_ASSERT(cfg.spi_dev == NULL);
    TT_ASSERT(cfg.cal_path == NULL);
}
