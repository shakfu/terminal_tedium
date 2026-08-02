#include "tt_board.h"

#include <string.h>

/*
 * Pin assignments recovered from the legacy externals and the OSC client:
 *
 *   wm8731    gates   GPIO 12, 16      (tedium_output.c)
 *             trigs   GPIO 4, 17, 14, 27  (OSC client/main.c TR1..TR4)
 *             buttons GPIO 23, 25, 24     (OSC client/main.c B1..B3)
 *
 *   pcm5102a  gates   GPIO 16, 26      (tedium_output.c)
 *             trigs   GPIO 4, 17, 2, 3    (externals/ReadMe.md, adc2FUDI.c)
 *             buttons GPIO 23, 24, 25
 *
 * Note that externals/ReadMe.md lists the pcm5102a trigger pins for both
 * variants, which is wrong for wm8731 boards -- GPIO 2 and 3 are the I2C
 * lines there. The OSC client has it right.
 *
 * The pcm5102a panel map is the inverse of adc2FUDI.c's map_adc[] and matches
 * the outlet ordering in terminal_tedium_adc.c; the two legacy sources agree.
 */

static const tt_board_desc k_boards[] = {
    {
        TT_BOARD_WM8731, "wm8731", 6,
        { 0, 1, 2, 3, 4, 5, 6, 7 },
        4, { 4, 17, 14, 27 },
        2, { 12, 16 },
        3, { 23, 25, 24 }
    },
    {
        TT_BOARD_PCM5102A, "pcm5102a", 8,
        { 5, 6, 1, 4, 7, 0, 3, 2 },
        4, { 4, 17, 2, 3 },
        2, { 16, 26 },
        3, { 23, 24, 25 }
    }
};

const tt_board_desc *tt_board_get(tt_board id)
{
    size_t i;
    for (i = 0; i < sizeof(k_boards) / sizeof(k_boards[0]); i++)
        if (k_boards[i].id == id) return &k_boards[i];
    return &k_boards[0];
}

int tt_board_num_cv(tt_board board)
{
    return tt_board_get(board)->num_cv;
}

const char *tt_board_name(tt_board board)
{
    return tt_board_get(board)->name;
}

void tt_config_init(tt_config *cfg, tt_board board)
{
    memset(cfg, 0, sizeof(*cfg));
    cfg->board        = board;
    cfg->hal          = TT_HAL_AUTO;
    cfg->scan_rate_hz = 4000;
    cfg->spi_speed_hz = 4000000;
    cfg->rt_priority  = 0;
    cfg->spi_dev      = NULL;
    cfg->gpio_chip    = NULL;
    cfg->cal_path     = NULL;
    cfg->ring_scans   = 1024;
}
