#include "stm32h5_spi.h"

/* Compiles to nothing when CubeMX has not enabled the HAL SPI module; any
 * caller of sx_spi_*() then fails at link time, on purpose. */
#if STM32H5_PLATFORM && defined(HAL_SPI_MODULE_ENABLED)

#include <string.h>

static SPI_HandleTypeDef *handle_of(const sx_spi_config_t *config)
{
    SPI_HandleTypeDef *h;

    if (config == NULL || config->pDriver == NULL) {
        return NULL;
    }
    h = (SPI_HandleTypeDef *)config->pDriver;
    if (h->Instance == NULL || h->State == HAL_SPI_STATE_RESET) {
        return NULL;                            /* MX_SPIx_Init() has not run */
    }
    if (h->Init.Mode != SPI_MODE_MASTER || h->Init.DataSize != SPI_DATASIZE_8BIT) {
        return NULL;                            /* master, 8-bit only */
    }
    return h;
}

static uint32_t kernel_hz(const SPI_TypeDef *i)
{
    if (i == SPI1) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI1); }
#ifdef SPI2
    if (i == SPI2) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI2); }
#endif
#ifdef SPI3
    if (i == SPI3) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI3); }
#endif
#ifdef SPI4
    if (i == SPI4) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI4); }
#endif
#ifdef SPI5
    if (i == SPI5) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI5); }
#endif
#ifdef SPI6
    if (i == SPI6) { return HAL_RCCEx_GetPeriphCLKFreq(RCC_PERIPHCLK_SPI6); }
#endif
    return 0U;
}

static bool prescaler_for(uint32_t kernel, uint32_t want_hz, uint32_t *presc)
{
    static const struct { uint32_t div; uint32_t val; } tab[] = {
        {2U, SPI_BAUDRATEPRESCALER_2},   {4U, SPI_BAUDRATEPRESCALER_4},
        {8U, SPI_BAUDRATEPRESCALER_8},   {16U, SPI_BAUDRATEPRESCALER_16},
        {32U, SPI_BAUDRATEPRESCALER_32}, {64U, SPI_BAUDRATEPRESCALER_64},
        {128U, SPI_BAUDRATEPRESCALER_128}, {256U, SPI_BAUDRATEPRESCALER_256},
    };
    if (kernel == 0U) {
        return false;
    }
    for (unsigned i = 0; i < sizeof tab / sizeof tab[0]; i++) {
        if (kernel / tab[i].div <= want_hz) {   /* fastest divider not above the request */
            *presc = tab[i].val;
            return true;
        }
    }
    return false;
}

/* DWT cycle counter: microsecond waits that need no timer. */
static void cyc_enable(void)
{
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static uint32_t cyc_now(void)
{
    return DWT->CYCCNT;
}

static void wait_us(uint32_t us)
{
    uint32_t n = us * (SystemCoreClock / 1000000U);
    uint32_t s = cyc_now();
    while ((uint32_t)(cyc_now() - s) < n) {
    }
}

static void cs_drive(const sx_spi_config_t *c, bool active)
{
    bool level = (active == c->cs_active_high);
    sx_gpio_write(c->cs, level ? SX_GPIO_HIGH : SX_GPIO_LOW);
}

int sx_spi_init(sx_spi_t *spi, sx_spi_config_t *config)
{
    SPI_HandleTypeDef *h;
    SPI_InitTypeDef    saved;
    uint32_t           presc = 0U;

    if (spi == NULL || config == NULL) {
        return SX_SPI_ERR_PARAM;
    }
    spi->ready = false;
    spi->have_last = false;
    spi->config = config;

    h = handle_of(config);
    if (h == NULL) {
        return SX_SPI_ERR_PARAM;
    }
    if (config->cs != NULL && config->cs->mode != SX_GPIO_MODE_OUTPUT_PP) {
        return SX_SPI_ERR_PARAM;
    }

    saved = h->Init;
    if (config->clock_hz != 0U) {
        uint32_t pol, ph;
        switch (config->mode) {
            case SX_SPI_MODE_0: pol = SPI_POLARITY_LOW;  ph = SPI_PHASE_1EDGE; break;
            case SX_SPI_MODE_1: pol = SPI_POLARITY_LOW;  ph = SPI_PHASE_2EDGE; break;
            case SX_SPI_MODE_2: pol = SPI_POLARITY_HIGH; ph = SPI_PHASE_1EDGE; break;
            case SX_SPI_MODE_3: pol = SPI_POLARITY_HIGH; ph = SPI_PHASE_2EDGE; break;
            default:            return SX_SPI_ERR_PARAM;
        }
        if (!prescaler_for(kernel_hz(h->Instance), config->clock_hz, &presc)) {
            return SX_SPI_ERR_PARAM;
        }
        h->Init.BaudRatePrescaler = presc;
        h->Init.CLKPolarity = pol;
        h->Init.CLKPhase = ph;
        h->Init.FirstBit = config->lsb_first ? SPI_FIRSTBIT_LSB : SPI_FIRSTBIT_MSB;
    }
    h->Init.MasterKeepIOState = SPI_MASTER_KEEP_IO_STATE_ENABLE;

    if (HAL_SPI_Init(h) != HAL_OK) {
        h->Init = saved;
        return SX_SPI_ERR_BUS;
    }

    memset(spi->dummy_buf, config->dummy, sizeof spi->dummy_buf);

    if (config->cs != NULL) {
        sx_gpio_value_t idle = config->cs_active_high ? SX_GPIO_LOW : SX_GPIO_HIGH;
        sx_gpio_write(config->cs, idle);        /* idle level first, then make it an output */
        sx_gpio_init(config->cs, idle);
        cyc_enable();
    }
    spi->ready = true;
    return SX_SPI_OK;
}

static int map_status(HAL_StatusTypeDef st)
{
    if (st == HAL_OK)      { return SX_SPI_OK; }
    if (st == HAL_TIMEOUT) { return SX_SPI_ERR_TIMEOUT; }
    return SX_SPI_ERR_BUS;
}

int sx_spi_transfer(sx_spi_t *spi, const uint8_t *tx, uint8_t *rx, uint16_t len, uint32_t timeout_ms)
{
    SPI_HandleTypeDef *h;

    if (spi == NULL || !spi->ready || spi->config == NULL) {
        return SX_SPI_ERR_PARAM;
    }
    h = (SPI_HandleTypeDef *)spi->config->pDriver;
    if (h == NULL || (tx == NULL && rx == NULL)) {
        return SX_SPI_ERR_PARAM;
    }
    if (tx == NULL && len > SX_SPI_DUMMY_MAX) {
        return SX_SPI_ERR_PARAM;
    }
    if (len == 0U) {
        return SX_SPI_OK;
    }
    if (rx == NULL) {
        return map_status(HAL_SPI_Transmit(h, tx, len, timeout_ms));
    }
    return map_status(HAL_SPI_TransmitReceive(h, (tx != NULL) ? tx : spi->dummy_buf, rx, len, timeout_ms));
}

int sx_spi_write(sx_spi_t *spi, const uint8_t *tx, uint16_t len, uint32_t timeout_ms)
{
    if (tx == NULL) {
        return SX_SPI_ERR_PARAM;
    }
    return sx_spi_transfer(spi, tx, NULL, len, timeout_ms);
}

int sx_spi_read(sx_spi_t *spi, uint8_t *rx, uint16_t len, uint32_t timeout_ms)
{
    if (rx == NULL) {
        return SX_SPI_ERR_PARAM;
    }
    return sx_spi_transfer(spi, NULL, rx, len, timeout_ms);
}

void sx_spi_select(sx_spi_t *spi)
{
    const sx_spi_config_t *c;

    if (spi == NULL || !spi->ready || spi->config == NULL || spi->config->cs == NULL) {
        return;
    }
    c = spi->config;
    if (spi->have_last && c->cs_gap_us != 0U) {
        uint32_t n = (uint32_t)c->cs_gap_us * (SystemCoreClock / 1000000U);
        while ((uint32_t)(cyc_now() - spi->last_deselect_cyc) < n) {
        }
    }
    cs_drive(c, true);
    if (c->cs_setup_us != 0U) {
        wait_us(c->cs_setup_us);
    }
}

void sx_spi_deselect(sx_spi_t *spi)
{
    if (spi == NULL || !spi->ready || spi->config == NULL || spi->config->cs == NULL) {
        return;
    }
    cs_drive(spi->config, false);
    spi->last_deselect_cyc = cyc_now();
    spi->have_last = true;
}

#endif // STM32H5_PLATFORM && HAL_SPI_MODULE_ENABLED