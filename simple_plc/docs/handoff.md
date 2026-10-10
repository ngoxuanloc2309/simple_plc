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
>
> **Repo App** (`ngoxuanloc2309/paa`, .NET, public) cũng cần đọc khi làm việc
> xuyên App + firmware: `src/SimplePLC.Application/Logic/Compilation/RuleCompiler.cs`
> (biên dịch rule), `src/SimplePLC.Infrastructure/Devices/RuleTableWriter.cs`
> (thứ tự deploy), `src/SimplePLC.Infrastructure/Gateways/FunctionBlockGateway.cs`
> (đọc/ghi khối FB), `docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md`.
> Khi App trả lời bằng chữ, đối chiếu với code: đã có lần câu trả lời không
> khớp code (mục 4, bài học 12).

## 0. Trạng thái hiện tại

- Board: **Zigbee-IO SKU** (`board/board_device/board_zigbee_io.c`), 4 DI /
  4 DO / 0 AI / 32 VFLAG / 32 VREG / 32 VREG_RETAIN / 8 COUNTER,
  STM32H523CCU6. Branch **`board_dev`**.
- **Bước 1 → 6 của Wire Profile V2.0 đã xong và verify trên board thật.**
- **Bước 7 (RTC): xong và verify trên board thật** (`test_rtc.py COM14`: ALL PASS
  — khối `0x0810`, `status_flags` RO, từ chối ghi sai, đồng hồ -0.1 % so với
  thời gian thật, Time Window mốc phút/khung/qua nửa đêm). **Còn chưa chạy:**
  `test_rtc.py --reboot` và test mất điện thật bằng tay. Chi tiết ở mục 3.
- **Bước 8a (khối FB `0x0B00..0x0B7F`: nhận ghi cấu hình, đọc, Counter
  `CV`/`Q`): xong và verify trên board (CV/Q đã ĐỔI cách tính ở phiên 2026-10-08, xem #15; bản mới đã build ARM và verify trên board: `test_fb.py COM14`, `--reboot`, `--probe-64` đều ALL PASS)** (`test_fb.py COM14`: ALL PASS, kể cả
  `--probe-64`). **8c PVD ghi retain khẩn cấp: ĐÃ CODE; phần retain/Flash đã verify trên board (2026-10-09, `test_retain.py COM14` và `--periodic --commits 0`: ALL PASS), PHẦN PVD THẬT CHƯA TEST** (board chưa thiết kế phần cứng PVD/giữ điện; xem #25 và mục 3). **Bước 8d (Timer chạy thật): XONG — người dùng xác nhận 2026-10-10 đã chạy và test cùng App** (code: `plc_fb_scan()`, `rule_ref` ở thanh ghi `+6` của Timer trong `services/plc_fb/plc_fb.c`; các đoạn ghi "Timer chỉ lưu/đọc, `status`/`ET` = 0" và "chờ App chốt IN/RESET/Q" ở #20 và mục 3 là LỖI THỜI). Chi tiết ở mục 3.
- **Bước 8b (lưu cấu hình FB vào Flash; bản nháp FB + COMMIT): xong và verify trên board** (`test_fb.py COM14 --reboot`: ALL PASS; `test_rtc.py`, `test_tag.py`, `test_sysclear.py` cả bản thường lẫn `--reboot`: ALL PASS, không hồi quy). Ghi FB vào BẢN NHÁP; `0x0B00..` luôn đọc ra cấu hình đang chạy; COMMIT mới áp dụng và lưu Flash cùng Rule Table; cấu hình FB sống sót qua REBOOT. Hành vi 8a đổi: `test_fb.py` đã viết lại cho khớp. Chi tiết mục 2 (#19, #21..#24) và mục 3. **Còn chưa chạy:** `test_fb.py --probe-64` bản mới trên board, mất điện thật giữa lúc lưu (chỉ test thủ công được), build ARM chưa kiểm `.text+.data` so với `0x08036000`.
- **Đã sửa lỗi FC16 > 64 byte** (khung 73 byte không được trả lời) trong
  `components/usb_cdc/sx_usb_cdc.c` (`sx_usb_tiny_read()`), verify trên board
  bằng `test_fb.py --probe-64`. Xem mục 3.
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
| 7a | RTC driver | `utils/epoch/`, `components/rtc/sx_rtc.h`, `platforms/stm32/stm32h5/rtc/` |
| 7b | Khối RTC `0x0810`, Time Window, `status_flags` do firmware tự tính | `services/plc_rtc/`, `plc_modbus_cfg.c`, `plc_engine.c`, `plc_rule.c`, `plc_rule_eval.c`, `board_zigbee_io.c` (`g_rtc_caps`) |
| 8a | Khối FB `0x0B00..0x0B7F`: ghi cấu hình (all-or-nothing, bỏ qua field firmware sở hữu), đọc, Counter `CV`/`Q` đọc từ tag CV do App chọn (lúc viết 8a là `COUNTER[i]`, đã sửa, xem #15), `Q` tính ra. Cấu hình CHỈ trong RAM (chưa Flash) | `services/plc_fb/plc_fb.{h,c}`, `plc_modbus_cfg.c` (bảng block + `cb_write_multiple_registers`), `plc_engine.c` (`plc_fb_init()`), `services/CMakeLists.txt` |
| 8b | Cấu hình FB lưu Flash: bản nháp + cờ khối, COMMIT áp dụng; đoạn FB 112 byte nằm TRONG bản ghi Rule Table (bit 15 của `rule_count` = có đoạn FB, một CRC phủ cả hai) | `services/plc_fb/plc_fb.{h,c}`, `services/plc_rule_flash/plc_rule_flash.{h,c}`, `platforms/stm32/stm32h5/flash_define/splc_flash_define.h`, `plc_modbus_cfg.c` (`write_commit_command()`), `app/plc_app/plc_system_clear.c` |
| fix | FC16 > 64 byte được trả lời (đo thời gian thật + chuyển byte FIFO TinyUSB → `rxQueue` trong lúc chờ) | `components/usb_cdc/sx_usb_cdc.c` (`sx_usb_tiny_read()`) |

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
10. **`status_flags` (`0x0813`) là READ-ONLY, firmware tự tính mỗi lần đọc**
    (người dùng chốt: App chỉ đọc, không ghi): `SYNCED` từ `sx_rtc_is_synced()`,
    `HW_PRESENT` từ `g_rtc_caps.hw_present` (board tự khai báo, Zigbee-IO = 0),
    `BATTERY_LOW` = 0. Studio vẫn gửi FC16 4 thanh ghi (thanh ghi thứ 4 bị bỏ
    qua) hoặc 3 thanh ghi (epoch + tz); cả hai được chấp nhận. Lệch GV-004
    (`Flags=3`) đúng 1 bit `HW_PRESENT` — có chủ đích.
11. **Time Window theo đúng V2.0 (Structs mục 7), spec v0.1 đã lỗi thời**
    (người dùng chốt): `trigger_tag` bị bỏ qua; `Lo<=Hi` khung trong ngày;
    `Lo>Hi` khung qua nửa đêm — cả hai bắn MỖI scan khi điều kiện đúng (mức);
    `compare_op==EQ` và `Lo==Hi` = mốc phút, bắn 1 lần ở sườn lên. Mọi
    `compare_op` khác EQ bị bỏ qua. KHÔNG có lớp tương thích v0.1: ví dụ 4.4
    (`Lo=700, Hi=0, NONE`) bị đọc thành khung 07:00..00:00. **Đội App phải sinh
    rule "đúng giờ" dạng `EQ, Lo=Hi`.** Chưa SYNCED / chưa có tz → rule không
    bắn; đồng bộ giờ đúng lúc đang ở phút đích → bắn 1 lần; reboot/thoát diag
    giữa phút đó → bắn lại (runtime reset, quyết định #5).
12. **`tz_offset_min` chỉ giữ trong RAM** (không Flash). Sau REBOOT RTC vẫn giữ
    giờ + `SYNCED` nhưng tz về 0 và `plc_rtc_get_local_hhmm()` trả "không hợp
    lệ" cho đến khi Studio ghi lại `0x0810`: Time Window KHÔNG chạy trong
    khoảng đó (cố ý — thiếu tz thì giờ địa phương sẽ lặng lẽ thành UTC). Mất
    điện thật thì mất cả giờ (không VBAT) nên cũng cần Studio kết nối lại.
    Ghi epoch ngoài 2000..2099 hoặc tz ngoài -720..+840 → exception `0x03`,
    không đổi gì; ghi bắt đầu ở `0x0812`/`0x0813` → `0x02`; FC06 hoặc qty
    khác 3/4 → `0x03`.

13. **Bước 8 theo hướng A ("dual generation"), đã thống nhất với team App:**
    với `WireProfile >= 2`, App VẪN sinh macro rule (Timer/Counter chạy bằng
    Rule Engine, `INC_COUNTER`, dwell) VÀ ghi thêm bảng cấu hình FB. Firmware
    chưa có engine Timer/Counter riêng. Bảng FB chỉ để lưu cấu hình và hiển
    thị. Lý do: struct FB 16 byte không có field "tag nguồn" nên firmware
    không biết `IN`/`CU`/`CD`/`RESET` nối tag nào. (Lúc kiểm tra code App,
    `RuleCompiler.cs` chỉ sinh macro rule khi `WireProfile < 2`; App đã đồng
    ý sửa, **chưa kiểm tra lại code App sau khi sửa**.)
14. **Quyền vùng FB (người dùng chấp nhận theo trả lời của team App):**
    `0x0B00..0x0B7F` là R/W. Host ghi cấu hình khi Deploy bằng FC16 —
    Timer: `mode` (+1), `pt_ms` (+2..+3); Counter: `mode` (+1),
    `preset_value` (+2..+3), `retain_tag_index` (+6). Firmware sở hữu
    `status_bits`, `et_ms`/`current_value`, `reserved`: nếu Host ghi kèm (App
    ghi CẢ khối 8 reg, các field đó = 0) thì firmware nhận lệnh và BỎ QUA
    các field đó. **Lệch Structs v2.0 mục 3.6/3.7 (ghi RO) — Structs cần sửa
    cho khớp.**
15. **CV của Counter nằm ở tag do App chọn (SỬA 2026-10-08, thay cho quy ước cũ
    "Counter `i` ↔ tag `COUNTER[i]`" vốn dựa trên giả định sai).** Trong App
    ("Storage Register (CV)"), người dùng chọn tag chứa CV: `VREG_RETAIN`
    (mặc định), `VREG`, và về nguyên tắc cả `VFLAG`/`COUNTER`. Rule `INC_COUNTER`
    do App sinh ra cộng thẳng vào tag đó (INC_COUNTER chạy trên mọi tag, không
    giới hạn kind). Firmware chỉ ĐỌC tag đó để hiển thị: `current_value` = giá
    trị tag CV; `Q` (status bit 3): CTU `CV >= PV`, CTD `CV <= 0`. Không có tag
    CV (`0xFFFF`) thì `CV` và `Q` đều đọc 0 (CTD không được báo "xong" oan).
    `CU`/`CD`/`RESET` (bit 0..2) đọc ra 0. Counter `i` KHÔNG gắn với tag
    `COUNTER[i]`; hai thứ không liên quan. Mode 0 thì `CV` đọc ra 0.
16. **Mode:** Timer 1=TON, 2=TOF, 3=TP; Counter 1=CTU, 2=CTD; 0 = khối không
    dùng. CTUD và HSC bỏ (App xác nhận không cần).
17. **Thanh ghi `+6` của khối Counter = index toàn cục thật của tag chứa CV**
    (tên trên wire/Structs vẫn là `retain_tag_index`; trong firmware là
    `cv_tag`). Layout DENSE: 72..103 là VREG_RETAIN trên board này, nhưng tag CV
    có thể là VFLAG / VREG / VREG_RETAIN / COUNTER bất kỳ. Firmware validate:
    `0xFFFF` hoặc index tồn tại với kind VFLAG/VREG/VREG_RETAIN/COUNTER (DI, DO,
    AI, Modbus tag bị từ chối), không hai counter trùng tag. Counter không cần
    tag `COUNTER[i]` tồn tại (bỏ ràng buộc "counter không có trên board chỉ được
    mode 0"). Sai → exception `0x03`, ghi vượt `0x0B7F` hoặc bắt đầu dưới
    `0x0B00` → `0x02`.

    **Nguồn gốc (để không đổ lỗi nhầm):** Structs v2.0 mục 3.7 và Wire Contract 9.3
    chỉ mô tả `retain_tag_index` là "TagIndex VREG_RETAIN hoặc `0xFFFF`", KHÔNG có
    trường nào nói "tag chứa CV" nói chung. App làm đúng spec (chỉ gửi index khi CV
    là VREG_RETAIN). Lỗi thật là phía firmware phiên trước: tự giả định CV = tag
    `COUNTER[i]` mà không có căn cứ trong spec và không đối chiếu code App. Việc
    firmware giờ hiểu trường `+6` rộng hơn spec là MỞ RỘNG có chủ đích, tương thích
    ngược (index VREG_RETAIN cũ vẫn hợp lệ).

    **Việc phía App (CHƯA làm, KHÔNG gấp; App là repo `paa`, commit đã đọc
    `31029fe`):**
    - `src/SimplePLC.Application/Logic/Compilation/RuleCompiler.cs` dòng ~260:
      `retainTagIndex` chỉ lấy `CvTagIndex` khi nằm trong dải VREG_RETAIN, ngược lại
      `0xFFFF`. Sửa để gửi `CvTagIndex` cho mọi tag CV hợp lệ (VFLAG/VREG/VREG_RETAIN/
      COUNTER). Chỉ cần sửa ĐÚNG MỘT CHỖ này.
    - `SimplePLC.Studio/Services/RuleCompiler.cs` KHÔNG phải bộ biên dịch cũ độc lập:
      nó chuyển view-model thành đồ thị rồi gọi `_coreCompiler.Compile()` của
      Application (dòng ~270). Con số 84 ở dòng 138 chỉ là giá trị dự phòng khi người
      dùng chưa chọn tag CV; 84 SAI trên board Zigbee-IO (VREG_RETAIN bắt đầu ở tag 72,
      84 là VREG_RETAIN số 12 chỉ đúng với bảng cố định Structs mục 4). Nên đổi thành
      tag VREG_RETAIN đầu tiên lấy từ `TagCatalog`/`DeviceResourceInfo` (nguyên tắc
      "App không hard-code"). Hai interface cùng tên `IRuleCompiler` (Application và
      Studio) dễ gây nhầm khi đọc code.
    - Chưa sửa thì KHÔNG hỏng gì: rule đếm dùng `CvTagIndex` nên số đếm đúng; VREG_RETAIN
      vẫn lưu/xoá đúng. Hậu quả duy nhất: với CV là VREG/VFLAG thì `current_value` và
      `Q` trên khối FB (`0x0B40+`) đọc ra 0. Trong code App mình không thấy chỗ nào
      hiển thị CV từ khối FB (`RuntimeStateStore` chỉ lưu `Counters`; mình chỉ grep, chưa
      đọc kỹ giao diện), nên hiện chưa ảnh hưởng thứ người dùng nhìn thấy.
    - **Spec cần sửa cho khớp (CHƯA làm):** Structs v2.0 mục 3.7 và Wire Contract mục
      9.3: đổi mô tả `retain_tag_index` thành "TagIndex của tag chứa CV (VFLAG/VREG/
      VREG_RETAIN/COUNTER) hoặc `0xFFFF`", ghi rõ CV/Q đọc từ tag đó. Khi sửa App thì
      sửa spec cùng lúc, báo team App.

18. **Counter retain: KHÔNG cần code firmware (SỬA 2026-10-08).** Tag CV kind
    `VREG_RETAIN` được `plc_retain.c` lưu/nạp như mọi tag retain (giá trị live,
    `retain_snapshot_write()` đọc `tag_read`, 5 phút một lần khi có đổi), và
    `CLEAR_RETAIN`/`FACTORY_RESET` đã đưa các tag retain về 0 nên số đếm cũng về 0.
    Người dùng chọn VREG/VFLAG thì số đếm mất khi reboot, đúng ý. Không chép
    `CV` → tag retain (cơ chế đó chỉ cần nếu CV nằm ở tag khác), không đặt
    `RETAIN_DIRTY`. Bỏ các điểm chưa chốt (a) từ chối ghi tag retain gắn Counter,
    (b) `CLEAR_RETAIN` đưa Counter về 0, (c) cách lưu Counter: không còn tồn tại.
    Mã mẫu Wire Contract 9.5 (chép `CV` vào `s_vreg_retain_shadow[]` + bật
    `RETAIN_DIRTY`) vẫn KHÔNG làm theo.

19. **Cấu hình FB lưu Flash cùng Rule Table — ĐÃ LÀM (8b).** Đoạn FB (112 byte, big-endian, chỉ field do Host sở hữu: 8 Timer × {mode u16, pt_ms u32} + 8 Counter × {mode u16, preset i32, retain_tag_index u16}) nằm CUỐI chính bản ghi Rule Table; bit 15 của trường `rule_count` = "có đoạn FB" (bit 0..14 = số rule); MỘT `crc16` phủ header + rule + FB. Mất điện giữa chừng chỉ còn nguyên bản cũ hoặc nguyên bản mới, không bao giờ rule mới + FB cũ. Mọi lần save luôn ghi đoạn FB. Bản ghi cũ (trước 8b, không cờ) vẫn nạp được, FB = DISABLED. Chiều ngược lại: firmware CŨ đọc cờ như `rule_count > MAX_RULES` và coi bản ghi hỏng (hạ cấp firmware cần Deploy lại). Mọi code đọc trường này PHẢI tách cờ trước (`& 0x7FFF` / `& 0x8000`). Kích thước tối đa 3320 byte, vừa 1 sector 8 KB.

20. **[LỖI THỜI — Timer đã chạy thật, xem mục 0 / 8d]** **Timer: hoãn phần chạy** (người dùng chọn làm Counter trước). Hiện Timer
    chỉ lưu và đọc lại `mode`/`pt_ms`; `status`/`ET` đọc ra 0. Chờ App chốt
    cách nối `IN`/`RESET`/`Q` (mục 3, Bước 8d).

21. **Bản nháp FB ("Cách 2", người dùng chốt):** `plc_fb_write()` ghi vào bản nháp, validate ngay lúc ghi (all-or-nothing, exception `0x02`/`0x03` như 8a). Mỗi khối Host ghi (bất kỳ thanh ghi nào của khối, kể cả field firmware sở hữu) được đánh cờ "đã ghi"; lần đầu chạm khối, bản nháp của nó khởi tạo từ cấu hình đang chạy (FC06 một thanh ghi giữ nguyên các field còn lại). Đọc `0x0B00..` luôn trả cấu hình ĐANG CHẠY (giống `ACTIVE_RULE_TABLE`): giữa lúc ghi FB và COMMIT đọc lại thấy giá trị cũ. Lý do: cả Deploy (rule + FB) có tính nguyên tử, và 8c/8d cần vùng đệm này. Tốn thêm khoảng 112 byte RAM nháp.

22. **COMMIT thành công:** `plc_fb_commit_draft()` chạy NGAY SAU `rule_table_commit()` và TRƯỚC `plc_rule_flash_save()`: khối có cờ nhận nội dung nháp, khối KHÔNG có cờ về DISABLED (cấu hình thuộc về chương trình đang commit), rồi lưu Flash. Lưu Flash lỗi: RAM giữ bản mới, `CONFIG_ERROR_CODE = FLASH` (giống rule).

23. **Bản nháp sống bao lâu:** xoá (cùng cờ) khi COMMIT xong dù thành công hay LỖI (CRC sai, `rule_table_commit` từ chối), khi `CLEAR_RULES`/`FACTORY_RESET`, và khi boot. Lý do: App ghi FB TRƯỚC khi nạp rule nên firmware không có "điểm bắt đầu Deploy"; nếu nháp sống qua COMMIT lỗi, khối ghi dở sẽ dính sang Deploy sau dù project đã bỏ nó. Hệ quả: mỗi Deploy App phải ghi lại FB từ đầu (App đã làm vậy). Ghi sai magic vào `0xA000` KHÔNG phải một lần COMMIT nên không xoá nháp. Quy tắc cũ "cờ còn qua COMMIT lỗi" bị thay bằng quy tắc này.

24. **Kiểm tra trùng `retain_tag_index` chỉ so giữa các khối CÓ TRONG BẢN NHÁP**, không so với cấu hình đang chạy (khối không ghi sẽ DISABLED sau COMMIT). Ví dụ: đang chạy Counter 0 dùng tag 72; project mới bỏ Counter 0, Counter 1 dùng tag 72; App chỉ ghi Counter 1: phải được chấp nhận. `plc_fb_import()` (nạp từ Flash) coi mọi khối là có mặt. `CLEAR_RULES`/`FACTORY_RESET` đưa FB về DISABLED cùng lúc với rule (trước khi lưu, để bản ghi rỗng mang đoạn FB rỗng); lưu lỗi thì khôi phục cả rule lẫn FB trong RAM.

29. **Chế độ RTOS của thư viện (`SX_OS_USE_FREERTOS` trong `app/sx_os_config.h`, 2026-10-10).** Thư viện KHÔNG tạo task và không đụng Cube: host gọi `plc_engine_init()` rồi `plc_engine_poll()` từ MỘT task của họ (`for(;;){ plc_engine_poll(); vTaskDelay(1); }`); mọi thứ phía sau vẫn đơn luồng, không khóa (tag, bảng rule, bản nháp FB/Modbus, retain), task khác không được đụng vào. Macro = 0 thì hành vi y như trước (các hàm `sx_os_*` là inline rỗng). Macro = 1 chỉ đổi 3 thứ: (a) `sx_delay_ms/_s` ngủ qua scheduler (`vTaskDelay`, làm tròn LÊN tick) khi scheduler đang chạy, trước đó vẫn `HAL_Delay`; (b) các vòng chờ bận (`sx_usb_tiny_read` chờ nốt khung, `sx_usb_tiny_write` khi FIFO TX đầy, vòng bơm USB 500 ms lúc boot trong `board_zigbee_io.c`) gọi `sx_os_yield_wait()` = ngủ 1 tick; (c) logger dùng mutex (`FREE_RTOS` giờ lấy từ `SX_OS_USE_FREERTOS`; sửa luôn lỗi cũ: thoát sớm khi `serial_write == NULL` làm mutex bị giữ mãi, và `log_print_hex` dùng chung `buff` mà không khóa). `sx_get_tick_ms()` GIỮ `HAL_GetTick()` (an toàn trong ISR PVD ưu tiên 0, không phụ thuộc RTOS): host phải để HAL timebase chạy (mặc định Cube; khi bật FreeRTOS nên đổi timebase sang TIM). TinyUSB giữ `CFG_TUSB_OS = OPT_OS_NONE`, vì `tud_task()` chỉ gọi từ task PLC. Phần thực thi FreeRTOS nằm ở Layer 0 `platforms/freertos/sx_os_freertos.c` (target `splc_platform_freertos` là OBJECT library, object được đưa thẳng vào file thực thi bằng `target_sources(... $<TARGET_OBJECTS:...>)` trong `simple_plc/CMakeLists.txt`; KHÔNG dùng static library vì phụ thuộc thứ tự link, xem #30); `components/os/sx_os.h` chỉ là hợp đồng Layer 1. Host phải cung cấp header FreeRTOS (CMake `-DSPLC_OS_INCLUDE_DIRS=...`, biến này giờ định nghĩa trong `platforms/freertos/CMakeLists.txt`) và `FreeRTOSConfig.h` có `INCLUDE_vTaskDelay`, `INCLUDE_xTaskGetSchedulerState`, `configSUPPORT_DYNAMIC_ALLOCATION` (mutex logger). Đã kiểm: gcc host + header FreeRTOS giả, 2 chế độ macro (biên dịch 6 file + chạy thử khóa logger, làm tròn tick); Đã build ARM cả 2 chế độ (xem #30); CHƯA chạy với FreeRTOS thật trên board. Không log từ ISR khi macro = 1.

30. **Hoàn thiện chế độ OS/bare-metal (2026-10-10) — sửa các chỗ #29 mô tả nhưng code chưa có hoặc sai.** Phát hiện khi đối chiếu code với #29: (a) `add_subdirectory(platforms/freertos)` trỏ vào thư mục không tồn tại (file nằm ở `platforms/stm32/stm32h5/freertos/`) nên CMake lỗi ngay lúc configure; đã `git mv` về `platforms/freertos/`; (b) `sx_delay_ms` nhánh OS rỗng, `sx_delay_s` luôn `HAL_Delay`; (c) `FREE_RTOS` của logger vẫn mặc định 0, mutex không bao giờ bật; (d) vòng chờ USB (`sx_usb_tiny_read/write`) và vòng bơm USB 500 ms lúc boot chưa gọi `sx_os_yield_wait()`; (e) `splc_platform_freertos` là STATIC library có `PUBLIC splc_components` nên CMake chèn lại `libsplc_components.a` SAU nó và `sx_os_yield_wait` không resolve (lỗi link chỉ lộ khi `SX_OS_USE_FREERTOS=1`). Đã làm: `stm32h5_time.c` (ngủ qua scheduler khi đang chạy, `sx_delay_s` chia lát 1 s tránh tràn `s*1000`); `logger.c` lấy `FREE_RTOS` từ `SX_OS_USE_FREERTOS` (bỏ default 0 trong `logger.h` để Layer U không include `app/`), khóa/mở mutex cân bằng, bỏ qua khóa khi mutex chưa tạo, `snprintf/vsnprintf` giới hạn buffer 4096, `log_print_hex` dùng chung khóa, `logger_init(.., NULL)` giữ writer cũ, writer mặc định `fputs` thay `printf(s)` (lỗi `%` trong log); `sx_usb_cdc.c` và `board_zigbee_io.c` gọi `sx_os_yield_wait()` TRƯỚC `tud_task()` + `usb_rx_task()` trong vòng chờ (byte đến trong lúc ngủ được chuyển ngay sang `rxQueue`; timeout vẫn đo bằng tick HAL nên tick RTOS chậm không kéo dài timeout; nên đặt `configTICK_RATE_HZ=1000`, tick 100 Hz thì mỗi lần ngủ 10 ms); `splc_platform_freertos` thành OBJECT library. **Đã verify:** build ARM thật (arm-none-eabi-gcc 13.2, preset Release) — macro 0: link OK, `.text+.data` = 70,5 KB (< `0x08036000` = 216 KB, đóng mục kiểm kích thước ở trên), ELF không chứa symbol RTOS nào; macro 1 với header FreeRTOS-Kernel thật + `FreeRTOSConfig.h` giả: mọi file của thư viện biên dịch không warning, chỉ còn undefined đúng các API kernel `xTaskGetSchedulerState`, `vTaskDelay`, `xQueueCreateMutex`, `xQueueSemaphoreTake`, `xQueueGenericSend` (host cung cấp). Logger test trên PC (gcc host, stub semphr): 2 chế độ, cắt chuỗi dài, hex 3000 byte, take/give cân bằng (4/4), mutex tạo 1 lần. **Chưa làm:** link + chạy với kernel FreeRTOS thật và task thật trên board (cần host project có FreeRTOS; HAL timebase phải chuyển sang TIM, xem #29), đo ảnh hưởng erase/ghi Flash lên task khác (HAL Flash chặn trong lúc erase sector; chỉ 1 task PLC nên không vi phạm khóa nhưng task khác bị trễ), kiểm PVD ISR không đụng API RTOS (đọc code: chỉ `sx_get_tick_ms()` = `HAL_GetTick()`).

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
- ~~Bước 8b: khối FB không được ghi trong lần deploy~~ — ĐÃ CHỐT (#22): khối không ghi thì DISABLED khi COMMIT. Rủi ro còn lại cho đội App: client khác chỉ COMMIT rule (không ghi FB) sẽ xoá cấu hình FB; và nếu Studio đọc lại FB giữa lúc ghi và lúc COMMIT sẽ thấy giá trị CŨ. **Chưa kiểm tra code App có đọc lại FB trước COMMIT không** (`FunctionBlockGateway.cs`, `RuleTableWriter.cs`).
- Đổi cấu hình counter (mode/PV) hiện KHÔNG đặt lại `CV` (0 cho CTU, PV cho
  CTD); `CV` do rule của App quản lý. Hỏi App nếu cần firmware khởi tạo.
- Quyết định #4 với `VREG_RETAIN`: hiện bản nháp bị BỎ, không ghi 0 vào giá trị
  live (ghi 0 sẽ phá dữ liệu đã commit). Xoá retain thật là việc của
  `CLEAR_RETAIN` / `FACTORY_RESET`.
- Quyết định #4 với phạm vi: hiện chỉ reset tag HOST ĐÃ GHI trong phiên (theo
  bitmap), không đụng tag do rule bật trước ENTER. Muốn mạnh hơn (đưa TẤT CẢ
  DO về 0) thì đổi sang quét theo kind, sửa 1 chỗ trong
  `diag_baseline_reset_dirty_tags()`.

- **#25 PVD ghi retain khẩn cấp (2026-10-09).** Nối PVD hoàn toàn bằng file của dự án, KHÔNG sửa file CubeMX (#11): `PVD_AVD_IRQHandler` nằm trong `stm32h5_pwd.c`; nếu sau này bật NVIC PVD trong CubeMX (sinh handler riêng) thì định nghĩa `SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX` để tránh trùng ký hiệu. Đăng ký callback = bật NVIC. Mọi thao tác Flash của vòng chính ngoài `plc_retain.c` phải bọc `retain_flash_op_begin/end()`. Bản ghi PVD chỉ ghi khi dữ liệu retain đổi so với record mới nhất; chống dội `RETAIN_EMERGENCY_MIN_INTERVAL_MS` = 5000. `retain_snapshot_write()` giờ chỉ dùng ở vòng chính (không còn là điểm vào cho ISR).

## 3. Kế hoạch các bước tiếp theo

### Bước 7 — RTC (`0x0810`, 4 reg) — XONG, ĐÃ VERIFY TRÊN BOARD (trừ reboot/mất điện)

Layout: `epoch_utc_s` (u32, High Word trước), `tz_offset_min` (i16),
`status_flags` (u16, RO, firmware tự tính — quyết định #10).

**Cấu hình CubeMX đã chốt (đừng đổi khi regenerate):** RTC clock source = LSI
(`RCC_RTCCLKSOURCE_LSI`), Activate Calendar, 24 h, **Async 127 / Sync 249**
(1 Hz với LSI 32 kHz; 127/255 chỉ đúng với LSE, dùng nhầm thì giờ chậm ~2,3 %),
alarm/wakeup/timestamp tắt. Linker `--wrap=HAL_RTC_SetTime/SetDate` (đã có trong
`simple_plc/CMakeLists.txt`) để `MX_RTC_Init()` không xoá giờ mỗi lần boot;
thiếu thì link lỗi `undefined reference to __real_HAL_RTC_SetTime` (cố ý).

**Đã làm:**
- Layer U/1/0: `splc_epoch`, `sx_rtc.h`, `stm32h5_rtc` (7a, như trước).
- Layer 2: `rule_scan(now_ms, now_hhmm)` và `rule_state_machine_step(..., now_hhmm)`;
  `RULE_HHMM_INVALID` (`0xFFFFFFFF`) = không có giờ hợp lệ; Time Window không đi
  qua bước compare theo `trigger_tag`; `prev_value` của rule Time Window lưu
  "điều kiện cửa sổ đúng ở scan trước" (0/1) để bắt sườn lên mốc phút.
  `trigger_timing_ok()` trả false khi `now_hhmm == RULE_HHMM_INVALID`.
- `core/plc_device/plc_device.h`: `SPLC_RtcFlags`, `SPLC_RtcCaps`.
- Layer 3: `services/plc_rtc/plc_rtc.{h,c}` (đọc/ghi khối, validate all-or-nothing,
  `plc_rtc_get_local_hhmm()`), nối vào `plc_modbus_cfg.c` (bảng đọc `0x0810..0x0813`;
  ghi FC16 xử lý trong `cb_write_multiple_registers`, ánh xạ kết quả → exception).
- Layer 4: `plc_engine.c` tính `hhmm` mỗi scan, truyền vào `rule_scan()`.
- Board: `board_zigbee_io.c` đặt `g_rtc_caps.hw_present = false`.
- CMake: `services/CMakeLists.txt` thêm `plc_rtc.c` + include `utils/epoch`.

**Đã verify trên PC (gcc host):** Layer 2 (12 ca A–J: khung trong ngày, qua nửa
đêm, mốc phút 1 lần/re-arm, giờ không hợp lệ, đồng bộ giữa phút đích, compare_op
khác EQ bị bỏ qua, EQ với Lo≠Hi là khung, guard, reset runtime, INTERVAL và
ON_RISE không đổi); `plc_rtc` (ghi/đọc, biên 2000/2099, tz biên, lỗi phần cứng
không đổi tz, `status_flags` bỏ qua giá trị Host); đầu-cuối qua nanoMODBUS thật
với transport giả (exception `0x02`/`0x03` đúng, các block khác không bị ảnh
hưởng). `plc_engine.c` qua `-fsyntax-only` với HAL thật. `board_zigbee_io.c`
chưa kiểm được trên PC (thiếu submodule tinyusb) — chỉ thêm include + 1 dòng gán.

**Đã verify trên board:** build ARM OK với `--wrap`; `test_rtc.py COM14` ALL PASS
(bước 1–8 trừ `--reboot`). LSI + prescaler 127/249 cho sai số -0.1 % trong 5 s.
**Còn lại:** `python test_rtc.py COM14 --reboot` (giờ + SYNCED sống sót qua
REBOOT, tz về 0, Time Window im lặng đến khi Host ghi lại giờ); test mất điện
thật bằng tay (xem docstring `test_rtc.py`: `flags` phải về 0); báo đội App sinh
rule "đúng giờ" dạng `EQ, Lo=Hi` (quyết định #11).

**Phát hiện phụ — ĐÃ SỬA ở Bước 8:** FC16 dài hơn 64 byte (vd. 2 rule = 73
byte vào `0x9010`) từng KHÔNG được trả lời (pymodbus: "No response received
after 3 retries"). Nguyên nhân (đọc từ code, rồi verify bằng `test_fb.py
--probe-64` sau khi sửa): khung 73 byte đến qua 2 gói USB (64 + 9).
`sx_usb_tiny_read()` (1) đếm timeout bằng số vòng lặp chứ không phải ms thật
(5 ms thực chất ~5 vòng, cỡ micro giây), và (2) trong lúc chờ chỉ gọi
`tud_task()`; byte chỉ được chuyển từ FIFO TinyUSB sang `rxQueue` trong
`sx_usb_tiny_process()` (10 ms/lần). nanoMODBUS thấy thiếu byte, huỷ 64 byte
đầu, phần đuôi bị đọc nhầm thành yêu cầu mới. Sửa: đo bằng `HAL_GetTick()` và gọi
`usb_rx_task()` sau mỗi `tud_task()` trong vòng chờ; timeout 0 (kiểm tra byte
đầu mỗi chu kỳ) giữ nguyên "không chờ". Worst case khi khung bị cắt giữa chừng:
trễ ~5 ms một lần. App vẫn đang chia tối đa 3 khối FB mỗi lệnh FC16 (57 byte);
giờ có thể bỏ giới hạn này. `test_rtc.py` vẫn mỗi rule một lệnh (chưa đổi lại).

### Bước 8 — FB Timer / Counter (`0x0B00` / `0x0B40`)

**8a — XONG, ĐÃ VERIFY TRÊN BOARD** (`test_fb.py COM14` và `--probe-64`: ALL
PASS). Quyết định #13..#20 ở mục 2. Layout (Structs 3.6/3.7), mỗi record 8 reg,
Timer `i` tại `0x0B00 + 8i`, Counter `i` tại `0x0B40 + 8i`:

| Offset | Timer | Counter | Chủ sở hữu |
|---|---|---|---|
| +0 | `status_bits` | `status_bits` (bit 3 = Q) | firmware |
| +1 | `mode` | `mode` | Host |
| +2..+3 | `pt_ms` | `preset_value` (int32) | Host |
| +4..+5 | `et_ms` | `current_value` = giá trị tag CV (SỬA, xem #15) | firmware |
| +6 | reserved | `retain_tag_index` = tag chứa CV (#17) | Host (Counter) |
| +7 | reserved | reserved | — |

Module: `services/plc_fb/plc_fb.{h,c}` (Layer 3, không include nanomodbus).
`plc_fb_init()` gọi một lần trong `plc_engine_init()` sau
`tag_table_load_from_flash()`. `plc_fb_write()` làm việc trên bản sao, validate
hết rồi mới áp dụng (all-or-nothing); `plc_modbus_cfg.c` ánh xạ kết quả →
exception `0x02`/`0x03` giống khối RTC. Đã verify trên PC (gcc host với
`plc_tag.c` thật) rồi trên board.

**Đã verify trên board (`test_fb.py`):** ghi cả khối Timer/Counter như App
(rác trong field firmware sở hữu bị bỏ qua), High Word trước, PV âm,
`retain_tag_index`; `CV` theo tag COUNTER qua DIAG, `Q` CTU/CTD, `CV` âm; rule
`INC_COUNTER` làm `CV` tăng trên khối FB và `Q` bật tại PV; các từ chối
(`0x03` mode sai / retain không phải VREG_RETAIN / ngoài dải / trùng, `0x02`
vượt hết khối / dưới `0x0B00`), all-or-nothing trên yêu cầu 2 khối; FC06 vào
một thanh ghi cấu hình; deploy kiểu App 8+8 khối chia 3,3,2; khung 4 khối
(73 byte) được trả lời. **Chưa verify:** nội dung khung 4 khối có được áp dụng
đúng không (bản `test_fb.py` đã chạy chưa có bước đọc lại này; bản mới có
`check` "the 4-block frame was applied" — chạy lại `--probe-64`).

**Phát hiện khi test (cho team App):**
- Đọc `0x0B00..0x0B7F` bằng MỘT FC03 128 thanh ghi bị từ chối (Modbus tối đa
  125 mỗi lệnh; nanoMODBUS kiểm `quantity > 125`). `FunctionBlockGateway.cs`
  đang đọc 128 trong một lệnh và nuốt lỗi → màn hình FB trống. App phải
  tách (vd. 64 + 64; board trả lời 64 reg ổn). **Chưa xác nhận App đã sửa.**
- `RuleCompiler.cs` chỉ sinh macro rule khi `WireProfile < 2` (mục 2, #13).

**Còn lại:**
- **8b — XONG, VERIFY TRÊN PC VÀ TRÊN BOARD (`test_fb.py COM14 --reboot` ALL PASS: nháp/COMMIT, DISABLED, từ chối, trùng retain trong nháp, COMMIT lỗi xoá nháp, CLEAR_RULES/FACTORY_RESET, FB sống sót REBOOT rồi mất sau CLEAR_RULES + REBOOT)** (#19, #21..#24). Verify trên PC (gcc host + ASan/UBSan, firmware thật Layer 2/3 + nanoMODBUS thật, Flash giả mô phỏng mất điện giữa chừng một lần ghi/xoá): (1) ghi FB là nháp, đọc vẫn thấy cũ, COMMIT mới thấy; (2) khối không ghi thành DISABLED; (3) trùng retain chỉ tính trong nháp; (4) COMMIT sai CRC xoá nháp, Deploy sau không dính khối cũ; sai magic không xoá nháp; (5) `CLEAR_RULES`/`FACTORY_RESET` xoá FB và nháp; (6) lưu/nạp Flash khứ hồi, bản ghi trước-8b vẫn nạp, hỏng đoạn FB bị CRC bắt rồi rơi về B; (7) quét mất điện tại MỌI đơn vị ghi/xoá của một lần save (41 đơn vị): sau khởi động lại luôn là đúng bản cũ hoặc đúng bản mới (25/19), KHÔNG lần nào lẫn. 75 kiểm tra PASS. `test_fb.py` bản mới chạy với firmware PC qua pty (REBOOT mô phỏng mất sạch RAM, giữ Flash), kèm `--reboot --probe-64`: ALL PASS. **Ý nghĩa đã đổi so với 8a:** `test_fb.py` cũ kiểm "ghi xong đọc ngay thấy giá trị mới" không còn đúng; script mới ghi → COMMIT → đọc. **Còn lại:** `--probe-64` trên board, mất điện thật giữa lúc lưu, kiểm `.text+.data` ARM.
- **8c — Counter retain (SỬA 2026-10-08): hết việc firmware cho Counter, chỉ còn
  PVD.** Xem #15, #17, #18. Phần đã làm trong phiên này (ĐÃ build ARM và verify trên board:
  `test_fb.py COM14`, `--reboot`, `--probe-64` đều ALL PASS; bước 7 trên board: CV tăng
  33 → 84 trong 0,5 s, Q bật ở CV=85 với PV=50, `CLEAR_RETAIN` đưa CV về 0; bước 12:
  cấu hình FB sống sót REBOOT, mất sau CLEAR_RULES + REBOOT; probe: khung 73 byte được
  trả lời, bản `--probe-64` này chỉ xác nhận "có trả lời" và link còn sống, không đọc lại
  nội dung 4 khối. Chưa verify: số đếm của tag VREG_RETAIN sống sót REBOOT thật): `plc_fb.c/.h` đọc CV/Q từ tag CV do App chọn,
  bỏ `counter_exists()`, validate theo kind tag; `test_fb.py` viết lại các bước
  5, 6, 7, 8, 9 theo hợp đồng mới (CLEAR_RETAIN đưa CV retain về 0 ở bước 7).
  Verify trên PC: harness gcc host + ASan/UBSan với `plc_tag.c` và `plc_fb.c` thật,
  30/30 kiểm tra PASS (mọi kind tag CV, Q CTU/CTD, không có tag CV, từ chối
  DI/DO/ngoài dải, trùng tag trong nháp, board ít COUNTER, khứ hồi Flash image,
  bản ghi cũ trước đổi vẫn nạp được vì layout 112 byte không đổi).
  **8c PVD — ĐÃ CODE (2026-10-09). Trên board: `test_retain.py COM14` ALL PASS (32 tag
  retain sống sót REBOOT; 130 COMMIT_RETAIN liên tiếp qua ranh giới sector và xoay vòng, REBOOT
  giữa chừng và cuối đều đọc đúng giá trị mới nhất; CLEAR_RETAIN sau xoay vòng; lưu rule xen kẽ
  commit retain, cả hai thứ tự). Test này KHÔNG phân biệt firmware cũ/mới nếu chip không bỏ qua
  erase khi Flash khoá, nên chưa chứng minh được lỗi erase-trước-unlock cũ có thật trên chip.
  CHƯA test: `test_retain.py --periodic` (chu kỳ 5 phút, ~6 phút), đường PVD thật (chưa có
  phần cứng), kiểm `.text+.data` ARM, mất điện thật giữa lúc ghi.** (xem #25).
  **`--periodic` đã chạy trên board: ALL PASS.** Rule đếm RETAIN0 ~100,5 lượt/giây, không commit;
  sau 330 s REBOOT khôi phục 29798 (live trước REBOOT là 33329), tức bản ghi tự động nằm ở khoảng
  300 s sau khởi động, đúng `RETAIN_SNAPSHOT_PERIOD_MS`. Vậy nhánh "chu kỳ 5 phút ghi khi dữ
  liệu đổi" đã được kiểm; nhánh "không đổi thì không ghi" vẫn không quan sát được qua Modbus.
  Phát hiện khi làm: chuỗi PVD trước đó KHÔNG được nối (comment cũ trong `stm32h5_pwd.c` ghi
  "đã làm" là sai): `stm32h5_pwd.c` không có trong `SPLC_PLATFORM_STM32H5_SRC`,
  `components/pwd` không có trong include path, không có `PVD_AVD_IRQHandler` (vector rơi vào
  `Default_Handler`) và NVIC chưa bật (`HAL_MspInit` chỉ cấu hình PVD level 4 + IT falling +
  EnablePVD). Đã làm: (1) `sx_flash_write_quiet()` (không log, trả bool) + dùng chung vòng
  program với `sx_flash_write()` (hành vi cũ giữ nguyên); (2) `stm32h5_pwd.c`: định nghĩa
  `PVD_AVD_IRQHandler`, đăng ký callback = bật NVIC (ưu tiên 0), NULL = tắt; nối vào CMake;
  (3) `plc_retain`: `retain_emergency_snapshot()` (ISR: không erase, không log, hoãn nếu vòng
  chính đang thao tác Flash, bỏ qua nếu dữ liệu không đổi, chống dội 5 s), cờ bảo vệ
  `retain_flash_op_begin/end()` (cũng bọc `rule_flash_write_sector()`), erase trước sector kế
  tiếp ngay khi ghi hết sector, khi khởi động bỏ qua slot ghi dở và erase sector kế tiếp nếu
  chưa trắng, chu kỳ 5 phút CHỈ ghi khi dữ liệu đổi (trước đây ghi vô điều kiện, lệch thiết kế
  đã ghi); (4) `plc_engine_init()` đăng ký callback sau `retain_store_restore()`, tắt được bằng
  `-DPLC_PVD_EMERGENCY_SAVE_ENABLE=0`. **Sửa lỗi có sẵn:** `retain_snapshot_write()` cũ gọi
  `sx_flash_erase()` TRƯỚC `sx_flash_unlock()` (khi xoay vòng sang sector đã dùng, tức từ lần ghi
  thứ 118, khoảng 9,75 giờ ở chu kỳ 5 phút); nếu phần cứng bỏ qua erase khi Flash khoá thì các
  lần ghi sau đều lỗi. Chạy lại code cũ trong harness (giả định erase bị bỏ qua khi khoá): sau 130
  lần ghi, khởi động lại đọc ra 117 thay vì 130. Code mới erase trong cặp unlock/lock.
  Verify trên PC (gcc host + ASan/UBSan, `plc_retain.c` thật, Flash giả có khoá/erase/lỗi
  program/mất điện, nanoMODBUS thật cho CRC): 34 kiểm tra PASS: chỉ ghi khi đổi; ISR hoãn khi
  bận rồi `retain_service()` ghi; ISR không gọi erase/log; chống dội; pre-erase + xoay vòng 200
  lần ghi không lỗi; ISR gặp sector đầy thì hoãn; mất điện tại mọi điểm của lần ghi ISR (30 ca,
  gồm ghi dở nửa quad-word): luôn đúng bản cũ hoặc đúng bản mới (26/4), lần ghi kế tiếp luôn được;
  mất điện sau ghi đầy sector, trước pre-erase: khởi động lại erase sector kế tiếp.
  **Chưa làm / chưa biết:** build ARM (kiểm `.text+.data` và stack ISR ~0,6 KB); thời gian
  program 1 record và thời gian giữ điện sau PVD (cần phần cứng + oscilloscope); đối chiếu
  `PWR_PVDLEVEL_4` với điện áp tối thiểu để program Flash trong datasheet; xác nhận phần cứng
  thật có bỏ qua erase khi khoá hay không. Chưa ghi snapshot ngay trước REBOOT (có trong thiết
  kế cũ, chưa nằm trong phạm vi lần này).
- **8d — Timer chạy thật** (#20): cần App chốt cách nối `IN`/`RESET`/`Q`.
  Hai phương án đã nêu: (a) quy ước cố định VFLAG (Timer `i`: `IN`=VFLAG[i],
  `RESET`=VFLAG[8+i], `Q`=VFLAG[16+i]) và RuleCompiler sinh rule sao chép;
  (b) dùng 2 thanh ghi `reserved` của Timer để App ghi tag nguồn (mở rộng
  wire, cần sửa spec cả hai phía). Counter không cần vì đếm qua `INC_COUNTER`.
  Nếu App muốn `ET`/`IN`/`RUNNING` thật trên màn hình, đây là chỗ bắt buộc.

### Việc tồn đọng, giải quyết tiện thể khi đụng tới file liên quan

- **App + spec cho CV tag (không gấp):** xem #17 (RuleCompiler.cs dòng ~260, giá trị dự
  phòng 84 ở Studio, Structs 3.7 / Wire Contract 9.3).
- **(Đã làm bằng `test_retain.py`: giá trị VREG_RETAIN commit qua DIAG sống sót REBOOT, và số đếm do rule `INC_COUNTER` được chu kỳ 5 phút ghi và khôi phục qua REBOOT.)** **Test số đếm VREG_RETAIN sống sót REBOOT thật** (đếm trên VREG_RETAIN, `COMMIT_RETAIN`
  trong DIAG, REBOOT, đọc lại CV): `test_fb.py --reboot` hiện chỉ kiểm cấu hình FB qua
  reboot, chưa kiểm giá trị đếm. `--probe-64` bản hiện tại chỉ kiểm "có trả lời", không
  đọc lại nội dung 4 khối.
- Validate `guard_tag` / `trigger_tag` / `action_tag` nằm trong `0..MAX_TAGS-1`
  (hoặc `GUARD_TAG_NONE`) khi nạp rule — hiện index sai không crash nhưng rule
  bị vô hiệu hoá âm thầm.
- PVD → ghi Retain khẩn cấp: đã code (xem Bước 8c), còn chờ build ARM và test trên board có phần cứng giữ điện.
- `modbus_usb_write()` bỏ qua `timeout_ms`, có thể vượt ngân sách scan 10 ms
  dưới tải nặng (đo được 83 ms với 100 rule).
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM` chưa implement,
  ngoài phạm vi V2.0.
- Một lỗi Flash KHÔNG thể lập trình được trong khi erase vẫn chạy: bước đầu của
  `plc_rule_flash_save()` (copy A→B) đã huỷ bản cũ trước khi biết lỗi. Có từ
  trước V2.0, áp dụng cho cả COMMIT rule thường và `CLEAR_RULES`.
- `docs/architecture.md` cần được soát lại cho khớp V2.0 và code hiện tại.
  Structs v2.0 mục 3.6/3.7 còn ghi khối FB là chỉ đọc (App đã sửa tài liệu
  của họ thành R/W); `Wire_Contract` ghi `MODE/PT/PV/CV/RETAIN_TAG_INDEX` R/W.
- **Chưa điều tra — mất log sau khi nạp firmware mới:** lần nạp bản Bước 8a
  KHÔNG có dòng log nào (không thấy cả "Zigbee-IO board init start"); erase
  full chip rồi nạp lại thì log xuất hiện. Diff của bản đó không đụng Flash
  hay log. Giả thuyết (chưa kiểm chứng): (1) trong `plc_engine_init()` ba
  lệnh đọc Flash (`rule_table_load_from_flash`, `plc_rule_flash_load`,
  `retain_store_restore`) chạy TRƯỚC `board_init()` (nơi `logger_init`), nên
  treo/fault ở đó thì không có log nào; dữ liệu cũ trong Flash có thể kích
  hoạt; (2) firmware lớn lấn vào vùng dữ liệu `0x08036000..0x0803FFFF`
  (retain 3 sector + Rule Table A/B): chip STM32H523CC chỉ có 256 KB nhưng
  `STM32H523xx_FLASH.ld` khai báo `FLASH LENGTH = 512K` nên linker không bắt.
  Cách xác định: `arm-none-eabi-size` / file `.map` (kết thúc `.text+.data`
  phải < `0x08036000`); tái hiện: nạp KHÔNG erase rồi xoá riêng sector 27..31.
  Nếu đúng (2), cân nhắc đặt `LENGTH = 216K` (file `.ld` do CubeMX sinh —
  hỏi người dùng trước khi sửa); nếu đúng (1), các hàm load Flash cần chịu
  được dữ liệu rác (kiểm CRC, không fault).
- **Phát hiện khi test 8b (có từ trước, CHƯA sửa, chờ bạn quyết):** `plc_rule_flash_save()` coi "đọc lại A thấy CRC hợp lệ" là thành công. Nếu Flash không nhận cả erase lẫn program (khoá/bảo vệ ghi), A giữ nguyên BẢN CŨ còn hợp lệ nên hàm trả `true` dù không lưu gì; mô phỏng: `plc_clear_rules()` với Flash "chết" báo thành công và không khôi phục RAM. Đề xuất: sau khi đọc lại, kiểm thêm `seq_num` == `new_seq_num`. Chưa có bằng chứng xảy ra trên chip (lỗi thật thường để dữ liệu rách, CRC bắt được).
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

12. **Đối chiếu câu trả lời của team App với code App.** Trả lời "App/Rule
    Engine sẽ lo" cho cách nối `IN`/`Q` hoá ra không khớp: `RuleCompiler.cs`
    không sinh rule nào cho Timer/Counter khi `WireProfile >= 2`. Đọc repo
    `paa` trước khi thiết kế phần dựa vào hành vi của App.
13. **Timeout đếm theo vòng lặp không phải timeout.** Vòng chờ không có độ
    trễ mà `time++` mỗi lượt thì "5 ms" chỉ là 5 vòng. Khi chờ dữ liệu, đo
    bằng tick thật và nhớ chuyển dữ liệu từ buffer của stack (TinyUSB) sang
    queue của mình ngay trong vòng chờ.
14. **Giới hạn Modbus:** FC03 tối đa 125 thanh ghi, FC16 tối đa 123; cả hai
    chưa tính giới hạn gói USB 64 byte. Thiết kế đọc/ghi một khối lớn phải
    chia nhỏ.
15. **Lỗi trước `logger_init` không có log.** Mọi thứ chạy trước `board_init()`
    (đọc Flash trong `plc_engine_init()`) treo thì màn hình log trống; đừng
    kết luận "code không chạy" mà chưa kiểm tra những bước này.
16. **Script test phải bắt ngoại lệ của pymodbus** (`ModbusException`) ở các
    bước thăm dò mà board có thể không trả lời, và vẫn chạy bước dọn dẹp.

19. **Đừng suy ra ngữ nghĩa field từ tên field hay từ một phía.** "Counter `i` = tag `COUNTER[i]`" được phiên trước giả định mà không xem UI/`RuleCompiler.cs` của App; thực tế người dùng chọn tag CV bất kỳ (VREG_RETAIN mặc định). Trước khi thiết kế phần dựa vào hành vi App, đọc UI và compiler của App (bài học 4, 12), và hỏi người dùng.

17. **Việc chưa giao cho người dùng thì mất khi phiên kết thúc** (container không giữ): hai phiên 8b đầu hết token khi code còn trong container, phải làm lại. Sau mỗi mốc, present nguyên file ngay; đừng để dồn đến cuối.
18. **Flash giả phải mô phỏng đúng "mất điện":** sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA, chỉ lệnh đang chạy mới "rách". Harness đầu cho mỗi erase vẫn xoá nửa sector nên báo lỗi giả "mất cả A lẫn B". Kết quả lạ thì kiểm harness trước khi nghi firmware.

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
- Với mỗi bước mới: code → người dùng build ARM → chạy script test trên board
  → cập nhật file này.
- **Chỉ giao code của project + script test Python (`test_*.py`, chạy với board
  qua Modbus).** KHÔNG giao harness C chạy trên PC (Flash giả, shim HAL...): người
  dùng thấy khó kiểm soát vì project chưa hoàn thiện (chốt 2026-10-09). Việc tự
  kiểm tra nội bộ của Claude (ví dụ gcc host) vẫn được làm nhưng không đưa vào
  gói giao, và kết quả phải nói rõ là chạy trên mô hình giả. Nếu một tính năng
  không test được trên board (ví dụ PVD khi chưa có phần cứng giữ điện), nói rõ
  là chưa test được thay vì đưa harness thay thế.

## 6. Verify & test

**Script test trên board** (cần `pip install pymodbus pyserial` đúng
interpreter — dùng `python -m pip`, máy dev có nhiều Python do ESP-IDF):

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | descriptor, nạp rule, commit, live watch |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, Rule Engine thực sự dừng, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD, baseline reset trước REBOOT |
| `test_rtc.py COM14 [--reboot]` | Bước 7: khối `0x0810`, `status_flags` RO, từ chối ghi sai (`0x02`/`0x03`), tốc độ đồng hồ, Time Window (mốc phút, khung, qua nửa đêm), giờ sống sót qua REBOOT. **Ghi Flash, xoá rule, đặt giờ board.** Đã chạy trên board: ALL PASS (chưa chạy `--reboot`). |
| `test_fb.py COM14 [--reboot] [--probe-64]` | Bước 8a + 8b: khối FB `0x0B00..0x0B7F`. Ghi FB vào nháp rồi COMMIT mới đọc thấy; nháp vô hình trước COMMIT; khối không ghi thành unused; từ chối (`0x03`/`0x02`), all-or-nothing, trùng retain chỉ trong nháp; FC06 giữ field còn lại; deploy 3,3,2; COMMIT lỗi xoá nháp; `CLEAR_RULES`/`FACTORY_RESET` xoá FB; `CV`/`Q` theo tag CV do App chọn (VREG/VFLAG/COUNTER qua DIAG), rule `INC_COUNTER` trên tag VREG_RETAIN + `CLEAR_RETAIN`. `--reboot`: FB sống sót qua REBOOT cùng rule, mất sau CLEAR_RULES + REBOOT. `--probe-64`: FC16 4 khối (73 byte). **Ghi Flash (rule + FB), ép tag COUNTER, cuối cùng `CLEAR_RULES`.** Đã chạy trên board (`--reboot`): ALL PASS; `--probe-64` bản mới chưa chạy trên board. |
| `test_retain.py COM14 [--commits N] [--periodic]` | VREG_RETAIN qua REBOOT: 32 tag dương/âm, N `COMMIT_RETAIN` liên tiếp (mặc định 130 > vòng 117 record: qua ranh giới sector + xoay vòng/erase), REBOOT giữa chừng, `CLEAR_RETAIN` sau xoay vòng, lưu rule xen kẽ commit retain. `--periodic` (~6 phút): rule đếm tăng RETAIN0 không commit, sau 5 phút REBOOT phải khôi phục giá trị khác 0. KHÔNG kiểm được PVD (chưa có phần cứng) và "chu kỳ không đổi thì không ghi" (không có thanh ghi cho thấy). **Ghi Flash, xoá rule + retain, REBOOT nhiều lần.** Đã chạy trên board: ALL PASS (cả bản mặc định và `--periodic --commits 0`). |
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