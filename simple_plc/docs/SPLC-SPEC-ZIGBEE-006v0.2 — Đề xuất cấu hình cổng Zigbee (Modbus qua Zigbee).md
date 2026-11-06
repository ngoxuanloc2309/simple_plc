# SPLC-SPEC-ZIGBEE-006 v0.2 — Cấu hình cổng Zigbee (Modbus qua Zigbee)

> **Trạng thái:** v0.2 (04/11/2026), chưa có code. Đã gộp phản hồi của đội App (v0.1 → v0.2). Một số điểm là **giả định của đội Firmware, App chưa xác nhận bằng văn bản** (đánh dấu **[GIẢ ĐỊNH]**, danh sách ở mục 9). Hai bên sẽ chuẩn hóa lại dần; điểm nào lệch thì sửa tài liệu và code theo bản chuẩn hóa.

## 0. Thay đổi so với v0.1

| Nội dung | v0.1 | v0.2 |
| --- | --- | --- |
| Số cổng Zigbee `+4` | Studio ghi `PORT_NUMBER` | **Bỏ.** Firmware tự gán cổng nội bộ. `+4` thành RESERVED |
| Cờ điều khiển | `CTRL`, `ROLE`, `FRAMING`, `NET_FLAGS` là các thanh ghi riêng | Gom vào **`CONTROL_FLAGS`** (`+0`) |
| Ăng-ten | không có | **Thêm** (bit 4 của `CONTROL_FLAGS`) |
| Khoá mạng | ghi một chiều, không có cơ chế giữ | Thêm **`PRESERVE_KEY`** và **`KEY_SET`** |
| Kiểu khung | chọn RTU/ASCII | **Cố định RTU**; `FRAMING = 1` bị từ chối |
| Thứ tự làm | Master và Slave song song | **Slave trước**, Master sau |
| Studio | một cổng số 5 trong danh sách RS-485 | **Loại giao tiếp** riêng: RS-485 (theo `0x0A0A`) và Zigbee (theo capability `0x8000`) |

## 1. Mục tiêu và ca sử dụng

Một mạng Zigbee hoạt động như một bus RS-485 không dây. Board DTU/Gateway đóng vai Modbus **Master** (module làm Coordinator), các board Zigbee-IO (ví dụ X.Link Zigbee 44) đóng vai Modbus **Slave** (module làm Router).

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

- **Zigbee không dùng để nạp rule từ Studio.** Studio cấu hình thiết bị qua USB (hoặc RS-485 ở chế độ App). Zigbee chỉ là đường dữ liệu Modbus.
- Zigbee là **tầng truyền (transport) vô tuyến**, không phải một "cổng RS-485 thứ hai". Board X.Link 44 chỉ có **một** cổng RS-485 vật lý và một ăng-ten; `0x0A0A` (số cổng RS-485 vật lý) **không** tính Zigbee.
- **Slave (làm trước):** luồng Slave chạy ngầm trong firmware, người dùng không kéo khối MB\_READ/MB\_WRITE nào. Phục vụ bản đồ cố định SLAVE-001 (DI `0x0000`, DO `0x0001`, VFLAG, VREG, COUNTER, SYS\_DIAG `0x00F0`).
- **Master (làm sau):** khối MB\_READ (`0x1000`) và MB\_WRITE (`0x1100`) có thể trỏ vào Zigbee, chỉ áp dụng cho thiết bị đóng vai Master.
- Chỉ hỗ trợ khung **RTU** trong phiên bản này (mục 6).

## 2. Nguyên tắc thiết kế

1. **Thư viện chỉ xử lý logic và giữ cấu hình.** Áp cấu hình vào module (Coordinator/Router, PAN ID, kênh, ăng-ten, khởi động lại module) và gửi/nhận byte do người viết port của từng sản phẩm làm. Thư viện không phụ thuộc loại module.
2. **Khối Zigbee chỉ tồn tại trên board có Zigbee.** Firmware khai capability `ZIGBEE` ở `0x0A08` **chỉ sau khi** host đăng ký port Zigbee (cùng cơ chế Wi-Fi và TLS cert).
3. **Một trường vai trò duy nhất:** Master hoặc Slave. Port tự ánh xạ sang Coordinator/Router. Không có tổ hợp sai để ghi xuống.
4. **Định danh cổng Zigbee do firmware/phần cứng quyết định**, Studio không ghi (Hardware Invariant).
5. **Phân định kiểm tra:** Core kiểm cái chung (cờ, `SLAVE_ID`, khả năng thiết bị, khoá, xung đột cổng). Port kiểm tham số sóng (PAN ID, kênh, ăng-ten) theo module thật và báo qua `ERROR_CODE`. Studio giới hạn dải chọn trên UI theo Profile thiết bị.
6. **Cấu hình đi theo Deploy** (nháp + COMMIT, lưu Flash), giống MQTT, MB\_READ, MB\_WRITE.

## 3. Giao diện Studio (đề xuất từ App, đã đồng ý)

- Mô hình **Loại giao tiếp**: `RS-485` (hiện số cổng theo `0x0A0A`) và `Zigbee` (hiện khi capability `ZIGBEE = 0x8000`).
- Nhãn vai trò: `Master (Coordinator)` và `Slave (Router)`. Phần trong ngoặc chỉ là chú thích theo module; wire chỉ ghi Master hoặc Slave.
- **Không đưa End Device vào nhãn Slave.** End Device có thể ngủ nên không nhận được lệnh; Modbus Slave phải là Router.
- Tuỳ chọn ASCII bị disable trong bản này (mục 6).
- Slave ID 236 và 237 bị chặn trực tiếp trên UI qua Device Profile khi dùng module DRF (mục 8).

## 4. Wire

### 4.1 Capability

| Bit | Giá trị | Tên | Ý nghĩa |
| --- | --- | --- | --- |
| 15 | `0x8000` | `SPLC_CAP_ZIGBEE` | Thiết bị có Zigbee; vùng `0x1700..0x173F` hợp lệ |

- Thiết bị chỉ khai bit này sau khi host đăng ký port Zigbee.
- Thiết bị Zigbee cũng khai `MODBUS_SLAVE` (`0x4000`) và/hoặc `MODBUS_MASTER` (`0x0100`) cho **vai mà port hỗ trợ**.
- App đã xác nhận cấp bit 15.

### 4.2 Vùng thanh ghi `0x1700..0x173F` (64 thanh ghi)

Big-endian, 32-bit High Word trước. App đã xác nhận cấp vùng này.

| Offset | Tên | Quyền | Mô tả |
| --- | --- | --- | --- |
| `+0` | `CONTROL_FLAGS` | R/W | Xem bảng bit bên dưới |
| `+1` | `SLAVE_ID` | R/W | 1..247, chỉ có nghĩa khi `Role = Slave` |
| `+2` | `PAN_ID` | R/W | `0x0001..0xFFFE` (Studio giới hạn theo Profile). Thư viện chỉ lưu và chuyển cho port |
| `+3` | `CHANNEL` | R/W | 11..26 (Studio giới hạn theo Profile). Thư viện chỉ lưu và chuyển cho port |
| `+4` | `RESERVED` | R/W | Ghi 0. Lưu và đọc lại nguyên văn, không kiểm |
| `+5..+12` | `NET_KEY` | **W** | 16 byte khoá AES-128, mỗi thanh ghi byte cao trước. **Đọc lại trả 0** |
| `+13..+31` | `RESERVED` | R/W | Ghi 0. Lưu và đọc lại nguyên văn, không kiểm |
| `+32` | `LINK_STATE` | RO | 0 = tắt, 1 = đang nối/join, 2 = đã nối, 3 = lỗi (port báo) |
| `+33` | `ERROR_CODE` | RO | Mã lỗi do port báo (0 = không lỗi) |
| `+34` | `NODE_COUNT` | RO | Số node thấy được (Coordinator; không có thông tin thì 0) |
| `+35` | `RSSI` | RO | **[GIẢ ĐỊNH]** int16, dBm có dấu; 0 = không có thông tin |
| `+36` | `STATUS_FLAGS` | RO | **[GIẢ ĐỊNH]** bit 0 = `KEY_SET` (đã có khoá trong Flash); các bit khác = 0 |
| `+37..+63` | `RESERVED` | RO | 0 |

**`CONTROL_FLAGS` (`+0`):**

| Bit | Tên | Mô tả |
| --- | --- | --- |
| 0 | `ENABLE` | 1 = bật, 0 = tắt |
| 1 | `ROLE` | 0 = Master (Coordinator), 1 = Slave (Router) |
| 2 | `FRAMING` | 0 = RTU, 1 = ASCII. **Bản này chỉ chấp nhận 0** (mục 6) |
| 3 | `SECURITY_ENABLE` | 1 = bật AES-128 (do port/module áp) |
| 4 | `ANTENNA` | 0 = PCB tích hợp, 1 = ngoài (SMA). Firmware chỉ lưu và chuyển cho port |
| 5 | `PRESERVE_KEY` | Cờ **lệnh**, không lưu: 1 = giữ khoá đang có trong Flash (mục 5). **Đọc lại luôn trả 0** |
| 6..15 | — | Phải bằng 0 |

Ghi vào vùng RO (`+32..+63`) được ACK và bỏ qua (cùng quy ước Wi-Fi). Ghi vượt `0x173F` hoặc bắt đầu trước `0x1700` trong vùng này theo quy tắc exception `0x02` như các khối khác.

### 4.3 Cách ghi: nháp + COMMIT

1. App ghi vùng `+0..+31` vào **bản nháp**.
2. COMMIT (`0xA000 = 0xA5A5`) trong quy trình Deploy 6 bước; vùng này nằm trong danh sách ghi ngoại vi trước staging.
3. Firmware kiểm cả bộ lúc COMMIT. Sai thì từ chối **cả COMMIT** với `CONFIG_STATUS = 4`, `CONFIG_ERROR_CODE = 2`. Các lỗi:
   - `FRAMING = 1` (chưa hỗ trợ ASCII, mục 6).
   - Bit 6..15 của `CONTROL_FLAGS` khác 0. **[GIẢ ĐỊNH]**
   - `ENABLE = 1`, `ROLE = Slave`, `SLAVE_ID` ngoài 1..247. **[GIẢ ĐỊNH]** (chỉ kiểm khi bật, giống cổng RS-485)
   - `ENABLE = 1` mà thiết bị không khai vai tương ứng (`MODBUS_MASTER` cho Master, `MODBUS_SLAVE` cho Slave).
   - Lỗi về khoá (mục 5).
4. Lỗi **7** (`SLAVE_PORT_CONFLICT`, `CONFIG_STATUS = 4`, `CONFIG_ERROR_CODE = 7`) khi khối MB\_READ/MB\_WRITE đang bật trỏ vào Zigbee mà `ENABLE = 1` và `ROLE = Slave`. App cũng chặn lúc biên dịch như đã làm cho RS-485.
5. **Khối không ghi trong nháp thì giữ cấu hình đang chạy** (giống MQTT).
6. Đọc lại vùng này luôn trả cấu hình **đang chạy**. `NET_KEY` đọc ra 0; `PRESERVE_KEY` đọc ra 0.
7. `FACTORY_RESET` xoá cấu hình Zigbee **kể cả khoá**, về mặc định (tắt, `KEY_SET = 0`). **`CLEAR_RULES` không xoá** cấu hình Zigbee **[GIẢ ĐỊNH, dự kiến, xác nhận lại khi viết code]**: khoá mạng là định danh mạng, xoá từ xa sẽ mất liên lạc, và nếu mỗi lần Deploy xoá thì `PRESERVE_KEY` vô nghĩa. Cách lưu Flash có thể phải đổi quy ước "bản ghi rỗng" của `CLEAR_RULES`.
8. Sau COMMIT, thư viện tăng `generation` và báo cho port. Port đọc cấu hình mới, áp vào module (có thể mất vài giây, kể cả khởi động lại module), rồi báo `LINK_STATE`. Thư viện không chờ module.
9. Lỗi do port phát hiện (PAN ID/kênh/ăng-ten không hợp lệ với module) **không** làm COMMIT thất bại; port báo qua `LINK_STATE = 3` và `ERROR_CODE`.

### 4.4 Master: khối MB\_READ/MB\_WRITE chọn Zigbee thế nào (đã code ở firmware; chưa chạy board)

- Studio **không** ghi số cổng Zigbee xuống `0x1700`. Khối MB trỏ vào Zigbee bằng một **hằng số cố định trong trường cổng của khối** (bit 4..7 của thanh ghi đầu khối), nằm ngoài dải RS-485.
- **[GIẢ ĐỊNH, CHƯA XÁC NHẬN]** Firmware dùng giá trị **thô 4** (Studio hiển thị "cổng 5", vì RS-485 dùng thô 0..3 = cổng 1..4). Giá trị thô 5..15 bị từ chối COMMIT với lỗi 2. Độ rộng bit và dải của trường này trong codec App **chưa xác nhận** (mục 9, điểm 1). Nếu 4 không ghi được thì đổi một hằng số (`PLC_MBM_PORT_ZIGBEE`), không đổi cấu trúc.
- Zigbee là một slot Master của `plc_mbm`/`plc_mbw`, **không có bản ghi trong bảng `0x1400`**: trạng thái lấy từ khối `0x1700` đang chạy (`ENABLE`, `ROLE`) và ống byte của port. Chỉ dùng được khi `ENABLE = 1`, `ROLE = Master` và port đã đăng ký vai Master kèm ống byte.
- Khối bật trỏ vào Zigbee mà Zigbee chưa bật, vai Slave, hoặc không có ống Master: COMMIT được nhận, lúc chạy khối báo `err_code 1` và **không gửi byte**.
- Khối MB bật trỏ vào Zigbee khi vai Zigbee (nháp hoặc đang chạy) là Slave: COMMIT bị từ chối `CONFIG_STATUS = 4`, `CONFIG_ERROR_CODE = 7` (xem 4.3 mục 4).
- Timeout, retry và khoảng nghỉ: timeout và interval theo từng khối; **khoảng nghỉ giữa hai khung 50 ms và retry 1 là chọn tạm của firmware** (`ZB_GAP_MS`, `ZB_RETRIES` trong `plc_mbm.c`), không có trường cấu hình. Khuyến nghị nhà sản xuất module cho mạng lớn: trễ giữa các lệnh khoảng 200 ms, timeout 2-5 s.
- Khung response được tách theo độ dài (mục 6.1), không dựa T3.5.
- Kiểu khung: RTU (mục 6).

### 4.5 Slave: phục vụ bản đồ nào

Khi `ROLE = Slave`, thiết bị phục vụ **bản đồ cố định SLAVE-001** trên đường Zigbee, giống cổng RS-485 ở vai Slave. **Không** phục vụ bản đồ cấu hình (`0x0000..` của Studio), vùng nạp rule (`0x9000`), COMMIT (`0xA000`), Wi-Fi, MQTT trên đường này. Master có thể đọc `SYS_DIAG` ở `0x00F0`.

Ghi chú triển khai: `plc_mbs` hiện chỉ phục vụ cổng có vai Slave trong bảng `0x1400`. Zigbee không nằm trong bảng đó nên cần một endpoint phục vụ riêng, cắm vào ống byte của port Zigbee.

## 5. Khoá mạng (`NET_KEY`, `PRESERVE_KEY`, `KEY_SET`)

Vấn đề cần tránh: Upload Project từ PLC lên Studio, sửa nhỏ rồi Deploy lại. Vì `NET_KEY` đọc ra 0 nên Studio có thể gửi khoá toàn 0, làm mất khoá mạng và ngắt liên lạc cả hệ thống.

**Quy tắc (chốt trừ các mục [GIẢ ĐỊNH]):**

| Tình huống trong nháp | Hành vi lúc COMMIT |
| --- | --- |
| Không ghi khối Zigbee | Giữ cấu hình đang chạy, kể cả khoá (mục 4.3 điểm 5) |
| `PRESERVE_KEY = 1` | Bỏ qua `NET_KEY` đã ghi; giữ khoá trong Flash. Nếu `SECURITY_ENABLE = 1` mà chưa có khoá trong Flash ⇒ từ chối, lỗi 2 |
| `PRESERVE_KEY = 0`, `SECURITY_ENABLE = 1` | Bắt buộc có `NET_KEY` mới trong nháp. Chưa ghi, hoặc toàn 0 ⇒ từ chối, lỗi 2 |
| `SECURITY_ENABLE = 0` | Bỏ qua `PRESERVE_KEY` và `NET_KEY`, không báo lỗi. Khoá đang lưu (nếu có) được giữ nguyên **[GIẢ ĐỊNH]** để bật lại bảo mật không mất khoá |

- `KEY_SET` (`STATUS_FLAGS` bit 0) cho Studio biết thiết bị đã có khoá hay chưa, không lộ khoá.
- `NET_KEY` chỉ ghi: FC03 trả 0, khác mật khẩu Wi-Fi (đọc lại được theo quyết định riêng).
- **Cảnh báo bảo mật:** khoá nằm **nguyên văn trong Flash** (như mật khẩu broker MQTT) và phía App lưu nguyên văn trong file dự án. Đây chưa phải bảo mật lớp bộ nhớ; thuộc phạm vi `SPLC-SPEC-SECURITY-001`.
- Việc thiết bị có áp AES-128 lên sóng hay không do port/module quyết định; thư viện chỉ lưu khoá và cờ.

## 6. Kiểu khung: cố định RTU

- Trên wire vẫn giữ bit `FRAMING` để dự phòng. Bản này chỉ hỗ trợ `0 = RTU`.
- Nhận `FRAMING = 1` ⇒ từ chối COMMIT với lỗi 2 (tuyệt đối không âm thầm chạy RTU). Studio disable tuỳ chọn ASCII.
- Hệ quả: firmware không build bộ xử lý ASCII cho Zigbee, không tốn thêm RAM so với cổng RTU.

### 6.1 Tách khung RTU qua ống không giữ nhịp

Module không dây không giữ khoảng nghỉ 3,5 ký tự nên không dựa vào T3.5. Port khai ống là loại "không giữ nhịp" (`frame_sync`), thư viện tách khung RTU theo **độ dài**:

- **Request** (Slave nhận): suy từ function code (FC01..06 = 8 byte; FC15/16 = 9 + byte count). **Đã làm và kiểm trên host giả** (126 kiểm tra). Chưa kiểm trên module thật.
- **Response** (Master nhận): suy từ byte count (FC01..04) hoặc cố định (FC05/06/15/16 và exception). **Đã code (`fr_resp` trong `mb_port.c`) và build ARM sạch; chưa chạy harness, chưa chạy board.**

## 7. Ghi chú cho người viết port (không thuộc thư viện)

Thư viện cung cấp: hàm đọc cấu hình đang chạy và `generation`; callback để port báo `LINK_STATE`, `ERROR_CODE`, `NODE_COUNT`, `RSSI`; ống byte (đọc không chặn, ghi) mà port tự cắm vào; thuộc tính khai báo ống không giữ nhịp.

## 8. Ràng buộc riêng của module DRF1609H (chỉ để port DRF tham khảo)

Lấy từ DRF Zigbee Module User Guide (firmware V8.1+), **chưa thử trên module thật**:

- **Slave ID không được là 236 (`0xEC`) hay 237 (`0xED`).** Module coi byte đầu `0xFC`, `0xFD`, `0xED`, `0xEC` là lệnh; khung Modbus bắt đầu bằng ID 236/237 có thể bị nuốt hoặc vỡ. **Studio chặn trên UI** qua Device Profile. **Port** từ chối và báo `ERROR_CODE` (lớp phòng thủ thứ hai). Thư viện không kiểm ở COMMIT vì không biết module. Số hiệu `ERROR_CODE` do port định nghĩa (chưa chốt danh mục chung, mục 9).
- Chế độ truyền trong suốt **thuần**: các chế độ nối thêm địa chỉ nguồn làm phản hồi dư byte.
- Chiều truyền: Coordinator gửi tới mọi Router; Router chỉ gửi về Coordinator. Master phải là Coordinator, Slave phải là Router.
- Gói tối đa 269 byte; tốt nhất ≤ 96 byte (FC03 nên ≤ 45 thanh ghi).
- UART module: chỉ 8N1, 8E1, 8O1; baud 1200..115200 (mặc định 38400); không có 7 bit. Baud phải khớp từng cặp module–MCU.
- Lệnh liên kết (`INS01`) làm mọi node ngừng nhận dữ liệu không dây khoảng 1 phút: không gọi khi đang phục vụ Modbus. Ghi tham số cần khởi động lại module mới có hiệu lực.
- Tài liệu không nói module chốt gói RF sau bao lâu im lặng trên UART; một request có thể bị chia hai gói. Đó là lý do tách khung theo độ dài.
- Lỗi bit thấp nhưng khác 0: Master cần retry.
- **Chưa kiểm:** module có chọn ăng-ten bằng lệnh hay không (bit `ANTENNA`); khoảng PAN ID và kênh module chấp nhận.

## 9. Điểm còn mở / giả định chờ chuẩn hóa

Đội App đã xác nhận bằng văn bản: kiểm tra chung ở Core và kiểm tra sóng ở Port; cố định RTU và từ chối `FRAMING = 1`; Slave ID 236/237 chặn trên UI và port báo `ERROR_CODE`; cấp `0x8000` và `0x1700..0x173F`; tổ chức code giữa `simple_plc` và board port do đội Firmware quyết định (Issue #1 mở tại `SynaptiXCompany/RS485_RF_IO`).

Các điểm sau là **giả định của đội Firmware**, theo thoả thuận hai bên sẽ chuẩn hóa lại sau:

| # | Điểm | Giả định hiện dùng |
| --- | --- | --- |
| 1 | Giá trị trường cổng trong khối MB để chỉ Zigbee | Giá trị thô 4 ("cổng 5"), ngoài dải thô 0..3 của RS-485; 5..15 bị từ chối. **Độ rộng và dải của trường trong codec App chưa biết.** Chỉ ảnh hưởng phần Master |
| 2 | `KEY_SET` | `STATUS_FLAGS` ở `+36`, bit 0 |
| 3 | Ngữ nghĩa `PRESERVE_KEY` | Theo bảng mục 5, kể cả "khối không ghi thì giữ khoá" |
| 4 | `CLEAR_RULES` và Zigbee | `CLEAR_RULES` không xoá, `FACTORY_RESET` xoá. Cách lưu Flash chưa kiểm |
| 5 | `RSSI` | int16 dBm có dấu, 0 = không có thông tin |
| 6 | Bit 6..15 của `CONTROL_FLAGS` khác 0 | Từ chối COMMIT (lỗi 2) |
| 7 | `SLAVE_ID` chỉ kiểm khi `ENABLE = 1` và `ROLE = Slave` | Giống cổng RS-485 |
| 8 | Danh mục `ERROR_CODE` chung | Chưa có; do port định nghĩa |
| 9 | `SECURITY_ENABLE = 0` giữ khoá đang lưu | Giữ nguyên, chỉ `FACTORY_RESET` xoá |
| 10 | Khoảng nghỉ giữa hai khung và retry của Master trên ống Zigbee | 50 ms và 1 retry, hằng số của firmware, không có trường cấu hình (mục 4.4) |

## 10. Việc đội App cần làm

1. Codec và UI cho `0x1700..0x173F` theo bảng 4.2; Studio ẩn/hiện theo capability `0x8000`.
2. Mô hình Loại giao tiếp (RS-485 / Zigbee) trong Studio.
3. Disable tuỳ chọn ASCII; chặn Slave ID 236/237 khi dùng module DRF.
4. Khi Upload: không gửi lại khoá toàn 0. Hoặc bỏ qua khối Zigbee khi người dùng không đổi gì, hoặc đặt `PRESERVE_KEY = 1`.
5. Chặn lúc biên dịch khối MB\_READ/MB\_WRITE trỏ vào Zigbee khi vai là Slave (lỗi 7).
6. Deploy: thiết bị không khai `ZIGBEE` mà project có khối Zigbee bật ⇒ dừng Deploy kèm lý do (giống `MODBUS_SLAVE`).
7. Trả lời điểm 1 (độ rộng và dải của trường cổng) trước khi làm phần Master.

## 11. Kế hoạch kiểm thử

1. **Host giả (không phần cứng):** ống giả không giữ nhịp, RTU, cả hai vai; request chia nhiều mảnh; hai request dính nhau; khung sai CRC; kiểm đột biến. Với khối cấu hình: từ chối `FRAMING = 1`, bit dự phòng, `SLAVE_ID`, các ca khoá (mục 5), lỗi 7, "khối không ghi thì giữ".
2. **Một board + App:** Studio ghi khối Zigbee, COMMIT, đọc lại (`NET_KEY` = 0, `PRESERVE_KEY` = 0), từ chối khi sai, persistence qua REBOOT, `CLEAR_RULES` giữ Zigbee, `FACTORY_RESET` xoá Zigbee và khoá.
3. **Hai board, port giả (dây nối thẳng):** Master đọc DI/DO/SYS\_DIAG của Slave, ghi DO (RTU).
4. **Module Zigbee thật:** độ trễ, gói bị chia, nhiều Slave, timeout/retry theo khuyến nghị. Không có kết quả nào ở mục này trước khi thử trên module thật.

## 12. Chưa làm / chưa biết

- Đã có code firmware: khối cấu hình `0x1700` (nháp, COMMIT, Flash, capability `0x8000`), endpoint Slave SLAVE-001 trên ống byte, tách khung request và response theo độ dài, Master qua Zigbee cho MB\_READ/MB\_WRITE. **Chưa chạy trên board, chưa thử module DRF1609H.** Đường `LIB=MB` của Master Zigbee (kênh master trong `mb_port`, tách khung response) mới được build, chưa chạy harness.
- Chưa có glue USART2 phía port (ngoài thư viện) và chưa nối `utils/zigbee` (cấu hình module bằng lệnh `INS*`).
- Script kiểm board cho khối cấu hình và lỗi 7: `test/test_zigbee_cfg.py` (chưa chạy board).
- Chưa chốt hằng số cổng Zigbee trong khối MB (điểm 1 mục 9).
- Cách lưu Flash: bản ghi Rule Table có cờ bit 8 (64 B); `CLEAR_RULES` không xoá Zigbee, `FACTORY_RESET` xoá kể cả khoá. Đã code, chưa kiểm trên board (điểm 4 mục 9).
- Khoá mạng lưu nguyên văn trong Flash.
- **Test qua sóng (06/11/2026):** script `test/test_zigbee_slave_read.py` (PC làm master qua một module DRF Coordinator cắm PC, đọc DI/DO của board Slave) mới chạy với slave giả, CHƯA chạy với module thật vì chưa có Coordinator phía PC. Dự kiến test luôn khi board DTU xong. Module DRF phải được đặt tay (Router/Coordinator, PAN ID, kênh, truyền trong suốt, UART 115200 8N1) vì firmware chưa có driver lệnh `INS*`.
- Port chưa từ chối được Slave ID 236/237 ở COMMIT (thư viện chưa có hook `validate` cho port): hiện chỉ báo `LINK_STATE = 3`, `ERROR_CODE = 1`.