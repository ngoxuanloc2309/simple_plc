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

- Branch `ruleflash`. Board: **Zigbee-IO SKU** (`board/board_zigbee_io.c`),
  4 DI / 4 DO / 0 AI, STM32H523CCU6.
- **Rule Engine chạy đúng trên board thật, App thật (SimplePLC.Studio) đã
  nạp và chạy đúng N rule (đã test tới 4), kể cả live watch.** Cả 2 bug
  chặn việc này đều đã tìm ra và sửa xong (mục 1, mục 1b) — không còn bug
  đã biết nào ở đường nạp rule qua App.
- **Rule Table lưu Flash (2-sector A/B): ĐÃ XONG**, verify thật trên board
  (log `PLC_RULE_FLASH : rule table saved to Flash A+B (seq_num=N)`, N
  tăng dần, `CONFIG_ERROR_CODE` luôn 0). Thiết kế đầy đủ: mục 1.1.
- **`SPLC_SYSTEM_CMD_REBOOT`: ĐÃ XONG** (mục 1c) — nút Reboot Device trên
  App giờ thực sự reset MCU, không chỉ trả `ACCEPTED` suông như trước.
  **`FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` vẫn CHƯA implement** —
  đang chờ người dùng chốt phạm vi "factory default" nghĩa là gì cho SKU
  này (mục 3.2 có câu hỏi cụ thể cần trả lời trước khi code).
- Việc treo cũ, chưa quay lại: test tay `--manual` của `test_rule.py`
  (`m_basic`, `m_dwell`, `m_guard`, `m_wiring`) và
  `test_rule_manual_simple.py` — người dùng nói đã chạy nhưng log MCU bị
  trôi, chưa có bằng chứng bằng số. Xin lại log khi rảnh.

## 1. Rule Table trên Flash (2-sector A/B) — ĐÃ XONG

### 1.1 Thiết kế đã chốt (đừng đổi mà không hỏi lại)

- 2 sector A (chạy) + B (sao lưu), lấy từ vùng Retain cũ (Retain còn lại
  3 sector thay vì 4). Cơ chế: commit mới → copy A→B (backup) → clear+ghi
  A → đọc lại CRC A → đúng thì đồng bộ B=A, sai thì phục hồi copy B→A.
  Tại mọi bước, luôn có ≥1 sector giữ bản hợp lệ kể cả mất điện giữa
  chừng — bảng lần bước chi tiết xem code `plc_rule_flash.c`'s comment.
- **Ghi Flash NGAY trong `write_commit_command()`** (đồng bộ, không tách
  lệnh riêng) — khớp đúng spec gốc (`SimplePLC_RuleStruct_MCU_Spec_v0.1.md`
  mục 5.2 bước 4: atomic-swap RAM + lưu Flash là CÙNG một bước).
- Ghi Flash lỗi nhưng RAM đổi đúng: `CONFIG_STATUS = READY` +
  `CONFIG_ERROR_CODE = SPLC_ERROR_FLASH` (không dùng `ERROR`, App sẽ hiểu
  nhầm rule không chạy).
- CRC Rule Table (Modbus lẫn Flash) PHẢI dùng `rule_table_wire_crc16()`
  (byte wire, không phải byte RAM struct) — xem mục 4.2, tốn 3 vòng debug
  trước khi chốt.
- File: `services/plc_rule_flash/plc_rule_flash.{c,h}` (Layer 3, ngang
  hàng `plc_retain/`, không gộp chung).

### 1.2 Bugfix: Retain Flash ghi hỏng sau ~5 phút chạy liên tục — ĐÃ SỬA

Nguyên nhân: `SPLC_RETAIN_RECORD_SIZE` (200 byte) không align 16 byte
(STM32H5 flash quad-word). Sửa: làm tròn size lên 208 byte. Xác nhận
bằng đọc Flash thật qua STM32CubeProgrammer (SWD, không qua ICACHE)
trước khi sửa.

## 1c. `SPLC_SYSTEM_CMD_REBOOT` — ĐÃ XONG

**Vấn đề:** `write_system_command()` (`plc_modbus_cfg.c`) chỉ từng decode
lệnh + set `SYSTEM_COMMAND_RESULT.status = ACCEPTED` — không có Layer 4
nào thực thi hành động thật (`NVIC_SystemReset()`...). Nút "Reboot" trên
App bấm không có tác dụng, dù App nhận `ACCEPTED` OK. Đây không phải bug
mới — chính code đã tự ghi TODO này từ trước; giờ mới bị lộ ra khi App
test tính năng.

**Đã sửa — file mới:**
- `components/system/sx_system.h` — Layer 1 contract, `sx_system_reset()`.
- `platforms/stm32/stm32h5/system/stm32h5_system.{h,c}` — Layer 0, gọi
  `NVIC_SystemReset()` thật.
- `app/plc_app/plc_system_cmd_service.{h,c}` — Layer 4, "chất keo" dịch
  lệnh Modbus (Layer 3) thành hành động thật (Layer 0/1). Gọi từ
  `plc_engine.c`'s `scan_cycle()`, **cuối cùng** trong scan cycle (sau
  `modbus_config_service()`), vì nó có thể gọi `sx_system_reset()`
  (không return) — mọi việc khác trong cycle đó phải xong trước.

**Sửa:**
- `plc_modbus_cfg.{c,h}`: thêm `plc_modbus_cfg_get_pending_system_command()`
  (Layer 3→4 hand-off, consume-once — trả lệnh 1 lần duy nhất, tránh
  reboot lặp lại nếu gọi nhiều lần) và
  `plc_modbus_cfg_set_system_command_result()` (Layer 4 cập nhật
  DONE/ERROR sau khi xử lý — hiện REBOOT không dùng tới, vì reboot thành
  công thì không bao giờ "quay lại" để báo DONE).
- `plc_engine.{c,h}`: gọi `plc_system_cmd_service()` cuối `scan_cycle()`.
- 3 file `CMakeLists.txt` (`app/`, `components/`, `platforms/stm32/stm32h5/`):
  đăng ký file mới.

**Điểm kỹ thuật quan trọng — ĐỪNG bỏ khi sửa lại:** Reboot **không xảy ra
ngay lập tức** khi nhận lệnh. `plc_system_cmd_service.c` trì hoãn
300ms (30 scan cycle, `PLC_REBOOT_DELAY_MS`) trước khi thật sự gọi
`sx_system_reset()`. Lý do: phản hồi FC06 "ACCEPTED" chỉ được QUEUE vào
`txQueue`, không có gì đảm bảo đã truyền hết qua USB ngay lúc đó — nếu
reset ngay lập tức, App luôn thấy timeout dù MCU làm đúng (cùng họ
triệu chứng với bug USB timing ở mục 1b cũ, dù nguyên nhân khác).

**Chưa làm — cần người dùng chốt trước khi code:**
`FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` vẫn chỉ trả `ACCEPTED` suông
(giống REBOOT trước khi sửa). Không code liều vì cả 3 đều xoá dữ liệu
thật (Flash erase) — xem câu hỏi cụ thể ở mục 3.2.

## 1b. Bug đã sửa: App chỉ nạp được 1 rule, ≥2 rule bị lỗi/timeout

Đây là bug lớn nhất đã tốn nhiều vòng điều tra trong phiên trước — ghi
lại đầy đủ để không lặp lại sai lầm nếu triệu chứng tương tự tái diễn.

**Triệu chứng:** `test_plc.py`/`test_rule.py`/`test_plc_4rules.py` (script
Python) luôn nạp N rule thành công. App .NET thật (SimplePLC.Studio) chỉ
nạp được **đúng 1 rule**; từ 2 rule trở lên App báo
`Lỗi ngoại lệ: The operation has timed out` ngay sau bước ghi Staging
buffer, MCU log dừng đúng sau dòng `rule_count_staged=N, status ->
RECEIVING` (không có gì tiếp theo).

**Nguyên nhân gốc (đã xác nhận bằng cách đọc code App thật, repo
`ngoxuanloc2309/paa`, không phải đoán):** App's
`ModbusChunkPlanner.DefaultMaxChunkSize = 64` — đơn vị là REGISTER, không
phải byte. 64 register = 128 byte data, cộng header+CRC ra tới ~137
byte/frame Modbus RTU khi chunk đầy — vượt xa 1 gói USB CDC Full-Speed
(`CFG_TUD_CDC_RX_BUFSIZE = 64 byte`, `port/usb/tusb_config.h`). Với 1
rule (16 register, tổng frame 41 byte) luôn vừa 1 gói USB → không bao
giờ lộ bug. Từ 2 rule (32 register, 73 byte) trở lên cần ≥2 gói USB →
firmware (`nmbs_set_byte_timeout(0)`, non-blocking poll theo đúng thiết
kế "never blocks scan budget") không có cơ chế đợi phần còn thiếu của
cùng 1 frame qua nhiều lần poll → request coi như mất → App timeout sau
đúng 1000ms (`UsbCdcTransport`'s `ReadTimeout` mặc định).

**Đã sửa (2 phía, cả 2 đều nên giữ):**
- **App (đã sửa, xác nhận hoạt động):** giảm `DefaultMaxChunkSize` từ 64
  xuống 16 register/chunk (đúng 1 rule/chunk) — đây là fix chính, đã xác
  nhận qua test thật trên board.
- **Firmware (đã làm, vẫn nên giữ dù không phải nguyên nhân chính của
  bug này):** `plc_modbus_cfg.c`'s `nmbs_set_byte_timeout()` đổi từ `0`
  sang `MODBUS_BYTE_TIMEOUT_MS = 5` — chỉ áp dụng cho byte SAU byte đầu
  tiên của 1 request đang đến (không phải lúc rảnh chờ request mới, vẫn
  giữ `read_timeout_ms = 0`), nên không phá "never blocks" khi App
  rảnh/mất kết nối. Đây là cải thiện độ bền cho các trường hợp biên
  tương lai (App khác, hoặc chunk size lại bị nới ra), không phải fix
  của chính bug 1-rule-only này.

**Cảnh báo cho tương lai:** giới hạn thực tế an toàn của 1 request FC16
ghi `STAGING_RULE_TABLE` là **≤41 byte tổng (16 register = 1 rule)**, do
CDC Full-Speed 64-byte packet. Nếu ai đó (App hoặc firmware) sau này lại
nới kích thước chunk lên, bug này tái hiện y hệt.

## 1d. Bug đã sửa: guard_tag=0 khiến rule bị chặn sai (App)

**Triệu chứng:** nạp 4 rule (DI0→DO0, DI1→DO1, DI2→DO2, DI3→DO3) qua App,
chỉ DI0→DO0 chạy đúng; kích DI1/DI2/DI3 không có phản ứng gì.

**Nguyên nhân (repo App, `RuleMapper.cs`):** App mã hoá "rule không có
guard" bằng `GuardTag = 0`. Nhưng theo tag layout v1.9 (đã chốt trong
`architecture.md` mục 2.2), `0` là `TAG_DI0` thật, không phải sentinel
trống — sentinel đúng là `GUARD_TAG_NONE = 0x7FFF`. Hệ quả: MỌI rule
"không guard" từ App đều bị firmware hiểu nhầm thành "guard theo DI0" —
chỉ rule nào tình cờ có `trigger_tag == DI0` mới "vô tình" chạy đúng vì
trigger và guard trùng nhau.

**Đã sửa (App, repo `ngoxuanloc2309/paa`):**
- `ModbusRegisterMap.cs`: thêm hằng `GuardTagNone = 0x7FFF`.
- `RuleMapper.cs`: `ToDto()` gán `GuardTagNone` thay vì `0` khi không có
  guard; `ToDomain()` đổi điều kiện `GuardTagIndex > 0` thành
  `GuardTagIndex != GuardTagNone` (điều kiện cũ hiểu nhầm guard=DI0 thật
  thành "không có guard" — bug đối xứng ở chiều đọc).

**Lưu ý:** rule nào đã lỡ nạp+lưu Flash với `guard_tag=0` sai TRƯỚC khi
sửa vẫn còn sai trong Flash A/B cho tới khi nạp lại bằng App bản đã sửa.

## 2. Kiến trúc trong 30 giây

7 layer, include một chiều từ trên xuống, không heap. Chi tiết:
`docs/architecture.md`.

```
Layer 4    app/, board/        plc_engine, plc_system_cmd_service, board_<sku>.c
Layer 3    services/           plc_io, plc_retain, plc_modbus_cfg, plc_rule_flash
Layer 3.5  port/               modbus_transport_t, modbus_usb
Layer 2    core/               tag table + rule engine (build được trên PC)
Layer 1    components/         driver contract (gpio/adc/flash/uart/usb_cdc/system)
Layer 0    platforms/stm32/stm32h5/   HAL thật
Layer U    utils/, libs/       cqueue, logger, filter, nanoMODBUS, TinyUSB
```

Vòng quét: `plc_engine_poll()` (không chặn, tự canh nhịp
`PLC_SCAN_INTERVAL_MS = 10ms`) gọi `scan_cycle(now)`:
`input_scan → rule_scan(now) → output_scan → modbus_config_service →
retain_service → plc_system_cmd_service`.

Điểm cần nhớ khi đọc code:
- Layout tag v1.9: **không có sentinel ở index 0**, `TAG_DI0 == 0`.
  "Không có guard" = `GUARD_TAG_NONE` (`0x7FFF`), KHÔNG phải 0 — đã có 1
  bug thật ở App vì hiểu sai điều này (mục 1d), kiểm tra kỹ nếu sửa gì
  liên quan guard/tag index ở cả 2 phía App và firmware.
- `SPLC_RuleRecord` = 32 byte. CRC của Rule Table (Modbus lẫn Flash)
  PHẢI tính trên byte **wire** (`rule_table_wire_crc16()`), KHÔNG phải
  byte RAM của struct — xem mục 4.2.
- 1 request Modbus FC16 ghi `STAGING_RULE_TABLE` an toàn tối đa **16
  register (1 rule, 41 byte)** — giới hạn thực tế do USB CDC Full-Speed
  64-byte packet + firmware poll non-blocking (mục 1b). App/firmware nào
  sau này đổi cách chia chunk phải nhớ giới hạn này.
- `plc_device.h`, `plc_error.h`, `plc_system_cmd.h` cố ý chỉ có `.h`
  (không thêm `.c` vào Layer 2).
- Flash map: Rule Table A = sector #31, B = #30, Retain = #27-29.

## 3. Bug đã biết, CHƯA sửa

### 3.1 `modbus_usb_write()` bỏ qua `timeout_ms`

`sx_usb_tiny_write()` luôn block tới khi ghi xong. Nếu USB nghẽn có thể
vượt ngân sách 10 ms của vòng quét. `test_rule.py`'s `t_load` (100 rule,
stress 5s) từng đo `scan_time_ms` lên tới 83ms dưới tải nặng — nghi do
đây. Cần quyết định: làm write non-blocking thật, hay chấp nhận.

### 3.2 `FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` — CHƯA thực thi

`REBOOT` đã xong (mục 1c). 3 lệnh còn lại vẫn chỉ trả `ACCEPTED` suông,
chưa Flash-erase gì thật. **Câu hỏi cụ thể cần người dùng trả lời trước
khi code** (vì spec `v1.7`/`v1.9` chỉ ghi mơ hồ "khôi phục mặc định theo
policy sản phẩm", không định nghĩa chi tiết):

- `FACTORY_RESET` gồm những gì: chỉ Rule Table + Retain (giống
  `CLEAR_RULES` + `CLEAR_RETAIN` cộng lại), hay còn xoá cả tag
  config/network settings khác?
- Sau khi xoá xong, có tự động reboot luôn không, hay chờ App gửi
  `REBOOT` riêng?
- `CLEAR_RULES`/`CLEAR_RETAIN` khi đứng riêng (không phải qua
  `FACTORY_RESET`) có cần reboot theo sau không, hay chỉ cần xoá
  Flash + reset RAM state là đủ (tương tự `rule_table_commit()` với
  `rule_count=0`)?

### 3.3 Các việc chưa thực thi / chưa validate khác

- Chưa validate `guard_tag`/`trigger_tag`/`action_tag` nằm trong
  `0..MAX_TAGS-1` (hoặc `GUARD_TAG_NONE`). Index sai không crash nhưng
  rule bị vô hiệu hóa âm thầm.
- **PVD → ghi Retain khẩn cấp chưa được nối.** Có sẵn: PVD bật trong
  CubeMX, `HAL_PWR_PVDCallback()`, `sx_power_register_low_voltage_
  callback()`, `retain_snapshot_write()`. Thiếu: không ai gọi
  `sx_power_register_low_voltage_callback(retain_snapshot_write)`.
- `TRG_TIME_WINDOW`: `now_hhmm` hardcode 0, chưa có nguồn RTC.
- `ACT_WRITE_REMOTE` / `ACT_LOG_EVENT` / `ACT_SEND_ALARM`: chưa có
  implementation.

### 3.4 Tồn tại trong repo nhưng KHÔNG PHẢI việc của Claude sửa

`STM32H523xx_FLASH.ld` khai `LENGTH = 512K`, chip thật chỉ 256KB. File
CubeMX tự sinh — nếu sửa thì phải qua CubeMX/`.ioc`, không sửa tay. Chỉ
báo người dùng.

## 4. Bài học đã rút ra (quan trọng, đọc kỹ)

### 4.1 Luôn đọc file định nghĩa trước khi thêm gì mới

Trước khi thêm hằng số/enum mới, đọc file gốc trước. Đã có bài học thật
2 lần: (1) suýt tạo mã lỗi Flash trùng ý nghĩa với `SPLC_ERROR_FLASH` có
sẵn; (2) suýt bỏ sót `GuardTagIndexMask` đã tồn tại khi thêm
`GuardTagNone` ở App. Áp dụng chung: mã lỗi, hằng số tag, địa chỉ Modbus
— kiểm tra tồn tại trước khi định nghĩa mới, ở CẢ HAI phía App và
firmware (chúng là 2 repo riêng, dễ quên kiểm tra phía kia).

### 4.2 CRC của Rule Table PHẢI tính trên byte wire, không phải byte RAM struct

Tốn 3 vòng debug trên board mới lộ (commit `9789b3a` trở đi). Chuỗi bug
gốc: (1) FC06 tới thanh ghi RW bị từ chối do bảng dispatch đặt callback
sai cột; (2) CRC tính trên byte RAM little-endian của struct — sai cả ý
nghĩa (phụ thuộc chip) lẫn giá trị (`nmbs_crc_calc` trả CRC hoán byte);
(3) `test_plc.py` cũ cũng tính CRC kiểu RAM, khớp firmware cũ nhưng sai
spec. **Quy ước chốt theo spec 8.4:** CRC tính trên `rule_count x 32
byte` wire — mỗi register high byte trước, mỗi trường 32-bit High Word
rồi Low Word. Firmware: `rule_record_to_wire()` + `rule_table_wire_
crc16()` trong `plc_modbus_cfg.c` (KHÔNG dùng `nmbs_crc_calc` cho việc
này — nó trả CRC hoán byte, dùng cho RTU framing).

### 4.3 Con số trong log là bằng chứng định danh phiên bản code, không phải để đoán

Khi bế tắc, in cả các giả thuyết cạnh nhau thay vì suy diễn từ một con số
duy nhất. Đã từng nghi nhầm "board chạy ELF cũ" trong khi thực ra 2 module
(App/firmware) đang dùng 2 công thức CRC khác nhau.

### 4.4 Mô phỏng đầu-cuối trên PC trước khi lên board

Build 1 chương trình C `#include` thẳng file `.c` cần test (để thấy cả
`static`), transport/flash giả qua RAM hoặc stdin/stdout, chạy chính
script Python thật (không viết lại test) nói chuyện với chương trình đó.
Bắt được lỗi mà test từng phía riêng lẻ bỏ sót. Lưu ý stub `logger.h`
phải in ra `stderr`, không phải `stdout`, nếu protocol test đọc dữ liệu
qua `stdout`.

### 4.5 Log "không thấy" thường do bị trôi, không phải do không chạy

Vòng poll nhanh (10 lần/giây) sinh nhiều dòng DEBUG che mất dòng quan
trọng. Trước khi kết luận "không log ra", thử lọc theo từ khóa hoặc rút
ngắn thời gian chạy trước khi nghi ngờ code không chạy tới đó.

### 4.6 Quyết định kiến trúc: diễn giải lại bằng lời của mình để người dùng xác nhận

Khi người dùng mô tả 1 cơ chế phức tạp, diễn giải lại thành đoạn văn rõ
ràng, đầy đủ từng bước và hỏi "đúng ý bạn không" trước khi code — rẻ hơn
nhiều so với code sai rồi sửa lại.

### 4.7 Khi debug bug xuyên 2 hệ thống (App + Firmware), đọc CẢ HAI repo thật

Bug ở mục 1b (chunk size) và 1d (guard_tag) đều nằm ở phía App, không
phải firmware — nhiều vòng đoán mò dựa trên log firmware một mình đều
sai hướng, chỉ giải quyết được khi đọc trực tiếp code App
(`ngoxuanloc2309/paa`). Khi triệu chứng liên quan tới giao tiếp App↔MCU
mà log firmware "trông đúng", luôn hỏi/đọc code phía App trước khi tiếp
tục đoán ở phía firmware.

### 4.8 Đừng code liều những hành động phá huỷ dữ liệu khi spec mơ hồ

`FACTORY_RESET`/`CLEAR_RULES`/`CLEAR_RETAIN` (mục 3.2) bị hoãn lại có
chủ đích, không phải quên: spec chỉ ghi mơ hồ, code sai phạm vi sẽ xoá
nhầm hoặc thiếu dữ liệu thật trên board đã lắp đặt. So với `REBOOT` (chỉ
1 cách hiểu, an toàn để code ngay), bất cứ lệnh nào Flash-erase dữ liệu
NÊN hỏi xác nhận phạm vi cụ thể trước, không suy đoán "chắc ý họ là...".

## 5. Quy trình làm việc với người dùng

- Trao đổi **tiếng Việt**, code/comment **tiếng Anh**.
- Người dùng tự push; Claude không có quyền push. Bắt đầu phiên hoặc khi
  người dùng báo "đã push": `git pull`, rồi **build/compile verify thật**
  (không chỉ đọc diff).
- **2 repo liên quan:** firmware (`ngoxuanloc2309/simple_plc`, branch
  `ruleflash`) và App (`ngoxuanloc2309/paa`, .NET). Bug giao tiếp
  App↔MCU có thể nằm ở BẤT KỲ phía nào — đọc cả 2 khi log firmware
  "trông đúng" nhưng App vẫn lỗi (mục 4.7).
- **Giao file:** người dùng muốn nhận **nguyên file** (present từng file
  để copy-paste cả file), KHÔNG muốn patch/zip/đoạn thay thế.
- Quyết định kiến trúc lớn: hỏi bằng `ask_user_input_v0`, **tách từng
  quyết định nhỏ**, không gộp nhiều câu vào một. Nếu cơ chế phức tạp,
  diễn giải lại bằng lời trước khi code (mục 4.6).
- **Không tự sửa file CubeMX tự sinh** (ghi "Auto-generated") — kể cả
  khi thấy bug thật. Chỉ báo người dùng.
- Khi đề xuất nguyên nhân, nói rõ mức chắc chắn; kiểm chứng bằng dữ liệu
  thật (log, test PC, đọc code thật) trước khi khẳng định — đừng dừng ở
  giả thuyết đầu tiên nghe hợp lý (mục 4.7's bài học).
- Trước khi thêm hằng số/mã lỗi/enum mới, đọc file định nghĩa gốc trước
  (mục 4.1), ở cả 2 phía App/firmware nếu liên quan giao tiếp Modbus.
- **Đừng implement hành động phá huỷ dữ liệu (Flash erase, factory
  reset...) khi spec mơ hồ về phạm vi** — hỏi xác nhận cụ thể trước
  (mục 4.8).

## 6. Lệnh verify nhanh (không cần toolchain ARM)

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
(App hay firmware) khi log firmware một mình không đủ (mục 4.7).

**Ranh giới layer:** `nm -u <file>.o` không được có symbol `sx_usb_*`
với Layer 3 (`plc_modbus_cfg.c`), hay `sx_flash_*`/`sx_usb_*` với Layer 2
(`plc_rule.c`, `plc_tag.c`). `plc_rule_flash.c` PHẢI có `sx_flash_*`
(đúng, Layer 3), KHÔNG được ở Layer 2. `plc_system_cmd_service.c`
(Layer 4) PHẢI có `sx_system_reset` — không được gọi thẳng từ
`plc_modbus_cfg.c` (Layer 3, transport/protocol-agnostic theo thiết kế).