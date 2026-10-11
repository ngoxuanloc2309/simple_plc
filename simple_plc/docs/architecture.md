# SimplePLC — Kiến trúc Firmware

> Tài liệu tham chiếu kiến trúc, **đồng bộ với code nhánh `board_dev_gw` và
> Wire Profile V2.0**. Trạng thái/việc đang làm: `docs/handoff.md`. Nguồn sự thật cho
> wire format: `SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` (struct, memory
> map) và `SimplePLC_Wire_Contract_V2_Draft.md` (hành vi). Khi file này mâu thuẫn với
> chúng, chúng thắng. `SimplePLC_RuleStruct_MCU_Spec_v0.1.md` chỉ còn đúng cho logic
> Rule Engine (5 bước Trigger→Compare→Dwell→Guard→Action).

## 0. Bối cảnh

SimplePLC là thư viện firmware lõi cho họ sản phẩm IIoT (Remote I/O, Datalogger, Gateway,
Controller) — dùng lại như lwIP: người dùng thư viện chỉ viết một file board + một file cấu
hình (`splcopts.h`), không sửa bên trong thư viện. Board hiện có: **Zigbee-IO** (4DI/4DO)
trên STM32H523CCU6 (Cortex-M33, 250 MHz, 256 KB Flash, 272 KB SRAM) — đã chạy trên phần
cứng; **Gateway** (DI/DO ảo qua RS485, mục 5.1) — mới build, chưa chạy phần cứng. Nguyên
tắc bắt buộc: **không heap động, struct kích thước cố định, tách lớp rõ ràng** (để port
xuống MCU nhỏ).

- Thiết bị chạy bảng rule `IF trigger THEN action` trên một bảng "tag" phẳng; App
  (SimplePLC.Studio) cấu hình rule và FB qua **Modbus RTU** (nanoMODBUS) trên một
  `modbus_transport_t` — hiện là USB-CDC (TinyUSB). RS485 dùng cho Gateway làm Modbus
  Master tới các board IO (mục 5.1), không phải kênh App↔MCU.
- Mô hình **kéo (poll)**: ghi một giá trị chỉ ghi RAM rồi dừng; bước khác, lên lịch riêng,
  đọc lại sau. Không có cascade/sự kiện đẩy.
- **Nhúng vào firmware chủ** (đã có TinyUSB/nanoMODBUS/logger/FreeRTOS): xem mục 3.1.

## 1. Bảy layer

```
Layer 4     Engine & Application entry     app/      + board/ (ghép từng SKU)
Layer 3     PLC Application Services       services/
Layer 3.5   Protocol / Library Porting     port/
Layer 2     PLC Core                       core/
Layer 1     SX Driver Core (hợp đồng)      components/
Layer 0     Platform (chip cụ thể)         platforms/
Layer U     Utils + thư viện ngoài         utils/, libs/
Ngoài layer config/ (mặc định thư viện), splc_config/ (file của sản phẩm), docs/
```

Quy tắc xuyên suốt:
1. **Include chỉ đi một chiều, từ trên xuống.**
2. **Mô hình kéo**, không đẩy.
3. **Layer 2 là ranh giới port/build:** không include Layer 0/1/3.5 → build và test được
   trên PC thuần.
4. Layer U không include ngược lên. Layer 3.5 chỉ đứng giữa Layer U/thư viện ngoài và Layer 1.
5. **Layer 0 nằm TRONG repo** (không như lwIP port ra ngoài): chip mới = thêm file dưới
   `platforms/<family>/<chip>/`; chọn nền bằng option `SPLC_PLATFORM`, dạng file-per-platform
   qua macro, không vtable.
6. Người dùng thư viện chỉ gọi API công khai, viết `board_<sku>.c` và `splcopts.h`;
   không chạm vào core/components/platforms.

### Cấu trúc thư mục

```
simple_plc/
├── app/        Layer 4: plc_engine.{c,h} (init + scan loop), plc_system_cmd_service,
│               plc_system_clear; sx_os_config.h / sx_platform_config.h (chỉ SUY RA cờ)
├── board/      Layer 4: board.{c,h} (hooks), board_config.h, board_tag_define.h (MAX_TAGS),
│               board_device/board_<sku>.{c,h} — pin wiring, SPLC_TagLayout, descriptor
├── services/   Layer 3: plc_io (+ plc_io_remote: DI/DO ảo của Gateway), plc_retain,
│               plc_rule_flash, plc_modbus_cfg, plc_fb, plc_rtc
├── port/       Layer 3.5: modbus_usb (App link), modbus_serial (RS485 master, Gateway),
│               modbus_tcp (stub rỗng), modbus_transport (giao diện), usb (tusb_config.h,
│               usb_descriptors.c)
├── core/       Layer 2: plc_tag, plc_rule (+ state machine), plc_internal_rule (eval, action),
│               plc_device, plc_diag, plc_error, plc_system_cmd
├── components/ Layer 1: sx_gpio, sx_adc, sx_flash, sx_uart, sx_time, sx_rtc, sx_pwd,
│               sx_system, sx_timer, sx_os, sx_usb_cdc (+ .c dùng TinyUSB)
├── platforms/  Layer 0: stm32/stm32h5/{gpio,adc,flash,flash_define,uart,time,timer,rtc,pwd,
│               system}, freertos/ (hiện thực sx_os cho FreeRTOS, không gắn chip)
├── utils/      Layer U: cqueue, epoch, logger, filter/*
├── libs/       nanomodbus, tinyusb (git submodule)
├── config/     splc_opt.h (mặc định + kiểm tra), splcopts_template.h, splc_log.h (log)
└── docs/
splc_config/splcopts.h   (gốc repo) — file cấu hình của sản phẩm hiện tại
```

## 2. Cấu hình (kiểu lwipopts.h)

- Mọi thư viện file cần option chỉ `#include "splc_opt.h"`. Nó include `splcopts.h` của sản
  phẩm trước, rồi đặt mặc định cho mọi option chưa định nghĩa và `#error` khi giá trị sai.
- CMake: `-DSPLC_OPTS_DIR=<thư mục chứa splcopts.h>` (mặc định `<top-level>/splc_config`);
  thiếu file → lỗi ngay lúc configure. Thư mục này và `config/` được thêm vào include path
  của mọi layer.
- Option: target (`SPLC_PLATFORM`, `SPLC_BOARD`), OS (`SX_OS_USE_FREERTOS`), engine
  (`PLC_SCAN_INTERVAL_MS`, `MAX_RULES` ≤ 100, `PLC_REBOOT_DELAY_MS`), Flash
  (`SPLC_FLASH_SIZE_KB`), retain (`RETAIN_SNAPSHOT_PERIOD_MS`,
  `PLC_PVD_EMERGENCY_SAVE_ENABLE`, `RETAIN_EMERGENCY_MIN_INTERVAL_MS`), link App
  (`SPLC_MODBUS_UNIT_ID`, `SPLC_USB_RX/TX_BUF_SIZE`), Remote I/O (`SPLC_REMOTE_MAX_NODES`,
  `_TIMEOUT_MS`, `_POLL_MS`, `_OFFLINE_AFTER`, `_RETRY_MS`), log (`SPLC_LOG_LEVEL`,
  `SPLC_LOG_BUFFER_SIZE`, `SPLC_LOGGER_BUILTIN`, macro `SPLC_LOG_OUTPUT`).
- CMake đọc `SPLC_BOARD`, `SPLC_PLATFORM`, `SPLC_LOGGER_BUILTIN` thẳng từ `splcopts.h`
  (`cmake_modules/splc_select.cmake`).
- Không phải option: hằng wire, sector size và vị trí 5 sector dữ liệu (chỉ phụ thuộc
  `SPLC_FLASH_SIZE_KB`), tag layout của board.
- Cờ `STM32H5_PLATFORM`, `BOARD_*`, `SX_NO_OS` được SUY RA từ option trong
  `sx_platform_config.h`, `board_config.h`, `sx_os_config.h`.

## 3. Chế độ OS

`SX_OS_USE_FREERTOS = 0` (mặc định): bare-metal, `main()` gọi `plc_engine_init()` rồi
`plc_engine_poll()` mỗi vòng. `= 1`: host gọi cả hai từ **một** task; thư viện không tạo
task và đơn luồng, không khóa (tag, bảng rule, nháp FB/Modbus, retain) — task khác không
được đụng vào. Chỉ đổi: `sx_delay_*` ngủ qua scheduler, vòng chờ USB gọi
`sx_os_yield_wait()`, logger dùng mutex. `sx_get_tick_ms()` luôn là `HAL_GetTick()`.
Hợp đồng Layer 1: `components/os/sx_os.h`; Layer 0: `platforms/freertos/sx_os_freertos.c`
(OBJECT library, object đưa vào executable bằng `$<TARGET_OBJECTS:splc_platform_freertos>`).
Yêu cầu host khi `= 1`: header FreeRTOS qua `-DSPLC_OS_INCLUDE_DIRS=...`; `FreeRTOSConfig.h`
có `INCLUDE_vTaskDelay`, `INCLUDE_xTaskGetSchedulerState`, `configSUPPORT_DYNAMIC_ALLOCATION`
(`configTICK_RATE_HZ=1000` nên dùng); HAL timebase chuyển sang TIM (`sx_get_tick_ms()` vẫn
là `HAL_GetTick()`, an toàn trong ISR PVD); không log từ ISR; TinyUSB giữ `OPT_OS_NONE`.

### 3.1 Nhúng vào firmware chủ (định hướng, đang làm)

Firmware chủ thường đã có TinyUSB, nanoMODBUS, logger, FreeRTOS. SimplePLC làm "khách", điểm
nối kiểu lwIP: `modbus_transport_t` (`read/write/connected`, như `netif input/output`) và
macro `SPLC_LOG_OUTPUT` (như `LWIP_PLATFORM_DIAG`).

| Việc | Trạng thái |
|---|---|
| Log: `config/splc_log.h` (`log_*` → `SPLC_LOG_OUTPUT`), `SPLC_LOGGER_BUILTIN=0` = dùng logger của host | xong |
| Dùng chung target `nanomodbus`/`tinyusb`/`cqueue` (`if(NOT TARGET ...)`) | chưa |
| `SPLC_USB_EXTERNAL`: không build `sx_usb_cdc.c`/`port/usb`, host giữ `tusb_init`/`tud_task`/`tud_*_cb` | chưa |
| Transport ngoài: board điền `read/write/connected`; `read` trả ngay khi chưa có dữ liệu | chưa |
| File board nằm ngoài thư viện; vị trí vùng Flash dữ liệu do host khai | chưa |

Lõi (core/services/app) không bao giờ `#include "tusb.h"`; chỉ board file được phép.
Chi tiết firmware gateway thật và kế hoạch từng bước: `docs/handoff.md` mục 4.

## 4. Chi tiết layer

### Layer U
`cqueue` (hàng đợi vòng cho USB/UART, dùng `cqueue_init_static`), `splc_epoch` (đổi
epoch ↔ lịch, giờ địa phương), `logger` (cùng API với `libs/logger` của firmware gateway; buffer dùng chung cỡ
`SPLC_LOG_BUFFER_SIZE`, `snprintf` có giới hạn, mutex khi RTOS; chỉ build khi
`SPLC_LOGGER_BUILTIN=1`; code SimplePLC gọi qua `config/splc_log.h`), `filter/*` (IIR, Kalman, MA, Median — chưa dùng).
`libs/nanomodbus` (Modbus RTU), `libs/tinyusb`.

### Layer 0 / 1
Hợp đồng `sx_*.h` ở Layer 1; hiện thực STM32H5 ở Layer 0 (HAL). USB CDC dùng driver
`stm32_fsdev` của TinyUSB (USB_DRD_FS), byte đi qua `cqueue`; `sx_usb_tiny_read()` đo
timeout bằng tick thật. Flash: ghi quad-word 16 byte, erase theo sector 8 KB; layout vùng
dữ liệu ở `platforms/stm32/stm32h5/flash_define/splc_flash_define.h`:

Vùng dữ liệu luôn là **5 sector cuối** của Flash, tính ngược từ `END = FLASH_BASE +
SPLC_FLASH_SIZE_KB * 1024` (option duy nhất người dùng khai báo trong `splcopts.h`; sector
8 KB là hằng số của họ H5, không phải option). Cột ví dụ là chip 256 KB (STM32H523CC):

| Sector tính từ cuối | Địa chỉ | Ví dụ 256 KB | Dùng cho |
|---|---|---|---|
| cuối cùng | `END - 1 sector` | `0x0803E000` | Rule Table A (bản chạy) |
| thứ 2 từ cuối | `END - 2` | `0x0803C000` | Rule Table B (bản dự phòng) |
| thứ 3, 4, 5 từ cuối | `END - 3`, `END - 4`, `END - 5` | `0x0803A000`, `0x08038000`, `0x08036000` | Retain (log xoay vòng 3 sector) |

Firmware (`.text+.data`) phải nằm dưới `SPLC_FLASH_DATA_BASE_ADDR` (= `END - 40 KB`; ví dụ
256 KB: `0x08036000`) — đặt `LENGTH` của FLASH trong linker script bằng
`SPLC_FLASH_SIZE_KB - 40 KB`. `splc_flash_define.h` kiểm tra lúc biên dịch: size là bội số
của 16 KB, trong khoảng 128..2048 KB, 5 sector nằm trọn trong bank 2. Giả định cần đối chiếu
datasheet khi dùng chip mới: hai bank bằng nhau.

### Layer 2 — PLC Core
- **`plc_tag`**: một mảng `g_tag_value[MAX_TAGS]` duy nhất + `g_tag_table[]` (kind,
  channel). `TagKind`: NONE=0, DI=1, DO=2, AI=3, VFLAG=4, VREG=5, MB_COIL=6, MB_HOLDING=7,
  VREG_RETAIN=8, COUNTER=9. Layout theo board qua `SPLC_TagLayout` (không có ô sentinel
  ở index 0; thứ tự nhóm DI, DO, AI, VFLAG, VREG, VREG_RETAIN, COUNTER — Zigbee-IO: 112
  tag). Truy cập từ Layer 3+ qua `tag_read/tag_write/tag_get_kind` và
  `tag_di_base_index()`... Tag layout cố định lúc biên dịch theo board, không đổi qua
  Modbus. Giá trị MAX_TAGS từ `board/board_tag_define.h`.
- **`plc_rule`**: `SPLC_RuleRecord` **32 byte** (16 thanh ghi): `threshold_lo/hi`, `for_ms`,
  `action_param` (4 byte); `trigger_tag`, `action_tag`, `guard_tag` (2 byte); `enabled`,
  `trigger_type`, `compare_op`, `action_type` (1 byte); `reserved[6]` (sender ghi 0).
  Định danh rule = vị trí. `guard_tag`: bit 0-14 = index, bit 15 = NEGATE, **"không có
  guard" = `GUARD_TAG_NONE` = `0x7FFF`** (KHÔNG phải 0, vì index 0 là `TAG_DI0` thật).
  Trigger: ON_CHANGE, ON_RISE, ON_FALL, TIME_WINDOW, INTERVAL. Compare: NONE, EQ, NEQ,
  GT, LT, GTE, LTE, BETWEEN. Action: SET_TAG, TOGGLE_TAG, INC_COUNTER, WRITE_REMOTE,
  LOG_EVENT, SEND_ALARM, ADD_TAG, SCALE_TAG (3 cái giữa-sau chưa implement:
  WRITE_REMOTE, LOG_EVENT, SEND_ALARM).
- **Rule State Machine** (`rule_scan(now_ms, now_hhmm)`): `switch/case` tường minh
  `IDLE → TRIGGERED → COMPARED → DWELLING → GUARD_CHECK → FIRE / BLOCKED`, fallthrough có
  chủ đích trong một lần gọi; chỉ `DWELLING` sống qua nhiều scan. "Mức còn giữ" lúc dwell
  suy từ giá trị hiện tại theo hướng trigger, không chỉ dựa `compare_ok()`. `now_hhmm =
  RULE_HHMM_INVALID` khi chưa có giờ hợp lệ → Time Window không bắn. Time Window theo
  Structs v2.0 mục 7 (khung trong ngày, qua nửa đêm, mốc phút `EQ, Lo=Hi`).
  `rule_runtime_reset()` đặt lại runtime như vừa nạp.
- **`plc_device.h`**: `SPLC_DeviceDescriptor` (20 B; `protocol_version=2`,
  `rule_format_version=7`), `SPLC_DeviceHealth` (20 B; `scan_time_ms`),
  `SPLC_DeviceResourceInfo` (20 B; `wire_profile=2`, `max_rules`, số lượng từng kind —
  App tự dựng ProductDefinition/TagCatalog từ đây, không lookup theo `device_variant`).
- **`plc_diag`** (hằng lease: min 1000, mặc định 3000, max 60000 ms), **`plc_error`**
  (`SPLC_ERROR_NONE..FLASH`=0..6), **`plc_system_cmd`** (NONE, REBOOT, FACTORY_RESET,
  CLEAR_RULES, CLEAR_RETAIN).

### Layer 3.5
`port/modbus_usb`: `modbus_usb_read/write` nối nanoMODBUS với `sx_usb_tiny_*` qua giao diện
`modbus_transport_t`. Hạn chế: `modbus_usb_write()` bỏ qua `timeout_ms`.
`port/modbus_serial`: **Modbus RTU master không chặn** trên `sx_uart_t` (FC03 + FC16, một
request đang chạy mỗi master). `modbus_rtu_master_read/write()` gửi (TX ~1,5 ms), rồi
`modbus_rtu_master_poll()` mỗi scan gom byte; khung hoàn tất khi đủ độ dài theo function code,
timeout bằng tick thật, CRC bằng `nmbs_crc_calc()` (byte-swapped, ghi big-endian). Chân DE
(nếu có) kéo cao quanh lúc gửi; echo của transceiver bị bỏ. `modbus_tcp` là file rỗng.

### Layer 3 — Services
- **`plc_io`**: đăng ký kênh (`plc_io_register_di/do/ai(tag_idx, hw)`); `input_scan()` (DI
  0/1, AI mã ADC thô) và `output_scan()` (DO từ tag). Cuối mỗi hàm gọi hook của
  **`plc_io_remote`** (DI/DO ảo của Gateway, mục 5.1; không làm gì khi chưa có node).
- **`plc_modbus_cfg`**: nanoMODBUS server + register map (mục 5), State Machine #2 nạp
  rule (staging → verify CRC16 → commit nguyên tử), Diag Control (ENTER/HEARTBEAT/EXIT,
  lease, ghi tag trong diag, retain draft), RTC, FB, System Command.
- **`plc_rule_flash`**: lưu Rule Table A/B (xem handoff mục 1 về thứ tự và recovery); bản
  ghi = header 8 B (seq_num, rule_count + cờ FB, crc16) + rule wire + đoạn FB 128 B.
- **`plc_retain`**: snapshot xoay vòng cho VREG_RETAIN (record 208 B: seq, count, crc16,
  tag_idx+value); khôi phục lúc boot lấy record CRC-hợp-lệ có seq lớn nhất; lưu theo
  COMMIT_RETAIN, chu kỳ, và PVD khẩn cấp (ISR).
- **`plc_fb`**: khối Timer/Counter `0x0B00..0x0B7F` (nháp + COMMIT; Timer báo cáo qua
  `rule_ref`; Counter đọc CV từ tag do Host chọn).
- **`plc_rtc`**: khối `0x0810` (epoch UTC, tz, `status_flags` RO), `plc_rtc_get_local_hhmm()`.

### Layer 4 — Engine
`plc_engine_init()` theo thứ tự: `tag_table_load_from_flash(&layout)` (từ
`board_get_tag_layout()`, phải trước `board_init()`) → `plc_fb_init()` →
`rule_table_load_from_flash()` + `plc_rule_flash_load()` → `retain_store_restore()` →
đăng ký callback PVD (nếu bật) → `board_init()` (nối chân, tạo transport) →
`plc_modbus_cfg_init()`.

`plc_engine_poll()` gọi `scan_cycle()` mỗi `PLC_SCAN_INTERVAL_MS` (so sánh tick kiểu
unsigned, an toàn khi tràn):

```
diag_tick → input_scan
  → [không bị diag treo]  rule_scan (+ rule_runtime_reset khi vừa thoát diag) → plc_fb_scan
  → output_scan → modbus_config_service → retain_service → ghi scan_time
  → plc_system_cmd_service            (cuối cùng: có thể reset MCU)
```

Board hook (`board.h`): `board_get_tag_layout()`, `board_init()`, `board_get_modbus_transport()`.
`plc_engine.c` không biết SKU; chỉ một `board_<sku>.c` được link (CMake `SPLC_BOARD_SKU`).

## 5. Modbus register map V2.0 (tóm tắt — chi tiết ở Structs v2.0)

Chỉ dùng FC03, FC06, FC16. 32-bit = 2 thanh ghi, **High Word trước**; 2×uint8 = 1 thanh
ghi, high byte trước; CRC khung RTU little-endian.

| Địa chỉ | R/W | Khối |
|---|---|---|
| `0x0000..0x0009` | R | Device Descriptor |
| `0x0010` | R | Rule Table Info (số rule active) |
| `0x0020..0x0029` | R | Device Resource Info |
| `0x0100..0x073F` | R | Active Rule Table (16 reg/rule, tối đa 100) |
| `0x0800..0x0809` | R | Device Health |
| `0x0810..0x0813` | R/W | RTC (`epoch_utc_s`, `tz_offset_min`, `status_flags` RO) |
| `0x0900..0x09FF` | R (W khi `DIAG_CONTROL`) | Runtime tag, 2 reg/tag, cửa sổ 128 tag (tag ≥ `MAX_TAGS` đọc 0, ghi bị từ chối `0x02`) |
| `0x0A00` | W | System Command |
| `0x0A01..0x0A02` | R | Command Status, Error Code |
| `0x0A20..0x0A24` | R/W | Diag Control (Command, State, Flags, LeaseMs, ErrorCode) |
| `0x0B00..0x0B3F` / `0x0B40..0x0B7F` | R/W | 8 Timer / 8 Counter, 8 reg mỗi khối |
| `0x9000..0x9005` | R/W | Staging handshake (status, error, staged count, expected CRC, active count, active CRC) |
| `0x9010..0x964F` | W | Staging Rule Buffer |
| `0xA000` | W | Commit (`0xA5A5`) |
| `0xA001` | R | Active Rule Version (chỉ trong RAM; về 1 sau reboot) |

Nạp rule: ghi `RULE_COUNT_STAGED` + `EXPECTED_CRC16` → ghi staging (chia khối nhỏ: một
FC16 an toàn ≤ 16 thanh ghi = 1 rule = 41 byte vì gói USB FS 64 byte) → đọc `CONFIG_STATUS`
→ ghi `0xA5A5` → đọc version. CRC = CRC-16/MODBUS trên **byte wire**. Exception: `0x02`
địa chỉ sai, `0x03` giá trị sai. Giới hạn: FC03 ≤ 125 reg, FC16 ≤ 123 reg.

### 5.1 Remote I/O của Gateway (link RS485 tới board IO)

Gateway là master; board IO (chân DI/DO thật) là **slave thuần** cài khối thanh ghi dưới đây
bằng nanoMODBUS (FC03/FC16), KHÔNG chạy SimplePLC. Lease/SAFE nằm ở slave.

| Địa chỉ | R/W | Ý nghĩa |
|---|---|---|
| `0x0C00` | R | DI (bit i = DI i) |
| `0x0C01` | R | DO đọc lại |
| `0x0C02` | R | STATUS: bit0 LEASE_ALIVE, bit2 FAILSAFE, bit3 RESET |
| `0x0C03/0x0C04` | W | MASK, VALUE: `do = (do & ~mask) \| (value & mask)`, ghi cùng lúc |
| `0x0C05` | R/W | LEASE ms (0 = tắt, hoặc 1000..60000) |
| `0x0C06` | R/W | SAFE: bit DO ép về khi lease hết hạn |

`plc_io_remote` (một request đang chạy trong toàn bộ, tối đa 1 request mới mỗi scan), ưu
tiên: **SYNC** (FC16 4 reg: mask=all, value, lease, safe — lúc đầu, khi node online lại, khi
STATUS báo RESET/FAILSAFE) > **WRITE** (FC16 2 reg khi tag DO khác giá trị đã báo, mask =
bit đổi) > **REFRESH** (ghi lại mọi bit DO mỗi lease/3, giữ lease sống và tự sửa lệch) >
**POLL** (FC03 3 reg mỗi `SPLC_REMOTE_POLL_MS`: DI + STATUS). Sau `SPLC_REMOTE_OFFLINE_AFTER`
request lỗi liên tiếp node thành offline (`plc_io_remote_node_online()`), thử lại mỗi
`SPLC_REMOTE_RETRY_MS`, rồi SYNC lại; khi offline tag DI GIỮ giá trị cuối (không tạo sườn
xuống giả cho rule `ON_FALL`).

Khai báo ở file board, bằng dữ liệu: `GATEWAY_IO_NODES(X)` với `X(unit_id, di_count,
do_count)` mỗi hàng một board IO (≤ 16 DI + ≤ 16 DO, ≤ `SPLC_REMOTE_MAX_NODES` hàng). Tag ảo
nối tiếp theo thứ tự hàng (DI trước, DO sau, base lấy từ `tag_di_base_index()`/
`tag_do_base_index()`); tag layout suy từ bảng và kiểm lúc biên dịch (`_Static_assert`,
`MAX_TAGS` = 128 cho SKU này). 4DI/4DO → 8DI/8DO = sửa một hàng.

## 6. Mảng dữ liệu chính

| Mảng | Chứa | Ai ghi | Ai đọc |
|---|---|---|---|
| `g_tag_value[MAX_TAGS]` | giá trị sống | `input_scan`, `rule_scan`, Modbus (diag) | `rule_scan`, `output_scan`, Modbus |
| `g_tag_table[MAX_TAGS]` | kind/channel | boot | `tag_get_kind()` |
| `g_rule_table[MAX_RULES]` | rule tĩnh | `rule_table_commit()` | `rule_scan`, Modbus |
| `g_rule_runtime[MAX_RULES]` | trạng thái động | state machine | chính nó, `plc_fb_scan` |
| `s_staging_rule_table[]` | RAM đệm nạp rule | Modbus | commit |
| `g_device_descriptor/health/resource_info` | nhận dạng, sức khoẻ | hằng/Layer 3-4 | Modbus |

## 7. Checklist khi code

- [ ] `sizeof(SPLC_RuleRecord) == 32`; không thêm `rule_id`; `reserved[6]` = 0 trước khi tính CRC.
- [ ] Mọi CRC dùng CRC-16/MODBUS trên byte wire; không dùng CRC32.
- [ ] Truy cập tag từ Layer 3+ qua `tag_read/tag_write`; Layer 2 không include Layer 0/1.
- [ ] File Layer 3 không gọi lẫn nhau trực tiếp (trừ phụ thuộc đã có); Layer 3 không có
      symbol `sx_usb_*`.
- [ ] `guard_tag` "không guard" = `GUARD_TAG_NONE` (`0x7FFF`).
- [ ] Mọi thao tác Flash ngoài `plc_retain.c` bọc `retain_flash_op_begin/end()`.
- [ ] Không sửa file CubeMX tự sinh; cần hành vi khác → linker `--wrap` / lớp `sx_*`.
- [ ] Option mới: thêm vào `config/splc_opt.h` (mặc định + check) và template; không rải
      `#define` cấu hình vào header layer.
- [ ] Không phụ thuộc `configTICK_RATE_HZ` cho thời gian scan (dùng `sx_get_tick_ms()`).
- [ ] Remote I/O không chặn: `plc_io_remote_*` chỉ chạy trong `input_scan/output_scan`, một
      request mỗi scan; chỉ board file `#include "tusb.h"`/gọi `logger_init`.

## 8. Còn mở

Nhúng vào firmware chủ (bảng mục 3.1); board Gateway thật (H563) và firmware slave cho board
IO; Event Log (`ACT_LOG_EVENT`) và Alarm (`ACT_SEND_ALARM`) chưa thiết kế; validate index tag
khi nạp rule; `modbus_tcp` còn rỗng. Danh sách đầy đủ: `docs/handoff.md` mục 4.