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

**Repo firmware board gateway đang chạy, chưa tích hợp rule engine**: https://github.com/ngoxuanloc2309/wg_ue.git

## 0. Trạng thái hiện tại (nhánh `board_dev_gw`, tách từ `main` = `board_dev_add_select_os`)

- **Board đã chạy trên phần cứng: Zigbee-IO** (`board_zigbee_io.c`): 4 DI / 4 DO / 0 AI /
  32 VFLAG / 32 VREG / 32 VREG_RETAIN / 8 COUNTER = 112 tag (DENSE: DI 0-3, DO 4-7,
  VFLAG 8-39, VREG 40-71, RETAIN 72-103, COUNTER 104-111). MCU STM32H523CCU6 (256 KB).
- **Wire Profile V2.0 xong và verify trên board** (Diag `0x0A20`, ghi tag trong diag, retain
  draft/COMMIT, 3 lệnh xoá + REBOOT, RTC + Time Window, khối FB `0x0B00..0x0B7F`, retain +
  PVD; **đường PVD thật chưa test**).
- **Cấu hình kiểu lwIP + chế độ OS** (nhánh trước): xong, build ARM OK, chưa chạy lại trên board.
- **Việc của nhánh này — build ARM Release OK (arm-none-eabi-gcc 13), CHƯA nạp/chạy trên
  board; logic Remote I/O mới chỉ test trên PC với slave RS485 giả:**
  1. **Flash tính ngược từ cuối** bằng option `SPLC_FLASH_SIZE_KB` (quyết định 11).
  2. **Gateway: DI/DO ảo qua RS485** — `port/modbus_serial` (Modbus RTU master không chặn),
     `services/plc_io/plc_io_remote.*`, SKU `board_ethernet_wifi_gateway.*`, mục
     `BOARD_GATEWAY` trong `board_tag_define.h`, option `SPLC_REMOTE_*` (quyết định 12).
     **File board này là bản mẫu viết cho dự án CubeMX H523 (`huart1`, `hlpuart1`,
     `USART1_DE`), KHÔNG đúng với board gateway thật (H563) — phải viết lại** (mục 4).
  3. **Logger của firmware chủ:** `config/splc_log.h`, `SPLC_LOGGER_BUILTIN`,
     `SPLC_LOG_OUTPUT` (quyết định 14).
  4. **Ống byte App link kiểu lwIP — bước A1 xong:** `port/modbus_link/` (`splc_link_register`,
     `splc_link_input`, `output()`; quyết định 13/16), option `SPLC_LINK_RX_BUF_SIZE`.
     Kiểm trên PC (1523 check, ThreadSanitizer sạch) + build ARM sạch. **CHƯA có code nào gọi
     nó** (board vẫn dùng `modbus_transport_usb_create()`), nên firmware không đổi và chưa
     có gì để chạy trên board. Các bước còn lại ở mục 4.A.
- **Số đo build (Release, linker):** Zigbee-IO FLASH 79 256 B / RAM 77 696 B (số này giữ
  nguyên sau khi thêm `modbus_link`, vì linker loại bỏ nó khi chưa ai gọi); SKU Gateway
  FLASH 69 760 B / RAM 76 696 B (`main` cũ: 76 948 / 76 840). `modbus_link.c.obj` riêng:
  955 B code + 541 B RAM (512 B là ring). Mọi số trong tài liệu cũ (69 KB / 37,4 KB) đã lỗi thời.
- **Chưa verify (không chặn việc tiếp theo):** chạy lại `test_plc.py`, `test_fb.py`,
  `test_retain.py` với firmware mới (config + OS + Flash + logger + hook `plc_io_remote`
  trong `input_scan/output_scan`); Remote I/O với board IO thật (timing RS485, nhả chân DE);
  FreeRTOS kernel thật (đo độ trễ task khác lúc erase Flash; HAL timebase chuyển sang TIM);
  PVD thật, mất điện giữa lúc ghi Flash, `test_rtc.py --reboot`.

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
  byte FIFO TinyUSB → `rxQueue` trong lúc chờ (khung > 64 byte được trả lời). `plc_modbus_cfg`
  đặt `read_timeout=0` (chờ request) nhưng `byte_timeout` > 0 sau khi request đã bắt đầu:
  chính nhờ đó mảnh gói USB thứ hai (đến sau ~1 ms) được ghép vào khung.
- **App link tổng quát (`port/modbus_link`):** core giữ ring RX (SPSC lock-free,
  `acquire/release`) và MỌI timeout (`read` chờ bằng tick thật + `sx_os_yield_wait`, `write`
  retry `output()` một phần đến hết timeout); port chỉ gọi `splc_link_input()` và cài
  `output()`. Hợp đồng luồng: `splc_link_input()` từ đúng MỘT ngữ cảnh (task hoặc ISR, ISR-safe,
  không chặn); `output()/process()/transport` chạy ở ngữ cảnh engine; `register/unregister` chỉ
  lúc khởi tạo. Chi tiết: `docs/architecture.md` mục 3.1.
- **Multi-board:** `SPLC_TagLayout` truyền qua tham số; board chọn bằng `SPLC_BOARD` trong
  `splcopts.h` (CMake đọc, bảng ở `cmake_modules/splc_select.cmake`); thêm board = 1 file
  `board_<sku>.c` + 1 hàng bảng. `MAX_TAGS` ở `board/board_tag_define.h` (authoritative);
  các macro `TAG_DI0...` trong đó chỉ là bảng tham chiếu, KHÔNG board .c nào include.
- **Remote I/O (Gateway):** `plc_io.c` gọi `plc_io_remote_input_scan()` (cache DI → tag) ở
  cuối `input_scan()` và `plc_io_remote_output_scan()` (thu kết quả + bắn tối đa 1 request
  RS485 mới/scan) ở cuối `output_scan()`. Không có node nào được thêm → không làm gì
  (board Zigbee-IO không bị ảnh hưởng). Không bao giờ chặn vòng scan.

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
  `SPLC_MODBUS_UNIT_ID`, `SPLC_USB_RX/TX_BUF_SIZE`, `SPLC_LINK_RX_BUF_SIZE` (lũy thừa của 2,
  ≥ 256, mặc định 512), `SPLC_LOG_LEVEL`,
  `SPLC_LOG_BUFFER_SIZE`, `SPLC_LOGGER_BUILTIN`, `SPLC_LOG_OUTPUT` (macro, không có mặc
  định số), `SPLC_FLASH_SIZE_KB`, `SPLC_REMOTE_MAX_NODES/TIMEOUT_MS/POLL_MS/OFFLINE_AFTER/
  RETRY_MS` (xem `splc_opt.h`). CMake cũng ĐỌC `SPLC_BOARD`, `SPLC_PLATFORM`,
  `SPLC_LOGGER_BUILTIN` thẳng từ `splcopts.h` (`splc_select.cmake`). Thêm option: thêm vào `splc_opt.h` (mặc định + check) và
  template.
- KHÔNG phải option: hằng wire (địa chỉ thanh ghi, kích thước struct, lease diag), kích
  thước sector và 5 sector dữ liệu (`splc_flash_define.h`), tag layout board.
- `app/sx_platform_config.h`, `board/board_config.h`, `app/sx_os_config.h` giờ chỉ
  SUY RA cờ (`STM32H5_PLATFORM`, `BOARD_*`, `SX_NO_OS`) từ option; đừng định nghĩa tay.
- CMake: `include_directories()` phạm vi thư mục (mọi layer thấy `splc_opt.h`) +
  INTERFACE target `splc_config` link vào executable (main.c).
- Override bằng `-D` trên dòng lệnh chỉ có tác dụng khi `splcopts.h` không định nghĩa
  trùng option đó (hoặc bọc `#ifndef`).

**Chế độ OS.** `SX_OS_USE_FREERTOS = 0`: bare-metal. `= 1`: host gọi `plc_engine_init()` rồi
`plc_engine_poll()` từ MỘT task; thư viện không tạo task, mọi thứ đơn luồng không khóa —
task khác KHÔNG được đụng tag/rule/draft/retain. Đổi đúng 3 thứ (sleep qua scheduler, vòng
chờ bận gọi `sx_os_yield_wait()`, logger dùng mutex). Yêu cầu host và chi tiết:
`docs/architecture.md` mục 3. `sx_get_tick_ms()` giữ `HAL_GetTick()` (an toàn trong ISR PVD).
Không log từ ISR khi = 1. Phần RTOS ở `platforms/freertos/` (OBJECT library, object đưa vào
executable bằng `$<TARGET_OBJECTS:splc_platform_freertos>`, KHÔNG dùng static lib).

## 3. Quyết định đã chốt (người dùng xác nhận — ĐỪNG tự đổi, hỏi lại nếu nghi)

1. `protocol_version = 2` ("Protocol Version: 1" trong Wire Contract là lỗi đánh máy).
2. RTC = RTC nội STM32H5, clock **LSI**, không VBAT/LSE; mất điện → chưa đồng bộ, reset mềm
   giữ giờ. CubeMX Async 127 / Sync 249; linker `--wrap=HAL_RTC_SetTime/SetDate` để
   `MX_RTC_Init()` không xoá giờ. `status_flags` (`0x0813`) RO do firmware tính;
   `tz_offset_min` chỉ trong RAM (sau REBOOT về 0, Time Window im đến khi Studio ghi lại).
3. Time Window theo Structs v2.0 mục 7: `Lo<=Hi` khung trong ngày, `Lo>Hi` qua nửa đêm
   (bắn mỗi scan khi đúng); `compare_op==EQ` và `Lo==Hi` = mốc phút (bắn 1 lần ở sườn lên);
   `trigger_tag` bị bỏ qua. Không có lớp tương thích v0.1.
4. SYSTEM_COMMAND luôn chạy ngay; dirty interlock chỉ áp cho `CMD_EXIT_DIAG`. Trong diag,
   trước SYSTEM_COMMAND, tag host đã ghi về 0. Thoát diag (EXIT/hết lease):
   `rule_runtime_reset()` mọi rule như vừa nạp (input đang cao → `ON_RISE` bắn ngay).
5. `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload Flash. Retain dùng `plc_retain.c` (không làm
   ping-pong Appendix A — lệch tài liệu CÓ CHỦ ĐÍCH, App không thấy).
6. **FB — "dual generation":** App vẫn sinh macro rule (Timer/Counter chạy bằng Rule Engine)
   VÀ ghi bảng FB; firmware chỉ lưu cấu hình và BÁO CÁO. `0x0B00..0x0B7F` R/W; Host sở hữu
   `mode`, `pt_ms`/`preset`, thanh ghi `+6`; firmware sở hữu `status_bits`, `et_ms`/
   `current_value`, `reserved`. Mode: Timer 1=TON 2=TOF 3=TP; Counter 1=CTU 2=CTD; 0 = không dùng.
7. **Counter:** `+6` = index toàn cục tag chứa CV (VFLAG/VREG/VREG_RETAIN/COUNTER; `0xFFFF` =
   không có; DI/DO/AI/Modbus bị từ chối `0x03`; không hai counter trùng tag). Firmware chỉ ĐỌC
   tag: `CV` = giá trị tag, `Q`: CTU `CV>=PV`, CTD `CV<=0`. Counter `i` KHÔNG gắn tag
   `COUNTER[i]`; CV là `VREG_RETAIN` thì retain như mọi tag retain. **Timer:** `+6` =
   `rule_ref` (rule index + 1, 0 = chưa gắn); `plc_fb_scan()` đọc runtime rule đó + tag Q.
8. **FB qua bản nháp:** ghi vào nháp (validate ngay, all-or-nothing, `0x02/0x03`), đọc
   `0x0B00..` luôn trả cấu hình ĐANG CHẠY. COMMIT: khối có cờ nhận nháp, khối không ghi →
   DISABLED, rồi lưu Flash cùng Rule Table. Nháp bị xoá sau COMMIT (kể cả lỗi), `CLEAR_RULES`/
   `FACTORY_RESET`, và khi boot ⇒ mỗi Deploy App phải ghi lại FB từ đầu.
9. **Không sửa file CubeMX tự sinh, kể cả khối `USER CODE`.** Cần hành vi khác → chặn ở lớp
   thấp (linker `--wrap`, lớp `sx_*`), giữ ở Layer 0 + CMake. Thấy bug thật trong file sinh
   (vd. `STM32H523xx_FLASH.ld` khai `LENGTH=512K` trên chip 256 KB) → báo người dùng, HỎI
   trước khi sửa.
10. `0x0A20` đọc trả lệnh cuối đã ghi (khớp GV-005) dù Wire Contract ghi WO. Layout tag
    board này DENSE, khác bảng cố định Structs mục 4 — code validate theo KIND nên đúng cho
    cả hai; test script tính index từ `DeviceResourceInfo`.
11. **Flash:** vùng dữ liệu (Rule A/B + Retain) LUÔN là 5 sector cuối, tính ngược từ
    `END = FLASH_BASE + SPLC_FLASH_SIZE_KB*1024`. Người dùng chỉ khai size; sector 8 KB là
    hằng của họ H5 (không phải option, vì sai thì xoá nhầm code); giả định H5 dual-bank chia
    đôi (đối chiếu datasheet khi dùng chip mới). Kiểm lúc biên dịch: bội số 16 KB, 128..2048,
    5 sector nằm trong bank 2. Bản đồ Flash/bootloader của firmware gateway (`wg_ue`) hiện
    KHÔNG xét (người dùng quyết định); khi nhúng sẽ xung đột (mục 4).
12. **Gateway:** SimplePLC chạy trên gateway; board IO có chân thật là **slave RS485 thuần**
    (nhận lệnh bật/tắt DO, được hỏi thì trả DI) — KHÔNG chạy SimplePLC/Rule Engine/Flash
    rule. DI/DO ảo ↔ (cổng RS485, unit ID, bit) khai CỨNG lúc biên dịch trong file board
    (`GATEWAY_IO_NODES`), thiết kế theo dữ liệu: 4DI/4DO → 8DI/8DO là sửa một dòng bảng,
    thêm board IO là thêm một dòng. Gateway chỉ khai UART log + UART RS485 (không ngoại vi
    khác). Giao thức link: khối `0x0C00..0x0C06` (architecture mục 5.1).
13. **Nhúng vào firmware chủ (kiểu lwIP) — thiết kế đã chốt:** SimplePLC là thư viện dùng
    cho mọi board đã định nghĩa trong libs; người dùng chỉ viết `splcopts.h` và (khi cần) một
    file `.c` port, như `lwipopts.h` + `ethernetif.c`. Người dùng chọn board CÓ SẴN bằng
    `SPLC_BOARD` như hiện tại; KHÔNG làm file board nằm ngoài thư viện. Hai nguyên tắc:
    (a) tài nguyên có trạng thái/phần cứng (USB, UART, Flash, ngắt) chỉ có MỘT chủ — hoặc
    SimplePLC (module port tùy chọn), hoặc host; (b) thư viện thuần (nanoMODBUS) thì SimplePLC
    LUÔN dùng bản riêng (quyết định 15). Core chỉ thấy một "ống byte" (`modbus_link`); USB,
    RS485 hay Ethernet chỉ là cách khác nhau để cắm vào ống, nên KHÔNG có selector loại đường
    truyền (`SPLC_APP_LINK` đã bị loại) và không có `SPLC_USB_EXTERNAL`/`SPLC_NANOMODBUS_BUILTIN`.
    Core không được phụ thuộc TinyUSB: `sx_usb_cdc.c`, descriptor, ISR USB và `modbus_usb`
    trở thành MODULE PORT tùy chọn (cờ trong `splcopts.h`, tên đề xuất `SPLC_PORT_USB_CDC`,
    CHƯA chốt tên; board cần USB thì `#error` khi cờ = 0). Quy ước cũ "`read` của transport ngoài
    trả NGAY khi chưa có dữ liệu" bị BỎ: nó làm mất khung > 64 byte (đúng bug FC16 73 byte
    từng gặp); logic chờ timeout phải nằm trong core, một chỗ. Điểm nối log vẫn là
    `SPLC_LOG_OUTPUT` (quyết định 14).
14. **Logger:** SimplePLC code chỉ gọi `log_error/warn/info/debug` (`config/splc_log.h`) →
    `SPLC_LOG_OUTPUT(level, tag, ...)`. Mặc định gọi `log_func()` của `logger.h`;
    `SPLC_LOGGER_BUILTIN=1` (mặc định) dùng `utils/logger` + board gọi `logger_init`;
    `=0` host cung cấp `logger.h`/logger và gọi `logger_init`, SimplePLC không build
    `logger.c` và không đưa `utils/logger` vào include path (trùng tên `logger.h`).
    `utils/logger` cùng dòng với `libs/logger` của `wg_ue` (cùng API; macro `LOGD/LOGI/LOGW/
    LOGE`) nhưng an toàn hơn (`snprintf`, kiểm tra tràn, mutex NULL-an-toàn).
15. **nanoMODBUS: luôn là bản riêng của SimplePLC, đổi tên symbol** (người dùng xác nhận):
    header wrapper đổi các hàm công khai `nmbs_*` → `splc_nmbs_*`, KHÔNG sửa file vendored
    `libs/nanomodbus/nanomodbus.c/.h`; include path của nó chỉ PRIVATE. Lý do: nanoMODBUS là
    logic thuần (không có ngoại vi), nên hai bản cùng tồn tại không tranh chấp gì; dùng chung
    bản của host làm độ tin cậy phụ thuộc phiên bản và cờ biên dịch của host (host làm master
    có thể đã tắt server) và phải kiểm lại ở mỗi lần port. Giá: ~7,5 KB Flash (số đo
    `nanomodbus.c.obj` = 7 507 B). Điều kiện phải kiểm bằng build (bước A2) và báo ngay nếu
    không đạt, KHÔNG tự đổi hướng: không có symbol dữ liệu toàn cục trùng (`nm`), header của
    host không lọt vào TU của SimplePLC. TinyUSB thì KHÔNG đổi tên được (ISR, descriptor,
    ngoại vi duy nhất) — chỉ một chủ.
16. **Đăng ký ống: tường minh (kiểu `netif_add`), không dùng hàm `weak`** (người dùng xác
    nhận). `splc_link_register(&ops)` trước `plc_engine_init()`; quên đăng ký thì
    `splc_link_get_transport()` trả `false` và phải báo lỗi rõ, thay vì firmware chạy mà App
    không bao giờ có phản hồi. Board có module USB của SimplePLC (Zigbee-IO) tự đăng ký trong
    `board_hw_init()`, người dùng board đó không làm gì thêm. `output()` KHÔNG được gọi thẳng
    driver không an toàn luồng (vd. TinyUSB của task khác): phải đẩy byte vào queue của task
    driver.

## 4. Việc còn lại / tồn đọng

**A. Nhúng vào firmware chủ (ưu tiên, theo thứ tự — mỗi bước build + kiểm riêng; thiết kế
ở quyết định 13/15/16):**
1. ✅ **`port/modbus_link`** (xong): ring RX SPSC, `read/write` có timeout thật,
   `splc_link_register/input/get_transport`. Kiểm: `test_det.c` (đồng hồ giả, 1523 check, gồm
   khung 73 byte đến 64 + 9 byte; kiểm bằng đột biến: phá code 3 kiểu thì test đỏ) và `test_mt.c`
   (2 thread thật, 3 triệu byte, ThreadSanitizer sạch; hạ `acquire/release` xuống `relaxed` thì
   TSan báo race). Harness CHƯA nằm trong repo (xem mục 6, 7). Chưa nối vào board nào.
2. **nanoMODBUS bản riêng đổi tên symbol** (quyết định 15): header wrapper
   `nmbs_* → splc_nmbs_*` (vài chục dòng, include trước `nanomodbus.h`), include path PRIVATE.
   Kiểm: `nm` không còn `nmbs_*` tên cũ và không có symbol dữ liệu toàn cục; dựng một TU/target
   giả lập "host có `nanomodbus.h` riêng" link chung để chắc không trùng symbol và không lẫn
   include; build ARM đủ cả hai board.
3. **Module USB port tùy chọn:** gom `components/usb_cdc/sx_usb_cdc.c`, `port/usb/`,
   `port/modbus_usb`, phần TinyUSB của `libs/CMakeLists.txt` thành một module build khi cờ
   (đề xuất `SPLC_PORT_USB_CDC`) = 1, đọc từ `splcopts.h` như `SPLC_LOGGER_BUILTIN`. Dọn layer:
   `sx_usb_cdc.h` (Layer 1) không include `tusb_types.h`; bỏ `#if STM32H5_PLATFORM` trong file
   .c; `HAL_GetTick()` → `sx_get_tick_ms()`; thêm hàm bọc `splc_usb_irq()` để board khỏi
   include `tusb.h`; module tự `splc_link_register()` trong `board_hw_init()`. GIỮ nguyên đường
   USB cũ cho tới khi bước 4 qua.
4. **Hồi quy trên board Zigbee-IO** (người dùng chạy): `test_fb.py --probe-64` (khung 73 byte),
   `test_plc.py`, `test_retain.py`. Qua rồi mới xoá đường USB cũ (khi đó `modbus_usb_write()`
   bỏ qua timeout, mục E, cũng hết).
5. **Port vào `wg_ue` + viết lại `board_ethernet_wifi_gateway.c` cho board H563 thật** (hiện
   là bản mẫu H523 và dùng transport giả `idle_read`; xem B): file port mẫu gọi
   `splc_link_input()` từ task USB của host và cài `output()` qua queue; trên host
   `SPLC_LOGGER_BUILTIN=0`, không gọi `logger_init`/không khởi UART log. CẦN đọc `wg_ue` TRƯỚC
   khi viết: cờ NMBS thực của host, `CFG_TUD_CDC` + chỉ số ACM, task USB nào gọi
   `tud_task_ext`, và **ai chạy vòng quét engine khi nhúng (task của host hay thư viện)** —
   chưa xét.
6. Flash cho host: `SPLC_FLASH_SIZE_KB` chưa đủ nếu host đã dùng hết bản đồ Flash — cần
   option địa chỉ bắt đầu vùng dữ liệu hoặc để host khai vị trí.
7. Linker `STM32H523xx_FLASH.ld` khai `LENGTH=512K` (chip 256 KB): nên đặt
   `SPLC_FLASH_SIZE_KB − 40 KB`. File CubeMX sinh — HỎI người dùng trước khi sửa.
Rủi ro đã biết: chưa đo độ trễ khi task engine và task USB chạy cùng lúc trên phần cứng; kích
thước ring phải đủ cho khung dài nhất (256 B RTU) khi engine ngủ một scan 10 ms.

**B. Firmware gateway thật (`ngoxuanloc2309/wg_ue`) — những gì đã đọc (chưa đối chiếu hết):**
STM32H563, FreeRTOS + lwIP. TinyUSB do `usb_netif_task` sở hữu (`tusb_init`, `tud_task_ext`,
các `tud_*_cb` trong `usb_netif.c`, `USB_DRD_FS_IRQHandler` → `tud_int_handler`); descriptor
composite RNDIS/ECM (192.168.7.1) + một CDC ACM chưa có code nào dùng; `CFG_TUSB_OS=FREERTOS`,
`CFG_TUD_CDC=2` nhưng chỉ 1 ACM trong descriptor. `middleware/nanomodbus` có chữ ký hàm giống
`libs/nanomodbus` của SimplePLC (CHỈ so từng hàm `nmbs_*`; CHƯA so phiên bản và cờ biên dịch của
host như `NMBS_SERVER_DISABLED`, nên không được coi là đủ để dùng chung bản — xem quyết định 15). `libs/logger` cùng dòng với
`utils/logger`. Theo tài liệu pinout của repo: 3 cổng RS485 (USART3/6/10, mỗi cổng có DE)
do `modbus_manager`/`mmb_serial` quản lý → SimplePLC cần một cổng riêng. Bản đồ Flash/
bootloader `libs/bootloader/flash_define.h` dùng hết Flash (bỏ qua theo quyết định 11).

**C. Remote I/O:**
- Chưa có firmware slave cho board IO (cài khối `0x0C00..0x0C06` bằng nanoMODBUS; lease/SAFE
  nằm ở slave). Chưa thử trên phần cứng.
- Chưa phát hiện trùng `unit_id` giữa hai hàng bảng trên cùng bus.
- Node offline mà DI giữ giá trị cuối: rule cần biết trạng thái dùng `plc_io_remote_node_online()`;
  chưa có tag báo "node offline" cho App.

**D. App + spec (không gấp):** `RuleCompiler.cs` (~dòng 260) chỉ gửi `retainTagIndex` khi CV là
VREG_RETAIN (giá trị dự phòng 84 sai trên board này, RETAIN bắt đầu ở tag 72); đọc
`0x0B00..0x0B7F` một FC03 128 reg bị từ chối (tối đa 125) → App tách 64+64; sửa Structs
3.6/3.7 và Wire Contract 9.3 (quyền R/W, `+6`, `rule_ref`); App sinh rule "đúng giờ" `EQ, Lo=Hi`.

**E. Firmware khác:** validate `guard/trigger/action_tag` ∈ `0..MAX_TAGS-1` khi nạp rule (hiện
index sai chỉ vô hiệu hoá rule âm thầm); `modbus_usb_write()` bỏ qua `timeout_ms` (đường mới `modbus_link` đã tôn trọng timeout; đường cũ còn tới bước A4); chưa ghi
snapshot retain ngay trước REBOOT; lỗi Flash lúc erase trong bước copy A→B huỷ bản cũ trước
khi biết lỗi; `ACT_WRITE_REMOTE/LOG_EVENT/SEND_ALARM` chưa implement; không có log trước
`logger_init` (đọc Flash trong `plc_engine_init()` chạy TRƯỚC `board_init()`); `utils/cqueue`
(`cqueue_init()` dùng malloc, code chết) và `utils/filter/*` chưa soát; comment cũ nhắc
`FREE_RTOS` còn ở vài CMakeLists.

## 5. Bài học

1. Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới.
2. Bằng chứng trước khi kết luận: log là dấu vân tay của phiên bản code; lọc theo từ khoá.
3. Test chỉ có nghĩa nếu trạng thái đầu xác định (reset board, kiểm `DO0=0` trước rule thử;
   test persistence phải kiểm dữ liệu sống sót qua reboot TRƯỚC khi xoá).
4. Timeout đếm bằng vòng lặp không phải timeout: đo bằng tick thật.
5. Đừng suy ngữ nghĩa field từ tên hay từ một phía; đọc UI/compiler của App và hỏi người dùng.
6. Flash giả mô phỏng mất điện: sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA.
7. Việc chưa giao thì mất khi phiên kết thúc (đã mất cả bản gateway một lần vì hết token
   trước khi present): present nguyên file ngay sau mỗi mốc. Code trong repo lệch handoff
   thì tin code và sửa handoff.
8. Không code hành động phá huỷ dữ liệu khi spec mơ hồ — hỏi phạm vi trước.
9. Đổi giá trị mặc định/cấu hình: kiểm cả bản `-D`/override, giá trị sai (`#error`), thiếu
   `splcopts.h`, không chỉ bản mặc định.
10. Hiểu kiến trúc trước khi code: một phiên đã nhét SimplePLC đầy đủ vào board IO trong khi
    board IO chỉ cần là slave RS485 — hỏi "thiết bị này có cần chạy PLC không?" trước.
11. Đừng tuyên bố môi trường thiếu công cụ khi chưa thử cài (`apt-get install` chạy được).
12. Hợp đồng giữa core và port phải để core giữ phần khó (timeout, ghép khung nhiều mảnh). Quy
    ước "port trả ngay khi chưa có dữ liệu" nghe gọn nhưng đẩy sự đúng đắn sang từng người
    viết port và làm lại đúng bug khung > 64 byte. Đọc comment lịch sử bug trong code
    (`sx_usb_tiny_read`) trước khi đề xuất đổi hợp đồng.
13. Test xanh chưa chứng minh gì: phá code có chủ đích (đột biến) và xem test có đỏ không;
    với đa luồng, hạ memory order xuống `relaxed` rồi xem ThreadSanitizer có báo race không.
14. Đo trước khi ghi con số vào comment/tài liệu (đã ghi "~400 byte" cho `modbus_link`, đo ra
    955 byte). Khẳng định về host ("API giống hệt") phải nêu đã so cái gì và CHƯA so cái gì.

## 6. Quy trình làm việc

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Đầu phiên hoặc khi người dùng báo đã
  push: `git pull`, rồi **build/compile verify thật** (không chỉ đọc diff). Claude có
  thể `apt-get install gcc-arm-none-eabi ninja-build`, `pip install cmake
  --break-system-packages` và `git submodule update --init` (tinyusb) để build
  (`cmake -S . -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
  -DCMAKE_BUILD_TYPE=Release [-DSPLC_OPTS_DIR=<dir>]`).
- **Giao nguyên file** (present từng file để copy-paste), KHÔNG patch/zip/đoạn thay thế.
  Chỉ giao code dự án + script test Python; KHÔNG giao harness C chạy trên PC (ngoại lệ đã
  xảy ra: `test_det.c`/`test_mt.c` của `modbus_link` được giao theo yêu cầu người dùng; muốn
  giữ thì đặt vào `test/` của repo kèm target CMake PC — quyết định của người dùng). Việc tự
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

**Kiểm trên PC (không cần board):** Layer 2 (`plc_tag.c`, `plc_rule.c`, `plc_internal_rule/*.c`)
biên dịch bằng `gcc -std=c11` + stub; `sizeof(SPLC_RuleRecord)==32`. Có thể kiểm cú pháp
file Layer 0/4 bằng gcc host + header giả. Logic Remote I/O (`plc_io_remote.c`,
`modbus_serial.c`) kiểm bằng gcc host + slave RS485 giả (10 kịch bản: sync, ghi DO, lease,
DI, offline/online lại, slave reset, CRC sai, dữ liệu thừa); harness này KHÔNG nằm trong repo
và không giao cho người dùng (xem mục 6). Nói rõ đó là mô hình giả.

**`modbus_link` trên PC** (đã chạy; harness ở `/tmp`, KHÔNG trong repo): cần `splcopts.h` tối
thiểu cho test (`SX_OS_USE_FREERTOS 0`, `SPLC_LOG_OUTPUT` rỗng/`printf`) và stub
`sx_get_tick_ms/sx_get_tick_s/sx_delay_*`. `gcc -std=c11 -Wall -Wextra -Wpedantic -Wshadow
-Wconversion -O2 -I<config> -I<app> -I<components/time> -I<components/os> -I<port/modbus_link>
-I<port/modbus_transport> test_det.c port/modbus_link/modbus_link.c`; bản đa luồng thêm
`-D_POSIX_C_SOURCE=200809L -fsanitize=thread -lpthread`. Nói rõ đó là mô hình giả, chưa phải board.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 không có symbol `sx_usb_*`; Layer 2 không
có `sx_flash_*`/`sx_usb_*`; `plc_rule_flash.c` PHẢI có `sx_flash_*`; `sx_system_reset` chỉ
được gọi từ `plc_system_cmd_service.c` (Layer 4); chỉ board file được `#include "tusb.h"`
(sau bước A3: chỉ module USB port); `modbus_link.c` không có symbol `sx_usb_*`/`tud_*`.

**Kiểm cấu hình (build ARM):** mặc định (Zigbee-IO); `SPLC_BOARD_GATEWAY` qua
`-DSPLC_OPTS_DIR`; `SPLC_FLASH_SIZE_KB` 128/256/512/… (sai phải `#error`/`_Static_assert`);
`SX_OS_USE_FREERTOS=1` (cần `-DSPLC_OS_INCLUDE_DIRS`); `MAX_RULES=50`; `SPLC_LOGGER_BUILTIN=0`
(cần logger của host mới link được); thiếu `splcopts.h` phải `FATAL_ERROR` lúc configure.