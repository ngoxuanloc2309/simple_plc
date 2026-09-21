# SimplePLC — Handoff

> Mục đích: cho phiên làm việc tiếp theo (Claude khác hoặc chính bạn) nắm
> "đang ở đâu, làm gì tiếp" mà không cần đọc lại lịch sử chat.
> Đọc SAU `Readme.md` và `docs/architecture.md` (nguồn kiến trúc chính).
> File này chỉ ghi: trạng thái hiện tại, việc còn mở, bài học quan trọng.
>
> **Quy tắc vàng:** trước khi tin bất cứ điều gì dưới đây, chạy
> `git pull && git log --oneline -10`. Nếu có commit mới hơn, ưu tiên code
> thật hơn file này.

## 0. Trạng thái hiện tại

- Branch `main`, commit gốc đã verify: `98afb3e` ("fix read tag fail"),
  cộng thêm thay đổi tick/scan-interval ở mục 0b (CHƯA commit/push,
  CHƯA chạy trên board).
- Board đang dùng: **Zigbee-IO SKU** (`board/board_zigbee_io.c`), 4 DI /
  4 DO / 0 AI, STM32H523CCU6.
- **Đã chạy được trên board thật:** build (`build.bat build`), flash, USB
  enum + mount, Modbus server khởi tạo, **đọc/ghi Modbus bằng
  `test_plc.py`**, **nạp rule đầy đủ (stage → CRC → commit → READY)** và
  đọc lại `ACTIVE_RULE_TABLE` khớp hệt bản đã stage. MCU in
  `RULE UPLOAD DONE ...`, App in khối `RULE UPLOAD DONE` (xác nhận bằng số
  `ACTIVE_RULE_COUNT/CRC16/VERSION` đọc lại từ MCU).
- **Rule chạy thật trên board — ĐÃ XÁC NHẬN** (log `test_plc.py`: DI0 có
  xung 0→1 thì DO0 lên 1 và giữ nguyên; DI1..DI3 đọc đúng). Nhờ commit
  `98afb3e`: trước đó `g_tag_table[]` toàn `TAG_NONE` nên
  `plc_io_register_*()` từ chối âm thầm mọi kênh.
- **Rule KHÔNG được lưu vào Flash** — chỉ nằm trong RAM, mất điện/reset là
  mất (xem mục 3.7). Đây là hành vi hiện tại, không phải lỗi ngẫu nhiên.

## 0b. Thay đổi đang chờ build/flash (tick + nhịp quét)

Đã sửa trên PC, **chưa build bằng toolchain ARM, chưa chạy trên board**.
Người dùng đã chốt: truyền tick bằng **tham số** (không dùng setter) và
nhịp quét **10 ms**.

- `rule_scan(void)` → `rule_scan(uint32_t now_ms)`. Bỏ static
  `s_rule_scan_now_ms`. Quên truyền giờ = lỗi biên dịch (đó chính là bug
  3.1 cũ). Chỉ có 1 chỗ gọi: `plc_engine.c`.
- `plc_engine.h` thêm `PLC_SCAN_INTERVAL_MS` (mặc định 10U, ghi đè được
  bằng `target_compile_definitions`) và `plc_engine_poll()`: hàm không
  chặn, tự kiểm tra đã qua đủ nhịp chưa rồi mới chạy 1 vòng quét.
  `main.c` chỉ còn `plc_engine_poll();` — bỏ `s_last_scan_tick` và
  `SCAN_INTERVAL_MS` khỏi `main.c`. Toàn bộ logic pacing nằm trong engine
  (dễ reuse cho SKU khác).
- `scan_cycle(now)` trong `plc_engine.c` lấy tick **1 lần** đầu vòng và
  đưa cùng giá trị đó cho `rule_scan()` (mọi rule trong 1 vòng thấy cùng
  "bây giờ").
- **Bug mới tìm được, có sẵn từ trước, đã sửa:** nhánh `DWELLING` trong
  `plc_rule.c` tính `now_ms - dwell_start_tick` cả với rule INTERVAL /
  TIME_WINDOW (không bao giờ arm dwell, nên `dwell_start_tick ==
  DWELL_NOT_STARTED == 0xFFFFFFFF`). `now - 0xFFFFFFFF == now + 1`, nhỏ hơn
  `for_ms` khi tick vừa tràn qua 0 → rule INTERVAL đến hạn bị coi là "chưa
  đủ dwell" và trễ thêm 1 chu kỳ quét (1 lần / ~49,7 ngày). Sửa: chỉ xét
  dwell khi `dwell_start_tick != DWELL_NOT_STARTED`. Test hồi quy FAIL trên
  code cũ, PASS trên code mới.
- **Đã kiểm chứng trên PC** (gcc, stub logger + đồng hồ giả): dwell 100/300/
  500/700 ms bắn đúng hạn (trễ < 1 chu kỳ quét, không bao giờ sớm); hủy dwell
  giữa chừng thì không bắn và rise lần sau đếm lại từ đầu; INTERVAL 250/1000
  ms đúng khoảng cách; tràn tick 32-bit; 5 rule chạy song song độc lập;
  `plc_engine_poll()` chạy đúng ~100 vòng/giây kể cả khi tick tràn.
- **Chưa kiểm chứng:** build ARM; chạy trên board; ảnh hưởng của nhịp 10 ms
  tới `test_plc.py` (mỗi request Modbus giờ được phục vụ tối đa mỗi 10 ms —
  nếu thấy `No response` thì nghi điểm này trước).

## 1. Kiến trúc trong 30 giây

7 layer, include một chiều từ trên xuống, không heap. Chi tiết:
`docs/architecture.md`.

```
Layer 4    app/, board/        plc_engine, board_<sku>.c (pin wiring)
Layer 3    services/           plc_io, plc_retain, plc_modbus_cfg
Layer 3.5  port/               modbus_transport_t, modbus_usb
Layer 2    core/               tag table + rule engine (build được trên PC)
Layer 1    components/         driver contract (gpio/adc/flash/uart/usb_cdc)
Layer 0    platforms/stm32/stm32h5/   HAL thật
Layer U    utils/, libs/       cqueue, logger, filter, nanoMODBUS, TinyUSB
```

Vòng quét: `input_scan → rule_scan → output_scan → modbus_config_service
→ retain_service`.

Điểm cần nhớ khi đọc code:
- Layout tag v1.9: **không có sentinel ở index 0**, `TAG_DI0 == 0`.
  "Không có guard" = `GUARD_TAG_NONE` (`0x7FFF`), KHÔNG phải 0.
- `SPLC_RuleRecord` = 32 byte, CRC dùng CRC-16/MODBUS.
- `plc_device.h`, `plc_error.h`, `plc_system_cmd.h` cố ý chỉ có `.h`
  (không thêm `.c` vào Layer 2).
- Flash map (`platforms/.../flash_define/splc_flash_define.h`): Rule Table
  = sector #31 (`0x0803E000`), Retain = sector #27-30 (32KB, xoay vòng).

## 2. Việc tiếp theo (theo thứ tự đề xuất)

1. **Build + flash thay đổi ở mục 0b rồi chạy lại `test_plc.py`** để chắc
   nhịp 10 ms không làm hỏng đọc/ghi Modbus, rule đơn giản vẫn chạy. Sau đó
   thử 1 rule có dwell thật (ví dụ `for_ms = 500`) — trước đây rule dwell
   không bao giờ bắn.
2. **Lưu rule vào Flash** (mục 3.7) — cần chốt quyết định mở trước.
3. **Chốt các quyết định mở** (mục 4) trước khi viết thêm tính năng.

## 3. Bug đã biết, CHƯA sửa

### 3.1 / 3.2 — ĐÃ SỬA trên PC, chờ xác nhận trên board (xem mục 0b)

`s_rule_scan_now_ms` luôn = 0 (dwell/interval không chạy) và
`SCAN_INTERVAL_MS = 0U` (vòng quét hết tốc độ) đều đã xử lý bằng thay đổi
ở mục 0b. Chỉ đóng hẳn sau khi chạy đúng trên board.

### 3.3 `sx_usb_tiny_read()` — `_timeoutMS` vẫn đếm vòng lặp, không phải ms

`components/usb_cdc/sx_usb_cdc.c`. Bug nghiêm trọng (timeout=0 khiến hàm
luôn trả 0 byte dù queue đã đầy → App báo "No response", log
`available=8` lặp) **đã sửa ở commit `7c31964`**: vòng lặp giờ rút byte
sẵn có trước, chỉ xét timeout khi queue rỗng. Phần còn lại: biến `time++`
đếm vòng lặp `tud_task()`, KHÔNG phải mili giây. Với `timeout = 0` (cách
`plc_modbus_cfg` dùng) thì đúng; chỉ sai nếu sau này dùng timeout > 0.

### 3.4 `modbus_usb_write()` bỏ qua `timeout_ms`

`sx_usb_tiny_write()` luôn block tới khi ghi xong. Nếu USB nghẽn có thể
vượt ngân sách 10 ms của vòng quét. Cần quyết định: làm write non-blocking
thật, hay chấp nhận (App luôn đọc kịp).

### 3.5 Các việc chưa thực thi / chưa validate

- `write_system_command()` (`plc_modbus_cfg.c`) mới decode + đổi status
  sang ACCEPTED, **chưa thực thi** reboot / factory reset / clear rules /
  clear retain. Việc thực thi thuộc Layer 3/4, chưa viết.
- Chưa validate `guard_tag`/`trigger_tag`/`action_tag` nằm trong
  `0..MAX_TAGS-1` (hoặc `GUARD_TAG_NONE`) — không ở `rule_table_commit()`
  lẫn `plc_modbus_cfg.c` (kiểm tra `tag_idx >= MAX_TAGS` duy nhất nằm ở
  đường đọc runtime tag, không phải rule). Index sai không crash nhưng
  rule bị vô hiệu hóa âm thầm.
- **PVD → ghi Retain khẩn cấp chưa được nối.** Đã có: PVD bật trong
  CubeMX, `HAL_PWR_PVDCallback()` (`stm32h5_pwd.c`), API
  `sx_power_register_low_voltage_callback()`, `retain_snapshot_write()`.
  Thiếu: không có dòng nào (board/engine) gọi
  `sx_power_register_low_voltage_callback(retain_snapshot_write)`. Hiện chỉ
  ghi được mỗi 5 phút, mất điện đột ngột thì mất dữ liệu retain từ lần ghi
  cuối. `plc_retain.h` ghi rõ đây là việc của Layer 4. Lưu ý callback chạy
  trong ngắt: `retain_snapshot_write()` phải đủ ngắn và an toàn khi gọi từ
  ISR (chưa kiểm chứng).
- `TRG_TIME_WINDOW`: `now_hhmm` hardcode 0, chưa có nguồn RTC.
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM`: chưa có
  implementation (cần Modbus Master, Event Log, Alarm — chưa thiết kế).

### 3.6 Tồn tại trong repo nhưng KHÔNG PHẢI việc của Claude sửa

- `STM32H523xx_FLASH.ld` khai `LENGTH = 512K`, chip thật chỉ 256KB. File
  CubeMX tự sinh — nếu sửa thì phải qua CubeMX/`.ioc`, không sửa tay
  (sẽ bị ghi đè khi Generate Code). Chỉ báo người dùng.

### 3.7 Rule Table CHƯA được lưu vào Flash

`rule_table_commit()` chỉ `memcpy` vào `g_rule_table[]` (RAM).
`rule_table_load_from_flash()` (gọi lúc boot ở `plc_engine_init()`) hiện
chỉ là TODO: `memset` bảng về 0, không đọc Flash. Nghĩa là **mọi rule nạp
qua Modbus mất sau reset/mất điện**, board lên với 0 rule.

Đã có sẵn: chỗ dành cho rule (`SPLC_FLASH_RULE_TABLE_ADDR = 0x0803E000`,
1 sector 8 KB, đủ cho 100 rule × 32 B = 3200 B) trong
`platforms/stm32/stm32h5/flash_define/splc_flash_define.h`, và API
`sx_flash_read/write/erase`. `plc_retain` chỉ lưu tag `VREG_RETAIN`,
KHÔNG dùng cho rule (`plc_retain.h` ghi rõ).

Ràng buộc kiến trúc: Layer 2 (`plc_rule.c`) không được gọi `sx_flash_*`
(phải build được trên PC). Việc ghi/đọc Flash thuộc Layer 3; lúc boot đọc
Flash → kiểm CRC → đưa qua `rule_table_commit()` (đúng như comment TODO
trong `plc_rule.c`). Cần quyết định trước khi làm: lưu A/B luân phiên hay
1 bản; ghi ngay khi commit hay chờ lệnh riêng (mục 4, câu 9).

## 4. Quyết định kiến trúc CHƯA CHỐT (hỏi lại, đừng tự quyết)

1. ~~Cách Layer 4 truyền tick ms vào `rule_scan()`~~ — **ĐÃ CHỐT: tham
   số** `rule_scan(uint32_t now_ms)`, nhịp quét 10 ms
   (`PLC_SCAN_INTERVAL_MS`). Lý do: quên truyền giờ thì compiler báo lỗi
   (setter thì im lặng chạy sai — chính là bug 3.1 cũ), và test trên PC chỉ
   cần truyền giờ giả.
2. **Retain: giữ ghi mỗi 5 phút + PVD khẩn cấp, hay đổi?** Ghi định kỳ
   (`RETAIN_SNAPSHOT_PERIOD_MS`) đã chạy. Ghi khẩn cấp khi sụt áp: phần cứng
   PVD đã bật (`Core/Src/stm32h5xx_hal_msp.c`) và callback đã có
   (`sx_power_register_low_voltage_callback()`), nhưng **chưa ai gọi hàm
   đăng ký đó với `retain_snapshot_write`** — xem mục 3.5.
3. **Pin mapping đầy đủ 8DI/8DO/4AI** — board Zigbee-IO mới có 4DI/4DO/0AI.
   `.ioc` hiện có `OUT0-3`/`IN0-3`; ADC mới cấu hình 1 kênh (IN1).
   Cần thêm kênh trong CubeMX nếu SKU 4AI thật sự cần.
4. **Dùng `utils/filter/` (MA/EMA/Median/Kalman/IIR...) cho AI không**, loại
   nào, và wire vào CMake thế nào. Code filter có sẵn nhưng chưa dùng ở
   đâu; `plc_io.c` hiện lưu raw ADC code, không lọc.
5. **Validate `guard_tag` ở đâu**: `plc_modbus_cfg.c` hay
   `rule_table_commit()`.
6. **`ADC_SAMPLETIME_247CYCLES_5`** có phù hợp trở kháng nguồn 4AI thật
   không (cần spec).
7. **Modbus Master cho Gateway** (RTU/TCP) — để dành, chỉ cần USB lúc này.
8. **`components/CMakeLists.txt` link `tinyusb` không điều kiện** — nếu
   chưa `git submodule update --init --recursive` thì configure vỡ. Xác
   nhận người dùng luôn checkout submodule trước khi build; nếu có thì chỉ
   cần ghi comment, không cần sửa CMake.
9. **Lưu Rule Table vào Flash thế nào** (mục 3.7): (a) 1 bản hay 2 bản
   A/B để chống mất điện giữa lúc xóa/ghi sector; (b) ghi ngay khi commit
   hay chỉ khi App gửi lệnh riêng (có thể gắn vào `SYSTEM_COMMAND`, mục
   3.5); (c) cần header (magic + `rule_count` + CRC) để boot kiểm hợp lệ.

## 5. Bài học đã rút ra (quan trọng, đọc kỹ)

### 5.1 Bug Modbus/USB đã sửa (commit `689edc3`) — đừng điều tra lại

**Triệu chứng:** "USB enum fail" — cắm USB sau khi board đã chạy thì không
enum, reset thì OK, rút cáp không có log.

**Nguyên nhân thật:** KHÔNG phải USB. `plc_modbus_cfg_init()` gọi
`nmbs_server_create(..., address_rtu = 0, ...)`. nanoMODBUS **từ chối**
`address_rtu == 0` trên RTU (0 là broadcast) và trả lỗi *trước khi* gọi
`nmbs_create()`. Code bỏ qua giá trị trả về nên vẫn log `init OK`, còn
`s_nmbs` (static) toàn 0 → mọi con trỏ `platform.read/write/flush` là
NULL → lần `nmbs_server_poll()` đầu tiên gọi vào `0x0` → **HardFault**.
CPU treo thì `tud_task()` ngừng chạy nên USB stack không phản hồi bus
reset — đó là lý do trông như lỗi enum. Reset board "chữa" được vì USB đã
cắm sẵn nên enum xong trước khi crash.

**Cách sửa:**
- `modbus_transport_t` có thêm field `unit_id` (1..247);
  `modbus_transport_usb_create(usb, unit_id)`; board đặt
  `MODBUS_UNIT_ID 1` trong `board_zigbee_io.h`.
- `plc_modbus_cfg_init()` kiểm tra kết quả `nmbs_server_create()`; nếu lỗi
  thì log + đặt `s_initialized = false`. `modbus_config_service()` vẫn
  gọi `transport->process()` (giữ USB sống) nhưng bỏ qua
  `nmbs_server_poll()` khi chưa khởi tạo.
- **nanoMODBUS lọc theo unit ID thật** (request có unit khác bị bỏ qua,
  không trả lời) — comment cũ "accepted but not checked" là SAI. App phải
  gửi đúng `MODBUS_UNIT_ID` (`test_plc.py --unit`, mặc định 1).

**Cách chẩn đoán đã hiệu quả** (dùng lại cho bug tương tự): GDB + `bt`
thấy frame `#2 0x00000000` (gọi con trỏ NULL) → tính offset `platform`
trong `nmbs_t` bằng compile thật → `x/8wx` đọc thẳng RAM thấy toàn 0 →
chứng minh `nmbs_create()` chưa từng chạy. Build Release không có `-g`
nên `print s_nmbs.platform.read` báo "unknown type"; dùng địa chỉ +
offset thay thế.

### 5.1b Nạp rule qua Modbus: 3 bug nối tiếp (commit `9789b3a` trở đi)

Khi chạy `test_plc.py` lần đầu trên board, việc nạp rule vỡ theo 3 lớp,
lớp trước che lớp sau. Đã sửa hết; ghi lại vì cùng kiểu lỗi có thể tái
diễn khi thêm thanh ghi mới.

1. **FC06 tới `0x9002`/`0x9003` → `ILLEGAL_DATA_ADDRESS` (exception 2).**
   `cb_write_single_register()` chỉ chấp nhận `0x0A00`/`0xA000`; các thanh
   ghi RW còn lại nằm trong `s_blocks`, nhưng callback ghi của chúng bị đặt
   nhầm vào cột `write_single_cb` (không ai gọi, signature cũng sai). Kết
   quả: cả FC06 lẫn FC16 tới hai thanh ghi này đều lỗi. Sửa: bảng dùng một
   cột `write_multi_cb` duy nhất, FC06 chuyển tiếp vào cùng đường FC16 với
   `quantity = 1`. Bất kỳ thanh ghi RW 1-register mới nào cũng đi qua đó.
2. **CRC của Rule Table tính trên byte RAM của struct.** Sai cả về ý nghĩa
   (phụ thuộc endianness/layout chip) lẫn giá trị (`nmbs_crc_calc` trả CRC
   bị hoán byte, không phải CRC-16/MODBUS chuẩn). **Quy ước chốt theo spec
   8.4:** CRC tính trên chuỗi *wire* — `rule_count × 32 byte`, mỗi
   register high byte trước, mỗi trường 32-bit High Word rồi Low Word —
   độc lập với chip. Firmware: `rule_record_to_wire()` +
   `rule_table_wire_crc16()` trong `plc_modbus_cfg.c`; App:
   `rule_registers_to_bytes()` trong `test_plc.py` (ghép register
   big-endian). **Không dùng `nmbs_crc_calc` cho việc này.**
3. **`test_plc.py` cũ tính CRC trên byte RAM little-endian** — khớp với
   firmware cũ chứ không khớp spec. Đã đổi sang wire big-endian. App thật
   sau này (C#/web) phải hash đúng 32 byte/rule theo spec 8.4, không hash
   struct nội bộ của chính nó.

Khi thêm log chẩn đoán nạp rule: `RULE UPLOAD DONE` (INFO) + liệt kê
`rule[i]` (DEBUG, tối đa 10 rule để không tràn UART/USB) ở firmware; khối
`RULE UPLOAD DONE` đọc lại `ACTIVE_RULE_COUNT/CRC16/VERSION` từ MCU ở
`test_plc.py` (báo `MISMATCH` và raise nếu MCU khác số đã gửi).

### 5.2 Giả thuyết đã bị bác bỏ — không quay lại

`vbus_sensing_enable = DISABLE` (`Core/Src/usb.c`) từng bị nghi là nguyên
nhân USB. Người dùng bác bỏ vì sản phẩm cũ cấu hình y hệt vẫn chạy tốt —
và đúng, nguyên nhân thật là mục 5.1. **Bài học:** lý thuyết khớp triệu
chứng chưa đủ để kết luận; hỏi sớm "đã từng làm cấu hình tương tự chưa"
trước khi đầu tư đọc datasheet.

### 5.3 Nguyên tắc code

- **Luôn kiểm tra giá trị trả về của hàm khởi tạo thư viện ngoài.** Bug
  5.1 ẩn được chỉ vì bỏ qua return value và log `init OK` vô điều kiện.
- **Đừng tin comment mô tả hành vi thư viện** — đọc code thư viện thật.
  Comment "unit_id accepted but not checked" viết ra từ giả định, chưa
  từng chạy trên phần cứng.
- `gcc -c` sạch chỉ chứng minh cú pháp, không chứng minh chạy đúng. Mọi
  module mới nên có test chạy thật trên PC (transport giả) trước khi lên
  board.

### 5.4 Bài học phương pháp từ chuỗi debug nạp rule

- **Đối chiếu SPEC trước, đừng lấy giá trị một bên làm đáp án.** Lần sửa
  đầu chỉ so với con số `test_plc.py` in ra rồi coi là đúng; hóa ra cả hai
  phía cùng lệch spec, mất thêm 2 vòng chạy board mới lộ.
- **Con số trong log là bằng chứng để định danh phiên bản code.** Log
  `actual=0x6575` làm nghi "board chạy ELF cũ", nhưng thêm 1 dòng log chẩn
  đoán in cả `crc_wire` lẫn `crc_ram` cạnh nhau cho thấy code mới đang chạy
  đúng, còn *App* mới là bên tính CRC trên chuỗi byte khác. Nếu bế tắc, in
  cả hai giả thuyết ra cạnh nhau thay vì suy diễn từ một con số.
- **Mô phỏng đầu-cuối trên PC bằng chính `test_plc.py`**: nối script với
  firmware build PC (server đọc/ghi frame RTU qua stdin/stdout). Bắt được
  lỗi mà test từng phía riêng lẻ bỏ sót. Lưu ý stub `logger` phải in ra
  `stderr`, không phải `stdout`, nếu không lẫn vào luồng frame.
- **Log chỉ thấy "không có" khi nó bị trôi.** Vòng `Watching DI0/DO0` poll
  10 lần/giây, mỗi request sinh 2 dòng `SX_USB_TINY` DEBUG (`available=`,
  `USB write:`), che mất dòng commit. Đây là hành vi đúng, không phải bug;
  đang phát triển giữ `LOGGER_DEBUG`, chỉ cần đổi mức `logger_init` sang
  INFO khi muốn yên tĩnh (các dòng quan trọng đều là INFO/WARN).

## 6. Quy trình làm việc với người dùng

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Bắt đầu phiên hoặc khi
  người dùng báo "đã push": `git pull`, rồi **build/compile verify thật**
  (không chỉ đọc diff). Người dùng hay sửa song song trên CubeMX/GitHub
  UI.
- **Giao file:** người dùng muốn nhận **nguyên file** (present từng file
  để copy-paste cả file), KHÔNG muốn patch/zip/đoạn thay thế.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, **tách từng
  quyết định nhỏ**, không gộp nhiều câu vào một.
- **Không tự sửa file CubeMX tự sinh** (ghi "Auto-generated" /
  "generated only once") — kể cả khi thấy bug thật. Chỉ báo người dùng.
- Khi đề xuất nguyên nhân, nói rõ mức chắc chắn; kiểm chứng bằng dữ liệu
  thật (GDB, log) trước khi khẳng định.
- **Trước khi hỏi người dùng "có build lại chưa"**, đối chiếu file họ đã
  push với file mình giao (`cmp`) và kiểm tra chuỗi/số duy nhất của bản
  mới có trong log. Người dùng thường push đúng; nghi ngờ nên đặt vào
  giả thuyết khác trước.

## 7. Lệnh verify nhanh (không cần toolchain ARM)

**Layer 2** (rule engine): compile `core/plc_tag/plc_tag.c`,
`core/plc_rule/plc_rule.c`, `core/plc_internal_rule/*.c` bằng `gcc -std=c11`
cùng test dùng `tag_write`/`rule_scan`/`rule_table_commit` (DI1 rise → DO2
set; guard trên `TAG_DI0` chặn đúng). Cần include path `utils/logger` (hoặc
1 stub `logger.h` định nghĩa `log_info/warn/debug/error`) vì `plc_rule.c`
dùng logger. Kỳ vọng: `sizeof(SPLC_RuleRecord)==32`, `TAG_DI0==0`, test pass.

**`plc_modbus_cfg.c` + nanoMODBUS**: link `plc_modbus_cfg.c`,
`libs/nanomodbus/nanomodbus.c` và Layer 2 với 1 `modbus_transport_t` giả
(`read` trả byte từ buffer, `write` ghi vào buffer, `unit_id = 1`), cần stub
`sx_time.h` (`sx_get_tick_ms`) và `logger.h`. Nên kiểm tra 4 ca:
(1) request đúng unit → có phản hồi; (2) request sai unit → im lặng, không
crash; (3) không có dữ liệu → không crash; (4) `unit_id = 0` → init báo
lỗi và server bị tắt (không gọi `nmbs_server_poll`). Test ca (3) với bản
cũ từng segfault đúng như HardFault trên board.

**Nạp rule đầu-cuối (App thật ↔ firmware PC):** build 1 chương trình C
`#include` thẳng `plc_modbus_cfg.c` (để thấy các `static`), transport giả
đọc/ghi hex từng dòng qua stdin/stdout, stub `logger.h` in ra **stderr**.
Chạy `test_plc.py` với 1 `Client` giả (`read_holding_registers`,
`write_register`, `write_registers`) nói chuyện RTU thật với chương trình
đó. Kỳ vọng: `CONFIG_STATUS=3 (READY)`, `ACTIVE_RULE_TABLE` đọc lại khớp
`staged`, `ACTIVE_RULE_CRC16` == CRC App tính. Nên thử thêm ca số âm /
32-bit lớn / guard có bit NEGATE để bắt lỗi thứ tự word/byte, và ca
`rule_count > 10` (log liệt kê phải dừng ở 10 dòng).

**Ranh giới layer:** `nm -u plc_modbus_cfg.o` không được có symbol
`sx_usb_*`; nếu xuất hiện là ranh giới Layer 3/3.5 bị vỡ.