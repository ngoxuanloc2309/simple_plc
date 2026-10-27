#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/*
 * splcopts.h - YOUR product's SimplePLC configuration (template).
 *
 * Copy this file to a directory of your own project, name it splcopts.h and
 * tell CMake where it is:
 *
 *     cmake ... -DSPLC_OPTS_DIR=<that directory>
 *
 * (default: <top-level source dir>/splc_config). Uncomment and edit only
 * what you want to change; everything else keeps the default from the
 * library's config/splc_opt.h, which also documents every option and its
 * legal range. Never edit splc_opt.h itself.
 */

/* --- Target ---------------------------------------------------------- */
/* #define SPLC_PLATFORM                 SPLC_PLATFORM_STM32H5 */
/* #define SPLC_BOARD                    SPLC_BOARD_ZIGBEE_IO_4DI_4DO */   /* also CMake -DSPLC_BOARD_SKU */

/* #define SPLC_FLASH_SIZE_KB            256 */    /* chip Flash in KB; data sectors are the last 5 */
/* #define SPLC_FLASH_DATA_ADDR          0x081F0000UL */ /* optional: 8 KB-aligned address of the 5 data sectors when the host owns the end of Flash */

/* --- Operating system ------------------------------------------------ */
/* #define SX_OS_USE_FREERTOS            1 */      /* 0 bare-metal (default), 1 FreeRTOS */

/* --- Engine ---------------------------------------------------------- */
/* #define PLC_SCAN_INTERVAL_MS          10U */
/* #define MAX_RULES                     100 */    /* 1..100 */
/* #define PLC_REBOOT_DELAY_MS           300U */

/* --- Retain ---------------------------------------------------------- */
/* #define RETAIN_SNAPSHOT_PERIOD_MS     (5U * 60U * 1000U) */
/* #define PLC_PVD_EMERGENCY_SAVE_ENABLE 1 */      /* 0 if the board has no hold-up energy */
/* #define RETAIN_EMERGENCY_MIN_INTERVAL_MS 5000U */

/* --- App <-> device link --------------------------------------------- */
/* #define SPLC_PORT_USB_CDC             1 */      /* 1 = SimplePLC owns USB (TinyUSB + CDC port module); 0 = host owns it (default) */
/* #define SPLC_MODBUS_UNIT_ID           1 */
/* #define SPLC_LINK_RX_BUF_SIZE         512 */   /* power of two, >= 256 */

/* --- Remote I/O (Gateway) -------------------------------------------- */
/* #define SPLC_REMOTE_MAX_NODES         8 */       /* 1..32 I/O boards on RS485 */
/* #define SPLC_REMOTE_TIMEOUT_MS        100U */    /* 20..2000 per request */
/* #define SPLC_REMOTE_POLL_MS           50U */     /* 10..1000 DI read period per node */
/* #define SPLC_REMOTE_OFFLINE_AFTER     3U */      /* failed requests -> node offline */
/* #define SPLC_REMOTE_RETRY_MS          1000U */   /* retry period of an offline node */

/* --- Modbus master blocks (MB_READ) ----------------------------------- */
/* #define SPLC_MBM_ENABLE               0 */      /* 1 = Modbus-master service + 125-register master buffers; 0 = no change at all */
/* #define SPLC_MODBUS_MAX_TRANSPORTS    1 */      /* 1..3 App-link transports served at once (USB + RS-485 + TCP); 1 = no change at all */
/* #define SPLC_WIFI_ENABLE              0 */      /* 1 = Wi-Fi block 0x1300..0x13FF through host callbacks (splc_wifi_register()); 0 = no change at all */
/* #define SPLC_MQTT_TLS_CERT_ENABLE     0 */      /* 1 = TLS certificate block 0x0F00..0x0F43 through host callbacks (splc_mqtt_tls_register(); needs SPLC_MQTT_ENABLE 1); 0 = no change at all */
/* #define SPLC_MBW_ENABLE               0 */      /* 1 = MB_WRITE blocks 0x1100..0x11FF (needs SPLC_MBM_ENABLE 1); 0 = no change at all */
/* #define SPLC_MBM_PORT_COUNT           1 */      /* 1..4 RS-485 ports for MB_READ/MB_WRITE (reported at 0x0A0A); the host registers one UART per port with splc_mbm_set_rs485_port(); line settings come from the App (0x1400) */

/* --- MQTT Subscribe ----------------------------------------------------- */
/* #define SPLC_MQTT_SUB_ENABLE          0 */      /* 1 = remote commands/setpoints from MQTT into tags (needs SPLC_MQTT_ENABLE 1); 0 = no change at all */
/* #define SPLC_MQTT_SUB_FIFO_DEPTH      1 */      /* 1..8 queued messages per block for FIFO delivery; 1 = latest only */

/* --- REAL (float32) tags ---------------------------------------------- */
/* #define SPLC_REAL_ENABLE              0 */      /* 1 = Tag DataType Registry 0x0080 (BOOL/INT32/REAL per tag) + float32 Rule Engine (rule flags bit 0); 0 = no change at all */

/* --- MQTT Publish ---------------------------------------------------- */
/* #define SPLC_MQTT_ENABLE              0 */      /* 1 = build plc_mqtt (Gateway); 0 = no change at all */
/* #define SPLC_MQTT_PAYLOAD_MAX         256 */    /* 128..512 payload buffer (with NUL) */
/* #define SPLC_MQTT_RETRY_MS            200U */   /* wait before retrying after publish returned < 0 */
/* #define SPLC_MQTT_MIN_INTERVAL_MS     100U */   /* floor of interval_ms for PERIODIC blocks */
/* #define SPLC_MQTT_TLS_SUPPORTED       0 */      /* 1 only if the host client really does TLS */

/* --- Logging --------------------------------------------------------- */
/* #define SPLC_RTC_BUILTIN              1 */      /* 0 = host supplies sx_rtc_*() (no CubeMX RTC needed) */
/* #define SPLC_ADC_BUILTIN              1 */      /* 0 = host supplies sx_adc_*() (no CubeMX ADC needed) */
/* #define SPLC_RTC_EXTERNAL             0 */      /* 1 = external RTC chip: register get/set hooks with plc_rtc_register() (implies SPLC_RTC_BUILTIN 0) */
/* #define SPLC_RTC_EXT_READ_PERIOD_MS   1000U */  /* 100..60000: how often the registered hooks are called */
/* #define SPLC_LOG_LEVEL                4 */      /* 0 OFF .. 4 DEBUG */
/* #define SPLC_LOG_BUFFER_SIZE          4096 */

#endif /* SPLCOPTS_H */