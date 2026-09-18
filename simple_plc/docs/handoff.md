# SimplePLC — Handoff cho phiên làm việc tiếp theo

> Viết bởi Claude. Mục đích: cho phép 1 Claude khác (hoặc chính bạn) tiếp
> tục công việc mà không cần đọc lại toàn bộ lịch sử chat. Đọc file này
> SAU KHI đã đọc `Readme.md` và `docs/architecture.md` — file đó vẫn là
> nguồn kiến trúc chính, file này chỉ ghi lại "đang làm tới đâu" và "làm
> gì tiếp theo".
>
> **Đây là bản VÁ TIẾP** trên nền bản trước (544 dòng, verify tại commit
> `52a9ef7`). Thay đổi chính từ bản trước tới giờ: **`plc_modbus_cfg.c`/
> `.h` (Layer 3) đã viết xong và build-verify thật** — server nanoMODBUS
> đầy đủ, toàn bộ register map v1.9 (mục 8/9 của tài liệu structs), Rule
> Transfer staging/CRC/commit protocol (mục 1.10 mới); và ngay sau đó,
> **giao diện `modbus_transport_t` (Layer 3.5, mục 1.11 mới) đã được
> thêm để tách `plc_modbus_cfg.c` khỏi USB cụ thể** — trả lời câu hỏi
> "sau này dùng Gateway thì có phải viết lại toàn bộ code không" bằng
> cách làm `plc_modbus_cfg.c` không còn include `sx_usb_cdc.h` nữa, chỉ
> biết một interface `read/write/process` chung. Không có quyết định
> kiến trúc lớn nào khác bị đảo ngược trong đợt vá này.

## 0. Trạng thái repo tại thời điểm viết file này

- Branch: `main`
- Commit mới nhất đã verify: `0330e4b` ("update docs 1" — bản handoff
  trước đó, viết bởi 1 phiên Claude khác làm việc trực tiếp với người
  dùng trong khi phiên hiện tại đang xử lý việc khác song song).
- **Phát hiện MỚI ở phiên vá này (chưa từng ghi ở đâu trước):**
  `components/CMakeLists.txt` link `tinyusb` PUBLIC KHÔNG ĐIỀU KIỆN
  (dòng ~91-93), nhưng `libs/CMakeLists.txt` chỉ tạo target `tinyusb`
  BÊN TRONG `if(EXISTS .../tinyusb/src/tusb.c)` (guard cho submodule
  TinyUSB chưa checkout). Nếu submodule chưa checkout (đúng tình trạng
  sandbox verify của Claude, và có thể cả máy người dùng nếu chưa chạy
  `git submodule update --init --recursive`), `cmake configure`/`build`
  sẽ fail với lỗi liên quan `tinyusb` target không tồn tại hoặc
  `tusb_types.h: No such file` — ĐÃ TÁI HIỆN THẬT bằng cmake+make trong
  sandbox. CHƯA SỬA, xem mục 3 câu hỏi 13 (mới) — cần hỏi người dùng
  máy thật đã checkout submodule chưa trước khi quyết định thêm guard
  hay không (nếu người dùng luôn checkout trước khi build, đây không
  phải bug thật cần sửa, chỉ là giả định ngầm chưa ghi rõ ràng).
- **Phát hiện MỚI, TÍCH CỰC:** TinyUSB đã được wire xong thật sự
  (`libs/CMakeLists.txt`, dùng `tinyusb_target_add()` — helper chính
  thức của TinyUSB) kể từ bản handoff trước, kèm 1 sửa lỗi kiến trúc
  quan trọng: driver đúng cho STM32H5 là **`stm32_fsdev`** (USB
  full-speed device-only, xác nhận thật qua `.ioc`:
  `NVIC.USB_DRD_FS_IRQn=true`), KHÔNG PHẢI `dcd_synopsys`/dwc2 như một
  ghi chú cũ hơn trong `architecture.md` từng giả định sai (đã tự sửa,
  xem comment trong `libs/CMakeLists.txt`). Việc này không do phiên vá
  hiện tại làm — ghi nhận lại vì đây là thông tin quan trọng, dễ bị bỏ
  sót nếu chỉ đọc phần "commit mới nhất".
- **1 sửa cục bộ Claude vừa áp lại trong phiên vá này** (patch này từng
  được note ở bản handoff trước là "cục bộ, chưa push" — giờ đã áp
  dụng lại, xem lịch sử chat để biết ai áp lần đầu):
  `port/CMakeLists.txt` — chỉ dọn 2 đoạn comment lỗi thời (từng viết
  "plc_modbus_cfg.c, Layer 3, not yet written" và một khối "TODO(wiring)"
  nói nanoMODBUS chưa link được vào target nào — cả 2 điều đó không còn
  đúng, `plc_modbus_cfg.c` đã tồn tại và `services/CMakeLists.txt` đã
  link `nanomodbus` từ lâu). **Không đổi logic/include path nào** — đã
  build-verify lại bằng cmake+make thật sau khi sửa, kết quả giống hệt
  trước khi sửa (chỉ khác đúng lỗi TinyUSB đã biết ở trên).
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

### 1.10 `services/plc_modbus_cfg/plc_modbus_cfg.c` (Layer 3) — HOÀN CHỈNH, build-verify bằng gcc thật (không cần toolchain ARM)

**Đây là module lớn nhất từng viết cho Layer 3** (~670 dòng `.c`, đúng
scope tài liệu `SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md`
mục 8/9). Sở hữu RAM instance của `g_device_descriptor`/
`g_device_resource_info`/`g_device_health` (Layer 4 ghi vào lúc boot,
file này chỉ expose read-only qua Modbus).

**Thiết kế: 1 bảng dispatch theo địa chỉ (`s_blocks[]`)**, không phải
switch/case khổng lồ — mỗi block map `[start_addr, end_addr]` tới
`read_cb`/`write_multi_cb`. `cb_read_holding_registers()`/
`cb_write_multiple_registers()` (2 callback nanoMODBUS gọi) tự động xử
lý 1 request tràn qua nhiều block liên tiếp, và mọi gap không map (ví dụ
`0x0011-0x001F` reserved) đọc trả về 0, ghi bị từ chối bằng
`NMBS_EXCEPTION_ILLEGAL_DATA_ADDRESS` — không bao giờ âm thầm chấp nhận
1 write mà App tưởng đã thành công.

Đã cài đủ toàn bộ block trong register map: `DEVICE_DESCRIPTOR`,
`RULE_TABLE_INFO`, `DEVICE_RESOURCE_INFO`, `ACTIVE_RULE_TABLE` (encode
16 register/rule, high-word-first, đúng field order `SPLC_RuleRecord`
Layer 2 đã có sẵn — không cần copy field thủ công), `DEVICE_HEALTH`
(tính `uptime_s` lazy lúc đọc, không tính mỗi scan cycle vì không ai
khác trong firmware cần giá trị này liên tục), `RUNTIME_TAG_VALUES` (2
register/tag cho MỌI loại tag, đúng checklist mục 8 file này —
`architecture.md`), `SYSTEM_COMMAND`/`_RESULT`.

**Rule Transfer staging protocol (0x9000-0xA001) đầy đủ:**
- `s_staging_rule_table[MAX_RULES]` là buffer RAM RIÊNG, không đụng
  `g_rule_table[]` (Layer 2) đang chạy — App có thể ghi dở dang qua
  nhiều Modbus transaction mà Rule Engine vẫn chạy bình thường trên
  bảng cũ suốt thời gian đó.
- `CONFIG_STATUS` state machine: IDLE → RECEIVING (App ghi
  `RULE_COUNT_STAGED`) → VERIFYING (App ghi `COMMIT_COMMAND = 0xA5A5`)
  → READY (CRC khớp, `rule_table_commit()` Layer 2 thành công) hoặc
  ERROR (CRC sai/magic sai/`rule_count_staged > MAX_RULES`).
- CRC verify dùng `nmbs_crc_calc()` có sẵn trong nanoMODBUS (CRC-16/
  MODBUS) — không viết thêm 1 bản CRC riêng cho việc này.
- `s_active_rule_version` tăng sau mỗi commit thành công — App có thể
  poll `ACTIVE_RULE_VERSION` (0xA001) để biết rule table vừa đổi mà
  không cần đọc lại toàn bộ `ACTIVE_RULE_TABLE` mỗi lần.

**`write_system_command()` (SYSTEM_COMMAND, 0x0A00) mới chỉ decode +
đổi `status` sang ACCEPTED — CHƯA THỰC THI lệnh thật** (không tự
`NVIC_SystemReset()`, không tự xoá Flash) — đúng ranh giới đã ghi trong
`plc_system_cmd.h`: thực thi thuộc Layer 4, chưa viết (xem mục 2.2).

**Build-verify:** compile bằng `gcc -std=c11 -Wall -Wextra` với stub
tối thiểu cho `tusb_types.h`/`cqueue.h`/`sx_time.h` (không cần toolchain
ARM lẫn TinyUSB thật, vì lúc verify này `plc_modbus_cfg.c` chưa include
gì từ USB nữa — xem mục 1.11) — 0 lỗi, 0 warning. Verify thêm bằng `nm`:
object file chỉ còn undefined symbol về Layer 2 (`tag_read`,
`rule_table_commit`, `g_rule_table`, `g_rule_count`) và nanoMODBUS, không
còn symbol nào tên `sx_usb_*`.

### 1.11 `port/modbus_transport/modbus_transport.h` (Layer 3.5, MỚI) — tách `plc_modbus_cfg.c` khỏi USB cụ thể

**Câu hỏi khởi nguồn:** "sau này dùng Gateway thay vì Remote I/O thì có
phải viết lại toàn bộ code không?" — và tiếp theo, "dùng macro để chuyển
đổi SKU được không?". Câu trả lời thứ hai đã có bài học thật trong chính
`plc_device.h` (1 bản nháp `#ifdef SPLC_DEVICE_CLASS_REMOTE_IO` từng
thất bại vì macro luôn được `#define`, `#ifdef` luôn đúng bất kể build
gì) — kết luận: chọn SKU là quyết định runtime + build-target khác file
(`board_remoteio.c`/`board_gateway.c`), không phải `#ifdef` trong 1 file
dùng chung.

**Vấn đề cụ thể phát hiện khi áp dụng nguyên tắc đó vào
`plc_modbus_cfg.c` vừa viết (mục 1.10):** hàm `plc_modbus_cfg_init()`
nhận thẳng `sx_usb_tiny_t *`, và `modbus_config_service()` gọi thẳng
`sx_usb_tiny_process()` theo tên — nghĩa là chữ ký hàm public của
Layer 3 đã "biết" nó chạy trên USB, dù `.c` không dùng macro nào. Nếu
viết `board_remoteio.c` trước với chữ ký này, sau này thêm Gateway
(RS485/UART) hoặc Modbus TCP sẽ phải sửa lại `plc_modbus_cfg_init()`
VÀ mọi board đã gọi nó theo chữ ký cũ.

**Giải pháp: `modbus_transport_t`** (`port/modbus_transport/
modbus_transport.h`, header-only, không include bất kỳ driver Layer 1
nào) — 1 struct nhỏ chỉ có `ctx` (opaque, `void*`), `read`/`write`
(khớp NGUYÊN VĂN chữ ký `nmbs_platform_conf.read/write` để gán thẳng
không cần adapter), `process` (bơm lớp dưới mỗi scan cycle, có thể
NULL), và `kind` (RTU hay TCP — thay cho `plc_modbus_cfg.c` cũ hard-code
`NMBS_TRANSPORT_RTU`).

**Cách wire:**
- `port/modbus_usb/modbus_usb.h`/`.c` thêm 1 hàm dựng
  `modbus_transport_usb_create(sx_usb_tiny_t *usb)` trả về
  `modbus_transport_t` đã điền sẵn — bọc `modbus_usb_read()`/
  `modbus_usb_write()` ĐÃ CÓ TỪ TRƯỚC, không viết lại logic transport
  thật, chỉ thêm 1 lớp đóng gói.
- `plc_modbus_cfg.h`/`.c`: bỏ hẳn `#include "sx_usb_cdc.h"`, đổi
  `plc_modbus_cfg_init(sx_usb_tiny_t *usb)` thành
  `plc_modbus_cfg_init(const modbus_transport_t *transport)` (copy theo
  giá trị vào biến static `s_transport`, không giữ con trỏ ngoài).
  `modbus_config_service()` gọi `s_transport.process(s_transport.ctx)`
  nếu khác NULL, thay vì gọi thẳng `sx_usb_tiny_process()`.
- Board init (Layer 4, chưa viết) sẽ gọi:
  ```c
  sx_usb_tiny_init(&usb, &usb_cfg);
  modbus_transport_t transport = modbus_transport_usb_create(&usb);
  plc_modbus_cfg_init(&transport);
  ```

**Việc KHÔNG đổi:** `modbus_usb_read()`/`modbus_usb_write()` (logic
transport thật) và toàn bộ 600+ dòng dispatch table/staging/CRC/commit
trong `plc_modbus_cfg.c` — chỉ đổi phần khởi tạo transport ở đầu/cuối
file.

**Việc CHƯA làm, cố ý để ngỏ:** MQTT (người dùng nhắc tới cho nạp rule
tương lai) KHÔNG dùng chung được interface `read/write` byte-stream này
— MQTT là pub/sub theo topic, không phải request/response theo địa chỉ
thanh ghi kiểu Modbus. Nếu sau này cần nạp rule qua MQTT, đó sẽ là 1
service Layer 3 khác hẳn (`plc_mqtt_cfg.c`?), dùng lại
`s_staging_rule_table`/CRC/commit logic nhưng đóng gói payload khác —
KHÔNG nằm trong phạm vi `modbus_transport_t`. `port/modbus_serial/` và
`port/modbus_tcp/` vẫn còn là stub gần như rỗng (không đổi từ mục
1.4.3) — chưa viết `modbus_transport_uart_create()`/
`modbus_transport_tcp_create()`, vì Gateway chưa tới lượt làm (xem mục
6, người dùng đã xác nhận chỉ cần USB lúc này).

**Bug đã biết, VẪN CÒN, không liên quan gì tới đợt sửa transport này**
(đã ghi từ mục 1.4.3, nhắc lại ở đây để không quên): `modbus_usb_write()`
bỏ qua `timeout_ms` vì `sx_usb_tiny_write()` không có chế độ
non-blocking thật — nếu App gọi với `nmbs_set_byte_timeout(nmbs, 0)`
(đúng như `plc_modbus_cfg_init()` đang làm), hành vi thật vẫn có thể
block quá ngân sách 10ms scan cycle nếu USB tạm nghẽn. Cần quyết định
thật trước khi coi kênh USB là production-ready.

## 2. Việc CHƯA làm — theo layer, có ghi rõ cái gì đang chặn cái gì

### 2.1 Layer 3 (`services/`) — CẢ 3 FILE ĐÃ VIẾT XONG (`plc_io.c`, `plc_retain.c`, `plc_modbus_cfg.c`)

**Không còn file nào rỗng ở Layer 3.** `plc_retain.c` và `plc_io.c` đã
xong từ bản handoff trước (mục 1.7/1.8); `plc_modbus_cfg.c` là bổ sung
mới nhất của đợt vá này (mục 1.10), cùng lớp transport-agnostic mới
(mục 1.11). Layer 3 không còn là điểm chặn của dự án nữa — điểm chặn
lớn nhất giờ chuyển hẳn sang **Layer 4** (mục 2.3 ngay dưới đây) và
**pin mapping thật** (mục 3).

Bug/TODO còn sót lại ở Layer 3, chưa phải việc mới:
- Bug timeout USB (mục 1.3.1, `sx_usb_tiny_read()` đếm vòng lặp thay vì
  mili-giây thật) — vẫn còn nguyên, chưa sửa.
- `modbus_usb_write()` không tôn trọng `byte_timeout_ms == 0` thật sự
  non-blocking (nhắc lại ở mục 1.11) — vẫn còn nguyên, chưa sửa.
- `plc_modbus_cfg.c`'s `write_system_command()` mới decode + đổi status,
  CHƯA thực thi lệnh thật (reboot/factory reset/clear) — đúng kế hoạch,
  việc đó thuộc Layer 4 (mục 2.2).

### 2.2 `plc_system_cmd.c` (Layer 3/4) — chưa viết, đúng kế hoạch

Từng bị đề xuất viết ở Layer 2 rồi bị bác bỏ (xem mục 1.1) — việc thực
thi command thật (reboot, factory reset, clear rules/retain) cần
Flash/NVIC, thuộc Layer 3/4, viết sau khi các file Layer 3 khác xong.

### 2.3 Layer 4 (`app/plc_app/plc_engine.c`) — rỗng, chưa viết, GIỜ LÀ ĐIỂM CHẶN LỚN NHẤT

Cả 3 file Layer 3 đã xong (mục 2.1), nên `plc_engine.c` không còn gì
chặn về hạ tầng nữa ngoài pin mapping thật (mục 3). Đã thống nhất (chưa
viết code): `device_class`/`device_variant` truyền runtime qua
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

**ĐÃ CHỐT (mục 3 câu hỏi 11):** `plc_engine_init()` KHÔNG gọi
`plc_modbus_cfg_init()` và KHÔNG nhận `modbus_transport_t` qua tham số
— 2 việc này tách biệt hoàn toàn. Board init (Layer 4, chưa viết) tự
gọi `plc_modbus_cfg_init(&transport)` riêng, TRƯỚC khi gọi
`plc_engine_init(&engine_params)`. `plc_engine_init_params_t` giữ
nguyên struct đã thống nhất ở trên, không thêm field transport. Xem
mục 3 câu hỏi 11 để đọc đầy đủ lý do và thứ tự gọi thật.

**Vẫn còn vấn đề cũ chưa giải quyết:** `rule_scan()` dùng biến
`static uint32_t s_rule_scan_now_ms` nội bộ, luôn = 0. Cách Layer 4
truyền tick thật vào (tham số hay setter) — CHƯA QUYẾT ĐỊNH, mục 3.

### 2.4 `utils/filter/` chưa wire vào CMake — xem mục 1.6 và mục 3

### 2.5 `board/` — chỉ có include guard rỗng, thảo luận thiết kế đã có nhưng CHƯA VIẾT CODE

`board/board.h`, `board/board.c`, `board/board_config.h` đều gần như
trống (12/0/5 dòng, chỉ include guard). Đã thống nhất pattern qua thảo
luận (KHÔNG viết code) — xem mục 3 câu hỏi 10 để biết 3 điểm cần chốt
trước khi viết thật:
- `board.h` khai báo `board_init()` (tầng chung) VÀ `board_hw_init()`
  (mỗi SKU tự implement, tránh đụng tên hàm khi cả 2 file cùng compile
  vào 1 target).
- Mỗi SKU (`board_remoteio.c`, sau này `board_gateway.c`) dùng macro
  CubeMX đã sinh sẵn trong `Core/Inc/gpio.h` (`OUT0_Pin`,
  `OUT0_GPIO_Port`, đã xác nhận thật tồn tại) — KHÔNG tự đánh số
  `GPIOA`/`GPIO_PIN_8` bằng tay, để tránh lệch pha khi regenerate CubeMX.
- Mỗi SKU tự gọi `plc_io_register_di/do/ai()` (Layer 3, đã có sẵn API
  đăng ký) trực tiếp trong `board_hw_init()` của nó — `board_gateway.c`
  có thể bỏ qua hoàn toàn phần này nếu không có I/O vật lý.

Pin mapping thật (DI/DO GPIO, AI ADC channel) vẫn CHƯA CÓ — cần đọc kỹ
`RS485_IO_RF_V2.ioc` (đã xác nhận `Core/Inc/main.h`, KHÔNG PHẢI `gpio.h`
như 1 ghi chú trước đó nhầm — CubeMX đặt macro pin label như `OUT0_Pin`/
`OUT0_GPIO_Port` trong `main.h`, `gpio.h` chỉ khai báo `MX_GPIO_Init()` —
đã tự grep lại để xác nhận đúng vị trí thật ở phiên vá này, có `OUT0-3`/
`IN0-3` = 4 output + 4 input, CHƯA đủ cho 8DI/8DO cần thiết) trước khi
viết `board_remoteio.c` thật.

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
10. **3 điểm cần chốt trước khi viết `board.h`/`board_remoteio.c` thật**
    (mục 2.5, MỚI) — đã thảo luận và có hướng đồng ý sơ bộ nhưng CHƯA
    CHỐT bằng code thật:
    - Tên hàm `board_init()` (tầng chung) vs `board_hw_init()` (mỗi
      SKU) — tránh đụng tên khi cả 2 file cùng compile vào 1 target.
    - Dùng macro CubeMX sinh sẵn (`OUT0_Pin`, ...) thay vì tự đánh số
      `GPIOA`/`GPIO_PIN_8` bằng tay trong `board_remoteio.c`.
    - Mỗi SKU tự gọi `plc_io_register_di/do/ai()` trực tiếp trong
      `board_hw_init()` của nó, `board_gateway.c` có thể bỏ hoàn toàn.
11. ~~**Layer 4 gọi `plc_modbus_cfg_init()` từ đâu, với transport
    nào**~~ — **ĐÃ CHỐT** (phiên vá này). Quyết định: **board init tự
    gọi `plc_modbus_cfg_init()` riêng, TRƯỚC khi gọi `plc_engine_init()`**
    (không phải `plc_engine_init()` tự nhận `modbus_transport_t` qua
    params). Lý do: `plc_retain.h`/`plc_io.h` (Layer 3, đã viết) đều
    dùng pattern "mỗi service tự đứng độc lập, không tham số phức tạp"
    (`retain_store_restore(void)`, `plc_io_register_di(uint16_t,
    sx_gpio_pin_t*)` — không hàm nào nhận struct tổng hợp) — để
    `plc_engine_init()` (dùng CHUNG cho mọi SKU, kể cả Gateway sau này)
    tự nhận `modbus_transport_t` sẽ làm rò rỉ khái niệm Modbus lên tầng
    Rule Engine, dù bản thân Rule Engine không liên quan gì tới Modbus.
    Giữ tách biệt giúp: unit test Rule Engine một mình không cần dựng
    `modbus_transport_t` giả; `board_gateway.c` (sau này) có thể chọn
    KHÔNG gọi `plc_modbus_cfg_init()` mà không phải sửa
    `plc_engine_init_params_t`. Thứ tự gọi thật trong board init (Layer
    4, chưa viết) sẽ là:
    ```c
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    retain_store_restore();

    sx_usb_tiny_init(&usb, &usb_cfg);
    modbus_transport_t transport = modbus_transport_usb_create(&usb);
    plc_modbus_cfg_init(&transport);

    plc_engine_init(&engine_params);  // CHỈ set device_class/variant/hw_version,
                                        // KHÔNG nhận modbus_transport_t
    ```
    `plc_engine_init_params_t` giữ nguyên như đã thống nhất trước (chỉ
    `device_class`, `device_variant`, `hw_version_major/minor/patch`) —
    không thêm field transport nào vào đó.
12. **Xác nhận `nmbs_set_byte_timeout(nmbs, 0)` có thực sự an toàn với
    `modbus_usb_write()` không** (mục 1.11, bug cũ nhắc lại) — trước khi
    coi kênh USB Modbus config service là production-ready, cần quyết
    định: sửa `sx_usb_tiny_write()` có chế độ non-blocking thật, hay
    xác nhận use case hiện tại không bao giờ cần write không-block thật
    sự (App luôn đợi được write xong trong ngân sách scan cycle).
13. **`components/CMakeLists.txt` link `tinyusb` PUBLIC không điều kiện,
    nhưng `libs/CMakeLists.txt` chỉ tạo target đó có điều kiện** (mục 0,
    MỚI phát hiện phiên vá này) — máy build thật của người dùng đã luôn
    `git submodule update --init --recursive` trước khi build chưa? Nếu
    có (luôn checkout trước), đây chỉ là 1 giả định ngầm nên ghi rõ
    thành comment, không phải bug cần sửa CMake. Nếu KHÔNG (có thể quên
    checkout, đặc biệt trên máy mới/CI sau này), `components/CMakeLists.txt`
    cần 1 `if(TARGET tinyusb)` (hoặc tương đương) quanh
    `target_link_libraries(splc_components PUBLIC tinyusb)` để
    `cmake configure` không vỡ hoàn toàn khi thiếu submodule — hiện tại
    Claude chưa tự sửa vì đây là quyết định "chấp nhận yêu cầu luôn
    checkout trước" hay "làm graceful khi thiếu", cần hỏi trước.

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

## 5.1 Lệnh verify nhanh `plc_modbus_cfg.c`/`modbus_usb.c` (MỚI — không cần toolchain ARM, không cần TinyUSB thật)

`plc_modbus_cfg.c` include `sx_time.h` trực tiếp (không qua USB) và,
sau đợt vá mục 1.11, KHÔNG còn include `sx_usb_cdc.h`/`modbus_usb.h`
nữa — chỉ `modbus_transport.h` (header-only, chỉ `<stdint.h>`). Nhờ
vậy, compile riêng file này bằng `gcc` thường vẫn khả thi, chỉ cần 3
header stub tối thiểu cho `modbus_usb.c` (không phải cho
`plc_modbus_cfg.c`):

```bash
mkdir -p /tmp/splc_verify_modbus/stubs && cd /tmp/splc_verify_modbus
cat > stubs/tusb_types.h << 'EOF'
#ifndef TUSB_TYPES_STUB_H
#define TUSB_TYPES_STUB_H
#endif
EOF
cat > stubs/cqueue.h << 'EOF'
#ifndef CQUEUE_STUB_H
#define CQUEUE_STUB_H
typedef struct { int dummy; } CQueue_t;
#endif
EOF
cat > stubs/sx_time.h << 'EOF'
#ifndef SX_TIME_STUB_H
#define SX_TIME_STUB_H
#include <stdint.h>
uint32_t sx_get_tick_ms(void);
#endif
EOF

cd <đường-dẫn-tới-repo>/simple_plc

gcc -c -std=c11 -Wall -Wextra \
  -I/tmp/splc_verify_modbus/stubs \
  -Iport/modbus_transport -Iport/modbus_usb -Icomponents/usb_cdc \
  port/modbus_usb/modbus_usb.c -o /tmp/splc_verify_modbus/modbus_usb.o

gcc -c -std=c11 -Wall -Wextra \
  -I/tmp/splc_verify_modbus/stubs \
  -Iport/modbus_transport -Iport/modbus_usb -Icomponents/usb_cdc \
  -Icore/plc_tag -Icore/plc_rule -Icore/plc_device -Icore/plc_error -Icore/plc_system_cmd \
  -Ilibs/nanomodbus \
  services/plc_modbus_cfg/plc_modbus_cfg.c -o /tmp/splc_verify_modbus/plc_modbus_cfg.o

# Xác nhận ranh giới layer thật sự có hiệu lực ở cấp linker, không chỉ comment:
nm -u /tmp/splc_verify_modbus/plc_modbus_cfg.o   # KHÔNG được có symbol nào tên sx_usb_*
nm /tmp/splc_verify_modbus/modbus_usb.o          # phải thấy sx_usb_tiny_* là undefined ở đây, không phải ở plc_modbus_cfg.o
```

Kỳ vọng: cả 2 lệnh `gcc -c` đều exit 0, 0 warning kể cả với `-Wall
-Wextra`. `nm -u plc_modbus_cfg.o` chỉ liệt kê symbol Layer 2
(`tag_read`, `rule_table_commit`, `g_rule_table`, `g_rule_count`) và
nanoMODBUS (`nmbs_*`) — không có `sx_usb_tiny_*` nào. Nếu sau này có ai
vô tình include lại `sx_usb_cdc.h` vào `plc_modbus_cfg.c`, lệnh `nm`
này sẽ lộ ra ngay (symbol `sx_usb_tiny_*` xuất hiện lại trong
`plc_modbus_cfg.o`), coi đây là dấu hiệu ranh giới Layer 3/3.5 bị vỡ.

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
- **Mốc quan trọng: `build.bat build` đã chạy THÀNH CÔNG THẬT trên máy
  Windows của người dùng** (toolchain ARM thật, không phải sandbox giả
  lập) — link ra `RS485_IO_RF_V2.elf`, RAM 3728B/272KB (1.34%), FLASH
  27456B (xem note ngay dưới về con số % bị sai). Đây là lần đầu tiên
  toàn bộ chain build thật (CMake + toolchain ARM + linker) được xác
  nhận hoạt động end-to-end, không chỉ verify từng phần trong sandbox.
- **BÀI HỌC NGHIÊM TRỌNG — Claude đã tự ý sửa 1 file CubeMX tự sinh,
  SAI, đã bị người dùng chỉnh đúng và đã revert:** lúc soát output build
  thành công ở trên, Claude nhận thấy linker báo `FLASH: 512 KB` trong
  khi chip thật (STM32H523CCU6) chỉ có 256KB (đúng như
  `stm32h5_flash.c`/`splc_flash_define.h` đã xác nhận nhiều lần) — nghi
  ngờ đúng, `STM32H523xx_FLASH.ld` (CubeMX tự sinh) quả thật khai
  `LENGTH = 512K` sai. NHƯNG Claude đã **tự ý sửa tay file đó** thay vì
  chỉ báo cho người dùng — vi phạm chính nguyên tắc Claude từng nhiều
  lần tự đặt ra ("file CubeMX sinh không phải của mình mà sửa", đã nói
  y hệt về `cmake/stm32cubemx/CMakeLists.txt`). Người dùng đã chỉnh
  đúng ngay: sửa tay sẽ MẤT khi CubeMX Generate Code lần sau (đè lại
  512K), và việc sửa đúng phải qua `.ioc`/CubeMX, không phải sửa file
  output. Đã `git checkout` revert lại bản gốc, KHÔNG giữ bản tự sửa.
  Người dùng sau đó yêu cầu bỏ qua việc này, tập trung việc chính — bug
  512K vẫn còn tồn tại thật trong repo (chưa ai sửa qua CubeMX), nhưng
  KHÔNG PHẢI việc Claude tự ý động vào lần nữa. **Quy tắc rút ra: mọi
  file có ghi "Auto-generated by STM32CubeIDE"/"generated only once" ở
  đầu — kể cả khi phát hiện bug thật trong đó — chỉ được BÁO CHO NGƯỜI
  DÙNG, không tự sửa, dù chỉ thêm comment giải thích.**