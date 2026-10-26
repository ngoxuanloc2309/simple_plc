/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.c
  * @brief          : Main program body
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* USER CODE END Header */
/* Includes ------------------------------------------------------------------*/
#include "main.h"
#include "cmsis_os2.h"
#include "adc.h"
#include "gpdma.h"
#include "icache.h"
#include "usart.h"
#include "rtc.h"
#include "tim.h"
#include "usb.h"
#include "gpio.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "stdint.h"
#include "plc_engine.h"
#include "FreeRTOS.h"
#include "task.h"
/* Build selection (mutually exclusive; the PLC engine is NOT started in either test build):
 *   TEST_LED_RGB = 1 : WS2812B strip test on OUT0 (see led_rgb_test_task() below), log on LPUART1.
 *   TEST_RS485   = 1 : RS485 Remote I/O slave test on USART1 (see rs485_test_task() below).
 *   both 0           : normal firmware (PLC engine).
 * Default (nothing defined) stays TEST_RS485 = 1; set TEST_LED_RGB to 1 to switch to the LED test. */
#ifndef TEST_LED_RGB
#define TEST_LED_RGB 0
#endif
#ifndef TEST_RS485
#define TEST_RS485  1//(!TEST_LED_RGB)
#endif
#if TEST_LED_RGB && TEST_RS485
#error "TEST_LED_RGB and TEST_RS485 cannot both be 1 (both use the main task slot)"
#endif
#if TEST_RS485 || TEST_LED_RGB
#include <string.h>
#include <stdio.h>
#endif
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */
#define PLC_TASK_STACK_WORDS  1024U                      /* 4 KB; tune with uxTaskGetStackHighWaterMark() */
#define PLC_TASK_PRIORITY     (tskIDLE_PRIORITY + 25U)   /* just above defaultTask (osPriorityNormal = 24) */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/

/* USER CODE BEGIN PV */

/* USER CODE END PV */

/* Private function prototypes -----------------------------------------------*/
void SystemClock_Config(void);
void MX_FREERTOS_Init(void);
/* USER CODE BEGIN PFP */
#if SX_OS_USE_FREERTOS && !TEST_RS485 && !TEST_LED_RGB
static void plc_task(void *arg);
#endif
#if TEST_RS485
static void rs485_test_task(void *arg);
#endif
#if TEST_LED_RGB
static void led_rgb_test_task(void *arg);
#endif

/* USER CODE END PFP */

/* Private user code ---------------------------------------------------------*/
/* USER CODE BEGIN 0 */
#if TEST_RS485
/*
 * RS485 test on USART1 (TX PB14, RX PB15, DE = USART1_DE = PB13, GPIO driven, 115200 8N1 from
 * CubeMX). The only thing printed on LPUART1 is the bytes received on USART1, one line per
 * received frame (frame end = idle line), as hex ("48 65 6C 6C 6F"). Nothing else is logged.
 *
 * Silent (never logged): a periodic test string sent on the bus (RS485_TEST_TX_PERIOD_MS, 0 = off) and an
 * echo of every received frame (RS485_TEST_ECHO).
 */
#define RS485_TEST_TX_PERIOD_MS   0U     /* periodic test string; 0 = off (must be off: it would corrupt the Modbus bus) */
#define RS485_TEST_ECHO           0      /* 1 = send every received frame back */
#define RS485_TEST_LOG_ASCII      0      /* 1 = append the printable text after the hex */
#define RS485_TEST_RX_BUF         128U
/* Bench experiment for the RO (RDO) level. 1 = hold PB13 HIGH for good (transceiver driver on,
 * receiver off) and do nothing else: measure the RO net. Normal run = PB13 low (receiver on). */
#define RS485_TEST_DE_HOLD_HIGH   0

static uint8_t           s_rs485_rxbuf[RS485_TEST_RX_BUF];   /* HAL receives into this */
static uint8_t           s_rs485_frame[RS485_TEST_RX_BUF];   /* last complete frame */
static volatile uint16_t s_rs485_frame_len;
static volatile uint8_t  s_rs485_frame_ready;                /* ISR sets, task clears */

/* ISR context (USART1 interrupt): only copy the frame and re-arm. No RTOS calls. */
void HAL_UARTEx_RxEventCallback(UART_HandleTypeDef *huart, uint16_t size)
{
    if (huart->Instance != USART1)
    {
        return;
    }
    if (size > RS485_TEST_RX_BUF) { size = RS485_TEST_RX_BUF; }
    if (s_rs485_frame_ready == 0U)
    {
        memcpy(s_rs485_frame, s_rs485_rxbuf, size);
        s_rs485_frame_len = size;
        s_rs485_frame_ready = 1U;
    }
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, s_rs485_rxbuf, RS485_TEST_RX_BUF);
}

#if 1
static void rs485_test_send(const uint8_t *data, uint16_t len)
{
    HAL_GPIO_WritePin(USART1_DE_GPIO_Port, USART1_DE_Pin, GPIO_PIN_SET);    /* driver on */
    (void)HAL_UART_Transmit(&huart1, (uint8_t *)data, len, 200U);           /* waits TC */
    HAL_GPIO_WritePin(USART1_DE_GPIO_Port, USART1_DE_Pin, GPIO_PIN_RESET);  /* back to receive */
}
#endif


/* ---- Remote I/O slave (gateway master: plc_io_remote, architecture.md 5.1) -------------------
 * Modbus RTU slave, FC03 + FC16 only. On every valid frame addressed to RS485_TEST_UNIT_ID the
 * board scans DI (IN0..IN3) and applies DO (OUT0..OUT3):
 *   0x0C00 R  DI  (bit i = IN i, raw level: high = 1)   0x0C03 W MASK
 *   0x0C01 R  DO read-back                              0x0C04 W VALUE  do=(do&~mask)|(value&mask)
 *   0x0C02 R  STATUS b0 LEASE_ALIVE b2 FAILSAFE b3 RESET 0x0C05 R/W LEASE ms (0=off, 1000..60000)
 *                                                       0x0C06 R/W SAFE (DO bits forced on expiry)
 */
#define RS485_TEST_UNIT_ID   1U
#define RIO_REG_FIRST        0x0C00U
#define RIO_REG_COUNT        7U
#define RIO_DO_BITS          0x000FU
#define RIO_ST_LEASE_ALIVE   (1U << 0)
#define RIO_ST_FAILSAFE      (1U << 2)
#define RIO_ST_RESET         (1U << 3)

static uint16_t s_rio_do;
static uint16_t s_rio_lease_ms;
static uint16_t s_rio_safe;
static uint16_t s_rio_status = RIO_ST_RESET;   /* RESET until the master has synced us */
static uint32_t s_rio_last_write_tick;

static uint16_t rio_crc16(const uint8_t *d, uint16_t n)
{
    uint16_t crc = 0xFFFFU;
    while (n--)
    {
        crc ^= *d++;
        for (uint8_t i = 0U; i < 8U; i++)
        {
            crc = (crc & 1U) ? (uint16_t)((crc >> 1) ^ 0xA001U) : (uint16_t)(crc >> 1);
        }
    }
    return crc;
}

static uint16_t rio_scan_di(void)
{
    uint16_t v = 0U;
    v |= (HAL_GPIO_ReadPin(IN0_GPIO_Port, IN0_Pin) == GPIO_PIN_SET) ? 0x1U : 0U;
    v |= (HAL_GPIO_ReadPin(IN1_GPIO_Port, IN1_Pin) == GPIO_PIN_SET) ? 0x2U : 0U;
    v |= (HAL_GPIO_ReadPin(IN2_GPIO_Port, IN2_Pin) == GPIO_PIN_SET) ? 0x4U : 0U;
    v |= (HAL_GPIO_ReadPin(IN3_GPIO_Port, IN3_Pin) == GPIO_PIN_SET) ? 0x8U : 0U;
    return v;
}

static void rio_apply_do(uint16_t v)
{
    s_rio_do = (uint16_t)(v & RIO_DO_BITS);
    HAL_GPIO_WritePin(OUT0_GPIO_Port, OUT0_Pin, (s_rio_do & 0x1U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(OUT1_GPIO_Port, OUT1_Pin, (s_rio_do & 0x2U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(OUT2_GPIO_Port, OUT2_Pin, (s_rio_do & 0x4U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
    HAL_GPIO_WritePin(OUT3_GPIO_Port, OUT3_Pin, (s_rio_do & 0x8U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Lease watchdog: call often. Lease expired -> DO forced to SAFE, FAILSAFE flag set. */
static void rio_lease_tick(void)
{
    if (s_rio_lease_ms != 0U && (s_rio_status & RIO_ST_LEASE_ALIVE) &&
        (uint32_t)(HAL_GetTick() - s_rio_last_write_tick) > s_rio_lease_ms)
    {
        s_rio_status &= (uint16_t)~RIO_ST_LEASE_ALIVE;
        s_rio_status |= RIO_ST_FAILSAFE;
        rio_apply_do(s_rio_safe);
    }
}

static uint16_t rio_read_reg(uint16_t addr)
{
    switch (addr)
    {
    case 0x0C00U: return rio_scan_di();
    case 0x0C01U: return s_rio_do;
    case 0x0C02U: return s_rio_status;
    case 0x0C05U: return s_rio_lease_ms;
    case 0x0C06U: return s_rio_safe;
    default:      return 0U;      /* MASK/VALUE read as 0 */
    }
}

static void rio_exception(uint8_t *tx, uint8_t fc, uint8_t code, uint16_t *txlen)
{
    tx[0] = RS485_TEST_UNIT_ID; tx[1] = (uint8_t)(fc | 0x80U); tx[2] = code; *txlen = 3U;
}

/* Returns the response length (0 = no response). */
static uint16_t rio_handle(const uint8_t *rx, uint16_t len, uint8_t *tx)
{
    uint16_t txlen = 0U;
    if (len < 8U || rx[0] != RS485_TEST_UNIT_ID) { return 0U; }
    uint16_t crc = (uint16_t)(rx[len - 2U] | (rx[len - 1U] << 8));
    if (crc != rio_crc16(rx, (uint16_t)(len - 2U))) { return 0U; }

    uint8_t  fc   = rx[1];
    uint16_t addr = (uint16_t)((rx[2] << 8) | rx[3]);
    uint16_t qty  = (uint16_t)((rx[4] << 8) | rx[5]);

    if (addr < RIO_REG_FIRST || qty == 0U || (uint32_t)addr + qty > RIO_REG_FIRST + RIO_REG_COUNT)
    {
        if (fc == 0x03U || fc == 0x10U) { rio_exception(tx, fc, 0x02U, &txlen); }
        else                            { rio_exception(tx, fc, 0x01U, &txlen); }
    }
    else if (fc == 0x03U)
    {
        tx[0] = RS485_TEST_UNIT_ID; tx[1] = 0x03U; tx[2] = (uint8_t)(qty * 2U);
        for (uint16_t i = 0U; i < qty; i++)
        {
            uint16_t v = rio_read_reg((uint16_t)(addr + i));
            tx[3U + 2U * i] = (uint8_t)(v >> 8);
            tx[4U + 2U * i] = (uint8_t)v;
        }
        txlen = (uint16_t)(3U + 2U * qty);
    }
    else if (fc == 0x10U && len == (uint16_t)(9U + 2U * qty) && rx[6] == (uint8_t)(qty * 2U))
    {
        uint16_t mask = 0U, value = 0U; uint8_t have_mask = 0U, have_value = 0U, bad_value = 0U;
        for (uint16_t i = 0U; i < qty; i++)
        {
            uint16_t a = (uint16_t)(addr + i);
            uint16_t v = (uint16_t)((rx[7U + 2U * i] << 8) | rx[8U + 2U * i]);
            if      (a == 0x0C03U) { mask = v;  have_mask = 1U; }
            else if (a == 0x0C04U) { value = v; have_value = 1U; }
            else if (a == 0x0C05U)
            {
                if (v != 0U && (v < 1000U || v > 60000U)) { bad_value = 1U; }
                else                                      { s_rio_lease_ms = v; }
            }
            else if (a == 0x0C06U) { s_rio_safe = (uint16_t)(v & RIO_DO_BITS); }
            /* 0x0C00..0x0C02 are read-only: ignored */
        }
        if (bad_value != 0U)
        {
            rio_exception(tx, fc, 0x03U, &txlen);
        }
        else
        {
            if (have_mask || have_value)
            {
                rio_apply_do((uint16_t)((s_rio_do & ~mask) | (value & mask)));
            }
            s_rio_last_write_tick = HAL_GetTick();
            s_rio_status |= RIO_ST_LEASE_ALIVE;
            s_rio_status &= (uint16_t)~(RIO_ST_FAILSAFE | RIO_ST_RESET);
            tx[0] = RS485_TEST_UNIT_ID; tx[1] = 0x10U; tx[2] = rx[2]; tx[3] = rx[3]; tx[4] = rx[4]; tx[5] = rx[5];
            txlen = 6U;
        }
    }
    else
    {
        rio_exception(tx, fc, 0x01U, &txlen);
    }
    uint16_t c = rio_crc16(tx, txlen);
    tx[txlen++] = (uint8_t)c;
    tx[txlen++] = (uint8_t)(c >> 8);
    return txlen;
}

static void rs485_test_task(void *arg)
{
    (void)arg;
#if RS485_TEST_TX_PERIOD_MS > 0U
    uint32_t tx_count = 0U;
    TickType_t last_tx = xTaskGetTickCount();
#endif

#if RS485_TEST_DE_HOLD_HIGH
    HAL_GPIO_WritePin(USART1_DE_GPIO_Port, USART1_DE_Pin, GPIO_PIN_SET);     /* receiver off */
    for (;;)
    {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
#endif
    HAL_GPIO_WritePin(USART1_DE_GPIO_Port, USART1_DE_Pin, GPIO_PIN_RESET);   /* receive */
    (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, s_rs485_rxbuf, RS485_TEST_RX_BUF);

    for (;;)
    {
        /* The HAL stops an interrupt receive after an UART error (overrun...) and never
         * restarts it: clear the error and arm again. */
        if (huart1.ErrorCode != HAL_UART_ERROR_NONE)
        {
            huart1.ErrorCode = HAL_UART_ERROR_NONE;
        }
        if (huart1.RxState == HAL_UART_STATE_READY)
        {
            (void)HAL_UARTEx_ReceiveToIdle_IT(&huart1, s_rs485_rxbuf, RS485_TEST_RX_BUF);
        }

        if (s_rs485_frame_ready != 0U)
        {
            uint8_t  frame[RS485_TEST_RX_BUF];
            uint16_t len = s_rs485_frame_len;
            memcpy(frame, s_rs485_frame, len);
            s_rs485_frame_ready = 0U;

            /* "48 65 6C 6C 6F\r\n" (+ "  Hello" if enabled) */
            char line[3U * RS485_TEST_RX_BUF + RS485_TEST_RX_BUF + 8U];
            uint16_t n = 0U;
            for (uint16_t i = 0U; i < len; i++)
            {
                n += (uint16_t)snprintf(&line[n], 4U, "%02X%s", frame[i], (i + 1U < len) ? " " : "");
            }
#if RS485_TEST_LOG_ASCII
            line[n++] = ' ';
            line[n++] = ' ';
            for (uint16_t i = 0U; i < len; i++)
            {
                line[n++] = (frame[i] >= 0x20U && frame[i] < 0x7FU) ? (char)frame[i] : '.';
            }
#endif
            line[n++] = '\r';
            line[n++] = '\n';
            (void)HAL_UART_Transmit(&hlpuart1, (uint8_t *)line, n, 200U);

            {
                uint8_t  rsp[RS485_TEST_RX_BUF];
                uint16_t rlen = rio_handle(frame, len, rsp);   /* scans DI, applies DO */
                if (rlen != 0U)
                {
                    rs485_test_send(rsp, rlen);
                    vTaskDelay(pdMS_TO_TICKS(3));
                    s_rs485_frame_ready = 0U;  /* drop bytes the transceiver looped back */
                }
            }
#if RS485_TEST_ECHO
            rs485_test_send(frame, len);
            vTaskDelay(pdMS_TO_TICKS(3));
            s_rs485_frame_ready = 0U;      /* drop bytes the transceiver looped back */
#endif
        }

#if RS485_TEST_TX_PERIOD_MS > 0U
        TickType_t now = xTaskGetTickCount();
        if ((TickType_t)(now - last_tx) >= pdMS_TO_TICKS(RS485_TEST_TX_PERIOD_MS))
        {
            last_tx = now;
            uint8_t msg[40];
            int m = snprintf((char *)msg, sizeof(msg), "SPLC-RS485-TEST #%lu\r\n", (unsigned long)++tx_count);
            if (m > 0)
            {
                rs485_test_send(msg, (uint16_t)m);
                vTaskDelay(pdMS_TO_TICKS(3));
                s_rs485_frame_ready = 0U;  /* drop bytes the transceiver looped back */
            }
        }
#endif

        rio_lease_tick();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}
#endif /* TEST_RS485 */

#if TEST_LED_RGB
/* ---- WS2812B (IP30, 144 LED/m) test on OUT0 = PA6 ---------------------------------------------
 * Data is bit-banged on a GPIO: 800 kHz NRZ, timing from the DWT cycle counter so it follows
 * SystemCoreClock (no assumption about 250 MHz), interrupts masked for the ~4.5 ms of one frame.
 * Frame order is GRB, MSB first; >280 us low latches. 1 bit = 1250 ns: T0H 400 ns, T1H 800 ns.
 *
 * Wiring (strip JST 3-pin: red = 5V, white = GND, green = DATA):
 *   - 5V from a SEPARATE supply (not the board), GND common with the board. 144 LED at full white
 *     can draw ~8.6 A; LED_RGB_BRIGHTNESS keeps the test patterns well below 1 A.
 *   - OUT0 -> ~330 ohm series resistor -> DATA. 1000 uF across 5V/GND at the strip input.
 *   - A 3.3 V data level is marginal for a 5 V WS2812B (VIH = 0.7 x VDD = 3.5 V): use a 74AHCT125 /
 *     74HCT245 level shifter if the strip flickers or stays dark.
 *   - OUT0 must be the bare MCU pin: through an optocoupler / transistor / ULN stage 800 kHz will not
 *     pass (check the schematic; LED_RGB_INVERT = 1 only fixes polarity, not speed).
 */
#define LED_RGB_COUNT        144U          /* LEDs on the strip (lower it to test with a short piece) */
#define LED_RGB_BRIGHTNESS   16U           /* global scale 0..255 (16 = ~6 %) */
#define LED_RGB_PORT         OUT0_GPIO_Port
#define LED_RGB_PIN          OUT0_Pin
#define LED_RGB_INVERT       0             /* 1 = external inverting stage on the data line */
#define LED_RGB_T0H_NS       400U
#define LED_RGB_T1H_NS       800U
#define LED_RGB_BIT_NS       1250U

static uint8_t s_led_grb[LED_RGB_COUNT * 3U];

static void led_rgb_dwt_enable(void)
{
#if defined(DCB_DEMCR_TRCENA_Msk)
    DCB->DEMCR |= DCB_DEMCR_TRCENA_Msk;
#else
    CoreDebug->DEMCR |= CoreDebug_DEMCR_TRCENA_Msk;
#endif
    DWT->CYCCNT = 0U;
    DWT->CTRL |= DWT_CTRL_CYCCNTENA_Msk;
}

static void led_rgb_gpio_init(void)
{
    GPIO_InitTypeDef gi = {0};
    gi.Pin   = LED_RGB_PIN;
    gi.Mode  = GPIO_MODE_OUTPUT_PP;
    gi.Pull  = GPIO_NOPULL;
    gi.Speed = GPIO_SPEED_FREQ_VERY_HIGH;   /* CubeMX left OUT0 at LOW speed: far too slow for 800 kHz */
    HAL_GPIO_Init(LED_RGB_PORT, &gi);
    HAL_GPIO_WritePin(LED_RGB_PORT, LED_RGB_PIN, LED_RGB_INVERT ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

/* Shift the whole frame out. Blocks ~4.5 ms for 144 LEDs with all interrupts masked. */
static void led_rgb_show(void)
{
    const uint32_t c_t0h = (uint32_t)(((uint64_t)SystemCoreClock * LED_RGB_T0H_NS) / 1000000000ULL);
    const uint32_t c_t1h = (uint32_t)(((uint64_t)SystemCoreClock * LED_RGB_T1H_NS) / 1000000000ULL);
    const uint32_t c_bit = (uint32_t)(((uint64_t)SystemCoreClock * LED_RGB_BIT_NS) / 1000000000ULL);
    const uint32_t hi  = LED_RGB_INVERT ? ((uint32_t)LED_RGB_PIN << 16) : (uint32_t)LED_RGB_PIN;
    const uint32_t lo  = LED_RGB_INVERT ? (uint32_t)LED_RGB_PIN : ((uint32_t)LED_RGB_PIN << 16);
    GPIO_TypeDef  *port = LED_RGB_PORT;

    __disable_irq();
    uint32_t t = DWT->CYCCNT;
    for (uint32_t i = 0U; i < (uint32_t)sizeof(s_led_grb); i++)
    {
        uint8_t b = s_led_grb[i];
        for (uint8_t bit = 0U; bit < 8U; bit++)
        {
            const uint32_t high = (b & 0x80U) ? c_t1h : c_t0h;
            b = (uint8_t)(b << 1);
            port->BSRR = hi;
            while ((uint32_t)(DWT->CYCCNT - t) < high) { }
            port->BSRR = lo;
            while ((uint32_t)(DWT->CYCCNT - t) < c_bit) { }
            t += c_bit;                      /* fixed schedule: no drift from loop overhead */
        }
    }
    __enable_irq();
    /* Latch (>280 us low) is guaranteed by the caller's delay (>= 2 ms) before the next frame. */
}

static void led_rgb_set(uint32_t i, uint8_t r, uint8_t g, uint8_t b)
{
    if (i >= LED_RGB_COUNT) { return; }
    s_led_grb[3U * i + 0U] = (uint8_t)(((uint16_t)g * LED_RGB_BRIGHTNESS) / 255U);
    s_led_grb[3U * i + 1U] = (uint8_t)(((uint16_t)r * LED_RGB_BRIGHTNESS) / 255U);
    s_led_grb[3U * i + 2U] = (uint8_t)(((uint16_t)b * LED_RGB_BRIGHTNESS) / 255U);
}

static void led_rgb_fill(uint8_t r, uint8_t g, uint8_t b)
{
    for (uint32_t i = 0U; i < LED_RGB_COUNT; i++) { led_rgb_set(i, r, g, b); }
}

static void led_rgb_log(const char *msg)
{
    char line[64];
    int n = snprintf(line, sizeof(line), "[LED] %s\r\n", msg);
    if (n > (int)sizeof(line) - 1) { n = (int)sizeof(line) - 1; }   /* snprintf returns the untruncated length */
    if (n > 0) { (void)HAL_UART_Transmit(&hlpuart1, (uint8_t *)line, (uint16_t)n, 100U); }
}

/* Red only: every LED solid red, the same frame re-sent every 10 ms (~4.3 ms burst, then low). */
static void led_rgb_test_task(void *arg)
{
    (void)arg;
    led_rgb_dwt_enable();
    led_rgb_gpio_init();

    const uint32_t a = DWT->CYCCNT;
    for (volatile uint32_t k = 0U; k < 100U; k++) { }
    if (DWT->CYCCNT == a)
    {
        led_rgb_log("ERROR DWT CYCCNT not running");
        for (;;) { vTaskDelay(pdMS_TO_TICKS(1000)); }
    }

    led_rgb_log("red only");
    led_rgb_fill(255U, 0U, 0U);
    for (;;)
    {
        led_rgb_show();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
#endif /* TEST_LED_RGB */
/* USER CODE END 0 */

/**
  * @brief  The application entry point.
  * @retval int
  */
int main(void)
{

  /* USER CODE BEGIN 1 */

  /* USER CODE END 1 */

  /* MCU Configuration--------------------------------------------------------*/

  /* Reset of all peripherals, Initializes the Flash interface and the Systick. */
  HAL_Init();

  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* Configure the system clock */
  SystemClock_Config();

  /* USER CODE BEGIN SysInit */

  /* USER CODE END SysInit */

  /* Initialize all configured peripherals */
  MX_GPIO_Init();
  MX_GPDMA1_Init();
  MX_ICACHE_Init();
  MX_LPUART1_UART_Init();
  MX_USART1_UART_Init();
  MX_USART2_UART_Init();
  MX_TIM1_Init();
  MX_TIM2_Init();
  MX_TIM3_Init();
  MX_TIM4_Init();
  MX_TIM5_Init();
  MX_USB_PCD_Init();
  MX_ADC1_Init();
  MX_RTC_Init();
  /* USER CODE BEGIN 2 */
  #if TEST_RS485
  xTaskCreate(rs485_test_task, "rs485t", PLC_TASK_STACK_WORDS, NULL, PLC_TASK_PRIORITY, NULL);
  #endif
  #if TEST_LED_RGB
  xTaskCreate(led_rgb_test_task, "ledrgb", PLC_TASK_STACK_WORDS, NULL, PLC_TASK_PRIORITY, NULL);
  #endif
  #if !TEST_RS485 && !TEST_LED_RGB
  #if SX_OS_USE_FREERTOS
  /* RTOS mode: plc_engine_init() and plc_engine_poll() must run from ONE task
   * (see simple_plc/components/os/sx_os.h). Created before the scheduler starts. */
  xTaskCreate(plc_task, "plc", PLC_TASK_STACK_WORDS, NULL, PLC_TASK_PRIORITY, NULL);
  #else
  plc_engine_init();
  #endif
  #endif
  /* USER CODE END 2 */

  /* Init scheduler */
  osKernelInitialize();
  /* Call init function for freertos objects (in app_freertos.c) */
  MX_FREERTOS_Init();

  /* Start scheduler */
  osKernelStart();

  /* We should never get here as control is now taken by the scheduler */

  /* Infinite loop */
  /* USER CODE BEGIN WHILE */
  while (1)
  {
    /* USER CODE END WHILE */

    /* USER CODE BEGIN 3 */
    // plc_engine_poll();
  }
  /* USER CODE END 3 */
}

/**
  * @brief System Clock Configuration
  * @retval None
  */
void SystemClock_Config(void)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while(!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY)) {}

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI48|RCC_OSCILLATORTYPE_HSI
                              |RCC_OSCILLATORTYPE_LSI;
  RCC_OscInitStruct.HSIState = RCC_HSI_ON;
  RCC_OscInitStruct.HSIDiv = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.LSIState = RCC_LSI_ON;
  RCC_OscInitStruct.HSI48State = RCC_HSI48_ON;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLL1_SOURCE_HSI;
  RCC_OscInitStruct.PLL.PLLM = 4;
  RCC_OscInitStruct.PLL.PLLN = 31;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 2;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1_VCIRANGE_3;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1_VCORANGE_WIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 2048;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK|RCC_CLOCKTYPE_SYSCLK
                              |RCC_CLOCKTYPE_PCLK1|RCC_CLOCKTYPE_PCLK2
                              |RCC_CLOCKTYPE_PCLK3;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_SYSCLK_DIV1;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_HCLK_DIV1;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_HCLK_DIV1;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_5) != HAL_OK)
  {
    Error_Handler();
  }

  /** Configure the programming delay
  */
  __HAL_FLASH_SET_PROGRAM_DELAY(FLASH_PROGRAMMING_DELAY_2);
}

/* USER CODE BEGIN 4 */
#if SX_OS_USE_FREERTOS && !TEST_RS485 && !TEST_LED_RGB
static void plc_task(void *arg)
{
  (void)arg;
  plc_engine_init();
  for (;;)
  {
    plc_engine_poll();
    HAL_GPIO_WritePin(GPIOB, GPIO_PIN_13, GPIO_PIN_RESET);
    vTaskDelay(1);
  }
}
#endif

/* USER CODE END 4 */

/**
  * @brief  Period elapsed callback in non blocking mode
  * @note   This function is called  when TIM6 interrupt took place, inside
  * HAL_TIM_IRQHandler(). It makes a direct call to HAL_IncTick() to increment
  * a global variable "uwTick" used as application time base.
  * @param  htim : TIM handle
  * @retval None
  */
void HAL_TIM_PeriodElapsedCallback(TIM_HandleTypeDef *htim)
{
  /* USER CODE BEGIN Callback 0 */

  /* USER CODE END Callback 0 */
  if (htim->Instance == TIM6)
  {
    HAL_IncTick();
  }
  /* USER CODE BEGIN Callback 1 */

  /* USER CODE END Callback 1 */
}

/**
  * @brief  This function is executed in case of error occurrence.
  * @retval None
  */
void Error_Handler(void)
{
  /* USER CODE BEGIN Error_Handler_Debug */
  /* User can add his own implementation to report the HAL error return state */
  __disable_irq();
  while (1)
  {
  }
  /* USER CODE END Error_Handler_Debug */
}
#ifdef USE_FULL_ASSERT
/**
  * @brief  Reports the name of the source file and the source line number
  *         where the assert_param error has occurred.
  * @param  file: pointer to the source file name
  * @param  line: assert_param error line source number
  * @retval None
  */
void assert_failed(uint8_t *file, uint32_t line)
{
  /* USER CODE BEGIN 6 */
  /* User can add his own implementation to report the file name and line number,
     ex: printf("Wrong parameters value: file %s on line %d\r\n", file, line) */
  /* USER CODE END 6 */
}
#endif /* USE_FULL_ASSERT */