#ifndef __SX_SPI_H__
#define __SX_SPI_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>
#include <stdbool.h>
#include "sx_gpio.h"

/* Return codes: 0 on success, negative on failure. */
#define SX_SPI_OK            0
#define SX_SPI_ERR_PARAM    (-1)
#define SX_SPI_ERR_BUS      (-2)
#define SX_SPI_ERR_TIMEOUT  (-3)

/* Longest read-only transfer (sx_spi_read / transfer with tx == NULL): the
 * dummy bytes clocked out come from a buffer inside sx_spi_t. */
#define SX_SPI_DUMMY_MAX    256U

typedef enum {
    SX_SPI_MODE_0 = 0,   /* CPOL 0, CPHA 0 */
    SX_SPI_MODE_1 = 1,   /* CPOL 0, CPHA 1 */
    SX_SPI_MODE_2 = 2,   /* CPOL 1, CPHA 0 */
    SX_SPI_MODE_3 = 3    /* CPOL 1, CPHA 1 */
} sx_spi_mode_t;

typedef struct sx_spi_config {
    void           *pDriver;        /* the real HAL handle, e.g. &hspi1 (already MX_SPIx_Init()ed, 8-bit master) */
    uint32_t        clock_hz;       /* wanted SCK (rounded DOWN to a divider); 0 = keep the CubeMX timing/mode/bit order */
    sx_spi_mode_t   mode;           /* ignored when clock_hz == 0 */
    bool            lsb_first;      /* ignored when clock_hz == 0 */
    sx_gpio_pin_t  *cs;             /* optional software chip select (output pin), NULL = none */
    bool            cs_active_high;
    uint16_t        cs_setup_us;    /* wait after asserting CS */
    uint16_t        cs_gap_us;      /* minimum CS idle time between two selections */
    uint8_t         dummy;          /* byte clocked out on read-only transfers */
} sx_spi_config_t;

typedef struct sx_spi {
    sx_spi_config_t *config;
    bool             ready;
    uint8_t          dummy_buf[SX_SPI_DUMMY_MAX];
    uint32_t         last_deselect_cyc;
    bool             have_last;
    bool             dwt_ok;        /* DWT cycle counter verified running; else CS waits use a (longer) NOP loop */
} sx_spi_t;

/*
 * Wires spi->config, applies clock/mode/bit order (when clock_hz != 0), turns
 * on "master keep IO state" so SCK/MOSI do not float between transfers, and
 * puts the CS pin (if any) in its idle level BEFORE it becomes an output.
 * On failure the HAL handle is left as it was and the driver is not ready.
 */
int sx_spi_init(sx_spi_t *spi, sx_spi_config_t *config);

/* Blocking full-duplex transfer. tx == NULL: clock out cfg->dummy (len <=
 * SX_SPI_DUMMY_MAX). rx == NULL: discard. tx == rx (in place) is allowed.
 * len == 0 is a no-op. Does NOT touch CS. */
int sx_spi_transfer(sx_spi_t *spi, const uint8_t *tx, uint8_t *rx, uint16_t len, uint32_t timeout_ms);
int sx_spi_write(sx_spi_t *spi, const uint8_t *tx, uint16_t len, uint32_t timeout_ms);
int sx_spi_read(sx_spi_t *spi, uint8_t *rx, uint16_t len, uint32_t timeout_ms);

/* Software CS (no-ops without cfg->cs): honours polarity, cs_setup_us after
 * select and cs_gap_us since the previous deselect. */
void sx_spi_select(sx_spi_t *spi);
void sx_spi_deselect(sx_spi_t *spi);

#ifdef __cplusplus
}
#endif

#endif