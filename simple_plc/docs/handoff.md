# SimplePLC — Handoff

> Cho phiên làm việc sau nắm "đang ở đâu, làm gì tiếp, luật gì phải giữ". Đọc SAU `Readme.md` và
> `docs/architecture.md`. File này chỉ giữ: trạng thái hiện tại, quyết định đã chốt, kế hoạch, quy trình, bài học.
> Lịch sử từng bước đã bỏ — xem `git log`. Số thứ tự quyết định được GIỮ NGUYÊN để tài liệu khác còn tham chiếu đúng
> (có chỗ trống).
>
> **Quy tắc vàng:** trước khi tin điều gì dưới đây, chạy `git pull && git log --oneline -10`. Code lệch file này thì
> tin code và sửa file này.

**Nguồn tài liệu** (mâu thuẫn giữa các tài liệu → tin code thật của App `paa` và firmware, rồi sửa tài liệu):
1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` — struct/memory map (nguồn chính, Wire V2.0).
2. `docs/SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md` + `splc_mqtt_wire.h` — khối MQTT Publish (Telemetry
   `0x0C00`, Broker `0x0E00`). **Lỗi thời một chỗ:** ghi `0x0D80..0x0DFF` là Reserved (xem F.1).
3. `docs/plc_mqtt_design.md` — thiết kế MQTT Publish (quyết định 35).
4. `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` — chỉ còn đúng cho LOGIC rule (Trigger→Compare→Dwell→Guard→Action).
5. `docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md` — checklist; còn lỗi (mục 11.2 ghi FC16 là `0x16`, đúng `0x10`; chưa có mục MQTT; giả định
   124 tag cố định của Remote I/O V1; mục 13.8 ghi `pwm_freq_hz` "Little-Endian", sai so với ICD PULSE_OUT và codec). `paa` `cdc0511` thêm mục 13.8 (PULSE_OUT),
   13.9 (Wi-Fi ST67) và 13.10 (quy trình nạp 6 bước: đọc `0x0A08` → ghi ngoại vi `0x0B00, 0x0B40, 0x0E00, 0x0C00, 0x0D80, 0x1000, 0x1100, 0x1200` →
   staging `0x9010` → CRC `0x9003` → COMMIT `0xA000=0xA5A5`).
6. **ICD nằm trong repo App**, `paa/docs/firmware/` (đọc 21/10/2026, `paa` `27c35d3`): `SPLC_SPEC_MQTT_001_TELEMETRY_ICD` (layout khối Telemetry SAI —
   không dùng), `..._MQTT_002_SUBSCRIBE_ICD`, `..._MQTT_003_TLS_CERTIFICATE_WIRE_ICD`, `..._MODBUS_MASTER_001_READ_ICD`, `..._MODBUS_MASTER_002_WRITE_ICD`,
   `..._DIRECT_WRITE_003_ICD` (mã mới `SPLC-SPEC-PULSE-003` v2.1.0: khối đổi tên PULSE_OUT, mã nhị phân vẫn `DIRECT_WRITE`), `..._WIFI_004_ICD` (ST67, ngoài
   phạm vi), `..._MODBUS_TCP_005_ICD` (Modbus TCP, cờ capability `0x0400`, ngoài phạm vi), `GOLDEN_VECTORS_V2_0.md` (hex dump chuẩn MB_READ, MB_WRITE 2 chế độ, MQTT SUB, PULSE_OUT — DÙNG LÀM TEST), `splc_mqtt_wire.h`; header chung `paa/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h`. Từ `cdc0511` ICD MB_READ/MB_WRITE/PULSE_OUT khớp
   codec C# và header (xem F.1); từ `27c35d3` header qua `gcc -std=c11` (struct Wi-Fi đã đúng 128 B). Link `g:/HoaNV/...` trong tài liệu không mở được.

**Repo liên quan**
- Firmware lib: `ngoxuanloc2309/simple_plc`, nhánh `board_dev_gw` (bản đã đọc `4743cfb`). Tài liệu ở `simple_plc/docs/`.
- App (.NET, SimplePLC.Studio): `ngoxuanloc2309/paa`, `main` (bản đã đọc `27c35d3`, 21/10/2026). Bug xuyên App+firmware phải đọc CẢ
  HAI repo. Luôn `git pull` App trước khi kết luận "App chưa có X".
- Gateway thật (host nhúng SimplePLC): `ngoxuanloc2309/wg_ue`, `main` (STM32H563, FreeRTOS + lwIP, 3 cổng RS485
  USART3/6/10, TinyUSB do host sở hữu; bản đã đọc `60087b2`, chưa pull lại).

## 0. Trạng thái (nhánh `board_dev_gw`)

**Đã chạy trên board Zigbee-IO (STM32H523CCU6):** Wire Profile V2.0 (diag, retain, xoá lệnh, RTC + Time Window, FB
`0x0B00..0x0B7F`) qua USB CDC; `test_fb.py --probe-64`, `test_plc.py`, `test_retain.py` ALL PASS.

**Đã chạy trên board gateway (`wg_ue`):**
- SimplePLC khởi động, App link RS485 qua USART6 (6/6 phản hồi, chưa soak) — chỉ sau khi thay IC MAX485 (quyết định 30).
- **MQTT Publish (E.1–E.6):** port `wg_ue/ports/splc_port/splc_mqtt_port.c` chạy trên board; đổi broker bằng App/Python
  khi đang connected và khi offline đều chạy (người dùng xác nhận). Kết quả chi tiết `test_mqtt.py` chưa ghi.
- Heap host lúc rảnh: `used=205/293KB, lowest free=61KB` (đạt điều kiện ≥ ~30 KB của quyết định 26; CHƯA đo dưới tải).

**R1 — bảng kiểu tag `0x0080..0x00BF` (`plc_tagtype`, `SPLC_REAL_ENABLE=1`): ĐÃ CHẠY TRÊN BOARD 21/10/2026 (COM17, 112 tag dense, ô 112 tồn tại với kind NONE ⇒ `MAX_TAGS` > 112; người dùng chưa xác nhận SKU).**
`test/test_tagtype.py COM17 --unit 1 --baudrate 115200 --reboot` **ALL PASS**: nháp ≠ bảng chạy, byte-exact sau COMMIT, thứ tự byte (High = tag chẵn), Deploy không ghi bảng thì giữ bảng, FC06 giữ các thanh ghi khác,
COMMIT từ chối cả bộ (REAL trên DI/DO/VFLAG/COUNTER, kiểu 4/0xFF, bản nháp bị bỏ), REAL nhận trên VREG/VREG_RETAIN, đổi số nguyên↔REAL thì giá trị về 0 (REAL→REAL và BOOL↔INT32 giữ giá trị), giới hạn địa chỉ `0x02`,
bảng sống sót REBOOT (đọc từ Flash), `CLEAR_RETAIN` giữ bảng, `CLEAR_RULES` và `FACTORY_RESET` xoá bảng và vẫn trống sau REBOOT. Chưa chạy trên board: AI kiểu REAL (board thử không có AI), mất điện giữa lúc ghi Flash, hạ firmware `REAL=1` → `0`
(bản ghi coi là hỏng, Deploy lại — như MQTT). Số đo RAM/FLASH: build ARM của người dùng `REAL=1` ra RAM 82 624 B / FLASH 85 916 B (cấu hình `splcopts.h` lúc đó CHƯA ghi ⇒ chưa so được với baseline). Số đo ở sandbox phiên trước (cờ test=0, đo lại trên máy người dùng
trước khi tin): `REAL=0` đúng baseline ở cả hai board (Zigbee-IO MQTT=0 80 296 / 77 664 B, MQTT=1 86 500 / 82 104 B; Gateway 55 388 / 72 624 B); `REAL=1`: Zigbee-IO MQTT=0 81 284 / 78 184 B, MQTT=1 87 476 / 82 616 B, Gateway 56 348 / 73 016 B (≈ +1 KB FLASH, +0,4–0,5 KB RAM).
R2..R5 (Rule Engine float, Publish, MB_READ, SUB) CHƯA code; chưa có bit capability REAL trên `0x0A08` (xem quyết định 42).

**Slave RS485 cho board IO (thử nghiệm, đã chạy với gateway thật 16/10/2026):** khối `TEST_RS485` của `Core/Src/main.c`
(`rio_handle()`, task `rs485_test_task`), USART1 (TX PB14, RX PB15, DE=PB13 GPIO, 115200 8N1), unit `RS485_TEST_UNIT_ID=1`.
Modbus RTU FC03+FC16 theo khối `0x0C00..0x0C06` (architecture 5.1): quét DI (IN0..3 = PB6/5/4/3) và áp DO
(OUT0..3 = PA6/PA7/PB0/PB1, `do=(do&~mask)|(value&mask)`); lease/SAFE/STATUS có cài, chỉ ở RAM. Sai CRC/unit → bỏ qua;
sai địa chỉ → `0x02`; lease ngoài `0|1000..60000` → `0x03`. Phát chặn. `RS485_TEST_TX_PERIOD_MS` phải = 0.
**Chưa verify:** hết lease → DO về SAFE rồi sync lại; soak dài; nhiều node trên một bus; build `TEST_RS485=0` cùng engine.
**Lưu ý:** `TEST_RS485` và `TEST_LED_RGB` mặc định bật ⇒ linker bỏ thư viện PLC; build thật phải
`-DCMAKE_C_FLAGS="-DTEST_LED_RGB=0 -DTEST_RS485=0"`. Slave này không phải SimplePLC (quyết định 12).

**Số đo build chuẩn** (ARM Release, arm-none-eabi-gcc 13.2.1, **đo lại 19/10/2026** bằng `splc_config/splcopts.h`
sửa `SPLC_MQTT_ENABLE`, board Zigbee-IO, cờ test = 0):

| Cấu hình | FLASH | RAM |
|---|---|---|
| `SPLC_MQTT_ENABLE=0` (**baseline, phải ra ĐÚNG số này**) | 80 296 B | 77 664 B |
| `SPLC_MQTT_ENABLE=1` (có 0x0A08 + log chẩn đoán) | 86 500 B (+6 204) | 82 104 B (+4 440) |

Gateway chuẩn (`SPLC_BOARD_GATEWAY`, `SX_OS_USE_FREERTOS=0`, `SPLC_PORT_USB_CDC=0`, `SPLC_MQTT_ENABLE=0`, `SPLC_LOG_LEVEL=4`, đo 20/10/2026):
FLASH 55 388 B / RAM 72 624 B (số 55 356 cũ lệch 32 B). Với `SPLC_MBM_ENABLE=1`: FLASH 55 388 B (chưa ai gọi `read_fc` nên chưa đổi) / RAM 73 032 B
(+408 B: một master 168 → 576 B). Zigbee-IO `MQTT=0 + MBM=1`: FLASH 80 424 B (+128) / RAM 77 664 B (không có master tĩnh). Bản `MBM=0` ra ĐÚNG số cũ ở cả
hai board (đã so với build code gốc cùng cấu hình). Gateway với `splcopts.h` của host (FreeRTOS, logger host) KHÔNG so được với các số trên; so FLASH giữa
hai máy phải dùng hiệu số (bài học 24). `splc_config/splcopts.h` đặt `SPLC_MQTT_ENABLE 1`, `SPLC_PORT_USB_CDC 1`,
`SX_OS_USE_FREERTOS 1`, `SPLC_LOG_LEVEL 4` ⇒ build mặc định KHÔNG ra số chuẩn; đo baseline phải dùng
`-DSPLC_OPTS_DIR=<dir có splcopts.h với MQTT=0>`. `port_gw/splcopts.h` đặt `SPLC_LOG_LEVEL 3`.

**Build ARM sạch nhưng CHƯA chạy trên board:** Zigbee-IO qua RS485 (29); đổi tên chống trùng host (21), `SPLC_AS_LIBRARY` (22),
`SPLC_RTC/ADC_BUILTIN` (23), `SPLC_FLASH_DATA_ADDR` (17); chế độ USB CDC-only của gateway (28); Remote I/O qua RS485.

**Nhúng vào `wg_ue`:** SimplePLC thêm +22 464 B RAM, +26 780 B FLASH (số đo trước MQTT). RAM host gần đầy:
`configTOTAL_HEAP_SIZE`=296 KB, dư 2 992 B RAM tĩnh (quyết định 26); số đo sau khi bật MQTT chưa ghi.

**Lỗi Deploy MQTT từ App không xuống firmware: App đã sửa (`paa` `cdc0511`), CHƯA chạy lại để xác nhận.** Triệu chứng cũ: App báo SUCCESS nhưng
firmware ghi `COMMIT: MQTT draft_mask=0x00 ... app_configured=0 generation=0`. Nguyên nhân: `RuleTableGateway` chỉ override 2 overload nên C# rơi vào
default interface method của `IRuleTableWriter`, vứt hết timers/counters/MQTT/broker. Bản sửa (đọc code + changelog 2.8.1, chưa chạy App): bỏ default
interface method, overload tiện ích thành static extension method, `RuleTableGateway` chuyển tiếp đủ 10 tham số, có test `RuleTableGatewayDeployTests`.
Firmware KHÔNG cần sửa. Cần xác nhận trên board: log App có `[CAPS] MCU capabilities: 0x11|0x13`, `[MQTT Broker] Writing ...`; log firmware có
`broker write`, `draft_mask` có bit `0x10` và `0x01`, `enabled=1 host_empty=0`, `app_configured=1 generation=1`; rồi đọc `0x0E54` (Connected /
`last_error_code`). Nếu project có khối MQTT SUB, Deploy sẽ FAIL rõ ràng (firmware chưa khai `MQTT_SUB`) — đúng thiết kế. Xác nhận xong thì hạ log
chẩn đoán (quyết định 37).

**Chưa làm:** bản đồ Flash host (4.A.5), slave board IO chính thức (4.A.4), chủ đồng hồ gateway (23), khoá Flash chung với
OTA (hoãn, 27), **MB_READ, MB_WRITE, MQTT Subscribe, PULSE_OUT, và R2..R5 của REAL (mục 4.F/F.5; chưa viết dòng code nào; R1 đã xong)**.

**Chưa verify (không chặn việc tiếp):** lease/SAFE slave board IO; chạy lại `test_plc/test_fb/test_retain` sau các thay đổi
đổi-tên/config/OS/Flash/logger; Remote I/O với board IO thật; FreeRTOS kernel thật (trễ task lúc erase Flash); PVD thật; mất
điện giữa lúc ghi Flash; `test_rtc.py --reboot`; đường RS485 của Zigbee-IO; hạ firmware `MQTT=1` → `MQTT=0` (bản ghi Flash bị
coi là hỏng → phải Deploy lại).

**Vấn đề mạng của host `wg_ue` (KHÔNG thuộc SimplePLC; đã đọc code + log, chưa sửa):**
- `network_manager.c` probe DNS mỗi 20 s (timeout 10 s); fail ≥ 2 lần ⇒ interface bị demote (chờ 120 s, gấp đôi mỗi lần, tối
  đa 900 s) ⇒ route NONE ⇒ MQTT bị ngắt. Trông như "ngắt định kỳ"; MQTT client không có timer ngắt.
- `network_probe_hostname()` đọc `g_sys_config.mqtt.broker_url` (broker mặc định của host), KHÔNG đọc override của SimplePLC.
- ETH liên tục `DHCP timeout`; modem LTE `a7680c` có log `AT< (ERROR)`.
- COMMIT lúc gateway offline: override có hiệu lực ở lần thử kế nhưng port chỉ ép reconnect khi `connected && conn_gen !=
  gen` ⇒ chờ hết backoff (tới 60 s). Đúng hành vi, chỉ chậm.
- Muốn sửa (cần người dùng đồng ý, file host): probe dùng host của override hoặc `pool.ntp.org`; nới
  `PROBE_FAIL_THRESHOLD/TIMEOUT_MS`; reset backoff khi cấu hình đổi lúc offline.
- DataBroker của host publish `factory/{gateway_id}/{device_id}/telemetry` với `{gateway_id}` CHƯA được thay (hiện
  `"data":{}`): placeholder của host; topic phải khác topic của SimplePLC.

**Thử dải LED WS2812B (`TEST_LED_RGB`, `Core/Src/main.c`):** PA6 phát xung đúng chu kỳ 1,25 µs, nhưng tại cực OUT0 trên
board (sau tầng đệm/đổi mức) mất xung ⇒ LED không sáng. Chưa chốt cách xử lý; cần biết linh kiện đệm. Liên quan trực tiếp
đến DIRECT_WRITE mode BUS (4.F).

## 1. Cơ chế cần giữ (không sửa lại trừ khi có lý do mới)

- **Scan loop** (`app/plc_app/plc_engine.c`, 10 ms): `diag_tick → input_scan → [engine không bị diag treo: rule_scan +
  plc_fb_scan] → output_scan → plc_mqtt_scan (bỏ qua khi diag treo) → modbus_config_service → retain_service → ghi scan_time
  → plc_system_cmd_service` (cuối cùng vì có thể reset).
- **Rule Table lưu Flash A/B** (`plc_rule_flash`): copy A→B, erase+ghi A, đọc lại kiểm CRC và seq_num mới, lỗi thì B→A. Đoạn FB
  (128 B; bản cũ 112 B vẫn nạp được) nằm TRONG bản ghi: bit 15 `rule_count` = có FB, bit 14 = FB 128 B, bit 13 = có đoạn MQTT
  960 B (chỉ khi `SPLC_MQTT_ENABLE=1`; tắt thì bit 13 vẫn thuộc số rule, mask 0x3FFF), bit 0..12 = số rule; bố cục
  `header | rule | FB | MQTT | TYPES`, một CRC phủ tất cả (TYPES = bit 11, 128 B, chỉ khi `SPLC_REAL_ENABLE=1`; mặt nạ đếm = `0x3FFF` trừ các cờ mà build sở hữu: `0x37FF` với REAL, `0x1FFF` với MQTT, `0x17FF` với cả hai). Mọi code đọc trường này phải tách cờ trước. Mật khẩu broker nằm nguyên văn
  trong đoạn MQTT.
- **CRC Rule/Retain tính trên byte WIRE** (`rule_table_wire_crc16()`). `nmbs_crc_calc()` trả kết quả hoán byte (chỉ cho RTU framing).
- **Retain** (`plc_retain`): log xoay vòng 3 sector, record 208 B; lưu khi COMMIT_RETAIN, chu kỳ `RETAIN_SNAPSHOT_PERIOD_MS` (chỉ
  khi dữ liệu đổi), PVD khẩn cấp (`retain_emergency_snapshot()` trong ISR: không erase, không log, hoãn nếu vòng chính đang
  thao tác Flash). Mọi thao tác Flash ngoài `plc_retain.c` phải bọc `retain_flash_op_begin/end()`.
- **PVD** nối bằng file dự án (`stm32h5_pwd.c` định nghĩa `PVD_AVD_IRQHandler`); nếu CubeMX tự sinh handler thì định nghĩa
  `SPLC_PVD_IRQ_HANDLER_FROM_CUBEMX`. Tắt bằng `PLC_PVD_EMERGENCY_SAVE_ENABLE 0`.
- **System command:** luôn chạy ngay (bất kể diag/dirty). REBOOT trì hoãn `PLC_REBOOT_DELAY_MS` (300 ms) cho phản hồi kịp ra —
  ĐỪNG bỏ. CLEAR_RULES/CLEAR_RETAIN/FACTORY_RESET ghi một bản ghi RỖNG hợp lệ qua đường lưu đã verify; FACTORY_RESET =
  CLEAR_RETAIN + CLEAR_RULES, không reboot (CLEAR_RULES/FACTORY_RESET xoá cả cấu hình MQTT, có backup/rollback nếu lưu lỗi).
- **Giới hạn Modbus:** FC16 ≤ 123 reg, FC03 ≤ 125 reg. Gói USB FS 64 B nên App chia 1 rule (41 B)/lệnh. `plc_modbus_cfg` đặt
  `read_timeout=0` nhưng `byte_timeout>0` sau khi request đã bắt đầu — nhờ đó mảnh gói thứ hai được ghép vào khung.
- **`modbus_link` (ống byte App):** core giữ ring RX (SPSC lock-free) và MỌI timeout (`read` chờ bằng tick thật; `write` timeout
  **idle**). `splc_link_input()` từ đúng MỘT ngữ cảnh (task hoặc ISR, không chặn); `output()/process()/transport` chạy ở ngữ
  cảnh engine; `register` chỉ lúc khởi tạo. Chi tiết: `architecture.md` 3.1.
- **nanoMODBUS riêng (`libs/splc_nanomodbus`):** `splc_nmbs.h` đổi tên 37 hàm toàn cục, `#undef` mọi macro `NMBS_*`, include
  header vendored bằng đường dẫn tương đối; `libs/nanomodbus/` giữ nguyên. `splc_check_symbols.cmake` chạy sau build; nâng cấp
  thư viện mà có hàm toàn cục mới thì build fail → thêm một `#define`.
- **Multi-board:** `SPLC_TagLayout` truyền qua tham số; chọn bằng `SPLC_BOARD` trong `splcopts.h` (bảng ở
  `cmake_modules/splc_select.cmake`); thêm board = 1 file `board_<sku>.c` + 1 hàng bảng. `MAX_TAGS` ở `board/board_tag_define.h`.
- **Remote I/O (Gateway):** `plc_io.c` gọi `plc_io_remote_input_scan()` cuối `input_scan()` và `plc_io_remote_output_scan()` cuối
  `output_scan()` (tối đa 1 request RS485 mới/scan, không bao giờ chặn). Ưu tiên SYNC > WRITE > REFRESH > POLL (architecture 5.1).
  Node offline: tag DI giữ giá trị cuối; rule cần biết thì dùng `plc_io_remote_node_online()`.
- **Cấu hình kiểu lwIP:** người dùng chỉ sửa `splcopts.h`; `config/splc_opt.h` đặt mặc định + `#error` khi sai. Thêm option =
  thêm vào `splc_opt.h` (mặc định + check) và `splcopts_template.h`. CMake đọc `SPLC_BOARD`, `SPLC_PLATFORM`,
  `SPLC_LOGGER_BUILTIN`, `SPLC_PORT_USB_CDC`, `SPLC_RTC_BUILTIN`, `SPLC_ADC_BUILTIN` thẳng từ `splcopts.h`; `-D` chỉ có tác dụng
  khi `splcopts.h` không định nghĩa trùng. KHÔNG phải option: hằng wire, kích thước sector, tag layout.
- **Mức log:** `SPLC_LOG_LEVEL` 0 OFF, 1 ERROR, 2 WARN, 3 INFO, 4 DEBUG (`config/splc_opt.h`, mặc định 4).
- **Chế độ OS:** `SX_OS_USE_FREERTOS=0` bare-metal; `=1` host gọi `plc_engine_init()` rồi `plc_engine_poll()` từ MỘT task — thư
  viện đơn luồng không khoá, task khác KHÔNG được đụng tag/rule/draft/retain; không log từ ISR. `sx_get_tick_ms()` giữ `HAL_GetTick()`.

## 2. Quyết định đã chốt (người dùng xác nhận — ĐỪNG tự đổi, hỏi lại nếu nghi)

**Wire / engine**
1. `protocol_version = 2`.
2. RTC = RTC nội STM32H5, clock **LSI**, không VBAT/LSE; mất điện → chưa đồng bộ, reset mềm giữ giờ. Linker
   `--wrap=HAL_RTC_SetTime/SetDate` để `MX_RTC_Init()` không xoá giờ. `status_flags` (`0x0813`) RO; `tz_offset_min` chỉ trong
   RAM (sau REBOOT về 0, Time Window im đến khi Studio ghi lại).
3. Time Window theo Structs v2.0 mục 7: `Lo<=Hi` khung trong ngày, `Lo>Hi` qua nửa đêm (bắn mỗi scan khi đúng); `compare_op==EQ`
   và `Lo==Hi` = mốc phút (bắn 1 lần ở sườn lên); `trigger_tag` bị bỏ qua.
4. SYSTEM_COMMAND luôn chạy ngay; dirty interlock chỉ áp cho `CMD_EXIT_DIAG`. Trong diag, trước SYSTEM_COMMAND, tag host đã ghi
   về 0. Thoát diag (EXIT/hết lease): `rule_runtime_reset()` mọi rule như vừa nạp.
5. `DISCARD_RETAIN` bỏ bản nháp, KHÔNG reload Flash. Retain dùng `plc_retain.c` (lệch tài liệu Appendix A có chủ đích).
6. **FB "dual generation":** App vẫn sinh macro rule (Timer/Counter chạy bằng Rule Engine) VÀ ghi bảng FB; firmware chỉ lưu cấu
   hình và BÁO CÁO. `0x0B00..0x0B7F` R/W; Host sở hữu `mode`, `pt_ms`/`preset`, thanh ghi `+6`; firmware sở hữu `status_bits`,
   `et_ms`/`current_value`, `reserved`. Timer 1=TON 2=TOF 3=TP; Counter 1=CTU 2=CTD; 0 = không dùng.
7. **Counter:** `+6` = index toàn cục tag chứa CV (VFLAG/VREG/VREG_RETAIN/COUNTER; `0xFFFF` = không có; DI/DO/AI/Modbus bị từ
   chối `0x03`; không hai counter trùng tag). Firmware chỉ ĐỌC tag: `Q` = CTU `CV>=PV`, CTD `CV<=0`. **Timer:** `+6` = `rule_ref`
   (rule index + 1, 0 = chưa gắn).
8. **FB qua bản nháp:** ghi vào nháp (validate ngay, all-or-nothing, `0x02/0x03`), đọc `0x0B00..` luôn trả cấu hình ĐANG CHẠY.
   COMMIT: khối có cờ nhận nháp, khối không ghi → DISABLED, rồi lưu Flash cùng Rule Table. Nháp bị xoá sau COMMIT (kể cả lỗi),
   `CLEAR_RULES`/`FACTORY_RESET`, và khi boot ⇒ mỗi Deploy App ghi lại FB từ đầu.
9. **Không sửa file CubeMX tự sinh, kể cả khối `USER CODE`.** Cần hành vi khác → chặn ở lớp thấp (linker `--wrap`, lớp `sx_*`).
   Thấy bug thật trong file sinh (vd. `STM32H523xx_FLASH.ld` khai `LENGTH=512K` trên chip 256 KB) → báo, HỎI trước khi sửa.
   Ngoại lệ có chủ đích: `FreeRTOSConfig.h` của `wg_ue` (quyết định 26).
10. `0x0A20` đọc trả lệnh cuối đã ghi. Layout tag board Zigbee-IO là DENSE (DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71, RETAIN 72-103,
    COUNTER 104-111), khác bảng cố định Structs mục 4 — code validate theo KIND; test script tính index từ `DeviceResourceInfo`.
11. **Flash:** vùng dữ liệu (Rule A/B + Retain) LUÔN là 5 sector cuối, tính ngược từ `END = FLASH_BASE + SPLC_FLASH_SIZE_KB*1024`.
    Sector 8 KB là hằng họ H5; giả định dual-bank chia đôi. Kiểm lúc biên dịch: bội số 16 KB, 128..2048, 5 sector trong bank 2.

**Kiến trúc nhúng**
12. **Gateway:** SimplePLC chạy trên gateway; board IO là **slave RS485 thuần** (nhận lệnh DO, được hỏi thì trả DI) — KHÔNG chạy
    SimplePLC/Rule Engine/Flash rule. DI/DO ảo ↔ (cổng RS485, unit ID, bit) khai cứng lúc biên dịch trong file board
    (`GATEWAY_IO_NODES`). Link: khối `0x0C00..0x0C06` (architecture 5.1).
13. **Nhúng vào firmware chủ (kiểu lwIP):** người dùng chỉ viết `splcopts.h` và (khi cần) một file `.c` port; chọn board CÓ SẴN bằng
    `SPLC_BOARD`. (a) tài nguyên có trạng thái/phần cứng (USB, UART, Flash, ngắt) chỉ có MỘT chủ; (b) thư viện thuần (nanoMODBUS,
    cqueue) SimplePLC LUÔN dùng bản riêng đổi tên. Core chỉ thấy một "ống byte" (`modbus_link`); core không phụ thuộc TinyUSB —
    USB-CDC là module port tùy chọn `port/usb/` (`SPLC_PORT_USB_CDC`, mặc định 0). Logic chờ timeout nằm trong core, một chỗ.
14. **Logger:** SimplePLC chỉ gọi `log_error/warn/info/debug` (`config/splc_log.h`) → `SPLC_LOG_OUTPUT`. `SPLC_LOGGER_BUILTIN=1`
    (mặc định): dùng `utils/logger`; `=0`: host cung cấp `logger.h` và gọi `logger_init`.
15. **nanoMODBUS luôn là bản riêng đổi tên symbol** (~7,5 KB Flash). TinyUSB KHÔNG đổi tên được — chỉ một chủ.
16. **Đăng ký ống tường minh (kiểu `netif_add`), không dùng hàm `weak`:** `splc_link_register(&ops)` trước `plc_engine_init()`; quên
    đăng ký thì `splc_link_get_transport()` trả `false` và phải báo lỗi rõ. `output()` KHÔNG được gọi thẳng driver không an toàn
    luồng: đẩy byte vào queue của task driver.
17. **Vùng Flash dữ liệu do host khai được:** `SPLC_FLASH_DATA_ADDR` (tuỳ chọn). Khai = địa chỉ sector thấp nhất của 40 KB: Retain
    +0/+8K/+16K, Rule B +24K, Rule A +32K; chia hết 8 KB, trong bank 2 (`_Static_assert`). `wg_ue` dùng hết 2 MB nên phải khai.
    Bản đồ đề xuất cho host (CHƯA nạp): xem 4.A.5.
18. **RS485 master qua callback:** `modbus_serial_port_t {read (không chặn), write (0 = xong, trả về sau khi bit cuối ra khỏi UART),
    set_tx (NULL nếu port tự điều khiển DE), ctx, baudrate}`; Layer 3.5 không phụ thuộc `sx_uart/sx_gpio`. Host đăng ký bằng
    `splc_gateway_set_rs485_port()`. Lưu ý: `rs485_recv_polling(port, buf, n, 0)` của host tăng `counters.timeouts` mỗi lần rỗng.
19. **Nhúng `wg_ue` KHÔNG sửa `usb_netif.c` và `rs485_driver.h`.** Task engine tự đọc CDC instance 0 mỗi scan → `splc_link_input()`,
    ghi trả lời vào FIFO TX rồi flush (trễ tối đa 1 scan). Owner cổng RS485 = `RS485_OWNER_MODBUS_GATEWAY`; host phải dành riêng
    cổng. Cổng do `SPLC_HOST_RS485_PORT` (0/1/2 = USART3/6/10). Host chỉ sửa: 1 dòng `splc_host_start();` trong `gateway.c` (sau
    `modbus_master_init();`) + CMake `ports/splc_port/`.
21. **Tên không được trùng host.** Mọi file/symbol/guard có thể trùng firmware chủ mang tiền tố `splc_` (`splc_cqueue`,
    `splc_board`, `splc_app_config.h`, `splc_nmbs.h`). File/symbol MỚI phải kiểm trùng với `wg_ue` (header basename + `nm`) trước
    khi coi là xong. (Tên `plc_mqtt*`/`splc_mqtt*` đã kiểm: chưa trùng.)
22. **Hai chế độ build:** `SPLC_AS_LIBRARY=OFF` (mặc định) = firmware độc lập. `=ON` = thư viện cho host: host `add_subdirectory`
    rồi link INTERFACE target `splc`. Host PHẢI tự: thêm `$<TARGET_OBJECTS:splc_platform_freertos>` vào executable khi
    `SX_OS_USE_FREERTOS=1`; thêm `-Wl,--wrap=HAL_RTC_SetTime/SetDate` nếu `SPLC_RTC_BUILTIN=1`.
23. **`SPLC_RTC_BUILTIN` / `SPLC_ADC_BUILTIN`** (mặc định 1). `=0`: host tự cấp `sx_rtc_*` / `sx_adc_*`. Gateway đặt cả hai = 0;
    `sx_rtc_*` trong `splc_port.c` là stub "chưa có giờ" (Time Window không bắn, App ghi RTC bị từ chối, `{ts}` MQTT = 0).
    **Chủ sở hữu đồng hồ CHƯA chốt:** NTP của host trả giờ ĐỊA PHƯƠNG, SimplePLC cần epoch UTC + `tz_offset_min`.
24. **Bus tới board IO:** 115200 8N1; cổng mặc định USART6 trước, nay dời sang USART3 (quyết định 30). `modbus_master_init()` của
    host cấu hình cả 3 cổng và `splc_check_port()` đặt lại baud mỗi giây — nếu cổng bị cấu hình cho dịch vụ khác, hai bên giành nhau.
25. **Cách nhúng vào `wg_ue`:** thư viện ở `libs/simple_plc/` (submodule), port ở `ports/splc_port/` (`CMakeLists.txt`,
    `splc_port.c/.h`, `splc_mqtt_port.c/.h`, `splcopts.h`). Quy tắc: (a) target host tên `ports_splc_port`; (b)
    `ports/splc_port/CMakeLists.txt` `set(SPLC_AS_LIBRARY ON ... FORCE)` và `set(SPLC_OPTS_DIR ... FORCE)` TRƯỚC
    `add_subdirectory`; (c) `splc_platform_freertos` gắn vào **executable** của host, KHÔNG vào static library; (d) `logger.h`
    của host thấy được nhờ `include_directories(libs/logger ...)` ở scope gốc; (e) file host đã sửa: `CMakeLists.txt` gốc,
    `apps/main/CMakeLists.txt`, `apps/main/gateway.c`, và cho MQTT `apps/mqtt_client/mqtt_client.{c,h}` (`mqtt_client_publish_ex`,
    `mqtt_client_set_override`, `mqtt_client_connected_override_generation`). Không sửa `usb_netif.c`, `rs485_driver.*`,
    `flash_port.c`, file CubeMX. Sửa host chỉ làm trên bản clone ở sandbox, người dùng tự chuyển sang repo chính. Baseline `wg_ue`
    trên Linux cần workaround KHÔNG thuộc dự án này (hoa/thường tên file, thiếu `#include "semphr.h"`/`"queue.h"`) — chỉ vá ở
    sandbox. `network_manager.c` CHƯA từng được phép sửa.
26. **RAM cho SimplePLC trên `wg_ue`:** hạ `configTOTAL_HEAP_SIZE` trong `Core/Inc/FreeRTOSConfig.h` dòng 74 bằng tay (chấp nhận
    file CubeMX bị sửa; sinh lại sẽ mất). Đang dùng **296 KB**: dư chỉ 2 992 B RAM tĩnh — buffer tĩnh mới > ~3 KB sẽ làm link
    FAIL. Điều kiện: `lowest free` thật của host (log `MONITOR`, `Core/Src/app_freertos.c`) ≥ ~30 KB khi chạy đủ tải.
27. **OTA của `wg_ue` KHÔNG dùng (người dùng xác nhận) → không làm khoá Flash chung.** `ota_init()` + task `OTA_HEALTH` còn trong
    `gateway.c`, KHÔNG xoá khi chưa đọc `ota.c` + `bootloader.c` (`ota_mark_healthy()` có thể là bước xác nhận image). Nếu sau này
    dùng OTA phải thiết kế khoá Flash (PVD ghi retain trong ISR, không được chờ mutex).
28. **Chế độ USB CDC-only cho gateway** (nhánh `usb_cdc_gw` của `wg_ue`). Với `GW_USB_NETIF_ENABLE=ON` PC báo "Unknown USB
    Device"; tắt SimplePLC vẫn lỗi ⇒ KHÔNG do SimplePLC; nguyên nhân CHƯA biết. Cờ `GW_USB_CDC_ENABLE`, TinyUSB của host, một ACM,
    PID 0x4001. Phép thử: enumerate được ⇒ lỗi ở descriptor composite RNDIS/ECM; vẫn Unknown ⇒ lỗi tầng chung. **Chưa chạy trên board.**
29. **Zigbee-IO nói chuyện với App qua RS485 (phía user, KHÔNG thêm module port):** `rs485_output/rs485_pump` nằm trong
    `board_zigbee_io.c`; `SPLC_PORT_USB_CDC` chọn USB (1) hay RS485 (0). USART1, DE = PB13, 115200 8N1, cực DE `RS485_DE_ACTIVE_HIGH`.
    Phát CHẶN (~4 ms/khung 41 B) — lệch hợp đồng "output không chặn", chấp nhận cho test. Hai sửa Layer 0 (`stm32h5_uart.c`): khoá ngắt
    quanh `splc_cqueue_*`; `HAL_UART_ErrorCallback` nhận lại RX sau overrun. **Chưa chạy trên board.**
30. **Gateway nói chuyện với App qua RS485 — làm ở glue của HOST (`wg_ue/ports/splc_port/`, bản chuẩn; `port_gw/` trong repo này là bản
    nháp cũ), SimplePLC không đổi.** `app_output()` = `rs485_send()` (host tự điều khiển DE), `app_process()` =
    `rs485_recv_polling(...,0)` → `splc_link_input()`. Option `SPLC_HOST_APP_RS485_PORT` (âm = USB CDC, mặc định) + `_BAUD/_PARITY`;
    owner `RS485_OWNER_MODBUS_GATEWAY`. `#error` khi cổng App trùng cổng IO, hoặc `GW_USB_CDC_ENABLE=ON` cùng RS485. Cổng App = USART6
    ⇒ bus board IO dời sang USART3 (`SPLC_HOST_RS485_PORT 0`). Phát CHẶN `rs485_send` (~3,6 ms/41 B). Phần cứng: `RE/DE` MAX485 nối
    chung một GPIO ⇒ DE tích cực cao. Triệu chứng lạ (khung hỏng, break 1 s) chỉ hết sau khi **thay IC MAX485** (quy kết, chưa đo
    scope). Chưa đo, không chặn: (a) phân cực bus rảnh yếu (R40/R28 → A−B ~30 mV < ngưỡng ±200 mV); (b) R17 kéo `RE/DE` lên 3V3 lúc
    reset; (c) kiểm BOM xem IC là MAX3485/SP3485 (3,3 V) hay MAX485 5V. Công cụ `gw_rs485_diag.c` (`GW_RS485_DIAG_*`): **các cờ
    DRAIN/TX_TEST/CAPTURE mặc định = 1 → đặt 0 trước khi commit/chạy thật** (drain nuốt byte ⇒ SimplePLC không nhận được gì).

**MQTT**
31. **SimplePLC chỉ làm LOGIC và GIỮ DỮ LIỆU, không có client mạng.** Không TCP/TLS/MQTT client trong lib; truyền mạng do host cắm qua
    ống `publish` kiểu `netif` (cùng triết lý 13/16). Bật bằng `SPLC_MQTT_ENABLE` (mặc định 0). Lib GIỮ cấu hình broker (host, port,
    client_id, user, pass, TLS, clean session, keepalive, version) vì App gửi `0x0E00` ở mọi lần deploy; user đọc qua
    `splc_mqtt_get_broker(out, &app_configured, &generation)` (copy ra). `generation` chỉ tăng khi cấu hình THẬT SỰ đổi hoặc
    `app_configured` đổi. `app_configured` = đã nhận broker từ App VÀ `enabled=1` VÀ host không rỗng; `false` → host dùng cấu hình
    mặc định (`g_sys_config.mqtt`), `true` → dùng cấu hình của lib. `enabled=0` = quay về mặc định host (không tắt MQTT được bằng ô
    Enabled; muốn ngừng publish thì tắt các khối telemetry). Host báo trạng thái bằng `splc_mqtt_set_status` (CONNECTED/TLS/AUTH,
    latency, last_error, **generation của cấu hình mà kết nối được dựng theo**); lib chỉ publish khi CONNECTED **và**
    `status.generation == generation`. Password đọc lại luôn trả 0 nhưng nằm nguyên văn trong Flash — ghi rõ khi giao tài liệu.
    **Mọi hàm `splc_mqtt_*` chỉ gọi từ ngữ cảnh engine** (port bơm status/broker trong hàm bơm của task `SPLC`, chu kỳ 100 ms).
32. **Lưu/áp dụng cấu hình MQTT:** ghi vào BẢN NHÁP (kiểm rẻ từng mảnh, địa chỉ ngoài bản đồ → `0x02`), kiểm nội dung cả bộ lúc COMMIT,
    áp dụng nguyên tử, lưu Flash cùng Rule Table (một CRC; cờ MQTT = bit 13 của `rule_count`; bản ghi cũ bit 13 = 0 vẫn nạp được; tối
    đa 4 304 B < sector 8 KB). Khối KHÔNG được ghi trong nháp thì GIỮ cấu hình đang chạy (KHÁC FB, quyết định 8). Vùng
    `0x0E00..0x0E53` luôn ACK, lỗi bắt ở COMMIT. App ghi thứ tự Timer → Counter → Telemetry → Broker → staging/commit.
33. **Layout khối MQTT Telemetry/Broker:** Header `splc_mqtt_wire.h` + Structs 3.9/3.10 + golden vector là đúng (`MqttTelemetryCodec.cs`
    của App khớp Header). ICD `SPLC_SPEC_MQTT_001` sai (layout khối Telemetry, code mẫu `memcpy` thẳng vào struct) — đã báo đội App.
34. **MQTT Subscribe:** App đã có ICD `SPLC-SPEC-MQTT-002` và vùng `0x0D80..0x0DFF`; layout ICD và `MqttSubscriptionCodec` khớp, header C đúng kích thước, codec ghi đủ fail-safe `0/1/2` (`paa` `cdc0511`). Hướng dự kiến: ống đối xứng `splc_mqtt_input(...)` cắm bởi host (quyết định 31). Thứ tự làm so với MB_READ chưa chốt; còn các câu hỏi ở F.2–F.3.
35. **Thiết kế MQTT Publish (chi tiết `docs/plc_mqtt_design.md`):**
    - **Q1** QoS/retain theo từng khối; host có `mqtt_client_publish_ex(topic, payload, qos, retain)`.
    - **Q2** chưa dùng TLS: `SPLC_MQTT_TLS_SUPPORTED=0`, COMMIT từ chối `use_tls=1`. Host không hỗ trợ `clean_session=0`/MQTT 5 (port
      ghi log cảnh báo và bỏ qua).
    - **Q3** cấu hình App vào client host qua lớp ghi đè chỉ trong RAM (`mqtt_client_set_override`, đọc nguyên khối trong critical
      section), KHÔNG chép vào `g_sys_config` (vì `config_save()` ghi cả struct xuống file). Broker App không vừa buffer host
      (31/31/47/63 ký tự), hoặc TLS ⇒ KHÔNG áp dụng, host giữ broker mặc định (có log cảnh báo). `base_topic` (LWT) giữ của host.
    - **Q4** không publish khi `diag` đang treo engine. **Q5** bắn một lần ngay khi sẵn sàng (giá trị nền).
    - **Q6** token Custom cho mọi tag: `{valK}`, `{valK:N}`, `{tagK}`, `{vals}`, `{ts}`, `{device}`. **Q7** `{ts}` = 0 khi chưa có giờ.
    - **Q8** `reserved_header` (+15) = 8 tag × 2 bit số chữ số lẻ (0 = số nguyên); `deadband` theo đơn vị kỹ thuật (`deadband × 10^dec` so
      với delta thô); App thăm dò `0x0E54` trước khi gửi MQTT. **Đề xuất phía firmware, đã gửi đội App, CHƯA có xác nhận.**
    - **Số thập phân:** tag là `int32`; in bằng số nguyên + hệ số 10^dec (0..3), KHÔNG dùng printf float; số âm giữ dấu.
    - **Thuật toán:** tối đa 1 publish/scan, quay vòng 4 khối; ON_CHANGE / COV / PERIODIC; `publish` trả `<0` thì không cập nhật
      `last_value`, thử lại sau `SPLC_MQTT_RETRY_MS`; mất kết nối → bỏ dữ liệu, không tự xếp hàng. Nối lại broker → mọi khối bắn lại
      baseline một lần; enable tag đi 0 → khác 0 → bắn lại baseline; `tx_message_count` đếm số lần callback trả 0 (host ĐÃ NHẬN, chưa phải
      broker đã ack); lỗi dựng payload chỉ đếm vào `splc_mqtt_format_errors()` và log một lần.
    - `{device}` trên gateway lấy từ `g_sys_config.device.device_id`, KHÔNG phải `g_sys_config.mqtt.client_id` (rỗng).
36. **Thanh ghi khả năng `0x0A08` (2 reg, uint32 big-endian) CHỈ tồn tại khi `SPLC_MQTT_ENABLE=1`** (người dùng chốt 19/10/2026; baseline
    MQTT=0 giữ 80 296/77 664 B). Bit: BASE_LOGIC 0x1, RETENTIVE 0x2, RTC 0x4, ETHERNET 0x8, MQTT_PUB 0x10, MQTT_SUB 0x20, SD 0x40 (header App
    còn có MODBUS_MASTER 0x100 và WIFI 0x200 — firmware CHƯA khai). Hàm `read_hw_capabilities` trong `plc_modbus_cfg.c` khai MQTT_PUB, thêm BASE_LOGIC nếu
    `max_rules>0`, RETENTIVE nếu `vreg_retain_count>0`. App phải coi Modbus exception `0x02` khi đọc `0x0A08` = "thiết bị chỉ có khả năng cơ
    bản"; nhưng rule có khối MQTT mà thiết bị không khai bit tương ứng ⇒ FAIL deploy kèm lý do, không báo SUCCESS.
37. **Log chẩn đoán MQTT đang ở mức INFO** (`plc_mqtt.c`, `plc_modbus_cfg.c`): `telemetry write`, `broker write`, `hw capabilities read`,
    `COMMIT: MQTT draft_mask=.. broker_draft: ..`. Giữ INFO cho tới khi lỗi Deploy (mục 0) được xác nhận xong; rồi đổi lời gọi
    `log_info` → `log_debug` (đổi `SPLC_LOG_LEVEL` một mình KHÔNG ẩn được chúng).
38. **MB_READ / MB_WRITE (người dùng chốt 19/10/2026):** làm MB_READ trước, rồi MB_WRITE. Master Modbus của lib phải hỗ trợ tới **125 thanh ghi/request** (không chặn ở 16): mở rộng `modbus_serial` (nay chỉ FC03/FC16, `MODBUS_MASTER_MAX_REGS 16`, `MODBUS_MASTER_FRAME_MAX 64`) thêm FC01/02/04/05/06/15 và khung 256 B. Nguồn chuẩn layout = `ModbusMasterCodec.cs` + header (ICD khớp từ `cdc0511`); firmware giải mã từng thanh ghi, KHÔNG ép struct vào byte wire (thanh ghi big-endian, High byte trước).
39. **Cổng RS485 cho MB_READ cấu hình được trong `splcopts.h` (người dùng chốt 19–20/10/2026; TÊN option là đề xuất, chưa code):** host đăng ký cổng qua `splc_mbm_set_rs485_port()` (cùng kiểu `splc_gateway_set_rs485_port()`, quyết định 18); `SPLC_MBM_ENABLE` (mặc định 0, baseline MQTT=0 không đổi); host chọn USART bằng `SPLC_HOST_MBM_RS485_PORT`. KHÔNG có option chế độ: chế độ dùng chung được SUY RA từ cấu hình cổng và **chỉ cho phép khi App KHÔNG dùng USB** (`SPLC_PORT_USB_CDC=0`, App đi RS485): (a) cổng MB_READ trùng cổng App ⇒ dùng chung với App, MB_READ NHƯỜNG App — khi Studio đang kết nối (có khung đến trong `SPLC_MBM_APP_YIELD_MS`, đề xuất 3000 ms) MB_READ không phát request mới, tag giữ giá trị cuối; Studio im quá thời gian đó thì chạy lại; (b) cổng MB_READ trùng cổng Remote I/O ⇒ dùng chung với IO, cần bộ phân xử và cùng baud. Khi App đi USB (`SPLC_PORT_USB_CDC=1`), cổng MB_READ PHẢI là cổng riêng (khác cổng Remote I/O): trùng ⇒ `#error`. Firmware in một dòng log lúc khởi động cho biết đang ở chế độ nào. Hệ quả của (a): glue host cần MỘT bộ bơm byte duy nhất (master đang chờ trả lời → byte vào master, ngược lại → `splc_link_input()`), vì hai bên đọc chung hàng đợi RX; Studio đọc live liên tục nên khi đang kết nối MB_READ gần như dừng — chế độ này dành cho nạp/cấu hình, không cho chạy sản xuất lâu dài.
40. **REAL / Float32 là yêu cầu bắt buộc (đội App, changelog 2.9.0, 21/10/2026; người dùng xác nhận làm):** firmware hỗ trợ tag BOOL/INT32/REAL qua **Tag DataType Registry** `0x0080..0x00BF` (64 reg = 128 B, FC03/FC16 R/W, 1 byte/tag: 1=BOOL 2=INT32 3=REAL; tag `2k` ở High byte, tag `2k+1` ở Low byte của Reg `0x0080+k`). Quy tắc App chốt: (a) Rule Engine: `rule.flags & 0x01` ⇒ so sánh float32 (`=,!=,>,<,>=,<=,BETWEEN`, dung sai ε=1e-5) và `SCALE_TAG` y = x·`action_param_f` + `threshold_hi_f`; Rule Engine CHỈ nhìn cờ này, không tra bảng; KHÔNG được từ chối/Exception 0x03. (b) MB_READ: pin `Real` ⇒ ghi float (đã nhân multiplier) vào tag; pin nguyên với multiplier lẻ ⇒ tra bảng: tag đích REAL ⇒ ghi float, INT32 ⇒ `(int32_t)roundf(raw*multiplier)`; Bool ⇒ 0/1. (c) MQTT Publish: tag REAL in `%.4g`, INT32 in số nguyên, BOOL in 0/1; offset +15 luôn 0 (bỏ `dec`, thay đề xuất E.7). Phạm vi: R1 bảng kiểu tag, R2 Rule Engine float, R3 Publish, R4 MB_READ, R5 MQTT SUB (F.5). Gate bằng option riêng `SPLC_REAL_ENABLE` (tên là đề xuất, mặc định 0 để baseline không đổi). Tag REAL = bit float32 trong ô `int32_t g_tag_value[]` (không đổi kiểu mảng).
41. **Lưu Flash (người dùng chốt 21/10/2026, cách 1):** các khối mới nằm trong CÙNG bản ghi Rule Table, mỗi khối một cờ trong trường `rule_count`: bit 12 = khối MB_READ (512 B = 4 × 64 reg), bit 11 = bảng kiểu tag (128 B); layout `header | rules | FB | MQTT | MB | TYPES`, một CRC chung. Mặt nạ đếm = `0x3FFF` trừ bit 13 khi `SPLC_MQTT_ENABLE=1`, trừ bit 12 khi `SPLC_MBM_ENABLE=1`, trừ bit 11 khi `SPLC_REAL_ENABLE=1`; build tắt giữ mặt nạ và kích thước cũ. Hạ firmware không xóa vùng cấu hình ⇒ bản cũ coi bản ghi là hỏng (cần Deploy lại); người dùng luôn xóa cả chip khi nạp code mới nên chấp nhận. **Đã code cho bit 11 (R1)**; bit 12 (MB_READ) vẫn thuộc mặt nạ đếm cho tới khi MB_READ có đoạn của nó (lúc đó trở thành cờ do `SPLC_MBM_ENABLE` sở hữu). Bản ghi `REAL=0` giống HỆT bản ghi của code gốc (đã so từng byte bằng harness PC ở cả `MQTT=0/1`); bản ghi cũ không có bit 11 vẫn nạp được bằng build `REAL=1` (bảng để chưa khai báo).
42. **R1 `plc_tagtype` (code xong, chạy board 21/10/2026):** bảng 128 B (1 byte/tag, 0 = chưa khai = INT32, 1 BOOL, 2 INT32, 3 REAL). Ghi qua BẢN NHÁP (sao từ bảng chạy ở lần ghi đầu nên ghi một phần giữ phần còn lại), kiểm cả bộ lúc COMMIT như MQTT: kiểu > 3, hoặc REAL trên tag có kind không thuộc {AI, VREG, VREG_RETAIN, MB_HOLDING} (kể cả ô dưới `MAX_TAGS` mà không nhóm layout nào phủ = kind NONE, vd. Gateway 112 tag trong `MAX_TAGS` 128) ⇒ từ chối CẢ COMMIT (`CONFIG_STATUS` ≠ READY, lỗi 2, bản nháp bị bỏ, rule/FB/MQTT giữ nguyên). Mục ứng với tag `>= MAX_TAGS` bị BỎ QUA (không lỗi). Khi COMMIT làm một tag VREG/VREG_RETAIN/MB_HOLDING đổi giữa số nguyên và REAL thì giá trị tag về 0 (bit cũ sẽ bị đọc sai kiểu); AI do lần quét vào kế ghi lại; BOOL↔INT32 giữ giá trị. Deploy không ghi bảng ⇒ giữ bảng đang chạy (như MQTT, KHÁC FB). Bảng chạy lưu trong bản ghi Flash cùng Rule Table (quyết định 41), `CLEAR_RULES`/`FACTORY_RESET` xoá bảng (có backup/rollback nếu lưu lỗi), `CLEAR_RETAIN` không đụng. `plc_tagtype_import()` kiểm cả ảnh, hỏng thì KHÔNG áp dụng gì và bảng để chưa khai. Rule Engine KHÔNG tra bảng (cờ `rule.flags & 0x01`, quyết định 40). **Cờ capability cho REAL CHƯA gửi lên dây** (`PLC_TAGTYPE_CAP_PROVISIONAL = 0x0800` chỉ là hằng giữ chỗ, không ai đọc) cho tới khi đội App xác nhận bit; hệ quả: App chưa thể biết firmware nhận bảng qua `0x0A08` — đọc thử `0x0080` (exception `0x02` ⇒ build `REAL=0`). Ghi chú đối chiếu: ghi bảng vào `0x0080..0x00BF` luôn ACK (kể cả nội dung sai), chỉ yêu cầu chạy quá `0x00BF` hoặc bắt đầu ở khoảng trống `0x007F` mới trả `0x02`.

## 3. Quy trình làm việc

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Đầu phiên hoặc khi người dùng báo đã push: `git pull`, rồi **build/compile verify
  thật** (không chỉ đọc diff). Cài được: `apt-get install gcc-arm-none-eabi ninja-build`, `pip install cmake
  --break-system-packages`, `git submodule update --init --depth 1 simple_plc/libs/tinyusb`. Build:
  `cmake -S . -B <dir> -G Ninja -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake -DCMAKE_BUILD_TYPE=Release
  "-DCMAKE_C_FLAGS=-DTEST_LED_RGB=0 -DTEST_RS485=0" [-DSPLC_OPTS_DIR=<dir>]` (build mặc định ở chế độ test nên linker bỏ thư viện PLC).
- **Giao nguyên file** (present từng file để copy-paste, giữ cây thư mục), KHÔNG patch/zip/đoạn thay thế; kèm danh sách file cần
  xoá/đổi tên. Present NGAY sau mỗi mốc. Chỉ giao code dự án + script test Python; KHÔNG giao harness C chạy trên PC (trừ khi người
  dùng yêu cầu). Việc tự kiểm tra nội bộ vẫn làm nhưng nêu rõ là chạy trên mô hình giả. Tính năng không test được trên board thì nói
  rõ là chưa test.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, từng quyết định nhỏ.
- Nói rõ mức chắc chắn khi đề xuất nguyên nhân; kiểm chứng bằng dữ liệu thật.
- Mỗi bước mới: code → build ARM (Zigbee-IO cả `MQTT=0` và `MQTT=1`, và Gateway) → người dùng nạp + chạy test trên board → cập nhật file này.

## 4. Kế hoạch

**Việc tiếp theo (đề xuất):** (1) chạy lại Deploy MQTT với App `cdc0511` để xác nhận bản sửa (mục 0), rồi hạ log chẩn đoán (quyết định 37); (2) *(R1 bảng kiểu tag XONG, chạy board 21/10/2026)* MB_READ (F.4(b)) theo quy tắc App ở quyết định 40; (3) theo dõi trả lời đội App cho các câu còn chờ ở F.3 (và nhờ họ ghi bảng `0x0080` thật vào Deploy); (4) validate index tag khi nạp rule (4.D); (5) đo lại baseline
Gateway + RAM/FLASH Gateway với MQTT bật + kiểm ranh giới layer bằng `nm`; (6) chờ người dùng quyết: chủ đồng hồ gateway (4.A.3), bản đồ Flash host (4.A.5), có sửa `network_manager.c` không (mục 0).

**A. Nhúng vào `wg_ue` (còn lại)**
1. *(đã xong)* SimplePLC nhúng và chạy trên board gateway.
2. *(cần người dùng)* Đo `lowest free` heap của host DƯỚI TẢI (điều kiện quyết định 26).
3. *(cần người dùng)* Chốt chủ sở hữu đồng hồ (quyết định 23) — cần cho Time Window và `{ts}` trên gateway.
4. **Firmware slave cho board IO:** còn lại: (a) tách khỏi khối `TEST_RS485` của `main.c` thành `rio_slave.{c,h}` (vẫn ngoài file CubeMX
   tự sinh) và gọi từ `main.c` không phụ thuộc cờ test; (b) test lease/SAFE trên board; (c) cân nhắc nanoMODBUS bản riêng thay parser tay;
   (d) phát không chặn nếu cần; (e) kiểm trùng `unit_id` giữa các node.
5. **Bootloader + Flash (đã đọc code, chờ người dùng chốt):** đề xuất thu mỗi slot app 82→80 sector (640 KB) → 3 slot kết thúc ở
   `0x081F0000`; dữ liệu 40 KB `0x081F0000..0x081F9FFF`; 1 sector trống `0x081FA000`; scratch giữ `0x081FC000`. Image app hiện 603 660 B
   < 640 KB. Host phải đổi `flash_define.h` (82→80 ×3), `LENGTH` FLASH của app 656K→640K, build + nạp lại bootloader. **Rủi ro lớn:**
   `bootloader.c` chỉ ghi layout slot khi `magic_number` sai — thiết bị đã có partition table giữ layout cũ, slot Factory cũ phủ đúng
   vùng dữ liệu SimplePLC ⇒ ghi đè lẫn nhau. Board dev phải xoá sector partition `0x0800E000` sau khi nạp bootloader mới; thiết bị
   ngoài thực địa cần cơ chế migrate (CHƯA có). KHÔNG nạp địa chỉ `0x081F0000` cho tới khi người dùng chốt.
6. *(chờ xác nhận)* Kết nối lại Studio (hồ sơ `GatewayRs485EthV2_0`) với gateway; kiểm danh mục tag gateway (112 tag, dense).
7. Đo trên board: độ trễ scan khi task `SPLC` (prio 6, trùng `UART_LOG`, thấp hơn MODBUS/BROKER/SHELL 11 và NETWORK 12) chạy cùng task
   USB; ring phải đủ khung dài nhất (256 B RTU) khi engine ngủ một scan.
8. *(cần người dùng đồng ý sửa file host)* Mạng host (chi tiết mục 0): probe DNS dùng host của override/`pool.ntp.org`, nới ngưỡng probe,
   reset backoff khi cấu hình MQTT đổi lúc offline; kiểm DNS router bằng `nslookup`; cân nhắc tắt ETH/LTE nếu không dùng.

**B. Remote I/O còn mở:** chưa phát hiện trùng `unit_id` giữa hai hàng bảng trên cùng bus; chưa có tag báo "node offline" cho App.

**C. App + spec (không gấp):** App sinh rule "đúng giờ" `EQ, Lo=Hi` (chưa kiểm); Structs mục 4 (124 tag cố định) chỉ đúng cho Remote I/O V1;
Conformance spec cần sửa như mục "Nguồn tài liệu".

**D. Firmware khác:** validate `guard/trigger/action_tag` ∈ `0..MAX_TAGS-1` khi nạp rule (hiện index sai chỉ vô hiệu hoá rule âm thầm);
chưa ghi snapshot retain ngay trước REBOOT; lỗi Flash lúc erase trong bước copy A→B huỷ bản cũ trước khi biết lỗi;
`ACT_WRITE_REMOTE/LOG_EVENT/SEND_ALARM` chưa implement; không có log trước `logger_init`; `utils/filter/*` chưa soát; linker
`STM32H523xx_FLASH.ld` khai `LENGTH=512K` (chip 256 KB — HỎI trước khi sửa).
**Dọn repo:** comment cũ nhắc `modbus_usb`/`sx_usb_cdc`/`FREE_RTOS` còn ở `plc_modbus_cfg.h`, `services/CMakeLists.txt`,
`app/CMakeLists.txt`, `modbus_transport.h`, `splc_board.h`; `port_gw/` là bản nháp cũ (xoá hoặc đánh dấu); làm rõ `splc_config/splcopts.h`
(đang bật MQTT/USB CDC/FreeRTOS dù comment ghi "RS485 variant").

**E. MQTT Publish (xong E.1–E.6, chi tiết `plc_mqtt_design.md`):** option + `splc_mqtt_fmt` + `splc_mqtt_wire.h`; `plc_mqtt` (nháp/COMMIT, định
tuyến `0x0C00..0x0D7F`/`0x0E00..0x0E5F`); đoạn MQTT trong Flash (bit 13) + hook `CLEAR_RULES`/`FACTORY_RESET`; `plc_mqtt_scan` + ống
`splc_mqtt_register()`; port phía `wg_ue` chạy board; `test/test_mqtt.py` (1087 dòng; GHI Flash và chạy `CLEAR_RULES` — chỉ board dev).
Còn nên làm: đo RAM/FLASH Gateway với MQTT bật. **E.7 (đã gửi đội App, chờ phản hồi):** dùng `reserved_header`; token `CustomTemplate`;
quy đổi 10^dec/deadband; thăm dò `0x0E54` + hiển thị `0x0E54..0x0E5F` + đổi nhãn nút Test Connection thành "PC tới broker" (nút chỉ mở TCP
socket từ PC); sửa ICD và Conformance 11.2. Mật khẩu broker lưu chữ thường trong `ProjectMetadata` (phía App).

**F. MB_READ, MB_WRITE, MQTT Subscribe, PULSE_OUT (đã đọc ICD + codec + code firmware liên quan, CHƯA viết dòng code nào)**

*F.0 Phạm vi theo ICD (`paa/docs/firmware/`, `cdc0511`):*

| Phân hệ | Vùng thanh ghi | Cấu trúc |
|---|---|---|
| MQTT SUB (`MQTT-002`) | `0x0D80..0x0DFF` | 2 khối × 64 reg: cờ (enable, QoS, FIFO/Latest, ignore retained, fail-safe action), timeout, topic 32 B, 4 pin (tag đích, kiểu, default, `json_key` 12 B) |
| MB_READ (`MBM-001`) | `0x1000..0x10FF` | 4 khối × 64 reg: slave/FC, địa chỉ, số lượng, chu kỳ, timeout, tag valid/error/errcode, 4 pin (offset, kiểu, endian, multiplier, nhãn) |
| MB_WRITE (`MBM-002`) | `0x1100..0x11FF` | 4 khối × 64 reg (header 16 reg + payload 48 reg): FC 05/06/15/16, chế độ Direct (4 pin) hoặc ChannelIndexed (Sort-to-Light), trigger, tag Done/Busy/Err |
| PULSE_OUT (`PULSE-003` v2.1.0, trước là DIRECT_WRITE) | `0x1200..0x12FF` | 8 khối × 16 reg (32 B): DO0..DO7, mode DO/PWM/PTO/BUS(WS2812), tag kênh/tham số, fail-safe |
| (thêm) TLS cert (`MQTT-003`) | `0x0F00..0x0F43` | chuyển chứng chỉ theo mảnh — chưa xét |
| (thêm) Wi-Fi ST67 (`WIFI-004`) | `0x1300..0x13FF` | cấu hình + quét AP, cờ `0x0A08` bit `0x0200` — ngoài phạm vi, nhưng nằm trong trình tự nạp 6 bước; firmware chưa khai cờ nên chưa xét Deploy khi project có Wi-Fi |

Firmware hiện KHÔNG có gì cho các vùng này (kiểm 20/10 bằng grep `0x0D80/0x1000/0x1100/0x1200`, `PULSE_OUT`, `MB_READ/WRITE`: chỉ `plc_io_remote.c` và `modbus_serial.*` nhắc \"MODBUS_MASTER\", đó là master của Remote I/O); `0x0A08` chưa khai `MQTT_SUB`/`MODBUS_MASTER`/`WIFI`.

*F.1 Tài liệu App (đọc `paa` `cdc0511`, 20/10/2026):*

Đã giải quyết so với bản `c0d4acb`:
1. Header C: MB_READ có `reserved[4]`, đã có `#include <string.h>`; struct MB_READ/MB_WRITE 128 B và PULSE_OUT 32 B đúng kích thước (tính bằng gcc).
2. MB_READ ICD: kiểu dữ liệu pin `1=Bool 2=Int16 3=UInt16 4=Int32 5=UInt32 6=Real`, khớp codec.
3. MB_WRITE ICD viết lại khớp `ModbusMasterCodec.cs` + header: `CONTROL_FLAGS` bit 2..3 = WriteMode (0 Direct, 1 ChannelIndexed); `+2` start/base cmd; `+3` count/channels; `+4..+5` base_param (uint32); `+6..+7` interval; `+8..+9` timeout; `+10` trigger mode; `+11` trig tag; `+12..+15` done/busy/err/err_code tag; payload `+16..+63`: ChannelIndexed = channel/command/param tag ở `+16/+17/+18`, Direct = 4 pin × 12 reg.
4. App đã gửi MB_WRITE và PULSE_OUT: `RuleTableWriter` ghi `0x1100` và `0x1200` (và ghi 0 vào đầu các khối cũ không còn dùng); có `DirectWriteCodec` + test.
5. PULSE_OUT: thanh ghi big-endian rõ ràng (Reg 0 = `(mode<<8)|target_pin`, Reg 2 = `(trigger<<8)|flags`, Reg 6 = `(duty_max<<8)|duty_min`, 32-bit High Word trước); `DirectWriteCodec` khớp từng thanh ghi. Struct C trong ICD khai `target_pin` trước `mode` chỉ là bố cục bộ nhớ; trên dây `mode` ở High byte ⇒ firmware giải mã TỪNG THANH GHI, không ép struct vào byte wire.
6. MQTT SUB: codec ghi đủ fail-safe `0/1/2` (`ResetToZero`).

Còn mở (đọc `27c35d3`, 21/10/2026):
1. Header C nay qua `gcc -std=c11` (struct Wi-Fi 128 B). Đội App nói Studio ghi đủ flags/duty của PULSE_OUT và tag Done/Busy/Error/ErrCode của MB_WRITE (DTO/codec/RuleCompiler có sửa); CHƯA kiểm kỹ.
2. **Bảng kiểu tag `0x0080` mới có trên tài liệu và header; trong `src/` chỉ thấy hằng số, chưa thấy Studio ghi nó khi Deploy.** Thời điểm nạp (nháp/COMMIT, bước nào trong quy trình 6 bước), giá trị 0 (chưa khai) và cờ capability cho REAL chưa được nói.
3. Khối MB_READ có thêm `READ_MODE` (`+12`: 0 tuần hoàn, 1 theo sườn lên) và `TRIG_TAG_INDEX` (`+13`) trong header + codec, nhưng ICD MB_READ chưa mô tả. MB_WRITE ICD thêm enum trigger (0 sườn lên, 1 đổi giá trị ≥ deadband, 2 tuần hoàn) và struct C trong ICD có thể còn lệch codec: đối chiếu lại khi làm MB_WRITE.
4. Tài liệu firmware cũ: `SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md` dòng 103 và test TC-MQTT-07 / TC07 định `0x0D80..0x0DFF` là Reserved → Exception `0x02`. Làm SUB phải sửa các tài liệu và test này.
5. SUB: `target_tag_index` "0..127" nhưng tag dense (`MAX_TAGS`); codec gộp Int16 vào INT32; `0xFFFF` ở `target_tag_index` = pin không dùng (ICD không nhắc). SUB ICD mô tả FreeRTOS queue, `xTaskGetTickCount()`, `jsmn` — không dùng: cần ống `splc_mqtt_input`, tick qua `sx_get_tick_ms()`, bộ tách `json_key` không cấp phát.

*F.2 Rào cản kiến trúc:*
- **SUB:** host phải `subscribe` các topic do lib khai (`wg_ue/apps/mqtt_client` cần thêm hàm — file host, chỉ sửa ở sandbox) và đẩy bản tin vào lib trong ngữ cảnh engine. Cần thêm bit `MQTT_SUB` vào `0x0A08` và dùng cơ chế nháp/COMMIT/Flash như quyết định 32 (có thể cần chỗ trong bản ghi Flash).
- **`modbus_serial` (đọc code 20/10):** chỉ FC03 + FC16; `MODBUS_MASTER_MAX_REGS 16`, `MODBUS_MASTER_FRAME_MAX 64`; mỗi master (`modbus_rtu_master_t`) chỉ có MỘT request đang chạy và `plc_io_remote` đang dùng nó. MB_READ cần thêm FC01/02/04, MB_WRITE cần FC05/06/15, và quyết định 38 đòi 125 reg/request (khung 256 B). Đo thật: +408 B RAM mỗi master (168 → 576 B); RAM tĩnh dư của host 2 992 B nên một master thêm vẫn vừa, hai master (cổng riêng) tốn +816 B (còn ~2 176 B cho `plc_mbm`). Hàm gửi chặn tới khi bit cuối ra UART: khung FC16 123 reg = 255 B ≈ 25 ms ở 115200 ⇒ MB_WRITE nhiều thanh ghi sẽ chặn scan 10 ms, phải giải quyết khi làm MB_WRITE (MB_READ không bị: khung hỏi chỉ 8 B).
- **Cổng/bus của MB_READ, MB_WRITE:** xem quyết định 39. Gateway: USART6 = App (nếu đi RS485), USART3 = bus board IO (`plc_io_remote`), USART10 chưa dùng; board Zigbee-IO chỉ có UART của link App. Chung bus với Remote I/O cần bộ phân xử (nếu không, Remote I/O poll mỗi scan có thể chiếm bus liên tục, MB_READ bị đói) và cùng baud (board IO 115200, cảm biến thường 9600). Hợp đồng \"không chặn trong scan\" (quyết định 12/18) ⇒ máy trạng thái không chặn, có timeout, một request/scan như Remote I/O.
- **Tag:** `g_tag_value[]` là `int32_t`; ngưỡng rule là `int32`; `SPLC_ACT_SCALE_TAG` = `tag*param/1000+threshold_hi` (cắt phần lẻ); MQTT Publish in số lẻ bằng 10^dec. `core/plc_tag` có sẵn kind `TAG_MB_COIL`/`TAG_MB_HOLDING` (`reg_addr`, ghi chú \"future Gateway SKU\") — KHÔNG khớp thiết kế MB_READ (khối khai slave/địa chỉ, 4 pin ghi vào tag thường VFLAG/VREG); đề xuất không dùng cho MB_READ (chưa chốt).
- **PULSE_OUT:** engine đã sở hữu DO qua `output_scan` ⇒ ghi trực tiếp GPIO sẽ tranh chấp với tag DO; PWM/PTO cần timer, BUS cần DMA 800 kHz (nhưng đường tín hiệu OUT0 đang mất xung — mục 0). Trên gateway, DO là DO ảo của board IO qua RS485 nên `DO0..DO7` của ICD không có nghĩa trực tiếp.

*F.3 Câu hỏi còn mở với đội App (tin nhắn 8 câu đã soạn 21/10; trả lời của họ ở changelog 2.9.0):*
Đã được trả lời: thứ tự byte bảng kiểu (High byte = tag chẵn); quy tắc MB_READ; Rule Engine chỉ nhìn cờ float; Publish theo kiểu; `pwm_freq_hz` High Word trước; struct Wi-Fi 128 B.
Còn chờ: (2) bảng kiểu tag nạp ở bước nào và có nháp/COMMIT không; (3) bit capability cho REAL (đề xuất `0x0800`; `0x0400` là Modbus TCP); (4) tag AI kiểu REAL: firmware chỉ đổi mã ADC nguyên sang float tương đương hay có quy đổi; (6) dung sai 1e-5 là tuyệt đối hay tương đối, có áp cho `>`/`<` không; (7) MQTT gặp NaN/Inf; (8) SUB: số lẻ vào tag INT32 làm tròn?; giá trị 0 của bảng kiểu.
Giả định tạm của firmware khi chưa có trả lời (đổi được): giá trị 0 = INT32; dung sai tuyệt đối 1e-5; NaN/Inf ⇒ bỏ qua lần publish; SUB làm tròn như MB_READ; cờ capability REAL tạm `0x0800` (CHƯA gửi lên dây cho đến khi App xác nhận); bảng kiểu đi theo nháp + COMMIT như MQTT và lưu Flash (quyết định 41).
Việc khác: PULSE_OUT phạm vi giai đoạn đầu (đề xuất chỉ mode DO) và quan hệ với DO của engine; thứ tự MQTT SUB và PULSE_OUT sau MB_READ/MB_WRITE.

*F.5 REAL — các bước (quyết định 40; **R1 XONG và chạy board 21/10/2026, quyết định 42**; R2..R5 CHƯA code):*
R1 `plc_tagtype.{c,h}`: bảng 128 B, nháp/validate/COMMIT/discard, `plc_tagtype_get(tag)`, export/import vào bản ghi Flash (bit 11), định tuyến `0x0080..0x00BF` ở `plc_modbus_cfg`, cờ capability, `CLEAR_RULES`/`FACTORY_RESET`; kiểm COMMIT: REAL chỉ cho AI/VREG/VREG_RETAIN/ModbusHolding, kiểu ngoài 1..3 ⇒ từ chối. R4 MB_READ (F.4, cần R1; thêm `READ_MODE`/`TRIG_TAG`). R2 Rule Engine float (so sánh + `SCALE_TAG` float, ghi bit float vào tag; ε theo F.3). R3 Publish: tự viết hàm in tương đương `%.4g` (Layer U, không printf-float của libc; test so với `snprintf` trên PC) và theo kiểu tag. R5 SUB: pin REAL cần bộ tách số thực không dùng libc. Thứ tự: R1 → R4 → R2 → R3 → R5. Test PC dùng `GOLDEN_VECTORS_V2_0.md`. Kiểm tra build có bật FPU của H523/H563 trước khi đo FLASH.

*F.4 Kế hoạch làm:* mỗi phân hệ một file `plc_<tên>.c/.h` (tránh trùng tên host, quyết định 21), bật bằng option riêng trong `splc_opt.h` (mặc định 0 để baseline MQTT=0 không đổi); định tuyến trong `cb_write_multiple_registers`/`cb_read_holding_registers` của `plc_modbus_cfg`; lưu cấu hình qua nháp/COMMIT; thêm bit vào `0x0A08`; test Python `test_<tên>.py`; kiểm ranh giới layer bằng `nm`; build ARM `MQTT=0` (đúng baseline) và `=1`. MB_READ theo thứ tự: (a) **XONG 20/10/2026** — mở rộng `modbus_serial` (FC01/02/04, 125 reg/khung 256 B, timeout gửi tính theo baud) đặt sau option mới `SPLC_MBM_ENABLE` (mặc định 0 ⇒ code và kích thước đúng như cũ); kiểm bằng slave GIẢ trên PC (ASan+UBSan, cả `MBM=0` và `=1`: đọc 125 reg/bit, exception, CRC sai, timeout, sai FC/byte count, giới hạn tham số, FC16 123 reg = khung 255 B; harness không nằm trong repo) + build ARM (Zigbee-IO, Gateway). CHƯA chạy trên board. FC05/06/15 (ghi) để lúc làm MB_WRITE; (b) `plc_mbm.{c,h}` (CHƯA bắt đầu; R1 bảng kiểu tag đã xong, dùng `plc_tagtype_get()`): nháp/COMMIT/Flash (bit 12, quyết định 41), lập lịch 4 khối mỗi scan một request, ghi tag valid/error/errcode theo ICD, `READ_MODE` tuần hoàn/sườn lên + `TRIG_TAG`, nhường App khi dùng chung với App; (c) khai `MODBUS_MASTER` trong `0x0A08`; (d) glue host (`splc_mbm_set_rs485_port`, bộ bơm byte dùng chung khi dùng chung với App); (e) test Python + đo RAM/FLASH.

Ghi nhớ: `0x0C00..0x0C06` trên slave board IO (Remote I/O) trùng SỐ với Telemetry Block 0 của gateway — khác thiết bị/bus nên không xung đột,
nhưng dễ nhầm.

## 5. Bài học (luật rút ra — đừng lặp lại)

1. Đọc file định nghĩa trước khi thêm hằng số/enum/mã lỗi mới.
2. Bằng chứng trước khi kết luận: log là dấu vân tay của phiên bản code; lọc theo từ khoá. **Log thiếu một dòng mà phiên bản mới lẽ ra phải in (vd. `[CAPS]`)
   = bản chạy chưa phải bản mới HOẶC code chưa được gọi tới.**
3. Test chỉ có nghĩa nếu trạng thái đầu xác định (reset board, kiểm `DO0=0`; test persistence phải kiểm dữ liệu sống sót qua reboot TRƯỚC khi xoá).
4. Timeout đếm bằng vòng lặp không phải timeout: đo bằng tick thật.
5. Đừng suy ngữ nghĩa field từ tên hay từ một phía; đọc UI/compiler của App và hỏi người dùng.
6. Flash giả mô phỏng mất điện: sau mất điện mọi erase/program tiếp theo phải bị BỎ QUA.
7. **Việc chưa giao thì mất khi phiên kết thúc:** present nguyên file ngay sau mỗi mốc; người dùng push ngay.
8. Không code hành động phá huỷ dữ liệu khi spec mơ hồ — hỏi phạm vi trước.
9. Đổi giá trị mặc định/cấu hình: kiểm cả bản `-D`/override, giá trị sai (`#error`), thiếu `splcopts.h`.
10. Hiểu kiến trúc trước khi code: hỏi "thiết bị này có cần chạy PLC không?".
11. Đừng tuyên bố môi trường thiếu công cụ khi chưa thử cài.
12. Hợp đồng core↔port: core giữ phần khó (timeout, ghép khung nhiều mảnh); đọc comment lịch sử bug trước khi đề xuất đổi.
13. Test xanh chưa chứng minh gì: phá code có chủ đích (đột biến); với đa luồng, hạ memory order xuống `relaxed` rồi xem ThreadSanitizer có báo race không.
14. Đo trước khi ghi con số vào comment/tài liệu. Khẳng định về host phải nêu đã so cái gì và CHƯA so cái gì.
15. Kế hoạch đổi tên symbol phải dựa trên `nm`, không dựa trên quy ước đặt tên; cần bước kiểm TỰ ĐỘNG sau build.
16. Bỏ một phụ thuộc bắc cầu thì build lại MỌI cấu hình. Test host với harness giả chưa chứng minh gì về CMake.
17. Test chế độ library phải dùng host thật hoặc host giả đủ việc; thử cả ca thiếu (phải FAIL link) lẫn ca đủ.
18. Handoff cũ có thể nói đã xoá file nhưng repo vẫn còn (hoặc ngược lại): kiểm bằng `git status`/`ls` trước khi tin.
19. **Build baseline TRƯỚC khi tin handoff/commit** (một commit đổi tên từng bỏ sót `#include` làm Zigbee-IO không build dù handoff ghi "đã build sạch"; số
    baseline FLASH trong handoff từng lệch 32 B so với đo thật).
20. Khi đo kích thước bằng link giả (`--defsym`), đừng gán symbol mà kernel/driver THẬT cũng định nghĩa.
21. Nhúng vào host thật mới lộ điều mô hình giả không lộ (RAM host gần đầy, thứ tự archive, trùng tên target). Luôn build baseline host TRƯỚC, rồi mới nối.
    Đừng cộng số đo rời — đọc map thật.
22. Đọc code bootloader trước khi tin một đề xuất bản đồ Flash: partition table lưu kích thước slot trong Flash, nên đổi `flash_define.h` mà không migrate
    thì thiết bị cũ chạy layout cũ.
23. Đo bằng ĐÚNG cờ mà người dùng build (`build.bat`), không bằng preset mặc định (`GW_MDNS_ENABLE` khác nhau).
24. So FLASH giữa hai máy phải tính đường dẫn tuyệt đối nhúng trong ELF (~49 chuỗi, 1 ký tự ≈ 49 B). Dùng hiệu số cùng một máy.
25. `build.bat` gốc của `wg_ue` cứng đường dẫn Ninja; chạy nó dù một lần cũng ghi `CMAKE_MAKE_PROGRAM` sai vào cache `build\` → xoá `build\`
    (`build_gw.bat rebuild`). `apps\CMakeLists.txt` và `apps\main\CMakeLists.txt` dễ nhầm: chỉ file thứ hai định nghĩa `apps_main`.
26. **Triệu chứng bus lạ → loại trừ linh kiện trước khi dựng giả thuyết.** Mô phỏng khớp từng byte ≠ nguyên nhân gốc. Đổi linh kiện/dongle/dây, đo A−B và chân
    RO bằng đồng hồ/scope trước khi kết luận.
27. **Pull repo liên quan trước khi kết luận "App chưa có X"**; nêu rõ commit đã đọc.
28. **Tài liệu spec có thể tự mâu thuẫn — chọn bản đúng bằng code codec thật.** Không lấy code mẫu trong tài liệu làm nguồn. **Header C trong tài liệu phải
    `gcc -fsyntax-only` được; `_Static_assert` kích thước là phép thử rẻ nhất.**
29. **Kết nối MQTT chập chờn ≠ lỗi cấu hình SimplePLC.** Đọc log theo thứ tự: `demoting` / `Route event` (lớp mạng host) → `MQTT connect start: ... broker=`
    (broker thực sự được nối) → `generation N != connection M` (đường đổi cấu hình). Dòng `Broker:` trong status in broker của cấu hình riêng, đừng dùng để suy ra
    kết nối. Số `generation` của netmgr và của SimplePLC là hai bộ đếm khác nhau. Khi một giả thuyết không có dòng log đặc trưng thì rút lại, đừng giữ.
30. **Hai broker cùng tên mặc định làm test mù:** muốn chứng minh override được áp dụng, gửi broker KHÁC broker mặc định của host, rồi đọc `broker=` trong `MQTT connect start`.
31. **Default interface method (C#) có thể nuốt tham số âm thầm:** lớp triển khai thiếu một overload thì bị rơi vào default, không có lỗi biên dịch. Khi bản sửa "không có
    tác dụng", lần theo ĐƯỜNG GỌI thật (ai được gán vào biến interface lúc chạy) trước khi nghi code đã sửa; kiểm cả đối tượng được inject lúc có session và lúc không.
32. **Một giả thuyết đã đưa ra phải được kiểm lại khi có dữ liệu mới:** giả thuyết "lần đọc `0x0A08` lúc Deploy lỗi" hoá ra sai về nguyên nhân gốc (code đó chưa từng chạy).
    Nói rõ "chưa chạy, chỉ đọc code" khi kết luận.
33. **Đọc code firmware thật trước khi nói về firmware.** Từng suy "MQTT lưu số nguyên rồi in số lẻ" từ handoff; chỉ khi đọc `plc_tag.h` và `plc_mqtt.c` mới xác nhận được.
34. **Đừng kết luận "file này không đổi" từ `git diff --stat | grep`:** đường dẫn dài bị cắt thành `...` và không khớp. Hỏi thẳng `git diff --stat <a> <b> -- <file>`. Changelog của App cũng chỉ là lời khai: đối chiếu bằng diff và codec thật.
35. **Slave giả trong test PC phải trả lời SAU khi `write()` trả về:** `send_frame` xả RX ngay sau khi gửi (bỏ echo của transceiver), nên slave giả trả lời tức thì sẽ bị xả mất và trông y như lỗi của thư viện. Cho slave giả một độ trễ vài ms.
36. **Test board đừng giả định bố cục tag ngoài những gì đọc được từ `0x0020`:** `runtime_tag_count` < `MAX_TAGS` thì ô thừa vẫn tồn tại (kind NONE) — test bước "REAL ngoài số tag" từng giả định bị bỏ qua, thực tế bị từ chối. Test phải chấp nhận mọi kết quả đúng của firmware và in ra nhánh đã xảy ra; so với bảng mô hình (`exp`), đừng so với hằng (`{}`) khi một bước phía trước có thể bị bỏ qua.
37. **Thiết bị giả trong test PC chỉ chứng minh logic script:** chạy script trên mô hình rồi phá mô hình có chủ đích (bỏ kiểm tra COMMIT, bỏ reset giá trị) để biết test thật sự bắt được lỗi; kết quả cuối cùng vẫn là chạy trên board.

## 6. Verify & test

**Script test trên board** (`python -m pip install pymodbus pyserial`; `test_mqtt.py` cần thêm `paho-mqtt` cho `--broker`). Các script ghi Flash/xoá rule — chỉ chạy trên board dev:

| Script | Kiểm cái gì |
|---|---|
| `test_plc.py` | bản hiện tại là probe bring-up RS485 (tham số `--period/--baud/--unit`) |
| `test_diag.py [manual \| manual-dwell --expire] COM14` | state machine diag, lease, engine dừng thật, reset runtime khi thoát diag |
| `test_tag.py COM14 [--pins --commit --reboot]` | ghi tag trong diag, all-or-nothing, retain draft/COMMIT/DISCARD |
| `test_rtc.py COM14 [--reboot]` | khối RTC, từ chối ghi sai, tốc độ đồng hồ, Time Window |
| `test_fb.py COM14 [--reboot] [--probe-64]` | khối FB (nháp/COMMIT, từ chối, CV/Q, persistence); `--probe-64`: FC16 73 byte |
| `test_retain.py COM14 [--commits N] [--periodic]` | retain qua REBOOT, xoay vòng sector, CLEAR_RETAIN; `--periodic` ~6 phút |
| `test_sysclear.py COM14 [--reboot]` | 3 lệnh xoá, persistence, lệnh trong diag, lệnh sai |
| `test_mqtt.py COM35 --unit 1 --baudrate 115200 [--broker]` | khối Telemetry/Broker/status `0x0C00..0x0E5F`, nháp/COMMIT, CLEAR_RULES xoá MQTT; `--broker` chuyển gateway sang broker test rồi trả lại |
| `test_tagtype.py COM17 --unit 1 --baudrate 115200 [--reboot]` | bảng kiểu tag `0x0080..0x00BF`: nháp/COMMIT, từ chối cả bộ, đổi kiểu reset giá trị, persistence + 3 lệnh xoá (`--reboot`); cần `SPLC_REAL_ENABLE=1`; GHI Flash, chạy `CLEAR_RULES` — chỉ board dev; import helper của `test_mqtt.py` |

Trước `test_diag.py manual`: reset board, xác nhận `DO0=0`. Một số script từng dùng helper của `test_plc.py` bản cũ — kiểm trước khi chạy.
**Kiểm tay cho lỗi Deploy MQTT (không cần App):** FC03 đọc `0x0A08` quantity 2 (mong đợi `0x0000 0x0011` hoặc `0x0013`); FC16 ghi thử `0x0E00` rồi COMMIT; đọc `0x0E54`.

**Kiểm cấu hình (build ARM) mỗi khi đụng CMake/option:** mặc định (Zigbee-IO, nhớ `splc_config/splcopts.h` đang bật MQTT); `SPLC_BOARD_GATEWAY` qua
`-DSPLC_OPTS_DIR`; `SPLC_FLASH_SIZE_KB` 128/256/512/… (sai phải `#error`/`_Static_assert`); `SX_OS_USE_FREERTOS=1` (cần `-DSPLC_OS_INCLUDE_DIRS` hoặc
stm32cubemx); `MAX_RULES=50`; `SPLC_LOGGER_BUILTIN=0` (cần logger host); `SPLC_RTC_BUILTIN=0`/`SPLC_ADC_BUILTIN=0` (standalone phải FAIL link thiếu
`sx_rtc_*`/`sx_adc_read`; có `splc_port.c` thì link qua); `SPLC_AS_LIBRARY=ON`; thiếu `splcopts.h` phải `FATAL_ERROR` lúc configure;
`SPLC_MQTT_ENABLE=0/1` (`=0` phải ra ĐÚNG 80 296 / 77 664 B). So FLASH/RAM với số đo chuẩn ở mục 0.

**Ranh giới layer** (`nm -u <file>.o`): Layer 3 không có symbol `sx_usb_*`; Layer 2 không có `sx_flash_*`/`sx_usb_*`; `plc_rule_flash.c` PHẢI có `sx_flash_*`;
`sx_system_reset` chỉ được gọi từ `plc_system_cmd_service.c`; chỉ `port/usb/splc_usb_cdc.c` được `#include "tusb.h"`; `modbus_link.c` không có
`sx_usb_*`/`tud_*`; không file nào ngoài `libs/splc_nanomodbus/` được `#include "nanomodbus.h"`; `nm` trên `libsplc_nanomodbus.a` không có symbol toàn cục
thiếu tiền tố `splc_`; `nm` trên binary không có `cqueue_*`/`board_init`/`nmbs_*` không tiền tố; `plc_mqtt` không `#include` nanomodbus/tusb.

**Kiểm trên PC (không cần board, mô hình giả — nói rõ khi báo cáo):** Layer 2 (`plc_tag.c`, `plc_rule.c`, `plc_internal_rule/*.c`) biên dịch bằng `gcc -std=c11` + stub,
`sizeof(SPLC_RuleRecord)==32`; `modbus_link` (đồng hồ giả + 2 thread, ThreadSanitizer); `plc_io_remote.c` + `modbus_serial.c` với slave RS485 giả; `splc_mqtt_fmt`,
`plc_mqtt` (Flash/đồng hồ/callback giả). Harness KHÔNG nằm trong repo; muốn giữ thì đặt vào `test/` kèm target CMake PC — quyết định của người dùng.