# SimplePLC — Handoff cho phiên làm việc tiếp theo

> Viết bởi Claude (phiên trước, hết token). Mục đích: cho phép 1 Claude
> khác (hoặc chính bạn) tiếp tục công việc mà không cần đọc lại toàn bộ
> lịch sử chat. Đọc file này SAU KHI đã đọc `Readme.md` và
> `docs/architecture.md` — file đó vẫn là nguồn kiến trúc chính, file này
> chỉ ghi lại "đang làm tới đâu" và "làm gì tiếp theo".

## 0. Trạng thái repo tại thời điểm viết file này

- Branch: `main`
- Commit mới nhất đã verify: `03ed248` ("add cmake")
- Lệnh verify: `git log --oneline -8` để xem có commit mới hơn không trước
  khi đọc tiếp phần dưới — nếu có commit mới, ưu tiên đọc code thật hơn
  file này.

## 1. Việc đã xong (đã build + test thật, không chỉ đọc code)

### 1.1 Layer 2 (`core/`) — HOÀN CHỈNH cho phạm vi hiện tại

Tất cả các lỗi build đã được sửa và verify bằng compile + chạy thật (gcc
thủ công VÀ CMake thật, không chỉ đọc code):

- `core/plc_tag/plc_tag.h`, `.c` — sửa lỗi `TagKind` → `SPLC_TagKind`.
- `core/plc_rule/plc_rule.h` — thêm `RuleExecState` enum, `DWELL_NOT_STARTED`,
  thêm field `state` vào `SPLC_RuleRuntime`, đổi `rule_table_commit()` sang
  `bool` + `uint16_t rule_count`.
- `core/plc_rule/plc_rule_state_machine.h` — file mới, khai báo
  `rule_state_machine_step()`.
- `core/plc_rule/plc_rule.c` — viết lại, đồng bộ tên type sang `SPLC_*`,
  **bổ sung implementation còn thiếu** cho `g_rule_table`/`g_rule_runtime`/
  `g_rule_count`, `rule_table_load_from_flash()`, `rule_scan()`,
  `rule_table_commit()` (trước đó hoàn toàn chưa có).
- `core/plc_internal/plc_rule_action.h`, `.c` — đồng bộ tên type sang
  `SPLC_*`.
- `core/plc_internal/plc_rule_eval.h`, `.c` — đã đúng chuẩn từ trước,
  không cần sửa.

**Đã verify:** `sizeof(SPLC_RuleRecord) == 32` (đúng spec v1.7). Test
end-to-end: commit 1 rule "ON_RISE DI1 → SET DO1" → `rule_scan()` →
tag DO1 đổi giá trị đúng như kỳ vọng. PASS.

### 1.2 CMake — đã tách theo layer, build thật qua CMake (không chỉ gcc tay)

- `simple_plc/core/CMakeLists.txt` — Layer 2 là 1 static library riêng
  (`splc_core`), không link bất kỳ layer nào khác (đúng nguyên tắc "ranh
  giới port" trong architecture.md).
- `simple_plc/CMakeLists.txt` — `add_subdirectory(core)` +
  `target_link_libraries(${CMAKE_PROJECT_NAME} PRIVATE splc_core)`. Các
  layer khác (0/1/3/3.5/4/U) TẠM THỜI vẫn add trực tiếp vào executable
  qua `target_sources()` vì chưa có `CMakeLists.txt` riêng — xem mục 2.3.
- `CMakeLists.txt` (root) — thêm `add_subdirectory(simple_plc)`.

**Đã verify bằng CMake thật** (cài `cmake` trong sandbox, không có sẵn ban
đầu): mô phỏng full chain root→simple_plc→core, build ra
`libsplc_core.a`, link vào executable giả lập tên `RS485_IO_RF_V2`, chạy
đúng. Xem lệnh verify ở cuối file này để lặp lại khi cần.

## 2. Việc CHƯA làm — theo đúng thứ tự ưu tiên

### 2.1 Layer 2 — còn thiếu 2 file (chưa viết, chỉ mới có trong docs)

Đọc kỹ `docs/SimplePLC_App_MCU_Structs_v1.7.md` mục 1, 4, 6, 7 trước khi
viết — mọi field/enum dưới đây đã có sẵn định nghĩa chính xác trong đó,
KHÔNG cần tự nghĩ ra field mới.

- **`core/plc_device/plc_device.h` + `.c`** (chưa tồn tại):
  - `SPLC_DeviceClass`, `SPLC_RemoteIoVariant`, `SPLC_DataloggerVariant`,
    `SPLC_GatewayVariant`
  - `SPLC_DeviceDescriptor` (20 byte, RO)
  - `SPLC_ResetReason`, `SPLC_HealthFlags`
  - `SPLC_DeviceHealth` (20 byte, RO) — **CHÚ Ý:** field tên
    `scan_time_ms`/`max_scan_time_ms` trong v1.7 nhưng comment ghi đơn vị
    thật là **microsecond** — mâu thuẫn tên/đơn vị nằm sẵn trong tài liệu
    gốc (không phải lỗi bạn tạo ra). `architecture.md` lại ghi tên field
    là `scan_time_us`/`max_scan_time_us`. **Cần hỏi lại người viết spec
    xem đây có phải lỗi đánh máy trong docx gốc không, trước khi chốt tên
    field cuối cùng trong code.** Đừng tự quyết định 1 trong 2, vì đây là
    field nằm trong Modbus register map (0x0800-0x0809) — sai tên không
    sao (nó chỉ là tên C), nhưng sai ĐƠN VỊ (ms vs µs) khi implement thật
    sẽ khiến App hiển thị sai số cho người dùng.
  - `extern SPLC_DeviceDescriptor g_device_descriptor;` (hằng số biên dịch)
  - `extern SPLC_DeviceHealth g_device_health;` (cập nhật liên tục bởi
    Layer 3/4)

- **`core/plc_system_cmd/plc_system_cmd.h` + `.c`** (chưa tồn tại), hoặc
  gộp vào `plc_device` nếu thấy hợp lý hơn:
  - `SPLC_SystemCommand` enum (NONE/REBOOT/FACTORY_RESET/CLEAR_RULES/
    CLEAR_RETAIN)
  - `SPLC_CommandStatus` enum (IDLE/ACCEPTED/BUSY/DONE/ERROR)
  - `SPLC_SystemCommandRequest` (2 byte), `SPLC_SystemCommandResult`
    (4 byte)
  - `SPLC_ErrorCode` — cân nhắc đặt ở file riêng `plc_error.h` vì dùng
    chung cho cả system command LẪN Modbus config service
    (`plc_modbus_cfg.c`, chưa viết).

**Sau khi viết xong 2 file này, nhớ verify bằng cách thêm vào
`core/CMakeLists.txt`'s `target_sources()` và build lại + viết 1 test nhỏ
kiểm tra `sizeof()` của từng struct khớp đúng số byte ghi trong v1.7.**

### 2.2 `plc_tag_def.h` — bảng 69 tag cụ thể (chưa viết)

`architecture.md` mục 2.1 đã cho khung phân bổ:
```
1-8=DI, 9-16=DO, 17-20=AI, 21-36=VFLAG, 37-52=VREG, 53-68=VREG_RETAIN,
69-127 dự trù Gateway
```
Cần viết thành `#define` cụ thể (theo đúng "Cách A" — không dùng `enum` tự
đánh số, xem `architecture.md` mục 7), ví dụ:
```c
#define TAG_DI0  1
#define TAG_DI1  2
...
#define TAG_DO0  9
...
```
File này cần xong TRƯỚC khi viết `plc_io.c` (Layer 3), vì `plc_io.c` cần
biết chính xác index nào ánh xạ ra chân GPIO/ADC nào.

### 2.3 CMake — tách tiếp các layer khác khi có code thật

Hiện tại chỉ `core/` (Layer 2) có `CMakeLists.txt` riêng. Khi các layer
sau có code thật (không còn rỗng), tách theo đúng mẫu `core/CMakeLists.txt`
đã làm:
- `utils/CMakeLists.txt` → library `splc_utils` (hiện `cqueue.c` đã có
  code thật, `logger.c` thì CHƯA — xem cảnh báo bên dưới)
- `services/CMakeLists.txt` → library `splc_services`, link `splc_core`
- `port/CMakeLists.txt` → library `splc_port`, link `libs/nanomodbus`
- `app/CMakeLists.txt` → library `splc_app`, link mọi thứ trên

**CẢNH BÁO quan trọng đã phát hiện, CHƯA XỬ LÝ:**
- `utils/logger/logger.c` **vẫn còn `#include <FreeRTOS.h>`/`<semphr.h>`**
  và gọi `xSemaphoreTake/Give/CreateMutex`. `architecture.md` mục "Layer U"
  đã ghi rõ: khi port từ `WS_v1` phải BỎ mutex FreeRTOS vì SimplePLC chạy
  bare-metal đơn luồng. **Việc này CHƯA được làm.** File này hiện bị loại
  khỏi `target_sources` trong `simple_plc/CMakeLists.txt` để tránh lỗi
  compile/link (không có FreeRTOS trong build hiện tại). Ai làm tiếp cần
  port lại `logger.c` bỏ mutex trước khi thêm nó vào build.
- `board/board.c` và `port/usb/usb_descriptors.c` **rỗng (0 dòng)** —
  cũng bị loại khỏi build vì lý do tương tự.
- `port/modbus_serial/modbus_serial.c` chỉ có 2 dòng `#include`, chưa có
  logic thật.
- `libs/nanomodbus/nanomodbus.c` (2462 dòng) có code thật NHƯNG CHƯA được
  add vào build ở đâu cả — chưa cần vì chưa ai gọi tới nó
  (`plc_modbus_cfg.c` còn rỗng).
- `libs/tinyusb/` hoàn toàn rỗng — TinyUSB chưa được vendor vào repo.

### 2.4 Layer 3 (`services/`) — 3 file rỗng (0 dòng), CHƯA VIẾT

Thứ tự khuyến nghị:
1. `plc_retain.c` — độc lập nhất, không phụ thuộc `plc_tag_def.h`, có thể
   làm trước hoặc song song.
2. `plc_io.c` — cần `plc_tag_def.h` (mục 2.2) xong trước.
   `input_scan()`/`output_scan()`.
3. `plc_modbus_cfg.c` — phức tạp nhất: state machine #2
   (`CFG_STATE_IDLE/RECEIVING/VERIFYING/READY/ERROR`), dùng nanoMODBUS,
   CRC-16/MODBUS trên `rule_count * 32` byte, staging + atomic commit vào
   `g_rule_table[]` qua `rule_table_commit()` (đã có sẵn ở Layer 2, xem
   mục 1.1). Nên làm SAU CÙNG trong Layer 3 vì phụ thuộc hiểu đúng toàn bộ
   register map ở `docs/SimplePLC_App_MCU_Structs_v1.7.md` mục 8.

### 2.5 Layer 4 (`app/plc_app/plc_engine.c`) — rỗng, CHƯA VIẾT

Chỉ nên làm SAU KHI ít nhất `plc_io.c` xong, vì scan loop
(`plc_engine_scan_once()`) mới có ý nghĩa thực tế khi có I/O thật. Khung
hàm đã có sẵn trong `architecture.md` mục "Layer 4":
```c
void plc_engine_init(void) {
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    plc_io_init();
    retain_store_restore();
    plc_modbus_cfg_init();
    pvd_init();
    watchdog_init();
}

void plc_engine_scan_once(void) {
    input_scan();
    rule_scan();
    output_scan();
    modbus_config_service();
    retain_service();
    watchdog_kick();
}
```
**Lưu ý:** `rule_scan()` hiện tại (Layer 2) dùng biến `static uint32_t
s_rule_scan_now_ms` nội bộ trong `plc_rule.c`, luôn = 0 — chưa có cách
nào Layer 4 truyền tick thật vào. Đây là 1 vấn đề thiết kế cần giải quyết
khi viết `plc_engine.c`: hoặc thêm tham số `now_ms` vào `rule_scan()`
(đổi chữ ký hàm, cần sửa `plc_rule.h`), hoặc thêm 1 setter function như
`rule_scan_set_tick(uint32_t now_ms)` gọi trước `rule_scan()` mỗi vòng
quét. Chưa quyết định — cần bàn với người dùng trước khi tự chọn hướng.

## 3. Các quyết định kiến trúc CHƯA CHỐT (đừng tự ý quyết định, hỏi lại)

1. **Đơn vị `scan_time_ms` vs `scan_time_us`** trong `SPLC_DeviceHealth`
   — xem mục 2.1.
2. **Vị trí Flash lưu Active Rule Table** — địa chỉ, kích thước tối thiểu
   `100*32=3200` byte, có cần wear-leveling như `plc_retain.c` hay ghi đè
   1 chỗ cố định. Chưa quyết — xem `architecture.md` mục 10.
3. **Cách Layer 4 truyền tick ms vào `rule_scan()`** — xem mục 2.5.
4. **Nguồn RTC cho `SPLC_TRG_TIME_WINDOW`** — chưa có, hiện `plc_rule.c`
   hardcode `now_hhmm = 0`, khiến trigger loại này KHÔNG hoạt động đúng.
   Đã ghi rõ bằng TODO trong code.
5. **Modbus Master cho Gateway** (`plc_modbus_master.c`) — RTU thôi hay
   cả TCP? Chưa quyết, chưa cần làm ngay (chỉ ảnh hưởng SKU Gateway,
   không ảnh hưởng Remote I/O đang làm).

## 4. Lệnh verify nhanh (chạy lại bất cứ lúc nào để kiểm tra Layer 2 + CMake còn sạch)

```bash
# Build test PC-only cho riêng Layer 2, dùng CMake thật (không cần STM32 toolchain):
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
#include "plc_rule.h"
int main(void) {
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    assert(sizeof(SPLC_RuleRecord) == 32);
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
  dùng đã tự push, vì họ có thể tự sửa tay hoặc dùng Claude khác giữa các
  phiên.
- Người dùng đã đồng ý hướng "mỗi layer 1 CMakeLists riêng" (không gộp 1
  file chung) — xem mục 2.3 để tiếp tục đúng hướng này.
- Mọi thay đổi code nên được verify bằng compile/chạy thật (gcc hoặc
  CMake thật trong sandbox), không chỉ đọc code bằng mắt — đây là thói
  quen đã thiết lập xuyên suốt các phiên trước và người dùng có vẻ đánh
  giá cao việc này.