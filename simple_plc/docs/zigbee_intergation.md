# SimplePLC — Cổng Zigbee: hành vi MCU hiện tại (dành cho team App)

> **Mục đích:** mô tả CHÍNH XÁC những gì firmware làm ở thời điểm này với khối Zigbee `0x1700..0x173F`,
> để App (SimplePLC.Studio) code đúng theo. Tài liệu viết từ **code thật** (`services/plc_zigbee`,
> `services/plc_modbus_cfg`, `services/plc_mbs`, `services/plc_mbm`, port `Zigbee_IO/port/`), không phải
> từ spec. Bố cục thanh ghi theo **`SPLC_SPEC_ZIGBEE_006_ICD.md` v2.0.0 (đội App, 09/11/2026)**; firmware đã đổi theo ICD đó (bản v0.2 cũ không còn hiệu lực). Khi tài liệu này khác ICD, **tin tài liệu này** (và báo lại để sửa).
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

Truy cập bằng **FC03 (đọc)** và **FC06 / FC16 (ghi)**. Không dùng coil. Offset tính từ `0x1700`. Khớp ICD v2.0.0 mục 3.

| Offset | Tên | R/W | Mô tả / kiểm tra khi `ENABLE=1` |
|---|---|---|---|
| `+0` | `CONTROL_FLAGS` | R/W | bit0 `ENABLE`; bit1 `ROLE` (0 = Master, 1 = Slave); bit2 `FRAMING` (**phải = 0**, RTU); bit3 `SECURITY_ENABLE`; bit4 `ANTENNA` (0 = PCB, 1 = SMA); bit5 `PRESERVE_KEY` (**cờ lệnh**, không lưu, đọc lại luôn 0); bit6..15 **phải = 0** (luôn kiểm) |
| `+1` | `CHANNEL` | R/W | 11..26 |
| `+2` | `PAN_ID` | R/W | `0x0001..0xFFFE` |
| `+3` | `LOCAL_ADDR` | R/W | `0..0xFFFD` |
| `+4` | `DEST_ADDR` | R/W | bất kỳ (`0xFFFF` = broadcast) |
| `+5` | `POWER_LEVEL` | R/W | 0..4 |
| `+6` | `BAUD_RATE_IDX` | R/W | 0..4 (9600, 19200, 38400, 57600, 115200) |
| `+7` | `PARITY` | R/W | 0 None, 1 Even, 2 Odd |
| `+8` | `DATA_BITS` | R/W | cố định 8 |
| `+9` | `STOP_BITS` | R/W | 1 hoặc 2 |
| `+10` | `RESPONSE_TIMEOUT_MS` | R/W | 50..5000 |
| `+11` | `INTER_FRAME_DELAY_MS` | R/W | 10..500 |
| `+12` | `RETRY_COUNT` | R/W | 0..5 |
| `+13` | `SLAVE_ID` | R/W | 1..247, chỉ kiểm khi `ROLE=Slave`; 236/237 do **port** từ chối (mục 5) |
| `+14..+15` | RESERVED | R/W | lưu và đọc lại như ghi, không kiểm |
| `+16..+23` | `NET_KEY` | **W-only** | 16 byte, **byte cao trước** (reg `+16` = byte 0 ở bit 15..8, byte 1 ở bit 7..0, …). **Đọc luôn trả 0.** |
| `+24..+31` | `NETWORK_NAME` | R/W | 16 byte ASCII, byte cao trước; tối đa 15 ký tự in được (0x20..0x7E) + NUL; byte thứ 16 phải là NUL |
| `+32..+35` | RESERVED | RO | ghi bị bỏ qua, đọc 0 (không lưu) |
| `+36` | `STATUS_FLAGS` | RO | bit0 `KEY_SET` = đang lưu một khoá khác 0. Do **lib** điền. |
| `+37` | `LINK_STATE` | RO | do port báo: `0` OFF, `1` JOINING, `2` UP, `3` ERROR |
| `+38` | `RSSI_DBM` | RO | `int16` dBm; 0 = không có thông tin |
| `+39` | `LQI` | RO | 0..255; 0 = không có thông tin |
| `+40..+43` | `TX_PACKETS`, `RX_PACKETS`, `TX_ERRORS`, `RX_ERRORS` | RO | bộ đếm 16 bit do port báo |
| `+44` | `ERROR_CODE` | RO | `0` NONE, `1` TIMEOUT, `2` INVALID_RF_PARAM, `3` RESTRICTED_ID, `4` JOIN_FAILED, `5` KEY_REJECTED, `6` HW_FAULT |
| `+45..+63` | — | RO | luôn 0 |

Chi tiết cần nhớ:
- **Khối cấu hình `+0..+31` được lưu Flash 64 byte** (kích thước bản ghi không đổi); `+32..+63` không lưu.
- **Ghi vào `+32..+63`:** firmware **ACK và bỏ qua** (không lỗi).
- **Ghi vượt quá `0x173F`** (ví dụ FC16 bắt đầu `0x173F` quantity 2): Modbus exception `0x02`, **không thay đổi gì**.
- **Đọc** luôn trả **cấu hình ĐANG CHẠY**, không bao giờ trả nháp (kể cả khi vừa ghi xong, chưa COMMIT).
- **Khối `ENABLE=0` không bị kiểm các trường RF/tên/Slave ID**: lưu và đọc lại nguyên văn, App có thể "đỗ" bất kỳ giá trị nào.
- `PRESERVE_KEY` không bao giờ đọc lại được; `KEY_SET` (`+36` bit0) là cách duy nhất biết "đã có khoá".
- Bộ đếm `+40..+43` hiện do port đếm **lượng truyền trên UART tới module** (một khung ghi = 1 gói TX; một lần đọc không rỗng = 1 gói RX), không phải số gói Zigbee thật; `LINK_STATE`/`RSSI`/`LQI` ở port Zigbee-IO và DTU hiện = 0 (chưa có driver lệnh `INS`).

## 4. Quy trình nạp: nháp + COMMIT (giống khối MQTT)

1. Ghi khối vào `0x1700` (FC16/FC06). Mọi ghi vào `+0..+31` đi vào **NHÁP**.
   Lần ghi đầu tiên tạo nháp bằng cách **sao từ cấu hình đang chạy**, nên **ghi từng phần được** (ví dụ chỉ `+13` SLAVE_ID)
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
| 5 | `ENABLE=1` và một trường **ngoài dải** ở bảng mục 3 (`CHANNEL`, `PAN_ID`, `LOCAL_ADDR`, `POWER_LEVEL`, `BAUD_RATE_IDX`, `PARITY`, `DATA_BITS`, `STOP_BITS`, hai thời gian, `RETRY_COUNT`, `NETWORK_NAME` không phải ASCII in được / không có NUL, `SLAVE_ID` ngoài 1..247 khi Slave) | 4 / **2** |
| 6 | `SECURITY=1`, `PRESERVE_KEY=1`, **chưa có khoá lưu** | 4 / **2** |
| 7 | `SECURITY=1`, `PRESERVE_KEY=0`, nháp **không chứa khoá mới khác 0** (chưa ghi `+16..+23`, hoặc ghi toàn 0) | 4 / **2** |
| 8 | **Port từ chối** qua hook `validate` (xem mục 9): trên port Zigbee-IO và DTU, `ENABLE=1`, `ROLE=Slave`, `SLAVE_ID` = **236 hoặc 237** | 4 / **2** |
| 9 | Có khối **MB_READ / MB_WRITE đang bật** trỏ vào cổng Zigbee (giá trị thô 4, mục 7) trong khi vai (nháp, hoặc đang chạy nếu không có nháp) là **Slave** | 4 / **7** (`SLAVE_PORT_CONFLICT`) |

Lưu ý:
- Khi `ENABLE=0`, các kiểm tra 3, 4, 5 **không áp dụng**; nhưng kiểm tra 1, 2, 6, 7 (cờ, FRAMING, khoá) **vẫn áp dụng** nếu `SECURITY=1`.
- Các dải ở dòng 5 là dải của ICD v2.0.0; **firmware kiểm thật** (khác bản v0.2 cũ). Studio vẫn nên giới hạn trên UI để người dùng không phải chờ COMMIT lỗi. Cờ `ANTENNA` không có dải.
- Bản ghi Flash cũ (bố cục v0.2) mà đang `ENABLE=1`: khi nạp bị coi là hỏng, Zigbee về DISABLED (cần Deploy lại).
- **Slave ID 236 và 237** (module DRF dùng `0xEC / 0xED` làm lệnh riêng): lib không kiểm, nhưng **port Zigbee-IO và DTU từ chối ở COMMIT** (dòng 8). Studio vẫn **nên chặn trên UI** để người dùng không phải chờ COMMIT lỗi. `[ĐÃ CHỐT]`
- Kiểm tra ở dòng 9 dùng khối MB đang bật, nên chuyển Zigbee sang Slave **khi vẫn còn khối MB_READ/MB_WRITE bật trỏ vào Zigbee** cũng bị từ chối.

## 6. Khoá mạng (`NET_KEY`) — quy tắc chính xác

Khoá **chỉ ghi, không bao giờ đọc ra** (đọc `+16..+23` luôn = 0). Nháp **bắt đầu với khoá = 0** (không sao chép khoá đang lưu),
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
  `[ĐÃ CHỐT]` theo ICD v2.0.0 (`PortIndex = 4`). Còn cần App xác nhận: độ rộng bit của trường và việc giữ trống 5..15.
- Zigbee là **một slot Master thêm** sau các cổng RS-485, **không có bản ghi trong bảng `0x1400`**; trạng thái lấy từ khối `0x1700` đang chạy.
- Khối trên Zigbee chạy khi: `ENABLE=1`, `ROLE=Master`, và port đã đăng ký vai Master kèm đường byte. Thiếu một trong ba
  ⇒ khối báo **`err_code 1`**, không gửi byte nào.
- Khối MB đang bật trỏ vào Zigbee khi vai là Slave ⇒ COMMIT bị từ chối với lỗi **7** (mục 5, dòng 9).
- Tham số của lib `[Firmware tự chọn, đổi được]`: khoảng cách giữa hai khung 50 ms, thử lại 1 lần.
- Response được tách khung **theo độ dài** (kênh trong suốt, không có khoảng lặng 3,5 ký tự), nên response dài (FC03 tới 125 thanh ghi) đến nhiều mảnh vẫn được ghép đúng.

## 8. Báo trạng thái (`+36..+44`)

| Thanh ghi | Nguồn |
|---|---|
| `STATUS_FLAGS` (`+36`) | **Lib** tự điền: bit0 `KEY_SET` = đang lưu một khoá khác 0. |
| `LINK_STATE` (`+37`) | **Port** báo qua callback `status`: `0` OFF, `1` JOINING, `2` UP, `3` ERROR. |
| `RSSI_DBM` (`+38`), `LQI` (`+39`) | Port báo; 0 = không có thông tin. |
| `TX/RX_PACKETS`, `TX/RX_ERRORS` (`+40..+43`) | Port báo (hiện đếm lượng truyền UART, xem mục 3). |
| `ERROR_CODE` (`+44`) | Port báo theo danh mục ICD (mục 3). Port hiện dùng `3` RESTRICTED_ID khi một cấu hình đã lưu trong Flash có Slave ID 236/237. |

Port không đăng ký callback `status` (hoặc callback trả "không có thông tin") ⇒ lib trả `0` cho `+37..+44`.
Studio nên coi `0` ở `RSSI`/`LQI` là "không có thông tin", không phải "tín hiệu 0 dBm".

## 9. Phân chia lib / port

**Lib làm:** giữ cấu hình, nháp/COMMIT/kiểm tra (mục 4–6), lưu Flash, phục vụ Modbus Slave và Master trên ống byte, khai capability.
Bộ đếm `generation` tăng mỗi khi cấu hình đang chạy **thật sự đổi** (COMMIT có thay đổi, nạp Flash, `FACTORY_RESET`).

**Port (sản phẩm) làm:**
1. **Áp cấu hình xuống module radio** (PAN, kênh, ăng-ten, công suất, UART, khoá, vai) mỗi khi `generation` đổi. Việc này có thể mất vài giây và có thể khởi động lại module,
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
- Giới hạn các trường RF theo dải ở mục 3 (firmware cũng kiểm khi `ENABLE=1`).
- Hiển thị khoá là "đã đặt / chưa đặt" qua `KEY_SET`, **không bao giờ** đọc lại được khoá.
- Khi `SECURITY=1` mà không nhập khoá mới ⇒ đặt `PRESERVE_KEY=1` (nếu `KEY_SET=1`), còn `KEY_SET=0` thì **bắt buộc** nhập khoá.
- Sau COMMIT thất bại: đọc `0x9000/0x9001`, báo lỗi rõ, và **ghi lại tất cả** (mọi nháp đã bị vứt).
- Không ghi khối Zigbee nếu người dùng không đổi gì (khối không ghi ⇒ giữ nguyên cấu hình và khoá).

**Không được làm:**
- Không ghi `0x1700` khi thiếu cờ `0x8000`.
- Không coi `0` ở `RSSI` / `LQI` là giá trị đo thật (0 = không có thông tin).
- Không đếm Zigbee vào số cổng RS-485 (`0x0A0A`).
- Không mong `CLEAR_RULES` xoá cấu hình Zigbee (chỉ `FACTORY_RESET`).

## 13. Ví dụ giá trị thanh ghi (không kèm khung Modbus)

Board Slave, Slave ID 5, PAN `0x1234`, kênh 15, bật mã hoá, khoá mới `00 11 22 … EE FF`, tên "Node-A":

| Offset | Giá trị | Ghi chú |
|---|---|---|
| `+0` | `0x000B` | ENABLE (0x1) + ROLE_SLAVE (0x2) + SECURITY (0x8) |
| `+1` | `0x000F` | CHANNEL = 15 |
| `+2` | `0x1234` | PAN_ID |
| `+3` | `0x0001` | LOCAL_ADDR |
| `+4` | `0x0000` | DEST_ADDR (Coordinator) |
| `+5` | `0x0002` | POWER_LEVEL |
| `+6` | `0x0004` | BAUD_RATE_IDX = 115200 |
| `+7` | `0x0000` | PARITY None |
| `+8` | `0x0008` | DATA_BITS |
| `+9` | `0x0001` | STOP_BITS |
| `+10` | `0x05DC` | RESPONSE_TIMEOUT_MS = 1500 |
| `+11` | `0x0096` | INTER_FRAME_DELAY_MS = 150 |
| `+12` | `0x0003` | RETRY_COUNT |
| `+13` | `0x0005` | SLAVE_ID = 5 |
| `+14..+15` | `0x0000 0x0000` | RESERVED |
| `+16..+23` | `0x0011 0x2233 0x4455 0x6677 0x8899 0xAABB 0xCCDD 0xEEFF` | NET_KEY, byte cao trước: `+16 = 0x0011` nghĩa là byte0 = `0x00` (bit 15..8), byte1 = `0x11` (bit 7..0) |
| `+24..+31` | `0x4E6F 0x6465 0x2D41 0x0000 0x0000 0x0000 0x0000 0x0000` | "Node-A" + NUL |

Sau đó đổi riêng Slave ID thành 6, giữ khoá: ghi `+0 = 0x002B` (như trên + PRESERVE_KEY 0x20) và `+13 = 0x0006`. Khối còn lại không cần ghi lại.

## 14. Danh sách việc cần App xác nhận / sửa

1. ~~Giá trị thô `4` của trường cổng~~ đã chốt theo ICD v2.0.0; còn: độ rộng bit/dải của trường và giữ trống 5..15 (mục 7).
2. `CLEAR_RULES` có nằm trong quy trình Deploy không; firmware giữ nguyên cấu hình Zigbee khi `CLEAR_RULES` (mục 10).
3. Studio có bỏ qua khối Zigbee khi Upload → Deploy mà không đổi gì không (nếu có thì không cần `PRESERVE_KEY` trong luồng đó).
4. ~~Danh mục `ERROR_CODE`~~ đã có trong ICD v2.0.0 (0..6).
5. Xác nhận App chặn Slave ID 236/237 trên UI (firmware cũng từ chối ở COMMIT).
5b. **ICD mục 1.2** nói Zigbee "chỉ đóng vai Slave" nhưng cũng định nghĩa `ROLE = Master/Coordinator` và `PortIndex = 4` cho MB_READ/MB_WRITE: firmware hiểu `ROLE` là vai **Modbus** (Master/Slave), không phải vai mạng Zigbee (Coordinator/Router); xin App xác nhận và tách rõ hai khái niệm.
6. Nội dung Issue #1 ở `SynaptiXCompany/RS485_RF_IO` (firmware chưa đọc repo này).

## 15. Kiểm thử gợi ý cho App (dựa trên hành vi trên)

| # | Thao tác | Kết quả mong đợi |
|---|---|---|
| 1 | Ghi `+0 = 0x0004` (FRAMING=1), COMMIT | `0x9000=4`, `0x9001=2`; đọc lại `+0` vẫn là giá trị cũ |
| 2 | Ghi `+0 = 0x0043` (bit 6 = 1), COMMIT | lỗi 2 |
| 3 | `ENABLE+Slave`, `+13 = 0` hoặc `248`, COMMIT | lỗi 2 |
| 3b | `ENABLE+Slave`, `+13 = 236` hoặc `237`, COMMIT (port Zigbee-IO / DTU) | `0x9000=4`, `0x9001=2`; cấu hình đang chạy không đổi |
| 3c | `ENABLE`, `+1 = 10` hoặc `27` (CHANNEL), COMMIT | lỗi 2 (11 và 26 thì OK) |
| 4 | `ENABLE+Slave`, `+13 = 5`, COMMIT | OK; đọc lại `+13 = 5`; reboot rồi đọc lại vẫn = 5 |
| 5 | `SECURITY=1`, không ghi khoá, không `PRESERVE_KEY`, COMMIT | lỗi 2 |
| 6 | Ghi khoá, `SECURITY=1`, COMMIT | OK; đọc `+36` bit0 = 1; đọc `+16..+23` = 0 |
| 7 | `SECURITY=1` + `PRESERVE_KEY=1` khi **chưa có** khoá | lỗi 2 |
| 8 | Ghi `+13` rồi COMMIT với `SECURITY=0` khi đã có khoá | OK; `+36` bit0 vẫn = 1 (khoá giữ) |
| 9 | Ghi vào `+32..+63` | ACK, không đổi gì; ghi `0x173F` quantity 2 → exception `0x02` |
| 10 | COMMIT lỗi khi đang nạp thêm rule | sau lỗi, rule/FB/MQTT nháp cũng mất; phải ghi lại hết |
| 11 | `FACTORY_RESET` | `+0 = 0`, `KEY_SET = 0` |
| 12 | `CLEAR_RULES` | cấu hình Zigbee và `KEY_SET` giữ nguyên |