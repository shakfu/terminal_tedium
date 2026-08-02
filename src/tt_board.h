/*
 * Board descriptors.
 *
 * The two Terminal Tedium revisions differ in CV channel count, in how the
 * MCP3208's channels map onto the panel jacks, and in which GPIOs carry the
 * gate outputs and trigger inputs. Encoding that here means the rest of the
 * library, and every binding, is variant-agnostic.
 *
 * Trigger and button inputs are pulled up and pull low when active. The HAL
 * normalises that, so everything above this layer sees active == 1.
 */

#ifndef TT_BOARD_H
#define TT_BOARD_H

#include "tedium/tedium.h"

typedef struct {
    tt_board    id;
    const char *name;
    int         num_cv;

    /* panel_from_adc[p] is the MCP3208 channel feeding panel input p. */
    uint8_t     panel_from_adc[TT_MAX_CV];

    int         num_triggers;
    uint8_t     trigger_gpio[TT_MAX_TRIGGERS];

    int         num_gates;
    uint8_t     gate_gpio[TT_MAX_GATES];

    int         num_buttons;
    uint8_t     button_gpio[TT_MAX_BUTTONS];
} tt_board_desc;

/* Never returns NULL; an unknown id falls back to the wm8731 descriptor. */
const tt_board_desc *tt_board_get(tt_board id);

#endif /* TT_BOARD_H */
