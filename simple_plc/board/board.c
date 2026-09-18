#include "board.h"

/*
 * board.c - Layer 4
 *
 * board_init() is the single entry point every SKU calls the same way;
 * it only forwards to board_hw_init(), which is defined once per SKU
 * (see board_zigbee_io.c). Kept as a separate function (rather than
 * having callers invoke board_hw_init() directly) so future common
 * setup shared by every SKU has one place to live without touching
 * per-SKU files.
 */
void board_init(void)
{
    board_hw_init();
}