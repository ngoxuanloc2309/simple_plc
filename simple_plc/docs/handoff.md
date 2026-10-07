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

**Đang ở giai đoạn lập kế hoạch cho Wire Profile V2.0 — CHƯA bắt đầu code
phần V2.0 nào.** Toàn bộ nền tảng V1.9 (Rule Engine, Flash persistence,
REBOOT, multi-board) đã chạy ổn trên board thật + App thật, coi là xong,
chi tiết nén ở mục 1. Việc cần làm tiếp theo: mục 2 (kế hoạch V2.0, chia
bước nhỏ theo thứ tự).

Board hiện dùng: **Zigbee-IO SKU** (`board/board_device/board_zigbee_io.c`),
4 DI / 4 DO / 0 AI, STM32H523CCU6. Branch **`board_dev`**.

## 1. Nền tảng V1.9 — ĐÃ XONG (tóm tắt, không sửa lại trừ khi có lý do mới)

- **Rule Engine chạy đúng trên board thật + App thật**, nạp/chạy đúng N
  rule (test tới 4), kể cả live watch. 2 bug lớn từng chặn việc này đã sửa
  (chunk size App > 64 register vỡ gói USB CDC 64 byte; `guard_tag=0` bị
  App hiểu nhầm là "không guard" thay vì `GUARD_TAG_NONE=0x7FFF`) — chi
  tiết đầy đủ ở lịch sử git log nếu triệu chứng tương tự tái diễn.
- **Rule Table lưu Flash (2-sector A/B, atomic, ping-pong)**: ĐÃ XONG, đã
  verify Flash thật trên board. Thiết kế: `services/plc_rule_flash/
  plc_rule_flash.{c,h}`, CRC tính trên **byte wire** (`rule_table_wire_
  crc16()`), KHÔNG phải byte RAM struct (mục 4.2 — tốn 3 vòng debug mới
  chốt, đừng lặp lại sai lầm này ở V2.0).
- **`SPLC_SYSTEM_CMD_REBOOT`**: ĐÃ XONG. `app/plc_app/plc_system_cmd_
  service.{c,h}` (Layer 4) dịch lệnh Modbus thành `sx_system_reset()`
  thật, gọi cuối `scan_cycle()`. Trì hoãn 300ms (`PLC_REBOOT_DELAY_MS`)
  trước khi reset thật — để phản hồi FC06 kịp truyền hết qua USB trước
  khi MCU biến mất, ĐỪNG bỏ delay này khi sửa lại.
- **Multi-board refactor**: ĐÃ XONG. Layer 2 (`plc_tag.c`) nhận
  `SPLC_TagLayout` qua tham số runtime thay vì hard-code. Đổi board chỉ
  cần viết 1 file `board_<sku>.c` mới trong `board/board_device/`.
- **`FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN`**: CHƯA implement thật,
  chỉ trả `ACCEPTED` suông. Giờ đã có đủ quyết định để code (mục 2, bước
  2.5) — không còn là "chờ chốt phạm vi" nữa.

## 2. KẾ HOẠCH: Chuyển sang Wire Profile V2.0

### 2.0 Bối cảnh & tài liệu nguồn

Không dùng v1.7/v1.9 nữa. V2.0 là **strict superset** của v1.9 — mọi thứ
ở mục 1 vẫn giữ nguyên layout wire, V2.0 chỉ **thêm** 3 khối chức năng
mới. 2 tài liệu nguồn (đọc theo thứ tự ưu tiên khi có mâu thuẫn — xem
mục 2.1 câu 1 về 1 mâu thuẫn đã gặp và cách xử lý):

1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` —
   struct nhị phân, memory map tổng quan.
2. `docs/SimplePLC_Wire_Contract_V2_Draft.md` — hành vi chi tiết,
   state machine, validation rule, golden vector. **Ưu tiên tài liệu này
   khi 2 tài liệu mâu thuẫn về HÀNH VI** (structs doc chỉ định nghĩa
   layout, không định nghĩa behavior).

**3 khối mới cần implement:**
- **RTC** (`0x0810`, 4 reg) — đồng bộ epoch time từ App, phục vụ
  `TRG_TIME_WINDOW` (đang hardcode `now_hhmm=0`, mục 3.3 cũ).
- **Diagnostic Control Block** (`0x0A20`, 5 reg) — cơ chế lease-based
  (3000ms, heartbeat) cho phép App ghi trực tiếp DO/VFLAG/VREG/
  VREG_RETAIN/COUNTER qua `0x0900..0x09FF` khi ở state `DIAG_CONTROL`.
  DI/AI luôn Read-Only (đúng mô hình vật lý, không đổi).
- **Function Block Timer/Counter** (`0x0B00`/`0x0B40`, 128 reg) — 8 Timer
  (TON/TOF/TP) + 8 Counter (CTU/CTD/CTUD/HSC) kiểu IEC 61131-3, chạy độc
  lập song song Rule Engine.

### 2.1 Quyết định thiết kế đã chốt (người dùng xác nhận trực tiếp — ĐỪNG tự ý đổi, hỏi lại nếu nghi ngờ)

1. **`protocol_version = 2`** trong `DeviceDescriptor` (0x0000). 2 tài
   liệu từng mâu thuẫn (Structs doc + Golden Vector GV-001 ghi `2`, Wire
   Contract doc header ghi `1`) — đã hỏi, chốt theo `2`. Dòng header
   "Protocol Version: 1" trong Wire Contract doc là lỗi đánh máy của tài
   liệu, bỏ qua.

2. **RTC dùng RTC nội của STM32H5** (built-in domain, không IC rời/thạch
   anh 32.768kHz riêng trên board Zigbee-IO). Hệ quả cần làm rõ khi code
   tới (xem mục 2.2 câu hỏi còn treo #2): bit `SPLC_RTC_FLAG_HW_PRESENT`
   set sao cho đúng ý nghĩa; có `VBAT` nuôi domain backup khi mất nguồn
   chính không — cần kiểm tra schematic/CubeMX trước khi giả định "mất
   điện vẫn giữ giờ chạy tiếp".

3. **`SYSTEM_COMMAND` (0x0A00: REBOOT/FACTORY_RESET/CLEAR_RULES/
   CLEAR_RETAIN) LUÔN thực thi ngay lập tức**, bất kể `DIAG_STATE` đang
   là gì hay `RETAIN_DIRTY` đang là `0`/`1`. KHÔNG áp dụng cơ chế chặn
   "dirty interlock" (cơ chế đó trong Wire Contract doc chỉ áp dụng cho
   `CMD_EXIT_DIAG`, KHÔNG áp dụng cho `SYSTEM_COMMAND`). Đã hỏi lại 2 lần
   vì người dùng đảo ngược lựa chọn ban đầu — đây là quyết định cuối.

4. **Trước khi thực thi `SYSTEM_COMMAND` bất kỳ trong lúc `DIAG_STATE ==
   DIAG_CONTROL`, firmware phải tự động set MỌI tag ghi-được-trong-diag
   (DO, VFLAG, VREG, VREG_RETAIN, COUNTER) về `0`** (baseline factory —
   không phải snapshot lúc `ENTER_DIAG`, luôn luôn là `0`), rồi mới thực
   thi lệnh. Mục đích: tránh actuator vật lý (relay/van) bị kẹt ở trạng
   thái "lỡ tay bật test" khi MCU reboot hoặc dữ liệu bị xoá.
   - Phạm vi: áp dụng cho **mọi lệnh** trong `SYSTEM_COMMAND` (không chỉ
     REBOOT), và **mọi nhóm tag ghi được trong diag**.
   - Hệ quả kỹ thuật: cần một cơ chế theo dõi "tag nào đã bị ghi trong
     phiên diag hiện tại" (vd. dirty-bitmap theo tag_idx) — KHÔNG thể chỉ
     dựa vào `RETAIN_DIRTY` vì flag đó chỉ bao phủ riêng `VREG_RETAIN`,
     không bao quát DO/VFLAG/VREG/COUNTER.
   - KHÔNG áp dụng cho luồng `CMD_EXIT_DIAG` bình thường (thoát diag khi
     không có SYSTEM_COMMAND nào tới) — luồng đó giữ nguyên hành vi theo
     Wire Contract doc mục 6.3 Case B: chạy 1 vòng scan thật, Rule Engine
     tự tính output dựa trên input vật lý + rule hiện hành.

### 2.2 Câu hỏi còn treo (hỏi khi code tới phần liên quan, đừng tự suy đoán)

1. **Thứ tự ưu tiên implement**: Diagnostic Control trước hay FB
   Timer/Counter trước? (Kế hoạch bước ở mục 2.3 đang giả định Diagnostic
   Control trước vì nó mở khóa test tay DO — xác nhận lại nếu người dùng
   muốn đổi thứ tự.)
2. **Bit `SPLC_RTC_FLAG_HW_PRESENT`** (0x0810, status_flags bit 1) nên
   set `1` hay `0` với RTC nội STM32H5 — tài liệu có vẻ ngụ ý bit này
   dành cho IC RTC rời, cần hỏi lại ý nghĩa đúng trước khi code phần RTC.
3. **Flash sector cho `VREG_RETAIN` ping-pong** (Wire Contract Appendix A
   mô tả 1 scheme riêng, header 16 byte + CRC) — có dùng chung cơ chế/
   sector với Rule Table A/B hiện có (mục 1, `plc_rule_flash.c`) hay cần
   vùng Flash riêng? Đối chiếu với `plc_retain.c` hiện tại trước khi code
   để tránh xung đột sector.

### 2.3 Các bước implement (theo thứ tự, mỗi bước build/test độc lập trước khi sang bước sau)

**Bước 1 — DeviceDescriptor & DeviceResourceInfo báo đúng V2.0: ĐÃ XONG**
- Set `protocol_version = 2`, `rule_format_version = 7`,
  `wire_profile = SPLC_WIRE_PROFILE_V2` (giá trị mới thêm vào enum).
- File đã sửa: `core/plc_device/plc_device.h` (thêm
  `SPLC_WIRE_PROFILE_V2` vào `SPLC_WireProfile` enum),
  `board/board_device/board_zigbee_io.c` (set 3 giá trị thật trong
  `board_device_info_init()`).
- **Build ARM thật thành công, người dùng đã xác nhận.** Chưa có log/
  test thật xác nhận App hoặc `test_plc.py read_info` đọc đúng giá trị
  qua Modbus trên board — nên verify khi tiện, không bắt buộc trước khi
  làm Bước 2.

**Bước 2 — Diagnostic Control Block (`0x0A20`), KHÔNG kèm write runtime tag — CODE XONG, CHỜ VERIFY TRÊN BOARD**

*Đã làm (verify trên PC: compile -Wall -Wextra sạch phần code mới, 48 check callback + chạy `test_diag.py` đầu-cuối qua frame RTU thật với firmware PC, kể cả frame GV-009 khớp từng byte. `plc_engine.c` mới chỉ kiểm cú pháp, chưa chạy):*
- `core/plc_diag/plc_diag.h` (Layer 2, chỉ type): enum command/state/flags/error + hằng số lease. `SPLC_DiagErrorCode` là enum RIÊNG, không dùng chung `SPLC_ErrorCode`.
- `plc_modbus_cfg.{c,h}` (Layer 3) sở hữu RAM state. `0x0A20` xử lý trực tiếp trong `cb_write_single_register` (FC06) và đầu `cb_write_multiple_registers` (FC16 quantity=1 OK, quantity>1 → exception `0x03`), KHÔNG qua `s_blocks[]`. `0x0A20..0x0A24` đọc qua `s_blocks[]`.
- `plc_engine.c` (Layer 4): gọi `plc_modbus_cfg_diag_tick(elapsed_ms)` ĐẦU `scan_cycle()` (elapsed = thời gian thật, không phải 10ms danh nghĩa), và bỏ qua `rule_scan()` khi `DIAG_CONTROL`. `input_scan`/`output_scan` vẫn chạy.
- `test_diag.py`: script verify trên board (`python test_diag.py COM5`, thêm `--skip-expiry` để bỏ test 4s).

*Diễn giải spec đã chọn (xác nhận lại nếu không đúng ý):*
1. Lệnh xử lý ngay trong callback Modbus, không có mailbox hàng đợi (callback chạy sau `output_scan()`, trước `input_scan()` vòng sau = đúng scan boundary). `STATE_TRANSITIONING` đã định nghĩa nhưng Bước 2 không bao giờ đặt nó.
2. Cột "`ERR_NONE`" trong ma trận 4.7 hiểu là "không phát sinh lỗi mới", KHÔNG xoá latch. Chỉ ENTER thành công / DISCARD (xoá `ERR_RETAIN_DIRTY`) / reboot mới xoá, theo 4.5. Nếu không, HEARTBEAT 1s/lần sẽ xoá `ERR_LEASE_EXPIRED` trước khi App kịp đọc.
3. HEARTBEAT sau khi lease hết hạn giữ nguyên `ERR_LEASE_EXPIRED` (không ghi đè bằng `INVALID_COMMAND`).
4. Đọc `0x0A20` trả lệnh cuối đã ghi (để khớp GV-005: `CMD=2`), dù Wire Contract ghi WO.
5. Ghi giá trị `0` vào `0x0A20` = không làm gì, không báo lỗi; giá trị >5 → `ERR_INVALID_COMMAND`.

*Còn để ngỏ cho Bước 4/5 (đã đánh dấu `STEP 4` trong code):* COMMIT/DISCARD hiện là no-op vì `RETAIN_DIRTY` luôn 0; lease hết hạn chưa huỷ RAM shadow retain; khi `rule_scan()` tiếp tục sau diag, `prev_value` của rule còn giá trị cũ nên có thể sinh 1 edge giả (chưa xử lý, xem comment trong `scan_cycle()`).

*Kế hoạch gốc của bước này:*
- Thêm state machine `DIAG_STATE` (ENGINE_RUNNING/DIAG_CONTROL/
  TRANSITIONING/FAULT), xử lý `CMD_ENTER_DIAG`/`CMD_HEARTBEAT`/
  `CMD_EXIT_DIAG` theo bảng state transition ở Wire Contract mục 4.7.
- Lease countdown 3000ms, giảm mỗi scan cycle (10ms).
- CHƯA cho ghi `0900..09FF` ở bước này — chỉ test được state chuyển đúng
  qua App/script (ENTER → đọc DIAG_STATE=2 → HEARTBEAT giữ lease →
  EXIT → về DIAG_STATE=1).
- Verify: script Python mô phỏng enter/heartbeat/exit, đọc lease đếm
  lùi đúng; để quá 3000ms không heartbeat, xác nhận tự động rơi về
  ENGINE_RUNNING với `DIAG_ERROR_CODE=LEASE_EXPIRED`.

**Bước 3 — Runtime Tag Write qua `0900..09FF` khi `DIAG_CONTROL`**
- Thêm `write_runtime_tag_values()`, validate theo per-tag group (mục
  2.0's tham chiếu Wire Contract mục 5.1 — KHÔNG dùng `TagIndex <
  runtime_tag_count`, dùng validate theo từng group capacity riêng).
- All-or-nothing cho multi-tag FC16 (Wire Contract mục 5.2) — 1 tag sai
  trong span thì reject toàn bộ frame, không ghi gì.
- Chặn ghi khi `DIAG_STATE != DIAG_CONTROL` → Modbus Exception `0x02`.
- Chặn ghi DI/AI/RESERVED ở MỌI state → luôn `0x02`.
- Theo dõi dirty-bitmap tag đã ghi trong phiên diag (phục vụ bước 5).
- Verify: ghi DO0 khi đang DIAG_CONTROL → đọc lại thấy giá trị mới, chân
  vật lý đổi thật; ghi DI0 → bị reject `0x02`; ghi khi ENGINE_RUNNING →
  bị reject `0x02`.

**Bước 4 — Retain Dirty Interlock cho `VREG_RETAIN`**
- Ghi `VREG_RETAIN` trong diag chỉ sửa RAM shadow, set `RETAIN_DIRTY=1`.
- `CMD_COMMIT_RETAIN`/`CMD_DISCARD_RETAIN` theo mục 7 Wire Contract —
  cần trả lời câu hỏi treo #3 (mục 2.2) trước bước này.
- `CMD_EXIT_DIAG` khi `RETAIN_DIRTY=1` → reject, latch
  `ERR_RETAIN_DIRTY` (theo đúng bảng mục 4.7 — lưu ý: quyết định #3/#4 ở
  mục 2.1 CHỈ miễn trừ cho `SYSTEM_COMMAND`, KHÔNG miễn trừ cho
  `CMD_EXIT_DIAG` — 2 luồng này có quy tắc khác nhau, đừng nhầm).

**Bước 5 — Baseline reset trước khi thực thi SYSTEM_COMMAND (quyết định #4)**
- Implement đúng mục 2.1 câu 4: khi nhận `SYSTEM_COMMAND` bất kỳ mà
  đang `DIAG_CONTROL`, set về `0` mọi tag có trong dirty-bitmap (từ bước
  3) trước khi gọi hành động thật (`sx_system_reset()`, Flash erase...).
- Verify: ENTER_DIAG → set DO0=1 → gửi REBOOT → xác nhận DO0 bị set về
  0 (log hoặc đọc lại nếu kịp trước khi MCU reset) trước khi reset xảy
  ra.

**Bước 6 — `FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` thực thi thật**
- Giờ đã đủ điều kiện code (quyết định #3, #4 đã chốt — không còn mơ hồ
  phạm vi). `CLEAR_RULES` = `rule_table_commit(rule_count=0)`.
  `CLEAR_RETAIN` = ghi `s_vreg_retain_shadow[]` toàn `0` + Flash commit.
  `FACTORY_RESET` = cả 2 cộng lại (xác nhận phạm vi chính xác với người
  dùng ngay trước khi code bước này nếu còn điểm mơ hồ nào khác).

**Bước 7 — RTC (`0x0810`)**
- `write` `epoch_utc_s`/`tz_offset_min` từ App lưu RAM (không Flash, trừ
  khi câu hỏi treo #3 ở mục 2.2 nói khác).
- Tính `current_hhmm` mỗi scan cycle theo công thức Structs doc mục 7,
  cắm vào `TRG_TIME_WINDOW` (thay thế `now_hhmm=0` hardcode cũ).
- Giải quyết câu hỏi treo #2 (mục 2.2) trước khi set `status_flags`.

**Bước 8 — FB Timer/Counter (`0x0B00`/`0x0B40`)**
- Khối lớn nhất, độc lập Rule Engine. Implement theo pseudocode tham
  khảo ở Wire Contract mục 9.5 (đã có sẵn ví dụ TON, cần làm thêm
  TOF/TP đối xứng + CTD/CTUD/HSC).
- Counter có `retain_tag_index` link tới `VREG_RETAIN` — phụ thuộc bước
  4 đã xong.

### 2.4 Việc còn treo từ V1.9, giải quyết tiện thể trong kế hoạch trên

- `guard_tag`/`trigger_tag`/`action_tag` chưa validate nằm trong
  `0..MAX_TAGS-1` — index sai không crash nhưng rule bị vô hiệu hoá âm
  thầm. Nên thêm validate khi động vào `plc_modbus_cfg.c` ở bước 3.
- PVD → ghi Retain khẩn cấp chưa nối (`sx_power_register_low_voltage_
  callback(retain_snapshot_write)` chưa ai gọi) — không thuộc V2.0 nhưng
  nên tiện tay nối lại nếu đụng tới `plc_retain.c` ở bước 4.
- `ACT_WRITE_REMOTE`/`ACT_LOG_EVENT`/`ACT_SEND_ALARM`: chưa implement,
  không thuộc phạm vi V2.0 lần này, để riêng.
- `modbus_usb_write()` bỏ qua `timeout_ms`, có thể vượt ngân sách scan
  10ms dưới tải nặng (đo được 83ms với stress test 100 rule) — chưa
  quyết định sửa hay chấp nhận, không thuộc V2.0.

## 3. Bài học đã rút ra (đọc kỹ, áp dụng khi code V2.0)

### 3.1 Luôn đọc file định nghĩa trước khi thêm gì mới

Trước khi thêm hằng số/enum mới, đọc file gốc trước — đã có 2 bài học
thật (suýt trùng mã lỗi `SPLC_ERROR_FLASH`; suýt bỏ sót `GuardTagIndexMask`
có sẵn). Áp dụng ở CẢ HAI phía App và firmware.

### 3.2 CRC của Rule Table/Retain PHẢI tính trên byte wire, không phải byte RAM struct

Tốn 3 vòng debug mới chốt ở V1.9. Quy ước: CRC tính trên dữ liệu **wire**
(mỗi register high byte trước, mỗi trường 32-bit High Word rồi Low Word),
KHÔNG dùng `nmbs_crc_calc` (nó trả CRC hoán byte, chỉ dùng cho RTU
framing). Áp dụng tương tự cho CRC Retain ping-pong ở V2.0 (Wire Contract
Appendix A.1 — CCITT-FALSE poly `0x1021`, khác thuật toán với CRC Modbus
RTU `0xA001`, đừng nhầm 2 loại CRC này).

### 3.3 Con số trong log là bằng chứng định danh phiên bản code, không phải để đoán

Khi bế tắc, in cả các giả thuyết cạnh nhau thay vì suy diễn từ 1 con số.

### 3.4 Mô phỏng đầu-cuối trên PC trước khi lên board

Build chương trình C `#include` thẳng file `.c` cần test, transport/flash
giả qua RAM hoặc stdin/stdout, chạy script Python thật nói chuyện với
chương trình đó. Stub `logger.h` phải in ra `stderr`, không phải `stdout`.

### 3.5 Log "không thấy" thường do bị trôi, không phải do không chạy

Vòng poll nhanh (10 lần/giây) sinh nhiều dòng DEBUG che mất dòng quan
trọng — lọc theo từ khóa trước khi nghi ngờ code không chạy tới đó.

### 3.6 Quyết định kiến trúc: diễn giải lại bằng lời trước khi code

Khi người dùng mô tả 1 cơ chế phức tạp, diễn giải lại thành đoạn văn rõ
ràng và hỏi "đúng ý bạn không" trước khi code — rẻ hơn code sai rồi sửa.
Đã áp dụng đúng cách này khi chốt quyết định #4 ở mục 2.1 (2 lần hỏi lại
cho rõ trước khi ghi nhận).

### 3.7 Khi debug bug xuyên 2 hệ thống (App + Firmware), đọc CẢ HAI repo thật

2 repo liên quan: firmware (`ngoxuanloc2309/simple_plc`, branch
`board_dev`) và App (`ngoxuanloc2309/paa`, .NET). Nhiều bug lớn ở V1.9
nằm ở phía App, không phải firmware — log firmware "trông đúng" không
có nghĩa bug không nằm ở đó. Đọc code App thật khi nghi ngờ, đừng chỉ
đoán từ log firmware.

### 3.8 Đừng code liều hành động phá huỷ dữ liệu khi spec mơ hồ

`FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` từng bị hoãn ở V1.9 vì spec
mơ hồ — đã hỏi xác nhận phạm vi cụ thể trước khi code (mục 2.1, quyết
định #3/#4). Nguyên tắc chung: bất cứ lệnh nào Flash-erase dữ liệu nên
hỏi xác nhận phạm vi cụ thể trước, không suy đoán "chắc ý họ là...".

## 4. Quy trình làm việc với người dùng

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Bắt đầu phiên hoặc khi
  người dùng báo "đã push": `git pull`, rồi **build/compile verify thật**
  (không chỉ đọc diff).
- **2 repo liên quan:** firmware (branch `board_dev`) và App (.NET). Bug
  giao tiếp App↔MCU có thể nằm ở BẤT KỲ phía nào (mục 3.7).
- **Giao file:** người dùng muốn nhận **nguyên file** (present từng file
  để copy-paste cả file), KHÔNG muốn patch/zip/đoạn thay thế.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, **tách từng
  quyết định nhỏ**, không gộp nhiều câu vào một. Diễn giải lại bằng lời
  trước khi code nếu cơ chế phức tạp (mục 3.6).
- **Không tự sửa file CubeMX tự sinh** (ghi "Auto-generated") — kể cả
  khi thấy bug thật. Chỉ báo người dùng.
- Khi đề xuất nguyên nhân, nói rõ mức chắc chắn; kiểm chứng bằng dữ liệu
  thật (log, test PC, đọc code thật) trước khi khẳng định (mục 3.7).
- Trước khi thêm hằng số/mã lỗi/enum mới, đọc file định nghĩa gốc trước
  (mục 3.1), ở cả 2 phía App/firmware nếu liên quan giao tiếp Modbus.
- **Đừng implement hành động phá huỷ dữ liệu khi spec mơ hồ** — hỏi xác
  nhận cụ thể trước (mục 3.8).

## 5. Lệnh verify nhanh (không cần toolchain ARM)

**Layer 2** (rule engine): compile `core/plc_tag/plc_tag.c`,
`core/plc_rule/plc_rule.c`, `core/plc_internal_rule/*.c` bằng
`gcc -std=c11` cùng test dùng `tag_write`/`rule_scan(now_ms)`/
`rule_table_commit`. Cần stub `logger.h` (`log_info/warn/debug/error`).
Kỳ vọng: `sizeof(SPLC_RuleRecord)==32`, `TAG_DI0==0`, test pass.

**`plc_modbus_cfg.c` + nanoMODBUS**: link `plc_modbus_cfg.c`,
`libs/nanomodbus/nanomodbus.c` và Layer 2 với 1 `modbus_transport_t` giả
(`read`/`write` qua buffer RAM, `unit_id = 1`), stub `sx_time.h` và
`logger.h`. Ví dụ include path thật đã dùng được (STM32H5-specific
headers KHÔNG cần, chỉ Layer 2/3/U):
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
**Không áp dụng được cho `plc_system_cmd_service.c`** (include thẳng
`sx_system.h`/`sx_time.h` gate bởi `#if STM32H5_PLATFORM`, cần HAL/CMSIS
thật) — chỉ kiểm tra được bằng mắt + build ARM thật.

**Nạp rule đầu-cuối (App thật ↔ firmware PC):** build 1 chương trình C
`#include` thẳng `plc_modbus_cfg.c`, transport giả đọc/ghi hex qua
stdin/stdout, stub `logger.h` in ra **stderr** (không phải stdout, sẽ
lẫn vào luồng frame). Chạy `test_plc.py`/`test_rule.py` thật với 1
`Client` giả nói chuyện RTU với chương trình đó. Có sẵn
`replay_app_style.py` (mô phỏng đúng cách App .NET gộp
`RULE_COUNT_STAGED`+`EXPECTED_CRC16` vào 1 request, và ghi cả
`STAGING_RULE_TABLE` trong 1 request lớn) — dùng để cô lập bug phía nào
(App hay firmware) khi log firmware một mình không đủ (mục 3.7).

**Ranh giới layer:** `nm -u <file>.o` không được có symbol `sx_usb_*`
với Layer 3 (`plc_modbus_cfg.c`), hay `sx_flash_*`/`sx_usb_*` với Layer 2
(`plc_rule.c`, `plc_tag.c`). `plc_rule_flash.c` PHẢI có `sx_flash_*`
(đúng, Layer 3), KHÔNG được ở Layer 2. `plc_system_cmd_service.c`
(Layer 4) PHẢI có `sx_system_reset` — không được gọi thẳng từ
`plc_modbus_cfg.c` (Layer 3, transport/protocol-agnostic theo thiết kế).