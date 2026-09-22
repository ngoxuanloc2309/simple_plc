# SimplePLC — Handoff

> Mục đích: cho phiên làm việc tiếp theo (Claude khác hoặc chính bạn) nắm
> "đang ở đâu, làm gì tiếp" mà không cần đọc lại lịch sử chat.
> Đọc SAU `Readme.md` và `docs/architecture.md` (nguồn kiến trúc chính).
> File này chỉ ghi: trạng thái hiện tại, việc đang làm, bài học quan trọng.
>
> **Quy tắc vàng:** trước khi tin bất cứ điều gì dưới đây, chạy
> `git pull && git log --oneline -10`. Nếu có commit mới hơn, ưu tiên code
> thật hơn file này.

## 0. Trạng thái hiện tại

- Branch `main &ruleflash, hiện dùng ruleflash`, commit đã verify: `69d3e86` ("add code test compare
  counter").
- Board: **Zigbee-IO SKU** (`board/board_zigbee_io.c`), 4 DI / 4 DO / 0 AI,
  STM32H523CCU6.
- **Rule Engine chạy đúng trên board thật, đã kiểm chứng kỹ:**
  - Đường Modbus (stage → CRC → commit → READY), `ACTIVE_RULE_TABLE` đọc
    lại khớp bản đã stage, DI/DO đăng ký đúng (8/8), rule đơn giản
    (DI0 rise → DO0 set) chạy đúng trên board.
  - Tick truyền qua tham số (`rule_scan(uint32_t now_ms)`), nhịp quét
    10 ms (`PLC_SCAN_INTERVAL_MS`) — đã build ARM, flash, chạy thật.
  - Bộ test tự động `test_rule.py` (không cần đấu dây, dùng rule
    `TRG_INTERVAL` làm nguồn giả lập): **59/59 PASS trên board thật**
    (upload/reject, interval, so sánh đủ 8 toán tử, guard + NEGATE +
    regression DI0, actions SET/TOGGLE/INC/SCALE, chain 3 tầng, edge
    ON_CHANGE/RISE/FALL, dwell, reload, stress 100 rule).
  - App thật (không phải `test_plc.py`) đã kết nối + nạp rule + xem mô
    phỏng DI/DO real-time thành công.
- **Rule KHÔNG được lưu vào Flash — ĐANG LÀM, xem mục 1.** Chỉ nằm
  trong RAM, mất điện/reset là mất. Đây là chủ ý ban đầu ("test xong
  thuật toán rồi mới lưu Flash, để đỡ hao mòn Flash lúc còn debug") —
  giờ thuật toán đã test xong, đang chuyển sang làm phần lưu Flash.
- Còn 1 việc treo từ trước, **không liên quan Flash**, chưa quay lại:
  test tay `--manual` của `test_rule.py` (`m_basic`, `m_dwell`, `m_guard`,
  `m_wiring`) và `test_rule_manual_simple.py` — người dùng nói đã chạy
  nhưng log MCU bị trôi, chưa có bằng chứng bằng số. Không chặn việc làm
  Flash, nhưng nên xin lại log khi rảnh.

## 1. ĐANG LÀM: Lưu Rule Table vào Flash (2-sector A/B)

### 1.1 Quyết định đã chốt với người dùng (đừng hỏi lại, đừng tự đổi)

1. **2 sector, không phải 1.** Sector A và B, mỗi sector chứa trọn 1 bản
   Rule Table. Lấy 1 sector từ vùng Retain (hiện 4 sector #27-30) làm
   sector B của Rule Table — Retain còn lại 3 sector.
2. **Cơ chế A/B** (người dùng tự thiết kế, đã diễn giải lại và người dùng
   xác nhận đúng — xem 1.2).
3. **Ghi Flash ngay khi commit qua Modbus thành công**, không tách lệnh
   riêng qua `SYSTEM_COMMAND`. Lý do người dùng nêu: sản phẩm thật nạp
   rule 1 lần lúc lắp đặt rồi chạy lâu dài, không phải kiểu test lặp lại
   nhiều lần — ghi ngay là hợp lý và khớp đúng spec gốc
   (`docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` mục 5.2, bước 4: "khớp
   → atomic-swap + lưu Flash + tăng version", tức atomic-swap RAM và lưu
   Flash LÀ CÙNG MỘT BƯỚC theo thiết kế gốc, không phải 2 giai đoạn).
4. **Gộp phản hồi làm một.** App chỉ đọc `CONFIG_STATUS` MỘT LẦN sau khi
   cả RAM và Flash đã xong xuôi hoàn toàn (không có trạng thái trung
   gian "RAM xong, Flash đang chờ" mà App nhìn thấy được). Vì
   `write_commit_command()` chạy đồng bộ trong 1 lần gọi, việc này tự
   nhiên đúng miễn là bước ghi Flash nằm TRONG hàm đó trước khi hàm trả
   về / set `CONFIG_STATUS`.
5. **Nếu ghi Flash lỗi nhưng RAM đã đổi đúng:** `CONFIG_STATUS = READY`
   (rule ĐANG chạy thật, App dùng được ngay) + `CONFIG_ERROR_CODE =
   SPLC_ERROR_FLASH` (mã có sẵn, xem 1.4) để App biết "chạy được nhưng
   chưa lưu vĩnh viễn, mất khi cúp điện". Không dùng `CONFIG_STATUS =
   ERROR` cho ca này — sẽ khiến App hiểu nhầm là rule không chạy.
6. **Chấp nhận PLC "đứng hình" vài trăm ms mỗi lần nạp rule mới**
   (ghi 2 sector, không có RTOS/ngắt tách biệt) — người dùng đã xác nhận
   không phải vấn đề.
7. **Không cần vùng nhớ trung gian thứ ba.** Đã lần từng bước, tại mọi
   thời điểm — kể cả mất điện giữa bất kỳ bước ghi nào — luôn có ít nhất
   1 trong 2 sector giữ bản hợp lệ để phục hồi.

### 1.2 Cơ chế A/B (nguyên văn ý người dùng, đã xác nhận đúng)

Trạng thái ổn định: A và B giữ cùng 1 bản rule, đều hợp lệ. A là bản
"đang chạy" (được đọc lúc boot), B là bản sao lưu.

Khi commit rule mới:
1. Copy A (bản cũ) → B — đảm bảo B luôn là bản sao lưu MỚI NHẤT của
   cấu hình đang chạy, phòng bước tiếp theo lỗi giữa chừng.
2. Clear A, ghi rule mới (vừa commit vào RAM) vào A.
3. Đọc lại A, kiểm CRC:
   - **Đúng** → đồng bộ lại B = A (clear B, copy A → B), để B luôn là
     bản sao lưu mới nhất, sẵn sàng cho lần nạp kế tiếp.
   - **Sai** (mất điện giữa chừng, lỗi ghi...) → báo lỗi
     (`SPLC_ERROR_FLASH`) + copy B → A để phục hồi A về trạng thái chạy
     được (B không hề bị đụng ở nhánh này, luôn còn nguyên).

Bảng lần bước (đã kiểm bằng tay, không cần vùng nhớ thứ ba):

| Bước đang thực hiện | Nếu mất điện NGAY LÚC NÀY | Phục hồi |
|---|---|---|
| Copy A→B | A còn nguyên (chưa đụng) | Dùng A, không cần làm gì |
| Clear A | A rỗng, B còn bản cũ hợp lệ | Copy B→A |
| Ghi rule mới vào A | A dở dang, B còn bản cũ hợp lệ | Copy B→A |
| Đồng bộ B=A (nhánh CRC đúng) | A đã có rule mới hợp lệ (đủ dùng dù B dở dang) | Không bắt buộc phục hồi ngay; lần nạp sau tự đồng bộ lại |
| Copy B→A (nhánh CRC sai, phục hồi) | A dở dang lần nữa, B vẫn nguyên (chỉ đọc B, ghi A) | Boot lại vẫn phát hiện A lỗi, retry copy B→A — không vòng lặp vô hạn nguy hiểm vì B bất biến |

### 1.3 Kế hoạch code — 1 file mới, 6 file sửa

**File mới:**
```
services/plc_rule_flash/plc_rule_flash.c
services/plc_rule_flash/plc_rule_flash.h
```
Layer 3, ngang hàng `plc_retain/`. Không gộp vào `plc_retain.c` (định
dạng/mục đích khác hẳn, `plc_retain.h` ghi rõ tách biệt — cần sửa câu đó,
xem bên dưới). Không gộp vào `plc_modbus_cfg.c` (file đã lớn, đây là 1
khối logic độc lập có state machine riêng).

**Sửa:**

| File | Việc |
|---|---|
| `app/splc_flash_define.h` | 2 sector cho Rule Table (A = sector #31 giữ nguyên, B = lấy 1 sector từ vùng Retain, ví dụ #30). `SPLC_FLASH_RETAIN_SECTOR_COUNT` 4U to 3U. |
| `core/plc_rule/plc_rule.c` | `rule_table_load_from_flash()`: bỏ nội dung "memset về 0", đúng như TODO đã ghi sẵn — Layer 3 (`plc_rule_flash_load()`) đọc Flash rồi tự gọi `rule_table_commit()`. |
| `app/plc_app/plc_engine.c` | `plc_engine_init()`: đổi bước gọi thành `plc_rule_flash_load()` (đọc A/B, tự phục hồi nếu cần, gọi `rule_table_commit()` bên trong). |
| `services/plc_modbus_cfg/plc_modbus_cfg.c` | `write_commit_command()`: sau khi `rule_table_commit()` (RAM) thành công, gọi `plc_rule_flash_save()` TRƯỚC KHI set `CONFIG_STATUS`/tăng version/log. Bỏ `static` ở 3 hàm `crc16_modbus_update()`, `rule_record_to_wire()`, `rule_table_wire_crc16()` — khai báo trong `plc_modbus_cfg.h` để `plc_rule_flash.c` include và dùng lại (người dùng chốt: giữ nguyên vị trí, không tách file CRC riêng). |
| `services/CMakeLists.txt` | Thêm `plc_rule_flash/plc_rule_flash.c` vào `SPLC_SERVICES_SRC` + include path, giống hệt cách `plc_retain.c` đã đăng ký. |
| `services/plc_retain/plc_retain.h` | Sửa câu comment sai: hiện ghi "Rule Table has its own separate Flash region ... with no wear-leveling" theo thiết kế 1-sector cũ — cần cập nhật cho khớp 2-sector A/B mới. |

**Không đổi:** chữ ký `rule_table_commit()`/`rule_table_load_from_flash()`
trong `plc_rule.h`, thứ tự 7 layer, giao thức Modbus với App (App vẫn
stage/commit y hệt cũ).

### 1.4 Mã lỗi: dùng cái có sẵn, ĐỪNG tạo mới

`core/plc_error/plc_error.h` đã có `SPLC_ERROR_FLASH = 6` sẵn từ trước,
đúng ý nghĩa "lỗi đọc/ghi Flash" cần dùng ở bước 1.1 câu 5. Comment đầu
file đã ghi rõ nguyên tắc "extend `SPLC_ErrorCode`, đừng tạo enum lỗi
mới". **Đã có bài học thật:** ở phiên trước, Claude từng đề xuất tạo
`SPLC_ERROR_FLASH_WRITE_FAILED` mới trước khi kiểm tra file này — người
dùng hỏi "hiện có những error nào" mới lộ ra đã có sẵn. Luôn đọc
`plc_error.h` trước khi định thêm mã lỗi.

### 1.5 Việc còn cần làm rõ trước/trong khi code (tự kiểm tra, hỏi nếu vướng)

- Định dạng header lưu trên mỗi sector: theo đúng quy ước `plc_retain.c`
  đã dùng (CRC-16/MODBUS field-embedded, tính với field CRC tạm zero,
  big-endian qua `write_u16_be`/`write_u32_be`) hay dùng thẳng
  `rule_table_wire_crc16()` đã có cho Rule Table (khuyến nghị dùng cái
  sau, vì đó là CRC đã chốt cho đúng cấu trúc `SPLC_RuleRecord` này —
  đừng tạo công thức CRC thứ ba).
- Cần `rule_count` trong header để biết đọc bao nhiêu byte (rule_count
  x 32, không cố định 3200 byte).
- `sx_flash_write()` chỉ ghi được theo bội số 16 byte (STM32H5 quad-word)
  — kiểm tra kích thước header + `rule_count x 32` có cần pad không.

## 2. Kiến trúc trong 30 giây

7 layer, include một chiều từ trên xuống, không heap. Chi tiết:
`docs/architecture.md`.

```
Layer 4    app/, board/        plc_engine, board_<sku>.c (pin wiring)
Layer 3    services/           plc_io, plc_retain, plc_modbus_cfg, plc_rule_flash (đang thêm)
Layer 3.5  port/               modbus_transport_t, modbus_usb
Layer 2    core/               tag table + rule engine (build được trên PC)
Layer 1    components/         driver contract (gpio/adc/flash/uart/usb_cdc)
Layer 0    platforms/stm32/stm32h5/   HAL thật
Layer U    utils/, libs/       cqueue, logger, filter, nanoMODBUS, TinyUSB
```

Vòng quét: `plc_engine_poll()` (không chặn, tự canh nhịp
`PLC_SCAN_INTERVAL_MS = 10ms`) gọi `scan_cycle(now)`:
`input_scan → rule_scan(now) → output_scan → modbus_config_service
→ retain_service`.

Điểm cần nhớ khi đọc code:
- Layout tag v1.9: **không có sentinel ở index 0**, `TAG_DI0 == 0`.
  "Không có guard" = `GUARD_TAG_NONE` (`0x7FFF`), KHÔNG phải 0.
- `SPLC_RuleRecord` = 32 byte. CRC của Rule Table (Modbus lẫn Flash)
  PHẢI tính trên byte **wire** (`rule_table_wire_crc16()`), KHÔNG phải
  byte RAM của struct — xem mục 4.2, đây là bug đã tốn 3 vòng debug.
- `plc_device.h`, `plc_error.h`, `plc_system_cmd.h` cố ý chỉ có `.h`
  (không thêm `.c` vào Layer 2).
- Flash map hiện tại (trước khi sửa theo mục 1): Rule Table = sector #31
  (`0x0803E000`), Retain = sector #27-30 (32KB, xoay vòng). Sau khi sửa:
  Rule Table A = #31, B = #30, Retain = #27-29.
- `plc_device.h`'s `SPLC_RemoteIoVariant` có `SPLC_REMOTE_IO_VARIANT_
  4DI_4DO = 3` (board Zigbee-IO thật) — chỉ ảnh hưởng hiển thị/identity
  phía App, không ảnh hưởng resource catalog (App luôn đọc từ
  `DEVICE_RESOURCE_INFO`, không dựa vào field này).

## 3. Bug đã biết, CHƯA sửa (không liên quan việc đang làm ở mục 1)

### 3.1 `modbus_usb_write()` bỏ qua `timeout_ms`

`sx_usb_tiny_write()` luôn block tới khi ghi xong. Nếu USB nghẽn có thể
vượt ngân sách 10 ms của vòng quét. `test_rule.py`'s `t_load` (100 rule,
stress 5s) từng đo `scan_time_ms` lên tới 83ms dưới tải nặng — nghi do
đây. Cần quyết định: làm write non-blocking thật, hay chấp nhận.

### 3.2 Các việc chưa thực thi / chưa validate

- `write_system_command()` (`plc_modbus_cfg.c`) mới decode + đổi status
  sang ACCEPTED, **chưa thực thi** reboot / factory reset / clear rules
  / clear retain.
- Chưa validate `guard_tag`/`trigger_tag`/`action_tag` nằm trong
  `0..MAX_TAGS-1` (hoặc `GUARD_TAG_NONE`). Index sai không crash nhưng
  rule bị vô hiệu hóa âm thầm.
- **PVD → ghi Retain khẩn cấp chưa được nối.** Có sẵn: PVD bật trong
  CubeMX, `HAL_PWR_PVDCallback()`, `sx_power_register_low_voltage_
  callback()`, `retain_snapshot_write()`. Thiếu: không ai gọi
  `sx_power_register_low_voltage_callback(retain_snapshot_write)`.
- `TRG_TIME_WINDOW`: `now_hhmm` hardcode 0, chưa có nguồn RTC.
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM`: chưa có
  implementation.

### 3.3 Tồn tại trong repo nhưng KHÔNG PHẢI việc của Claude sửa

`STM32H523xx_FLASH.ld` khai `LENGTH = 512K`, chip thật chỉ 256KB. File
CubeMX tự sinh — nếu sửa thì phải qua CubeMX/`.ioc`, không sửa tay. Chỉ
báo người dùng.

## 4. Bài học đã rút ra (quan trọng, đọc kỹ)

### 4.1 Luôn đọc file định nghĩa trước khi thêm gì mới

Trước khi thêm hằng số/enum mới, đọc file gốc trước (đã có bài học thật
ở mục 1.4: suýt tạo mã lỗi Flash trùng ý nghĩa với `SPLC_ERROR_FLASH` có
sẵn). Áp dụng chung: mã lỗi, hằng số tag, địa chỉ Modbus — kiểm tra tồn
tại trước khi định nghĩa mới.

### 4.2 CRC của Rule Table PHẢI tính trên byte wire, không phải byte RAM struct

Tốn 3 vòng debug trên board mới lộ (commit `9789b3a` trở đi). Chuỗi bug
gốc: (1) FC06 tới thanh ghi RW bị từ chối do bảng dispatch đặt callback
sai cột; (2) CRC tính trên byte RAM little-endian của struct — sai cả ý
nghĩa (phụ thuộc chip) lẫn giá trị (`nmbs_crc_calc` trả CRC hoán byte);
(3) `test_plc.py` cũ cũng tính CRC kiểu RAM, khớp firmware cũ nhưng sai
spec. **Quy ước chốt theo spec 8.4:** CRC tính trên `rule_count x 32
byte` wire — mỗi register high byte trước, mỗi trường 32-bit High Word
rồi Low Word. Firmware: `rule_record_to_wire()` + `rule_table_wire_
crc16()` trong `plc_modbus_cfg.c` (KHÔNG dùng `nmbs_crc_calc` cho việc
này — nó trả CRC hoán byte, dùng cho RTU framing). App: `rule_registers_
to_bytes()` trong `test_plc.py`. Việc lưu Flash ở mục 1 phải dùng đúng
2 hàm firmware này, không viết công thức CRC thứ ba.

### 4.3 Con số trong log là bằng chứng định danh phiên bản code, không phải để đoán

Khi bế tắc, in cả các giả thuyết cạnh nhau (ví dụ: CRC tính theo cả 2
cách rồi log cả 2 giá trị) thay vì suy diễn từ một con số duy nhất. Đã
từng nghi nhầm "board chạy ELF cũ" trong khi thực ra 2 module (App/
firmware) đang dùng 2 công thức CRC khác nhau.

### 4.4 Mô phỏng đầu-cuối trên PC trước khi lên board

Build 1 chương trình C `#include` thẳng file `.c` cần test (để thấy cả
`static`), transport/flash giả qua RAM hoặc stdin/stdout, chạy chính
script Python thật (không viết lại test) nói chuyện với chương trình đó.
Bắt được lỗi mà test từng phía riêng lẻ bỏ sót. Lưu ý stub `logger.h`
phải in ra `stderr`, không phải `stdout`, nếu protocol test đọc dữ liệu
qua `stdout`.

### 4.5 Log "không thấy" thường do bị trôi, không phải do không chạy

Vòng poll nhanh (10 lần/giây) sinh nhiều dòng DEBUG che mất dòng quan
trọng. Trước khi kết luận "không log ra", thử lọc theo từ khóa hoặc rút
ngắn thời gian chạy trước khi nghi ngờ code không chạy tới đó.

### 4.6 Quyết định kiến trúc: diễn giải lại bằng lời của mình để người dùng xác nhận

Khi người dùng mô tả 1 cơ chế phức tạp (ví dụ cơ chế A/B ở mục 1.2),
diễn giải lại thành đoạn văn rõ ràng, đầy đủ từng bước và hỏi "đúng ý
bạn không" trước khi code — rẻ hơn nhiều so với code sai rồi sửa lại.

## 5. Quy trình làm việc với người dùng

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Bắt đầu phiên hoặc khi
  người dùng báo "đã push": `git pull`, rồi **build/compile verify thật**
  (không chỉ đọc diff).
- **Giao file:** người dùng muốn nhận **nguyên file** (present từng file
  để copy-paste cả file), KHÔNG muốn patch/zip/đoạn thay thế.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, **tách từng
  quyết định nhỏ**, không gộp nhiều câu vào một. Nếu cơ chế phức tạp,
  diễn giải lại bằng lời trước khi code (mục 4.6).
- **Không tự sửa file CubeMX tự sinh** (ghi "Auto-generated") — kể cả
  khi thấy bug thật. Chỉ báo người dùng.
- Khi đề xuất nguyên nhân, nói rõ mức chắc chắn; kiểm chứng bằng dữ liệu
  thật (log, test PC) trước khi khẳng định.
- Trước khi thêm hằng số/mã lỗi/enum mới, đọc file định nghĩa gốc trước
  (mục 4.1).

## 6. Lệnh verify nhanh (không cần toolchain ARM)

**Layer 2** (rule engine): compile `core/plc_tag/plc_tag.c`,
`core/plc_rule/plc_rule.c`, `core/plc_internal_rule/*.c` bằng
`gcc -std=c11` cùng test dùng `tag_write`/`rule_scan(now_ms)`/
`rule_table_commit`. Cần stub `logger.h` (`log_info/warn/debug/error`).
Kỳ vọng: `sizeof(SPLC_RuleRecord)==32`, `TAG_DI0==0`, test pass.

**`plc_modbus_cfg.c` + nanoMODBUS**: link `plc_modbus_cfg.c`,
`libs/nanomodbus/nanomodbus.c` và Layer 2 với 1 `modbus_transport_t` giả
(`read`/`write` qua buffer RAM, `unit_id = 1`), stub `sx_time.h` và
`logger.h`.

**Nạp rule đầu-cuối (App thật ↔ firmware PC):** build 1 chương trình C
`#include` thẳng `plc_modbus_cfg.c`, transport giả đọc/ghi hex qua
stdin/stdout, stub `logger.h` in ra **stderr** (không phải stdout, sẽ
lẫn vào luồng frame). Chạy `test_plc.py`/`test_rule.py` thật với 1
`Client` giả nói chuyện RTU với chương trình đó.

**Ranh giới layer:** `nm -u <file>.o` không được có symbol `sx_usb_*`
với Layer 3 (`plc_modbus_cfg.c`), hay `sx_flash_*`/`sx_usb_*` với Layer 2
(`plc_rule.c`, `plc_tag.c`). Áp dụng cho `plc_rule_flash.c` mới: PHẢI có
`sx_flash_*` (đúng, đây là Layer 3), KHÔNG được xuất hiện trong bất kỳ
file Layer 2 nào.