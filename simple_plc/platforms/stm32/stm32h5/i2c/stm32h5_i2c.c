#include "stm32h5_i2c.h"

/* Compiles to nothing when CubeMX has not enabled the HAL I2C module; any
 * caller of sx_i2c_*() then fails at link time, on purpose. */
#if STM32H5_PLATFORM && defined(HAL_I2C_MODULE_ENABLED)

static I2C_HandleTypeDef *handle_of(const sx_i2c_config_t *config)
{
    I2C_HandleTypeDef *h;

    if (config == NULL || config->pDriver == NULL) {
        return NULL;
    }
    h = (I2C_HandleTypeDef *)config->pDriver;
    if (h->Instance == NULL || h->State == HAL_I2C_STATE_RESET) {
        return NULL;                            /* MX_I2Cx_Init() has not run */
    }
    return h;
}

static bool mem_size(uint8_t reg_size, uint16_t reg, uint16_t *hal_size)
{
    if (reg_size == 1U && reg <= 0xFFU) {
        *hal_size = I2C_MEMADD_SIZE_8BIT;
        return true;
    }
    if (reg_size == 2U) {
        *hal_size = I2C_MEMADD_SIZE_16BIT;
        return true;
    }
    return false;
}

static int map_status(I2C_HandleTypeDef *h, HAL_StatusTypeDef st)
{
    uint32_t err;

    if (st == HAL_OK)      { return SX_I2C_OK; }
    if (st == HAL_TIMEOUT) { return SX_I2C_ERR_TIMEOUT; }
    if (st == HAL_ERROR) {
        err = HAL_I2C_GetError(h);
        if ((err & HAL_I2C_ERROR_AF) != 0U)      { return SX_I2C_ERR_NACK; }
        if ((err & HAL_I2C_ERROR_TIMEOUT) != 0U) { return SX_I2C_ERR_TIMEOUT; }
    }
    return SX_I2C_ERR_BUS;                      /* HAL_BUSY and everything else */
}

int sx_i2c_init(sx_i2c_config_t *config)
{
    return (handle_of(config) != NULL) ? SX_I2C_OK : SX_I2C_ERR_PARAM;
}

int sx_i2c_write(sx_i2c_config_t *config, uint8_t addr7, const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    I2C_HandleTypeDef *h = handle_of(config);
    if (h == NULL || addr7 > 0x7FU || data == NULL || len == 0U) {
        return SX_I2C_ERR_PARAM;
    }
    return map_status(h, HAL_I2C_Master_Transmit(h, (uint16_t)(addr7 << 1), (uint8_t *)data, len, timeout_ms));
}

int sx_i2c_read(sx_i2c_config_t *config, uint8_t addr7, uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    I2C_HandleTypeDef *h = handle_of(config);
    if (h == NULL || addr7 > 0x7FU || data == NULL || len == 0U) {
        return SX_I2C_ERR_PARAM;
    }
    return map_status(h, HAL_I2C_Master_Receive(h, (uint16_t)(addr7 << 1), data, len, timeout_ms));
}

int sx_i2c_mem_write(sx_i2c_config_t *config, uint8_t addr7, uint16_t reg, uint8_t reg_size,
                     const uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    I2C_HandleTypeDef *h = handle_of(config);
    uint16_t sz;
    if (h == NULL || addr7 > 0x7FU || data == NULL || len == 0U || !mem_size(reg_size, reg, &sz)) {
        return SX_I2C_ERR_PARAM;
    }
    return map_status(h, HAL_I2C_Mem_Write(h, (uint16_t)(addr7 << 1), reg, sz, (uint8_t *)data, len, timeout_ms));
}

int sx_i2c_mem_read(sx_i2c_config_t *config, uint8_t addr7, uint16_t reg, uint8_t reg_size,
                    uint8_t *data, uint16_t len, uint32_t timeout_ms)
{
    I2C_HandleTypeDef *h = handle_of(config);
    uint16_t sz;
    if (h == NULL || addr7 > 0x7FU || data == NULL || len == 0U || !mem_size(reg_size, reg, &sz)) {
        return SX_I2C_ERR_PARAM;
    }
    return map_status(h, HAL_I2C_Mem_Read(h, (uint16_t)(addr7 << 1), reg, sz, data, len, timeout_ms));
}

int sx_i2c_probe(sx_i2c_config_t *config, uint8_t addr7, uint32_t timeout_ms)
{
    I2C_HandleTypeDef *h = handle_of(config);
    HAL_StatusTypeDef st;
    if (h == NULL || addr7 > 0x7FU) {
        return SX_I2C_ERR_PARAM;
    }
    st = HAL_I2C_IsDeviceReady(h, (uint16_t)(addr7 << 1), 1U, timeout_ms);
    if (st == HAL_ERROR) {
        return SX_I2C_ERR_NACK;                 /* IsDeviceReady reports "no ACK" as HAL_ERROR */
    }
    return map_status(h, st);
}

int sx_i2c_reset(sx_i2c_config_t *config)
{
    I2C_HandleTypeDef *h = handle_of(config);
    if (h == NULL) {
        return SX_I2C_ERR_PARAM;
    }
    __HAL_I2C_DISABLE(h);
    for (int i = 0; i < 8; i++) {               /* PE must stay low >= 3 APB clocks */
        __NOP();
    }
    __HAL_I2C_ENABLE(h);
    h->State = HAL_I2C_STATE_READY;
    h->Mode = HAL_I2C_MODE_NONE;
    h->ErrorCode = HAL_I2C_ERROR_NONE;
    __HAL_UNLOCK(h);
    return SX_I2C_OK;
}

#endif // STM32H5_PLATFORM && HAL_I2C_MODULE_ENABLED