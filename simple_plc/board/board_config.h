#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

/*
 * Board flags tested by board/board_tag_define.h and the board sources.
 * Derived from the single product option SPLC_BOARD (splcopts.h, defaults
 * in config/splc_opt.h); do not define them by hand.
 */
#include "splc_opt.h"

#define BOARD_ZIGBEE_IO_4DI_4DO     (SPLC_BOARD == SPLC_BOARD_ZIGBEE_IO_4DI_4DO)
#define BOARD_REMOTE_IO_8DI_8DO     (SPLC_BOARD == SPLC_BOARD_REMOTE_IO_8DI_8DO)
#define BOARD_DATALOGGER            (SPLC_BOARD == SPLC_BOARD_DATALOGGER)
#define BOARD_GATEWAY               (SPLC_BOARD == SPLC_BOARD_GATEWAY)

#endif