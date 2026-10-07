# SimplePLC — Handoff

> Mục đích: cho phiên làm việc tiếp theo (Claude khác hoặc chính bạn) nắm
> "đang ở đâu, làm gì tiếp" mà không cần đọc lại lịch sử chat.
> Đọc SAU `Readme.md` và `docs/architecture.md` (kiến trúc layer).
> File này chỉ ghi: trạng thái hiện tại, quyết định đã chốt, kế hoạch tiếp theo,
> bài học và quy trình làm việc.
>
> **Quy tắc vàng:** trước khi tin bất cứ điều gì dưới đây, chạy
> `git pull && git log --oneline -10`. Nếu có commit mới hơn, ưu tiên code
> thật hơn file này.
>
> **Lưu ý:** `docs/architecture.md` mô tả Wire Profile V1.9 và có vài chỗ đã
> lỗi thời (vd. "Layer 3/4 hoàn toàn rỗng"). Với V2.0, nguồn đúng là 2 tài
> liệu ở mục 2.

## 0. Trạng thái hiện tại

- Board: **Zigbee-IO SKU** (`board/board_device/board_zigbee_io.c`), 4 DI /
  4 DO / 0 AI / 32 VFLAG / 32 VREG / 32 VREG_RETAIN / 8 COUNTER,
  STM32H523CCU6. Branch **`board_dev`**.
- **Bước 1 → 6 của Wire Profile V2.0 đã xong và verify trên board thật.**
- **Việc tiếp theo: Bước 7 (RTC), rồi Bước 8 (FB Timer/Counter).** Chưa có
  dòng code nào của 2 bước này.
- **Chưa verify (không chặn việc tiếp theo):**
  - Rút nguồn thật khi retain = giá trị đã commit, bật lại, đọc lại. Đã
    verify qua `REBOOT` (dữ liệu sống sót qua reset), chưa verify qua mất điện.
  - Mất điện / ghi Flash lỗi thật giữa lúc đang ghi (chỉ có thể test thủ công).

## 1. Đã xong (tóm tắt, không sửa lại trừ khi có lý do mới)

**Nền tảng V1.9**
- Rule Engine, Rule Table lưu Flash 2-sector A/B (`services/plc_rule_flash/`),
  REBOOT (`app/plc_app/plc_system_cmd_service.c`, trì hoãn 300 ms —
  `PLC_REBOOT_DELAY_MS`, ĐỪNG bỏ delay này), multi-board
  (`SPLC_TagLayout` truyền qua tham số; đổi board = viết 1 file
  `board_<sku>.c`).
- CRC Rule Table/Retain tính trên **byte wire** (`rule_table_wire_crc16()`),
  không phải byte RAM struct.

**V2.0 — Bước 1..6**

| Bước | Nội dung | Vị trí chính |
|---|---|---|
| 1 | Descriptor báo `protocol_version=2`, `rule_format_version=7`, `wire_profile=2` | `core/plc_device/plc_device.h`, `board_zigbee_io.c` |
| 2 | Diagnostic Control Block `0x0A20`: ENTER/HEARTBEAT/EXIT, lease 3000 ms đếm theo thời gian thật, hết lease tự về `ENGINE_RUNNING` + `ERR_LEASE_EXPIRED` | `core/plc_diag/plc_diag.h`, `plc_modbus_cfg.c`, `plc_engine.c` |
| 3 | Ghi tag qua `0x0900..0x09FF` khi `DIAG_CONTROL`; validate theo KIND thật trong `g_tag_table[]`; all-or-nothing; dirty-bitmap `s_diag_dirty[]` | `plc_modbus_cfg.c` |
| 4 | `VREG_RETAIN` ghi vào bản nháp RAM; `RETAIN_DIRTY`; COMMIT/DISCARD; `EXIT_DIAG` bị chặn khi bẩn | `plc_modbus_cfg.c`, `plc_retain.{c,h}` |
| 5 | Trước mọi `SYSTEM_COMMAND` trong diag, tag host đã ghi trong phiên về 0 (`diag_baseline_reset_dirty_tags()`) | `plc_modbus_cfg.c` |
| 6 | `CLEAR_RULES` / `CLEAR_RETAIN` / `FACTORY_RESET` thật | `app/plc_app/plc_system_clear.{c,h}`, `plc_system_cmd_service.c` |

**Cơ chế Bước 6:** không erase sector thô. Ghi một bản ghi RỖNG hợp lệ qua
đường lưu đã verify (`plc_rule_flash_save()` / `retain_snapshot_write()`, tự
đọc lại kiểm CRC) — mất điện giữa chừng thì còn nguyên bản cũ hoặc nguyên bản
rỗng, không bao giờ lẫn. Lỗi Flash → khôi phục RAM, trả `ERROR` +
`SPLC_ERROR_FLASH`. `FACTORY_RESET` = `CLEAR_RETAIN` + `CLEAR_RULES` (cả hai
luôn được thử). Không reboot; App tự gửi REBOOT nếu muốn. Không đụng `VREG`
thường, `COUNTER`, tag layout, device identity. Xoá rule không đụng giá trị tag:
DO đang bật vẫn giữ đến khi reboot.

## 2. Tài liệu nguồn & quyết định đã chốt

V2.0 là **strict superset** của V1.9. Khi 2 tài liệu mâu thuẫn về HÀNH VI,
ưu tiên Wire Contract:

1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` — struct,
   memory map.
2. `docs/SimplePLC_Wire_Contract_V2_Draft.md` — hành vi, state machine,
   validation, golden vector.

Đã chốt (người dùng xác nhận — ĐỪNG tự ý đổi, hỏi lại nếu nghi ngờ):

1. **`protocol_version = 2`.** Dòng "Protocol Version: 1" trong header Wire
   Contract là lỗi đánh máy.
2. **RTC dùng RTC nội của STM32H5** (không IC rời / thạch anh riêng).
3. **`SYSTEM_COMMAND` luôn thực thi ngay**, bất kể `DIAG_STATE` hay
   `RETAIN_DIRTY`. Dirty interlock chỉ áp dụng cho `CMD_EXIT_DIAG`.
4. **Trước `SYSTEM_COMMAND` trong `DIAG_CONTROL`, tag ghi-được-trong-diag về 0**
   (baseline 0, không phải snapshot lúc ENTER). Không áp dụng cho luồng
   `CMD_EXIT_DIAG` thường.
5. **Thoát diag (EXIT hoặc hết lease): reset runtime của MỌI rule** như vừa
   nạp (`rule_runtime_reset()`, gọi trong `scan_cycle()` khi
   `plc_modbus_cfg_is_rule_engine_suspended()` chuyển true→false). Bảng rule
   giữ nguyên, không ghi Flash. Hệ quả chấp nhận: input đang cao lúc thoát diag
   → rule `ON_RISE` fire ngay lượt đầu.
6. **Retain lưu bằng `plc_retain.c`** (log xoay vòng 3 sector, record 208
   byte), KHÔNG làm ping-pong Appendix A. Lệch tài liệu CÓ CHỦ ĐÍCH ở định
   dạng Flash; App không thấy vì không đọc được Flash.
7. **Phạm vi Bước 6** như mục 1 (chỉ rule + retain).

**Lệch có chủ đích khác so với tài liệu (đội App cần biết):**
- `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload từ Flash (Rule Engine dừng trong
  diag nên giá trị live = lúc ENTER; reload sẽ lùi tới 5 phút thay đổi thật).
- Tag layout board này **DENSE** (DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71,
  RETAIN 72-103, COUNTER 104-111), khác bảng cố định trong Structs doc mục 4
  (DO ở 8..15). Code validate theo kind nên đúng cho cả hai. Test script tính
  index từ `DeviceResourceInfo`, không hard-code.
- Đọc `0x0A20` trả lệnh cuối đã ghi (để khớp GV-005 `CMD=2`), dù Wire Contract
  ghi WO.
- Cột "ERR_NONE" trong ma trận 4.7 hiểu là "không phát sinh lỗi mới", không
  xoá latch; chỉ ENTER thành công / DISCARD / reboot mới xoá.

**Hành vi quan sát được, cần đội App biết:**
- Sau `CLEAR_RULES` / `CLEAR_RETAIN` / `FACTORY_RESET` trong diag, phiên diag
  VẪN SỐNG (`DIAG_STATE` giữ `DIAG_CONTROL`, lease vẫn đếm). App tự EXIT.
- `active_rule_version` chỉ nằm trong RAM: về 1 sau mỗi reboot.
- Lệnh xoá chạy đồng bộ trong 1 chu kỳ scan; erase/ghi Flash có thể vượt 10 ms,
  `FACTORY_RESET` lâu nhất (2 lần ghi). Phản hồi USB có thể trễ — App nên để
  timeout đủ rộng và poll `0x0A01` để thấy `DONE`.

**Cần người dùng xác nhận lại (diễn giải của Claude, chưa được duyệt rõ ràng):**
- Quyết định #4 với `VREG_RETAIN`: hiện bản nháp bị BỎ, không ghi 0 vào giá trị
  live (ghi 0 sẽ phá dữ liệu đã commit). Xoá retain thật là việc của
  `CLEAR_RETAIN` / `FACTORY_RESET`.
- Quyết định #4 với phạm vi: hiện chỉ reset tag HOST ĐÃ GHI trong phiên (theo
  bitmap), không đụng tag do rule bật trước ENTER. Muốn mạnh hơn (đưa TẤT CẢ
  DO về 0) thì đổi sang quét theo kind, sửa 1 chỗ trong
  `diag_baseline_reset_dirty_tags()`.

## 3. Kế hoạch các bước tiếp theo

### Bước 7 — RTC (`0x0810`, 4 reg)

Layout: `epoch_utc_s` (u32, High Word trước), `tz_offset_min` (i16),
`status_flags` (u16: `0x0001` SYNCED, `0x0002` HW_PRESENT, `0x0004`
BATTERY_LOW). Studio tự ghi FC16 4 reg ngay sau khi kết nối để đồng bộ giờ.

Việc cần làm:
- Đọc/ghi `0x0810..0x0813` trong `plc_modbus_cfg.c` (nguồn thật là RTC nội
  qua một API Layer 1 mới, vd. `sx_rtc.h`, và implementation
  `platforms/stm32/stm32h5/rtc/`). Khớp GV-004 (`Flags=3`, tz `+420`).
- Mỗi scan cycle tính `current_hhmm` rồi truyền vào `TRG_TIME_WINDOW`
  (đang hardcode `now_hhmm = 0` trong `plc_rule.c`). Công thức (Structs mục 7):
  ```
  local_epoch    = epoch_utc_s + tz_offset_min * 60
  seconds_of_day = local_epoch % 86400
  current_hhmm   = (seconds_of_day / 3600) * 100 + (seconds_of_day % 3600) / 60
  ```
  Khung `Lo<=Hi` : `Lo <= hhmm <= Hi`; qua nửa đêm `Lo>Hi`: `hhmm>=Lo || hhmm<=Hi`;
  `Op==EQ, Lo==Hi`: chỉ fire ở sườn lên chuyển phút.
- Layer 2 không được include Layer 0/1 → `hhmm` truyền vào `rule_scan()` như
  tham số (giống `now_ms`).
- Test: golden vector GV-004 / GV-008; viết `test_rtc.py` (ghi epoch + tz, đọc
  lại, rule `TIME_WINDOW` fire đúng khung giờ); kiểm tra chưa sync thì rule
  time-window không fire bừa.

**Câu hỏi PHẢI hỏi người dùng trước khi code Bước 7:**
1. Bit `HW_PRESENT` với RTC nội STM32H5 nên set `1` hay `0`? Structs doc ghi
   bit này là "có IC RTC hoặc thạch anh 32.768 kHz" — có vẻ dành cho RTC rời.
2. Board Zigbee-IO có nối **VBAT** nuôi backup domain không (xem schematic /
   `RS485_IO_RF_V2.ioc`)? Quyết định có được giả định "mất điện vẫn giữ giờ"
   và cách set `BATTERY_LOW`. Nếu không có VBAT, mỗi lần cấp điện lại phải đồng
   bộ lại → `SYNCED=0` sau reset.
3. Có lưu `tz_offset_min` vào Flash không (mặc định: không, chỉ RAM)?

### Bước 8 — FB Timer / Counter (`0x0B00` / `0x0B40`)

Khối lớn nhất, chạy độc lập song song Rule Engine, theo Wire Contract mục 9
(pseudocode TON ở 9.5, cần thêm TOF/TP, CTD/CTUD/HSC).
- 8 Timer (TON/TOF/TP) mỗi khối 8 reg: status, mode, `pt_ms`, `et_ms`.
- 8 Counter (CTU/CTD/CTUD/HSC) mỗi khối 8 reg: status, mode, `pv`, `cv`,
  `retain_tag_index` (link tới `VREG_RETAIN`, `0xFFFF` = không retain).
- Cần quyết định trước khi code: nguồn cấu hình FB (ai ghi `mode`/`pt_ms`/`pv`?
  hiện bảng chỉ mô tả vùng đọc RO), counter retain dùng cơ chế retain nào, và
  **`CLEAR_RETAIN` / `FACTORY_RESET` có xoá counter có `retain_tag_index` không**
  (hiện không đụng `COUNTER`).
- Xác nhận lại thứ tự ưu tiên (câu hỏi treo cũ: Diagnostic Control đã làm
  trước, nên Bước 8 là phần V2.0 cuối cùng).

### Việc tồn đọng, giải quyết tiện thể khi đụng tới file liên quan

- Validate `guard_tag` / `trigger_tag` / `action_tag` nằm trong `0..MAX_TAGS-1`
  (hoặc `GUARD_TAG_NONE`) khi nạp rule — hiện index sai không crash nhưng rule
  bị vô hiệu hoá âm thầm.
- PVD → ghi Retain khẩn cấp chưa nối
  (`sx_power_register_low_voltage_callback(retain_snapshot_write)` chưa ai gọi).
- `modbus_usb_write()` bỏ qua `timeout_ms`, có thể vượt ngân sách scan 10 ms
  dưới tải nặng (đo được 83 ms với 100 rule).
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM` chưa implement,
  ngoài phạm vi V2.0.
- Một lỗi Flash KHÔNG thể lập trình được trong khi erase vẫn chạy: bước đầu của
  `plc_rule_flash_save()` (copy A→B) đã huỷ bản cũ trước khi biết lỗi. Có từ
  trước V2.0, áp dụng cho cả COMMIT rule thường và `CLEAR_RULES`.
- `docs/architecture.md` cần được soát lại cho khớp V2.0 và code hiện tại.

## 4. Bài học đã rút ra

1. **Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới** (đã suýt
   trùng `SPLC_ERROR_FLASH`, suýt bỏ sót `GuardTagIndexMask`). Áp dụng ở cả App
   và firmware.
2. **CRC của Rule Table/Retain tính trên byte wire**, không phải byte RAM
   struct. Không dùng `nmbs_crc_calc` (hoán byte, chỉ cho RTU framing). CRC
   Retain Appendix A là CCITT-FALSE `0x1021`, khác CRC Modbus RTU `0xA001`.
3. **Bằng chứng trước khi kết luận:** con số trong log là dấu vân tay của phiên
   bản code; in các giả thuyết cạnh nhau thay vì suy diễn từ 1 con số. Log
   "không thấy" thường do bị trôi (poll 10 lần/giây sinh nhiều DEBUG) — lọc theo
   từ khóa trước khi nghi code không chạy.
4. **Bug xuyên App + Firmware:** đọc CẢ HAI repo (`ngoxuanloc2309/simple_plc`
   branch `board_dev`, và `ngoxuanloc2309/paa` .NET). Nhiều bug lớn ở V1.9 nằm
   phía App (chunk > 64 register vỡ gói USB CDC 64 byte; `guard_tag=0` bị hiểu
   là "không guard").
5. **Thiết kế test cho đúng:** test chỉ có ý nghĩa nếu trạng thái đầu xác định.
   Rule thử chỉ SET DO0=1 mà DO0 đã kẹt ở 1 thì không phân biệt được rule chạy
   hay dừng → reset board, xác nhận `DO0=0` trước. Test persistence phải kiểm
   dữ liệu SỐNG SÓT qua reboot trước khi xoá (precondition), nếu không "trống
   sau reboot" không chứng minh gì.
6. **Mô phỏng đầu-cuối trên PC trước khi lên board** (mục 6). Stub `logger.h`
   in ra `stderr`, không phải `stdout`.
7. **Diễn giải lại bằng lời trước khi code** khi cơ chế phức tạp; hỏi từng
   quyết định nhỏ, không gộp.
8. **Không code liều hành động phá huỷ dữ liệu khi spec mơ hồ** — hỏi xác nhận
   phạm vi cụ thể trước.
9. **`switch/case` + fallthrough không tự nhiên an toàn hơn `if/continue`**;
   state machine phải test thực nghiệm kỹ.

## 5. Quy trình làm việc với người dùng

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Bắt đầu phiên hoặc khi người
  dùng báo "đã push": `git pull`, rồi **build/compile verify thật** (không chỉ
  đọc diff).
- **Giao file:** người dùng muốn nhận **nguyên file** (present từng file để
  copy-paste cả file), KHÔNG muốn patch/zip/đoạn thay thế.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, tách từng quyết định
  nhỏ.
- **Không tự sửa file CubeMX tự sinh** (ghi "Auto-generated"), kể cả khi thấy
  bug thật. Chỉ báo người dùng.
- Khi đề xuất nguyên nhân, nói rõ mức chắc chắn; kiểm chứng bằng dữ liệu thật
  (log, test PC, đọc code) trước khi khẳng định.
- Với mỗi bước mới: code → verify PC (nếu được) → người dùng build ARM → chạy
  script test trên board → cập nhật file này.

## 6. Verify & test

**Script test trên board** (cần `pip install pymodbus pyserial` đúng
interpreter — dùng `python -m pip`, máy dev có nhiều Python do ESP-IDF):

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | descriptor, nạp rule, commit, live watch |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, Rule Engine thực sự dừng, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD, baseline reset trước REBOOT |
| `test_sysclear.py COM14 [--reboot]` | Bước 6: 3 lệnh xoá, persistence qua reboot, lệnh trong diag, lệnh sai. **Ghi Flash và xoá sạch rule + retain trên board.** |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`.

**Layer 2 không cần toolchain ARM:** compile `core/plc_tag/plc_tag.c`,
`core/plc_rule/plc_rule.c`, `core/plc_internal_rule/*.c` bằng `gcc -std=c11` +
stub `logger.h`. Kỳ vọng `sizeof(SPLC_RuleRecord)==32`.

**`plc_modbus_cfg.c` + nanoMODBUS:** link cùng Layer 2 với transport giả (đọc/ghi
qua buffer RAM, `unit_id=1`), stub `sx_time.h` và `logger.h`; chạy script Python
thật với firmware PC qua stdin/stdout. Include path đã dùng được:
```
gcc -c -std=c11 -fsyntax-only \
  -I services/plc_modbus_cfg -I services/plc_rule_flash \
  -I core/plc_rule -I core/plc_tag -I core/plc_device \
  -I core/plc_error -I core/plc_system_cmd -I libs/nanomodbus \
  -I port/modbus_transport -I utils/logger \
  -I platforms/stm32/stm32h5/flash_define \
  -I components/time -I components/flash \
  services/plc_modbus_cfg/plc_modbus_cfg.c
```
`plc_system_cmd_service.c` KHÔNG build được trên PC (gọi `sx_system_reset()`,
cần HAL/CMSIS). `plc_system_clear.c` build được trên PC với stub
`plc_rule_flash_save()` / `retain_snapshot_write()`.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 (`plc_modbus_cfg.c`) không được
có symbol `sx_usb_*`; Layer 2 không được có `sx_flash_*`/`sx_usb_*`;
`plc_rule_flash.c` PHẢI có `sx_flash_*`; `sx_system_reset` chỉ được gọi từ
`plc_system_cmd_service.c` (Layer 4), không từ `plc_modbus_cfg.c`.