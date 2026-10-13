# SimplePLC — Handoff

> Cho phiên làm việc sau nắm "đang ở đâu, làm gì tiếp, luật gì phải giữ". Đọc SAU
> `Readme.md` và `docs/architecture.md`. File này chỉ giữ: trạng thái hiện tại, quyết định
> đã chốt, kế hoạch, quy trình, bài học. Lịch sử từng bước đã bỏ — xem `git log`.
>
> **Quy tắc vàng:** trước khi tin điều gì dưới đây, chạy `git pull && git log --oneline -10`.
> Code lệch file này thì tin code và sửa file này.

**Nguồn tài liệu** (V2.0 là strict superset của V1.9). `SimplePLC_Wire_Contract_V2_Draft.md` ĐÃ BỊ XOÁ khỏi repo
(commit `bc777ae`); nội dung hành vi nay nằm trong Structs và Conformance. Mâu thuẫn giữa các tài liệu → tin code
thật của App (`paa`) và firmware, rồi sửa tài liệu:
1. `docs/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md` — struct/memory map (nguồn chính).
2. `docs/SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md` — khối MQTT (Telemetry `0x0C00`, Broker `0x0E00`);
   layout đúng = Structs 3.9/3.10 + `docs/firmware/splc_mqtt_wire.h` + golden vector (quyết định 33).
3. `docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md` — checklist tuân thủ (chưa có mục MQTT; mục 11.2 ghi nhầm
   FC16 là `0x16`, đúng là `0x10`; còn giả định 124 tag cố định của Remote I/O).
4. `docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md` — **layout khối Telemetry SAI** so với Header/Structs và code
   mẫu `memcpy` thẳng vào struct (sai thứ tự byte, không kiểm biên). KHÔNG dùng làm nguồn; chờ sửa (quyết định 33).
5. `docs/SimplePLC_RuleStruct_MCU_Spec_v0.1.md` — chỉ còn đúng cho LOGIC rule (Trigger→Compare→Dwell→Guard→Action);
   struct/CRC/Time Window/retain đã bị ghi đè.
Các tài liệu MQTT dẫn link `g:/HoaNV/...` (máy của đội App) — không mở được; file thật nằm trong repo `paa`.

**Repo liên quan**
- App: `ngoxuanloc2309/paa` (.NET, SimplePLC.Studio; bản đã đọc: `main` `8056a62`). Bug xuyên App+firmware phải
  đọc CẢ HAI repo (`RuleCompiler.cs`, `RuleTableWriter.cs`, `FunctionBlockGateway.cs`, `MqttTelemetryCodec.cs`,
  `MqttBrokerConfigCodec.cs`). Luôn `git pull` App trước khi kết luận "App chưa có X" (bài học 27).
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
`splc_port.c` → link qua. Đã link với `wg_ue` thật ở bản clone sandbox (xem ngay dưới); chưa chạy
trên board.

**Đã nối vào `wg_ue` (bản clone sandbox, `main` = `2684df5`) và LINK ĐƯỢC** (ARM Release, gcc 13.2.1,
`SPLC_AS_LIBRARY=ON`, `splcopts.h` của `port_gw/`, `GW_USB_NETIF_ENABLE` mặc định OFF). Không có
undefined, không multiple definition, không cảnh báo của `splc_port.c` (`-Wall -Wextra -Wshadow`).
Số đo `wg_ue` (chuẩn để bắt hồi quy; baseline vá tối thiểu 2 include trong sandbox, xem quyết định 25):

| | FLASH | RAM tĩnh |
|---|---|---|
| `wg_ue` baseline (chưa có SimplePLC) | 576 876 B (85,9% của slot 656 KB) | 654 432 B (**99,86%** của 640 KB − 16 B) |
| `wg_ue` + SimplePLC | 603 660 B (**+26 784 B**; 92,1% của 640 KB) | **+22 472 B** (~23,5 KB bss: services 12 KB, core 5,8 KB, app 3,9 KB) |

**RAM host gần đầy:** baseline chỉ còn ~912 B trống vì `ucHeap` = 320 KB tĩnh. Thêm SimplePLC → link
FAIL (`.bss` vượt 21 560 B). Bắt buộc giảm `configTOTAL_HEAP_SIZE` (hoặc nơi khác) ≥ ~22 KB, cộng task
`SPLC` 6 KB lấy từ chính heap đó. Thử với 296 KB thì link qua (RAM 99,54%). Mức heap THẬT còn dùng của
host chưa đo (cần `xPortGetMinimumEverFreeHeapSize()` trên board) nên 296 KB chỉ là con số thử,
CHƯA phải đề xuất. `FreeRTOSConfig.h` do CubeMX sinh → hỏi người dùng (quyết định 9).

**Đã xác nhận trên máy người dùng (Windows, STM32CubeCLT GCC 14.3.1, cờ của `build.bat`:
`GW_MDNS_ENABLE=ON`, `ST67W611_ARCH=T02`, `GW_USB_NETIF_ENABLE=OFF`):** `wg_ue` chưa nối SimplePLC
RAM 654 464 B / FLASH 570 768 B; nối SimplePLC (heap 320 KB) → link FAIL, `.bss` vượt 21 584 B,
RAM 676 928 B, FLASH 597 548 B ⇒ SimplePLC thêm **+22 464 B RAM, +26 780 B FLASH** (khớp số đo sandbox
GCC 13.2.1 trong 24 B). Với heap 296 KB (giá trị người dùng đang dùng, quyết định 26), build của người dùng
(GCC 14.3.1): RAM 652 352 B (99,54%, dư **2 992 B**), FLASH 597 548 B (88,95%) — link qua. Thử 288 KB trong sandbox
(GCC 13.2.1): RAM 644 160 B (98,29%, dư 11 184 B). **Chưa chạy trên board.** Số FLASH giữa các máy
lệch nhau vài chục–trăm byte vì ELF nhúng ~49 chuỗi đường dẫn build tuyệt đối (bài học 24); so RAM
và hiệu số, đừng so từng byte FLASH giữa hai máy. Preset `Release` mặc định của `wg_ue` có
`GW_MDNS_ENABLE=OFF`, khác `build.bat` → luôn đo bằng cờ của `build.bat`.

**Đã chạy trên board gateway (log người dùng, `GW_USB_NETIF_ENABLE=OFF`, heap 296 KB):** SimplePLC khởi động
(`PLC_RULE_FLASH` bảng rule rỗng, `BOARD_GATEWAY: RS485 master ready (115200 baud)`); `PLC_IO_REMOTE` báo node 0
(unit 1) offline vì chưa có firmware slave. CDC chưa kiểm được vì USB tắt. Sau đó bật USB netif: TinyUSB init OK, `USB netif ready`, nhưng PC báo Unknown USB Device (quyết định 28).

**Đã chạy trên board gateway (phiên bring-up RS485, người dùng chạy):** App link RS485 qua USART6 (quyết định 30) —
`python test_plc.py COM35 --period 1000 --baud 115200 --unit 1` → **6/6 phản hồi hợp lệ**, mỗi phản hồi 25 B
`01 03 14 00 03 00 01 00 01 00 00 00 ...` (thanh ghi đầu `0x0003` = `device_class` GATEWAY). Mới 6 mẫu, chưa soak.
Chỉ chạy được sau khi **thay IC MAX485 mới** (chi tiết và triệu chứng ở cuối quyết định 30). Sau đó bản Studio cũ (`254ae0a`) nối được
tới tầng đọc descriptor nhưng từ chối class 3 (`Device class 0x0003 is not supported`). App `8056a62` đã thêm hồ sơ
`GatewayRs485EthV2_0` (`SupportedDeviceProfile.cs`) — **chưa kết nối lại để xác nhận** (mục 4.A bước 9).

**Chưa chạy trên board:** đường RS485 của Zigbee-IO (quyết định 29; `test_plc → test_fb --probe-64 → test_retain` qua dongle USB-RS485, `python test_plc.py COMx`, baud 115200, unit 1).

**Chưa làm:** bản đồ Flash host (bước 5), firmware slave cho board IO. Khoá Flash chung với OTA: hoãn (quyết định 27). Chi tiết ở mục 4.

**Chưa verify (không chặn việc tiếp):** chạy lại `test_plc/test_fb/test_retain` với firmware
sau các thay đổi đổi-tên/config/OS/Flash/logger; Remote I/O với board IO thật (timing RS485,
nhả chân DE); FreeRTOS kernel thật (độ trễ task lúc erase Flash); PVD thật; mất điện giữa lúc
ghi Flash; `test_rtc.py --reboot`; nanoMODBUS riêng cùng tồn tại với bản của `wg_ue` thật
(mới thử bằng bản sao giả).

**MQTT — mới ở giai đoạn THIẾT KẾ, chưa có dòng code nào:** hướng đã chốt ở quyết định 31–34; việc tiếp theo và các điểm còn mở ở mục 4.E.

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

1. `protocol_version = 2` ("Protocol Version: 1" trong Wire Contract cũ — đã xoá — là lỗi đánh máy).
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
10. `0x0A20` đọc trả lệnh cuối đã ghi (khớp GV-005) dù Wire Contract cũ (đã xoá) ghi WO. Layout tag board
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

25. **Cách nhúng vào `wg_ue` (đã link được ở sandbox):** thư viện nằm ở `libs/simple_plc/` (submodule/bản
    chép của repo này; CMake thư viện là `libs/simple_plc/simple_plc/`), port ở `ports/splc_port/`
    (`CMakeLists.txt`, `splc_port.c/.h`, `splcopts.h`). Quy tắc rút ra khi link thật:
    (a) target của host tên **`ports_splc_port`** (không phải `splc_port`: SimplePLC đã dùng tên đó cho Layer
    3.5); (b) `ports/splc_port/CMakeLists.txt` `set(SPLC_AS_LIBRARY ON ... FORCE)` và
    `set(SPLC_OPTS_DIR <thư mục port> ... FORCE)` TRƯỚC `add_subdirectory`; (c) `splc_platform_freertos`
    phải gắn vào **executable** của host (`target_sources(${CMAKE_PROJECT_NAME} PRIVATE
    $<TARGET_OBJECTS:splc_platform_freertos>)`), KHÔNG gắn vào một static library — đặt trong
    `ports_splc_port` thì link FAIL `undefined sx_os_yield_wait` vì thứ tự archive; (d) `logger.h` của host
    được SimplePLC thấy nhờ `include_directories(libs/logger ...)` ở scope gốc của host (thư mục con kế thừa),
    còn `stm32cubemx` cấp header FreeRTOS nên KHÔNG cần `SPLC_OS_INCLUDE_DIRS`; (e) file host phải sửa
    (đã gói trong `wg_ue_changes/`): `CMakeLists.txt` gốc (+`add_subdirectory(ports/splc_port)` và
    +`ports_splc_port` trong danh sách link), `apps/main/CMakeLists.txt` (`apps_main` link
    `ports_splc_port` để thấy `splc_port.h`), `apps/main/gateway.c` (`#include "splc_port.h"` và
    `(void)splc_host_start();` ngay sau `modbus_master_init();`). Không sửa `usb_netif.c`,
    `rs485_driver.*`, `flash_port.c`, file CubeMX.
    **Baseline `wg_ue` trên Linux cần workaround KHÔNG thuộc dự án này** (người dùng xác nhận bỏ qua):
    `Drivers/` vs `drivers/`, `backend_T02.c` vs `backend_t02.c`, và thiếu `#include "semphr.h"`
    (`network/eth_netif/eth_driver.c`) / `"queue.h"` (`apps/shell/shell_serial.c`). Chỉ vá ở sandbox.

26. **Chỗ lấy RAM cho SimplePLC trên `wg_ue` (người dùng chọn, phiên này):** hạ `configTOTAL_HEAP_SIZE`
    trong `Core/Inc/FreeRTOSConfig.h` dòng 74, **sửa tay** (người dùng chấp nhận file CubeMX sinh bị sửa;
    `GatewayV2.ioc` KHÔNG lưu kích thước heap, chỉ lưu kiểu `Heap_4`, nên sinh lại từ CubeMX sẽ hoàn về
    giá trị khác và mất thay đổi). Giá trị **người dùng đang dùng: 296 KB** (hạ 24 576 B; phiên này đã đề xuất 288 KB, người dùng chọn
    296 KB): đủ cho +22 464 B tĩnh nhưng chỉ dư **2 992 B** RAM tĩnh — bất kỳ buffer tĩnh mới nào của
    host/SimplePLC (> ~3 KB) sẽ làm link FAIL lại. Heap còn khoảng 290 KB cho host sau khi trừ stack task
    `SPLC` 6 KB (ít hơn trước 30 KB). **Điều kiện chưa kiểm:** heap trống tối thiểu thật của host
    (`lowest free` trong log `MONITOR`, `Core/Src/app_freertos.c`) phải ≥ ~30 KB (tốt nhất ~38 KB) ở bản
    chưa nối SimplePLC, chạy đủ tải; người dùng cần đọc log đó trên board. Bản giao `Core/Inc/FreeRTOSConfig.h`
    có 288 KB — thay bằng giá trị người dùng đã tự chỉnh (296 KB). Khi CubeMX sinh lại phải làm lại.

27. **OTA của `wg_ue` KHÔNG dùng (người dùng xác nhận, phiên này) → không làm khoá Flash chung (A' / 4b).**
    Người dùng không dùng OTA và chấp nhận ghi nhận ở đây, không sửa code host. Hệ quả: vì
    `ports/flash_port` chỉ có `apps/ota` dùng (đã kiểm bằng grep), nên khi OTA không chạy thì không còn
    bên nào ghi bank 2 song song với `plc_rule_flash`/`plc_retain`. `ota_init()` + task `OTA_HEALTH`
    (`ota_mark_healthy()` sau 15 s) VẪN còn trong `gateway.c` và vẫn chạy; chỉ cần không ai kích hoạt
    tải OTA. **Chưa kiểm:** `ota_mark_healthy()` có phải bước xác nhận image cho bootloader (rollback nếu
    thiếu) hay không — vì vậy KHÔNG xoá `ota_init()`/`OTA_HEALTH` khi chưa đọc `ota.c` + `bootloader.c`.
    **Nếu sau này dùng OTA lại thì phải thiết kế khoá Flash (SPLC_FLASH_LOCK/UNLOCK hoặc tương đương;
    lưu ý PVD ghi retain trong ISR, không được chờ mutex).** Bản đồ Flash (4c) vẫn chờ người dùng.

28. **Chế độ USB CDC-only cho gateway (người dùng yêu cầu, nhánh `usb_cdc_gw` của `wg_ue`; thay thế một phần
    quyết định 20).** Lý do: với `GW_USB_NETIF_ENABLE=ON` PC báo "Unknown USB Device"; log chẩn đoán cho thấy
    MCU nhận RESET (`last_istr=0x400`, `irq=1845`) nhưng `daddr=0x80` (không qua SET_ADDRESS); đã tắt SimplePLC
    (comment `splc_host_start()`) vẫn lỗi ⇒ lỗi KHÔNG do SimplePLC. Đã loại trừ qua đọc code: ưu tiên ngắt USB 5
    (hợp lệ), trim CRS 32 (= mặc định HAL), ISR có `tud_int_handler`. Nguyên nhân CHƯA biết.
    Thiết kế: cờ CMake `GW_USB_CDC_ENABLE` (loại trừ nhau với `GW_USB_NETIF_ENABLE`, CMake FATAL_ERROR nếu bật
    cả hai). Dùng TinyUSB của host (KHÔNG dùng `port/usb` của SimplePLC: TinyUSB chỉ một chủ, quyết định 15).
    File: `ports/usb_port/Target/usb_descriptors_cdc.c` (một cấu hình, một ACM, PID 0x4001 khác 0x4021 để PC không
    dùng lại trạng thái lỗi đã cache), `tusb_config.h` (CDC=1, ECM_RNDIS=0, NCM=0), `libs/CMakeLists.txt` (chọn bộ
    descriptor, bỏ `lib/networking`), `ports/splc_port/gw_usb_cdc.[ch]` (task `USB_CDC` prio 5, stack 1024 word:
    `MX_USB_TinyUSB_Init()` → `tusb_init()` → `g_usb_tusb_initialized=1` → `tud_task_ext`; log "enumerated",
    "COM port opened", và diag mỗi 5 s khi chưa enumerate), `splc_port.c` (`splc_host_start()` gọi
    `gw_usb_cdc_start()` khi `GW_USB_CDC_ENABLE`), `network/usb_netif/usb_netif.c` (ở chế độ CDC-only chỉ còn stub
    `usb_netif_init/usb_netif_is_available`, vì `tud_mount_cb` cũ gọi `tcpip_callback` trên netif chưa khởi tạo và
    `wifi_driver.c`/`http_server.c`/`gateway.c` include `usb_netif.h` vô điều kiện). `gateway.c` KHÔNG sửa thêm.
    Số đo sandbox (ARM Release, GCC 13.2.1, cờ `build.bat` + `GW_USB_CDC_ENABLE=ON`, `NETIF=OFF`): FLASH 607 264 B,
    RAM 648 080 B (98,89%, dư 7 264 B); cùng máy, `NETIF=ON`: FLASH 614 412 B, RAM 653 528 B ⇒ CDC-only tiết kiệm
    ~5,4 KB RAM, ~7,1 KB FLASH. (Số FLASH giữa sandbox và máy người dùng lệch nhau — bài học 24; so hiệu số.)
    **Chưa chạy trên board.** Ý nghĩa phép thử: nếu CDC-only enumerate được ⇒ lỗi nằm ở descriptor composite
    RNDIS/ECM; nếu vẫn Unknown Device ⇒ lỗi ở tầng chung (clock/EP0/ISR/phần cứng), không phải descriptor.

29. **Zigbee-IO nói chuyện với App qua RS485 (người dùng chọn, phiên này) — làm ở phía user, KHÔNG thêm module port.**
    Lõi không đổi: `modbus_link` đã có đúng hai điểm nối (`output` và `splc_link_input`). Hai hàm
    `rs485_output`/`rs485_pump` nằm thẳng trong `board_zigbee_io.c`; `SPLC_PORT_USB_CDC` chọn USB (1) hay
    RS485 (0) cho board này (không thêm cờ mới). Cấu hình RS485 = thư mục `splc_config_rs485/` + preset
    `Release-RS485`. USART1, chân DE = PB13 (GPIO thường, nhãn CubeMX `USART1_DE`), baud/parity lấy theo
    CubeMX (115200 8N1), unit = `SPLC_MODBUS_UNIT_ID`. Cực DE: `RS485_DE_ACTIVE_HIGH` trong `board_zigbee_io.h`.
    Phát CHẶN (`HAL_UART_Transmit`, ~4 ms/khung 41 B, ~22 ms/khung 256 B) — đủ cho test, lệch hợp đồng
    "output không chặn" của `modbus_link`. Bỏ echo bằng cách tắt bộ nhận (`sx_uart_set_rx_enabled`) quanh lúc
    phát, không so khớp byte. Hai sửa Layer 0 (`stm32h5_uart.c`, dùng chung mọi nơi nhận byte qua `sx_uart`):
    (a) `sx_uart_read/available` khoá ngắt quanh `splc_cqueue_*` vì ISR và engine cùng sửa `count`;
    (b) `HAL_UART_ErrorCallback` nhận lại RX vì HAL huỷ nhận sau overrun (đã đọc `HAL_UART_IRQHandler`).
    Build ARM: Zigbee-IO RS485 FLASH 70 860 B / RAM 77 048 B; Zigbee-IO USB FLASH 80 248 B / RAM 77 664 B
    (+48 B so với số chuẩn 80 200 B, do khoá ngắt); Gateway chuẩn 55 356 / 72 624 giống từng byte.
    **Chưa chạy trên board.**

30. **Gateway nói chuyện với App qua RS485 (người dùng yêu cầu, phiên này) — làm ở glue của HOST, SimplePLC KHÔNG đổi.**
    Gateway không sở hữu ngoại vi (quyết định 12/18/19), nên App link RS485 là hai hàm trong `wg_ue/ports/splc_port/`
    (bản chuẩn; `port_gw/` ở repo này là bản nháp cũ, không còn đồng bộ): `app_output()` = `rs485_send()` (driver host tự
    điều khiển DE, đợi TC), `app_process()` = `rs485_recv_polling(...,0)` rồi `splc_link_input()`. Cổng RS485 THỨ HAI, khác
    cổng bus board IO: option `SPLC_HOST_APP_RS485_PORT` (0/1/2 = USART3/6/10; âm = USB CDC, mặc định) + `_BAUD/_PARITY`
    trong `splcopts.h`; nhận cổng với owner `RS485_OWNER_MODBUS_GATEWAY` (owner tính theo cổng, nên dùng chung id với cổng
    IO được), giữ cấu hình bằng `splc_check_port()` đã khái quát hoá theo từng cổng (mỗi 1 s, như cổng IO). `#error` khi: cổng
    App trùng cổng IO, hoặc `GW_USB_CDC_ENABLE=ON` cùng RS485 (phải build `-DGW_USB_CDC_ENABLE=OFF`; `build_gw.bat` hiện
    đặt ON nên cần tắt). Không thêm UART/ngắt/CubeMX nào: ngắt RX, stream buffer, DE, lỗi UART đều của `rs485_driver`; hai
    sửa `sx_uart` của quyết định 29 KHÔNG liên quan đường này (host dùng driver riêng). Phát CHẶN `rs485_send` (~3,6 ms/41 B,
    ~22 ms/250 B ở 115200), engine không scan lúc đó và cũng trễ poll bus IO tương ứng. MAX485 chung DE//RE nên không có echo.
    Ràng buộc host: cổng App không được cấp cho `modbus_master`/`serial_tcp_server` (web/shell); lỗi sẽ log `owned by another
    host service`. Build `wg_ue` ARM (sandbox, đã vá lỗi hoa/thường Linux như quyết định 25, NETIF=OFF CDC=OFF): FLASH
    605 124 B / RAM 651 368 B; so bản USB CDC-only 607 672 / 647 752: RAM +3,6 KB do host vẫn link lớp USB network mặc định
    khi tắt CDC-only (`_netd_epbuf` 3 344 B), KHÔNG do glue RS485 (+~216 B code `splc_check_port`). **Chưa chạy trên board**;
    người dùng chọn USART6 (index 1) làm cổng App cho test nạp rule, nên bus board IO được DỜI sang USART3 (index 0) trong `splcopts.h`
    (`SPLC_HOST_RS485_PORT 0`; board IO thật phải nối lại đầu USART3, hiện chưa có firmware slave nên node vẫn offline). Lưu ý nhãn:
    tài liệu phần cứng `wg_ue` gọi USART6 là "RS485 Port 2" (USART3 = Port 1), còn người dùng gọi USART6 là "rs485 1": đã theo tên UART.
    **Schematic khối RS485 của gateway (người dùng cung cấp, 3 cổng giống nhau):** `/RE` và `DE` của MAX485 nối chung một chân
    GPIO (`RE/DE`) ⇒ DE **tích cực cao** (cao = phát, thấp = nhận), khớp `rs485_set_de()` của host và `MX_GPIO_Init` (khởi tạo
    thấp); thu bị tắt khi phát ⇒ không có echo. Vậy cực DE đã được xác nhận (dòng "To verify" ở `wg_ue/Documents/Hardware/
    02_PINOUT_AND_INTERFACES.md` có thể đóng). VCC của MAX485 lấy từ 3V3 qua R25 (R26 sang +5 là phương án khác, không lắp): nếu
    linh kiện lắp thật đúng là MAX485 5V thì chạy 3,3 V là ngoài dải datasheet (4,75–5,25 V) — cần kiểm BOM xem là MAX3485/SP3485.
    Hai điểm phần cứng ảnh hưởng độ tin cậy, chưa đo, KHÔNG chặn việc nạp rule: (a) phân cực bus rảnh là R40 10K lên +5 và R28 10K
    xuống GND qua R34 120R ⇒ chênh lệch A−B lúc rảnh chỉ ~30 mV (còn ~15 mV khi đầu xa cũng có 120R), thấp hơn ngưỡng ±200 mV
    của MAX485, nên bus rảnh không được đảm bảo ở mức mark: có thể sinh byte rác (counter `rx_bytes` tăng khi không ai gửi) làm
    khung đầu tiên sau lúc rảnh bị hỏng; (b) R17 10K kéo `RE/DE` lên 3V3 ⇒ lúc reset / trước khi `MX_GPIO_Init` chạy, MAX485 đang ở
    chế độ PHÁT và giữ bus ở mức mark. Về nhãn: đầu ra `RS4853_P` đi qua cuộn chặn tới chân B (7) của MAX485 và `RS4853_N` tới chân
    A (6), tức tên ngoài đảo so với tên trong `RS48511_P/N`; nhãn A/B của dongle tuỳ hãng, nếu không có phản hồi thì đảo hai dây.
    **Bring-up trên board (phiên này; dongle USB-RS485 ở COM35, cổng USART6, 115200 8N1).** Kết quả cuối: xem mục 0 (6/6 OK).
    Triệu chứng lần lượt, theo log người dùng (nguyên nhân nêu ở dưới là SUY LUẬN, chưa đo bằng scope):
    (1) dây như ban đầu: khung `01 03 00 00 00 0A C5 CD` vào MCU thành `7F 7E FF FF EB 75 19` (2 byte cuối lỗi framing),
    `<CR><LF>` thành `79 3D`; `hello` từ gateway ra PC thành rác. Mô phỏng UART 8N1 với đường bị đảo mức khớp từng byte phần
    nhận của MCU. (2) đảo A/B: `hello` gateway → PC ra ĐÚNG, nhưng PC → gateway gần như không vào: chân RX (PC7) bị kéo
    thấp từng ~1 s, mỗi lần chỉ ra 1 byte `00` lỗi framing (break), khung Modbus mất hết; bơm `UUUU` từ Hercules thì ra hàng trăm
    byte lỗi framing. (3) **thay IC MAX485 mới → hết**: khung vào đúng `Rx[8]=01 03 00 00 00 0A C5 CD`, bộ ghi sóng ra các
    đoạn mức là bội của ~8,7 µs, rồi `test_plc` 6/6. Quy kết IC cũ lỗi vì thay xong hết triệu chứng; mô phỏng "đảo cực" khớp
    byte nhưng KHÔNG chứng minh được nguyên nhân (bài học 26). **Hướng đấu A/B của dongle sau khi thay IC: CHƯA ghi nhận**
    (người dùng chưa xác nhận dây đang đảo hay nguyên bản) — ghi lại khi biết.
    **Công cụ diag** (`wg_ue/ports/splc_port/gw_rs485_diag.c`, người dùng đã chép đè và chạy; mình chỉ kiểm cú pháp bằng header
    giả, chưa build ARM): `gw_rs485_diag_poll()` log dòng `RS485_DIAG` (số đếm + 16 byte đầu của khung cuối); ở chế độ drain
    (`GW_RS485_DIAG_DRAIN`) nó nuốt hết byte nên SimplePLC KHÔNG nhận được gì → phải tắt khi test thật. Phiên này thêm:
    `GW_RS485_DIAG_TX_TEST` (gửi `hello\r\n` mỗi `GW_RS485_DIAG_TX_PERIOD_MS`=2000 qua `rs485_send`, log kết quả), đọc mức chân
    RX `PC7` (`GW_RS485_DIAG_RXPIN_PORT/PIN`; số `edges` lấy mẫu ~1 ms nên VÔ NGHĨA với 115200 baud, chỉ xem `lo/hi`), và
    `GW_RS485_DIAG_CAPTURE` (bộ ghi sóng phần mềm: chờ tối đa 1,5 s cạnh xuống đầu tiên trên PC7, tắt ngắt 2 ms, in độ dài các
    đoạn mức theo µs bằng DWT; **CHẶN task SPLC** trong lúc chờ). **Cả hai cờ mặc định = 1 → đặt 0 (hoặc gỡ) trước khi commit/chạy
    thật.** Còn mở nhỏ: với IC mới, sau mỗi lần phát `hello` vẫn có `rx_bytes +1` kèm `hw_err +1` (nghi glitch lúc DE nhả, liên quan
    bus rảnh yếu R40/R28 ở trên); chưa điều tra, không ảnh hưởng khung Modbus nếu nằm ngoài khung.

31. **Khối MQTT: SimplePLC chỉ làm LOGIC và GIỮ DỮ LIỆU, không có client mạng (người dùng chốt).** Không TCP/TLS/MQTT client trong lib; truyền mạng do user cắm vào qua ống kiểu `netif` (cùng triết lý `modbus_link`, quyết định 13/16): dự kiến `ops.publish(topic, payload, len, qos, retain, ctx)` chạy ở ngữ cảnh engine, KHÔNG chặn, trả `<0` = bận/lỗi (thử lại scan sau). Service lõi `services/plc_mqtt` (Layer 3): thanh ghi `0x0C00/0x0E00`, COV/chu kỳ, dựng topic + payload; bật bằng `SPLC_MQTT_ENABLE` (mặc định 0, Zigbee-IO không bị ảnh hưởng). **Lib GIỮ cấu hình broker** (host, port, client_id, user, pass, TLS, clean session, keepalive, version) vì App gửi `0x0E00` ở mọi lần deploy (`MqttBrokerEnabled` mặc định `true`): struct C thường `splc_mqtt_broker_t`, user đọc qua `splc_mqtt_get_broker(&generation)`; `generation` chỉ tăng khi cấu hình THẬT SỰ đổi (tránh reconnect sau mỗi lần nạp rule). Password đọc lại qua Modbus luôn trả 0 (nhưng nằm nguyên văn trong Flash — ghi rõ khi giao tài liệu). Host cập nhật struct trạng thái (`splc_mqtt_set_status`: CONNECTED/TLS_ACTIVE/AUTH_OK, latency, last_error), lib phản chiếu ra `0x0E54..0x0E5F`; lib chỉ publish khi CONNECTED **và `status.generation == generation` hiện tại** (host báo kèm generation của cấu hình mà kết nối được dựng theo; nhờ vậy khi App đổi broker lúc COMMIT, lib tự dừng publish tới broker cũ cho tới khi host nối xong broker mới). Host tự ngắt/nối lại và xoá hàng đợi publish của broker cũ; có quay về broker cũ khi broker mới lỗi hay không là chính sách của port (khuyên: không tự quay về). `get_broker` copy ra (an toàn đa luồng), không trả con trỏ. **Nguồn cấu hình broker (người dùng chốt: "chưa config thì dùng mặc định của gateway, App gửi mới thì theo App"):** lib có cờ `app_configured` = đã nhận cấu hình broker từ App VÀ `enabled=1` VÀ host không rỗng (Studio mặc định `MqttBrokerEnabled=true` nên vẫn ghi `0x0E00` với host rỗng khi người dùng chưa nhập gì). `splc_mqtt_get_broker(out, &app_configured, &generation)`; port của host: `false` → dùng `g_sys_config.mqtt` mặc định, `true` → dùng cấu hình của lib; `generation` tăng khi `app_configured` đổi theo cả hai chiều. CLEAR_RULES/FACTORY_RESET xoá cả cấu hình broker (quay về mặc định gateway). `enabled=0` (kể cả kèm host không rỗng) = quay về cấu hình mặc định của gateway (người dùng chốt); hệ quả: không tắt được MQTT bằng ô Enabled của hộp thoại broker, muốn ngừng publish thì tắt/bỏ các khối telemetry. Không cần cờ `splcopts.h` cho ưu tiên App/web. Cấu hình web `g_sys_config.mqtt` của `wg_ue`: việc của port (dùng cái nào/ghi đè), lib không can thiệp. **Chưa duyệt thiết kế chi tiết.**
32. **Lưu/áp dụng cấu hình MQTT (ĐỀ XUẤT, đồng ý hướng chung; chi tiết chờ duyệt).** Đã đọc `RuleTableWriter.cs`: App ghi Timer `0x0B00` → Counter `0x0B40` → Telemetry `0x0C00` (khối thừa bị tắt) → Broker `0x0E00` → rồi staging/commit rule; mỗi khối 96 reg bị cắt thành nhiều frame FC16 nhỏ; không có try/catch riêng cho nhóm ghi MQTT (một exception làm hỏng cả deploy). Vì vậy: ghi vào BẢN NHÁP (kiểm rẻ từng mảnh, địa chỉ ngoài bản đồ → `0x02`), kiểm nội dung cả bộ lúc COMMIT, áp dụng nguyên tử, lưu Flash cùng Rule Table (một CRC; cờ "có MQTT" dự kiến bit 13 của `rule_count` — LƯU Ý: hiện `SPLC_RULE_FLASH_COUNT_MASK = 0x3FFF` (bit 0..13) và `_Static_assert` ở `plc_rule_flash.c:39-42` đòi FB_FLAG|FB_V2_FLAG|COUNT_MASK phủ đủ 0xFFFF, nên phải thu mask về `0x1FFF` + sửa assert; bản ghi cũ có bit 13 = 0 nên vẫn nạp được; kích thước: 3 336 B hiện tại + 768 B telemetry + 192 B broker = 4 296 B (làm tròn 4 304 B, bội 16 B) < sector 8 KB). Khối KHÔNG được ghi trong nháp thì GIỮ cấu hình đang chạy (KHÁC FB, quyết định 8). Vùng `0x0E00..0x0E53` luôn ACK, không trả exception. Nên để App chỉ gửi MQTT khi thiết bị hỗ trợ (thăm dò `0x0E54`) — chưa trao đổi với đội App.
33. **Layout khối MQTT:** Header `splc_mqtt_wire.h` + Structs 3.9/3.10 + golden vector là đúng (Header có `en_tag_index`, `tag_count`; ICD thì không). `MqttTelemetryCodec.cs` của App khớp Header. ICD `SPLC_SPEC_MQTT_001` sai — chờ đội App sửa.
34. **MQTT Subscribe (chiều nhận) chưa có wire format.** App có UI/parser Subscribe (`MqttSubscriptionParser`, `MqttSubscriptionConfigDto`) nhưng không có thanh ghi/codec nào trong `Infrastructure`/`Protocol/Constants` nạp nó xuống MCU (grep `paa` `8056a62`) — hiện chỉ chạy phía App. KHÔNG làm firmware Subscribe cho tới khi đội App chốt spec. Hướng dự kiến: ống đối xứng `splc_mqtt_input(topic, payload, len)`.

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
3. *(XONG ở sandbox)* `nm` trên `GatewayV2.elf` thật: không symbol trùng; `HAL_UART_RxCpltCallback` chỉ
   có bản của host; `PVD_AVD_IRQHandler` là `W` từ startup (SimplePLC tắt PVD ở gateway); nanoMODBUS của
   host (20 hàm `nmbs_*`) và bản riêng `splc_nmbs_*` (9 hàm còn sau gc) cùng tồn tại. Header trùng basename
   với host chỉ còn `logger.h`, `tusb_config.h` (+ `.c`: `logger.c`, `usb_descriptors.c`) — đúng các file
   SimplePLC loại khi nhúng; đã xác nhận bằng `compile_commands.json` (29 unit của SimplePLC, 0 unit
   đưa `utils/logger` hay `port/usb` vào include path/build).
4. *(XONG ở sandbox)* Build `wg_ue` đầy đủ kèm SimplePLC link được (số đo ở mục 0). Submodule của `wg_ue`
   đủ khi clone `--recurse-submodules`. Còn lại: người dùng chép `wg_ue_changes/` vào `wg_ue` thật,
   thêm `libs/simple_plc` (submodule), quyết định chỗ lấy RAM, build lại trên máy của mình.
5. *(đã đọc code; còn quyết định của người dùng)* **Bootloader + Flash** — đã đọc `libs/bootloader/
   flash_define.h`, `bootloader.c`, `STM32H563xx_FLASH.ld`, `apps/ota/ota.c`, `ports/flash_port`:
   - Số học đề xuất ĐÚNG: 56 KB boot + 8 KB partition + 3 slot × 82 sector (656 KB) + 16 KB scratch =
     2048 KB; app link cố định tại `0x08010000`. Thu mỗi slot 82→80 sector (640 KB) → 3 slot kết thúc ở
     `0x081F0000`; dữ liệu 40 KB `0x081F0000..0x081F9FFF`; 1 sector trống `0x081FA000`; scratch giữ
     `0x081FC000`. Cả vùng dữ liệu nằm bank 2 (≥ `0x08100000`); app chạy từ bank 1 nên erase bank 2 không
     làm đứng CPU.
   - Image app 603 660 B < 640 KB (dư 51 700 B, 7,9%).
   - **Phải đổi (host, người dùng làm):** `flash_define.h` (82→80 ×3), `LENGTH` FLASH của app 656K→640K,
     build + nạp lại bootloader.
   - **Rủi ro lớn:** `bootloader.c` chỉ ghi địa chỉ/kích thước slot vào partition table khi `magic_number`
     sai (khởi tạo lần đầu). Thiết bị đã có partition table GIỮ layout 82 sector cũ: slot Factory cũ phủ
     `0x081F0000..0x081FC000` = ĐÚNG vùng dữ liệu SimplePLC → bootloader/OTA sẽ ghi đè lẫn nhau. Board dev
     phải xoá sector partition `0x0800E000` sau khi nạp bootloader mới; thiết bị ngoài thực địa cần cơ chế
     migrate riêng (CHƯA có).
   - Địa chỉ `0x081F0000` vẫn chỉ là đề xuất, KHÔNG nạp cho tới khi người dùng chốt các mục trên.
6. *(cần người dùng)* Chốt chủ sở hữu đồng hồ (quyết định 23) nếu cần Time Window trên gateway.
7. *(cần người dùng/phần cứng)* **Firmware slave cho board IO** (khối `0x0C00..0x0C06` bằng
   nanoMODBUS; lease/SAFE nằm ở slave; chốt baud/parity — đang giả định 115200 8N1). Thiếu nó
   thì gateway chạy nhưng DI/DO ảo luôn offline → chặn test thật.
8. *(cần phần cứng)* Nạp + test: bật `GW_USB_NETIF_ENABLE=1`, WiFi lên IP, mở COM bằng Studio,
   chạy `test_plc.py`; đo độ trễ khi task engine và task USB chạy cùng lúc; kích thước ring phải
   đủ khung dài nhất (256 B RTU) khi engine ngủ một scan 10 ms.
9. *(chờ xác nhận)* **Studio nhận gateway:** `paa` `8056a62` đã có hồ sơ `GatewayRs485EthV2_0` (class 3, variant 1,
   protocol 2, rule_format 7) — CHƯA kết nối lại gateway bằng bản mới. Việc: kết nối lại; kiểm danh mục tag gateway
   (112 tag, layout dense: DI 0-3, DO 4-7, VFLAG 8-39, VREG 40-71, RETAIN 72-103, COUNTER 104-111); kiểm trần
   DI/DO/AI khi dựng profile (trước đây 8/8/4 cho mọi class — gateway nhiều board IO có thể vượt).

**A'. Rủi ro của port đã thấy khi đối chiếu `wg_ue` (chưa đo):**
- Task `SPLC` priority 6 trùng `UART_LOG` (6), thấp hơn MODBUS/BROKER/SHELL (11) và NETWORK (12) → khi
  gateway bận scan 10 ms có thể trễ. Đo trên board.
- FIFO CDC của TinyUSB: engine đọc/ghi từ task khác `usb_netif_task` (đang chạy `tud_task_ext`).
  **Đã kiểm mã nguồn (chưa chạy):** host đặt `CFG_TUSB_OS = OPT_OS_FREERTOS` nên `OSAL_MUTEX_REQUIRED=1`,
  và `tu_edpt_stream_init()` (`tusb.c:317`) tạo mutex cho FIFO RX/TX → truy cập song song được bảo vệ.
  Priority: `usb_netif_task` = 5, `SPLC` = 6 (trên 5, dưới nhóm ứng dụng 10..12). Task `SPLC` priority 6
  trùng `UART_LOG`/`LOG_THREAD_PRIO`(6); vẫn thấp hơn MODBUS/BROKER/SHELL (11), NETWORK (12): đo trễ scan trên board.
- **Chồng thao tác Flash với OTA (HOÃN — OTA không dùng, quyết định 27):** `ports/flash_port/flash_port.c` (OTA ghi Secondary
  slot, một phần nằm bank 2) gọi `HAL_FLASHEx_Erase` KHÔNG có khoá; `plc_rule_flash`/`plc_retain` cũng
  erase/ghi bank 2 qua `sx_flash`. Hai bên cùng lúc → lỗi BUSY/hỏng dữ liệu. Cần một khoá dùng chung
  (vd. option `SPLC_FLASH_LOCK()/UNLOCK()` trong `splcopts.h` + host bọc `flash_port` bằng cùng mutex) —
  thiết kế chưa chốt, cần người dùng.
- RAM host (xem mục 0): chỉ còn ~912 B trống ở baseline; SimplePLC +22 472 B; task `SPLC` 1536 word = 6 KB
  lấy từ heap.

**B. Remote I/O còn mở:** chưa phát hiện trùng `unit_id` giữa hai hàng bảng trên cùng bus; chưa có
tag báo "node offline" cho App.

**C. App + spec (không gấp):** (đã đọc code `paa` `8056a62`, chưa chạy) `RuleCompiler.cs` dùng CV tag đã resolve làm
`retainTagIndex` (hết giá trị dự phòng 84); `FunctionBlockGateway` chia 64+64 khi đọc `0x0B00..0x0B7F`. Còn lại: App sinh rule
"đúng giờ" `EQ, Lo=Hi` (chưa kiểm); Structs mục 4 (bảng 124 tag cố định) chỉ đúng cho Remote I/O V1.

**D. Firmware khác:** (`components/usb_cdc/` và `port/modbus_usb/` ĐÃ xoá trong vài commit gần nhất;
đã kiểm bằng `ls`) validate `guard/trigger/action_tag` ∈ `0..MAX_TAGS-1` khi nạp rule (hiện index
sai chỉ vô hiệu hoá rule âm thầm); chưa ghi snapshot retain ngay trước REBOOT; lỗi Flash lúc erase
trong bước copy A→B huỷ bản cũ trước khi biết lỗi; `ACT_WRITE_REMOTE/LOG_EVENT/SEND_ALARM` chưa
implement; không có log trước `logger_init` (đọc Flash trong `plc_engine_init()` chạy TRƯỚC
`splc_board_init()`); `utils/filter/*` chưa soát; linker `STM32H523xx_FLASH.ld` khai `LENGTH=512K`
(chip 256 KB — HỎI người dùng trước khi sửa, quyết định 9); comment cũ nhắc `modbus_usb`/
`sx_usb_cdc`/`FREE_RTOS` còn ở `plc_modbus_cfg.h`, `services/CMakeLists.txt`, `app/CMakeLists.txt`,
`modbus_transport.h`, `splc_board.h`.

**E. MQTT (theo thứ tự):**
1. Viết thiết kế chi tiết `plc_mqtt` (phần Publish): API ống publish, struct trạng thái, bản đồ nháp/COMMIT, thuật toán COV/chu kỳ, dựng payload, ước lượng RAM/Flash → người dùng duyệt → mới code. Mỗi bước: build ARM Zigbee-IO (với `SPLC_MQTT_ENABLE=0` phải ra ĐÚNG số đo chuẩn) và Gateway.
2. *(người dùng)* Kết nối lại gateway bằng App mới (bước 4.A.9). Hỏi đội App: (a) Subscribe nạp xuống MCU bằng khối thanh ghi nào; (b) thăm dò hỗ trợ MQTT (đọc `0x0E54`) và chỉ gửi MQTT khi hỗ trợ; (c) sửa ICD cho khớp Header/Structs.
2b. Phía App (đã đọc `paa` `8056a62`): nút Test Connection của hộp thoại broker chỉ mở TCP socket từ PC tới `host:port` (timeout 3 s), không có MQTT handshake/auth/TLS, và không chứng minh gateway tới được broker. App không có thư viện MQTT và KHÔNG đọc vùng trạng thái `0x0E54..0x0E5F`; `MqttBrokerConfigCodec.Decode` có nhưng chưa ai gọi (và password đọc lại luôn rỗng, khớp thiết kế quyết định 31). Đề nghị đội App: hiển thị trạng thái kết nối thật của thiết bị từ `0x0E54`, đổi nhãn nút Test thành "PC tới broker". Mật khẩu broker lưu chữ thường trong `ProjectMetadata`.
3. **Quyết định đã chốt (người dùng):** (b) `{ts}` khi chưa có giờ → gửi `0`; (c) gateway BẬT `SPLC_MQTT_ENABLE=1` ngay để thử lib (Zigbee-IO mặc định 0); (d) RAM: cứ build + chạy rồi mới tính hạ heap (`configTOTAL_HEAP_SIZE`); ước lượng khối MQTT ≈ 2,3–2,5 KB tĩnh trong khi host chỉ dư 2 992 B (quyết định 26) nên nhiều khả năng phải hạ heap — đo `lowest free` trên board trước; (a) `{val}` phải ra số THẬP PHÂN ngay ở bản đầu (cách thực hiện: xem 3b, CHƯA chốt).
3b. **Số thập phân trong payload — CHƯA chốt cách làm.** Vấn đề (lỗ hổng thiết kế, không phải bug code): tag là `int32` (`g_tag_value`, threshold/`action_param`, PV/CV đều `int32`; Rule Format 7 đã chốt) nhưng spec ví dụ `24.5`/`101.3`/`42.8` (Profile §5.3, ICD); `deadband` là float32; không tài liệu nào nói tag nguyên → số thập phân thế nào (đã grep Profile, ICD, Conformance, Structs v2.0). Cơ chế tỉ lệ duy nhất có sẵn là action `SCALE_TAG` (`y=k*x+b`, `k` nhân 1000) — vẫn ra số nguyên; vd 24,75 phải được rule sinh thành `2475` (nhân 100) rồi mới publish, và "số chữ số" phải KHỚP hệ số nhân đó. Phương án đã cân nhắc: (1) không làm gì — gửi `2475`, người nhận tự chia; (2) một số chung cho cả thiết bị, option `SPLC_MQTT_DECIMALS` trong `splcopts.h`; (3) một số cho mỗi khối publish, dùng `reserved_header` (+15, App hiện luôn ghi 0 — `MqttTelemetryCodec.cs:94`); (4) mỗi tag một số trong +15 (8 tag × 2 bit, tối đa 3 chữ số); (5) bảng thuộc tính tag mới ("Tag Scale", mỗi tag 4 bit, nháp+COMMIT+Flash ~64 B, App thêm thuộc tính Decimals cho từng tag; dùng chung được cho Subscribe sau này — hướng đúng bản chất, giống thanh ghi hệ số của SunSpec); (6) ghi thẳng trong template Custom: `{val:2}` = 2 chữ số thập phân, `{val}` = số nguyên, không đổi wire; App chỉ chuyển tiếp chuỗi `CustomTemplate` (không thấy validator placeholder, chỉ `MqttTopicValidator` xử lý `{device}` trong topic — CHƯA chạy App kiểm). **ĐỀ XUẤT của Claude (chờ người dùng chốt):** cách 6 + cách 2 làm số mặc định cho chế độ Compact/Raw (mặc định 0), nâng lên cách 5 khi App sẵn sàng (`{val:N}` vẫn chạy như ghi đè). Quy tắc kèm theo: deadband tính theo đơn vị kỹ thuật = `deadband × 10^dec` so với delta thô; định dạng bằng số nguyên, KHÔNG dùng printf float (tốn Flash); số âm phải giữ dấu (-0,5); `dec` > 6 hoặc cú pháp sai → từ chối lúc COMMIT. **Đã loại:** (i) tách phần nguyên/phần thập phân thành hai thanh ghi (mất dấu số âm khi phần nguyên = 0, mơ hồ số chữ số, đọc lệch giữa hai lần FC03); (ii) tag float32 (đổi toàn bộ lõi `int32`, float32 chỉ chính xác tới 2^24 ≈ 16,7 triệu nên bộ đếm và epoch vẫn phải là int, so sánh `EQ` không ổn định, `printf` float tốn Flash host đang cạn). Muốn float thật thì làm một đợt thiết kế riêng (vd. loại tag `TAG_VFLOAT`, protocol version mới). Câu hỏi cho đội App: spec ví dụ `24.5` nhưng tag là `int32` — App định quy đổi thế nào (nhân sẵn hay hệ số riêng) và có cho gõ `{val:N}` trong template không.
3c. **Baseline ARM đo phiên này** (gcc 13.2.1, `/tmp`, đường dẫn khác máy người dùng): Gateway chuẩn RAM 72 624 B / FLASH 55 356 B — KHỚP số chuẩn; Zigbee-IO mặc định RAM 77 664 B / FLASH 80 264 B (handoff ghi 80 248 B; lệch 16 B, nhiều khả năng do chuỗi đường dẫn nhúng — bài học 24). Cấu hình Gateway chuẩn dùng tạm `splcopts.h`: `SPLC_PLATFORM_STM32H5`, `SPLC_BOARD_GATEWAY`, `SPLC_FLASH_SIZE_KB 256`, `SX_OS_USE_FREERTOS 0`, `SPLC_PORT_USB_CDC 0`. **Lệch tài liệu:** quyết định 29 nhắc thư mục `splc_config_rs485/` và preset `Release-RS485` nhưng cả hai KHÔNG có trong repo; `splc_config/splcopts.h` còn comment "RS485 variant" nhưng đặt `SPLC_PORT_USB_CDC 1` — cần người dùng xác nhận (chưa commit hay lỗi thời).
4. Host `wg_ue` đã có `apps/mqtt_client` + `data_broker` (chưa đọc lại phiên này): cần quyết định bật cái nào và QoS/retain theo từng khối.
5. Ghi nhớ: `0x0C00..0x0C06` trên slave board IO (Remote I/O, architecture mục 5.1) trùng SỐ với Telemetry Block 0 của gateway — khác thiết bị/bus nên không xung đột thật, nhưng dễ nhầm.

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
21. Nhúng vào host thật mới lộ điều mô hình giả không lộ: (a) RAM host chỉ còn ~912 B (số RAM của
    SimplePLC đo độc lập là 72 KB, nhưng thực tế thêm 22 KB sau gc — đọc map thật, đừng cộng số đo rời);
    (b) OBJECT library gắn vào static library không được kéo vào (thứ tự archive) → gắn vào executable;
    (c) trùng tên target (`splc_port`). Luôn build baseline host TRƯỚC, rồi mới nối.
22. Đọc code bootloader trước khi tin một đề xuất bản đồ Flash: partition table lưu kích thước slot trong
    Flash, nên đổi `flash_define.h` mà không migrate thì thiết bị cũ chạy layout cũ.

23. Đo bằng ĐÚNG cờ mà người dùng build (`build.bat`), không bằng preset mặc định: `GW_MDNS_ENABLE`
    mặc định OFF trong CMake nhưng `build.bat` đặt ON, nên số đo preset lệch vài trăm byte RAM/KB FLASH.
24. So FLASH giữa hai máy/hai thư mục build phải tính đường dẫn tuyệt đối nhúng trong ELF (`__FILE__`,
    ~49 chuỗi ở `wg_ue`): 1 ký tự khác nhau ≈ 49 B. Dùng hiệu số (có/không SimplePLC) cùng một máy.
25. Hướng dẫn build cho người dùng: `build.bat` gốc của `wg_ue` cứng đường dẫn Ninja `C:\Users\DELL\...`
    (không chạy được trên máy khác); chạy nó dù chỉ một lần cũng ghi `CMAKE_MAKE_PROGRAM` sai vào cache
    `build\` → phải xoá `build\` (`build_gw.bat rebuild`). `apps\CMakeLists.txt` và `apps\main\CMakeLists.txt`
    dễ nhầm: chỉ file thứ hai định nghĩa `apps_main`. Trong khối `( )` của batch, `%ERRORLEVEL%` bị thay
    ngay lúc đọc khối — dùng `if errorlevel`/`exit /b 1`.
26. **Triệu chứng bus lạ → loại trừ linh kiện trước khi dựng giả thuyết.** Bring-up RS485 gateway (quyết định 30) mất nhiều vòng:
    mô phỏng "đảo cực" khớp từng byte, rồi bias, rồi break dài; cuối cùng chỉ thay IC MAX485 là hết. Mô phỏng khớp ≠ nguyên nhân gốc.
    Khi nghi phần cứng: đổi linh kiện/dongle/dây, đo A−B và chân RO bằng đồng hồ/scope trước khi đưa kết luận. Log `RS485_DIAG` chỉ in
    16 byte đầu của khung cuối, đừng suy ra nội dung các khung trước; số cạnh lấy mẫu 1 ms vô nghĩa ở 115200 baud.

27. **Pull repo liên quan trước khi kết luận "App chưa có X".** Đã có lần kết luận App chưa có MQTT trên commit cũ (`254ae0a`); sau khi pull, `cdc0738`..`8056a62` đã có đủ codec, UI, Subscribe. Pull rồi grep mọi loại file và nêu rõ commit đã đọc.
28. **Tài liệu spec có thể tự mâu thuẫn — chọn bản đúng bằng code codec thật.** ICD MQTT xếp khác Header/Structs; chỉ code codec của App cho biết bản nào đang chạy. Không lấy code mẫu trong tài liệu làm nguồn.

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