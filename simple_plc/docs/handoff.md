# SimplePLC — Handoff cho phiên làm việc tiếp theo

> Viết bởi Claude (phiên trước, hết token). Mục đích: cho phép 1 Claude
> khác (hoặc chính bạn) tiếp tục công việc mà không cần đọc lại toàn bộ
> lịch sử chat. Đọc file này SAU KHI đã đọc `Readme.md` và
> `docs/architecture.md` — file đó vẫn là nguồn kiến trúc chính, file này
> chỉ ghi lại "đang làm tới đâu" và "làm gì tiếp theo".
>
> **Đây là bản cập nhật lần 3**, thay thế hoàn toàn bản lần 2 (từng ghi
> Layer 0/1 "gần như trống" — KHÔNG còn đúng, xem mục 1.3 bên dưới). Layer 2
> vẫn hoàn chỉnh như bản lần 2 mô tả, cộng thêm 1 bugfix thật mới
> (`guard_tag` sentinel, mục 1.4). Layer 0/1 giờ đã có implementation thật
> cho GPIO/ADC/Flash/UART/Time/USB-CDC — khác hẳn giả định "phải chọn 1
> peripheral làm trước" mà bản lần 2 để ngỏ.

## 0. Trạng thái repo tại thời điểm viết file này

- Branch: `main`
- Commit mới nhất đã verify: `c45be5e` ("fix code according to Claude
  recommended")
- Lệnh verify: `git log --oneline -8` để xem có commit mới hơn không trước
  khi đọc tiếp phần dưới — nếu có commit mới, ưu tiên đọc code thật hơn
  file này.

## 1. Việc đã xong (đã build + test thật, không chỉ đọc code)

### 1.1 Layer 2 (`core/`) — HOÀN CHỈNH, đầy đủ theo checklist cũ

Cấu trúc thật hiện tại (6 module, mỗi module 1 thư mục con):

```
core/
├── plc_tag/
│   ├── plc_tag.h, plc_tag.c       — Tag Table (SPLC_TagKind, g_tag_table[], g_tag_value[])
│   └── plc_tag_def.h              — 124 #define cụ thể (TAG_DI0..TAG_COUNTER7),
│                                     "Cách A". ĐÃ CẬP NHẬT THEO V1.9: layout
│                                     mới bỏ sentinel TAG_NONE ở index 0 (v1.7
│                                     có 69 #define, TAG_DI0 ở index 1; v1.9 có
│                                     124 #define, TAG_DI0 ở index 0 — mọi index
│                                     dịch xuống 1 so với bản v1.7 cũ, và thêm
│                                     hẳn range COUNTER0-7 mới).
├── plc_rule/
│   ├── plc_rule.h, plc_rule.c     — SPLC_RuleRecord (32 byte), rule_scan(), rule_table_commit()
│   └── plc_rule_state_machine.h
├── plc_internal_rule/             — ĐỔI TÊN từ plc_internal/ (commit ca8125a trở về sau)
│   ├── plc_rule_eval.h/.c
│   └── plc_rule_action.h/.c
│   (chỉ plc_rule.c được include trực tiếp — PRIVATE trong CMake)
├── plc_device/
│   └── plc_device.h               — SPLC_DeviceClass, SPLC_*Variant, SPLC_DeviceDescriptor
│                                     (20B), SPLC_ResetReason, SPLC_HealthFlags,
│                                     SPLC_DeviceHealth (20B). CHỈ HEADER, không có .c.
├── plc_error/
│   └── plc_error.h                — SPLC_ErrorCode (dùng chung system_cmd + modbus_cfg sau này)
└── plc_system_cmd/
    └── plc_system_cmd.h           — SPLC_SystemCommand, SPLC_CommandStatus,
                                      SPLC_SystemCommandRequest (2B), SPLC_SystemCommandResult (4B)
                                      #include "plc_error.h". CHỈ HEADER, không có .c.
```

**Điểm quan trọng — 3 module cuối (`plc_device`, `plc_error`, `plc_system_cmd`)
KHÔNG có file `.c`, chỉ có `.h`.** Đây là quyết định có chủ đích, không phải
thiếu sót: các struct này (device descriptor, error code, system command)
không có logic nào thuần Layer 2 cả — mọi hàm thật sự đọc/ghi/xử lý chúng
đều cần đụng Layer 0/1 (đọc Flash lấy HW version, gọi NVIC_SystemReset,
board init gán device_class...). Layer 2 chỉ cung cấp *định nghĩa kiểu*, ai
cần dùng thì tự khai báo instance ở layer của mình (dự kiến Layer 3/4).
Đừng quay lại thêm `.c` + `extern g_device_descriptor` v.v. vào Layer 2 —
việc đó đã cân nhắc và bác bỏ, xem comment đầu file `plc_device.h` để đọc
lại lý do đầy đủ.

**Đã xoá 2 file nháp trùng lặp cũ** (từng nằm sai layer, gây lỗi compile
"redefinition" nếu include chung với `plc_device.h`):
- `app/app_config.h` — không xoá file, chỉ xoá nội dung struct trùng, để
  lại shell rỗng chờ config thật của Layer 4.
- `board/board_family.h` — xoá hẳn file, nội dung (3 variant enum) đã
  chuyển hết sang `plc_device.h`. Đồng thời sửa 1 bug logic: file cũ dùng
  `#ifdef SPLC_DEVICE_CLASS_REMOTE_IO` nhưng macro đó luôn `#define`'d
  (chỉ đổi giá trị 0/1), nên `#ifdef` luôn đúng — cả 3 khối variant enum bị
  compile cùng lúc bất kể SKU nào. `SPLC_DeviceClass` giờ là dữ liệu
  runtime trong `SPLC_DeviceDescriptor.device_class`, KHÔNG dùng compile-time
  macro nữa.

**Đã chốt xong 1 câu hỏi mở cũ:** đơn vị `scan_time_ms`/`max_scan_time_ms`
trong `SPLC_DeviceHealth` — xác nhận là **milliseconds** (đúng tên field),
không phải microsecond như comment stray trong docx gốc, và
`docs/architecture.md` ghi `scan_time_us` là **tên cũ/stale, không dùng**.
Đã sửa comment trong `plc_device.h` phản ánh đúng quyết định này.

**Đã verify:**
- `sizeof(SPLC_RuleRecord) == 32` (test cũ, vẫn pass)
- `sizeof(SPLC_DeviceDescriptor) == 20`
- `sizeof(SPLC_DeviceHealth) == 20`
- `sizeof(SPLC_SystemCommandRequest) == 2`
- `sizeof(SPLC_SystemCommandResult) == 4`
- `plc_tag_def.h`: 69 `#define` liên tục 0..68, không trùng/lệch, khớp
  `MAX_TAGS`. Test full range check `expected[i] == i` cho cả 69 giá trị.
- Toàn bộ 6 header include chung 1 file `.c` không xung đột (biên dịch
  test thật bằng gcc, không chỉ đọc mắt).
- Build full chain root→simple_plc→core qua CMake thật, link executable
  ngoài `splc_core`, tất cả `#include` resolve đúng (xác nhận PUBLIC/PRIVATE
  đúng — xem mục 1.2).

### 1.2 CMake (`core/CMakeLists.txt`) — đã sửa 2 lỗi thật sau lần đổi tên thư mục

Lịch sử: đợt đổi tên `plc_internal/` → `plc_internal_rule/` (để dọn ranh
giới rõ hơn) kèm theo request thêm `plc_device`/`plc_error`/`plc_system_cmd`
vào build đã gây 2 lỗi, PHÁT HIỆN VÀ SỬA bằng compile thật (không chỉ đọc
code), giữ lại đây để không lặp lại:

1. **`add_library()` trỏ sai path** — vẫn ghi `plc_internal/plc_rule_eval.c`
   sau khi thư mục đã đổi tên thành `plc_internal_rule/`. CMake báo lỗi
   `Cannot find source file`, build fail hoàn toàn, không tạo được
   Makefile. **Đã sửa** — path hiện tại đúng `plc_internal_rule/`.
2. **`plc_device`/`plc_error`/`plc_system_cmd` bị để `PRIVATE` thay vì
   `PUBLIC`** trong `target_include_directories()`. Hậu quả: bất kỳ ai
   link `splc_core` từ layer khác (Layer 3/4) sẽ KHÔNG tự động thấy được
   3 include path đó — `#include "plc_device.h"` từ file ngoài `core/` sẽ
   báo lỗi "No such file" dù file tồn tại thật. **Đã sửa** — chuyển sang
   `PUBLIC`. Ghi nhớ quy tắc: `PUBLIC` = cần cho cả bản thân `splc_core`
   LẪN ai link nó; `PRIVATE` = chỉ bản thân `splc_core` cần, không lộ ra
   ngoài (đúng cho `plc_internal_rule/` — chỉ `plc_rule.c` nội bộ dùng).

Trạng thái CMake hiện tại (xem file thật để chắc chắn, đây chỉ tóm tắt):
- `simple_plc/core/CMakeLists.txt` — `splc_core` build 4 file `.c`
  (`plc_tag.c`, `plc_rule.c`, `plc_rule_eval.c`, `plc_rule_action.c`),
  expose PUBLIC 5 include dir (`plc_tag`, `plc_rule`, `plc_device`,
  `plc_error`, `plc_system_cmd`), PRIVATE 1 include dir
  (`plc_internal_rule`).
- `simple_plc/CMakeLists.txt`, root `CMakeLists.txt` — chưa có thay đổi gì
  mới so với bản trước, vẫn `add_subdirectory()` xuyên suốt.

### 1.3 Layer 0 + Layer 1 (`platforms/`, `components/`) — ĐÃ CÓ IMPLEMENTATION THẬT, KHÔNG CÒN "GẦN NHƯ TRỐNG"

**Thông tin này đã LỖI THỜI trong bản handoff lần 2** (từng ghi "GẦN NHƯ
TRỐNG, phải làm TRƯỚC Layer 3" và để ngỏ câu hỏi "peripheral nào làm
trước"). Đã đọc lại thật bằng `wc -l` + đọc từng file `.c` ở phiên này:

| Peripheral | Layer 1 (`components/`) | Layer 0 (`platforms/stm32/stm32h5/`) |
|---|---|---|
| GPIO | `.h` sạch (contract rõ ràng, 47 dòng) | **`.c` đã implement thật** (53 dòng) — `HAL_GPIO_Init/ReadPin/WritePin`, xử lý đủ 4 mode (input/pullup/pulldown/output_pp) |
| ADC | `.h` có contract (42 dòng) | **`.c` đã implement thật** (55 dòng) — `HAL_ADC_Init/Start/PollForConversion/GetValue`, blocking với timeout 5ms |
| Flash | `.h` có contract (45 dòng) | **`.c` đã implement thật** (109 dòng) — `HAL_FLASH_Program` theo quad-word 16 byte, `HAL_FLASHEx_Erase` theo sector, có comment giải thích kỹ lý do KHÔNG dùng macro `FLASH_SIZE`/`FLASH_BANK_SIZE` của CMSIS header (gây Hard Fault thật trên STM32H5, và macro fallback sai kích thước 512KB thay vì 256KB thật của chip) |
| UART | `.h` có contract (71 dòng) | **`.c` đã implement thật** (116 dòng) — dùng `HAL_UART_Receive_IT` + binding table cố định (không malloc) để `HAL_UART_RxCpltCallback` dùng chung cho nhiều instance UART |
| USB CDC | **`.c` đã implement thật** (151 dòng, không còn rỗng) — dùng TinyUSB thật (`tud_cdc_read/write`, `tud_task`), có log qua `logger.h` | `port/usb/` có `tusb_config.h` (78 dòng, cấu hình CDC-only) + `usb_descriptors.c` (193 dòng) |
| Time | `.h` (13 dòng) | `.c` đã có (24 dòng) |
| I2C, Timer | `.h` tồn tại nhưng **0 dòng** (chỉ include guard) | Timer: `.c`/`.h` tồn tại nhưng **0 dòng** cả 2. I2C chưa có gì ở Layer 0. |

**Kết luận: KHÔNG còn phải chọn "peripheral nào làm trước" như bản handoff
lần 2 để ngỏ — GPIO, ADC, Flash, UART đều đã xong phần cốt lõi.** Timer và
I2C vẫn trống nhưng Remote I/O SKU hiện tại (8DI/8DO/4AI) không cần chúng
để chạy `plc_io.c`/`plc_retain.c`.

**Việc còn thiếu ở Layer 0/1 (không phải "chưa bắt đầu", mà là các mảnh
nhỏ hơn):**
- `sx_usb_tiny_read()` có tham số `_timeoutMS` nhưng thân hàm đếm số vòng
  lặp (`time++`), không phải mili-giây thực — timeout không chính xác đơn
  vị. Chưa gây lỗi chức năng rõ ràng nào, nhưng nên sửa trước khi dựa vào
  timeout này cho logic quan trọng (ví dụ Modbus response timeout).
- Timer (`components/timer/`, `platforms/.../timer/`) và I2C
  (`components/i2c/`) vẫn hoàn toàn trống (0 dòng) — không chặn Remote I/O
  SKU hiện tại, nhưng sẽ cần cho Datalogger/Gateway sau này nếu chúng cần
  timer phần cứng hay cảm biến I2C.
- Pin mapping thật (DI0-7/DO0-7 → chân GPIO cụ thể nào, AI0-3 → ADC channel
  nào) chưa có ở đâu trong code — xem mục 3.

Xin **KHÔNG lặp lại việc đọc "Layer 0/1 gần như trống" từ bản handoff cũ
hơn** — điều đó không còn đúng với code hiện tại trong repo.

### 1.4 Bugfix `guard_tag` sentinel — MỚI, đã sửa và verify bằng compile+chạy thật

**Vấn đề phát hiện:** `RULE_STATE_GUARD_CHECK` trong `plc_rule.c` từng so
sánh `guard_idx == TAG_NONE` (tức `== 0`) để quyết định "rule này không có
guard". Đúng dưới layout v1.7 (index 0 là ô sentinel vô nghĩa), nhưng SAI
dưới layout v1.9 (mục 1.1 ở trên, `plc_tag_def.h` đã cập nhật) — index 0
giờ là `TAG_DI0`, một tag thật. Hậu quả: **`TAG_DI0` từng là tag duy nhất
trong toàn hệ thống không thể dùng làm `guard_tag`** — bất kỳ rule nào set
`guard_tag = TAG_DI0` (= 0) với ý định gate theo DI0 sẽ bị hiểu nhầm thành
"không có guard" và luôn cho fire, bất kể DI0 thật sự bằng gì.

**Đã verify bằng compile+chạy thật** trước và sau khi sửa: viết 1 rule với
`guard_tag = TAG_DI0`, giữ DI0 = 0 — trước fix, rule vẫn fire (guard bị bỏ
qua hoàn toàn); sau fix, rule bị guard chặn đúng như kỳ vọng.

**Đã sửa** (`plc_rule.h`, `plc_rule.c`): thêm sentinel mới
`#define GUARD_TAG_NONE 0x7FFFu` (dùng toàn bộ 15 bit index — an toàn vì
`MAX_TAGS=128` << 32767, không tag hợp lệ nào trùng được). Logic
`RULE_STATE_GUARD_CHECK` giờ so sánh với `GUARD_TAG_NONE` thay vì `TAG_NONE`
(0). Đã comment rõ lý do ngay tại chỗ định nghĩa và tại chỗ dùng. Đã
regression-test lại toàn bộ Layer 2 sau khi sửa (dwell, edge trigger,
compare_op, sizeof checks) — không phá gì khác, `sizeof(SPLC_RuleRecord)`
vẫn = 32.

**Việc còn để ngỏ (chưa tự ý sửa, cần bàn khi viết Layer 3):**
`rule_table_commit()` hiện chưa validate `guard_tag` nhận từ App/Modbus —
nếu index bits nằm trong khoảng `128..(0x7FFF-1)` (không phải tag hợp lệ,
cũng không phải `GUARD_TAG_NONE`), `tag_read()` tự chặn (trả 0, không
crash) nhưng rule sẽ luôn bị coi guard "đóng" âm thầm, không báo lỗi gì.
Nên validate ở đâu — `plc_modbus_cfg.c` (Layer 3, lúc giải mã dữ liệu từ
Modbus) hay thêm validate ở `rule_table_commit()` (Layer 2) — CHƯA QUYẾT
ĐỊNH, xem mục 3.

**Trước khi bổ sung Layer 0/1 (timer/I2C hay sửa timeout USB), vẫn nên đọc `.ioc` file** (`RS485_IO_RF_V2.ioc`
ở root repo) hoặc hỏi người dùng về pin mapping thật (DI0-7/DO0-7 nối chân
GPIO nào, AI0-3 nối ADC channel nào) — chưa có thông tin này trong bất kỳ
doc nào đã đọc, cần hỏi trước khi viết phần map cụ thể trong `plc_io.c`
sau này (`plc_io.c` bản thân cũng chưa viết, xem mục 2.4).

### 2.2 CMake — tách tiếp Layer 0/1 khi có code thật

Áp dụng đúng mẫu `core/CMakeLists.txt` (PUBLIC cho header cần lộ ra ngoài,
PRIVATE cho header chỉ nội bộ — xem mục 1.2 để hiểu đúng khác biệt trước
khi tự làm, đã có bài học thật từ lỗi PUBLIC/PRIVATE ở Layer 2):
- `components/CMakeLists.txt` → library `splc_components` (Layer 1)
- `platforms/CMakeLists.txt` → library `splc_platform` (Layer 0)
Cả 2 đều CHƯA TỒN TẠI, `simple_plc/CMakeLists.txt` hiện add trực tiếp qua
`target_sources()` (nếu còn — cần re-check file thật, phần này có thể đã
đổi so với lần đọc gần nhất).

### 2.3 `plc_tag_def.h` — ĐÃ XONG (khác bản handoff cũ)

Bản handoff trước liệt việc này là "chưa viết, cần cho `plc_io.c`". Đã
xong — xem mục 1.1. Không cần làm lại.

### 2.4 Layer 3 (`services/`) — 3 file rỗng (0 dòng), CHƯA VIẾT — KHÔNG CÒN BỊ CHẶN NHƯ TRƯỚC

**Cập nhật quan trọng so với bản handoff lần 2:** lúc đó Layer 3 bị coi là
"BỊ CHẶN BỞI 2.1" (Layer 0/1 chưa xong). Giờ Layer 0/1 đã có GPIO/ADC/Flash/
UART thật (xem mục 1.3) — **Layer 3 không còn bị chặn về mặt hạ tầng
peripheral nữa**, chỉ còn thiếu pin mapping thật (mục 3) và việc nối
nanoMODBUS vào build (bên dưới).

Thứ tự khuyến nghị (không đổi thứ tự so với bản cũ, chỉ cập nhật lý do
chặn/không chặn từng bước):
1. `plc_retain.c` — Flash (Layer 0/1) đã xong, **không còn bị chặn**. Vẫn
   là lựa chọn "độc lập nhất" để bắt đầu vì không cần pin mapping.
2. `plc_io.c` — GPIO + ADC (Layer 0/1) đã xong, **không còn bị chặn về mặt
   driver**. Vẫn cần `plc_tag_def.h` (đã xong, xem mục 2.3) VÀ pin mapping
   thật (DI0-7/DO0-7 → GPIO port/pin nào, AI0-3 → ADC channel nào) — vẫn
   CHƯA CÓ ở đâu trong code, xem mục 3.
3. `plc_modbus_cfg.c` — phức tạp nhất. USB CDC (Layer 1) đã có
   implementation thật (mục 1.3), nhưng còn 2 việc chưa xong: (a)
   `port/modbus_usb/` (Layer 3.5, nối nanoMODBUS với `sx_usb_tiny_read/
   write`) chưa tồn tại — hiện `port/` chỉ có `port/usb/` (cấu hình TinyUSB
   descriptor, không phải Modbus porting layer) và `port/modbus_serial/`
   (2 file stub gần rỗng, dành cho Gateway); (b) nanoMODBUS
   (`libs/nanomodbus/`, ~3000 dòng, code có sẵn) chưa được compile vào bất
   kỳ target CMake nào — root `CMakeLists.txt` mới thêm `libs/` vào include
   path chung, chưa có `add_library`/`target_sources` thật. Vẫn nên làm
   SAU CÙNG trong Layer 3.

### 2.5 `plc_system_cmd.c` (Layer 3/4, KHÔNG phải Layer 2) — chưa viết, đúng như kế hoạch

Bản handoff cũ từng đề xuất viết `plc_system_cmd.c` NGAY TẠI Layer 2 — quyết
định đó đã bị BÁC BỎ trong phiên này (xem mục 1.1, lý do đầy đủ). Việc thực
thi command thật (reboot, factory reset, clear rules, clear retain) cần Flash/
NVIC — thuộc Layer 3/4, viết SAU KHI Layer 0/1 (đặc biệt Flash) xong.

### 2.6 Layer 4 (`app/plc_app/plc_engine.c`) — rỗng, CHƯA VIẾT

Chỉ nên làm SAU KHI ít nhất `plc_io.c` xong (không đổi so với bản cũ).
Khung hàm tham khảo `architecture.md` mục "Layer 4" vẫn đúng, không đổi.

**Điểm mới đã thống nhất với người dùng, CHƯA THỰC HIỆN (chỉ mới bàn, chưa
viết code):** việc chọn `device_class`/`device_variant` để gán vào
`g_device_descriptor` (Layer 4 sẽ khai báo instance thật, xem mục 1.1) sẽ
KHÔNG dùng compile-time `#ifdef` kiểu `board_family.h` cũ đã xoá — mà dùng
**param truyền vào lúc runtime** cho `plc_engine_init()`, ví dụ:

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

Board cụ thể (`main.c` của SKU đó) tự điền struct này và truyền vào lúc
gọi init — không phải `#ifdef SPLC_DEVICE_CLASS_REMOTE_IO` nữa. Đây là hệ
quả trực tiếp của quyết định bỏ macro ở mục 1.1, người dùng đã xác nhận
hướng này, CHỈ CẦN ÁP DỤNG KHI THẬT SỰ VIẾT `plc_engine.c`, không cần hỏi
lại.

**Vẫn còn nguyên vấn đề cũ chưa giải quyết:** `rule_scan()` hiện tại
(Layer 2) dùng biến `static uint32_t s_rule_scan_now_ms` nội bộ, luôn = 0.
Cách Layer 4 truyền tick thật vào — thêm tham số hay setter function — vẫn
CHƯA QUYẾT ĐỊNH, xem mục 3.

## 3. Các quyết định kiến trúc CHƯA CHỐT (đừng tự ý quyết định, hỏi lại)

1. **Peripheral nào làm trước ở Layer 0/1** — GPIO/ADC/Flash/cả GPIO+ADC.
   Câu hỏi đã đưa ra cho người dùng, CHƯA CÓ CÂU TRẢ LỜI khi phiên này kết
   thúc. Hỏi lại đầu phiên sau nếu chưa thấy trả lời trong lịch sử chat.
2. **Pin mapping thật** (DI0-7/DO0-7 → chân GPIO nào, AI0-3 → ADC channel
   nào) — chưa có, cần đọc `RS485_IO_RF_V2.ioc` hoặc hỏi người dùng trực
   tiếp trước khi viết phần map cụ thể trong `plc_io.c`.
3. **Vị trí Flash lưu Active Rule Table** — địa chỉ, kích thước tối thiểu
   `100*32=3200` byte, có cần wear-leveling như dự kiến cho `plc_retain.c`
   hay ghi đè 1 chỗ cố định. Chưa quyết — không đổi so với bản cũ.
4. **Cách Layer 4 truyền tick ms vào `rule_scan()`** — thêm tham số hay
   setter function. Chưa quyết — không đổi so với bản cũ.
5. **Nguồn RTC cho `SPLC_TRG_TIME_WINDOW`** — chưa có, hiện `plc_rule.c`
   hardcode `now_hhmm = 0`. Không đổi so với bản cũ.
6. **Modbus Master cho Gateway** (`plc_modbus_master.c`) — RTU thôi hay cả
   TCP? Chưa quyết, chưa cần làm ngay. Không đổi so với bản cũ.

## 4. Lệnh verify nhanh (chạy lại bất cứ lúc nào để kiểm tra Layer 2 + CMake còn sạch)

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

    /* Layer 2 sizeof checks per v1.7 */
    assert(sizeof(SPLC_RuleRecord) == 32);
    assert(sizeof(SPLC_DeviceDescriptor) == 20);
    assert(sizeof(SPLC_DeviceHealth) == 20);
    assert(sizeof(SPLC_SystemCommandRequest) == 2);
    assert(sizeof(SPLC_SystemCommandResult) == 4);

    /* plc_tag_def.h range check */
    assert(TAG_DI0 == 1 && TAG_VREG_R15 == 68);

    /* end-to-end rule test (unchanged from original handoff) */
    g_tag_table[1].kind = TAG_DI;
    g_tag_table[9].kind = TAG_DO;
    SPLC_RuleRecord r = {0};
    r.trigger_tag = 1; r.action_tag = 9; r.action_param = 1;
    r.trigger_type = SPLC_TRG_ON_RISE; r.action_type = SPLC_ACT_SET_TAG; r.enabled = 1;
    uint8_t raw[sizeof(r)]; memcpy(raw, &r, sizeof(r));
    assert(rule_table_commit(raw, 1));
    tag_write(1, 0); rule_scan(); assert(tag_read(9) == 0);
    tag_write(1, 1); rule_scan(); assert(tag_read(9) == 1);

    printf("ALL TESTS PASSED\n");
    return 0;
}
EOF
mkdir build && cd build && cmake .. && make && ./test_bin
```

Kỳ vọng output cuối: `ALL TESTS PASSED`.

## 5. Ghi chú quy trình làm việc với người dùng (bối cảnh, không phải kỹ thuật)

- Người dùng thích trao đổi bằng tiếng Việt, code/comment bằng tiếng Anh
  (giữ nguyên convention đã có trong repo).
- Người dùng tự push code lên GitHub sau khi Claude sửa/viết xong — Claude
  KHÔNG có quyền push, chỉ sửa file cục bộ trong sandbox rồi báo lại. Khi
  bắt đầu phiên mới, LUÔN `git pull` trước để lấy thay đổi mới nhất người
  dùng đã tự push — và LUÔN `git checkout -- . && git clean -fd` trước
  khi pull nếu sandbox có local edit, để tránh lỗi "would be overwritten
  by merge" (đã gặp thật nhiều lần trong phiên này).
- **Bài học quan trọng đã rút ra trong phiên này:** người dùng có thể tự
  sửa/push code KHÔNG khớp với bản Claude đã đề xuất trước đó (ví dụ:
  push lại bản `app_config.h`/`board_family.h` cũ dù đã thống nhất xoá,
  hoặc đổi tên thư mục mà quên cập nhật CMakeLists.txt tương ứng). Sau
  MỖI LẦN người dùng báo "đã push", PHẢI pull rồi verify lại bằng compile
  thật (không chỉ đọc code bằng mắt) trước khi tiếp tục — đã phát hiện
  nhiều lỗi thật theo cách này (redefinition conflict, sai path source,
  sai PUBLIC/PRIVATE).
- Người dùng đã đồng ý hướng "mỗi layer 1 CMakeLists riêng" (không gộp 1
  file chung) — không đổi so với bản cũ.
- Mọi thay đổi code nên được verify bằng compile/chạy thật (gcc hoặc
  CMake thật trong sandbox), không chỉ đọc code bằng mắt — người dùng
  đánh giá cao việc này, đã áp dụng xuyên suốt phiên này, tiếp tục giữ.
- Khi đề xuất quyết định kiến trúc có ảnh hưởng rộng (ví dụ: tách file,
  đổi vị trí struct, gộp/tách module), LUÔN hỏi người dùng trước bằng
  `ask_user_input_v0`, KHÔNG tự quyết — người dùng đã yêu cầu rõ điều này
  và đã dùng cách này thành công nhiều lần trong phiên (ví dụ: quyết định
  xoá `.c`/`extern` khỏi `plc_device`, quyết định tách `plc_error.h` riêng).