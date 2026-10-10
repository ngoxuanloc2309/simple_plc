# SimplePLC — Handoff

> Mục đích: cho phiên làm việc sau (Claude khác hoặc chính bạn) nắm "đang ở
> đâu, làm gì tiếp" mà không đọc lại lịch sử chat. Đọc SAU `Readme.md` và
> `docs/architecture.md`. File này chỉ ghi: trạng thái hiện tại, quyết định đã
> chốt, việc còn lại, bài học, quy trình.
>
> **Quy tắc vàng:** trước khi tin điều gì dưới đây, chạy
> `git pull && git log --oneline -10`. Có commit mới hơn thì tin code hơn file này.

**Nguồn tài liệu (V2.0 là strict superset của V1.9).** Khi mâu thuẫn về HÀNH VI
thì Wire Contract thắng; về struct/memory map thì Structs thắng:
1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md`
2. `docs/SimplePLC_Wire_Contract_V2_Draft.md`
3. `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` — chỉ còn đúng cho LOGIC rule
   (Trigger→Compare→Dwell→Guard→Action); struct/CRC/Time Window/retain đã bị ghi đè.

**Repo App:** `ngoxuanloc2309/paa` (.NET, SimplePLC.Studio). Bug xuyên App+firmware
phải đọc CẢ HAI repo (`RuleCompiler.cs`, `RuleTableWriter.cs`,
`FunctionBlockGateway.cs`). Đối chiếu câu trả lời của team App với code App.

## 0. Trạng thái hiện tại (nhánh `board_dev_add_select_os`)

- Board: **Zigbee-IO SKU** (`board/board_device/board_zigbee_io.c`): 4 DI / 4 DO /
  0 AI / 32 VFLAG / 32 VREG / 32 VREG_RETAIN / 8 COUNTER = 112 tag (DENSE:
  DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71, RETAIN 72-103, COUNTER 104-111).
  MCU STM32H523CCU6 (256 KB Flash thật).
- **Wire Profile V2.0 đã xong và verify trên board:** Diag Control `0x0A20`, ghi tag
  trong diag, retain draft/COMMIT, 3 lệnh xoá + REBOOT, RTC `0x0810` + Time Window,
  khối FB `0x0B00..0x0B7F` (Counter CV/Q, Timer chạy thật qua `rule_ref`, cấu hình FB
  lưu Flash chung bản ghi Rule Table), retain + PVD khẩn cấp (phần retain/Flash
  verify trên board; **đường PVD thật chưa test**, chưa có phần cứng giữ điện).
- **Việc của nhánh này — chọn OS + file cấu hình kiểu lwIP (xong, build ARM OK, CHƯA
  chạy trên board):**
  - `config/splc_opt.h` (mặc định + kiểm tra giá trị) và `splc_config/splcopts.h`
    (file của sản phẩm). Chi tiết mục 2.
  - `SX_OS_USE_FREERTOS` (0 = bare-metal, 1 = FreeRTOS một task) — mục 2.
  - Build ARM Release (arm-none-eabi-gcc 13): `.text+.data` ≈ 69 KB (< `0x08036000`
    = 216 KB), RAM 37,4 KB. `MAX_RULES=50` build được (RAM 28,6 KB). Bản
    `SX_OS_USE_FREERTOS=1` với FreeRTOS-Kernel thật + `FreeRTOSConfig.h` giả: mọi file
    của thư viện biên dịch không warning, link chỉ thiếu đúng 5 API kernel
    (`vTaskDelay`, `xTaskGetSchedulerState`, `xQueueCreateMutex`,
    `xQueueSemaphoreTake`, `xQueueGenericSend`) — host cung cấp.
- **Chưa verify (không chặn việc tiếp theo):**
  - Firmware mới (config + OS) chưa nạp/chạy lại trên board; chạy lại `test_plc.py`,
    `test_fb.py`, `test_retain.py` để chắc không hồi quy.
  - Link + chạy với kernel FreeRTOS thật và task thật; HAL timebase phải chuyển sang
    TIM; đo độ trễ task khác khi erase/ghi Flash (HAL Flash chặn lúc erase).
  - PVD thật, mất điện thật giữa lúc ghi Flash, `test_rtc.py --reboot`, tz/giờ qua
    mất điện.

## 1. Cơ chế cần biết (đã làm, không sửa lại trừ khi có lý do mới)

- **Scan loop** (`app/plc_app/plc_engine.c`, 10 ms): `diag_tick → input_scan →
  [nếu engine không bị diag treo: rule_scan + plc_fb_scan] → output_scan →
  modbus_config_service → retain_service → ghi scan_time → plc_system_cmd_service`
  (cuối cùng, vì có thể reset).
- **Rule Table lưu Flash A/B** (`services/plc_rule_flash/`): copy A→B, erase+ghi A,
  đọc lại kiểm CRC **và seq_num mới**, lỗi thì B→A. Mất điện luôn còn ít nhất 1 bản
  hợp lệ. Đoạn FB (128 byte; bản cũ 112 byte vẫn nạp được) nằm TRONG bản ghi:
  bit 15 `rule_count` = có FB, bit 14 = FB 128 byte, bit 0..13 = số rule; một CRC phủ
  tất cả. Mọi code đọc trường này phải tách cờ trước.
- **CRC Rule/Retain tính trên byte WIRE** (`rule_table_wire_crc16()`), không phải byte
  RAM struct. `nmbs_crc_calc()` trả kết quả hoán byte (chỉ cho RTU framing).
- **Retain** (`services/plc_retain/`): log xoay vòng 3 sector, record 208 byte.
  Lưu khi: COMMIT_RETAIN, chu kỳ `RETAIN_SNAPSHOT_PERIOD_MS` (CHỈ khi dữ liệu đổi),
  PVD khẩn cấp (`retain_emergency_snapshot()` trong ISR: không erase, không log,
  hoãn nếu vòng chính đang thao tác Flash, chống dội). Mọi thao tác Flash ngoài
  `plc_retain.c` phải bọc `retain_flash_op_begin/end()`. Sector kế được erase TRƯỚC.
- **PVD** nối hoàn toàn bằng file dự án (`stm32h5_pwd.c` có `PVD_AVD_IRQHandler`);
  đăng ký callback = bật NVIC. Nếu bật NVIC PVD trong CubeMX thì định nghĩa
  `SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX`. Tắt bằng `PLC_PVD_EMERGENCY_SAVE_ENABLE 0`.
- **System command:** `SYSTEM_COMMAND` luôn thực thi ngay (bất kể diag/dirty).
  REBOOT trì hoãn `PLC_REBOOT_DELAY_MS` (300 ms) để phản hồi USB kịp ra — ĐỪNG bỏ.
  CLEAR_RULES/CLEAR_RETAIN/FACTORY_RESET ghi một bản ghi RỖNG hợp lệ qua đường lưu
  đã verify (không erase thô); FACTORY_RESET = CLEAR_RETAIN + CLEAR_RULES, không
  reboot, không đụng VREG thường/COUNTER/tag layout. Phiên diag vẫn sống sau lệnh xoá.
- **USB/Modbus:** FC16 tối đa 123 reg, FC03 tối đa 125; gói USB FS 64 byte nên App
  chia 1 rule (41 byte)/lệnh. `sx_usb_tiny_read()` đo timeout bằng tick thật và bơm
  byte FIFO TinyUSB → `rxQueue` trong lúc chờ (khung > 64 byte được trả lời).
- **Multi-board:** `SPLC_TagLayout` truyền qua tham số; đổi board = 1 file
  `board_<sku>.c` + `-DSPLC_BOARD_SKU`. `MAX_TAGS` ở `board/board_tag_define.h`
  (authoritative); các macro `TAG_DI0...` trong đó chỉ là bảng tham chiếu, KHÔNG board
  .c nào include.

## 2. Hệ thống cấu hình & chế độ OS (nhánh này)

**Cấu hình kiểu lwIP.** Người dùng thư viện chỉ sửa MỘT file của sản phẩm:
- `simple_plc/config/splc_opt.h`: của thư viện, mọi option bọc `#ifndef` + khoảng hợp
  lệ (`#error` nếu sai). KHÔNG sửa để cấu hình sản phẩm. Nó `#include "splcopts.h"`.
- `splc_config/splcopts.h` (gốc repo, ngoài `simple_plc/`): của sản phẩm. Mẫu trống:
  `config/splcopts_template.h`. CMake: `-DSPLC_OPTS_DIR=<thư mục>` (mặc định
  `${CMAKE_SOURCE_DIR}/splc_config`; thiếu file → `FATAL_ERROR`).
- Option hiện có: `SPLC_PLATFORM`, `SPLC_BOARD` (phải khớp `SPLC_BOARD_SKU` của CMake,
  board .c kiểm bằng `#error`), `SX_OS_USE_FREERTOS`, `PLC_SCAN_INTERVAL_MS`,
  `MAX_RULES` (chỉ GIẢM, 1..100), `PLC_REBOOT_DELAY_MS`, `RETAIN_SNAPSHOT_PERIOD_MS`,
  `PLC_PVD_EMERGENCY_SAVE_ENABLE`, `RETAIN_EMERGENCY_MIN_INTERVAL_MS`,
  `SPLC_MODBUS_UNIT_ID`, `SPLC_USB_RX/TX_BUF_SIZE`, `SPLC_LOG_LEVEL`,
  `SPLC_LOG_BUFFER_SIZE`. Thêm option: thêm vào `splc_opt.h` (mặc định + check) và
  template.
- KHÔNG phải option: hằng wire (địa chỉ thanh ghi, kích thước struct, lease diag),
  layout Flash chip (`splc_flash_define.h`), tag layout board.
- `app/sx_platform_config.h`, `board/board_config.h`, `app/sx_os_config.h` giờ chỉ
  SUY RA cờ (`STM32H5_PLATFORM`, `BOARD_*`, `SX_NO_OS`) từ option; đừng định nghĩa tay.
- CMake: `include_directories()` phạm vi thư mục (mọi layer thấy `splc_opt.h`) +
  INTERFACE target `splc_config` link vào executable (main.c).
- Override bằng `-D` trên dòng lệnh chỉ có tác dụng khi `splcopts.h` không định nghĩa
  trùng option đó (hoặc bọc `#ifndef`).

**Chế độ OS.** `SX_OS_USE_FREERTOS = 0`: bare-metal, `main()` gọi `plc_engine_init()` rồi
`plc_engine_poll()`. `= 1`: host gọi cả hai từ MỘT task (`for(;;){ plc_engine_poll();
vTaskDelay(1); }`); thư viện không tạo task, mọi thứ phía sau đơn luồng không khóa —
task khác KHÔNG được đụng tag/rule/draft/retain. Macro = 1 chỉ đổi 3 thứ: (a)
`sx_delay_ms/_s` ngủ qua scheduler khi đã chạy; (b) vòng chờ bận (USB read/write,
vòng bơm USB lúc boot) gọi `sx_os_yield_wait()`; (c) logger dùng mutex. Yêu cầu host:
header FreeRTOS qua `-DSPLC_OS_INCLUDE_DIRS=...`, `FreeRTOSConfig.h` có
`INCLUDE_vTaskDelay`, `INCLUDE_xTaskGetSchedulerState`,
`configSUPPORT_DYNAMIC_ALLOCATION`; `configTICK_RATE_HZ=1000` nên dùng. `sx_get_tick_ms()`
GIỮ `HAL_GetTick()` (an toàn trong ISR PVD; host phải để HAL timebase chạy, khi dùng
FreeRTOS đặt timebase sang TIM). TinyUSB giữ `OPT_OS_NONE`. Không log từ ISR khi = 1.
Phần RTOS ở `platforms/freertos/` (OBJECT library, object đưa thẳng vào executable
bằng `$<TARGET_OBJECTS:splc_platform_freertos>` — KHÔNG dùng static lib, phụ thuộc
thứ tự link). `components/os/sx_os.h` là hợp đồng Layer 1; RTOS khác = thêm
`platforms/<rtos>/`.

## 3. Quyết định đã chốt (người dùng xác nhận — ĐỪNG tự đổi, hỏi lại nếu nghi)

1. `protocol_version = 2` (dòng "Protocol Version: 1" trong Wire Contract là lỗi đánh máy).
2. RTC dùng RTC nội STM32H5, clock **LSI**, không VBAT/pin, không LSE. Mất điện → RTC
   về chưa đồng bộ; reset mềm giữ giờ + `SYNCED`. CubeMX: Async 127 / Sync 249;
   linker `--wrap=HAL_RTC_SetTime/SetDate` để `MX_RTC_Init()` không xoá giờ.
3. `status_flags` (`0x0813`) RO, firmware tự tính. `tz_offset_min` chỉ trong RAM: sau
   REBOOT tz về 0 và Time Window im lặng đến khi Studio ghi lại `0x0810`.
4. Time Window theo Structs v2.0 mục 7: `Lo<=Hi` khung trong ngày, `Lo>Hi` qua nửa đêm
   (bắn mỗi scan khi đúng); `compare_op==EQ` và `Lo==Hi` = mốc phút (bắn 1 lần ở sườn
   lên). `trigger_tag` bị bỏ qua. Không có lớp tương thích v0.1: App phải sinh rule
   "đúng giờ" dạng `EQ, Lo=Hi`.
5. SYSTEM_COMMAND luôn chạy ngay; dirty interlock chỉ áp dụng cho `CMD_EXIT_DIAG`.
   Trong diag, trước SYSTEM_COMMAND, tag host đã ghi về 0 (baseline 0, theo bitmap).
6. Thoát diag (EXIT hoặc hết lease): `rule_runtime_reset()` mọi rule như vừa nạp; bảng
   rule giữ nguyên. Hệ quả: input đang cao → rule `ON_RISE` bắn ngay lượt đầu.
7. `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload Flash. Retain lưu bằng `plc_retain.c`
   (không làm ping-pong Appendix A — lệch tài liệu CÓ CHỦ ĐÍCH, App không thấy).
8. **FB — "dual generation":** App vẫn sinh macro rule (Timer/Counter chạy bằng Rule
   Engine) VÀ ghi bảng FB; firmware chỉ lưu cấu hình và BÁO CÁO. Vùng `0x0B00..0x0B7F`
   R/W; Host sở hữu `mode`, `pt_ms`/`preset`, thanh ghi `+6`; firmware sở hữu
   `status_bits`, `et_ms`/`current_value`, `reserved` (Host ghi kèm thì bị bỏ qua).
   Mode: Timer 1=TON 2=TOF 3=TP; Counter 1=CTU 2=CTD; 0 = không dùng. Không CTUD/HSC.
9. **Counter:** `+6` = index toàn cục của tag chứa CV (VFLAG/VREG/VREG_RETAIN/COUNTER,
   `0xFFFF` = không có; DI/DO/AI/Modbus bị từ chối `0x03`, không hai counter trùng
   tag). Firmware chỉ ĐỌC tag đó: `CV` = giá trị tag, `Q`: CTU `CV>=PV`, CTD `CV<=0`.
   Counter `i` KHÔNG gắn với tag `COUNTER[i]`. Counter retain không cần code riêng:
   CV là `VREG_RETAIN` thì `plc_retain.c` lưu như mọi tag retain; CLEAR_RETAIN đưa về 0.
10. **Timer:** `+6` = `rule_ref` (rule index + 1, 0 = chưa gắn). `plc_fb_scan()` đọc
    runtime của rule đó + tag Q (`action_tag`) để báo IN/Q/RUNNING/ET. `rule_ref`
    không kiểm với số rule lúc ghi (rule commit sau).
11. **FB qua bản nháp:** ghi FB vào nháp (validate ngay, all-or-nothing, `0x02/0x03`),
    đọc `0x0B00..` luôn trả cấu hình ĐANG CHẠY. COMMIT: khối có cờ nhận nháp, khối
    không ghi → DISABLED, rồi lưu Flash cùng Rule Table. Nháp bị xoá sau COMMIT (kể cả
    lỗi), `CLEAR_RULES`/`FACTORY_RESET`, và khi boot ⇒ mỗi Deploy App phải ghi lại FB
    từ đầu. Trùng cv_tag chỉ so giữa các khối trong nháp.
12. **Không sửa file CubeMX tự sinh, kể cả khối `USER CODE`.** Cần hành vi khác → chặn ở
    lớp thấp (linker `--wrap`, lớp `sx_*`) và giữ ở Layer 0 + CMake. Báo người dùng nếu
    thấy bug thật trong file sinh (vd. `STM32H523xx_FLASH.ld` khai `LENGTH=512K`, chip
    thật 256 KB — nên đặt ≤ 216K để linker bắt tràn vào vùng dữ liệu `0x08036000+`;
    HỎI trước khi sửa).
13. Quyết định `0x0A20` đọc trả lệnh cuối đã ghi (khớp GV-005), dù Wire Contract ghi WO.
    Layout tag board này DENSE, khác bảng cố định Structs mục 4 — code validate theo
    KIND nên đúng cho cả hai; test script tính index từ `DeviceResourceInfo`.

## 4. Việc còn lại / tồn đọng

**App + spec (không gấp):**
- `RuleCompiler.cs` (~dòng 260): `retainTagIndex` chỉ gửi khi CV là VREG_RETAIN; sửa gửi
  `CvTagIndex` cho mọi tag CV hợp lệ. Giá trị dự phòng 84 ở Studio sai trên board này
  (VREG_RETAIN bắt đầu ở tag 72).
- Đọc `0x0B00..0x0B7F` một FC03 128 reg bị từ chối (tối đa 125): App phải tách (64+64).
- Spec cần sửa: Structs 3.6/3.7 và Wire Contract 9.3 (quyền R/W, ý nghĩa `+6`, thêm
  `rule_ref`). Báo team App khi sửa.
- Đội App sinh rule "đúng giờ" `EQ, Lo=Hi` (quyết định 4).

**Firmware:**
- Validate `guard_tag`/`trigger_tag`/`action_tag` ∈ `0..MAX_TAGS-1` (hoặc `GUARD_TAG_NONE`)
  khi nạp rule — hiện index sai chỉ vô hiệu hoá rule âm thầm.
- `modbus_usb_write()` bỏ qua `timeout_ms` (đo scan_time tới 83 ms với 100 rule dưới tải).
- Chưa ghi snapshot retain ngay trước REBOOT.
- Lỗi Flash không-lập-trình-được trong lúc erase: bước copy A→B huỷ bản cũ trước khi
  biết lỗi (có từ trước, áp dụng cả COMMIT thường và CLEAR_RULES).
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM` chưa implement (ngoài V2.0;
  Event Log và Alarm chưa thiết kế). Modbus Master cho Gateway chưa có.
- Sai số LSI: cần chính xác hơn thì thêm LSE (đổi CubeMX + prescaler 127/255).
- Lỗi trước `logger_init` không có log: ba lệnh đọc Flash trong `plc_engine_init()` chạy
  TRƯỚC `board_init()`; treo ở đó thì màn hình log trống.
- Utils chưa soát kỹ: `utils/cqueue` (`cqueue_init()` dùng malloc, đang là code chết),
  `utils/filter/*`.
- Comment cũ còn nhắc `FREE_RTOS` mặc định ở `board/CMakeLists.txt`, `components/
  CMakeLists.txt`, `CMakeLists.txt` gốc — dọn khi đụng tới.

## 5. Bài học

1. Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới.
2. Bằng chứng trước khi kết luận: log là dấu vân tay của phiên bản code; lọc theo từ
   khoá trước khi nghi code không chạy (poll sinh nhiều DEBUG làm trôi log).
3. Test chỉ có nghĩa nếu trạng thái đầu xác định (reset board, kiểm `DO0=0` trước rule
   thử; test persistence phải kiểm dữ liệu sống sót qua reboot TRƯỚC khi xoá).
4. Timeout đếm bằng vòng lặp không phải timeout: đo bằng tick thật.
5. Đừng suy ngữ nghĩa field từ tên hay từ một phía; đọc UI/compiler của App và hỏi
   người dùng.
6. `switch/case` + fallthrough không tự an toàn hơn `if/continue` — test thực nghiệm.
7. Flash giả mô phỏng mất điện: sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA.
8. Việc chưa giao thì mất khi phiên kết thúc (container không giữ, repo chỉ có cái đã
   push): sau mỗi mốc present nguyên file ngay. Khi code trong repo lệch handoff
   (đã gặp với chế độ OS) thì tin code và sửa handoff.
9. Không code hành động phá huỷ dữ liệu khi spec mơ hồ — hỏi phạm vi trước.
10. Đổi giá trị mặc định/cấu hình: kiểm cả bản `-D`/override, giá trị sai (`#error`) và
    thiếu file `splcopts.h`, không chỉ bản mặc định.

## 6. Quy trình làm việc

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Đầu phiên hoặc khi người dùng báo đã
  push: `git pull`, rồi **build/compile verify thật** (không chỉ đọc diff). Claude có
  thể `apt-get install gcc-arm-none-eabi cmake ninja-build` và
  `git submodule update --init` (tinyusb) để build `cmake --preset Release`.
- **Giao nguyên file** (present từng file để copy-paste), KHÔNG patch/zip/đoạn thay thế.
  Chỉ giao code dự án + script test Python; KHÔNG giao harness C chạy trên PC. Việc tự
  kiểm tra nội bộ (gcc host) vẫn làm nhưng nêu rõ là chạy trên mô hình giả. Tính năng
  không test được trên board (vd. PVD thật) thì nói rõ là chưa test.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, từng quyết định nhỏ.
- Nói rõ mức chắc chắn khi đề xuất nguyên nhân; kiểm chứng bằng dữ liệu thật.
- Mỗi bước mới: code → người dùng build ARM → chạy test trên board → cập nhật file này.

## 7. Verify & test

**Script test trên board** (`python -m pip install pymodbus pyserial`; máy dev nhiều
Python, dùng đúng interpreter). Các script ghi Flash/xoá rule — chỉ chạy trên board dev:

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | descriptor, nạp rule, commit, live watch |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, engine dừng thật, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD |
| `test_rtc.py COM14 [--reboot]` | khối RTC, từ chối ghi sai, tốc độ đồng hồ, Time Window |
| `test_fb.py COM14 [--reboot] [--probe-64]` | khối FB (nháp/COMMIT, từ chối, CV/Q, persistence); `--probe-64`: FC16 73 byte |
| `test_retain.py COM14 [--commits N] [--periodic]` | retain qua REBOOT, xoay vòng sector, CLEAR_RETAIN; `--periodic` (~6 phút) chu kỳ 5 phút |
| `test_sysclear.py COM14 [--reboot]` | 3 lệnh xoá, persistence, lệnh trong diag, lệnh sai |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`.

**Không cần toolchain ARM:** Layer 2 (`plc_tag.c`, `plc_rule.c`, `plc_internal_rule/*.c`)
biên dịch bằng `gcc -std=c11` + stub `logger.h` (`sizeof(SPLC_RuleRecord)==32`).
`plc_modbus_cfg.c` kiểm cú pháp được với include path đã dùng trước đây (thêm
`-Isimple_plc/config -Isplc_config`). Kiểm cú pháp driver ARM trên PC: gcc host + HAL thật
(`-DUSE_HAL_DRIVER -DSTM32H523xx ...`), bỏ qua cảnh báo `cast to pointer from integer` của
header vendor.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 không có symbol `sx_usb_*`; Layer 2 không
có `sx_flash_*`/`sx_usb_*`; `plc_rule_flash.c` PHẢI có `sx_flash_*`; `sx_system_reset`
chỉ được gọi từ `plc_system_cmd_service.c` (Layer 4).

**Kiểm cấu hình:** build mặc định; build `SX_OS_USE_FREERTOS=1` (cần
`-DSPLC_OS_INCLUDE_DIRS`); build `MAX_RULES=50`; giá trị sai phải `#error`; thiếu
`splcopts.h` phải `FATAL_ERROR` ở bước configure.