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
- **Bước 7 (RTC) đang làm dở:** phần driver (Layer U/1/0) đã viết và build ARM
  thành công; còn Layer 3/4/2 và test. Chi tiết ở mục 3.
- **Sau đó: Bước 8 (FB Timer/Counter).** Chưa có dòng code nào.
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
| 7a | RTC driver (chưa nối Modbus/Rule Engine) | `utils/epoch/`, `components/rtc/sx_rtc.h`, `platforms/stm32/stm32h5/rtc/` |

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
8. **RTC (Bước 7): RTC nội, clock LSI 32 kHz, KHÔNG có VBAT/pin, KHÔNG có
   thạch anh riêng.** Hệ quả đã chốt: mất điện → RTC về "chưa đồng bộ"
   (`SYNCED=0`), Studio ghi lại giờ ngay khi kết nối; reset mềm (REBOOT,
   watchdog) → giữ giờ và giữ `SYNCED`. `HW_PRESENT=0`, `BATTERY_LOW=0` cố
   định. Giờ lệch dần theo sai số LSI giữa 2 lần đồng bộ — chấp nhận được với
   Time Window theo phút; cần chính xác hơn thì thêm thạch anh LSE.
9. **KHÔNG sửa file CubeMX tự sinh, kể cả trong khối `USER CODE`** (người
   dùng chốt: sửa sẽ phá cấu trúc layer, mất khi regenerate/đổi phần cứng).
   Giải pháp cho RTC xem mục 3 (linker `--wrap`).

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

### Bước 7 — RTC (`0x0810`, 4 reg) — ĐANG LÀM DỞ

Layout: `epoch_utc_s` (u32, High Word trước), `tz_offset_min` (i16),
`status_flags` (u16: `0x0001` SYNCED, `0x0002` HW_PRESENT, `0x0004`
BATTERY_LOW). Studio ghi cả 4 thanh ghi trong 1 lệnh FC16 ngay sau khi kết nối.

**Cấu hình CubeMX đã chốt (đừng đổi khi regenerate):** RTC clock source = LSI
(`RCC_RTCCLKSOURCE_LSI`, LSI bật trong RCC), Activate Calendar, 24 h,
**Async 127 / Sync 249** (cho 1 Hz với LSI 32 kHz; 127/255 chỉ đúng với LSE
32.768 kHz, dùng nhầm thì giờ chậm ~2,3 %), alarm/wakeup/timestamp tắt.

**ĐÃ XONG (build ARM thành công; chưa test trên board vì chưa có đường
đồng bộ giờ từ Studio):**
- `utils/epoch/splc_epoch.{h,c}` (Layer U): epoch ↔ ngày giờ, và
  `splc_epoch_to_hhmm(epoch, tz_offset_min)`. Đã kiểm trên PC với 3011 epoch
  so với `datetime` của Python: 0 sai lệch (gồm năm nhuận, 1970, 2106,
  tz `+420` và `-300`).
- `components/rtc/sx_rtc.h` (Layer 1): `sx_rtc_set_epoch()`,
  `sx_rtc_get_epoch()`, `sx_rtc_is_synced()`. Chỉ nói UTC epoch; múi giờ KHÔNG
  nằm ở đây mà ở khối Layer 3 `0x0810`.
- `platforms/stm32/stm32h5/rtc/stm32h5_rtc.{h,c}` (Layer 0): dùng `hrtc` do
  CubeMX sinh. Năm hợp lệ 2000..2099 (giới hạn RTC), ngoài khoảng → `false`.
  Cờ SYNCED lưu ở thanh ghi backup **`RTC_BKP_DR8`** (không dùng DR0..DR7 vì
  các thanh ghi đó không đọc được nếu khoá boot hardware key), magic
  `0x53504C43`. Backup domain mất cùng RTC khi mất VDD → đúng vòng đời cần.
  `HAL_RTC_GetTime` luôn theo sau bởi `HAL_RTC_GetDate`.
- CMake: `components/CMakeLists.txt` (thêm `rtc` vào include),
  `platforms/stm32/stm32h5/CMakeLists.txt` (thêm `rtc/stm32h5_rtc.c`, include
  `rtc` + `utils/epoch`), `simple_plc/CMakeLists.txt` (thêm `splc_epoch.c` và
  include `utils/epoch` vào executable, như `cqueue.c`).

**ĐÃ GIAO, CHƯA XÁC NHẬN ĐÃ ÁP DỤNG + BUILD — giữ giờ qua reset mềm bằng
linker `--wrap`:** `MX_RTC_Init()` (CubeMX, `Core/Src/rtc.c`) kết thúc bằng
`HAL_RTC_SetTime/SetDate` ghi 01/01/2000 vô điều kiện mỗi lần boot, nên mỗi
`REBOOT` sẽ xoá giờ. Để không sửa file CubeMX:
- `stm32h5_rtc.c` định nghĩa `__wrap_HAL_RTC_SetTime` / `__wrap_HAL_RTC_SetDate`:
  cho qua nếu gọi từ chính driver (cờ `s_driver_write`) hoặc nếu chưa SYNCED;
  bỏ qua (trả `HAL_OK`) nếu đã SYNCED.
- `simple_plc/CMakeLists.txt` thêm
  `target_link_options(${CMAKE_PROJECT_NAME} PRIVATE -Wl,--wrap=HAL_RTC_SetTime -Wl,--wrap=HAL_RTC_SetDate)`.
- Thiếu 2 dòng link option này thì link lỗi
  `undefined reference to __real_HAL_RTC_SetTime` — cố ý (không để mất giờ
  âm thầm). HAL_RTC_Init tự bỏ qua cấu hình prescaler khi lịch đã khởi tạo
  (INITS) nên không làm mất giờ.
- Logic đã kiểm bằng mô hình nhỏ trên PC (3 bước: mặc định khi chưa sync, ghi
  giờ khi sync, bỏ qua mặc định ở boot sau). CHƯA kiểm trên ARM/board.
- **Việc tiếp:** hỏi người dùng đã áp dụng 2 file này (`stm32h5_rtc.c`,
  `simple_plc/CMakeLists.txt`) và build ARM thành công chưa. Nếu link lỗi
  với `--wrap` trên toolchain thật, báo lại để tìm hướng khác.

**CÒN LẠI:**
1. **Layer 3, `plc_modbus_cfg.c`:** `0x0810..0x0813`: FC03 đọc; FC16 ghi 4
   thanh ghi → `sx_rtc_set_epoch()`, giữ `tz_offset_min` trong RAM (không
   Flash). Quyết định cuối về `status_flags`, xem bên dưới. Ghi epoch ngoài
   2000..2099 → Modbus exception `0x02`/`0x03` (chọn khi code, theo Wire
   Contract mục 5.3 nếu có quy định; nếu không, hỏi người dùng).
2. **Layer 4, `plc_engine.c`:** mỗi scan cycle tính `hhmm` bằng
   `splc_epoch_to_hhmm()` nếu `sx_rtc_is_synced()`; nếu chưa sync báo "không
   có giờ hợp lệ".
3. **Layer 2, `plc_rule.c`:** `rule_scan()` nhận thêm tham số `hhmm` (Layer 2
   không được include Layer 0/1), bỏ `now_hhmm = 0` hardcode. Rule
   `TRG_TIME_WINDOW` KHÔNG được fire khi chưa sync. Cần đọc
   `plc_rule_eval.c` (`trigger_timing_ok`) để xử lý `Op==EQ, Lo==Hi` (chỉ fire
   ở sườn lên chuyển phút) và khung qua nửa đêm (`Lo>Hi`).
4. **Test:** viết `test_rtc.py`: đọc/ghi/đọc lại `0x0810`, khớp GV-004 (trừ bit
   HW_PRESENT nếu chọn phương án A), giờ sống sót qua `REBOOT` (cần `--wrap`
   đã áp dụng), sau mất điện thật về `SYNCED=0`, rule time-window fire đúng
   khung giờ và không fire khi chưa sync.

**QUYẾT ĐỊNH ĐANG TREO — `status_flags` (`0x0813`):** người dùng chưa chốt
(nói "chưa hiểu", rồi dừng phiên). Đã giải thích: Studio ghi cả 4 thanh ghi
trong 1 FC16 nên firmware nhận 1 giá trị `status_flags` từ Studio và phải
quyết định xử lý. Ba phương án:
- **A (đề xuất của Claude):** bỏ qua giá trị Studio ghi, tự tính khi đọc:
  `SYNCED` từ `sx_rtc_is_synced()`, `HW_PRESENT=0`, `BATTERY_LOW=0`. Lệch
  GV-004 (`Flags=3`) đúng 1 bit `HW_PRESENT` — lệch có chủ đích vì board dùng
  LSI nội.
- **B:** theo đúng GV-004 (`HW_PRESENT=1`) — đúng byte nhưng báo sai sự thật.
- **C:** lưu nguyên giá trị Studio ghi — rủi ro báo sai SYNCED.
**Đừng tự chọn:** nhắc lại và hỏi người dùng bằng `ask_user_input_v0` trước
khi code Layer 3 (giải thích bằng lời đơn giản, người dùng đã nói chưa hiểu).

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
- Sai số LSI: nếu Time Window cần chính xác hơn, cân nhắc hiệu chuẩn hoặc thêm
  thạch anh LSE (đổi cấu hình CubeMX + prescaler về 127/255, code `sx_rtc` giữ
  nguyên).

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
10. **Cần hành vi khác với code CubeMX sinh ra?** Đừng sửa file sinh (kể cả khối
    `USER CODE`). Chặn ở lớp thấp hơn: linker `--wrap` cho hàm HAL, hoặc dựng
    lớp `sx_*` riêng. Giữ mọi thứ ở Layer 0 + CMake để regenerate/đổi phần cứng
    không làm mất.
11. **Kiểm cú pháp driver ARM trên PC:** biên dịch bằng gcc host với HAL thật của
    repo (`-DUSE_HAL_DRIVER -DSTM32H523xx -ICore/Inc -IDrivers/...`). Cảnh báo
    `cast to pointer from integer` trong header vendor là do host 64-bit, bỏ
    qua; chỉ lỗi/cảnh báo từ file của mình mới quan trọng. Stub HAL tự viết phải
    có include guard, nếu không sinh lỗi giả.

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
| `test_rtc.py` | **CHƯA VIẾT** (Bước 7, xem mục 3) |
| `test_sysclear.py COM14 [--reboot]` | Bước 6: 3 lệnh xoá, persistence qua reboot, lệnh trong diag, lệnh sai. **Ghi Flash và xoá sạch rule + retain trên board.** |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`.

**`splc_epoch` trên PC:** `gcc -std=c11 -Wall -Wextra -Iutils/epoch` + chương trình
in `epoch y m d H M S weekday hhmm(+420) hhmm(-300) roundtrip` cho nhiều epoch,
so với `datetime` của Python (đã làm: 3011 mẫu, 0 sai lệch). Làm lại khi sửa
`splc_epoch.c`.

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