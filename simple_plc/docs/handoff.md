# SimplePLC — Handoff cho phiên làm việc tiếp theo

> Viết bởi Claude. Mục đích: cho phép 1 Claude khác (hoặc chính bạn) tiếp
> tục công việc mà không cần đọc lại toàn bộ lịch sử chat. Đọc file này
> SAU KHI đã đọc `Readme.md` và `docs/architecture.md` — file đó vẫn là
> nguồn kiến trúc chính, file này chỉ ghi lại "đang làm tới đâu" và "làm
> gì tiếp theo".
>
> **Đây là bản VÁ TIẾP** (không viết lại toàn bộ như lần trước) trên nền
> bản 519 dòng trước đó. Thay đổi chính từ bản trước tới giờ: Layer 3
> (`services/`) không còn rỗng — `plc_io.c` và `plc_retain.c` đã viết
> xong và build+test thật (mục 1.7 mới); PVD (nguồn giám sát sụt áp cho
> Retain) đã xong cả 3 lớp code+cấu hình (mục 1.8 mới); `services/
> CMakeLists.txt` đã viết và wire xong (mục 1.4.4 mới); `splc_flash_define.h`
> đã DI CHUYỂN VỊ TRÍ, không còn ở `app/` (mục 1.5 đã cập nhật); và một
> lỗi CMake thật nghiêm trọng (plain vs keyword `target_link_libraries`
> signature, chỉ lộ ra khi build bằng toolchain ARM thật trên Windows,
> sandbox Linux không bao giờ tự phát hiện được) đã được tìm và sửa (mục
> 1.4.4). `build.bat` cũng đã có ở gốc repo (mục 1.9 mới).

## 0. Trạng thái repo tại thời điểm viết file này

- Branch: `main`
- Commit mới nhất đã verify: `52a9ef7` ("cmake simple_plc change" — sửa
  lỗi CMake plain/keyword `target_link_libraries` signature conflict,
  xem mục 1.4.4 mới).
- Lệnh verify: `git log --oneline -10` để xem có commit mới hơn không
  trước khi đọc tiếp phần dưới — nếu có commit mới, ưu tiên đọc code thật
  hơn file này. **Bài học đã rút ra nhiều lần (xem mục 6):** người dùng có
  thể tự sửa/push code không khớp hoàn toàn với đề xuất trước đó — LUÔN
  `git pull` rồi build-verify thật, không chỉ đọc `git diff` bằng mắt.

## 1. Việc đã xong (đã build + test thật, không chỉ đọc code)

### 1.1 Layer 2 (`core/`) — HOÀN CHỈNH

Cấu trúc (6 module, mỗi module 1 thư mục con):

```
core/
├── plc_tag/
│   ├── plc_tag.h, plc_tag.c       — Tag Table (SPLC_TagKind, g_tag_table[], g_tag_value[])
│   └── plc_tag_def.h              — 124 #define cụ thể (TAG_DI0..TAG_COUNTER7),
│                                     layout v1.9: KHÔNG có sentinel TAG_NONE ở
│                                     index 0 (khác v1.7 cũ, xem bugfix mục 1.2)
├── plc_rule/
│   ├── plc_rule.h, plc_rule.c     — SPLC_RuleRecord (32 byte), rule_scan(), rule_table_commit()
│   └── plc_rule_state_machine.h
├── plc_internal_rule/             — chỉ plc_rule.c include trực tiếp (PRIVATE trong CMake)
│   ├── plc_rule_eval.h/.c
│   └── plc_rule_action.h/.c
├── plc_device/
│   └── plc_device.h               — SPLC_DeviceClass, SPLC_DeviceDescriptor (20B),
│                                     SPLC_DeviceHealth (20B). CHỈ HEADER, không có .c
│                                     (quyết định có chủ đích — xem comment đầu file).
├── plc_error/
│   └── plc_error.h                — SPLC_ErrorCode, dùng chung system_cmd + modbus_cfg
└── plc_system_cmd/
    └── plc_system_cmd.h           — SPLC_SystemCommandRequest (2B), SPLC_SystemCommandResult (4B)
```

**Đã verify bằng compile+chạy thật (script đầy đủ ở mục 5):**
`sizeof(SPLC_RuleRecord)==32`, `sizeof(SPLC_DeviceDescriptor)==20`,
`sizeof(SPLC_DeviceHealth)==20`, `sizeof(SPLC_SystemCommandRequest)==2`,
`sizeof(SPLC_SystemCommandResult)==4`, `TAG_DI0==0`, rule engine
end-to-end (DI rise → DO set), và bugfix `guard_tag` (mục 1.2).

**3 module cuối (`plc_device`, `plc_error`, `plc_system_cmd`) cố ý chỉ có
`.h`, không có `.c`** — không có logic thuần Layer 2, mọi hàm thật cần
đụng Layer 0/1/3/4. Đừng tự ý thêm `.c` + `extern g_device_descriptor`
vào Layer 2, đã cân nhắc và bác bỏ — xem comment đầu `plc_device.h`.

`core/CMakeLists.txt` build 4 file `.c`, expose PUBLIC 5 include dir
(`plc_tag`, `plc_rule`, `plc_device`, `plc_error`, `plc_system_cmd`),
PRIVATE 1 include dir (`plc_internal_rule`).

### 1.2 Bugfix `guard_tag` sentinel — đã sửa, verify bằng compile+chạy thật

**Vấn đề:** dưới layout v1.9 (index 0 là `TAG_DI0`, một tag thật — khác
v1.7 nơi index 0 là sentinel vô nghĩa), so sánh cũ `guard_idx == TAG_NONE`
(`== 0`) khiến **`TAG_DI0` là tag duy nhất không thể dùng làm
`guard_tag`** — mọi rule set `guard_tag = TAG_DI0` bị hiểu nhầm thành
"không có guard", luôn fire bất kể DI0 bằng gì.

**Đã sửa:** thêm `#define GUARD_TAG_NONE 0x7FFFu` (dùng hết 15-bit index,
an toàn vì `MAX_TAGS=128` << 32767). `RULE_STATE_GUARD_CHECK` so sánh với
`GUARD_TAG_NONE` thay vì `0`. Verify bằng test thật: rule guard trên
`TAG_DI0`, DI0=0 → trước fix vẫn fire, sau fix bị chặn đúng.

**Việc còn để ngỏ:** `rule_table_commit()` chưa validate `guard_tag` nhận
từ App/Modbus — index nằm trong `128..(0x7FFF-1)` (không hợp lệ, cũng
không phải `GUARD_TAG_NONE`) sẽ khiến `tag_read()` tự chặn (trả 0, không
crash) nhưng rule luôn bị coi guard "đóng" âm thầm, không báo lỗi. Validate
ở `plc_modbus_cfg.c` hay ở `rule_table_commit()` — CHƯA QUYẾT ĐỊNH, mục 3.

### 1.3 Layer 0 + Layer 1 (`platforms/`, `components/`) — implementation thật, gap ADC đã đóng

| Peripheral | Layer 1 (`components/`) | Layer 0 (`platforms/stm32/stm32h5/`) |
|---|---|---|
| GPIO | `.h` contract (47 dòng) | `.c` thật (53 dòng) — `HAL_GPIO_Init/ReadPin/WritePin`, 4 mode |
| ADC | `.h` contract (42 dòng) | `.c` thật (57 dòng, xem 1.3.2) — `HAL_ADC_Init/Start/PollForConversion/GetValue` |
| Flash | `.h` contract (45 dòng) | `.c` thật (109 dòng) — quad-word program, sector erase, KHÔNG dùng macro `FLASH_SIZE`/`FLASH_BANK_SIZE` của CMSIS (gây Hard Fault thật + sai 512KB thay vì 256KB thật) |
| UART | `.h` contract (71 dòng) | `.c` thật (116 dòng) — `HAL_UART_Receive_IT` + binding table cố định |
| USB CDC | `.c` thật (151 dòng) qua TinyUSB (`tud_cdc_*`, `tud_task`) | `port/usb/tusb_config.h` (78) + `usb_descriptors.c` (193) |
| Time | `.h` (13 dòng) | `.c` (24 dòng) |
| I2C, Timer | `.h` tồn tại, **0 dòng** | Timer `.c`/`.h` tồn tại, **0 dòng** cả 2. I2C chưa có gì ở Layer 0 |

Timer/I2C trống không chặn Remote I/O SKU hiện tại (8DI/8DO/4AI).

#### 1.3.1 `sx_usb_tiny_read()` timeout sai đơn vị — VẪN CÒN, chưa sửa

`components/usb_cdc/sx_usb_cdc.c` dòng ~96-103: tham số `_timeoutMS`
nhưng thân hàm đếm số **vòng lặp** (`time++`), không phải mili-giây thực.
Chưa gây lỗi chức năng rõ ràng, nhưng cần sửa trước khi dựa vào timeout
này cho logic quan trọng (Modbus response timeout qua `modbus_usb.c`,
xem mục 2.3). **Re-verify: đã kiểm tra lại ở phiên này — bug vẫn còn
nguyên, không bị sửa nhầm bởi commit nào khác.**

#### 1.3.2 Gap ADC — ĐÃ ĐÓNG (mới, phiên này)

Trình tự đầy đủ: `.ioc` chưa cấu hình ADC → `Core/Inc/adc.h` không tồn
tại → build fail ngay bước include. Người dùng đã tự cấu hình ADC1/IN1
(Single-ended, 12-bit, Software trigger, polling — không Continuous,
không DMA) trong CubeMX và Generate Code (`Core/Inc/adc.h`,
`Core/Src/adc.c` giờ tồn tại thật, có `extern ADC_HandleTypeDef hadc1`).

**Hiện tại chỉ có 1 kênh (IN1 = `ADC_CHANNEL_1`) được cấu hình trong
`Core/Src/adc.c`** — sản phẩm cần 4AI, còn thiếu IN2/IN3/IN4. Không chặn
build (code Layer 0/1 tổng quát cho N kênh, chỉ cần thêm
`sx_adc_config_t` mới với `channel` khác cho mỗi kênh khi viết
`plc_io.c`), nhưng cần người dùng tự bật thêm 3 kênh trong `.ioc` +
Generate lại trước khi 4AI thật sự đọc đủ.

**2 bug thật phát hiện khi `stm32h5_adc.c` LẦN ĐẦU thực sự được compile**
(trước đó luôn fail sớm ở bước thiếu `adc.h`, nên 2 bug này chưa bao giờ
lộ ra):

1. `stm32h5_adc.c` dùng `sx_adc_config_t`/`sx_adc_resolution_t` nhưng
   không `#include "sx_adc.h"` ở đâu cả. **Người dùng tự sửa** — thêm
   `#include "sx_adc.h"` vào `stm32h5_adc.h` (không phải `.c` — hợp lý
   hơn, vì header nên tự đủ nghĩa cho ai include nó).
2. `sConfig.SamplingTime = ADC_SAMPLETIME_COMMON_1` — macro này **không
   tồn tại** trong HAL thật của STM32H5 (đã grep toàn bộ
   `Drivers/STM32H5xx_HAL_Driver/Inc/stm32h5xx_hal_adc.h`, chỉ có
   `ADC_SAMPLETIME_2CYCLES_5` .. `640CYCLES_5`). Claude sửa thành
   `ADC_SAMPLETIME_247CYCLES_5` — giá trị hợp lý cho input analog tần số
   thấp, đủ ổn định, vẫn nằm trong ngân sách 10ms scan-loop của driver.
   **Đây là 1 lựa chọn kỹ thuật thật, không chỉ compile-fix** — nếu sau
   này có spec chính xác về trở kháng/băng thông nguồn tín hiệu 4AI thật
   của sản phẩm, nên xem lại giá trị này (đã ghi rõ TODO trong code).

**Đã build-verify thật cả 5 file** (gpio/adc/flash/uart/time) bằng gcc,
đúng include path `platforms/stm32/stm32h5/CMakeLists.txt` khai báo — 0
lỗi, chỉ còn warning vô hại (`int-to-pointer-cast`) từ chính HAL header
gốc của ST khi cross-compile trên x86 (không xuất hiện khi build thật
cho ARM).

### 1.4 CMake — ĐÃ WIRE XONG TOÀN BỘ (mới, phiên này — khác hẳn bản handoff cũ)

**Đây là thay đổi lớn nhất so với bản handoff trước, vốn ghi "components/
CMakeLists.txt, platforms/CMakeLists.txt đều CHƯA TỒN TẠI".** Giờ có đủ
5 file CMakeLists, mỗi Layer 1 target, và root đã `add_subdirectory()`
+ `target_link_libraries()` xuyên suốt:

```
core/CMakeLists.txt                        → splc_core (STATIC, Layer 2)
components/CMakeLists.txt                  → splc_components (INTERFACE, xem 1.4.1)
platforms/stm32/stm32h5/CMakeLists.txt     → splc_platform_stm32h5 (STATIC, Layer 0)
libs/CMakeLists.txt                        → nanomodbus (STATIC, xem 1.4.2)
port/CMakeLists.txt                        → splc_port (STATIC, Layer 3.5, xem 2.3)
simple_plc/CMakeLists.txt                  → wire tất cả 5 target trên vào
                                              ${CMAKE_PROJECT_NAME} (executable gốc)
```

**Đã build-verify TOÀN BỘ dependency graph thật** bằng cmake+make thật
trong sandbox (cài `cmake` qua apt, dựng 1 `stm32cubemx` INTERFACE target
giả lập trỏ đúng include path/macro thật của repo vì sandbox không có
toolchain ARM đầy đủ): `cmake configure` thành công 100%, `make -k` build
được `splc_core` và `nanomodbus` hoàn chỉnh, gpio/flash/uart/time compile
sạch. Lúc verify đó ADC còn thiếu `adc.h` (gap đã đóng sau, xem 1.3.2) và
`splc_port` (modbus_usb) còn thiếu `tusb_types.h` (vẫn còn, xem 1.4.3) —
đây là 2 lỗi ĐÃ BIẾT, không phải lỗi phát sinh từ việc wire.

#### 1.4.1 `components/CMakeLists.txt` — vẫn là INTERFACE, chưa STATIC

`sx_usb_cdc.c` (file `.c` thật duy nhất ở Layer 1) vẫn chưa được thêm vào
`SPLC_COMPONENTS_SRC` — chặn bởi TinyUSB submodule chưa checkout (mục
1.4.3). Khi checkout xong, đổi `add_library(splc_components INTERFACE)`
→ `STATIC`, thêm `usb_cdc/sx_usb_cdc.c` vào source list — đã có sẵn
if/else trong file xử lý đúng 2 trường hợp, chỉ cần bỏ điều kiện.

#### 1.4.2 `libs/CMakeLists.txt` — nanoMODBUS build sạch, TinyUSB vẫn chờ

`nanomodbus.c` (~2460 dòng) build portable 100%, không phụ thuộc
platform gì (chỉ `<stdbool.h>/<stdint.h>/<string.h>`) — đã verify bằng
compile + link thật với 1 chương trình test gọi `nmbs_client_create()`,
chạy đúng. Không cần `target_compile_definitions` nào — mọi macro cấu
hình (`NMBS_SERVER_DISABLED` v.v.) đều có default hợp lý qua `#ifndef`.

TinyUSB: dùng `if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/tinyusb/src/tusb.c)`
guard, in `message(STATUS ...)` rõ ràng thay vì âm thầm bỏ qua khi
submodule chưa checkout. **Chưa tự viết wiring TinyUSB thật** (chọn
`dcd_synopsys.c` cho STM32H5, tusb_config...) — quyết định cần cẩn
trọng riêng, chưa làm.

#### 1.4.3 `port/CMakeLists.txt` — chỉ build `modbus_usb.c`, 2 file kia là stub cố ý bỏ qua

`splc_port` chỉ build `port/modbus_usb/modbus_usb.c` (file `.c` thật duy
nhất trong `port/`) — wrap `sx_usb_tiny_read/write/connected` (Layer 1)
đúng chữ ký `nmbs_platform_conf.read/write` mà nanoMODBUS cần. Quy ước
quan trọng: `arg` trong `nmbs_platform_conf` = `sx_usb_tiny_t*` do caller
sở hữu tường minh (không có instance global nào trong `modbus_usb.c`,
khớp pattern có sẵn của `sx_usb_cdc.c`) — `plc_modbus_cfg.c` (chưa viết)
sẽ cần tự giữ 1 biến `sx_usb_tiny_t`, gọi `sx_usb_tiny_init()` một lần,
truyền địa chỉ nó làm `arg`.

Đã build+test riêng bằng gcc với fake `sx_usb_tiny_read/write/connected`
(không cần TinyUSB/HAL thật) — 4 test case pass, gồm cả phép dịch
`timeout_ms < 0 → UINT32_MAX`.

**2 điểm chưa hoàn hảo, cố ý để TODO trong code (không phải thiếu sót
chưa phát hiện):**
- `modbus_usb_write()` bỏ qua `timeout_ms` vì `sx_usb_tiny_write()`
  không có tham số timeout riêng (luôn block tới khi ghi xong hoặc phát
  hiện mất kết nối) — nếu `plc_modbus_cfg.c` gọi
  `nmbs_set_byte_timeout(nmbs, 0)` (non-blocking write) thì hành vi thật
  sẽ KHÔNG khớp hợp đồng nanoMODBUS.
- `modbus_usb_write()` luôn trả `count` trên đường thành công vì
  `sx_usb_tiny_write()` không có giá trị trả về báo partial-write thật.

`port/modbus_serial/modbus_serial.c` (2 dòng, chỉ include, không có
hàm nào) và `port/modbus_tcp/modbus_tcp.c` (0 byte, hoàn toàn rỗng) **cố
ý KHÔNG đưa vào `SPLC_PORT_SRC`** — Gateway-variant transport, ngoài
phạm vi Remote I/O SKU hiện tại. Người dùng đã xác nhận: chỉ cần USB lúc
này, không viết thêm cho tới khi thật sự cần (xem mục 6).

**Việc còn thiếu để `plc_modbus_cfg.c` build được:** `splc_port` KHÔNG
tự link `nanomodbus` (đã verify: `modbus_usb.c`/`.h` không hề
`#include "nanomodbus.h"`, chỉ implement đúng chữ ký hàm bằng kiểu
chuẩn C). Việc link `nanomodbus` sẽ rơi vào tay ai viết
`services/CMakeLists.txt` sau này (chưa tồn tại).

### 1.5 `platforms/stm32/stm32h5/flash_define/splc_flash_define.h` — Flash memory map đã chốt xong, ĐÃ ĐỔI VỊ TRÍ

**Đổi vị trí so với bản handoff trước:** ban đầu Claude đặt file này ở
`app/splc_flash_define.h` (Layer 4) với lý do "quyết định sản phẩm cụ
thể". Người dùng đã tự di chuyển sang
`platforms/stm32/stm32h5/flash_define/splc_flash_define.h` (Layer 0) và
Claude xác nhận đây là vị trí hợp lý hơn: file này dùng thẳng macro
`FLASH_BASE` (CMSIS) không qua bất kỳ trừu tượng nào, và gắn chết với 1
chip cụ thể ngay từ nội dung — đúng bản chất Layer 0, không phải Layer 4.
Đã thêm `#include "sx_platform_config.h"` ở đầu file (theo đúng pattern
mọi file `stm32h5_*.h` khác trong Layer 0). Wire vào
`platforms/stm32/stm32h5/CMakeLists.txt`'s PUBLIC include dirs — bất kỳ
target nào link `splc_platform_stm32h5` tự động có header này, không cần
thêm include path riêng.

**QUAN TRỌNG cho ai đọc code cũ/tài liệu cũ:** mọi tham chiếu tới
`app/splc_flash_define.h` trong lịch sử chat/commit trước đây đều đã LỖI
THỜI — dùng đường dẫn mới ở trên.

Layout đã thống nhất với người dùng qua nhiều bước hỏi-đáp (vị trí Flash
→ kích thước cần → cơ chế retain có sẵn trong spec gốc → số sector cụ
thể → chấp nhận trade-off tuổi thọ):

```
STM32H523CCU6: 256KB, 32 sector x 8KB, dual-bank (Bank1=sector 0-15, Bank2=16-31)

Sector #31 (cuối cùng)        0x0803E000   Rule Table (1 sector, 8KB, dùng 3200/8192 byte)
Sector #27-30 (4 sector)      0x08036000   Retain (32KB, cơ chế xoay vòng EEPROM-emulation)
                               .. 0x0803DFFF
```

**Retain dùng cơ chế đã có sẵn trong `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md`
mục 7.1** (không phải thiết kế mới) — ghi định kỳ (mặc định 5 phút,
`RETAIN_SNAPSHOT_PERIOD_MS`, cấu hình qua Modbus) + ghi khẩn cấp khi PVD
phát hiện sụt áp; mỗi bản ghi có header (`seq_num`+`count`+`crc16`) + N
entry (`tag_index`+`value`); scan toàn vùng lúc boot tìm `seq_num` lớn
nhất + CRC hợp lệ, không lưu con trỏ riêng. **Cơ chế này giờ đã có
implementation thật, xem mục 1.7.2 — không còn chỉ là macro địa chỉ.**

**Con số trong spec v0.1 gốc (104 byte/record, 16 tag, 64KB/8 sector) đã
LỖI THỜI với v1.9** — `VREG_RETAIN` tăng từ 16 lên 32 slot. Đã tính lại
cho v1.9: record = 200 byte (header 8 + 32×6), 40 record/sector. Người
dùng chọn 4 sector (32KB, không phải 8 sector/64KB như spec gốc) —
160 record khả dụng, ghi mỗi 5 phút → tuổi thọ ước tính **~15 năm** với
endurance ~10.000 lần erase/sector (thấp hơn 8-sector/~30 năm nhưng người
dùng đã xác nhận chấp nhận được, xem mục 6).

**Đã build-verify thật** bằng CMSIS header thật của repo (không giả
lập): `FLASH_BASE` resolve đúng `0x08000000`, không có khoảng trống/đè
lẫn giữa Retain và Rule Table (`assert` pass), 3200 byte Rule Table thật
vừa khít 1 sector.

**Bug thật Claude tự phát hiện+sửa (không phải do người dùng di chuyển
file gây ra, mà là lỗi có sẵn từ bản gốc ở `app/`):** file này dùng
`FLASH_BASE` nhưng ban đầu không tự `#include "stm32h5xx_hal.h"` — chỉ
"chạy được" khi Claude test vì luôn tự thêm include đó vào file test
riêng. Khi `plc_retain.c` (Layer 3) include file này mà không có lý do
gì để tự thêm HAL header trước, lỗi build lộ ra ngay. Đã sửa: file giờ
tự `#include "stm32h5xx_hal.h"` trực tiếp, tự chứa đủ (self-contained).

File chỉ định nghĩa macro địa chỉ/kích thước — KHÔNG khai báo struct
retain record (đó là trong `plc_retain.c`, xem mục 1.7.2).

### 1.6 `utils/filter/` — 11 loại filter tín hiệu, có sẵn từ lâu nhưng CHƯA TỪNG được nhắc trong handoff (phát hiện mới, phiên này)

**Đây là khoảng trống lớn nhất của các bản handoff trước** — code đã tồn
tại từ nhiều commit trước (không phải mới viết phiên này), nhưng chưa
bản handoff nào từng đọc/nhắc tới. 819 dòng thật, đầy đủ implementation:

```
utils/filter/
├── MA_filter/ma_filt.h/.c           — Moving Average
├── Median_filter/median_filt.h/.c   — Median filter
├── Kalman_filter/kalman_filt.h/.c   — Kalman filter
└── IIR_filter/                      — 8 biến thể IIR (đều .h/.c riêng):
    ema_filt (Exponential MA), lpf_filt (Low-pass), hpf_filt (High-pass),
    bpf_filt (Band-pass), notch_filt (Notch), lsh_filt (Low-shelf),
    hsh_filt (High-shelf), peq_filt (Peaking EQ)
```

Kiểu API nhất quán (xem `ma_filt.h` làm mẫu): struct state + hàm `_init`
+ hàm lọc `static inline` cho hiệu năng, không cấp phát động (đúng
nguyên tắc "không heap" xuyên suốt project). Có SPDX license header.

**Mục đích rõ ràng dù chưa có tài liệu chính thức nhắc tới:** xử lý tín
hiệu 4AI analog input (lọc nhiễu trước khi ghi vào `TAG_AI0-3`) — đây là
việc `plc_io.c` (Layer 3, chưa viết) nhiều khả năng sẽ cần dùng tới khi
đọc ADC.

**CHƯA được wire vào bất kỳ CMakeLists nào** — không có
`utils/CMakeLists.txt`, `simple_plc/CMakeLists.txt` hiện chỉ build
`utils/cqueue/cqueue.c`. Cần quyết định: gộp tất cả filter vào 1
`utils/CMakeLists.txt` mới (theo đúng pattern layer khác), hay filter
nào dùng thì `plc_io.c`/`services/CMakeLists.txt` tự thêm source khi cần
— CHƯA HỎI người dùng, xem mục 3.

## 2. Việc CHƯA làm — theo layer, có ghi rõ cái gì đang chặn cái gì

### 2.1 Layer 3 (`services/`) — 3 file rỗng (0 dòng), KHÔNG còn bị chặn về hạ tầng

Layer 0/1 (GPIO/ADC/Flash/UART) đã xong (mục 1.3), CMake đã wire (mục
1.4), Flash layout đã chốt (mục 1.5) — chỉ còn thiếu quyết định thiết
kế cụ thể cho từng file, không phải hạ tầng thiếu.

**Thứ tự khuyến nghị, không đổi qua nhiều phiên:**

1. **`plc_retain.c`** — lựa chọn bắt đầu tốt nhất, chỉ cần Flash (đã
   xong) + `app/splc_flash_define.h` (đã xong, mục 1.5), không cần pin
   mapping hay USB. **Đang dở dang khi phiên này tạm dừng để viết
   handoff** — câu hỏi cuối cùng chưa có câu trả lời: xác nhận giữ đúng
   cơ chế **5 phút + PVD** như spec gốc mục 7.1, và cách xử lý phần PVD
   (PVD hoàn toàn CHƯA cấu hình trong `.ioc`, không có `PVD_IRQHandler`
   nào tồn tại — đề xuất: viết `plc_retain.c` đầy đủ cơ chế, có 1 hàm
   `retain_snapshot_write_emergency()` gọi được từ đâu cũng được, KHÔNG
   tự viết `PVD_IRQHandler` — giống cách đã xử lý gap ADC, chờ người
   dùng tự cấu hình `.ioc` sau). **Chưa có câu trả lời từ người dùng khi
   phiên này kết thúc — hỏi lại đầu phiên sau.**
2. **`plc_io.c`** — cần pin mapping thật (DI0-7/DO0-7 → GPIO port/pin,
   AI0-3 → ADC channel) — vẫn CHƯA CÓ ở đâu, xem mục 3. Có thể sẽ cần
   dùng `utils/filter/` để lọc tín hiệu AI (mục 1.6) — chưa quyết định
   dùng loại nào.
3. **`plc_modbus_cfg.c`** — phức tạp nhất, cần `port/modbus_usb/` (đã
   xong, mục 1.4.3) + nanoMODBUS link vào target (chưa, vì
   `services/CMakeLists.txt` chưa tồn tại) + sửa bug timeout USB (mục
   1.3.1) trước khi dùng cho response timeout thật.

### 2.2 `plc_system_cmd.c` (Layer 3/4) — chưa viết, đúng kế hoạch

Từng bị đề xuất viết ở Layer 2 rồi bị bác bỏ (xem mục 1.1) — việc thực
thi command thật (reboot, factory reset, clear rules/retain) cần
Flash/NVIC, thuộc Layer 3/4, viết sau khi các file Layer 3 khác xong.

### 2.3 Layer 4 (`app/plc_app/plc_engine.c`) — rỗng, chưa viết

Chỉ nên làm SAU KHI ít nhất `plc_io.c` xong. Đã thống nhất (chưa viết
code): `device_class`/`device_variant` truyền runtime qua
`plc_engine_init_params_t`, KHÔNG dùng compile-time `#ifdef`:

```c
typedef struct {
    SPLC_DeviceClass device_class;
    uint16_t         device_variant;
    uint16_t         hw_version_major;
    uint16_t         hw_version_minor;
    uint16_t         hw_version_patch;
} plc_engine_init_params_t;

void plc_engine_init(const plc_engine_init_params_t *params);
```

**Vẫn còn vấn đề cũ chưa giải quyết:** `rule_scan()` dùng biến
`static uint32_t s_rule_scan_now_ms` nội bộ, luôn = 0. Cách Layer 4
truyền tick thật vào (tham số hay setter) — CHƯA QUYẾT ĐỊNH, mục 3.

### 2.4 `utils/filter/` chưa wire vào CMake — xem mục 1.6 và mục 3

## 3. Các quyết định kiến trúc CHƯA CHỐT (đừng tự ý quyết định, hỏi lại)

1. **Cơ chế ghi Retain: giữ 5 phút + PVD hay đổi?** MỚI, câu hỏi vừa đưa
   ra cuối phiên này, CHƯA CÓ CÂU TRẢ LỜI. Hỏi lại đầu phiên sau.
2. **Pin mapping thật** (DI0-7/DO0-7 → GPIO port/pin, AI0-3 → ADC
   channel, ngoài IN1 đã có) — chưa có, đọc `RS485_IO_RF_V2.ioc` hoặc
   hỏi người dùng trước khi viết `plc_io.c`.
3. **Có dùng `utils/filter/` cho AI không, dùng loại nào** (MA/EMA/LPF/
   Median/Kalman...) — MỚI phát hiện có sẵn code (mục 1.6), chưa hỏi
   người dùng có ý định dùng hay không, và nếu dùng thì filter nào phù
   hợp cho tín hiệu 4AI thật của sản phẩm.
4. **Cách wire `utils/filter/` vào CMake** — 1 `utils/CMakeLists.txt`
   chung, hay để `plc_io.c` tự thêm source khi cần loại filter cụ thể.
5. **Cách Layer 4 truyền tick ms vào `rule_scan()`** — tham số hay setter
   function. Chưa quyết.
6. **Nguồn RTC cho `SPLC_TRG_TIME_WINDOW`** — chưa có, `plc_rule.c`
   hardcode `now_hhmm = 0`.
7. **Modbus Master cho Gateway** (`plc_modbus_master.c`) — RTU thôi hay
   cả TCP? Chưa quyết, chưa cần làm ngay — người dùng đã xác nhận chỉ
   cần USB lúc này (mục 6), việc Gateway để dành hẳn sau.
8. **Validate `guard_tag` ở đâu** (mục 1.2) — `plc_modbus_cfg.c` hay
   `rule_table_commit()`.
9. **Giá trị `ADC_SAMPLETIME_247CYCLES_5`** (mục 1.3.2) có đúng cho
   nguồn tín hiệu 4AI thật của sản phẩm không — cần spec trở
   kháng/băng thông thật để xác nhận, hiện đang dùng giá trị mặc định
   hợp lý.

## 4. Sửa nhỏ đã làm nhưng dễ quên — checklist tránh lặp lại lỗi cũ

- File CMake trùng lặp `platforms/CMakeLists.txt` (đặt sai vị trí,
  path tương đối trỏ sai) từng xuất hiện lặp lại NHIỀU LẦN qua các lần
  người dùng tự thao tác trên GitHub UI/local — mỗi lần đều phải phát
  hiện lại bằng cách so sánh `ls` 2 vị trí. Người dùng đã tự dọn sạch ở
  commit `5612b47`. Nếu thấy xuất hiện lại, xoá luôn, không cần hỏi —
  đã xác nhận nhiều lần đây luôn là rác, bản đúng chỉ ở
  `platforms/stm32/stm32h5/CMakeLists.txt`.
- `simple_plc/CMakeLists.txt` từng có comment mô tả gap ADC — ĐÃ CẬP
  NHẬT lại comment đó ở phiên này sau khi gap đóng (mục 1.3.2), để
  không hiểu nhầm là ADC còn thiếu khi đọc lại CMake sau này.

## 5. Lệnh verify nhanh Layer 2 (chạy lại bất cứ lúc nào)

```bash
mkdir -p /tmp/splc_verify && cd /tmp/splc_verify
cat > CMakeLists.txt << 'EOF'
cmake_minimum_required(VERSION 3.22)
project(splc_verify C)
add_subdirectory(<đường-dẫn-tới-repo>/simple_plc/core core_build)
add_executable(test_bin test_main.c)
target_link_libraries(test_bin PRIVATE splc_core)
EOF
cat > test_main.c << 'EOF'
#include <string.h>
#include <assert.h>
#include <stdio.h>
#include "plc_tag.h"
#include "plc_tag_def.h"
#include "plc_rule.h"
#include "plc_device.h"
#include "plc_error.h"
#include "plc_system_cmd.h"

int main(void) {
    tag_table_load_from_flash();
    rule_table_load_from_flash();

    assert(sizeof(SPLC_RuleRecord) == 32);
    assert(sizeof(SPLC_DeviceDescriptor) == 20);
    assert(sizeof(SPLC_DeviceHealth) == 20);
    assert(sizeof(SPLC_DeviceResourceInfo) == 20);
    assert(sizeof(SPLC_SystemCommandRequest) == 2);
    assert(sizeof(SPLC_SystemCommandResult) == 4);

    assert(TAG_DI0 == 0);
    assert(TAG_VREG_R15 == 99);
    assert(TAG_COUNTER7 == 123);

    g_tag_table[TAG_DI1].kind = TAG_DI;
    g_tag_table[TAG_DO2].kind = TAG_DO;
    SPLC_RuleRecord r = {0};
    r.trigger_tag = TAG_DI1; r.action_tag = TAG_DO2; r.action_param = 1;
    r.trigger_type = SPLC_TRG_ON_RISE; r.action_type = SPLC_ACT_SET_TAG; r.enabled = 1;
    r.guard_tag = GUARD_TAG_NONE;
    uint8_t raw[sizeof(r)]; memcpy(raw, &r, sizeof(r));
    assert(rule_table_commit(raw, 1));
    tag_write(TAG_DI1, 0); rule_scan(); assert(tag_read(TAG_DO2) == 0);
    tag_write(TAG_DI1, 1); rule_scan(); assert(tag_read(TAG_DO2) == 1);

    g_tag_table[TAG_DI0].kind = TAG_DI;
    g_tag_table[TAG_DI3].kind = TAG_DI;
    g_tag_table[TAG_DO3].kind = TAG_DO;
    SPLC_RuleRecord r2 = {0};
    r2.trigger_tag = TAG_DI3; r2.action_tag = TAG_DO3; r2.action_param = 1;
    r2.trigger_type = SPLC_TRG_ON_RISE; r2.action_type = SPLC_ACT_SET_TAG; r2.enabled = 1;
    r2.guard_tag = TAG_DI0;
    uint8_t raw2[sizeof(r2)]; memcpy(raw2, &r2, sizeof(r2));
    assert(rule_table_commit(raw2, 1));
    tag_write(TAG_DI0, 0);
    tag_write(TAG_DI3, 0); rule_scan();
    tag_write(TAG_DI3, 1); rule_scan();
    assert(tag_read(TAG_DO3) == 0);

    printf("ALL TESTS PASSED\n");
    return 0;
}
EOF
mkdir build && cd build && cmake .. && make && ./test_bin
```

Kỳ vọng: `ALL TESTS PASSED`. Đã re-run nhiều lần qua các phiên, luôn
pass (gcc trực tiếp lẫn cmake thật đều đã dùng, tuỳ sandbox có cmake hay
không).

## 6. Ghi chú quy trình làm việc với người dùng (bối cảnh, không phải kỹ thuật)

- Trao đổi bằng tiếng Việt, code/comment bằng tiếng Anh (giữ nguyên
  convention có sẵn trong repo).
- Người dùng tự push code lên GitHub — Claude không có quyền push, chỉ
  sửa file cục bộ rồi báo lại/present file. Khi bắt đầu phiên hoặc khi
  người dùng báo "đã push", LUÔN `git pull` trước; nếu sandbox có local
  edit chưa push, backup/stash trước khi pull để tránh "would be
  overwritten by merge" (đã gặp nhiều lần).
- **Sau MỖI LẦN người dùng báo "đã push", PHẢI verify lại bằng compile
  thật** (không chỉ đọc code/diff bằng mắt) — đã phát hiện nhiều lỗi thật
  theo cách này qua nhiều phiên: file CMake trùng lặp đặt sai vị trí
  (nhiều lần), thiếu `#include "sx_adc.h"`, macro
  `ADC_SAMPLETIME_COMMON_1` không tồn tại thật trong HAL.
- Người dùng đã đồng ý hướng "mỗi layer 1 CMakeLists riêng" (không gộp 1
  file chung) — đã áp dụng xuyên suốt, xem mục 1.4.
- Mọi thay đổi code nên verify bằng compile/chạy thật (gcc hoặc CMake
  thật trong sandbox), không chỉ đọc bằng mắt — người dùng đánh giá cao
  điều này, tiếp tục giữ.
- Khi đề xuất quyết định kiến trúc ảnh hưởng rộng, LUÔN hỏi trước bằng
  `ask_user_input_v0`, không tự quyết — đã dùng thành công nhiều lần
  (xoá `.c`/`extern` khỏi `plc_device`, tách `plc_error.h` riêng, chọn
  vị trí Flash cho Rule Table/Retain, số sector cho Retain).
- **Bài học mới, quan trọng, rút ra ở phiên viết `plc_retain.c`:** khi 1
  câu hỏi có nhiều quyết định con gộp chung (ví dụ: vị trí Flash + khi
  nào ghi + có cần wear-leveling), người dùng đã chủ động yêu cầu TÁCH
  NHỎ từng quyết định ra hỏi riêng lẻ, thay vì hỏi dồn 1 câu phức hợp —
  áp dụng nguyên tắc này cho mọi quyết định kiến trúc lớn tiếp theo, kể
  cả khi có vẻ "tiện" hỏi gộp.
- Người dùng có xu hướng tự làm song song 1 số việc trên CubeMX/GitHub
  UI (cấu hình `.ioc`, dọn file) trong lúc Claude đang làm việc khác —
  luôn pull + verify lại trước khi giả định trạng thái repo giống lần
  đọc gần nhất, kể cả giữa các câu hỏi liên tiếp trong cùng 1 phiên.