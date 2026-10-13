# SimplePLC MCU Firmware Conformance Specification (V2.0)

## 1. Giới thiệu & Phạm vi

Tài liệu này là đặc tả kỹ thuật chuẩn công nghiệp (**Authoritative Engineering Conformance Specification V2.0**) dành cho các kỹ sư phát triển Firmware nhúng (trên các nền tảng MCU công nghiệp như STM32F4/G4/H7, ESP32-S3, RP2040, GD32, NXP LPC, AVR,...) đảm bảo khả năng tương thích 100% với hệ sinh thái **SimplePLC** và phần mềm **SimplePLC Studio**.

Tất cả các định nghĩa kiểu dữ liệu C/C++, cấu trúc byte-exact, hằng số giao thức và thuật toán chuẩn được quy định duy nhất tại:
* C Header chuẩn V2.0: [`src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h)
* Bộ Golden Vectors V2.0: [`tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json)
* Kế thừa nền tảng: Bổ sung và tương thích toàn diện với Wire Contract V1.9 (`wire_profile = 2`).

---

## 2. Các nguyên tắc cốt lõi của giao thức (Core Protocol Invariants)

1. **Chỉ dùng Standard Modbus RTU**:
   * Chỉ sử dụng duy nhất 3 mã hàm Modbus chuẩn theo tiêu chuẩn Modbus-IDA:
     * **FC03 (0x03)**: Read Holding Registers
     * **FC06 (0x06)**: Write Single Register
     * **FC16 (0x10)**: Write Multiple Registers
   * **Nghiêm cấm** tự ý phát minh thêm Function Code tùy biến (như FC43, FC69,...).
2. **Thứ tự byte trên đường truyền (Wire Endianness & Word Ordering)**:
   * Thanh ghi Modbus 16-bit: **Big-Endian** trên đường truyền (High Byte gửi trước, Low Byte gửi sau).
   * Giá trị 32-bit (`int32_t`, `uint32_t`): Gồm 2 thanh ghi liên tiếp, **High Word trước, Low Word sau** (`[Reg0: High16, Reg1: Low16]`).
   * Mã kiểm tra CRC-16 Modbus (Đa thức `0xA001`, Giá trị khởi tạo `0xFFFF`): **Little-Endian** ở đuôi frame RTU (`[CRC_Lo, CRC_Hi]`).
3. **Chu kỳ quét định thời cố định (Deterministic Scan Cycle 10 ms)**:
   * Logic Rule Engine trên MCU chạy theo chu kỳ cố định danh định **10 ms**.
   * Đơn vị đo thời gian quét trong `DeviceHealth`: **`scan_time_ms`** và **`max_scan_time_ms`** (millisecond, không dùng microsecond để chống tràn số).
4. **Quyền sở hữu bộ nhớ Tag loại trừ lẫn nhau (Mutual-Exclusive Tag Store Ownership)**:
   * **`ENGINE_RUNNING`**: Rule Engine giữ quyền kiểm soát độc quyền bộ nhớ Tag Store (0x0900..0x09FF). Mọi lệnh ghi từ Host vào vùng này sẽ bị từ chối với lỗi Modbus `0x02 (Illegal Data Address)`.
   * **`DIAG_CONTROL`**: Rule Engine tạm dừng logic tại ranh giới chu kỳ quét và chuyển quyền sở hữu Tag Store cho Host (Studio). Host được phép ghi cưỡng bức vào các ngõ ra và cờ nhớ (`DO`, `VFLAG`, `VREG`, `VREG_RETAIN`, `COUNTER`).
5. **Giao dịch ghi nguyên tử All-or-Nothing trên FC16**:
   * Mỗi giá trị Tag chiếm đúng 2 thanh ghi (32-bit). Lệnh ghi vào `0x0900..0x09FF` bắt buộc dùng FC16 với số lượng chẵn `Quantity = 2 * N` và địa chỉ chẵn `0x0900 + TagIndex * 2`.
   * Nếu frame ghi chứa bất kỳ Tag nào là Read-Only (`DI`, `AI`) hoặc ngoài dải cho phép, MCU lập tức từ chối toàn bộ frame với Exception `0x02`, không thực hiện ghi một phần.

---

## 3. Bản đồ địa chỉ thanh ghi Modbus V2.0 (Register Memory Map)

| Vùng thanh ghi | Tên phân vùng | Chiều | FC | Kích thước | Mô tả chi tiết |
| :--- | :--- | :---: | :---: | :--- | :--- |
| `0x0000..0x0009` | **Device Descriptor** | R | FC03 | 10 regs (20 B) | Thông tin nhận dạng, Class (=1), Variant (=1), HW/FW Version, Protocol Version (=2), Rule Format Version (=7). |
| `0x0010` | **Rule Table Info** | R | FC03 | 1 reg (2 B) | Số lượng Rule đang kích hoạt (`active_rule_count`, 0..100). |
| `0x0020..0x0029` | **Device Resource Info** | R | FC03 | 10 regs (20 B) | Hồ sơ tự mô tả: WireProfile (=2), MaxRules (=100), ActiveTags (=124), DI=8, DO=8, AI=4, VFLAG=32, VREG=32, RETAIN=32, COUNTER=8. |
| `0x0100..0x073F` | **Active Rule Table** | R | FC03 | Max 1600 regs | Bảng Rule đang chạy thực tế (tối đa 100 rules × 16 thanh ghi = 3200 bytes). |
| `0x0800..0x0809` | **Device Health** | R | FC03 | 10 regs (20 B) | `uptime_s` (u32), `reset_reason` (u16), `health_flags` (u16), `cpu_load` (u16), `ram_usage` (u16), `scan_time_ms` (u32), `max_scan_time_ms` (u32). |
| `0x0810..0x0813` | **Real-Time Clock (RTC)** | R/W | FC03/FC16 | 4 regs (8 B) | Đồng hồ RTC: `epoch_utc_s` (u32, High Word trước), `tz_offset_min` (i16, ví dụ +420 cho UTC+7), `status_flags` (u16). |
| `0x0900..0x09FF` | **Runtime Tag Values** | R/W | FC03/FC16 | 256 regs (128 tags) | Giá trị 32-bit của các Tag theo `TagIndex` 0..127. Mỗi tag = 2 regs (High Word trước). Chỉ cho phép ghi trong chế độ `DIAG_CONTROL`. |
| `0x0A00` | **System Command** | W | FC06/FC16 | 1 reg (2 B) | Lệnh bảo trì hệ thống (`1: REBOOT`, `2: FACTORY_RESET`, `3: CLEAR_RULES`, `4: CLEAR_RETAIN`). |
| `0x0A01..0x0A02` | **System Command Result** | R | FC03 | 2 regs (4 B) | Kết quả thực thi lệnh: `[0]=CommandStatus`, `[1]=ErrorCode`. |
| `0x0A20..0x0A24` | **Diagnostic Control Block**| R/W | FC03/FC06/FC16 | 5 regs (10 B) | Phân hệ chẩn đoán cưỡng bức: `Command` (0x0A20), `State` (0x0A21), `Flags` (0x0A22), `LeaseRemainingMs` (0x0A23), `ErrorCode` (0x0A24). |
| `0x0B00..0x0B3F` | **Function Block Timers** | R/W | FC03/FC16 | 64 regs (128 B) | Cấu hình & viễn trắc 8 khối Timer IEC 61131-3 (TON, TOF, TP). Host ghi cấu hình (Mode, PT) qua FC16 (tối đa 3 khối/lệnh); Firmware tính toán và cập nhật trạng thái (StatusBits, ET). Mỗi khối 8 thanh ghi (16 bytes). |
| `0x0B40..0x0B7F` | **Function Block Counters**| R/W | FC03/FC16 | 64 regs (128 B) | Cấu hình & viễn trắc 8 khối Counter IEC 61131-3 (CTU, CTD). Host ghi cấu hình (Mode, PV, RetainTagIndex) qua FC16 (tối đa 3 khối/lệnh); Firmware tính toán và cập nhật trạng thái (StatusBits, CV). Mỗi khối 8 thanh ghi (16 bytes). |
| `0x0C00..0x0D7F` | **MQTT Telemetry Blocks**  | R/W | FC03/FC16 | 384 regs (768 B) | Cấu hình 4 khối xuất bản dữ liệu viễn trắc MQTT IIoT (SPLC-SPEC-MQTT-001). Mỗi khối 96 thanh ghi (192 bytes) gồm: Flags/Topic/Timing (32 regs) và Payload Template JSON (64 regs). |
| `0x0E00..0x0E5F` | **MQTT Broker Configuration**| R/W | FC03/FC16 | 96 regs (192 B) | Cấu hình kết nối mạng máy chủ MQTT Broker cấp hệ thống: Host/IP (64B), Port, TLS/SSL, Client ID, Username, Password, KeepAlive. |
| `0x9000..0x9005` | **Staging Config Handshake**| R/W | FC03/FC16 | 6 regs (12 B) | `Status`, `ErrorCode`, `RuleCountStaged`, `ExpectedCrc16`, `ActiveRuleCount`, `ActiveRuleCrc16`. |
| `0x9010..0x964F` | **Staging Rule Buffer** | W | FC16 | Max 1600 regs | Vùng đệm nạp Rule mới (ghi từng block tối đa 120 registers). |
| `0xA000` | **Commit Command** | W | FC06/FC16 | 1 reg (2 B) | Ghi `0xA5A5` (`SPLC_COMMIT_MAGIC`) để MCU kiểm tra CRC và tráo bảng (atomic swap). |
| `0xA001` | **Active Rule Version** | R | FC03 | 1 reg (2 B) | Tăng lên 1 mỗi khi commit thành công. |

---

## 4. Phân bổ `TagIndex` Động (Dynamic Tag Index Layout)

Từ phiên bản SimplePLC V2.0+, các chỉ số `TagIndex` **không còn cố định theo các mốc 8 slot tĩnh**. MCU Firmware tự động phân bổ và đóng gói liên tục (`Consecutive Packing`) các nhóm Tag dựa trên số lượng cổng thực tế khai báo trong `DeviceResourceInfo` (`0x0020`):

### 4.1. Công thức tính Base Index động
```text
DiBase         = 0
DoBase         = di_count
AiBase         = di_count + do_count
VflagBase      = di_count + do_count + ai_count
VregBase       = di_count + do_count + ai_count + vflag_count
VregRetainBase = di_count + do_count + ai_count + vflag_count + vreg_count
CounterBase    = di_count + do_count + ai_count + vflag_count + vreg_count + vreg_retain_count
TotalTags      = CounterBase + counter_count
```

Địa chỉ thanh ghi Modbus tương ứng:
$$\text{Modbus Address} = 0\text{x}0900 + (\text{TagIndex} \times 2)$$

### 4.2. So sánh ví dụ thực tế

| Nhóm Tag | Cấu hình Tiêu chuẩn (8DI, 8DO, 4AI) | Cấu hình Nhỏ gọn (4DI, 4DO, 2AI) | Đặc tính truy cập |
| :--- | :--- | :--- | :--- |
| **`DI`** | TagIndex `0 .. 7` (`0x0900..0x090F`) | TagIndex `0 .. 3` (`0x0900..0x0907`) | Read-Only (Optocoupler vật lý) |
| **`DO`** | TagIndex `8 .. 15` (`0x0910..0x091F`) | TagIndex `4 .. 7` (`0x0908..0x090F`) | Read-Write (Relay/Transistor) |
| **`AI`** | TagIndex `16 .. 19` (`0x0920..0x0927`)| TagIndex `8 .. 9` (`0x0910..0x0913`) | Read-Only (ADC 0-10V/4-20mA) |
| **`VFLAG`** | TagIndex `20 .. 51` (`0x0928..0x0967`)| TagIndex `10 .. 41` (`0x0914..0x0953`)| Read-Write (Cờ bit nội bộ RAM) |
| **`VREG`** | TagIndex `52 .. 83` (`0x0968..0x09A7`)| TagIndex `42 .. 73` (`0x0954..0x0993`)| Read-Write (Biến số 32-bit RAM) |
| **`VREG_RETAIN`**| TagIndex `84 .. 115` (`0x09A8..0x09E7`)| TagIndex `74 .. 105` (`0x0994..0x09D3`)| Read-Write (Biến lưu giữ Flash) |
| **`COUNTER`**| TagIndex `116 .. 123` (`0x09E8..0x09F7`)| TagIndex `106 .. 113` (`0x09D4..0x09E3`)| Read-Write (Bộ đếm phần cứng/mềm) |

> [!TIP]
> **Ưu điểm cho Firmware**: Khi board chỉ có 4 cổng DI, `DO0` sẽ bắt đầu ngay tại `TagIndex 4`. Firmware không cần chừa các slot trống không có thực `4..7`, giúp bộ nhớ RAM phẳng và việc lập trình vòng lặp IO đơn giản, tự nhiên hơn.

---

## 5. Đặc tả Cấu trúc Bản ghi Rule 32-Byte (`SPLC_RuleRecord_t`)

Mỗi bản ghi Rule chiếm đúng 16 thanh ghi Modbus (32 bytes), định dạng byte-exact theo chuẩn đóng gói `#pragma pack(push, 1)`:

```c
typedef struct SPLC_PACKED {
    int32_t  threshold_lo;        /* [Reg 0..1]   Ngưỡng dưới (High Word trước) */
    int32_t  threshold_hi;        /* [Reg 2..3]   Ngưỡng trên (High Word trước) */
    uint32_t for_ms;              /* [Reg 4..5]   Thời gian trễ / chu kỳ kích hoạt (ms) */
    int32_t  action_param;        /* [Reg 6..7]   Giá trị tham số hành động */
    uint16_t trigger_tag;         /* [Reg 8]      TagIndex kích hoạt (0..127) */
    uint16_t action_tag;          /* [Reg 9]      TagIndex tác động (0..127) */
    uint16_t guard_tag;           /* [Reg 10]     Bit 15: NEGATE (1=NOT), Bits 0..14: TagIndex (0x7FFF=None) */
    uint8_t  enabled;             /* [Reg 11 High] 1 = Kích hoạt, 0 = Vô hiệu */
    uint8_t  trigger_type;        /* [Reg 11 Low]  SPLC_TriggerType_t */
    uint8_t  compare_op;          /* [Reg 12 High] SPLC_CompareOp_t */
    uint8_t  action_type;         /* [Reg 12 Low]  SPLC_ActionType_t */
    uint8_t  reserved[6];         /* [Reg 13..15] Luôn ghi 0x00; receiver bỏ qua */
} SPLC_RuleRecord_t;
```

### Các tập Enum Logic:
* **TriggerType (`uint8_t`)**:
  * `0`: `ON_CHANGE` — Kích hoạt khi giá trị tag thay đổi so với chu kỳ trước.
  * `1`: `ON_RISE` — Kích hoạt khi có sườn lên (0 -> 1 hoặc vượt qua ngưỡng).
  * `2`: `ON_FALL` — Kích hoạt khi có sườn xuống (1 -> 0 hoặc tụt xuống dưới ngưỡng).
  * `3`: `TIME_WINDOW` — Kích hoạt theo khung giờ thực tế (dựa vào đồng hồ RTC).
  * `4`: `INTERVAL` — Kích hoạt định kỳ sau mỗi khoảng thời gian `for_ms`.
* **CompareOp (`uint8_t`)**:
  * `0`: `NONE`, `1`: `EQ` (`=`), `2`: `NEQ` (`!=`), `3`: `GT` (`>`), `4`: `LT` (`<`), `5`: `GTE` (`>=`), `6`: `LTE` (`<=`), `7`: `BETWEEN` (`Lo <= X <= Hi`).
* **ActionType (`uint8_t`)**:
  * `0`: `SET_TAG` — Gán giá trị `action_param` vào `action_tag`.
  * `1`: `TOGGLE_TAG` — Đảo trạng thái cờ logic (0 <-> 1).
  * `2`: `INC_COUNTER` — Tăng bộ đếm thêm `action_param`.
  * `3`: `WRITE_REMOTE` — Ghi ra thiết bị Modbus Slave ngoại vi.
  * `4`: `LOG_EVENT` — Ghi bản ghi nhật ký vào RAM circular buffer.
  * `5`: `SEND_ALARM` — Bật cờ cảnh báo hệ thống.
  * `6`: `ADD_TAG` — Cộng giá trị `action_param` vào giá trị tag hiện tại.
  * `7`: `SCALE_TAG` — Áp dụng công thức chuyển đổi tỉ lệ tuyến tính (`y = k * x + b`):
    * `action_param`: Hệ số tỉ lệ cố định đã nhân tỉ lệ với 1000 (`action_param = round(k * 1000)`).
    * `threshold_hi`: Độ lệch gốc (Offset `b`) dạng số nguyên.
    * Giá trị ngõ ra ghi vào `action_tag`:
      ```text
      ActionTag.Value = floor((TriggerTag.Value * action_param) / 1000) + threshold_hi
      ```
    * *Ví dụ:* Cảm biến analog `0..10000 mV` chuyển đổi sang `0..100°C` với hệ số `k = 0.01`, offset `b = 0`: `action_param = 10`, `threshold_hi = 0`.

---

## 6. Đặc tả Đồng bộ RTC & Thuật toán Kích hoạt Time Window

### 6.1. Khối thanh ghi RTC Clock (`0x0810..0x0813`)
Phân vùng gồm 4 thanh ghi (8 bytes):
* **`0x0810..0x0811`**: `epoch_utc_s` (`uint32_t`, High Word trước) — Unix timestamp (giây tính từ 1970-01-01 00:00:00 UTC).
* **`0x0812`**: `tz_offset_min` (`int16_t`) — Độ lệch múi giờ theo phút (ví dụ Việt Nam UTC+7 là `+420`, Tokyo UTC+9 là `+540`).
* **`0x0813`**: `status_flags` (`uint16_t`):
  * `Bit 0` (`0x0001`): `SPLC_RTC_FLAG_SYNCED` (1 = Đã đồng bộ với Host PC; 0 = Chưa đồng bộ). Host bật cờ này khi nạp giờ chuẩn.
  * `Bit 1` (`0x0002`): `SPLC_RTC_FLAG_HW_PRESENT` (1 = Có chip phần cứng RTC như DS3231/PCF8563 hoặc thạch anh 32.768kHz LSE; 0 = Đếm giờ mềm). *Do MCU tự kiểm tra phần cứng khi khởi động, Host không được tự ý xóa cờ này.*
  * `Bit 2` (`0x0004`): `SPLC_RTC_FLAG_BATTERY_LOW` (1 = Pin nuôi RTC bị yếu/hết pin hoặc tháo pin; 0 = Pin tốt). *Do MCU đo đạc điện áp pin / đọc thanh ghi cảnh báo của IC RTC để báo cáo cho Host.*

* **Quy tắc Giao tiếp & An toàn Múi giờ (Read-Before-Write & On-Connect Policy)**:
  * **Khi Host kết nối (On-Connect)**: Host chỉ thực hiện **FC03 đọc `0x0810..0x0813`** để cập nhật trạng thái hiển thị trên Status Bar. **Tuyệt đối KHÔNG tự động ghi đè giờ PC xuống MCU**, nhằm bảo vệ nguyên vẹn múi giờ quốc tế (ví dụ: Mỹ UTC-5, Nhật Bản UTC+9) đã cấu hình trước đó của thiết bị.
  * **Khi Host gửi lệnh đồng bộ (Explicit User Sync)**: Thao tác ghi đồng bộ thời gian/múi giờ mới chỉ diễn ra khi kỹ sư chủ động bấm nút "Đồng bộ" trong hộp thoại RTC.
  * Khi Host gửi frame **FC16 ghi `0x0810..0x0813`**, Host **bắt buộc phải bảo toàn** cờ `HW_PRESENT` và `BATTERY_LOW` của MCU (áp dụng cơ chế Read-Before-Write), chỉ cập nhật `epoch_utc_s`, `tz_offset_min` và `SYNCED = 1`.
  * Phía Firmware MCU: Nếu nhận được FC16, Firmware cập nhật `epoch_utc_s`, `tz_offset_min`, và đặt `SYNCED = 1`, đồng thời giữ nguyên trạng thái `HW_PRESENT` và `BATTERY_LOW` theo phần cứng thực tế.

### 6.2. Thuật toán tính toán giờ địa phương danh định `HHmm`
Trong mỗi chu kỳ quét 10ms, MCU tăng bộ đếm thời gian nội bộ. Khi cần đánh giá các Rule có `trigger_type == SPLC_TRG_TIME_WINDOW (3)`, MCU tính toán giá trị `current_hhmm`:

```text
local_epoch = epoch_utc_s + (tz_offset_min * 60)
seconds_of_day = local_epoch % 86400
hours = floor(seconds_of_day / 3600)
minutes = floor((seconds_of_day % 3600) / 60)
current_hhmm = (hours * 100) + minutes
```

*Ví dụ:* 07:15 sáng → `715`; 18:45 tối → `1845`.

### 6.3. Ngữ nghĩa đánh giá Time Window
* **Khung giờ trong ngày (`Lo <= Hi`, ví dụ 07:00 đến 17:00 → `700..1700`):**
  * Điều kiện đúng: `700 <= current_hhmm <= 1700`.
* **Khung giờ xuyên đêm qua nửa đêm (`Lo > Hi`, ví dụ 18:00 đến 06:00 sáng hôm sau → `1800..600`):**
  * Điều kiện đúng: `current_hhmm >= 1800 || current_hhmm <= 600`.
* **Khung giờ điểm chính xác (`Op == SPLC_CMP_EQ` với `Lo == Hi`, ví dụ đúng 08:30 → `830`):**
  * Chỉ đánh giá là `TRUE` tại thời điểm chuyển phút (edge transition) của phút đó để tránh kích hoạt liên tục trong suốt 60 giây.

---

## 7. Phân hệ Function Block IEC 61131-3 (Timers & Counters Subsystem)

Toàn bộ phân vùng `0x0B00..0x0B7F` (128 registers = 256 bytes) dành cho phân hệ Function Block chuyên dụng. Mỗi khối được căn gióng lũy thừa 2 với kích thước cố định **8 thanh ghi (16 bytes)**.

### 7.0. Nguyên tắc Phân quyền & Giới hạn Giao tiếp (R/W Access & Chunking Contract)
1. **Phân quyền truy cập (Read/Write Separation)**:
   * **Host (SimplePLC Studio)** có quyền ghi cấu hình khởi tạo (Configuration) bằng Modbus **FC16**:
     * Với Timer: `mode` (+1), `pt_ms` (+2..+3).
     * Với Counter: `mode` (+1), `preset_value` (+2..+3), `retain_tag_index` (+6).
   * **MCU Firmware** sở hữu và cập nhật các trường trạng thái / thời gian thực (Runtime Telemetry) trong chu kỳ quét:
     * Với Timer: `status_bits` (+0), `et_ms` (+4..+5).
     * Với Counter: `status_bits` (+0), `current_value` (+4..+5).
   * **Host đọc viễn trắc** bằng Modbus **FC03** để hiển thị Live Watch trên Canvas và bảng Function Block Inspector.
2. **Quy tắc chia gói FC16 (Chunking Contract - Tối đa 3 khối/lệnh)**:
   * Do giới hạn bộ đệm nhận UART RX của một số vi điều khiển là **64 bytes**, Host **bắt buộc chia nhỏ** mỗi lệnh ghi FC16 xuống tối đa **3 khối (24 thanh ghi = 48 bytes dữ liệu)**.
   * Frame Modbus RTU ghi 3 khối: 1 (Slave) + 1 (FC16) + 2 (Addr) + 2 (Count) + 1 (ByteCount) + 48 (Data) + 2 (CRC) = **57 bytes**, đảm bảo an toàn tuyệt đối không làm tràn RX buffer.
   * Khi nạp đủ 8 khối, Host phát 3 lệnh liên tiếp: Chunk 1 (3 khối), Chunk 2 (3 khối), Chunk 3 (2 khối).
3. **Đính chính địa chỉ (Address Disambiguation)**:
   * Bảng Timers bắt đầu tại `0x0B00` (kết thúc tại `0x0B3F`).
   * Bảng Counters bắt đầu tại **`0x0B40`** (kết thúc tại `0x0B7F`).
   * *Đính chính*: **Không tồn tại địa chỉ `0x0BC0`**. Địa chỉ `0x0BC0` là lỗi typo trong các bản thảo sơ khai (nhầm lẫn giữa `0x0B40` và `0x0BC0`), địa chỉ chuẩn xác duy nhất là `0x0B40`.

### 7.1. Bảng Timer Subsystem (`0x0B00..0x0B3F`, 8 Timers TON/TOF/TP)
Địa chỉ Timer `i` (`i` trong `[0..7]`): `Addr = 0x0B00 + (i * 8)`.
* **`+0`**: `status_bits` (`uint16_t`, RO, do MCU cập nhật):
  * `Bit 0` (`0x0001`): `IN` (Tín hiệu đầu vào đang kích hoạt).
  * `Bit 1` (`0x0002`): `Q` (Ngõ ra trạng thái của Timer).
  * `Bit 2` (`0x0004`): `RESET` (Tín hiệu Reset đang kích hoạt).
  * `Bit 3` (`0x0008`): `RUNNING` (Timer đang tích lũy thời gian `ET < PT`).
* **`+1`**: `mode` (`uint16_t`, R/W): `1 = TON` (On-Delay), `2 = TOF` (Off-Delay), `3 = TP` (Pulse Timer).
* **`+2..+3`**: `pt_ms` (`uint32_t`, R/W, High Word trước) — Thời gian cài đặt (Preset Time) tính bằng ms.
* **`+4..+5`**: `et_ms` (`uint32_t`, RO, High Word trước, do MCU cập nhật) — Thời gian đã trôi qua (Elapsed Time) tính bằng ms.
* **`+6..+7`**: `reserved[2]` — Luôn trả về 0x0000.

### 7.2. Bảng Counter Subsystem (`0x0B40..0x0B7F`, 8 Counters CTU/CTD)
Địa chỉ Counter `i` (`i` trong `[0..7]`): `Addr = 0x0B40 + (i * 8)`.
* **`+0`**: `status_bits` (`uint16_t`, RO, do MCU cập nhật):
  * `Bit 0` (`0x0001`): `CU` (Xung đếm lên active).
  * `Bit 1` (`0x0002`): `CD` (Xung đếm xuống active).
  * `Bit 2` (`0x0004`): `RESET` (Tín hiệu Reset active).
  * `Bit 3` (`0x0008`): `Q` (Ngõ ra Counter: Với CTU là `CV >= PV`, với CTD là `CV <= 0`).
* **`+1`**: `mode` (`uint16_t`, R/W): `1 = CTU` (Count Up), `2 = CTD` (Count Down).
* **`+2..+3`**: `preset_value` (`int32_t`, R/W, High Word trước) — Giá trị ngưỡng cài đặt (PV).
* **`+4..+5`**: `current_value` (`int32_t`, RO/Mirror, High Word trước) — Giá trị đếm hiện tại (CV).
  * Firmware trích xuất giá trị này từ ô nhớ thanh ghi được chỉ định bởi `retain_tag_index` (Storage Register Binding) để trả về cho App hiển thị viễn trắc lên node Counter trên Canvas.
* **`+6`**: `retain_tag_index` (`uint16_t`, R/W) — Chỉ số Tag thanh ghi lưu trữ (Storage Register Binding) của khối Counter:
  * Trỏ tới bất kỳ thanh ghi lưu trữ hợp lệ nào (`VREG`, `VREG_RETAIN`, `VFLAG`, `COUNTER[i]`). Được App gửi xuống lúc Deploy để Firmware biết vị trí thanh ghi chứa số đếm CV của khối Counter và cập nhật vào `current_value`.
  * Trả về `0xFFFF` (`SPLC_FB_RETAIN_NONE`) nếu Counter không có thanh ghi liên kết.
  * Nếu trỏ tới vùng `VREG_RETAIN`, giá trị đếm sẽ tự động được bảo lưu qua các chu kỳ mất nguồn / reboot theo cơ chế Retentive Flash.
* **`+7`**: `reserved` — Luôn trả về 0x0000.

---

## 8. Cơ chế Chẩn đoán & Cưỡng bức Ngoại vi (Diagnostic Override & Lease Watchdog)

### 8.1. Các thanh ghi Diagnostic Block (`0x0A20..0x0A24`)
* **`0x0A20`**: `DIAG_COMMAND` (WO, ghi bằng FC06 hoặc FC16):
  * `1 = ENTER_DIAG`: Yêu cầu chiếm quyền điều khiển thủ công.
  * `2 = HEARTBEAT`: Gia hạn bộ đếm thời gian thuê (Lease) về giá trị mặc định (3000 ms).
  * `3 = EXIT_DIAG`: Nhả quyền chẩn đoán, trả quyền cho Rule Engine.
  * `4 = COMMIT_RETAIN`: Ghi toàn bộ dữ liệu Retain từ RAM vào bộ nhớ Flash.
  * `5 = DISCARD_RETAIN`: Nạp lại dữ liệu Retain từ Flash vào RAM, xóa cờ Dirty.
* **`0x0A21`**: `DIAG_STATE` (RO, đọc bằng FC03):
  * `1 = ENGINE_RUNNING`: Chế độ tự động bình thường.
  * `2 = DIAG_CONTROL`: Chế độ chẩn đoán thủ công (Studio nắm quyền).
  * `3 = TRANSITIONING`: Đang chuyển tiếp an toàn tại ranh giới chu kỳ quét.
  * `4 = FAULT`: Lỗi phần cứng hoặc lỗi nghiêm trọng (khóa toàn bộ lệnh).
* **`0x0A22`**: `DIAG_FLAGS` (RO, đọc bằng FC03):
  * `Bit 0` (`0x0001`): `RETAIN_DIRTY` (1 = Dữ liệu Retain trong RAM chưa ghi vào Flash).
  * `Bit 1` (`0x0002`): `LEASE_ACTIVE` (1 = Bộ đếm thời gian thuê đang chạy).
* **`0x0A23`**: `DIAG_LEASE_REMAINING_MS` (RO): Thời gian thuê còn lại tính bằng ms (đếm lùi từ 3000 về 0).
* **`0x0A24`**: `DIAG_ERROR_CODE` (RO):
  * `0 = NONE`, `1 = DENIED_FAULT`, `2 = LEASE_EXPIRED`, `3 = FLASH_CRC_MISMATCH`, `4 = INVALID_COMMAND`, `5 = RETAIN_DIRTY`.

### 8.2. Vòng đời phiên chẩn đoán và cơ chế bảo vệ Failsafe
1. **Thiết lập quyền**: Host ghi `ENTER_DIAG (1)` vào `0x0A20`. MCU dừng chu kỳ logic tại scan boundary kế tiếp, nạp `LeaseRemainingMs = 3000`, bật cờ `LEASE_ACTIVE`, chuyển trạng thái `DIAG_STATE = DIAG_CONTROL (2)`.
   * **Quy tắc phân định chu kỳ quét khi ở `DIAG_CONTROL`**:
     * **Dedicated Function Blocks (Timers 0x0B00, Counters 0x0B40)**, **Đồng hồ RTC (0x0810)** và **Watchdog Lease countdown (0x0A23)** **vẫn tiếp tục được cập nhật bình thường** trong mỗi chu kỳ quét để bảo toàn tính toàn vẹn thời gian thực và cho phép timeout tự động.
     * **Bảng Luật thực thi (Active Rule Table 0x0100..0x073F)** **tạm thời bị bỏ qua (suspended)**, nhường toàn quyền điều khiển I/O cho kỹ sư. Điều này ngăn chặn hiện tượng tranh chấp ghi đè (Race Condition) làm mất giá trị mà kỹ sư đang cưỡng bức thủ công.
2. **Duy trì Heartbeat**: Studio định kỳ mỗi 1000 ms gửi lệnh `HEARTBEAT (2)` vào `0x0A20`. MCU nạp lại `LeaseRemainingMs = 3000`.
3. **Failsafe khi đứt kết nối (Lease Expiry)**:
   * Nếu cáp bị rút, Studio bị đóng đột ngột hoặc mất kết nối, trong mỗi chu kỳ quét 10ms MCU trừ `LeaseRemainingMs -= 10`.
   * Khi `LeaseRemainingMs <= 0`:
     * MCU tự động chuyển `DIAG_STATE = ENGINE_RUNNING (1)`.
     * Tắt cờ `LEASE_ACTIVE`, gán mã lỗi `DIAG_ERROR_CODE = LEASE_EXPIRED (2)`.
     * **Bảo vệ ngõ ra an toàn (Failsafe DO)**: MCU lập tức đưa toàn bộ các ngõ ra Digital Outputs `DO0..DO(do_count-1)` (từ chỉ số `TagLayoutMap.DoBase` đến `DoBase + do_count - 1`, địa chỉ thanh ghi tương ứng từ `0x0900 + DoBase * 2`) về mức an toàn `0` để ngăn ngừa sự cố chập cháy hoặc tai nạn máy công nghiệp.
4. **Bảo vệ Retain Dirty Interlock**:
   * Nếu kỹ sư thay đổi giá trị của `VREG_RETAIN` trong lúc test, MCU bật cờ `RETAIN_DIRTY`.
   * Nếu Host gửi `EXIT_DIAG (3)` khi `RETAIN_DIRTY == 1`, MCU **từ chối thoát** và gán mã lỗi `RETAIN_DIRTY (5)`. Host bắt buộc phải gửi lệnh `COMMIT_RETAIN (4)` để ghi Flash hoặc `DISCARD_RETAIN (5)` để hủy bỏ.

---

## 9. Quy trình Nạp Rule Nguyên tử (Atomic Staging & Commit Flow)

Để ngăn ngừa tình trạng nạp dở dang gây treo máy hoặc vận hành sai logic cơ cấu chấp hành, quá trình deploy Rule tuân thủ nghiêm ngặt 4 bước nguyên tử:

```text
Host App (SimplePLC Studio)                      MCU Firmware
   │                                                 │
   ├─ 1. FC16 Ghi 0x9002 (RuleCount) & 0x9003(CRC) ─>│ MCU chuyển StagingStatus = BUSY (2)
   │                                                 │
   ├─ 2. FC16 Ghi các block Rules vào 0x9010 ───────>│ Lưu vào RAM đệm StagingBuffer
   │     (Mỗi frame tối đa 120 registers)            │
   │                                                 │
   ├─ 3. FC06 Ghi 0xA5A5 vào 0xA000 (COMMIT) ───────>│ a) Tính CRC-16 trên toàn bộ Staging RAM
   │                                                 │ b) So khớp với ExpectedCRC (0x9003)
   │                                                 │ c) Nếu KHỚP:
   │                                                 │    - Hoán đổi con trỏ (Atomic Pointer Swap)
   │                                                 │    - Lưu bảng Rule vào Sector Flash
   │                                                 │    - Tăng ActiveRuleVersion (0xA001) += 1
   │                                                 │    - Set StagingStatus = DONE (3)
   │                                                 │ d) Nếu LỆCH CRC:
   │                                                 │    - Giữ nguyên Active Rules cũ đang chạy
   │                                                 │    - Set StagingStatus = ERROR (4)
   │                                                 │    - Set ErrorCode = CRC_MISMATCH (4)
   │                                                 │
   └─ 4. FC03 Đọc 0x9000 kiểm tra kết quả ──────────>│ Trả về DONE (3) hoặc ERROR (4)
```

---

## 10. Lưu trữ Bền vững Non-Volatile Flash Retain

* Vùng `VREG_RETAIN0 .. VREG_RETAIN31` (Tags 84..115, địa chỉ `0x09A8..0x09E7`) được lưu trữ tại phân vùng Flash/EEPROM/Fram chuyên dụng.
* **Cấu trúc Sector Flash chuẩn**:
  * `MagicWord` (4 bytes): `0x53504C43` (ASCII `"SPLC"`).
  * `DataVersion` (2 bytes): Số phiên bản dữ liệu lưu trữ.
  * `PayloadLength` (2 bytes): 128 bytes (32 tags × 4 bytes).
  * `Payload` (128 bytes): Giá trị nhị phân của 32 thanh ghi Retain.
  * `PayloadCRC16` (2 bytes): CRC-16 Modbus tính trên toàn bộ 128 bytes payload.
* **Điều kiện duy trì**:
  * Dữ liệu phải tồn tại nguyên vẹn qua chu kỳ mất nguồn đột ngột (Power-cycle) và lệnh phần mềm Reset (`SPLC_SYS_CMD_REBOOT`).
  * Chỉ được xóa về `0` khi Host gửi lệnh `SPLC_SYS_CMD_CLEAR_RETAIN (4)` hoặc `FACTORY_RESET (2)` vào thanh ghi `0x0A00`.

---

## 11. Định thời & Xử lý Ngoại lệ Modbus (Timing & Exception Handling)

### 11.1. Yêu cầu Timing trên đường truyền RS-485
* **Khoảng lặng `t_1.5`**: Tối thiểu 1.5 lần thời gian truyền 1 ký tự để phát hiện lỗi frame.
* **Khoảng lặng `t_3.5`**: Tối thiểu 3.5 lần thời gian truyền 1 ký tự giữa 2 frame liên tiếp.
* Ở baudrate > 19200 bps: Áp dụng thời gian cố định theo chuẩn:
  `t_1.5 = 750 µs`, `t_3.5 = 1.75 ms`.

### 11.2. Mã lỗi Modbus Exception chuẩn
Nếu Host gửi yêu cầu không hợp lệ, MCU phản hồi frame ngoại lệ chuẩn:
```text
[SlaveID, FC | 0x80, ExceptionCode, CRC_Lo, CRC_Hi]
```

* **`0x01 (Illegal Function)`**: Khi nhận FC ngoài 0x03, 0x06, 0x16.
* **`0x02 (Illegal Data Address)`**: Địa chỉ thanh ghi vượt ngoài bản đồ cho phép, hoặc cố gắng ghi vào Tag Store (0x0900) khi đang ở trạng thái `ENGINE_RUNNING`.
* **`0x03 (Illegal Data Value)`**: Số lượng registers yêu cầu vượt quá giới hạn (ví dụ đọc > 125 registers trong 1 FC03 frame) hoặc ghi số lẻ registers vào vùng Tag 32-bit.
* **`0x04 (Slave Device Failure)`**: Lỗi phần cứng không thể đọc/ghi bộ nhớ Flash khi commit.

---

## 12. Bảng Kiểm Tra Tuân Thủ Toàn Diện (Conformance Checklist CHKL-01 .. CHKL-14)

Trước khi xuất xưởng firmware hoặc tích hợp cùng SimplePLC Studio, firmware MCU bắt buộc phải vượt qua 100% các tiêu chí sau:

- [ ] **CHKL-01: Header Compilation**: Biên dịch firmware với [`simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h) không có cảnh báo padding hoặc alignment trên trình biên dịch GCC / Clang / Keil ARMCC.
- [ ] **CHKL-02: Device Descriptor Check**: Đọc `0x0000` (10 regs) trả về đúng `Class=1`, `Variant=1`, `ProtocolVersion=2`, `RuleFormatVersion=7`.
- [ ] **CHKL-03: Device Resource Info Check**: Đọc `0x0020` (10 regs) trả về đúng `WireProfile=2`, `MaxRules=100`, `ActiveTags=124`.
- [ ] **CHKL-04: Deterministic Scan Time**: Đọc `0x0800` (10 regs) trường `scan_time_ms` nằm quanh 10 ms danh định (tuyệt đối không phải microsecond).
- [ ] **CHKL-05: Real-Time Clock Sync**: Ghi timestamp UTC và múi giờ vào `0x0810..0x0813` bằng FC16 → MCU cập nhật đúng giờ và tính toán chính xác `current_hhmm`.
- [ ] **CHKL-06: Time Window Cross-Midnight**: Nạp rule kích hoạt từ 22:00 (2200) đến 04:00 (400) → Rule kích hoạt chính xác lúc 23:00 và 02:00, không kích hoạt lúc 12:00.
- [ ] **CHKL-07: Remote IO 124 Tags**: Đọc toàn bộ 248 thanh ghi từ `0x0900` không bị Exception `0x02`.
- [ ] **CHKL-08: Tag Store Mutual Exclusion**: Ghi vào `0x0910` (DO) khi `DIAG_STATE == ENGINE_RUNNING` → MCU từ chối với Modbus Exception `0x02`.
- [ ] **CHKL-09: Diagnostic Override Lease**: Ghi `ENTER_DIAG (1)` vào `0x0A20` → Ghi đè thành công `DO0=1`. Dừng gửi heartbeat → Sau 3000ms MCU tự động đưa `DO0=0` và chuyển về `ENGINE_RUNNING`.
- [ ] **CHKL-10: IEC 61131-3 FB Subsystem**: Đọc `0x0B00` (Timers) và `0x0B40` (Counters) trả về đúng cấu trúc 8 thanh ghi mỗi khối.
- [ ] **CHKL-11: Atomic Staging & CRC Verification**: Nạp payload với CRC giả mạo → MCU từ chối commit tại `0xA000`, giữ nguyên bảng logic đang chạy.
- [ ] **CHKL-12: Non-Volatile Flash Retain**: Ghi giá trị vào `VREG_RETAIN0` (0x09A8) → Reset nguồn MCU → Khởi động lại đọc đúng giá trị đã ghi.
- [ ] **CHKL-13: Retain Dirty Protection**: Thay đổi retain trong chế độ Diag → Gửi `EXIT_DIAG` → MCU từ chối với lỗi `RETAIN_DIRTY (5)` cho đến khi gọi `COMMIT_RETAIN`.
- [ ] **CHKL-14: Golden Vectors V2.0 Conformance**: Toàn bộ các gói tin Modbus RTU của firmware khớp 100% byte-for-byte với [`golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json).