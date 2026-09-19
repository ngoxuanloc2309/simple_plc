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

- Branch `main`, commit đã verify: `689edc3` ("fix code").
- Board đang dùng: **Zigbee-IO SKU** (`board/board_zigbee_io.c`), 4 DI /
  4 DO / 0 AI, STM32H523CCU6.
- **Đã chạy được trên board thật:** build (`build.bat build`), flash, USB
  enum + mount, Modbus server khởi tạo (`init OK ... unit_id=1`).
- **Chưa xác nhận trên board thật:** đọc/ghi Modbus bằng App
  (`test_plc.py`), nạp rule, rule chạy thật. Đây là bước tiếp theo hợp lý
  nhất (xem mục 2).

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

1. **Chạy `test_plc.py --unit 1` trên board thật** — đọc device
   descriptor, stage + commit 1 rule, xem DI0→DO0. Đây là lần đầu toàn bộ
   đường Modbus chạy thật; mọi bug bên dưới có thể lộ ra ở bước này.
2. **Sửa 2 bug nền tảng đã biết** (mục 3.1, 3.2) — cả hai chưa gây lỗi rõ
   ràng nhưng ảnh hưởng độ tin cậy của Modbus và Rule Engine.
3. **Chốt các quyết định mở** (mục 4) trước khi viết thêm tính năng.

## 3. Bug đã biết, CHƯA sửa

### 3.1 `s_rule_scan_now_ms` luôn = 0 → dwell/interval không hoạt động

`core/plc_rule/plc_rule.c`: `rule_scan()` truyền `s_rule_scan_now_ms`
(static, không ai cập nhật, luôn 0) vào state machine. Hệ quả: rule có
`for_ms > 0` (dwell) và `SPLC_TRG_INTERVAL` **không chạy đúng**. Rule
đơn giản (edge, không dwell) vẫn chạy bình thường.
Cần quyết định cách truyền tick từ Layer 4 (mục 4, câu 1).

### 3.2 `SCAN_INTERVAL_MS` = `0U` trong `Core/Src/main.c`

Vòng quét chạy hết tốc độ, không có nhịp 10 ms như `plc_engine.h` và
Readme mô tả. Chưa rõ có chủ ý (đang debug) hay quên đặt lại. Xác nhận
với người dùng trước khi đổi. Lưu ý: file nằm trong vùng `USER CODE` của
CubeMX nên sửa an toàn, nhưng vẫn hỏi trước.

### 3.3 `sx_usb_tiny_read()` — `_timeoutMS` đếm vòng lặp, không phải ms

`components/usb_cdc/sx_usb_cdc.c`. Với `timeout = 0` (cách
`plc_modbus_cfg` đang dùng) thì không sao vì thoát ngay. Chỉ thành vấn đề
nếu sau này dùng timeout > 0.

### 3.4 `modbus_usb_write()` bỏ qua `timeout_ms`

`sx_usb_tiny_write()` luôn block tới khi ghi xong. Nếu USB nghẽn có thể
vượt ngân sách 10 ms của vòng quét. Cần quyết định: làm write non-blocking
thật, hay chấp nhận (App luôn đọc kịp).

### 3.5 Các việc chưa thực thi / chưa validate

- `write_system_command()` (`plc_modbus_cfg.c`) mới decode + đổi status
  sang ACCEPTED, **chưa thực thi** reboot / factory reset / clear rules /
  clear retain. Việc thực thi thuộc Layer 3/4, chưa viết.
- `rule_table_commit()` chưa validate `guard_tag`/`trigger_tag`/
  `action_tag` nằm trong `0..MAX_TAGS-1` (hoặc `GUARD_TAG_NONE`). Index
  sai không crash nhưng rule bị vô hiệu hóa âm thầm.
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

## 4. Quyết định kiến trúc CHƯA CHỐT (hỏi lại, đừng tự quyết)

1. **Cách Layer 4 truyền tick ms vào `rule_scan()`** — tham số hay setter.
   (Liên quan trực tiếp bug 3.1.)
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

**Ranh giới layer:** `nm -u plc_modbus_cfg.o` không được có symbol
`sx_usb_*`; nếu xuất hiện là ranh giới Layer 3/3.5 bị vỡ.