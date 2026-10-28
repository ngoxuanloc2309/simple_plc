#ifndef __SX_I2C_H__
#define __SX_I2C_H__

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* Return codes: 0 on success, negative on failure. */
#define SX_I2C_OK            0
#define SX_I2C_ERR_PARAM    (-1)
#define SX_I2C_ERR_BUS      (-2)   /* bus error, arbitration lost, busy, anything else */
#define SX_I2C_ERR_NACK     (-3)   /* address or data not acknowledged */
#define SX_I2C_ERR_TIMEOUT  (-4)

typedef struct sx_i2c_config {
    void *pDriver;                 /* the real HAL handle, e.g. &hi2c1 (already MX_I2Cx_Init()ed) */
} sx_i2c_config_t;

/* Checks the handle (non-NULL, initialised by CubeMX). Does not touch the bus. */
int sx_i2c_init(sx_i2c_config_t *config);

/* All addresses are 7-bit (0x00..0x7F), NOT pre-shifted. All calls block up to timeout_ms. */
int sx_i2c_write(sx_i2c_config_t *config, uint8_t addr7, const uint8_t *data, uint16_t len, uint32_t timeout_ms);
int sx_i2c_read(sx_i2c_config_t *config, uint8_t addr7, uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* Register access: reg_size is 1 or 2 bytes (16-bit register sent high byte first). */
int sx_i2c_mem_write(sx_i2c_config_t *config, uint8_t addr7, uint16_t reg, uint8_t reg_size,
                     const uint8_t *data, uint16_t len, uint32_t timeout_ms);
int sx_i2c_mem_read(sx_i2c_config_t *config, uint8_t addr7, uint16_t reg, uint8_t reg_size,
                    uint8_t *data, uint16_t len, uint32_t timeout_ms);

/* 0 when the device ACKs its address. */
int sx_i2c_probe(sx_i2c_config_t *config, uint8_t addr7, uint32_t timeout_ms);

/* Recovers a stuck peripheral (power it off/on, clear HAL state). Timing is untouched. */
int sx_i2c_reset(sx_i2c_config_t *config);

#ifdef __cplusplus
}
#endif

#endif