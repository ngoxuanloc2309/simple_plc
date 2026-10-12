# SimplePLC — Handoff

> Cho phiên làm việc sau nắm "đang ở đâu, làm gì tiếp, luật gì phải giữ". Đọc SAU
> `Readme.md` và `docs/architecture.md`. File này chỉ giữ: trạng thái hiện tại, quyết định
> đã chốt, kế hoạch, quy trình, bài học. Lịch sử từng bước đã bỏ — xem `git log`.
>
> **Quy tắc vàng:** trước khi tin điều gì dưới đây, chạy `git pull && git log --oneline -10`.
> Code lệch file này thì tin code và sửa file này.

**Nguồn tài liệu** (V2.0 là strict superset của V1.9). Mâu thuẫn về HÀNH VI → Wire Contract
thắng; về struct/memory map → Structs thắng:
1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md`
2. `docs/SimplePLC_Wire_Contract_V2_Draft.md`
3. `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` — chỉ còn đúng cho LOGIC rule
   (Trigger→Compare→Dwell→Guard→Action); struct/CRC/Time Window/retain đã bị ghi đè.

**Repo liên quan**
- App: `ngoxuanloc2309/paa` (.NET, SimplePLC.Studio). Bug xuyên App+firmware phải đọc CẢ HAI
  repo (`RuleCompiler.cs`, `RuleTableWriter.cs`, `FunctionBlockGateway.cs`).
- Firmware gateway thật (host nhúng SimplePLC): `ngoxuanloc2309/wg_ue` (STM32H563, FreeRTOS +
  lwIP, 3 cổng RS485 USART3/6/10, TinyUSB do `usb_netif_task` sở hữu).

## 0. Trạng thái (nhánh `board_dev_gw`)

**Đã chạy trên board (Zigbee-IO, STM32H523CCU6):** Wire Profile V2.0 (diag, retain, xoá lệnh,
RTC + Time Window, FB `0x0B00..0x0B7F`), đường USB qua `modbus_link` + `port/usb`
(`test_fb.py --probe-64`, `test_plc.py`, `test_retain.py` ALL PASS).

**Đã build ARM sạch, CHƯA chạy trên board nào:**
- Đổi tên chống trùng host (quyết định 21), `SPLC_AS_LIBRARY` (22), `SPLC_RTC/ADC_BUILTIN` (23).
- Gateway: Remote I/O qua RS485 (`plc_io_remote`, `modbus_serial` dạng callback), SKU
  `board_ethernet_wifi_gateway` (không chạm UART/GPIO/CubeMX; host cấp cổng RS485 và ống App).
- `SPLC_FLASH_DATA_ADDR` (quyết định 17).

**Số đo build chuẩn** (ARM Release, arm-none-eabi-gcc 13.2.1) — dùng để bắt hồi quy:
Zigbee-IO FLASH 80 200 B / RAM 77 664 B; Gateway (bare-metal) FLASH 55 356 B / RAM 72 624 B.
Cả hai tái hiện lại đúng từng byte ở phiên sau khi sửa lỗi include và CMake (mục 5, bài học 19).
Gateway chuẩn = `SPLC_BOARD_GATEWAY`, `SX_OS_USE_FREERTOS=0`, `SPLC_PORT_USB_CDC=0`, FLASH 256 KB,
còn lại mặc định (`-DSPLC_OPTS_DIR`). Gateway có `splcopts.h` của `port_gw/` (FreeRTOS, logger của
host, RTC/ADC=0) cần logger của host mới link, nên KHÔNG so được với số trên — đo lại trong build `wg_ue`.

**Port cho `wg_ue` — bản nháp có sẵn ở `port_gw/`** (gốc repo: `splc_port.c/.h`, `splcopts.h`;
sẽ chép sang `wg_ue/ports/splc_port/`). Đã kiểm: `splc_port.c` biên dịch sạch (`-Wall -Wextra`)
với header THẬT của `wg_ue`; symbol còn thiếu chỉ là của host (`rs485_*`, `tud_cdc_n_*`, FreeRTOS,
`log_func`, `g_usb_tusb_initialized`) hoặc của SimplePLC. Gateway link được ở cả hai ca:
`SPLC_RTC/ADC_BUILTIN=0` KHÔNG stub → FAIL link (`sx_rtc_*`, `sx_adc_read`); có stub trong
`splc_port.c` → link qua. **Chưa** link với `wg_ue` thật, chưa chạy trên board.

**Chưa làm (A5 còn lại):** chép `port_gw/` vào `wg_ue/ports/splc_port/`, nối CMake SimplePLC vào
build `wg_ue`, sửa bản đồ Flash host, firmware slave cho board IO. Chi tiết ở mục 4.

**Chưa verify (không chặn việc tiếp):** chạy lại `test_plc/test_fb/test_retain` với firmware
sau các thay đổi đổi-tên/config/OS/Flash/logger; Remote I/O với board IO thật (timing RS485,
nhả chân DE); FreeRTOS kernel thật (độ trễ task lúc erase Flash); PVD thật; mất điện giữa lúc
ghi Flash; `test_rtc.py --reboot`; nanoMODBUS riêng cùng tồn tại với bản của `wg_ue` thật
(mới thử bằng bản sao giả).

## 1. Cơ chế cần giữ (không sửa lại trừ khi có lý do mới)

- **Scan loop** (`app/plc_app/plc_engine.c`, 10 ms): `diag_tick → input_scan → [engine không bị
  diag treo: rule_scan + plc_fb_scan] → output_scan → modbus_config_service → retain_service →
  ghi scan_time → plc_system_cmd_service` (cuối cùng vì có thể reset).
- **Rule Table lưu Flash A/B** (`plc_rule_flash`): copy A→B, erase+ghi A, đọc lại kiểm CRC và
  seq_num mới, lỗi thì B→A. Đoạn FB (128 B; bản cũ 112 B vẫn nạp được) nằm TRONG bản ghi: bit 15
  `rule_count` = có FB, bit 14 = FB 128 B, bit 0..13 = số rule; một CRC phủ tất cả. Mọi code đọc
  trường này phải tách cờ trước.
- **CRC Rule/Retain tính trên byte WIRE** (`rule_table_wire_crc16()`). `nmbs_crc_calc()` trả kết
  quả hoán byte (chỉ cho RTU framing).
- **Retain** (`plc_retain`): log xoay vòng 3 sector, record 208 B; lưu khi COMMIT_RETAIN, chu kỳ
  `RETAIN_SNAPSHOT_PERIOD_MS` (chỉ khi dữ liệu đổi), PVD khẩn cấp (`retain_emergency_snapshot()`
  trong ISR: không erase, không log, hoãn nếu vòng chính đang thao tác Flash). Mọi thao tác Flash
  ngoài `plc_retain.c` phải bọc `retain_flash_op_begin/end()`.
- **PVD** nối bằng file dự án (`stm32h5_pwd.c` định nghĩa `PVD_AVD_IRQHandler`); đăng ký callback =
  bật NVIC. Nếu CubeMX tự sinh handler thì định nghĩa `SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX`. Tắt bằng
  `PLC_PVD_EMERGENCY_SAVE_ENABLE 0`.
- **System command:** luôn chạy ngay (bất kể diag/dirty). REBOOT trì hoãn `PLC_REBOOT_DELAY_MS`
  (300 ms) cho phản hồi kịp ra — ĐỪNG bỏ. CLEAR_RULES/CLEAR_RETAIN/FACTORY_RESET ghi một bản ghi
  RỖNG hợp lệ qua đường lưu đã verify; FACTORY_RESET = CLEAR_RETAIN + CLEAR_RULES, không reboot.
- **Giới hạn Modbus:** FC16 ≤ 123 reg, FC03 ≤ 125 reg. Gói USB FS 64 B nên App chia 1 rule
  (41 B)/lệnh. `plc_modbus_cfg` đặt `read_timeout=0` nhưng `byte_timeout>0` sau khi request đã bắt
  đầu — nhờ đó mảnh gói USB thứ hai được ghép vào khung.
- **`modbus_link` (ống byte App):** core giữ ring RX (SPSC lock-free, `acquire/release`) và MỌI
  timeout (`read` chờ bằng tick thật + `sx_os_yield_wait`; `write` timeout **idle**: đo lại mỗi
  khi `output()` có tiến triển). Hợp đồng luồng: `splc_link_input()` từ đúng MỘT ngữ cảnh (task
  hoặc ISR, không chặn); `output()/process()/transport` chạy ở ngữ cảnh engine; `register` chỉ lúc
  khởi tạo. Chi tiết: `architecture.md` mục 3.1.
- **nanoMODBUS riêng (`libs/splc_nanomodbus`):** `splc_nmbs.h` đổi tên 37 hàm toàn cục (30 `nmbs_*`
  + 7 `recv_*_res`), `#undef` mọi macro `NMBS_*`, include header vendored bằng đường dẫn tương đối.
  `libs/nanomodbus/` giữ nguyên, không nằm trong include path. Script `splc_check_symbols.cmake`
  chạy sau build; nâng cấp thư viện mà có hàm toàn cục mới thì build fail → thêm một `#define`.
- **Multi-board:** `SPLC_TagLayout` truyền qua tham số; chọn bằng `SPLC_BOARD` trong `splcopts.h`
  (bảng ở `cmake_modules/splc_select.cmake`); thêm board = 1 file `board_<sku>.c` + 1 hàng bảng.
  `MAX_TAGS` ở `board/board_tag_define.h`.
- **Remote I/O (Gateway):** `plc_io.c` gọi `plc_io_remote_input_scan()` cuối `input_scan()` và
  `plc_io_remote_output_scan()` cuối `output_scan()` (tối đa 1 request RS485 mới/scan, không bao
  giờ chặn). Không có node → không làm gì. Ưu tiên request: SYNC > WRITE > REFRESH > POLL
  (architecture mục 5.1). Node offline: tag DI giữ giá trị cuối; rule cần biết thì dùng
  `plc_io_remote_node_online()`.
- **Cấu hình kiểu lwIP:** người dùng chỉ sửa `splcopts.h` của sản phẩm; `config/splc_opt.h` của
  thư viện đặt mặc định + `#error` khi sai. Thêm option = thêm vào `splc_opt.h` (mặc định + check)
  và `splcopts_template.h`. CMake đọc `SPLC_BOARD`, `SPLC_PLATFORM`, `SPLC_LOGGER_BUILTIN`,
  `SPLC_PORT_USB_CDC`, `SPLC_RTC_BUILTIN`, `SPLC_ADC_BUILTIN` thẳng từ `splcopts.h`. `-D` chỉ có
  tác dụng khi `splcopts.h` không định nghĩa trùng. KHÔNG phải option: hằng wire, kích thước
  sector, tag layout.
- **Chế độ OS:** `SX_OS_USE_FREERTOS=0` bare-metal; `=1` host gọi `plc_engine_init()` rồi
  `plc_engine_poll()` từ MỘT task — thư viện đơn luồng không khóa, task khác KHÔNG được đụng
  tag/rule/draft/retain; không log từ ISR. `sx_get_tick_ms()` giữ `HAL_GetTick()`.

## 2. Quyết định đã chốt (người dùng xác nhận — ĐỪNG tự đổi, hỏi lại nếu nghi)

1. `protocol_version = 2` ("Protocol Version: 1" trong Wire Contract là lỗi đánh máy).
2. RTC = RTC nội STM32H5, clock **LSI**, không VBAT/LSE; mất điện → chưa đồng bộ, reset mềm giữ
   giờ. Linker `--wrap=HAL_RTC_SetTime/SetDate` để `MX_RTC_Init()` không xoá giờ. `status_flags`
   (`0x0813`) RO do firmware tính; `tz_offset_min` chỉ trong RAM (sau REBOOT về 0, Time Window im
   đến khi Studio ghi lại).
3. Time Window theo Structs v2.0 mục 7: `Lo<=Hi` khung trong ngày, `Lo>Hi` qua nửa đêm (bắn mỗi
   scan khi đúng); `compare_op==EQ` và `Lo==Hi` = mốc phút (bắn 1 lần ở sườn lên); `trigger_tag`
   bị bỏ qua. Không có lớp tương thích v0.1.
4. SYSTEM_COMMAND luôn chạy ngay; dirty interlock chỉ áp cho `CMD_EXIT_DIAG`. Trong diag, trước
   SYSTEM_COMMAND, tag host đã ghi về 0. Thoát diag (EXIT/hết lease): `rule_runtime_reset()` mọi
   rule như vừa nạp (input đang cao → `ON_RISE` bắn ngay).
5. `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload Flash. Retain dùng `plc_retain.c` (không làm
   ping-pong Appendix A — lệch tài liệu CÓ CHỦ ĐÍCH, App không thấy).
6. **FB "dual generation":** App vẫn sinh macro rule (Timer/Counter chạy bằng Rule Engine) VÀ ghi
   bảng FB; firmware chỉ lưu cấu hình và BÁO CÁO. `0x0B00..0x0B7F` R/W; Host sở hữu `mode`,
   `pt_ms`/`preset`, thanh ghi `+6`; firmware sở hữu `status_bits`, `et_ms`/`current_value`,
   `reserved`. Mode: Timer 1=TON 2=TOF 3=TP; Counter 1=CTU 2=CTD; 0 = không dùng.
7. **Counter:** `+6` = index toàn cục tag chứa CV (VFLAG/VREG/VREG_RETAIN/COUNTER; `0xFFFF` = không
   có; DI/DO/AI/Modbus bị từ chối `0x03`; không hai counter trùng tag). Firmware chỉ ĐỌC tag:
   `Q` = CTU `CV>=PV`, CTD `CV<=0`. CV là `VREG_RETAIN` thì retain như mọi tag retain.
   **Timer:** `+6` = `rule_ref` (rule index + 1, 0 = chưa gắn); `plc_fb_scan()` đọc runtime rule
   đó + tag Q.
8. **FB qua bản nháp:** ghi vào nháp (validate ngay, all-or-nothing, `0x02/0x03`), đọc
   `0x0B00..` luôn trả cấu hình ĐANG CHẠY. COMMIT: khối có cờ nhận nháp, khối không ghi →
   DISABLED, rồi lưu Flash cùng Rule Table. Nháp bị xoá sau COMMIT (kể cả lỗi), `CLEAR_RULES`/
   `FACTORY_RESET`, và khi boot ⇒ mỗi Deploy App phải ghi lại FB từ đầu.
9. **Không sửa file CubeMX tự sinh, kể cả khối `USER CODE`.** Cần hành vi khác → chặn ở lớp thấp
   (linker `--wrap`, lớp `sx_*`), giữ ở Layer 0 + CMake. Thấy bug thật trong file sinh (vd.
   `STM32H523xx_FLASH.ld` khai `LENGTH=512K` trên chip 256 KB) → báo người dùng, HỎI trước khi sửa.
10. `0x0A20` đọc trả lệnh cuối đã ghi (khớp GV-005) dù Wire Contract ghi WO. Layout tag board
    Zigbee-IO là DENSE (DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71, RETAIN 72-103, COUNTER 104-111),
    khác bảng cố định Structs mục 4 — code validate theo KIND nên đúng cho cả hai; test script
    tính index từ `DeviceResourceInfo`.
11. **Flash:** vùng dữ liệu (Rule A/B + Retain) LUÔN là 5 sector cuối, tính ngược từ
    `END = FLASH_BASE + SPLC_FLASH_SIZE_KB*1024`. Người dùng chỉ khai size; sector 8 KB là hằng
    của họ H5 (không phải option, vì sai thì xoá nhầm code); giả định H5 dual-bank chia đôi (đối
    chiếu datasheet khi dùng chip mới). Kiểm lúc biên dịch: bội số 16 KB, 128..2048, 5 sector nằm
    trong bank 2.
12. **Gateway:** SimplePLC chạy trên gateway; board IO có chân thật là **slave RS485 thuần** (nhận
    lệnh bật/tắt DO, được hỏi thì trả DI) — KHÔNG chạy SimplePLC/Rule Engine/Flash rule. DI/DO ảo
    ↔ (cổng RS485, unit ID, bit) khai CỨNG lúc biên dịch trong file board (`GATEWAY_IO_NODES`),
    thiết kế theo dữ liệu: 4DI/4DO → 8DI/8DO là sửa một dòng bảng. Link giao thức: khối
    `0x0C00..0x0C06` (architecture mục 5.1).
13. **Nhúng vào firmware chủ (kiểu lwIP):** người dùng chỉ viết `splcopts.h` và (khi cần) một file
    `.c` port, như `lwipopts.h` + `ethernetif.c`; chọn board CÓ SẴN bằng `SPLC_BOARD`, KHÔNG làm
    file board nằm ngoài thư viện. Hai nguyên tắc: (a) tài nguyên có trạng thái/phần cứng (USB,
    UART, Flash, ngắt) chỉ có MỘT chủ — SimplePLC (module port tùy chọn) hoặc host; (b) thư viện
    thuần (nanoMODBUS, cqueue) SimplePLC LUÔN dùng bản riêng đổi tên. Core chỉ thấy một "ống byte"
    (`modbus_link`); không có selector loại đường truyền. Core không phụ thuộc TinyUSB: ống
    USB-CDC là MODULE PORT tùy chọn `port/usb/` (cờ `SPLC_PORT_USB_CDC`, mặc định 0; board cần USB
    thì `#error` khi cờ = 0). Logic chờ timeout nằm trong core, một chỗ (quy ước "`read` của
    transport ngoài trả NGAY" đã bỏ vì làm mất khung > 64 B).
14. **Logger:** SimplePLC chỉ gọi `log_error/warn/info/debug` (`config/splc_log.h`) →
    `SPLC_LOG_OUTPUT(level, tag, ...)`. `SPLC_LOGGER_BUILTIN=1` (mặc định): dùng `utils/logger` +
    board gọi `logger_init`; `=0`: host cung cấp `logger.h`/logger và gọi `logger_init`, SimplePLC
    không build `logger.c` và không đưa `utils/logger` vào include path (trùng tên `logger.h`).
15. **nanoMODBUS: luôn là bản riêng đổi tên symbol** (xem mục 1). Giá ~7,5 KB Flash. Bản riêng cố
    định cấu hình mặc định nên cờ của host không đổi được nó. TinyUSB thì KHÔNG đổi tên được
    (ISR, descriptor, ngoại vi duy nhất) — chỉ một chủ.
16. **Đăng ký ống: tường minh (kiểu `netif_add`), không dùng hàm `weak`.** `splc_link_register(&ops)`
    trước `plc_engine_init()`; quên đăng ký thì `splc_link_get_transport()` trả `false` và phải
    báo lỗi rõ. Board có module USB của SimplePLC (Zigbee-IO) tự đăng ký trong `board_hw_init()`.
    `output()` KHÔNG được gọi thẳng driver không an toàn luồng: đẩy byte vào queue của task driver.
17. **Vùng Flash dữ liệu do host khai được:** option `SPLC_FLASH_DATA_ADDR` (tuỳ chọn). Không khai =
    5 sector cuối như quyết định 11. Khai = địa chỉ sector thấp nhất của 40 KB: Retain +0/+8K/+16K,
    Rule B +24K, Rule A +32K; phải chia hết 8 KB và nằm trong bank 2 (`_Static_assert`). `wg_ue`
    dùng hết 2 MB nên phải khai. **Đề xuất** bản đồ host (CHƯA được nạp): thu 3 slot app 82→80
    sector (640 KB), dữ liệu `0x081F0000..0x081F9FFF`, 1 sector trống, Scratch giữ `0x081FC000`.
    **Chưa kiểm:** bootloader/partition table của host có hard-code kích thước slot không; image
    app hiện tại có < 640 KB không; bootloader phải build + nạp lại một lần. Sửa `wg_ue` chỉ làm
    trên bản clone ở sandbox, người dùng tự chuyển sang repo chính sau khi test tốt.
18. **RS485 master qua callback:** `modbus_serial_port_t {read (không chặn), write (0 = xong, trả
    về sau khi bit cuối ra khỏi UART), set_tx (NULL nếu port tự điều khiển DE), ctx, baudrate}`;
    `modbus_rtu_master_init()` copy struct. Layer 3.5 không phụ thuộc `sx_uart`/`sx_gpio`. Board
    gateway không sở hữu ngoại vi nào; host đăng ký tường minh bằng
    `splc_gateway_set_rs485_port()`. Lưu ý host: `rs485_recv_polling(port, buf, n, 0)` tăng
    `counters.timeouts` mỗi lần rỗng — bọc callback sẽ làm nhiễu bộ đếm đó.
19. **Nhúng `wg_ue` KHÔNG sửa `usb_netif.c` và `rs485_driver.h`.** Task engine của SimplePLC tự đọc
    CDC instance 0 mỗi scan → `splc_link_input()`, ghi trả lời vào FIFO TX rồi flush (chấp nhận
    trễ tối đa 1 scan 10 ms). **Owner cổng RS485 = `RS485_OWNER_MODBUS_GATEWAY`** (đã kiểm: owner
    `MODBUS_MASTER` đang được app `modbus_master` của host acquire; `MODBUS_GATEWAY` chưa ai
    acquire). Host phải dành riêng cổng đó; trùng thì `rs485_port_acquire` lỗi và Remote I/O báo
    node offline. Cổng (USART3/6/10 = 0/1/2) do người dùng chọn bằng option `SPLC_HOST_RS485_PORT`
    trong `splcopts.h`, không fix cứng. Host chỉ sửa: 1 dòng `splc_host_start();` trong
    `gateway.c` (ngay sau `modbus_master_init();`) + CMake build `ports/splc_port/`.
20. **Kênh App trên gateway chấp nhận chỉ sống sau WiFi** (lựa chọn A, người dùng chọn):
    `GW_USB_NETIF_ENABLE` mặc định OFF trong CMake của host; USB CDC chỉ tồn tại khi bật và WiFi
    đã lên IP. Descriptor host hiện chỉ có 1 ACM (`CFG_TUD_CDC=2`). Không làm đường USB độc lập
    trong host.
21. **Tên không được trùng host.** Mọi file/symbol/guard của SimplePLC có thể trùng firmware chủ
    đều mang tiền tố `splc_`: `splc_cqueue.[ch]` (`splc_cqueue_*`, `splc_cqueue_t`),
    `splc_board.[ch]` (`splc_board_init()`), `splc_app_config.h`, `splc_nmbs.h`. `splc_cqueue` chỉ
    có `init_static` (không malloc). File/symbol MỚI thêm vào SimplePLC phải kiểm trùng với
    `wg_ue` (header basename + `nm`) trước khi coi là xong.
22. **Hai chế độ build:** `SPLC_AS_LIBRARY=OFF` (mặc định) = firmware độc lập (CMake gốc nối mọi
    layer vào executable). `=ON` = thư viện cho host: host `add_subdirectory` rồi link INTERFACE
    target `splc` — không gắn nguồn/linker option nào vào executable của host. Host PHẢI tự: thêm
    `$<TARGET_OBJECTS:splc_platform_freertos>` vào một target của nó khi `SX_OS_USE_FREERTOS=1`
    (thiếu → `undefined sx_os_yield_wait`); thêm `-Wl,--wrap=HAL_RTC_SetTime/SetDate` nếu dùng
    `SPLC_RTC_BUILTIN=1`. Đã kiểm: library mode Zigbee-IO cho cùng tập symbol với standalone.
23. **Driver cần ngoại vi CubeMX mà host có thể không có:** `SPLC_RTC_BUILTIN` / `SPLC_ADC_BUILTIN`
    (mặc định 1). `=0`: host tự cấp `sx_rtc_set_epoch/get_epoch/is_synced` / `sx_adc_init/read`,
    SimplePLC không build driver tương ứng. Gateway đặt cả hai = 0; `sx_rtc_*` trong `splc_port.c`
    là stub "chưa có giờ" (Time Window không bắn, App ghi RTC bị từ chối). **Chủ sở hữu đồng hồ
    CHƯA chốt:** NTP của host `ntp_get_unix_time()` trả giờ ĐỊA PHƯƠNG (đã cộng timezone host),
    SimplePLC nhận epoch UTC và tự áp `tz_offset_min`.

24. **Board IO + cổng RS485 (người dùng xác nhận):** bus tới board IO là **115200 8N1**
    (`SPLC_HOST_RS485_BAUD/PARITY`). Cổng dành cho SimplePLC mặc định USART6 (`SPLC_HOST_RS485_PORT=1`);
    hiện host CHƯA dùng cổng đó cho dịch vụ nào. Host phải giữ như vậy: `modbus_master_init()` gọi
    `rs485_port_reconfigure()` cho cả 3 cổng theo cấu hình host, và `splc_check_port()` đặt lại baud
    mỗi giây — nếu sau này cổng được cấu hình cho dịch vụ khác, hai bên sẽ giành nhau.

## 3. Quy trình làm việc

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Đầu phiên hoặc khi người dùng báo đã push:
  `git pull`, rồi **build/compile verify thật** (không chỉ đọc diff). Cài được:
  `apt-get install gcc-arm-none-eabi ninja-build`, `pip install cmake --break-system-packages`,
  `git submodule update --init --depth 1 simple_plc/libs/tinyusb`. Build:
  `cmake -S . -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
  -DCMAKE_BUILD_TYPE=Release [-DSPLC_OPTS_DIR=<dir>]`.
- **Giao nguyên file** (present từng file để copy-paste, giữ cây thư mục), KHÔNG patch/zip/đoạn
  thay thế; kèm danh sách file cần xoá/đổi tên. Present NGAY sau mỗi mốc. Chỉ giao code dự án +
  script test Python; KHÔNG giao harness C chạy trên PC (trừ khi người dùng yêu cầu). Việc tự kiểm
  tra nội bộ vẫn làm nhưng nêu rõ là chạy trên mô hình giả. Tính năng không test được trên board
  (vd. PVD thật) thì nói rõ là chưa test.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, từng quyết định nhỏ.
- Nói rõ mức chắc chắn khi đề xuất nguyên nhân; kiểm chứng bằng dữ liệu thật.
- Mỗi bước mới: code → build ARM cả Zigbee-IO và Gateway → người dùng nạp + chạy test trên board
  → cập nhật file này.

## 4. Kế hoạch

**A. Nhúng vào `wg_ue` (ưu tiên, theo thứ tự; mỗi bước build + kiểm riêng)**
1. *(người dùng)* Push các thay đổi quyết định 21–23 (phần đã build sạch, chưa nằm trong repo
   nếu chưa push).
2. Đặt SimplePLC vào `wg_ue` dưới dạng submodule `libs/simple_plc` (cùng kiểu `tinyusb`/`logger`).
   Viết `ports/splc_port/`: `splc_port.c/.h`, `splc_port_cfg.h` (option riêng của port, vd.
   `SPLC_HOST_RS485_PORT`, baud/parity), `splcopts.h`, `CMakeLists.txt`. **Đã có nháp ở `port_gw/`
   (thiếu `CMakeLists.txt`)**, đã đối chiếu với `wg_ue` thật: owner `RS485_OWNER_MODBUS_GATEWAY` chưa ai
   acquire; CDC instance 0 là ACM duy nhất (descriptor chỉ khai 1 ACM, mạng là ECM/RNDIS riêng; host
   không gọi `tud_cdc_*`); `g_usb_tusb_initialized` có thật (`Core/Src/usb.c`); `rs485_send` tự điều khiển DE
   và chờ TC nên `set_tx=NULL` đúng; host chưa có `hrtc`/ADC nên RTC/ADC=0 + stub. Nội dung: task engine
   (`plc_engine_init` rồi `plc_engine_poll`), bơm CDC0 → `splc_link_input()`, `output()` qua FIFO
   TX, bọc `rs485_send/recv_polling` thành `modbus_serial_port_t`, `splc_host_start()`, stub
   `sx_rtc_*`. `SPLC_LOGGER_BUILTIN=0`, `SPLC_RTC_BUILTIN=0`, `SPLC_ADC_BUILTIN=0`. **CẦN đọc
   trước:** `Documents/Rules/*` (CLAUDE.md của `wg_ue` bắt buộc), `ports/modbus_port/mmb_serial.*`,
   `Drivers/uart/rs485_driver.h`, `usb_netif.c` + `tusb_config.h` (ai gọi `tud_task_ext`), cờ NMBS
   thật của `middleware/nanomodbus`, `gateway.c`.
3. *(đã làm một phần)* Binary Gateway không có symbol trần `cqueue_*`/`board_init`/`nmbs_*`/
   `HAL_UART_RxCpltCallback`/`PVD_AVD_IRQHandler`; header `board_*.h`/`splc_*.h` không trùng file nào
   của `wg_ue`. **Còn lại:** `nm` trên binary `wg_ue` thật sau khi nối CMake. Dò xung đột với host bằng
   `nm`/so header basename: `HAL_UART_RxCpltCallback`,
   `PVD_AVD_IRQHandler`, `cqueue_*`, `board_init`, `nmbs_*`, `logger.h`. Kiểm nanoMODBUS riêng
   cùng tồn tại với bản `wg_ue` thật (còn nợ từ quyết định 15).
4. Thử build gateway kèm `splc_port.c` bằng ARM gcc. Baseline `wg_ue` từng không configure được vì
   thiếu submodule `libs/x-cube-st67w61` (chưa thử lại) → nếu vẫn vậy, tách bản build rút gọn chỉ
   để kiểm link. Dòng `splc_host_start();` trong `gateway.c` sửa trên bản clone.
5. *(cần người dùng)* **Bootloader + Flash:** đọc `libs/bootloader/flash_define.h`, `bootloader/`,
   `STM32H563xx_FLASH.ld`; trả lời các điều "chưa kiểm" của quyết định 17. Cho tới lúc đó địa chỉ
   `0x081F0000` chỉ là đề xuất, KHÔNG nạp.
6. *(cần người dùng)* Chốt chủ sở hữu đồng hồ (quyết định 23) nếu cần Time Window trên gateway.
7. *(cần người dùng/phần cứng)* **Firmware slave cho board IO** (khối `0x0C00..0x0C06` bằng
   nanoMODBUS; lease/SAFE nằm ở slave; chốt baud/parity — đang giả định 115200 8N1). Thiếu nó
   thì gateway chạy nhưng DI/DO ảo luôn offline → chặn test thật.
8. *(cần phần cứng)* Nạp + test: bật `GW_USB_NETIF_ENABLE=1`, WiFi lên IP, mở COM bằng Studio,
   chạy `test_plc.py`; đo độ trễ khi task engine và task USB chạy cùng lúc; kích thước ring phải
   đủ khung dài nhất (256 B RTU) khi engine ngủ một scan 10 ms.

**A'. Rủi ro của port đã thấy khi đối chiếu `wg_ue` (chưa đo):**
- Task `SPLC` priority 6 trùng `UART_LOG` (6), thấp hơn MODBUS/BROKER/SHELL (11) và NETWORK (12) → khi
  gateway bận scan 10 ms có thể trễ. Đo trên board.
- FIFO CDC của TinyUSB: engine đọc/ghi từ task khác `usb_netif_task` (đang chạy `tud_task_ext`); đang
  giả định mutex FIFO của TinyUSB (`CFG_TUSB_OS=FreeRTOS`) bảo vệ — chưa kiểm cấu hình trong tinyusb của host.
- `plc_rule_flash` ghi Flash trong, host còn bootloader/OTA: tuyệt đối không chồng thao tác (bước 5).
- Heap host 320 KB (`configTOTAL_HEAP_SIZE`): task `SPLC` 1536 word = 6 KB, tính vào heap.

**B. Remote I/O còn mở:** chưa phát hiện trùng `unit_id` giữa hai hàng bảng trên cùng bus; chưa có
tag báo "node offline" cho App.

**C. App + spec (không gấp):** `RuleCompiler.cs` (~dòng 260) chỉ gửi `retainTagIndex` khi CV là
VREG_RETAIN (giá trị dự phòng 84 sai trên board này, RETAIN bắt đầu ở tag 72); đọc `0x0B00..0x0B7F`
một FC03 128 reg bị từ chối (tối đa 125) → App tách 64+64; sửa Structs 3.6/3.7 và Wire Contract
9.3 (quyền R/W, `+6`, `rule_ref`); App sinh rule "đúng giờ" `EQ, Lo=Hi`.

**D. Firmware khác:** `components/usb_cdc/` và `port/modbus_usb/` vẫn còn trong repo (không được build,
chỉ còn trong comment; `sx_usb_cdc.h` còn include `cqueue.h` cũ) — xoá khi người dùng đồng ý; validate `guard/trigger/action_tag` ∈ `0..MAX_TAGS-1` khi nạp rule (hiện index
sai chỉ vô hiệu hoá rule âm thầm); chưa ghi snapshot retain ngay trước REBOOT; lỗi Flash lúc erase
trong bước copy A→B huỷ bản cũ trước khi biết lỗi; `ACT_WRITE_REMOTE/LOG_EVENT/SEND_ALARM` chưa
implement; không có log trước `logger_init` (đọc Flash trong `plc_engine_init()` chạy TRƯỚC
`splc_board_init()`); `utils/filter/*` chưa soát; linker `STM32H523xx_FLASH.ld` khai `LENGTH=512K`
(chip 256 KB — HỎI người dùng trước khi sửa, quyết định 9); comment cũ nhắc `modbus_usb`/
`sx_usb_cdc`/`FREE_RTOS` còn ở `plc_modbus_cfg.h`, `services/CMakeLists.txt`, `app/CMakeLists.txt`,
`modbus_transport.h`, `splc_board.h`.

## 5. Bài học (luật rút ra — đừng lặp lại)

1. Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới.
2. Bằng chứng trước khi kết luận: log là dấu vân tay của phiên bản code; lọc theo từ khoá.
3. Test chỉ có nghĩa nếu trạng thái đầu xác định (reset board, kiểm `DO0=0` trước rule thử; test
   persistence phải kiểm dữ liệu sống sót qua reboot TRƯỚC khi xoá).
4. Timeout đếm bằng vòng lặp không phải timeout: đo bằng tick thật.
5. Đừng suy ngữ nghĩa field từ tên hay từ một phía; đọc UI/compiler của App và hỏi người dùng.
6. Flash giả mô phỏng mất điện: sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA.
7. **Việc chưa giao thì mất khi phiên kết thúc** (đã mất cả bản gateway một lần, rồi mất cả đợt đổi
   tên + `wg_ue/ports/splc_port` một lần nữa vì hết token trước khi present/push): present nguyên
   file ngay sau mỗi mốc; người dùng push ngay.
8. Không code hành động phá huỷ dữ liệu khi spec mơ hồ — hỏi phạm vi trước.
9. Đổi giá trị mặc định/cấu hình: kiểm cả bản `-D`/override, giá trị sai (`#error`), thiếu
   `splcopts.h`, không chỉ bản mặc định.
10. Hiểu kiến trúc trước khi code: hỏi "thiết bị này có cần chạy PLC không?" (board IO chỉ cần là
    slave RS485).
11. Đừng tuyên bố môi trường thiếu công cụ khi chưa thử cài.
12. Hợp đồng core↔port: core giữ phần khó (timeout, ghép khung nhiều mảnh); đọc comment lịch sử
    bug trước khi đề xuất đổi hợp đồng.
13. Test xanh chưa chứng minh gì: phá code có chủ đích (đột biến) và xem test có đỏ không; với đa
    luồng, hạ memory order xuống `relaxed` rồi xem ThreadSanitizer có báo race không. Đột biến
    phải nhắm đúng chỗ (host giả phải đủ chậm mới lộ lỗi idle timeout).
14. Đo trước khi ghi con số vào comment/tài liệu. Khẳng định về host ("API giống hệt") phải nêu đã
    so cái gì và CHƯA so cái gì.
15. Kế hoạch đổi tên symbol phải dựa trên `nm`, không dựa trên quy ước đặt tên (bỏ sót 7 hàm
    `recv_*_res`); cần bước kiểm TỰ ĐỘNG sau build.
16. Bỏ một phụ thuộc bắc cầu thì build lại MỌI cấu hình (gỡ `tinyusb` làm `sx_os_freertos.c` mất
    `FreeRTOS.h`). Test host với harness giả chưa chứng minh gì về CMake.
17. Test chế độ library phải dùng host thật hoặc host giả đủ việc: host giả thiếu bước thêm object
    FreeRTOS cho ra lỗi link giả; ngược lại phải thử cả ca thiếu (`BUILTIN=0` không stub phải
    FAIL link) lẫn ca đủ.
18. Handoff cũ có thể nói đã xoá file nhưng repo vẫn còn (`usb_cdc`, `modbus_usb`): kiểm bằng
    `git status`/`ls` trước khi tin.

19. **Build baseline TRƯỚC khi tin handoff/commit.** Commit đổi tên (`678ae47`) bỏ sót `#include "board.h"`
    ở 2 file board → Zigbee-IO không build; quyết định 23 ghi "đã build sạch" nhưng
    `platforms/stm32/stm32h5/CMakeLists.txt` vẫn luôn biên dịch driver RTC/ADC (cờ `*_BUILTIN` chỉ dùng cho
    `--wrap`). Chỉ phát hiện nhờ build thật + thử ca `BUILTIN=0`.
20. Khi đo kích thước bằng link giả (`--defsym`), đừng gán symbol mà kernel/driver THẬT của project cũng
    định nghĩa (vd. `xTaskCreate`): nó loại cả heap khỏi link, RAM tụt 72→39 KB một cách giả. Chỉ `defsym`
    symbol thuần của host, và đừng ghi con số đó vào tài liệu.

## 6. Verify & test

**Script test trên board** (`python -m pip install pymodbus pyserial`; dùng đúng interpreter).
Các script ghi Flash/xoá rule — chỉ chạy trên board dev:

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | descriptor, nạp rule, commit, live watch |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, engine dừng thật, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD |
| `test_rtc.py COM14 [--reboot]` | khối RTC, từ chối ghi sai, tốc độ đồng hồ, Time Window |
| `test_fb.py COM14 [--reboot] [--probe-64]` | khối FB (nháp/COMMIT, từ chối, CV/Q, persistence); `--probe-64`: FC16 73 byte |
| `test_retain.py COM14 [--commits N] [--periodic]` | retain qua REBOOT, xoay vòng sector, CLEAR_RETAIN; `--periodic` ~6 phút |
| `test_sysclear.py COM14 [--reboot]` | 3 lệnh xoá, persistence, lệnh trong diag, lệnh sai |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`.

**Kiểm cấu hình (build ARM) mỗi khi đụng CMake/option:** mặc định (Zigbee-IO);
`SPLC_BOARD_GATEWAY` qua `-DSPLC_OPTS_DIR`; `SPLC_FLASH_SIZE_KB` 128/256/512/… (sai phải
`#error`/`_Static_assert`); `SX_OS_USE_FREERTOS=1` (cần `-DSPLC_OS_INCLUDE_DIRS` hoặc stm32cubemx);
`MAX_RULES=50`; `SPLC_LOGGER_BUILTIN=0` (cần logger của host mới link được);
`SPLC_RTC_BUILTIN=0`/`SPLC_ADC_BUILTIN=0` (standalone phải FAIL link thiếu `sx_rtc_*`/`sx_adc_read` — đã
kiểm cả Zigbee-IO lẫn Gateway; có `splc_port.c` thì link qua);
`SPLC_AS_LIBRARY=ON` (host giả link `splc`); thiếu `splcopts.h` phải `FATAL_ERROR` lúc configure.
So FLASH/RAM với số đo chuẩn ở mục 0.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 không có symbol `sx_usb_*`; Layer 2 không có
`sx_flash_*`/`sx_usb_*`; `plc_rule_flash.c` PHẢI có `sx_flash_*`; `sx_system_reset` chỉ được gọi từ
`plc_system_cmd_service.c`; chỉ `port/usb/splc_usb_cdc.c` được `#include "tusb.h"`; `modbus_link.c`
không có `sx_usb_*`/`tud_*`; không file nào ngoài `libs/splc_nanomodbus/` được
`#include "nanomodbus.h"`; `nm` trên `libsplc_nanomodbus.a` không có symbol toàn cục thiếu tiền tố
`splc_`; `nm` trên binary không có symbol `cqueue_*`/`board_init`/`nmbs_*` không tiền tố.

**Kiểm trên PC (không cần board, mô hình giả — nói rõ khi báo cáo):** Layer 2 (`plc_tag.c`,
`plc_rule.c`, `plc_internal_rule/*.c`) biên dịch bằng `gcc -std=c11` + stub,
`sizeof(SPLC_RuleRecord)==32`; `modbus_link` (đồng hồ giả + 2 thread thật, ThreadSanitizer);
`plc_io_remote.c` + `modbus_serial.c` với slave RS485 giả (sync, ghi DO, lease, DI, offline/online
lại, slave reset, CRC sai, dữ liệu thừa). Harness KHÔNG nằm trong repo; muốn giữ thì đặt vào
`test/` kèm target CMake PC — quyết định của người dùng.