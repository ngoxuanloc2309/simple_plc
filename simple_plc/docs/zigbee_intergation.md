# SimplePLC — Cổng Zigbee: hành vi MCU hiện tại (dành cho team App)

> **Mục đích:** mô tả CHÍNH XÁC những gì firmware làm ở thời điểm này với khối Zigbee `0x1700..0x173F`,
> để App (SimplePLC.Studio) code đúng theo. Tài liệu viết từ **code thật** (`services/plc_zigbee`,
> `services/plc_modbus_cfg`, `services/plc_mbs`, `services/plc_mbm`, port `Zigbee_IO/port/`), không phải
> từ spec. Khi tài liệu này khác `SPLC-SPEC-ZIGBEE-006 v0.2`, **tin tài liệu này** (và báo lại để firmware sửa spec).
>
> **Phạm vi:** hành vi của thư viện `simple_plc` (Wire Profile V2.0). Phần việc của **port** (đẩy cấu hình xuống module radio,
> báo trạng thái mạng) được tách riêng ở mục 8 và 9. Mỗi điều khoản ghi `[ĐÃ CHỐT với App]`, `[Firmware tự chọn]`
> hoặc `[Cần App xác nhận]`.

---

## 1. Zigbee là gì trong hệ thống này

- Zigbee là **đường truyền Modbus không dây**, KHÔNG phải đường nạp rule. DTU/Coordinator là **Master**, các board
  IO (Router) là **Slave**. Rule vẫn nạp qua USB / RS-485 / TCP như cũ.
- Zigbee là **Loại giao tiếp riêng** trong Studio, **không phải "cổng RS-485 số 5"**. `0x0A0A` (số cổng RS-485)
  **không tính Zigbee**. `[ĐÃ CHỐT]`
- Khung Modbus qua Zigbee **chỉ RTU** (không ASCII). `[ĐÃ CHỐT]`
- Thư viện chỉ làm **logic + giữ cấu hình**. Việc cấu hình module radio (PAN, kênh, khoá…) là của **port** phía sản phẩm
  (xem mục 9, "Phân chia lib / port").

## 2. Phát hiện tính năng (capability)

Đọc `0x0A08` (32 bit, High Word trước) **trước khi** hiển thị hoặc ghi khối Zigbee:

| Bit | Giá trị | Ý nghĩa |
|---|---|---|
| 15 | `0x00008000` | `SPLC_CAP_ZIGBEE`: firmware có khối `0x1700`. Chỉ có khi build bật Zigbee **và** port Zigbee đã đăng ký lúc khởi động. |
| 14 | `0x00004000` | `MODBUS_SLAVE`: thêm vào khi port hỗ trợ vai Slave (và build phục vụ được Slave). |
| 8 | `0x00000100` | `MODBUS_MASTER`: do khối MB_READ khai (`MBM=1`), **không** khai thay Zigbee. |

**Quy tắc App:** không có bit `0x8000` ⇒ **không ghi `0x1700`**, ẩn mục Zigbee trong Studio.
Bit `0x8000` có nhưng `0x4000` không ⇒ không cho chọn vai Slave (COMMIT sẽ bị từ chối, xem mục 5).

## 3. Bản đồ thanh ghi `0x1700..0x173F` (64 thanh ghi, big-endian)

Truy cập bằng **FC03 (đọc)** và **FC06 / FC16 (ghi)**. Không dùng coil. Offset tính từ `0x1700`.

| Offset | Tên | R/W | Mô tả |
|---|---|---|---|
| `+0` | `CONTROL_FLAGS` | R/W | bit0 `ENABLE`; bit1 `ROLE` (0 = Master, 1 = Slave); bit2 `FRAMING` (**phải = 0**, RTU); bit3 `SECURITY_ENABLE`; bit4 `ANTENNA` (0 = PCB, 1 = ngoài/SMA); bit5 `PRESERVE_KEY` (**cờ lệnh**, không lưu, đọc lại luôn 0); bit6..15 **phải = 0** |
| `+1` | `SLAVE_ID` | R/W | 1..247; chỉ kiểm khi `ENABLE=1` **và** `ROLE=Slave` |
| `+2` | `PAN_ID` | R/W | lưu nguyên, **firmware không kiểm** |
| `+3` | `CHANNEL` | R/W | lưu nguyên, **firmware không kiểm** |
| `+4` | RESERVED | R/W | lưu và đọc lại đúng như ghi, không kiểm (Studio không ghi số cổng vào đây) |
| `+5..+12` | `NET_KEY` | **W-only** | 16 byte, **byte cao trước** (reg `+5` = byte 0 ở bit 15..8, byte 1 ở bit 7..0, …). **Đọc luôn trả 0.** |
| `+13..+31` | RESERVED | R/W | lưu và đọc lại như ghi |
| `+32` | `LINK_STATE` | RO | do port báo (xem mục 8) |
| `+33` | `ERROR_CODE` | RO | do port tự định nghĩa; 0 = không lỗi |
| `+34` | `NODE_COUNT` | RO | số nút thấy được (Coordinator); 0 = không biết |
| `+35` | `RSSI` | RO | `int16` dBm; 0 = không có thông tin |
| `+36` | `STATUS_FLAGS` | RO | bit0 `KEY_SET` = đang lưu một khoá khác 0. Các bit khác = 0. |
| `+37..+63` | — | RO | luôn 0 |

Chi tiết cần nhớ:
- **Ghi vào `+32..+63`:** firmware **ACK và bỏ qua** (không lỗi).
- **Ghi vượt quá `0x173F`** (ví dụ FC16 bắt đầu `0x173F` quantity 2): Modbus exception `0x02`, **không thay đổi gì**.
- **Đọc** luôn trả **cấu hình ĐANG CHẠY**, không bao giờ trả nháp (kể cả khi vừa ghi xong, chưa COMMIT).
- `PRESERVE_KEY` không bao giờ đọc lại được; `KEY_SET` (`+36` bit0) là cách duy nhất biết "đã có khoá".

## 4. Quy trình nạp: nháp + COMMIT (giống khối MQTT)

1. Ghi khối vào `0x1700` (FC16/FC06). Mọi ghi vào `+0..+31` đi vào **NHÁP**.
   Lần ghi đầu tiên tạo nháp bằng cách **sao từ cấu hình đang chạy**, nên **ghi từng phần được** (ví dụ chỉ `+1` SLAVE_ID)
   — ngoại lệ duy nhất là **khoá**, xem mục 6.
2. Ghi phần còn lại của Deploy như thường lệ (nạp rule, các khối ngoại vi, CRC `0x9003`…).
3. `COMMIT`: ghi `0xA5A5` vào `0xA000`. Firmware **kiểm toàn bộ nháp trước khi áp dụng bất cứ thứ gì**.
4. Đọc `CONFIG_STATUS` (`0x9000`) và `CONFIG_ERROR_CODE` (`0x9001`) để biết kết quả.

**Hành vi khi COMMIT:**
- **Thành công:** nháp trở thành cấu hình đang chạy, **lưu vào Flash** (nằm trong bản ghi Rule Table, 64 byte = 32 thanh ghi `+0..+31`,
  **khoá lưu nguyên văn**, `PRESERVE_KEY` không lưu). Sau reboot cấu hình Zigbee còn nguyên.
- **Thất bại do khối Zigbee:** `CONFIG_STATUS = 4 (ERROR)`, `CONFIG_ERROR_CODE = 2 (INVALID_PARAMETER)` hoặc `7` (mục 5).
  **Không có gì được áp dụng** và firmware **vứt TẤT CẢ nháp** (rule, FB, MQTT, MB_READ/WRITE, Zigbee…), không chỉ nháp Zigbee.
  ⇒ App phải **ghi lại toàn bộ** nếu muốn thử lại, không được chỉ ghi lại phần Zigbee đã sửa.
- **Một Deploy không ghi khối `0x1700`:** cấu hình Zigbee đang chạy **và khoá của nó được giữ nguyên**.
  ⇒ Studio **không cần** ghi khối Zigbee khi người dùng không đổi gì ở Zigbee, và **không cần** `PRESERVE_KEY` trong trường hợp đó.
- **Ghi y hệt cấu hình đang chạy:** không đổi gì (không tăng `generation`, không làm port áp dụng lại).

## 5. COMMIT bị từ chối khi nào (bảng đầy đủ)

| # | Điều kiện (trên NHÁP) | `CONFIG_STATUS` / `ERROR_CODE` |
|---|---|---|
| 1 | `CONTROL_FLAGS` có bit 6..15 khác 0 | 4 / **2** |
| 2 | `FRAMING` (bit 2) = 1 | 4 / **2** |
| 3 | `ENABLE=1`, `ROLE=Slave` mà port **không hỗ trợ vai Slave** (hoặc chưa đăng ký) | 4 / **2** |
| 4 | `ENABLE=1`, `ROLE=Master` mà port **không hỗ trợ vai Master** (hoặc chưa đăng ký) | 4 / **2** |
| 5 | `ENABLE=1`, `ROLE=Slave`, `SLAVE_ID` ngoài 1..247 | 4 / **2** |
| 6 | `SECURITY=1`, `PRESERVE_KEY=1`, **chưa có khoá lưu** | 4 / **2** |
| 7 | `SECURITY=1`, `PRESERVE_KEY=0`, nháp **không chứa khoá mới khác 0** (chưa ghi `+5..+12`, hoặc ghi toàn 0) | 4 / **2** |
| 8 | **Port từ chối** qua hook `validate` (xem mục 9): trên port Zigbee-IO và DTU, `ENABLE=1`, `ROLE=Slave`, `SLAVE_ID` = **236 hoặc 237** | 4 / **2** |
| 9 | Có khối **MB_READ / MB_WRITE đang bật** trỏ vào cổng Zigbee (giá trị thô 4, mục 7) trong khi vai (nháp, hoặc đang chạy nếu không có nháp) là **Slave** | 4 / **7** (`SLAVE_PORT_CONFLICT`) |

Lưu ý:
- Khi `ENABLE=0`, các kiểm tra 3, 4, 5 **không áp dụng**; nhưng kiểm tra 1, 2, 6, 7 (cờ, FRAMING, khoá) **vẫn áp dụng** nếu `SECURITY=1`.
- `PAN_ID`, `CHANNEL`, `ANTENNA`: **không có lỗi nào ở lớp thư viện** và port hiện cũng không kiểm. Dải hợp lệ do **Studio giới hạn trên UI** theo Profile. `[ĐÃ CHỐT]`
- **Slave ID 236 và 237** (module DRF dùng `0xEC / 0xED` làm lệnh riêng): lib không kiểm, nhưng **port Zigbee-IO và DTU từ chối ở COMMIT** (dòng 8). Studio vẫn **nên chặn trên UI** để người dùng không phải chờ COMMIT lỗi. `[ĐÃ CHỐT]`
- Kiểm tra ở dòng 9 dùng khối MB đang bật, nên chuyển Zigbee sang Slave **khi vẫn còn khối MB_READ/MB_WRITE bật trỏ vào Zigbee** cũng bị từ chối.

## 6. Khoá mạng (`NET_KEY`) — quy tắc chính xác

Khoá **chỉ ghi, không bao giờ đọc ra** (đọc `+5..+12` luôn = 0). Nháp **bắt đầu với khoá = 0** (không sao chép khoá đang lưu),
nhờ đó firmware phân biệt được "không ghi khoá" với "ghi lại đúng khoá cũ".

| `SECURITY` | `PRESERVE_KEY` | Nháp có khoá mới khác 0? | Kết quả |
|---|---|---|---|
| 0 | bất kỳ | bất kỳ | OK. **`PRESERVE_KEY` và `NET_KEY` bị BỎ QUA; khoá đang lưu được GIỮ** (không bị ghi đè, không bị xoá). |
| 1 | 1 | bất kỳ (bị bỏ qua) | OK nếu **đã có khoá lưu**; chưa có ⇒ lỗi 2. Khoá đang lưu được giữ. |
| 1 | 0 | có | OK, khoá mới thay khoá cũ. |
| 1 | 0 | không (chưa ghi / toàn 0) | **Lỗi 2.** |

Hệ quả App phải xử lý:
- **Ghi từng phần khi `SECURITY=1`:** ví dụ chỉ đổi `SLAVE_ID` mà nháp mang `SECURITY=1` ⇒ **bắt buộc đặt `PRESERVE_KEY=1` trong cùng COMMIT**,
  nếu không sẽ lỗi 2 (vì nháp không có khoá mới).
- **Không có cách "xoá khoá" bằng khối `0x1700`:** tắt `SECURITY` chỉ ngừng dùng khoá, khoá vẫn nằm trong Flash.
  Chỉ `FACTORY_RESET` mới xoá khoá (mục 10). Ghi khoá toàn 0 với `SECURITY=1` bị từ chối.
- "Có khoá hay chưa" = khoá lưu **khác toàn 0**. UI nên đọc `KEY_SET` (`+36` bit0) để hiển thị "Đã đặt khoá / Chưa đặt khoá".
- **Cảnh báo bảo mật:** khoá lưu **nguyên văn** trong Flash (như mật khẩu broker MQTT), **chưa** phải bảo mật lớp bộ nhớ
  (`SPLC-SPEC-SECURITY-001`). Phía App cũng không nên lưu nguyên văn trong file dự án nếu có thể tránh.

## 7. Zigbee làm Master (MB_READ / MB_WRITE qua Zigbee)

Khi board là Coordinator/DTU, khối MB_READ / MB_WRITE chọn đường Zigbee ở trường **cổng**:
- Giá trị thô của trường cổng cho Zigbee là **`4`** (hiển thị "cổng 5"); giá trị thô 5..15 bị từ chối.
  `[Cần App xác nhận]`: codec App có ghi được giá trị 4 và hiển thị "Zigbee" không; độ rộng bit và dải của trường; giữ trống 5..15.
- Zigbee là **một slot Master thêm** sau các cổng RS-485, **không có bản ghi trong bảng `0x1400`**; trạng thái lấy từ khối `0x1700` đang chạy.
- Khối trên Zigbee chạy khi: `ENABLE=1`, `ROLE=Master`, và port đã đăng ký vai Master kèm đường byte. Thiếu một trong ba
  ⇒ khối báo **`err_code 1`**, không gửi byte nào.
- Khối MB đang bật trỏ vào Zigbee khi vai là Slave ⇒ COMMIT bị từ chối với lỗi **7** (mục 5, dòng 9).
- Tham số của lib `[Firmware tự chọn, đổi được]`: khoảng cách giữa hai khung 50 ms, thử lại 1 lần.
- Response được tách khung **theo độ dài** (kênh trong suốt, không có khoảng lặng 3,5 ký tự), nên response dài (FC03 tới 125 thanh ghi) đến nhiều mảnh vẫn được ghép đúng.

## 8. Báo trạng thái (`+32..+36`)

| Thanh ghi | Nguồn |
|---|---|
| `LINK_STATE` (`+32`) | **Port** báo qua callback `status`. Quy ước giá trị: `0` OFF, `1` JOINING, `2` UP, `3` ERROR. |
| `ERROR_CODE` (`+33`) | **Port** tự định nghĩa (chưa có danh mục chung `[Cần App xác nhận]`); 0 = không lỗi. |
| `NODE_COUNT` (`+34`) | Port báo (Coordinator); 0 = không biết. |
| `RSSI` (`+35`) | Port báo, `int16` dBm; 0 = không có thông tin. |
| `STATUS_FLAGS` (`+36`) | **Lib** tự điền: bit0 `KEY_SET` = đang lưu một khoá khác 0. |

Port không đăng ký callback `status` (hoặc callback trả "không có thông tin") ⇒ lib trả `0` cho `+32..+35`.
Nên Studio hiển thị trạng thái mạng theo các giá trị này, và coi `0` ở `RSSI`/`NODE_COUNT` là "không có thông tin", không phải "tín hiệu 0 dBm" hay "không có nút nào".

## 9. Phân chia lib / port

**Lib làm:** giữ cấu hình, nháp/COMMIT/kiểm tra (mục 4–6), lưu Flash, phục vụ Modbus Slave và Master trên ống byte, khai capability.
Bộ đếm `generation` tăng mỗi khi cấu hình đang chạy **thật sự đổi** (COMMIT có thay đổi, nạp Flash, `FACTORY_RESET`).

**Port (sản phẩm) làm:**
1. **Áp cấu hình xuống module radio** (PAN, kênh, ăng-ten, khoá, vai) mỗi khi `generation` đổi. Việc này có thể mất vài giây và có thể khởi động lại module,
   nên COMMIT thành công nghĩa là cấu hình đã **được lưu**, chưa chắc module đã vào mạng; muốn biết thì xem `LINK_STATE` (mục 8).
2. **Báo trạng thái** (mục 8).
3. **Từ chối cấu hình mà module không chịu được** bằng hook tùy chọn `validate` trong `splc_zigbee_ops_t`. Lib gọi hook ở COMMIT, **sau** các kiểm tra của lib, với bản nháp đã giải mã (kể cả khoá sẽ được lưu); hook trả `false` ⇒ COMMIT bị từ chối (4 / 2), không có gì thay đổi và mọi nháp bị vứt. Port Zigbee-IO và DTU dùng hook này để từ chối **Slave ID 236 / 237** (khi `ENABLE=1`, `ROLE=Slave`). Hook **không** được gọi khi nạp cấu hình từ Flash lúc khởi động; port vẫn báo `LINK_STATE = 3`, `ERROR_CODE = 1` nếu gặp ID đó trong ảnh đã lưu. Một port không đăng ký hook thì lib chỉ kiểm 1..247.
4. Cung cấp **đường byte** tới module (đọc không chặn, ghi). Lib tự đặt kiểu tách khung theo độ dài cho đường này.

## 10. Reset và Flash

| Lệnh | Tác động lên khối Zigbee |
|---|---|
| `CLEAR_RULES` | **Không xoá** cấu hình Zigbee (kể cả khoá). `[Giả định, chờ App xác nhận]` |
| `FACTORY_RESET` | Xoá **toàn bộ**, kể cả khoá: về mặc định (`ENABLE=0`, không khoá, `KEY_SET=0`). |
| Reboot | Cấu hình Zigbee nạp lại từ Flash. |
| Hạ firmware sang bản không có Zigbee | Chưa xác định; coi như phải Deploy lại (giống các khối khác). |

Bản ghi Flash là **một** bản ghi chung của Rule Table (một CRC phủ tất cả các khối); cờ "có khối Zigbee" là bit 8 của trường `rule_count`
(cờ thuộc phần firmware, App không cần tự xử lý, chỉ cần đọc/ghi qua Modbus).

## 11. Board là Slave: MCU phục vụ gì trên ống Zigbee

Khi `0x1700` đang chạy với `ENABLE=1`, `ROLE=Slave`, `SLAVE_ID` 1..247, firmware mở một **server Modbus RTU Slave** trên ống Zigbee
(RTU, khung tách **theo độ dài**) và phục vụ **cùng bản đồ `SPLC-SPEC-SLAVE-001`** như các cổng Slave khác:

- Function code: FC01/05/15 (coil), FC02 (discrete input), FC03/06/16 (holding). Còn lại (kể cả FC04) ⇒ exception `0x01`.
- Holding: `0x0000` DI_WORD (RO), `0x0001` DO_WORD (RW), `0x0002..3` VFLAG_DWORD, `0x0010..17` AI0..3 (float32, RO),
  `0x0020..5F` VREG, `0x0060..9F` VREG_RETAIN, `0x00A0..AF` COUNTER, `0x00F0..FF` SYS_DIAG (RO). Coil `0x0000..0F` DO, `0x0064..83` VFLAG. Discrete `0x0000..0F` DI.
- Exception: `0x02` địa chỉ ngoài bản đồ / lỗ giữa các nhóm / yêu cầu vắt ngang hai vùng; `0x03` quantity hoặc byte count sai / ghi vào thanh ghi RO; `0x06` đang có phiên chẩn đoán giữ tag (ghi bị từ chối).
- Unit ID 0 = broadcast: lệnh ghi được thực hiện, **không trả lời**.
- Khung đủ độ dài + đúng CRC thì trả lời ngay trong cùng vòng scan; đổi `SLAVE_ID` hoặc tắt khối ⇒ server đóng/mở lại.

Chi tiết bản đồ: xem `SPLC_SPEC_SLAVE_001` (ICD) và `services/plc_mbs/plc_mbs.h`.

## 12. Việc App cần làm / không được làm (tóm tắt)

**Nên làm:**
- Đọc `0x0A08` trước; chỉ hiện/ghi Zigbee khi có bit `0x8000`; chỉ cho chọn Slave khi có `0x4000`.
- Khóa `FRAMING` ở RTU (bit2 = 0); luôn ghi bit 6..15 = 0.
- Chặn Slave ID 0, > 247, **236, 237** trên UI (lib kiểm 1..247 khi ENABLE + Slave; 236/237 do port từ chối ở COMMIT).
- Giới hạn `PAN_ID`, `CHANNEL`, `ANTENNA` theo Profile (firmware không kiểm).
- Hiển thị khoá là "đã đặt / chưa đặt" qua `KEY_SET`, **không bao giờ** đọc lại được khoá.
- Khi `SECURITY=1` mà không nhập khoá mới ⇒ đặt `PRESERVE_KEY=1` (nếu `KEY_SET=1`), còn `KEY_SET=0` thì **bắt buộc** nhập khoá.
- Sau COMMIT thất bại: đọc `0x9000/0x9001`, báo lỗi rõ, và **ghi lại tất cả** (mọi nháp đã bị vứt).
- Không ghi khối Zigbee nếu người dùng không đổi gì (khối không ghi ⇒ giữ nguyên cấu hình và khoá).

**Không được làm:**
- Không ghi `0x1700` khi thiếu cờ `0x8000`.
- Không coi `0` ở `RSSI` / `NODE_COUNT` là giá trị đo thật (0 = không có thông tin).
- Không đếm Zigbee vào số cổng RS-485 (`0x0A0A`).
- Không mong `CLEAR_RULES` xoá cấu hình Zigbee (chỉ `FACTORY_RESET`).

## 13. Ví dụ giá trị thanh ghi (không kèm khung Modbus)

Board Slave, Slave ID 5, PAN `0x1234`, kênh 15, bật mã hoá, khoá mới `00 11 22 … EE FF`:

| Offset | Giá trị | Ghi chú |
|---|---|---|
| `+0` | `0x000B` | ENABLE (0x1) + ROLE_SLAVE (0x2) + SECURITY (0x8) |
| `+1` | `0x0005` | SLAVE_ID = 5 |
| `+2` | `0x1234` | PAN_ID |
| `+3` | `0x000F` | CHANNEL = 15 |
| `+4` | `0x0000` | RESERVED |
| `+5..+12` | `0x0011 0x2233 0x4455 0x6677 0x8899 0xAABB 0xCCDD 0xEEFF` | NET_KEY, byte cao trước: `+5 = 0x0011` nghĩa là byte0 = `0x00` (bit 15..8), byte1 = `0x11` (bit 7..0) |

Sau đó đổi riêng Slave ID thành 6, giữ khoá: ghi `+0 = 0x002B` (như trên + PRESERVE_KEY 0x20) và `+1 = 0x0006`. Khối còn lại không cần ghi lại.

## 14. Danh sách việc cần App xác nhận / sửa

1. **Giá trị thô `4` của trường cổng** trong MB_READ/MB_WRITE cho Zigbee, và độ rộng/dải của trường (mục 7). Chặn phần Master.
2. `CLEAR_RULES` có nằm trong quy trình Deploy không; firmware giữ nguyên cấu hình Zigbee khi `CLEAR_RULES` (mục 10).
3. Studio có bỏ qua khối Zigbee khi Upload → Deploy mà không đổi gì không (nếu có thì không cần `PRESERVE_KEY` trong luồng đó).
4. Danh mục `ERROR_CODE` dùng chung giữa các port (hiện mỗi port tự định nghĩa).
5. Xác nhận App chặn Slave ID 236/237 trên UI (firmware cũng từ chối ở COMMIT) và giới hạn PAN/CHANNEL/ANTENNA trên UI (firmware không kiểm).
6. Nội dung Issue #1 ở `SynaptiXCompany/RS485_RF_IO` (firmware chưa đọc repo này).

## 15. Kiểm thử gợi ý cho App (dựa trên hành vi trên)

| # | Thao tác | Kết quả mong đợi |
|---|---|---|
| 1 | Ghi `+0 = 0x0004` (FRAMING=1), COMMIT | `0x9000=4`, `0x9001=2`; đọc lại `+0` vẫn là giá trị cũ |
| 2 | Ghi `+0 = 0x0043` (bit 6 = 1), COMMIT | lỗi 2 |
| 3 | `ENABLE+Slave`, `+1 = 0` hoặc `248`, COMMIT | lỗi 2 |
| 3b | `ENABLE+Slave`, `+1 = 236` hoặc `237`, COMMIT (port Zigbee-IO / DTU) | `0x9000=4`, `0x9001=2`; cấu hình đang chạy không đổi |
| 4 | `ENABLE+Slave`, `+1 = 5`, COMMIT | OK; đọc lại `+1 = 5`; reboot rồi đọc lại vẫn = 5 |
| 5 | `SECURITY=1`, không ghi khoá, không `PRESERVE_KEY`, COMMIT | lỗi 2 |
| 6 | Ghi khoá, `SECURITY=1`, COMMIT | OK; đọc `+36` bit0 = 1; đọc `+5..+12` = 0 |
| 7 | `SECURITY=1` + `PRESERVE_KEY=1` khi **chưa có** khoá | lỗi 2 |
| 8 | Ghi `+1` rồi COMMIT với `SECURITY=0` khi đã có khoá | OK; `+36` bit0 vẫn = 1 (khoá giữ) |
| 9 | Ghi vào `+32..+63` | ACK, không đổi gì; ghi `0x173F` quantity 2 → exception `0x02` |
| 10 | COMMIT lỗi khi đang nạp thêm rule | sau lỗi, rule/FB/MQTT nháp cũng mất; phải ghi lại hết |
| 11 | `FACTORY_RESET` | `+0 = 0`, `KEY_SET = 0` |
| 12 | `CLEAR_RULES` | cấu hình Zigbee và `KEY_SET` giữ nguyên |