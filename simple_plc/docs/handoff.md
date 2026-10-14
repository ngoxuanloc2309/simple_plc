# SimplePLC — Handoff

> Cho phiên làm việc sau nắm "đang ở đâu, làm gì tiếp, luật gì phải giữ". Đọc SAU
> `Readme.md` và `docs/architecture.md`. File này chỉ giữ: trạng thái hiện tại, quyết định
> thiết kế đã chốt, kế hoạch, quy trình, bài học. Lịch sử từng bước đã bỏ — xem `git log`.
> Số thứ tự quyết định được GIỮ NGUYÊN để các tài liệu khác còn tham chiếu đúng (có chỗ trống).
>
> **Quy tắc vàng:** trước khi tin điều gì dưới đây, chạy `git pull && git log --oneline -10`.
> Code lệch file này thì tin code và sửa file này.

**Nguồn tài liệu** (V2.0 là strict superset của V1.9; mâu thuẫn giữa các tài liệu → tin code thật của App
`paa` và firmware, rồi sửa tài liệu):
1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` — struct/memory map (nguồn chính).
2. `docs/SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md` — khối MQTT (Telemetry `0x0C00`, Broker `0x0E00`);
   layout đúng = Structs 3.9/3.10 + `splc_mqtt_wire.h` + golden vector.
3. `docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md` — checklist tuân thủ. Còn lỗi: mục 11.2 ghi FC16 là
   `0x16` (đúng `0x10`); chưa có mục MQTT; giả định 124 tag cố định của Remote I/O.
4. `docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md` — **layout khối Telemetry SAI**, code mẫu `memcpy`
   thẳng vào struct. KHÔNG dùng làm nguồn; đã báo đội App sửa (quyết định 33).
5. `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` — chỉ còn đúng cho LOGIC rule (Trigger→Compare→Dwell→Guard→Action).
6. `docs/plc_mqtt_design.md` — thiết kế chi tiết MQTT Publish (quyết định 35).

Các tài liệu MQTT dẫn link `g:/HoaNV/...` (máy đội App) không mở được; file thật nằm trong repo `paa`.

**Repo liên quan**
- App: `ngoxuanloc2309/paa` (.NET, SimplePLC.Studio; bản đã đọc `main` `8056a62`). Bug xuyên App+firmware phải đọc
  CẢ HAI repo (`RuleCompiler.cs`, `RuleTableWriter.cs`, `FunctionBlockGateway.cs`, `MqttTelemetryCodec.cs`,
  `MqttBrokerConfigCodec.cs`). Luôn `git pull` App trước khi kết luận "App chưa có X".
- Firmware gateway thật (host nhúng SimplePLC): `ngoxuanloc2309/wg_ue` (STM32H563, FreeRTOS + lwIP, 3 cổng RS485
  USART3/6/10, TinyUSB do host sở hữu; bản đã đọc `main` `60087b2`).

## 0. Trạng thái (nhánh `board_dev_gw`)

**Đã chạy trên board Zigbee-IO (STM32H523CCU6):** Wire Profile V2.0 (diag, retain, xoá lệnh, RTC + Time Window,
FB `0x0B00..0x0B7F`) qua USB CDC; `test_fb.py --probe-64`, `test_plc.py`, `test_retain.py` ALL PASS.

**Đã chạy trên board gateway (`wg_ue`):**
- SimplePLC khởi động, App link RS485 qua USART6 (`test_plc.py COM35 --period 1000 --baud 115200 --unit 1` → 6/6
  phản hồi; mới 6 mẫu, chưa soak), chỉ sau khi thay IC MAX485 (quyết định 30). `PLC_IO_REMOTE` báo node 0 offline
  vì chưa có firmware slave.
- **MQTT Publish (E.1–E.6):** port `wg_ue/ports/splc_port/splc_mqtt_port.c` chạy trên board (log `PLC_MQTT`,
  `SPLC_MQTT`). Người dùng xác nhận đã test: đổi broker bằng App/Python khi đang connected (host tự ngắt và nối
  broker mới, log `generation N != connection M: reconnecting`) và khi offline → có mạng lại (nối đúng broker đã
  cấu hình). Kết quả chi tiết `test_mqtt.py` chưa ghi ở đây.
- Heap host lúc rảnh (log `MONITOR`, CPU=1%): `Heap used=205/293KB, lowest free=61KB` — đạt điều kiện ≥ ~30 KB của
  quyết định 26, nhưng là số lúc rảnh; CHƯA đo dưới tải.

**Build ARM sạch nhưng CHƯA chạy trên board:** Zigbee-IO qua RS485 (29); đổi tên chống trùng host (21),
`SPLC_AS_LIBRARY` (22), `SPLC_RTC/ADC_BUILTIN` (23), `SPLC_FLASH_DATA_ADDR` (17), chế độ USB CDC-only của gateway (28);
Remote I/O qua RS485 (`plc_io_remote`, `modbus_serial`).

**Số đo build chuẩn** (ARM Release, arm-none-eabi-gcc 13.2.1; `SPLC_MQTT_ENABLE=0`) — dùng để bắt hồi quy:
Zigbee-IO FLASH 80 264 B / RAM 77 664 B (FLASH 80 200 B trước E.3; đo lại khi đổi code); Gateway chuẩn FLASH 55 356 B /
RAM 72 624 B (`SPLC_BOARD_GATEWAY`, `SX_OS_USE_FREERTOS=0`, `SPLC_PORT_USB_CDC=0`, FLASH 256 KB, `-DSPLC_OPTS_DIR`).
MQTT bật trên Zigbee-IO: RAM 82 104 B (+4 440), FLASH 85 976 B (+5 712). Gateway với `splcopts.h` của host (FreeRTOS,
logger host) KHÔNG so được với số trên. So FLASH giữa hai máy phải dùng hiệu số (bài học 24).
**Lưu ý:** `splc_config/splcopts.h` trong repo hiện đặt `SPLC_MQTT_ENABLE 1` (và `SPLC_PORT_USB_CDC 1`,
`SX_OS_USE_FREERTOS 1` dù comment ghi "RS485 variant") ⇒ build mặc định KHÔNG ra số chuẩn; đo baseline phải dùng
`-DSPLC_OPTS_DIR` trỏ tới `splcopts.h` có `SPLC_MQTT_ENABLE=0`.

**Nhúng vào `wg_ue`:** SimplePLC thêm **+22 464 B RAM, +26 780 B FLASH** (số đo trước MQTT, GCC 14.3.1, cờ của
`build.bat`). RAM host gần đầy: `configTOTAL_HEAP_SIZE` = 296 KB, dư 2 992 B RAM tĩnh (quyết định 26); số đo sau khi bật
MQTT chưa ghi.

**Chưa làm:** bản đồ Flash host (4.A.5), firmware slave cho board IO, chủ đồng hồ gateway (23), khoá Flash chung với OTA
(hoãn, 27), MQTT Subscribe (34).

**Chưa verify (không chặn việc tiếp):** chạy lại `test_plc/test_fb/test_retain` với firmware sau các thay đổi
đổi-tên/config/OS/Flash/logger; Remote I/O với board IO thật (timing RS485, nhả DE); FreeRTOS kernel thật (độ trễ task
lúc erase Flash); PVD thật; mất điện giữa lúc ghi Flash; `test_rtc.py --reboot`; đường RS485 của Zigbee-IO; hạ firmware
`MQTT=1` → `MQTT=0` (bản ghi Flash bị coi là hỏng → phải Deploy lại).

**Vấn đề mạng của host `wg_ue` (KHÔNG thuộc SimplePLC; đọc code + log, chưa sửa):**
- `network_manager.c` probe DNS mỗi 20 s (timeout 10 s); fail liên tiếp ≥ 2 ⇒ interface active bị demote (chờ
  120 s, gấp đôi mỗi lần demote liên tiếp, tối đa 900 s) ⇒ route NONE ⇒ MQTT bị ngắt, nối lại khi WiFi được bầu lại.
  Trông như "ngắt định kỳ"; MQTT client không có timer ngắt nào.
- `network_probe_hostname()` đọc `g_sys_config.mqtt.broker_url` (broker mặc định của host), KHÔNG đọc override của
  SimplePLC ⇒ DNS chậm với tên mặc định cũng làm WiFi bị cắt dù broker đã cấu hình chạy tốt. Chưa biết DNS router
  `192.168.110.1` có chậm thật không (kiểm bằng `nslookup`).
- ETH liên tục `DHCP timeout` (rx=0) ⇒ không có đường dự phòng; modem LTE `a7680c` có log `AT< (ERROR)`, chưa rõ lệnh nào.
- COMMIT lúc gateway offline: override có hiệu lực ngay ở lần thử kế (host đọc override mỗi lần bắt đầu DNS/connect) nhưng
  port chỉ ép reconnect khi `connected && conn_gen != gen` ⇒ phải chờ hết backoff (tới 60 s). Hành vi đúng, chỉ chậm.
- Rủi ro hẹp: DNS đã resolve host cũ rồi override đổi trước khi dựng lệnh connect ⇒ dùng `s_resolved_ip` cũ với
  username/client_id mới.
- Muốn sửa (cần người dùng đồng ý, đây là file host): probe dùng host của override hoặc `pool.ntp.org`; nới
  `PROBE_FAIL_THRESHOLD/TIMEOUT_MS`; reset backoff khi cấu hình đổi lúc offline.

## 1. Cơ chế cần giữ (không sửa lại trừ khi có lý do mới)

- **Scan loop** (`app/plc_app/plc_engine.c`, 10 ms): `diag_tick → input_scan → [engine không bị diag treo:
  rule_scan + plc_fb_scan] → output_scan → plc_mqtt_scan (bỏ qua khi diag treo) → modbus_config_service →
  retain_service → ghi scan_time → plc_system_cmd_service` (cuối cùng vì có thể reset).
- **Rule Table lưu Flash A/B** (`plc_rule_flash`): copy A→B, erase+ghi A, đọc lại kiểm CRC và seq_num mới, lỗi thì
  B→A. Đoạn FB (128 B; bản cũ 112 B vẫn nạp được) nằm TRONG bản ghi: bit 15 `rule_count` = có FB, bit 14 = FB 128 B,
  bit 13 = có đoạn MQTT 960 B (chỉ khi `SPLC_MQTT_ENABLE=1`; tắt thì bit 13 vẫn thuộc số rule, mask 0x3FFF),
  bit 0..12 = số rule; bố cục `header | rule | FB | MQTT`, một CRC phủ tất cả. Mọi code đọc trường này phải tách cờ
  trước. Mật khẩu broker nằm nguyên văn trong đoạn MQTT.
- **CRC Rule/Retain tính trên byte WIRE** (`rule_table_wire_crc16()`). `nmbs_crc_calc()` trả kết quả hoán byte
  (chỉ cho RTU framing).
- **Retain** (`plc_retain`): log xoay vòng 3 sector, record 208 B; lưu khi COMMIT_RETAIN, chu kỳ
  `RETAIN_SNAPSHOT_PERIOD_MS` (chỉ khi dữ liệu đổi), PVD khẩn cấp (`retain_emergency_snapshot()` trong ISR: không
  erase, không log, hoãn nếu vòng chính đang thao tác Flash). Mọi thao tác Flash ngoài `plc_retain.c` phải bọc
  `retain_flash_op_begin/end()`.
- **PVD** nối bằng file dự án (`stm32h5_pwd.c` định nghĩa `PVD_AVD_IRQHandler`); nếu CubeMX tự sinh handler thì định
  nghĩa `SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX`. Tắt bằng `PLC_PVD_EMERGENCY_SAVE_ENABLE 0`.
- **System command:** luôn chạy ngay (bất kể diag/dirty). REBOOT trì hoãn `PLC_REBOOT_DELAY_MS` (300 ms) cho phản hồi
  kịp ra — ĐỪNG bỏ. CLEAR_RULES/CLEAR_RETAIN/FACTORY_RESET ghi một bản ghi RỖNG hợp lệ qua đường lưu đã verify;
  FACTORY_RESET = CLEAR_RETAIN + CLEAR_RULES, không reboot (CLEAR_RULES/FACTORY_RESET xoá cả cấu hình MQTT, có
  backup/rollback nếu lưu lỗi).
- **Giới hạn Modbus:** FC16 ≤ 123 reg, FC03 ≤ 125 reg. Gói USB FS 64 B nên App chia 1 rule (41 B)/lệnh.
  `plc_modbus_cfg` đặt `read_timeout=0` nhưng `byte_timeout>0` sau khi request đã bắt đầu — nhờ đó mảnh gói thứ hai
  được ghép vào khung.
- **`modbus_link` (ống byte App):** core giữ ring RX (SPSC lock-free) và MỌI timeout (`read` chờ bằng tick thật;
  `write` timeout **idle**). Hợp đồng luồng: `splc_link_input()` từ đúng MỘT ngữ cảnh (task hoặc ISR, không chặn);
  `output()/process()/transport` chạy ở ngữ cảnh engine; `register` chỉ lúc khởi tạo. Chi tiết: `architecture.md` 3.1.
- **nanoMODBUS riêng (`libs/splc_nanomodbus`):** `splc_nmbs.h` đổi tên 37 hàm toàn cục, `#undef` mọi macro `NMBS_*`,
  include header vendored bằng đường dẫn tương đối; `libs/nanomodbus/` giữ nguyên. `splc_check_symbols.cmake` chạy
  sau build; nâng cấp thư viện mà có hàm toàn cục mới thì build fail → thêm một `#define`.
- **Multi-board:** `SPLC_TagLayout` truyền qua tham số; chọn bằng `SPLC_BOARD` trong `splcopts.h` (bảng ở
  `cmake_modules/splc_select.cmake`); thêm board = 1 file `board_<sku>.c` + 1 hàng bảng. `MAX_TAGS` ở
  `board/board_tag_define.h`.
- **Remote I/O (Gateway):** `plc_io.c` gọi `plc_io_remote_input_scan()` cuối `input_scan()` và
  `plc_io_remote_output_scan()` cuối `output_scan()` (tối đa 1 request RS485 mới/scan, không bao giờ chặn). Ưu tiên:
  SYNC > WRITE > REFRESH > POLL (architecture 5.1). Node offline: tag DI giữ giá trị cuối; rule cần biết thì dùng
  `plc_io_remote_node_online()`.
- **Cấu hình kiểu lwIP:** người dùng chỉ sửa `splcopts.h`; `config/splc_opt.h` đặt mặc định + `#error` khi sai. Thêm
  option = thêm vào `splc_opt.h` (mặc định + check) và `splcopts_template.h`. CMake đọc `SPLC_BOARD`, `SPLC_PLATFORM`,
  `SPLC_LOGGER_BUILTIN`, `SPLC_PORT_USB_CDC`, `SPLC_RTC_BUILTIN`, `SPLC_ADC_BUILTIN` thẳng từ `splcopts.h`; `-D` chỉ có
  tác dụng khi `splcopts.h` không định nghĩa trùng. KHÔNG phải option: hằng wire, kích thước sector, tag layout.
- **Chế độ OS:** `SX_OS_USE_FREERTOS=0` bare-metal; `=1` host gọi `plc_engine_init()` rồi `plc_engine_poll()` từ MỘT
  task — thư viện đơn luồng không khoá, task khác KHÔNG được đụng tag/rule/draft/retain; không log từ ISR.
  `sx_get_tick_ms()` giữ `HAL_GetTick()`.

## 2. Quyết định đã chốt (người dùng xác nhận — ĐỪNG tự đổi, hỏi lại nếu nghi)

**Wire / engine**
1. `protocol_version = 2`.
2. RTC = RTC nội STM32H5, clock **LSI**, không VBAT/LSE; mất điện → chưa đồng bộ, reset mềm giữ giờ. Linker
   `--wrap=HAL_RTC_SetTime/SetDate` để `MX_RTC_Init()` không xoá giờ. `status_flags` (`0x0813`) RO; `tz_offset_min`
   chỉ trong RAM (sau REBOOT về 0, Time Window im đến khi Studio ghi lại).
3. Time Window theo Structs v2.0 mục 7: `Lo<=Hi` khung trong ngày, `Lo>Hi` qua nửa đêm (bắn mỗi scan khi đúng);
   `compare_op==EQ` và `Lo==Hi` = mốc phút (bắn 1 lần ở sườn lên); `trigger_tag` bị bỏ qua.
4. SYSTEM_COMMAND luôn chạy ngay; dirty interlock chỉ áp cho `CMD_EXIT_DIAG`. Trong diag, trước SYSTEM_COMMAND, tag host
   đã ghi về 0. Thoát diag (EXIT/hết lease): `rule_runtime_reset()` mọi rule như vừa nạp.
5. `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload Flash. Retain dùng `plc_retain.c` (lệch tài liệu Appendix A có chủ đích).
6. **FB "dual generation":** App vẫn sinh macro rule (Timer/Counter chạy bằng Rule Engine) VÀ ghi bảng FB; firmware chỉ
   lưu cấu hình và BÁO CÁO. `0x0B00..0x0B7F` R/W; Host sở hữu `mode`, `pt_ms`/`preset`, thanh ghi `+6`; firmware sở hữu
   `status_bits`, `et_ms`/`current_value`, `reserved`. Timer 1=TON 2=TOF 3=TP; Counter 1=CTU 2=CTD; 0 = không dùng.
7. **Counter:** `+6` = index toàn cục tag chứa CV (VFLAG/VREG/VREG_RETAIN/COUNTER; `0xFFFF` = không có; DI/DO/AI/Modbus
   bị từ chối `0x03`; không hai counter trùng tag). Firmware chỉ ĐỌC tag: `Q` = CTU `CV>=PV`, CTD `CV<=0`.
   **Timer:** `+6` = `rule_ref` (rule index + 1, 0 = chưa gắn).
8. **FB qua bản nháp:** ghi vào nháp (validate ngay, all-or-nothing, `0x02/0x03`), đọc `0x0B00..` luôn trả cấu hình
   ĐANG CHẠY. COMMIT: khối có cờ nhận nháp, khối không ghi → DISABLED, rồi lưu Flash cùng Rule Table. Nháp bị xoá sau
   COMMIT (kể cả lỗi), `CLEAR_RULES`/`FACTORY_RESET`, và khi boot ⇒ mỗi Deploy App ghi lại FB từ đầu.
9. **Không sửa file CubeMX tự sinh, kể cả khối `USER CODE`.** Cần hành vi khác → chặn ở lớp thấp (linker `--wrap`, lớp
   `sx_*`). Thấy bug thật trong file sinh (vd. `STM32H523xx_FLASH.ld` khai `LENGTH=512K` trên chip 256 KB) → báo, HỎI
   trước khi sửa. Ngoại lệ có chủ đích: `FreeRTOSConfig.h` của `wg_ue` (quyết định 26).
10. `0x0A20` đọc trả lệnh cuối đã ghi. Layout tag board Zigbee-IO là DENSE (DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71,
    RETAIN 72-103, COUNTER 104-111), khác bảng cố định Structs mục 4 — code validate theo KIND; test script tính index từ
    `DeviceResourceInfo`.
11. **Flash:** vùng dữ liệu (Rule A/B + Retain) LUÔN là 5 sector cuối, tính ngược từ `END = FLASH_BASE +
    SPLC_FLASH_SIZE_KB*1024`. Sector 8 KB là hằng họ H5; giả định dual-bank chia đôi. Kiểm lúc biên dịch: bội số 16 KB,
    128..2048, 5 sector nằm trong bank 2.

**Kiến trúc nhúng**
12. **Gateway:** SimplePLC chạy trên gateway; board IO là **slave RS485 thuần** (nhận lệnh DO, được hỏi thì trả DI) —
    KHÔNG chạy SimplePLC/Rule Engine/Flash rule. DI/DO ảo ↔ (cổng RS485, unit ID, bit) khai cứng lúc biên dịch trong
    file board (`GATEWAY_IO_NODES`), thiết kế theo dữ liệu. Link: khối `0x0C00..0x0C06` (architecture 5.1).
13. **Nhúng vào firmware chủ (kiểu lwIP):** người dùng chỉ viết `splcopts.h` và (khi cần) một file `.c` port; chọn board
    CÓ SẴN bằng `SPLC_BOARD`. (a) tài nguyên có trạng thái/phần cứng (USB, UART, Flash, ngắt) chỉ có MỘT chủ; (b) thư
    viện thuần (nanoMODBUS, cqueue) SimplePLC LUÔN dùng bản riêng đổi tên. Core chỉ thấy một "ống byte" (`modbus_link`),
    không có selector loại đường truyền; core không phụ thuộc TinyUSB — USB-CDC là module port tùy chọn `port/usb/`
    (`SPLC_PORT_USB_CDC`, mặc định 0). Logic chờ timeout nằm trong core, một chỗ.
14. **Logger:** SimplePLC chỉ gọi `log_error/warn/info/debug` (`config/splc_log.h`) → `SPLC_LOG_OUTPUT`.
    `SPLC_LOGGER_BUILTIN=1` (mặc định): dùng `utils/logger`; `=0`: host cung cấp `logger.h` và gọi `logger_init`.
15. **nanoMODBUS luôn là bản riêng đổi tên symbol** (~7,5 KB Flash). TinyUSB KHÔNG đổi tên được — chỉ một chủ.
16. **Đăng ký ống tường minh (kiểu `netif_add`), không dùng hàm `weak`:** `splc_link_register(&ops)` trước
    `plc_engine_init()`; quên đăng ký thì `splc_link_get_transport()` trả `false` và phải báo lỗi rõ. `output()` KHÔNG
    được gọi thẳng driver không an toàn luồng: đẩy byte vào queue của task driver.
17. **Vùng Flash dữ liệu do host khai được:** `SPLC_FLASH_DATA_ADDR` (tuỳ chọn). Khai = địa chỉ sector thấp nhất của
    40 KB: Retain +0/+8K/+16K, Rule B +24K, Rule A +32K; chia hết 8 KB, trong bank 2 (`_Static_assert`). `wg_ue` dùng
    hết 2 MB nên phải khai. Bản đồ đề xuất cho host (CHƯA nạp): xem 4.A.5.
18. **RS485 master qua callback:** `modbus_serial_port_t {read (không chặn), write (0 = xong, trả về sau khi bit cuối ra
    khỏi UART), set_tx (NULL nếu port tự điều khiển DE), ctx, baudrate}`; Layer 3.5 không phụ thuộc `sx_uart/sx_gpio`.
    Host đăng ký bằng `splc_gateway_set_rs485_port()`. Lưu ý: `rs485_recv_polling(port, buf, n, 0)` của host tăng
    `counters.timeouts` mỗi lần rỗng.
19. **Nhúng `wg_ue` KHÔNG sửa `usb_netif.c` và `rs485_driver.h`.** Task engine tự đọc CDC instance 0 mỗi scan →
    `splc_link_input()`, ghi trả lời vào FIFO TX rồi flush (trễ tối đa 1 scan). Owner cổng RS485 =
    `RS485_OWNER_MODBUS_GATEWAY`; host phải dành riêng cổng. Cổng do `SPLC_HOST_RS485_PORT` (0/1/2 = USART3/6/10).
    Host chỉ sửa: 1 dòng `splc_host_start();` trong `gateway.c` (sau `modbus_master_init();`) + CMake `ports/splc_port/`.
20. *(thay thế một phần bởi 28)* Kênh App gateway qua USB chỉ sống sau WiFi; `GW_USB_NETIF_ENABLE` mặc định OFF.
21. **Tên không được trùng host.** Mọi file/symbol/guard có thể trùng firmware chủ mang tiền tố `splc_`
    (`splc_cqueue`, `splc_board`, `splc_app_config.h`, `splc_nmbs.h`). File/symbol MỚI phải kiểm trùng với `wg_ue`
    (header basename + `nm`) trước khi coi là xong. (Tên `plc_mqtt*`/`splc_mqtt*` đã kiểm: chưa trùng.)
22. **Hai chế độ build:** `SPLC_AS_LIBRARY=OFF` (mặc định) = firmware độc lập. `=ON` = thư viện cho host: host
    `add_subdirectory` rồi link INTERFACE target `splc`. Host PHẢI tự: thêm `$<TARGET_OBJECTS:splc_platform_freertos>`
    vào executable khi `SX_OS_USE_FREERTOS=1`; thêm `-Wl,--wrap=HAL_RTC_SetTime/SetDate` nếu `SPLC_RTC_BUILTIN=1`.
23. **`SPLC_RTC_BUILTIN` / `SPLC_ADC_BUILTIN`** (mặc định 1). `=0`: host tự cấp `sx_rtc_*` / `sx_adc_*`. Gateway đặt
    cả hai = 0; `sx_rtc_*` trong `splc_port.c` là stub "chưa có giờ" (Time Window không bắn, App ghi RTC bị từ chối,
    `{ts}` MQTT = 0). **Chủ sở hữu đồng hồ CHƯA chốt:** NTP của host trả giờ ĐỊA PHƯƠNG, SimplePLC cần epoch UTC +
    `tz_offset_min`.
24. **Bus tới board IO:** 115200 8N1; cổng mặc định USART6 trước, nay dời sang USART3 (quyết định 30). `modbus_master_init()`
    của host cấu hình cả 3 cổng và `splc_check_port()` đặt lại baud mỗi giây — nếu cổng bị cấu hình cho dịch vụ khác,
    hai bên giành nhau.
25. **Cách nhúng vào `wg_ue`:** thư viện ở `libs/simple_plc/` (submodule), port ở `ports/splc_port/`
    (`CMakeLists.txt`, `splc_port.c/.h`, `splc_mqtt_port.c/.h`, `splcopts.h`). Quy tắc: (a) target host tên
    `ports_splc_port`; (b) `ports/splc_port/CMakeLists.txt` `set(SPLC_AS_LIBRARY ON ... FORCE)` và
    `set(SPLC_OPTS_DIR ... FORCE)` TRƯỚC `add_subdirectory`; (c) `splc_platform_freertos` gắn vào **executable** của
    host, KHÔNG vào static library (thứ tự archive → `undefined sx_os_yield_wait`); (d) `logger.h` của host thấy được
    nhờ `include_directories(libs/logger ...)` ở scope gốc; (e) file host đã sửa: `CMakeLists.txt` gốc,
    `apps/main/CMakeLists.txt` (`apps_main` link `ports_splc_port`), `apps/main/gateway.c`, và cho MQTT:
    `apps/mqtt_client/mqtt_client.{c,h}` (`mqtt_client_publish_ex`, `mqtt_client_set_override`,
    `mqtt_client_connected_override_generation`). Không sửa `usb_netif.c`, `rs485_driver.*`, `flash_port.c`, file
    CubeMX. Sửa host chỉ làm trên bản clone ở sandbox, người dùng tự chuyển sang repo chính. Baseline `wg_ue` trên
    Linux cần workaround KHÔNG thuộc dự án này (hoa/thường tên file, thiếu `#include "semphr.h"`/`"queue.h"`) — chỉ
    vá ở sandbox. `network/network_manager/network_manager.c` CHƯA từng được phép sửa (xem mục 0, vấn đề mạng).
26. **RAM cho SimplePLC trên `wg_ue`:** hạ `configTOTAL_HEAP_SIZE` trong `Core/Inc/FreeRTOSConfig.h` dòng 74 bằng tay
    (người dùng chấp nhận file CubeMX bị sửa; sinh lại từ CubeMX sẽ mất thay đổi). Đang dùng **296 KB**: dư chỉ
    2 992 B RAM tĩnh — buffer tĩnh mới > ~3 KB sẽ làm link FAIL. Điều kiện: `lowest free` thật của host (log `MONITOR`,
    `Core/Src/app_freertos.c`) ≥ ~30 KB (tốt nhất ~38 KB) khi chạy đủ tải — hiện chỉ có số lúc rảnh (61 KB).
27. **OTA của `wg_ue` KHÔNG dùng (người dùng xác nhận) → không làm khoá Flash chung.** `ota_init()` + task `OTA_HEALTH`
    vẫn còn trong `gateway.c`, KHÔNG xoá khi chưa đọc `ota.c` + `bootloader.c` (`ota_mark_healthy()` có thể là bước
    xác nhận image cho bootloader). Nếu sau này dùng OTA phải thiết kế khoá Flash (PVD ghi retain trong ISR, không được
    chờ mutex).
28. **Chế độ USB CDC-only cho gateway** (nhánh `usb_cdc_gw` của `wg_ue`). Lý do: với `GW_USB_NETIF_ENABLE=ON` PC báo
    "Unknown USB Device" (MCU nhận RESET nhưng không qua SET_ADDRESS); tắt SimplePLC vẫn lỗi ⇒ KHÔNG do SimplePLC;
    nguyên nhân CHƯA biết. Cờ `GW_USB_CDC_ENABLE` (loại trừ `GW_USB_NETIF_ENABLE`), TinyUSB của host, một ACM, PID
    0x4001. Phép thử: enumerate được ⇒ lỗi ở descriptor composite RNDIS/ECM; vẫn Unknown ⇒ lỗi tầng chung. **Chưa
    chạy trên board.**
29. **Zigbee-IO nói chuyện với App qua RS485 (làm ở phía user, KHÔNG thêm module port):** `rs485_output/rs485_pump` nằm
    trong `board_zigbee_io.c`; `SPLC_PORT_USB_CDC` chọn USB (1) hay RS485 (0) cho board này. USART1, DE = PB13, 115200
    8N1, cực DE `RS485_DE_ACTIVE_HIGH`. Phát CHẶN (~4 ms/khung 41 B) — lệch hợp đồng "output không chặn", chấp nhận cho
    test. Hai sửa Layer 0 (`stm32h5_uart.c`): khoá ngắt quanh `splc_cqueue_*`; `HAL_UART_ErrorCallback` nhận lại RX sau
    overrun. **Chưa chạy trên board.** *(Lệch repo: thư mục `splc_config_rs485/` và preset `Release-RS485` nhắc ở đây
    KHÔNG có trong repo; `splc_config/splcopts.h` ghi "RS485 variant" nhưng đặt `SPLC_PORT_USB_CDC 1` — cần người dùng
    xác nhận chưa commit hay lỗi thời.)*
30. **Gateway nói chuyện với App qua RS485 — làm ở glue của HOST (`wg_ue/ports/splc_port/`, bản chuẩn; `port_gw/` ở repo
    này là bản nháp cũ, không đồng bộ), SimplePLC không đổi.** `app_output()` = `rs485_send()` (host tự điều khiển DE),
    `app_process()` = `rs485_recv_polling(...,0)` → `splc_link_input()`. Option `SPLC_HOST_APP_RS485_PORT` (âm = USB CDC,
    mặc định) + `_BAUD/_PARITY`; owner `RS485_OWNER_MODBUS_GATEWAY`. `#error` khi cổng App trùng cổng IO, hoặc
    `GW_USB_CDC_ENABLE=ON` cùng RS485. Cổng App = USART6 ⇒ bus board IO dời sang USART3 (`SPLC_HOST_RS485_PORT 0`).
    Phát CHẶN `rs485_send` (~3,6 ms/41 B). Phần cứng: `RE/DE` MAX485 nối chung một GPIO ⇒ DE tích cực cao; không có
    echo. Triệu chứng lạ (khung hỏng, break 1 s) chỉ hết sau khi **thay IC MAX485** (quy kết, chưa đo scope — bài học
    26). Điểm phần cứng chưa đo, không chặn: (a) phân cực bus rảnh yếu (R40/R28 → A−B ~30 mV, dưới ngưỡng ±200 mV) có
    thể sinh byte rác; (b) R17 kéo `RE/DE` lên 3V3 lúc reset ⇒ MAX485 ở chế độ phát; (c) kiểm BOM xem IC là
    MAX3485/SP3485 (3,3 V) hay MAX485 5V. Công cụ diag `gw_rs485_diag.c` (`GW_RS485_DIAG_*`): **các cờ DRAIN/TX_TEST/
    CAPTURE mặc định = 1 → đặt 0 (hoặc gỡ) trước khi commit/chạy thật** (drain nuốt byte ⇒ SimplePLC không nhận được gì).

**MQTT**
31. **SimplePLC chỉ làm LOGIC và GIỮ DỮ LIỆU, không có client mạng.** Không TCP/TLS/MQTT client trong lib; truyền mạng do
    host cắm qua ống `publish` kiểu `netif` (cùng triết lý 13/16). Bật bằng `SPLC_MQTT_ENABLE` (mặc định 0). Lib GIỮ cấu
    hình broker (host, port, client_id, user, pass, TLS, clean session, keepalive, version) vì App gửi `0x0E00` ở mọi
    lần deploy; user đọc qua `splc_mqtt_get_broker(out, &app_configured, &generation)` (copy ra). `generation` chỉ tăng
    khi cấu hình THẬT SỰ đổi hoặc `app_configured` đổi (hai chiều). `app_configured` = đã nhận broker từ App VÀ
    `enabled=1` VÀ host không rỗng; `false` → host dùng cấu hình mặc định (`g_sys_config.mqtt`), `true` → dùng cấu hình
    của lib. `enabled=0` = quay về mặc định host (không tắt MQTT được bằng ô Enabled; muốn ngừng publish thì tắt các
    khối telemetry). Host báo trạng thái bằng `splc_mqtt_set_status` (CONNECTED/TLS/AUTH, latency, last_error,
    **generation của cấu hình mà kết nối được dựng theo**); lib chỉ publish khi CONNECTED **và**
    `status.generation == generation`. Password đọc lại luôn trả 0 nhưng nằm nguyên văn trong Flash — ghi rõ khi giao
    tài liệu. **Mọi hàm `splc_mqtt_*` chỉ gọi từ ngữ cảnh engine** (port bơm status/broker trong hàm bơm của task
    `SPLC`, chu kỳ 100 ms) ⇒ không cần khoá.
32. **Lưu/áp dụng cấu hình MQTT:** ghi vào BẢN NHÁP (kiểm rẻ từng mảnh, địa chỉ ngoài bản đồ → `0x02`), kiểm nội dung cả
    bộ lúc COMMIT, áp dụng nguyên tử, lưu Flash cùng Rule Table (một CRC; cờ MQTT = bit 13 của `rule_count`; bản ghi cũ
    bit 13 = 0 vẫn nạp được; tối đa 4 304 B < sector 8 KB). Khối KHÔNG được ghi trong nháp thì GIỮ cấu hình đang chạy
    (KHÁC FB, quyết định 8). Vùng `0x0E00..0x0E53` luôn ACK, lỗi bắt ở COMMIT. App ghi thứ tự Timer → Counter →
    Telemetry → Broker → staging/commit.
33. **Layout khối MQTT:** Header `splc_mqtt_wire.h` + Structs 3.9/3.10 + golden vector là đúng (`MqttTelemetryCodec.cs`
    của App khớp Header). ICD `SPLC_SPEC_MQTT_001` sai — đã báo đội App sửa (E.7).
34. **MQTT Subscribe (chiều nhận) chưa có wire format** (App có UI/parser nhưng không có thanh ghi/codec nạp xuống MCU;
    grep `paa` `8056a62`). KHÔNG làm firmware Subscribe cho tới khi đội App chốt spec (đã hỏi ở E.7). Hướng dự kiến:
    ống đối xứng `splc_mqtt_input(topic, payload, len)`.
35. **Thiết kế MQTT Publish (chi tiết `docs/plc_mqtt_design.md`):**
    - **Q1** QoS/retain theo từng khối; host có `mqtt_client_publish_ex(topic, payload, qos, retain)`.
    - **Q2** chưa dùng TLS: `SPLC_MQTT_TLS_SUPPORTED=0`, COMMIT từ chối `use_tls=1`. Host cũng không hỗ trợ
      `clean_session=0`/MQTT 5 (port ghi log cảnh báo và bỏ qua).
    - **Q3** cấu hình App vào client host qua lớp ghi đè chỉ trong RAM (`mqtt_client_set_override`, đọc nguyên khối
      trong critical section), KHÔNG chép vào `g_sys_config` (vì `config_save()` ghi cả struct xuống file). Broker App
      không vừa buffer host (31/31/47/63 ký tự), hoặc TLS ⇒ KHÔNG áp dụng, host giữ broker mặc định (có log cảnh báo).
      `base_topic` (LWT) giữ của host.
    - **Q4** không publish khi `diag` đang treo engine. **Q5** bắn một lần ngay khi sẵn sàng (giá trị nền).
    - **Q6** token Custom cho mọi tag: `{valK}`, `{valK:N}`, `{tagK}`, `{vals}`, `{ts}`, `{device}`.
    - **Q7** `{ts}` = 0 khi chưa có giờ (gateway cho tới khi chốt chủ đồng hồ, quyết định 23).
    - **Q8** `reserved_header` (+15) = 8 tag × 2 bit số chữ số lẻ (0 = số nguyên, như App ghi hiện nay); `deadband`
      theo đơn vị kỹ thuật (`deadband × 10^dec` so với delta thô); App thăm dò `0x0E54` trước khi gửi MQTT. **Đề xuất
      phía firmware, đã gửi đội App (E.7), CHƯA có xác nhận.**
    - **Số thập phân:** tag là `int32`; in bằng số nguyên + hệ số 10^dec (0..3), KHÔNG dùng printf float; số âm giữ dấu.
    - **Thuật toán:** tối đa 1 publish/scan, quay vòng 4 khối; ON_CHANGE / COV / PERIODIC; `publish` trả `<0` thì
      không cập nhật `last_value`, thử lại sau `SPLC_MQTT_RETRY_MS`; mất kết nối → bỏ dữ liệu, không tự xếp hàng.
      Hành vi chốt thêm khi code: nối lại broker → mọi khối bắn lại baseline một lần; enable tag đi 0 → khác 0 → bắn
      lại baseline; `tx_message_count` đếm số lần callback trả 0 (host ĐÃ NHẬN, chưa phải broker đã ack); lỗi dựng
      payload chỉ đếm vào `splc_mqtt_format_errors()` và log một lần.
    - `{device}` trên gateway lấy từ `g_sys_config.device.device_id`, KHÔNG phải `g_sys_config.mqtt.client_id` (rỗng).
    - Host `wg_ue` còn luồng `apps/mqtt_client` + `data_broker` (publish `factory/{gateway_id}/{device_id}/telemetry`,
      hiện `"data":{}`, placeholder chưa thay): hai luồng cùng tồn tại, topic phải khác nhau.

## 3. Quy trình làm việc

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Đầu phiên hoặc khi người dùng báo đã push: `git pull`, rồi
  **build/compile verify thật** (không chỉ đọc diff). Cài được: `apt-get install gcc-arm-none-eabi ninja-build`,
  `pip install cmake --break-system-packages`, `git submodule update --init --depth 1 simple_plc/libs/tinyusb`. Build:
  `cmake -S . -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Release
  [-DSPLC_OPTS_DIR=<dir>]`.
- **Giao nguyên file** (present từng file để copy-paste, giữ cây thư mục), KHÔNG patch/zip/đoạn thay thế; kèm danh
  sách file cần xoá/đổi tên. Present NGAY sau mỗi mốc. Chỉ giao code dự án + script test Python; KHÔNG giao harness C
  chạy trên PC (trừ khi người dùng yêu cầu). Việc tự kiểm tra nội bộ vẫn làm nhưng nêu rõ là chạy trên mô hình giả.
  Tính năng không test được trên board thì nói rõ là chưa test.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, từng quyết định nhỏ.
- Nói rõ mức chắc chắn khi đề xuất nguyên nhân; kiểm chứng bằng dữ liệu thật.
- Mỗi bước mới: code → build ARM cả Zigbee-IO và Gateway → người dùng nạp + chạy test trên board → cập nhật file này.

## 4. Kế hoạch

**Việc tiếp theo (đề xuất):** (1) validate index tag khi nạp rule (4.D); (2) dọn repo (4.D, cuối mục); (3) build baseline
`SPLC_MQTT_ENABLE=0` + đo RAM/FLASH Gateway với MQTT bật + kiểm ranh giới layer bằng `nm`; (4) chờ người dùng quyết:
chủ đồng hồ gateway (4.A.3), bản đồ Flash host (4.A.5), có sửa `network_manager.c` không (mục 0).

**A. Nhúng vào `wg_ue` (còn lại)**
1. *(đã xong)* SimplePLC nhúng và chạy trên board gateway (mục 0).
2. *(cần người dùng)* Đo `lowest free` heap của host DƯỚI TẢI (điều kiện quyết định 26); số hiện có (61 KB) là lúc rảnh.
3. *(cần người dùng)* Chốt chủ sở hữu đồng hồ (quyết định 23) — cần cho Time Window và `{ts}` trên gateway.
4. *(cần người dùng/phần cứng)* **Firmware slave cho board IO** (khối `0x0C00..0x0C06` bằng nanoMODBUS; lease/SAFE ở
   slave; baud/parity giả định 115200 8N1). Thiếu nó thì DI/DO ảo luôn offline.
5. **Bootloader + Flash (đã đọc code, chờ người dùng chốt):** đề xuất thu mỗi slot app 82→80 sector (640 KB) →
   3 slot kết thúc ở `0x081F0000`; dữ liệu 40 KB `0x081F0000..0x081F9FFF`; 1 sector trống `0x081FA000`; scratch giữ
   `0x081FC000`. Image app hiện 603 660 B < 640 KB. Host phải đổi `flash_define.h` (82→80 ×3), `LENGTH` FLASH của app
   656K→640K, build + nạp lại bootloader. **Rủi ro lớn:** `bootloader.c` chỉ ghi layout slot khi `magic_number` sai —
   thiết bị đã có partition table giữ layout cũ, slot Factory cũ phủ đúng vùng dữ liệu SimplePLC ⇒ ghi đè lẫn nhau. Board
   dev phải xoá sector partition `0x0800E000` sau khi nạp bootloader mới; thiết bị ngoài thực địa cần cơ chế migrate
   (CHƯA có). KHÔNG nạp địa chỉ `0x081F0000` cho tới khi người dùng chốt.
6. *(chờ xác nhận)* Kết nối lại Studio (`paa` `8056a62`, hồ sơ `GatewayRs485EthV2_0`) với gateway; kiểm danh mục tag
   gateway (112 tag, dense) và trần DI/DO/AI khi dựng profile.
7. Đo trên board: độ trễ scan khi task `SPLC` (prio 6, trùng `UART_LOG`, thấp hơn MODBUS/BROKER/SHELL 11 và NETWORK 12)
   chạy cùng task USB; ring phải đủ khung dài nhất (256 B RTU) khi engine ngủ một scan.
8. *(cần người dùng đồng ý sửa file host)* Mạng host: probe DNS dùng host của override/`pool.ntp.org`, nới ngưỡng probe,
   reset backoff khi cấu hình MQTT đổi lúc offline (chi tiết mục 0). Kiểm DNS router bằng `nslookup`; cân nhắc tắt ETH/LTE
   nếu không dùng.

**B. Remote I/O còn mở:** chưa phát hiện trùng `unit_id` giữa hai hàng bảng trên cùng bus; chưa có tag báo "node
offline" cho App.

**C. App + spec (không gấp):** App sinh rule "đúng giờ" `EQ, Lo=Hi` (chưa kiểm); Structs mục 4 (124 tag cố định) chỉ
đúng cho Remote I/O V1; Conformance spec cần sửa như mục "Nguồn tài liệu".

**D. Firmware khác:** validate `guard/trigger/action_tag` ∈ `0..MAX_TAGS-1` khi nạp rule (hiện index sai chỉ vô hiệu
hoá rule âm thầm); chưa ghi snapshot retain ngay trước REBOOT; lỗi Flash lúc erase trong bước copy A→B huỷ bản cũ
trước khi biết lỗi; `ACT_WRITE_REMOTE/LOG_EVENT/SEND_ALARM` chưa implement; không có log trước `logger_init`;
`utils/filter/*` chưa soát; linker `STM32H523xx_FLASH.ld` khai `LENGTH=512K` (chip 256 KB — HỎI trước khi sửa).
**Dọn repo:** comment cũ nhắc `modbus_usb`/`sx_usb_cdc`/`FREE_RTOS` còn ở `plc_modbus_cfg.h`,
`services/CMakeLists.txt`, `app/CMakeLists.txt`, `modbus_transport.h`, `splc_board.h`; `port_gw/` là bản nháp cũ
(xoá hoặc đánh dấu); làm rõ `splc_config/splcopts.h` (mục 0, quyết định 29).

**E. MQTT Publish**
- E.1–E.4 *(xong)*: option + `splc_mqtt_fmt` + `splc_mqtt_wire.h`; `plc_mqtt` (cấu hình running/draft/COMMIT, định
  tuyến `0x0C00..0x0D7F`/`0x0E00..0x0E5F`, `plc_rtc_get_epoch`); đoạn MQTT trong Flash (bit 13) + hook
  `CLEAR_RULES`/`FACTORY_RESET`; `plc_mqtt_scan` + ống `splc_mqtt_register()`. Đã kiểm trên PC với Flash/đồng hồ/tag
  GIẢ (harness không nằm trong repo; đã phá code có chủ đích, test bắt được). Phần gọi `plc_mqtt_scan()` từ
  `scan_cycle()` và bỏ qua khi diag treo (`plc_engine.c`) chỉ kiểm bằng build ARM + chạy board.
- E.5 *(xong, chạy board)*: port phía `wg_ue` (`splc_mqtt_port.c`), xem mục 0 và quyết định 25/35.
- E.6 *(người dùng báo xong)*: `test/test_mqtt.py` (1087 dòng; chạy trên gateway qua RS485, GHI Flash và chạy
  `CLEAR_RULES` — chỉ board dev) + kiểm ranh giới layer. Còn nên làm: build baseline `MQTT=0` ra đúng số chuẩn; đo
  RAM/FLASH Gateway với MQTT bật.
- E.7 *(đã gửi đội App, chờ phản hồi)*: dùng `reserved_header`; token trong `CustomTemplate`; quy đổi 10^dec/deadband;
  thăm dò `0x0E54` + hiển thị `0x0E54..0x0E5F` + đổi nhãn nút Test Connection thành "PC tới broker" (nút chỉ mở TCP
  socket từ PC); sửa ICD và Conformance 11.2; chốt wire Subscribe. Mật khẩu broker lưu chữ thường trong
  `ProjectMetadata` (phía App). Khi có phản hồi: cập nhật Q8 (quyết định 35) và, nếu có wire Subscribe, mở việc E.8.

Ghi nhớ: `0x0C00..0x0C06` trên slave board IO (Remote I/O) trùng SỐ với Telemetry Block 0 của gateway — khác thiết bị/bus
nên không xung đột, nhưng dễ nhầm.

## 5. Bài học (luật rút ra — đừng lặp lại)

1. Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới.
2. Bằng chứng trước khi kết luận: log là dấu vân tay của phiên bản code; lọc theo từ khoá.
3. Test chỉ có nghĩa nếu trạng thái đầu xác định (reset board, kiểm `DO0=0`; test persistence phải kiểm dữ liệu sống sót
   qua reboot TRƯỚC khi xoá).
4. Timeout đếm bằng vòng lặp không phải timeout: đo bằng tick thật.
5. Đừng suy ngữ nghĩa field từ tên hay từ một phía; đọc UI/compiler của App và hỏi người dùng.
6. Flash giả mô phỏng mất điện: sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA.
7. **Việc chưa giao thì mất khi phiên kết thúc:** present nguyên file ngay sau mỗi mốc; người dùng push ngay.
8. Không code hành động phá huỷ dữ liệu khi spec mơ hồ — hỏi phạm vi trước.
9. Đổi giá trị mặc định/cấu hình: kiểm cả bản `-D`/override, giá trị sai (`#error`), thiếu `splcopts.h`.
10. Hiểu kiến trúc trước khi code: hỏi "thiết bị này có cần chạy PLC không?".
11. Đừng tuyên bố môi trường thiếu công cụ khi chưa thử cài.
12. Hợp đồng core↔port: core giữ phần khó (timeout, ghép khung nhiều mảnh); đọc comment lịch sử bug trước khi đề xuất đổi.
13. Test xanh chưa chứng minh gì: phá code có chủ đích (đột biến); với đa luồng, hạ memory order xuống `relaxed` rồi xem
    ThreadSanitizer có báo race không.
14. Đo trước khi ghi con số vào comment/tài liệu. Khẳng định về host phải nêu đã so cái gì và CHƯA so cái gì.
15. Kế hoạch đổi tên symbol phải dựa trên `nm`, không dựa trên quy ước đặt tên; cần bước kiểm TỰ ĐỘNG sau build.
16. Bỏ một phụ thuộc bắc cầu thì build lại MỌI cấu hình. Test host với harness giả chưa chứng minh gì về CMake.
17. Test chế độ library phải dùng host thật hoặc host giả đủ việc; thử cả ca thiếu (phải FAIL link) lẫn ca đủ.
18. Handoff cũ có thể nói đã xoá file nhưng repo vẫn còn (hoặc ngược lại — `test_mqtt.py` từng ghi "chưa có" khi đã có):
    kiểm bằng `git status`/`ls` trước khi tin.
19. **Build baseline TRƯỚC khi tin handoff/commit** (một commit đổi tên từng bỏ sót `#include` làm Zigbee-IO không build
    dù handoff ghi "đã build sạch").
20. Khi đo kích thước bằng link giả (`--defsym`), đừng gán symbol mà kernel/driver THẬT cũng định nghĩa.
21. Nhúng vào host thật mới lộ điều mô hình giả không lộ (RAM host gần đầy, thứ tự archive, trùng tên target). Luôn build
    baseline host TRƯỚC, rồi mới nối. Đừng cộng số đo rời — đọc map thật.
22. Đọc code bootloader trước khi tin một đề xuất bản đồ Flash: partition table lưu kích thước slot trong Flash, nên đổi
    `flash_define.h` mà không migrate thì thiết bị cũ chạy layout cũ.
23. Đo bằng ĐÚNG cờ mà người dùng build (`build.bat`), không bằng preset mặc định (`GW_MDNS_ENABLE` khác nhau).
24. So FLASH giữa hai máy phải tính đường dẫn tuyệt đối nhúng trong ELF (~49 chuỗi, 1 ký tự ≈ 49 B). Dùng hiệu số cùng một máy.
25. `build.bat` gốc của `wg_ue` cứng đường dẫn Ninja; chạy nó dù một lần cũng ghi `CMAKE_MAKE_PROGRAM` sai vào cache
    `build\` → xoá `build\` (`build_gw.bat rebuild`). `apps\CMakeLists.txt` và `apps\main\CMakeLists.txt` dễ nhầm: chỉ
    file thứ hai định nghĩa `apps_main`.
26. **Triệu chứng bus lạ → loại trừ linh kiện trước khi dựng giả thuyết.** Mô phỏng khớp từng byte ≠ nguyên nhân gốc. Đổi
    linh kiện/dongle/dây, đo A−B và chân RO bằng đồng hồ/scope trước khi kết luận.
27. **Pull repo liên quan trước khi kết luận "App chưa có X"**; nêu rõ commit đã đọc.
28. **Tài liệu spec có thể tự mâu thuẫn — chọn bản đúng bằng code codec thật.** Không lấy code mẫu trong tài liệu làm nguồn.
29. **Kết nối MQTT chập chờn ≠ lỗi cấu hình SimplePLC.** Đọc log theo thứ tự: `demoting` / `Route event` (lớp mạng host) →
    `MQTT connect start: ... broker=` (broker thực sự được nối) → `generation N != connection M` (đường đổi cấu hình).
    Dòng `Broker:` trong status in broker của cấu hình riêng, đừng dùng để suy ra kết nối. Số `generation` của netmgr và
    của SimplePLC là hai bộ đếm khác nhau. Khi một giả thuyết (vd. cạn pool DNS) không có dòng log đặc trưng (`DNS request
    allocation failed`) thì rút lại, đừng giữ.
30. **Hai broker cùng tên mặc định làm test mù:** muốn chứng minh override được áp dụng, gửi broker KHÁC broker mặc định của
    host (không để trùng), rồi đọc `broker=` trong `MQTT connect start`.

## 6. Verify & test

**Script test trên board** (`python -m pip install pymodbus pyserial`; `test_mqtt.py` cần thêm `paho-mqtt` cho
`--broker`). Các script ghi Flash/xoá rule — chỉ chạy trên board dev:

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | bản hiện tại là probe bring-up RS485 (tham số `--period/--baud/--unit`) |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, engine dừng thật, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD |
| `test_rtc.py COM14 [--reboot]` | khối RTC, từ chối ghi sai, tốc độ đồng hồ, Time Window |
| `test_fb.py COM14 [--reboot] [--probe-64]` | khối FB (nháp/COMMIT, từ chối, CV/Q, persistence); `--probe-64`: FC16 73 byte |
| `test_retain.py COM14 [--commits N] [--periodic]` | retain qua REBOOT, xoay vòng sector, CLEAR_RETAIN; `--periodic` ~6 phút |
| `test_sysclear.py COM14 [--reboot]` | 3 lệnh xoá, persistence, lệnh trong diag, lệnh sai |
| `test_mqtt.py COM35 --unit 1 --baudrate 115200 [--broker]` | khối Telemetry/Broker/status `0x0C00..0x0E5F`, nháp/COMMIT, CLEAR_RULES xoá MQTT; `--broker` chuyển gateway sang broker test rồi trả lại; độc lập, không import script khác |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`. Một số script (`test_rtc/test_sysclear/...`) từng dùng helper
của `test_plc.py` bản cũ — kiểm trước khi chạy. (`test/test_rule*.py` còn trong repo; chưa ghi mục đích ở đây.)

**Kiểm cấu hình (build ARM) mỗi khi đụng CMake/option:** mặc định (Zigbee-IO, nhớ `splc_config/splcopts.h` đang bật
MQTT — mục 0); `SPLC_BOARD_GATEWAY` qua `-DSPLC_OPTS_DIR`; `SPLC_FLASH_SIZE_KB` 128/256/512/… (sai phải
`#error`/`_Static_assert`); `SX_OS_USE_FREERTOS=1` (cần `-DSPLC_OS_INCLUDE_DIRS` hoặc stm32cubemx); `MAX_RULES=50`;
`SPLC_LOGGER_BUILTIN=0` (cần logger host); `SPLC_RTC_BUILTIN=0`/`SPLC_ADC_BUILTIN=0` (standalone phải FAIL link thiếu
`sx_rtc_*`/`sx_adc_read`; có `splc_port.c` thì link qua); `SPLC_AS_LIBRARY=ON`; thiếu `splcopts.h` phải `FATAL_ERROR`
lúc configure; `SPLC_MQTT_ENABLE=0/1` (`=0` phải ra ĐÚNG số chuẩn ở mục 0). So FLASH/RAM với số đo chuẩn.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 không có symbol `sx_usb_*`; Layer 2 không có `sx_flash_*`/`sx_usb_*`;
`plc_rule_flash.c` PHẢI có `sx_flash_*`; `sx_system_reset` chỉ được gọi từ `plc_system_cmd_service.c`; chỉ
`port/usb/splc_usb_cdc.c` được `#include "tusb.h"`; `modbus_link.c` không có `sx_usb_*`/`tud_*`; không file nào ngoài
`libs/splc_nanomodbus/` được `#include "nanomodbus.h"`; `nm` trên `libsplc_nanomodbus.a` không có symbol toàn cục thiếu
tiền tố `splc_`; `nm` trên binary không có `cqueue_*`/`board_init`/`nmbs_*` không tiền tố; `plc_mqtt` không `#include`
nanomodbus/tusb.

**Kiểm trên PC (không cần board, mô hình giả — nói rõ khi báo cáo):** Layer 2 (`plc_tag.c`, `plc_rule.c`,
`plc_internal_rule/*.c`) biên dịch bằng `gcc -std=c11` + stub, `sizeof(SPLC_RuleRecord)==32`; `modbus_link` (đồng hồ giả +
2 thread, ThreadSanitizer); `plc_io_remote.c` + `modbus_serial.c` với slave RS485 giả; `splc_mqtt_fmt`, `plc_mqtt`
(Flash/đồng hồ/callback giả). Harness KHÔNG nằm trong repo; muốn giữ thì đặt vào `test/` kèm target CMake PC — quyết
định của người dùng.