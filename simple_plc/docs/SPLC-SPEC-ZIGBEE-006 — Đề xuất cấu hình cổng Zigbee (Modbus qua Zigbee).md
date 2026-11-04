# SPLC-SPEC-ZIGBEE-006 — Đề xuất cấu hình cổng Zigbee (Modbus qua Zigbee)

> **Trạng thái:** ĐỀ XUẤT, chưa có code. Phần wire (địa chỉ, layout) là gợi ý hợp lý để đội App xem xét; đội App có quyền thiết kế lại chi tiết, miễn giữ các nguyên tắc ở mục 2. Firmware sẽ code sau khi hai bên chốt.

## 1. Mục tiêu và ca sử dụng

Một mạng Zigbee hoạt động như một bus RS-485 không dây. Board DTU đóng vai Modbus **Master**, các board Zigbee-IO đóng vai Modbus **Slave**. DTU hỏi, chỉ thiết bị có Slave ID đúng mới trả lời.

```
PC / MQTT / cloud
      │ Ethernet
┌─────┴──────┐
│ Board DTU  │  Modbus Master  (module Zigbee làm Coordinator)
└─────┬──────┘
      │ sóng Zigbee
 ┌────┼─────────────┐
IO #1   IO #2   IO #3   Modbus Slave, ID 1, 2, 3  (module làm Router)
```

- **Zigbee không dùng để nạp rule từ Studio.** Studio vẫn cấu hình thiết bị qua USB (hoặc RS-485 ở chế độ App). Zigbee chỉ là một đường dữ liệu Modbus.
- Phía Slave phục vụ bản đồ cố định SLAVE-001 (DI ở `0x0000`, DO ở `0x0001`, VFLAG, VREG, COUNTER, SYS\_DIAG ở `0x00F0`). Không cần bản đồ mới.
- Phía Master dùng các khối MB\_READ (`0x1000`) và MB\_WRITE (`0x1100`) hiện có.
- Cả hai vai đều hỗ trợ khung **RTU** và **ASCII**.

## 2. Nguyên tắc thiết kế

1. **Thư viện chỉ xử lý logic và giữ cấu hình.** Việc áp cấu hình vào module (Coordinator/Router, PAN ID, kênh, khởi động lại module), cũng như gửi/nhận byte, do người viết port của từng sản phẩm làm. Thư viện không phụ thuộc loại module hay giao thức không dây.
2. **Khối Zigbee chỉ tồn tại trên board có Zigbee.** Firmware khai năng lực ở `0x0A08` chỉ sau khi host đăng ký port Zigbee (cùng cơ chế đang dùng cho Wi-Fi và TLS cert). Studio dựa vào bit này và profile thiết bị để hiện hay ẩn tab.
3. **Một trường vai trò duy nhất: Master hoặc Slave.** Trên wire không có trường Coordinator/Router. Port tự ánh xạ vai Modbus sang vai Zigbee. Nhờ vậy không tồn tại tổ hợp sai để ghi xuống.
4. **Vai trò và kiểu khung độc lập nhau.** Master hay Slave, đều có thể chạy RTU hoặc ASCII.
5. **Cấu hình đi theo Deploy** (nháp + COMMIT, lưu Flash), giống MQTT, MB\_READ và MB\_WRITE.

## 3. Đề xuất cho giao diện Studio

Nhãn nên ghi kèm thuật ngữ Zigbee trong ngoặc để người dùng hiểu:

- `Master (Coordinator)`
- `Slave (Router)`

Lưu ý:

- Phần trong ngoặc chỉ là chú thích. Wire chỉ ghi Master hoặc Slave.
- **Không đưa End Device vào nhãn của Slave.** End Device có thể ngủ nên không nhận được lệnh của Coordinator khi đang ngủ. Một Modbus Slave phải nhận request bất cứ lúc nào, nên Slave phải là Router.
- Chú thích này đúng với module DRF1609H nhưng thuộc về module, không phải thư viện. Nếu có module khác thì đổi nhãn theo profile thiết bị, không đổi wire.
- Khối Zigbee chỉ hiện khi thiết bị khai bit capability Zigbee.

## 4. Đề xuất wire

### 4.1. Capability

Đề xuất bit mới ở `0x0A08`:

| Bit | Giá trị | Tên | Ý nghĩa |
| --- | --- | --- | --- |
| 15 | `0x8000` | `ZIGBEE` | Thiết bị có cổng Zigbee; vùng `0x1700..0x173F` hợp lệ |

Hiện `SPLC_HardwareCapabilities` dùng tới bit 14 (`0x4000`). Bit 15 còn trống. Nếu thiết bị có Zigbee thì cũng nên khai `MODBUS_SLAVE` (`0x4000`) và/hoặc `MODBUS_MASTER` (`0x0100`) cho vai mà port hỗ trợ.

### 4.2. Vùng thanh ghi

Đề xuất vùng `0x1700..0x173F` (64 thanh ghi), tất cả big-endian, 32-bit High Word trước. Hiện `ModbusRegisterMap.cs` chưa dùng gì từ `0x1700` trở lên.

| Offset | Tên | Quyền | Mô tả |
| --- | --- | --- | --- |
| `+0` | `CTRL` | R/W | bit 0 = ENABLED; các bit khác = 0 |
| `+1` | `ROLE` | R/W | 0 = Master, 1 = Slave (cùng quy ước `PORT_ROLE` của `0x1400`) |
| `+2` | `FRAMING` | R/W | 0 = RTU, 1 = ASCII |
| `+3` | `SLAVE_ID` | R/W | 1..247, chỉ có nghĩa khi `ROLE = 1` |
| `+4` | `PORT_NUMBER` | R/W | Số cổng logic mà khối MB\_READ/MB\_WRITE dùng để chọn Zigbee (xem 4.4) |
| `+5` | `NETWORK_ID` | R/W | PAN ID, firmware chỉ lưu và chuyển cho port |
| `+6` | `CHANNEL` | R/W | Kênh, firmware chỉ lưu và chuyển cho port |
| `+7` | `NET_FLAGS` | R/W | bit 0 = SECURITY\_ENABLED; các bit khác dành cho port |
| `+8..+15` | `NET_KEY` | **W** | 16 byte khoá mạng, ghi được, **đọc lại trả 0** |
| `+16..+31` | `RESERVED` | R/W | Dành mở rộng, ghi 0 |
| `+32` | `LINK_STATE` | **RO** | 0 = tắt, 1 = đang nối, 2 = đã nối, 3 = lỗi (port báo) |
| `+33` | `ERROR_CODE` | **RO** | Mã lỗi do port báo (0 = không lỗi) |
| `+34` | `NODE_COUNT` | **RO** | Số node thấy được (nếu port có thông tin, không thì 0) |
| `+35` | `RSSI` | **RO** | Cường độ tín hiệu gần nhất (nếu có, không thì 0) |
| `+36..+63` | `RESERVED` | RO | 0 |

Giải thích:

- `NETWORK_ID`, `CHANNEL`, `NET_FLAGS`, `NET_KEY` là các trường **mờ** đối với thư viện: thư viện lưu Flash và đưa nguyên cho port đọc, không hiểu ý nghĩa. Ý nghĩa và giới hạn do port quy định (ví dụ module DRF dùng PAN ID 16 bit, kênh theo bảng của module).
- `NET_KEY` nên **ghi một chiều**: đọc lại trả 0 để không lộ khoá cho bất kỳ ai nối được vào link App. (Mật khẩu Wi-Fi hiện đọc lại được theo quyết định riêng; với khoá mạng không dây thì không nên làm vậy.) Lưu ý khoá vẫn nằm nguyên văn trong Flash, như mật khẩu broker MQTT.
- Nếu App không muốn quản lý tham số mạng, có thể bỏ `+5..+15` và để port tự cấu hình. Firmware không bắt buộc các trường này.
- Vùng trạng thái RO (`+32..+35`) dùng để Studio hiện "đã nối / lỗi" mà không cần thêm kênh khác.

### 4.3. Cách ghi: nháp + COMMIT

Giống MQTT / MB\_READ / MB\_WRITE:

1. App ghi vùng `+0..+31` vào **bản nháp**.
2. App kích COMMIT (`0xA000 = 0xA5A5`) như các khối khác, trong quy trình Deploy 6 bước. Vùng này nằm trong danh sách ghi ngoại vi trước staging.
3. Firmware kiểm cả bộ lúc COMMIT. Nếu sai, từ chối cả COMMIT với `CONFIG_ERROR_CODE = 2`. Các lỗi cần kiểm:
   - `ROLE > 1`.
   - `FRAMING > 1`.
   - `ROLE = 1` mà `SLAVE_ID` ngoài 1..247.
   - `ENABLED = 1` mà thiết bị không khai vai tương ứng (`MODBUS_MASTER` cho Master, `MODBUS_SLAVE` cho Slave).
   - `ENABLED = 1` mà khối MB\_READ/MB\_WRITE còn trỏ vào cổng Zigbee khi vai là Slave (xem lỗi 7 dưới).
4. Khối không ghi trong nháp thì **giữ cấu hình đang chạy** (giống MQTT).
5. Đọc lại vùng này luôn trả cấu hình **đang chạy**.
6. `CLEAR_RULES` và `FACTORY_RESET` xoá cấu hình Zigbee về mặc định (tắt).
7. Sau COMMIT, thư viện tăng `generation` và báo cho port. Port đọc cấu hình mới, áp vào module (có thể mất vài giây, kể cả khởi động lại module) rồi báo `LINK_STATE`. Thư viện không chờ module.

Gợi ý lỗi: dùng lại `SLAVE_PORT_CONFLICT = 7` khi khối MB\_READ/MB\_WRITE bật trỏ vào cổng Zigbee đang ở vai Slave (cùng ý nghĩa với cổng RS-485 Slave). App cũng nên chặn lúc biên dịch như đã làm cho RS-485.

### 4.4. Master: khối MB\_READ / MB\_WRITE chọn Zigbee thế nào

Đề xuất coi Zigbee là **một cổng có số riêng trong danh sách cổng**, để layout khối MB\_READ/MB\_WRITE không đổi. Studio chỉ thêm một mục "Zigbee" vào ô chọn cổng.

- Số cổng của Zigbee lấy từ `PORT_NUMBER` (`+4`). Gợi ý mặc định = 5, vì bảng cổng RS-485 hiện giới hạn 1..4.
- Đội App cần xác nhận trường số cổng trong khối MB\_READ/MB\_WRITE (kích thước, khoảng giá trị) có chứa được số 5 không, và ô "số cổng RS-485 vật lý" ở `0x0A0A` (hiện 1..4) giữ nguyên, không tính Zigbee.
- Tham số timeout, độ trễ giữa các khung và số lần thử lại (retry) dùng các trường hiện có của từng khối. Với Zigbee, nhà sản xuất module khuyến nghị: trễ giữa các lệnh khoảng 200 ms, timeout 2–5 s khi mạng lớn.
- **Kiểu khung (RTU/ASCII)** lấy từ `FRAMING` của khối Zigbee, không đặt theo từng khối MB.

### 4.5. Slave: phục vụ bản đồ nào

Khi `ROLE = 1`, thiết bị phục vụ **bản đồ cố định SLAVE-001** trên đường Zigbee, giống cổng RS-485 ở vai Slave. Không phục vụ bản đồ cấu hình (`0x0000..` của Studio) trên đường này. Studio không dùng Zigbee để nạp rule. Master có thể đọc `SYS_DIAG` ở `0x00F0` để biết tình trạng thiết bị.

## 5. Ghi chú cho người viết port (không thuộc thư viện)

Thư viện cung cấp:

- Hàm đọc cấu hình đang chạy và `generation`.
- Callback để port báo `LINK_STATE`, `ERROR_CODE`, `NODE_COUNT`, `RSSI`.
- Ống byte (đọc không chặn, ghi) mà port tự cắm vào, **không** phụ thuộc UART hay module.
- Một thuộc tính khai báo của ống: **ống có giữ nhịp byte hay không** (xem 5.1).

### 5.1. Khung RTU qua ống không giữ nhịp

Một số ống (module không dây, socket) không giữ khoảng nghỉ 3,5 ký tự giữa các khung nên không dựa vào T3.5 được. Port khai ống thuộc loại "không giữ nhịp" để thư viện tách khung RTU theo **độ dài** thay vì theo thời gian:

- **Request** (Slave nhận): độ dài suy ra từ function code (FC01..06 = 8 byte; FC15/16 = 9 + byte count).
- **Response** (Master nhận): độ dài suy ra từ byte count (FC01..04) hoặc cố định (FC05/06/15/16 và exception).

Phía Slave đã làm và kiểm trên host giả. Phía Master **chưa làm**: đây là việc phải làm trong thư viện khi triển khai. ASCII không cần vì có ký tự `:` mở đầu và CRLF kết thúc.

### 5.2. Ràng buộc riêng của module DRF1609H

Chỉ để port DRF tham khảo. Thông tin lấy từ tài liệu DRF Zigbee Module User Guide (firmware V8.1+), **chưa thử trên module thật**:

- **Slave ID không được là 236 (`0xEC`) hay 237 (`0xED`).** Module coi byte đầu `0xFC`, `0xFD`, `0xED`, `0xEC` là lệnh; khung Modbus bắt đầu bằng ID 236/237 có thể bị nuốt hoặc vỡ. Port nên từ chối hoặc báo `ERROR_CODE` nếu cấu hình dính các ID này. (ASCII bắt đầu bằng `:` nên không dính.)
- **Chế độ truyền trong suốt thuần.** Các chế độ có thêm địa chỉ nguồn nối vào cuối dữ liệu sẽ làm phản hồi dư byte.
- **Chiều truyền:** Coordinator gửi tới mọi Router; Router chỉ gửi về Coordinator. Vì vậy Master phải là Coordinator, Slave phải là Router.
- **Gói tối đa 269 byte**, nhưng hiệu năng tốt nhất khi ≤ 96 byte (một lần đọc FC03 nên ≤ 45 thanh ghi).
- **UART của module:** chỉ 8N1, 8E1, 8O1; baud 1200..115200 (mặc định 38400). **Không có 7 bit**, nên ASCII phải dùng 8 bit. Baud phải khớp từng cặp module–MCU.
- Lệnh liên kết (`INS01`) khiến mọi node ngừng nhận dữ liệu không dây khoảng 1 phút; không gọi khi đang phục vụ Modbus. Ghi tham số cần khởi động lại module mới có hiệu lực.
- Tài liệu **không nói** module chốt gói RF sau bao lâu im lặng trên UART. Một request từ PC có thể bị chia thành hai gói. Đó là lý do cần tách khung theo độ dài.
- Lỗi bit khi truyền thấp nhưng khác 0, nên Master cần retry.

## 6. Việc đội App cần xem

1. Có chấp nhận địa chỉ `0x1700..0x173F` và bit `ZIGBEE = 0x8000` không, hay muốn dùng địa chỉ/bit khác (hai chỗ này App giữ bản đồ chung).
2. Layout 4.2 có hợp lý với codec/DTO của App không; có muốn bỏ nhóm tham số mạng (`+5..+15`) không.
3. Studio nhận biết "board có Zigbee" bằng bit capability, profile thiết bị, hay cả hai.
4. Trường số cổng trong khối MB\_READ/MB\_WRITE có chứa được số cổng 5 không (4.4).
5. Nhãn UI `Master (Coordinator)` / `Slave (Router)`, và cách hiển thị `LINK_STATE`.
6. Chặn lúc biên dịch khối MB\_READ/MB\_WRITE trỏ vào Zigbee khi vai là Slave (lỗi 7).
7. Xử lý khi Deploy: thiết bị không khai bit `ZIGBEE` mà project có khối Zigbee bật nên dừng Deploy kèm lý do rõ, giống `MODBUS_SLAVE`.

## 7. Kế hoạch kiểm thử (sau khi hai bên chốt)

1. **Host giả (không phần cứng):** ống giả không giữ nhịp, RTU và ASCII, cả hai vai; request chia nhiều mảnh; hai request dính nhau; khung sai CRC/LRC; kiểm đột biến.
2. **Một board + App:** Studio ghi khối Zigbee, COMMIT, đọc lại, từ chối khi sai (ROLE, FRAMING, Slave ID), lỗi 7, persistence qua REBOOT, `CLEAR_RULES`/`FACTORY_RESET`.
3. **Hai board, port giả (dây nối thẳng):** Master đọc DI/DO/SYS\_DIAG của Slave, ghi DO, ở cả RTU và ASCII.
4. **Module Zigbee thật:** thử độ trễ, gói bị chia, nhiều Slave, timeout/retry theo khuyến nghị. Không có kết quả nào ở mục này trước khi thử trên module thật.

## 8. Chưa làm / chưa biết

- Chưa có code ở firmware cho khối Zigbee, API port và tách khung Master theo độ dài.
- Chưa thử trên module DRF1609H; ràng buộc ở 5.2 lấy từ tài liệu nhà sản xuất.
- Chưa chốt cách đặt số cổng Zigbee trong khối MB (4.4), chờ đội App.
- Chưa chốt chuẩn lưu khoá mạng ở Flash (hiện đề xuất nguyên văn, ghi một chiều).
