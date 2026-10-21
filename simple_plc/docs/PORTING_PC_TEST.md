# SimplePLC — Hướng dẫn nhúng `splc` vào project khác để chạy / test logic (không cần phần cứng)

> Đặt file này ở `simple_plc/docs/PORTING_PC_TEST.md`. Đọc sau `Readme.md` và `architecture.md`.
>
> **Mức kiểm chứng (nhánh `board_dev_gw`, HEAD `1c81b84`, gcc trên Linux x86-64):**
> - **Đã build + chạy thật:** mục 3–6 (Layer 2: Tag Table + Rule Engine, gồm rule dwell, float32, Time Window), cả bằng `gcc` lẫn CMake, với ASan + UBSan: ALL PASS.
> - **Đã thử biên dịch (chưa chạy):** `utils/cqueue`, `utils/epoch`, `utils/mqtt_fmt`, `services/plc_tagtype`.
> - **Chưa thử:** mọi thứ trong mục 8 ngoài `plc_tagtype`. Với option mặc định (`SPLC_MQTT_ENABLE=0`, `SPLC_MBM_ENABLE=0`) `plc_mqtt.c` và `plc_mbm.c` biên dịch ra file rỗng, nên cần bật option mới biết có cần thêm stub không.
>
> Test trên PC chỉ chứng minh **logic**. Nó không chứng minh Flash thật, timing UART/RS485, USB, PVD, FreeRTOS, hay kết quả float trên FPU Cortex-M33 (xem mục 9).

---

## 1. Vì sao làm được

`architecture.md` (quy tắc 3) định **Layer 2 (`core/`) là ranh giới build/port**: Tag Table và Rule Engine không `#include` gì từ Layer 0/1/3.5. Layer 2 không có đồng hồ, không có Flash, không có phần cứng. Mọi thứ nó cần từ bên ngoài chỉ gồm bốn điểm:

| Cần cấp | Từ đâu | Ghi chú |
|---|---|---|
| `splcopts.h` | Bạn viết | Chọn `SPLC_BOARD`, từ đó có `MAX_TAGS` |
| `log_func()` | Bạn viết (test double) hoặc `utils/logger` | `plc_rule.c` gọi `log_debug/info/warn` |
| `now_ms`, `now_hhmm` | Tham số của `rule_scan()` | Layer 2 không đọc đồng hồ |
| `SPLC_TagLayout` | Bạn truyền vào `tag_table_load_from_flash()` | Dù tên có chữ "flash", hàm này chỉ dựng bảng từ `layout` (xem comment TODO trong `plc_tag.c`) |

Mô hình **kéo (pull)**: `tag_write()` chỉ ghi RAM. Muốn thấy tác dụng của rule thì phải gọi `rule_scan()`, rồi đọc lại bằng `tag_read()`.

---

## 2. File cần lấy

Chỉ 4 file `.c` của Layer 2:

```
simple_plc/core/plc_tag/plc_tag.c
simple_plc/core/plc_rule/plc_rule.c
simple_plc/core/plc_internal_rule/plc_rule_eval.c
simple_plc/core/plc_internal_rule/plc_rule_action.c
```

Include path cần có (đặt `<S>` = `simple_plc/`):

```
<thư mục chứa splcopts.h của bạn>
<S>/config                     # splc_opt.h, splc_log.h
<S>/board                      # board_tag_define.h, board_config.h  (CHỈ header)
<S>/utils/logger               # logger.h (chỉ cần khai báo)
<S>/core/plc_tag  <S>/core/plc_rule  <S>/core/plc_internal_rule
<S>/core/plc_device  <S>/core/plc_error  <S>/core/plc_system_cmd  <S>/core/plc_diag
```

Link thêm `-lm` (rule float dùng hàm toán học).

**Không** lấy `board/splc_board.c`, `board/board_device/*.c`, `services/`, `app/`, `platforms/`, `components/`, `port/`: chúng kéo theo HAL, TinyUSB, FreeRTOS.

> `board/` chỉ vào include path để `plc_tag.h` đọc `MAX_TAGS` từ `board_tag_define.h`. Không link `splc_board`.

---

## 3. Bước 1 — viết `splcopts.h`

Đây là file cấu hình kiểu lwIP của bạn. Tối thiểu:

```c
#ifndef SPLCOPTS_H
#define SPLCOPTS_H

/* Phải hợp lệ (splc_opt.h kiểm bằng #error) dù Layer 2 không dùng platform. */
#define SPLC_PLATFORM      SPLC_PLATFORM_STM32H5

/* Chọn bảng tag. MAX_TAGS lấy từ đây:
 *   ZIGBEE_IO_4DI_4DO -> 112 tag, REMOTE_IO_8DI_8DO / GATEWAY -> 128 tag */
#define SPLC_BOARD         SPLC_BOARD_ZIGBEE_IO_4DI_4DO

#define SPLC_FLASH_SIZE_KB 256
#define SPLC_LOG_LEVEL     4

/* Tuỳ chọn: tắt rule float32 (mặc định trong code là 1, xem lưu ý dưới) */
/* #define SPLC_REAL_ENABLE 0 */

#endif
```

Lưu ý:
- `splc_opt.h` đặt `SPLC_REAL_ENABLE` mặc định **1** (handoff từng ghi 0, code mới đúng). Nó đổi `sizeof` bố cục `reserved` trong `SPLC_RuleRecord` nhưng vẫn 32 byte.
- `MAX_RULES` mặc định 100; có thể đặt nhỏ hơn, từ 1 đến 100.
- Không cần `SX_OS_USE_FREERTOS`, `SPLC_PORT_USB_CDC`, v.v.: các option đó thuộc layer khác.

---

## 4. Bước 2 — cấp `log_func()`

`splc_log.h` mặc định chuyển `log_*` sang `log_func()` của `logger.h`. Với test PC, bạn chỉ cần tự định nghĩa nó, không cần build `utils/logger/logger.c`:

```c
#include <stdarg.h>
#include <stdio.h>
#include "logger.h"

void log_func(LOGGING_LEVELS lv, const char *tag, const char *f, ...) {
    va_list a; va_start(a, f);
    printf("[%d][%s] ", lv, tag); vprintf(f, a); printf("\n");
    va_end(a);
}
```

Cách khác, không cần `logger.h`: định nghĩa `SPLC_LOG_OUTPUT` trong `splcopts.h` (định nghĩa rỗng thì tắt hẳn log; header mà macro cần phải được include từ chính `splcopts.h`):

```c
#define SPLC_LOG_OUTPUT(level, tag, ...)  my_log((level), (tag), __VA_ARGS__)
```

Khi nhúng vào firmware/thư viện có logger riêng, dùng `SPLC_LOGGER_BUILTIN=0` và cung cấp `logger.h` của bạn.

---

## 5. Bước 3 — build

### 5a. Một dòng gcc (đã chạy)

```bash
S=/đường/dẫn/simple_plc/simple_plc
gcc -std=c11 -Wall -Wextra -fsanitize=address,undefined -g \
  -I. -I$S/config -I$S/board -I$S/utils/logger \
  -I$S/core/plc_tag -I$S/core/plc_rule -I$S/core/plc_internal_rule \
  -I$S/core/plc_device -I$S/core/plc_error -I$S/core/plc_system_cmd -I$S/core/plc_diag \
  test_core.c \
  $S/core/plc_tag/plc_tag.c $S/core/plc_rule/plc_rule.c \
  $S/core/plc_internal_rule/plc_rule_eval.c $S/core/plc_internal_rule/plc_rule_action.c \
  -lm -o test_core && ./test_core
```

### 5b. CMake cho project chủ (đã chạy với CMake qua pip)

Liệt kê nguồn thẳng thay vì `add_subdirectory` toàn repo (gốc repo kéo cả toolchain ARM và HAL):

```cmake
cmake_minimum_required(VERSION 3.16)
project(my_host C)
set(CMAKE_C_STANDARD 11)
set(SPLC_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/third_party/simple_plc/simple_plc")

add_library(splc_core STATIC
  ${SPLC_ROOT}/core/plc_tag/plc_tag.c
  ${SPLC_ROOT}/core/plc_rule/plc_rule.c
  ${SPLC_ROOT}/core/plc_internal_rule/plc_rule_eval.c
  ${SPLC_ROOT}/core/plc_internal_rule/plc_rule_action.c)

target_include_directories(splc_core PUBLIC
  ${CMAKE_CURRENT_SOURCE_DIR}                 # chứa splcopts.h
  ${SPLC_ROOT}/config ${SPLC_ROOT}/board ${SPLC_ROOT}/utils/logger
  ${SPLC_ROOT}/core/plc_tag ${SPLC_ROOT}/core/plc_rule ${SPLC_ROOT}/core/plc_internal_rule
  ${SPLC_ROOT}/core/plc_device ${SPLC_ROOT}/core/plc_error
  ${SPLC_ROOT}/core/plc_system_cmd ${SPLC_ROOT}/core/plc_diag)
target_link_libraries(splc_core PUBLIC m)

add_executable(my_test my_test.c)            # my_test.c có log_func() (mục 4)
target_link_libraries(my_test splc_core)
```

Thêm `-fsanitize=address,undefined` cho cả `splc_core` lẫn test nếu muốn bắt UB (nên bật).

---

## 6. Bước 4 — dùng API (trình tự đúng)

```c
#include "plc_tag.h"
#include "plc_rule.h"

/* 1. Dựng bảng tag theo layout của bạn (tổng đếm <= MAX_TAGS). */
SPLC_TagLayout lay = { .di_count=4, .do_count=4, .vflag_count=32, .vreg_count=32,
                       .vreg_retain_count=32, .counter_count=8 };
tag_table_load_from_flash(&lay);   /* xoá sạch g_tag_value về 0 */
rule_table_load_from_flash();      /* chỉ đặt bảng rule rỗng, KHÔNG đọc Flash */

/* 2. Lấy chỉ số tag. Đừng hard-code: index phụ thuộc layout. */
uint16_t di0 = tag_di_base_index(), do0 = tag_do_base_index(),
         vr0 = tag_vreg_base_index();

/* 3. Nạp rule. Mảng SPLC_RuleRecord (32 B/rule), đóng gói liên tục. */
SPLC_RuleRecord r = {0};
r.guard_tag    = GUARD_TAG_NONE;            /* 0x7FFF, KHÔNG phải 0 (0 là DI0 thật) */
r.enabled      = 1;
r.trigger_tag  = di0;  r.trigger_type = SPLC_TRG_ON_RISE;
r.action_type  = SPLC_ACT_SET_TAG;  r.action_tag = do0;  r.action_param = 1;
rule_table_commit((const uint8_t *)&r, 1);   /* true nếu thành công; commit reset runtime */

/* 4. Mỗi chu kỳ scan: bạn cấp thời gian. */
tag_write(di0, 1);                   /* mô phỏng đầu vào */
rule_scan(now_ms, RULE_HHMM_INVALID);/* now_ms: tick ms, chỉ hiệu số được dùng */
int32_t v = tag_read(do0);           /* kiểm kết quả */
```

Điểm cần nhớ khi test:
- **`now_ms`** do bạn quyết định, nên có thể giả lập mà không cần đợi thật (tăng 10 mỗi scan là xong). Chỉ hiệu số unsigned được dùng, nên dãy bắt đầu gần `0xFFFFFF00` vẫn đúng khi tràn (ví dụ dưới có kiểm).
- **`now_hhmm`** là giờ địa phương dạng HHMM (`830` = 08:30). Truyền `RULE_HHMM_INVALID` khi không có giờ: rule `TIME_WINDOW` sẽ không bắn.
- Độ phân giải thời gian bằng chu kỳ scan: dwell hoàn tất ở scan đầu tiên sau mốc, không bao giờ sớm.
- Tag DI/AI do Layer 3 (`input_scan`) ghi; trong test của bạn, `tag_write()` đóng vai đó.
- `rule_table_commit()` nhận `SPLC_RuleRecord` in-RAM (memcpy). Nếu muốn thử đường wire (CRC, staging, byte `flags` ở thanh ghi 13) thì phải đi qua `plc_modbus_cfg` (Layer 3), xem mục 8.

### Ba ví dụ đã chạy ALL PASS

**a) Dwell** (giữ DI0 ≥ 500 ms thì bật DO0; chạy xuyên chỗ tràn `uint32`):

```c
r.trigger_tag = di0; r.trigger_type = SPLC_TRG_ON_RISE; r.for_ms = 500;
r.action_type = SPLC_ACT_SET_TAG; r.action_tag = do0; r.action_param = 1;
rule_table_commit((uint8_t *)&r, 1);
uint32_t t = 0xFFFFFF00u;
tag_write(di0, 1);
for (int i = 0; i < 30; i++) { rule_scan(t, RULE_HHMM_INVALID); t += 10; }
/* 300 ms: tag_read(do0) == 0 */
for (int i = 0; i < 30; i++) { rule_scan(t, RULE_HHMM_INVALID); t += 10; }
/* 600 ms: tag_read(do0) == 1 */
```

**b) Rule float32** (`SPLC_REAL_ENABLE=1`; ngưỡng, `action_param` và giá trị tag trigger là bit float32 nằm trong ô `int32`):

```c
#include "plc_rule_eval.h"          /* splc_f32_to_bits() / splc_bits_to_f32() */
r.flags       = SPLC_RULE_FLAG_FLOAT_DATA;
r.trigger_tag = vr0; r.trigger_type = SPLC_TRG_ON_CHANGE; r.compare_op = SPLC_OP_GT;
r.threshold_lo = (int32_t)splc_f32_to_bits(80.0f);
r.action_type = SPLC_ACT_SET_TAG; r.action_tag = do0; r.action_param = 1;
rule_table_commit((uint8_t *)&r, 1);
tag_write(vr0, (int32_t)splc_f32_to_bits(79.5f));  /* scan: do0 == 0 */
tag_write(vr0, (int32_t)splc_f32_to_bits(80.5f));  /* scan: do0 == 1 */
```

`plc_rule_eval.h` là header nội bộ của `core/` (CMake gốc để PRIVATE). Chỉ include nó trong test; code sản phẩm không nên.

**c) Time Window qua nửa đêm** (18:00–06:00, bắn mỗi scan khi điều kiện đúng):

```c
r.trigger_type = SPLC_TRG_TIME_WINDOW; r.compare_op = SPLC_OP_NONE;
r.threshold_lo = 1800; r.threshold_hi = 600;        /* Lo > Hi = qua nửa đêm */
r.action_type = SPLC_ACT_SET_TAG; r.action_tag = do0; r.action_param = 1;
rule_table_commit((uint8_t *)&r, 1);
rule_scan(0,  1200);                 /* 12:00 -> do0 == 0 */
rule_scan(10, RULE_HHMM_INVALID);    /* chưa có giờ -> do0 == 0 */
rule_scan(20, 2330);                 /* 23:30 -> do0 == 1 */
```

### Mẹo viết test
- Trước mỗi ca, gọi lại `tag_table_load_from_flash()` + `rule_table_load_from_flash()` để có trạng thái đầu xác định (bài học 3, 37 của handoff).
- So sánh bit float bằng `splc_f32_to_bits()`, không so bằng `==` trên float; với `SCALE_TAG`, App tính hai bước (tích rồi cộng), engine chặn FMA để trùng bit (quyết định 45, bài học 41).
- Test xanh chưa chứng minh gì: **phá code có chủ đích** (đột biến: đổi `BETWEEN`, bỏ kiểm guard, ε=0, đổi `<` thành `<=`) rồi xem test có đỏ không (bài học 13, 37).
- Dùng lại đúng bố cục wire và CRC độc lập khi cần so với App (bài học 40: cắt nguyên văn hàm từ file nguồn vào harness thay vì chép lại).

---

## 7. Nhúng vào một thư viện/project khác

Có hai cách, tuỳ bạn muốn dừng ở đâu:

1. **Chỉ Layer 2 (khuyến nghị cho test logic và mô phỏng):** như mục 2–6. Project chủ nhận `libsplc_core.a`, tự gọi `tag_*` và `rule_scan()` theo đồng hồ của nó. Không còn việc nào khác.
2. **Thêm services để chạy cả cấu hình qua Modbus (mục 8):** cần thêm stub cho Layer 0/1 (`sx_*`). Chỉ làm khi cần thử đường staging/CRC/COMMIT.

Để dùng như thư viện trong firmware thật (không phải PC), xem `architecture.md` mục 3.1 và handoff quyết định 13–25 (`SPLC_AS_LIBRARY=ON`, `splc_link_register`, `SPLC_OPTS_DIR`, v.v.). Hướng dẫn này không bao quát phần đó.

Quy ước tên khi nhúng vào host: mọi file/symbol mới dễ trùng host phải mang tiền tố `splc_` (quyết định 21).

---

## 8. Mở rộng: những thứ khác có thể test trên PC (CHƯA kiểm, chỉ là đánh giá)

| Module | Quan sát khi thử biên dịch | Cần cấp thêm |
|---|---|---|
| `utils/cqueue`, `utils/epoch`, `utils/mqtt_fmt` | Biên dịch sạch, không symbol ngoài libc | Chỉ include path của chính nó |
| `services/plc_tagtype` | Biên dịch được | `tag_get_kind`, `tag_write` (có trong Layer 2) và `log_func` |
| `services/plc_fb` | Cần `tag_get_kind`, `tag_read` (có trong Layer 2) | Chưa xét kỹ |
| `services/plc_mqtt`, `plc_mbm` | Với option mặc định ra file rỗng (bị gate `SPLC_MQTT_ENABLE` / `SPLC_MBM_ENABLE`) | Phải bật option rồi mới biết cần stub nào; handoff nói đã kiểm bằng Flash/đồng hồ/callback/slave giả, harness không còn trong repo |
| `services/plc_modbus_cfg` | Gọi nanoMODBUS riêng (`splc_nmbs_*`), `plc_rtc_*`, `plc_rule_flash_save`, `retain_snapshot_write`, `sx_get_tick_ms` | Nhiều stub; đây là đường thử staging → CRC → COMMIT |
| `services/plc_rule_flash` | Kéo `sx_platform_config.h`, `sx_flash_*` | Flash giả (nhớ: sau "mất điện" mọi erase/program phải bị bỏ qua, bài học 6) |

Handoff mục "Kiểm trên PC" liệt kê những gì phiên trước đã làm theo kiểu này (Layer 2 + `sizeof(SPLC_RuleRecord)==32`, `modbus_link` với 2 thread + TSan, `plc_io_remote` + `modbus_serial` với slave giả, `splc_mqtt_fmt`, `plc_mqtt`). Các harness đó **không nằm trong repo**; muốn giữ thì đặt vào `test/` kèm target CMake PC.

Kiểm ranh giới layer (không cần build đầy đủ): `nm -u core_object.o` của Layer 2 không được có `sx_flash_*`/`sx_usb_*`/`tud_*`.

---

## 9. Test PC không thay được gì

| Cần board thật | Lý do |
|---|---|
| Flash A/B, mất điện giữa lúc ghi, PVD | Flash giả chỉ mô phỏng logic |
| UART/RS485 (timing DE, echo, break), USB-CDC | Phụ thuộc phần cứng và driver |
| FreeRTOS thật (trễ task lúc erase Flash) | Scheduler thật |
| Số bit float trên Cortex-M33 (FPv5) | GCC gộp `a*b+c` thành FMA trên M33 theo mặc định, x86 mặc định thì không: kiểm chứng trên PC có thể lệch bit cuối so với board (bài học 41) |
| Kích thước RAM/FLASH khi nhúng vào `wg_ue` | Phải đọc map thật (bài học 21) |
| Hành vi của App thật | Dùng golden vectors `GOLDEN_VECTORS_V2_0.md` của repo `paa` cho đường wire |

Khi báo kết quả test PC, nêu rõ "chạy trên mô hình giả" và liệt kê điều chưa kiểm trên board, đúng quy trình trong `handoff.md` mục 3.

---

## 10. Xử lý sự cố nhanh

| Triệu chứng | Nguyên nhân thường gặp |
|---|---|
| `fatal error: splcopts.h: No such file` | Thư mục chứa `splcopts.h` chưa vào include path |
| `#error "SPLC_BOARD must be one of..."` | `SPLC_BOARD` chưa dùng hằng `SPLC_BOARD_*` của `splc_opt.h` |
| `board_tag_define.h`: `MAX_TAGS` không định nghĩa | Chọn `SPLC_BOARD` không có nhánh định nghĩa `MAX_TAGS`, hoặc `board/` thiếu khỏi include path |
| `undefined reference to log_func` | Chưa cấp `log_func()` (mục 4) hoặc `SPLC_LOG_OUTPUT` |
| `undefined reference to ... fabsf/roundf` | Quên `-lm` |
| Rule không bao giờ bắn | `guard_tag` để 0 (phải `GUARD_TAG_NONE`); chưa gọi `rule_scan()`; `enabled=0`; sai index tag (index sai chỉ vô hiệu rule âm thầm, handoff 4.D); `rule_count` truyền sai |
| Rule float so sai | `flags` chưa bật, hoặc ngưỡng/giá trị tag không phải bit float (dùng `splc_f32_to_bits`) |
| `rule_table_commit` trả `false` | `rule_count` vượt `MAX_RULES` |
| Time Window không bắn | `now_hhmm` còn là `RULE_HHMM_INVALID` |