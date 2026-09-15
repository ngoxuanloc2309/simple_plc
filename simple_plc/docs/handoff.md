# SimplePLC — Handoff cho phiên làm việc tiếp theo

> Viết bởi Claude (phiên trước, hết token). Mục đích: cho phép 1 Claude
> khác (hoặc chính bạn) tiếp tục công việc mà không cần đọc lại toàn bộ
> lịch sử chat. Đọc file này SAU KHI đã đọc `Readme.md` và
> `docs/architecture.md` — file đó vẫn là nguồn kiến trúc chính, file này
> chỉ ghi lại "đang làm tới đâu" và "làm gì tiếp theo".
>
> **Đây là bản cập nhật lần 2**, thay thế hoàn toàn bản trước (từng dừng ở
> "Layer 2 còn thiếu plc_device/plc_system_cmd"). Layer 2 giờ đã xong hẳn —
> xem mục 1 bên dưới.

## 0. Trạng thái repo tại thời điểm viết file này

- Branch: `main`
- Commit mới nhất đã verify: `beac74a` ("modify cmake core")
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
│   └── plc_tag_def.h              — 69 #define cụ thể (TAG_DI0..TAG_VREG_R15), "Cách A"
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

## 2. Việc CHƯA làm — theo đúng thứ tự ưu tiên

### 2.1 Layer 0 + Layer 1 (`platforms/`, `components/`) — GẦN NHƯ TRỐNG, phải làm TRƯỚC Layer 3

**Đây là phát hiện quan trọng nhất của phiên này, khác hẳn giả định trước
đó.** Trước khi định viết `plc_io.c` (Layer 3), đã kiểm tra thật bằng `wc -l`
và xác nhận Layer 0/1 gần như chưa có gì:

| Peripheral | Layer 1 (`components/`) | Layer 0 (`platforms/stm32/stm32h5/`) |
|---|---|---|
| GPIO | `.h` rỗng (chỉ include guard) | `.h`/`.c` rỗng (0 dòng) |
| ADC | **CHƯA TỒN TẠI FILE NÀO CẢ** | **CHƯA TỒN TẠI FILE NÀO CẢ** |
| Flash | **CHƯA TỒN TẠI FILE NÀO CẢ** | **CHƯA TỒN TẠI FILE NÀO CẢ** |
| UART | có struct + 1 hàm signature (`sx_uart_init()`, chưa impl thật) | có `#if STM32H5_PLATFORM` skeleton, chưa impl |
| USB CDC | `.h` có struct (24 dòng), `.c` rỗng | chưa tồn tại (`sx_usb_tiny` theo `architecture.md` chưa được tạo file) |

Hệ quả trực tiếp: **`plc_io.c` (input_scan/output_scan) cần GPIO (DI/DO)
VÀ ADC (AI) chạy thật — cả 2 đều chưa có 1 dòng code.** `plc_retain.c` cần
Flash — cũng chưa có gì. Do đó **thứ tự trong bản handoff cũ
("plc_retain.c trước vì độc lập nhất") vẫn đúng về mặt phụ thuộc Layer 2,
nhưng cả 2 đều bị chặn bởi Layer 0/1 chưa xong**, không phải chỉ mỗi
`plc_io.c` như tưởng ban đầu.

**Việc tiếp theo, đã thống nhất với người dùng nhưng CHƯA CHỌN xong peripheral
nào làm trước** (câu hỏi để ngỏ, dùng `ask_user_input` lần cuối trong phiên
này nhưng chưa nhận được câu trả lời trước khi hết token) — 4 lựa chọn đã
đưa ra:
- GPIO trước (đơn giản nhất, cần cho DI/DO)
- ADC trước (cần cho AI)
- Flash trước (cần cho retain + rule table storage — xem mục 2.3 việc còn
  mở "vị trí Flash lưu Active Rule Table" cũng đụng tới đây)
- Làm GPIO+ADC cùng lúc vì cùng phục vụ `plc_io.c`

**Khuyến nghị cá nhân (không phải quyết định đã chốt):** GPIO trước — đơn
giản nhất trong 3 (chỉ cần `HAL_GPIO_ReadPin`/`WritePin`, không cần DMA/IT
như ADC, không cần wear-leveling như Flash), và cho phép có ngay 1 vertical
slice end-to-end nhỏ (DI vật lý → tag → rule → DO vật lý) để test thật sớm,
thay vì làm xong hết cả 3 peripheral rồi mới test được gì.

**Trước khi viết Layer 0/1 thật, cần đọc `.ioc` file** (`RS485_IO_RF_V2.ioc`
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

### 2.4 Layer 3 (`services/`) — 3 file rỗng (0 dòng), CHƯA VIẾT, BỊ CHẶN BỞI 2.1

Thứ tự khuyến nghị (không đổi so với bản cũ, nhưng giờ rõ ràng là BỊ CHẶN,
không phải "có thể làm song song"):
1. `plc_retain.c` — cần Flash (Layer 0/1) xong trước, KHÔNG còn "độc lập
   nhất" như bản handoff cũ ghi nhầm — bản cũ chưa kiểm tra thật trạng thái
   Layer 0/1 lúc viết câu đó.
2. `plc_io.c` — cần GPIO + ADC (Layer 0/1) xong trước, VÀ cần
   `plc_tag_def.h` (đã xong, xem mục 2.3) VÀ cần pin mapping thật
   (xem mục 2.1, chưa có).
3. `plc_modbus_cfg.c` — phức tạp nhất, cần USB CDC (Layer 0/1, hiện gần như
   trống) + nanoMODBUS (có code thật ở `libs/nanomodbus/`, 2462 dòng,
   nhưng CHƯA được nối vào build ở đâu). Vẫn nên làm SAU CÙNG trong Layer 3.

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