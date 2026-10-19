# SimplePLC MCU Firmware Conformance Specification (V2.0)

> [!IMPORTANT]
> **BẢN ĐẶC TẢ MASTER CHUẨN CỐT LÕI DUY NHẤT:**  
> 👉 [`docs/platform/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/platform/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md)  
> Toàn bộ kiến trúc, struct nhị phân, Memory Map V2.0, checklist nghiệm thu CHKL-01..20 và hướng dẫn code mẫu đều được quản lý tập trung tại file Master trên.

## 1. Giới thiệu & Phạm vi

Tài liệu này là đặc tả kỹ thuật chuẩn công nghiệp (**Authoritative Engineering Conformance Specification V2.0**) dành cho các kỹ sư phát triển Firmware nhúng (trên các nền tảng MCU công nghiệp như STM32F4/G4/H7, ESP32-S3, RP2040, GD32, NXP LPC, AVR,...) đảm bảo khả năng tương thích 100% với hệ sinh thái **SimplePLC** và phần mềm **SimplePLC Studio**.

Tất cả các định nghĩa kiểu dữ liệu C/C++, cấu trúc byte-exact, hằng số giao thức và thuật toán chuẩn được quy định duy nhất tại:
* C Header chuẩn V2.0: [`src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h)
* Bộ Golden Vectors V2.0: [`tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json)
* Kế thừa nền tảng: Bổ sung và tương thích toàn diện với Wire Profile 2.

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
| `0x0000..0x0009` | **Device Descriptor** | R | FC03 | 10 regs (20 B) | Nhận dạng phần cứng: Class (1: REMOTE_IO, 2: DATALOGGER, 3: GATEWAY, 4: CONTROLLER), Variant, HW/FW Version, Protocol Version (=2), Rule Format Version (=7). |
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
| `0x0D80..0x0DFF` | **MQTT Subscribe Blocks**  | R/W | FC03/FC16 | 128 regs (256 B) | Cấu hình 2 khối nhận lệnh điều khiển Inbound MQTT SUB (SPLC-SPEC-MQTT-002). Mỗi khối 64 thanh ghi (128 bytes) gồm: Control/Timing/Topic và 4 Pin Bindings. |
| `0x0E00..0x0E5F` | **MQTT Broker Configuration**| R/W | FC03/FC16 | 96 regs (192 B) | Cấu hình kết nối mạng máy chủ MQTT Broker cấp hệ thống: Host/IP (64B), Port, TLS/SSL, Client ID, Username, Password, KeepAlive. |
| `0x0F00..0x0F43` | **MQTT TLS Certificate Staging**| R/W | FC03/FC16 | 68 regs (136 B) | Vùng đệm nạp phân đoạn chứng chỉ X.509 Root CA theo khối struct 128 bytes (SPLC-SPEC-MQTT-003): Staging Buffer (`0x0F00..0x0F3F`), ACK (`0x0F40`), Error (`0x0F41`), Atomic Commit (`0x0F42`, `0xA5A5`). |
| `0x1000..0x10FF` | **Modbus Master Poller (MB_READ)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình 4 kênh Modbus RTU Master thu thập dữ liệu cảm biến ngoại vi RS-485 (SPLC-SPEC-MBM-001, mỗi kênh 64 registers = 128 bytes). |
| `0x1100..0x11FF` | **Modbus Master Writer (MB_WRITE)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình 4 kênh phát lệnh Modbus RTU Master xuất tín hiệu xuống thiết bị chấp hành (SPLC-SPEC-MBM-002, mỗi kênh 64 registers = 128 bytes). |
| `0x1200..0x12FF` | **Direct Hardware Output (DIRECT_WRITE)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình tối đa 8 khối điều khiển cọc ngõ ra số vật lý DO0..DO7 (SPLC-SPEC-DIRECT-WRITE-003, mỗi khối 16 registers = 32 bytes). |
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

Mỗi bản ghi Rule chiếm đúng 16 thanh ghi Modbus (32 bytes), định dạng byte-exact theo chuẩn đóng gói `#pragma pack(push, 1)`. 
SimplePLC V2.0 hỗ trợ cả **Chế độ Số nguyên (Legacy Int32)** và **Chế độ Số thực dấu phẩy động (IEEE-754 Single-Precision Float32 / REAL)** mà không làm thay đổi kích thước 32-byte của Wire Contract:

```c
/* Cờ thuộc tính Rule (Rule Flags) lưu tại Reg 13 High */
typedef enum {
    SPLC_RULE_FLAG_NONE       = 0x00, /* 0x00: Chế độ số nguyên Int32 (Mặc định) */
    SPLC_RULE_FLAG_FLOAT_DATA = 0x01  /* 0x01: Chế độ số thực Float32 IEEE-754 */
} SPLC_RuleFlags_t;

typedef struct SPLC_PACKED {
    union {
        int32_t  threshold_lo;    /* [Reg 0..1] Ngưỡng dưới dạng số nguyên Int32 (Big-Endian) */
        float    threshold_lo_f;  /* [Reg 0..1] Ngưỡng dưới dạng số thực IEEE-754 Float32 (khi flags & 0x01) */
    };
    union {
        int32_t  threshold_hi;    /* [Reg 2..3] Ngưỡng trên dạng số nguyên Int32 (Big-Endian) */
        float    threshold_hi_f;  /* [Reg 2..3] Ngưỡng trên dạng số thực IEEE-754 Float32 (khi flags & 0x01) */
    };
    uint32_t for_ms;              /* [Reg 4..5] Thời gian trễ / chu kỳ kích hoạt (ms) */
    union {
        int32_t  action_param;    /* [Reg 6..7] Tham số hành động Int32 */
        float    action_param_f;  /* [Reg 6..7] Tham số hành động Float32 (Hệ số góc k khi SCALE_TAG) */
    };
    uint16_t trigger_tag;         /* [Reg 8]    TagIndex kích hoạt (0..127) */
    uint16_t action_tag;          /* [Reg 9]    TagIndex tác động (0..127) */
    uint16_t guard_tag;           /* [Reg 10]   Bit 15: NEGATE (1=NOT), Bits 0..14: TagIndex (0x7FFF=None) */
    uint8_t  enabled;             /* [Reg 11 High] 1 = Kích hoạt, 0 = Vô hiệu */
    uint8_t  trigger_type;        /* [Reg 11 Low]  SPLC_TriggerType_t */
    uint8_t  compare_op;          /* [Reg 12 High] SPLC_CompareOp_t */
    uint8_t  action_type;         /* [Reg 12 Low]  SPLC_ActionType_t */
    uint8_t  flags;               /* [Reg 13 High] SPLC_RuleFlags_t (0x01 = FLOAT_DATA) */
    uint8_t  reserved[5];         /* [Reg 13 Low..15] Luôn ghi 0x00; receiver bỏ qua */
} SPLC_RuleRecord_t;
```

### 5.1. Cơ chế Chuyển đổi và Tương thích Ngược (Float vs Int32)
* Khi **`flags == 0x00` (Legacy / Integer Mode)**:
  * Toàn bộ các trường `threshold_lo`, `threshold_hi`, `action_param` được tính toán số học số nguyên 32-bit có dấu thông thường.
  * Tương thích 100% với các phiên bản Firmware và dự án SimplePLC cũ.
* Khi **`flags & 0x01 != 0` (Float32 / REAL Mode)**:
  * Ngưỡng so sánh `threshold_lo_f`, `threshold_hi_f` và tham số `action_param_f` được diễn giải dưới dạng chuẩn **IEEE-754 Single Precision (32-bit float)**.
  * Các phép toán so sánh `EQ`, `NEQ` sử dụng epsilon công nghiệp $\epsilon = 1 \times 10^{-5}$:
    * `EQ`: $|x - \text{threshold}| \le 10^{-5}$
    * `GT`: $x > \text{threshold}$
    * `BETWEEN`: $\text{threshold\_lo\_f} \le x \le \text{threshold\_hi\_f}$
  * Hỗ trợ đặt ngưỡng thập phân trực quan từ IDE (ví dụ: `55.5°C`, `-12.3 bar`).

### 5.2. Các tập Enum Logic:
* **TriggerType (`uint8_t`)**:
  * `0`: `ON_CHANGE` — Kích hoạt khi giá trị tag thay đổi so với chu kỳ trước (hoặc thỏa mãn điều kiện so sánh).
  * `1`: `ON_RISE` — Kích hoạt khi có sườn lên (0 -> 1 hoặc vượt qua ngưỡng).
  * `2`: `ON_FALL` — Kích hoạt khi có sườn xuống (1 -> 0 hoặc tụt xuống dưới ngưỡng).
  * `3`: `TIME_WINDOW` — Kích hoạt theo khung giờ thực tế (dựa vào đồng hồ RTC, luôn xử lý dạng số nguyên `HHMM`).
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
  * `7`: `SCALE_TAG` — Áp dụng công thức chuyển đổi tỉ lệ tuyến tính ($y = k \cdot x + b$):
    * **Chế độ Số nguyên (`flags == 0x00`)**:
      $$y = \left\lfloor\frac{x \times \text{action\_param}}{1000}\right\rfloor + \text{threshold\_hi}$$
    * **Chế độ Số thực Float32 (`flags & 0x01 != 0`)**:
      $$y_{\text{float}} = x_{\text{float}} \times \text{action\_param\_f} + \text{threshold\_hi\_f}$$
      Trong đó $k$ (`action_param_f`), $b$ (`threshold_hi_f`), và ngõ vào $x$ đều được tính toán theo đơn vị dấu phẩy động IEEE-754 chính xác tuyệt đối. Giá trị ngõ ra $y$ được lưu dưới dạng 32-bit float bit pattern vào thanh ghi Tag tương ứng.
    * *Ví dụ:* Cảm biến analog `0..10000 mV` chuyển đổi sang `0..100.0°C` với $k = 0.01$, offset $b = 0.0$: `action_param_f = 0.01f`, `threshold_hi_f = 0.0f`. Giá trị đọc được tại $5550 \text{ mV}$ sẽ ra đúng $55.5^\circ\text{C}$.

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

* **`0x01 (Illegal Function)`**: Khi nhận FC ngoài 0x03, 0x06, 0x10 (FC16 Write Multiple Registers).
* **`0x02 (Illegal Data Address)`**: Địa chỉ thanh ghi vượt ngoài bản đồ cho phép, hoặc cố gắng ghi vào Tag Store (0x0900) khi đang ở trạng thái `ENGINE_RUNNING`.
* **`0x03 (Illegal Data Value)`**: Số lượng registers yêu cầu vượt quá giới hạn (ví dụ đọc > 125 registers trong 1 FC03 frame) hoặc ghi số lẻ registers vào vùng Tag 32-bit.
* **`0x04 (Slave Device Failure)`**: Lỗi phần cứng không thể đọc/ghi bộ nhớ Flash khi commit.

---

## 12. Bảng Kiểm Tra Tuân Thủ Toàn Diện (Conformance Checklist CHKL-01 .. CHKL-15)

Trước khi xuất xưởng firmware hoặc tích hợp cùng SimplePLC Studio, firmware MCU bắt buộc phải vượt qua 100% các tiêu chí sau:

- [ ] **CHKL-01: Header Compilation**: Biên dịch firmware với [`simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h) không có cảnh báo padding hoặc alignment trên trình biên dịch GCC / Clang / Keil ARMCC.
- [ ] **CHKL-02: Device Descriptor Check**: Đọc `0x0000` (10 regs) trả về đúng Device Class hợp lệ (`1` = Remote I/O, `2` = Datalogger, `3` = Gateway, `4` = Controller), `ProtocolVersion=2`, `RuleFormatVersion=7`.
- [ ] **CHKL-03: Device Resource Info Check**: Đọc `0x0020` (10 regs) trả về đúng `WireProfile=2`, `MaxRules` và `ActiveTags` khớp với cấu hình profile thiết bị.
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
- [ ] **CHKL-15: MQTT IIoT Gateway & Telemetry Wire Conformance**: Đọc/ghi cấu hình Broker tại `0x0E00` (96 regs) và Telemetry Block tại `0x0C00` (96 regs) bằng FC03/FC16 đúng byte-exact theo [`splc_mqtt_wire.h`](splc_mqtt_wire.h); thanh ghi `+15` là `reserved_header` (luôn ghi/đọc `0x0000`); phản hồi trạng thái Live Broker tại `0x0E54..0x0E5F`.
- [ ] **CHKL-16: IEC 61131-3 Data Types & Native Telemetry Conformance**: Toàn bộ giá trị 32-bit tại `0x0900..0x09FF` tuân thủ union `SPLC_TagValue_t`. Các kênh số thực `REAL` (`AI`, `VREG`, `VREG_RETAIN`) lưu nhị phân chuẩn IEEE-754 Single Precision (High Word trước, Low Word sau), tuyệt đối không nhân chia $10^{dec}$; bộ phân giải JSON MQTT tự động format số thực `float` hay số nguyên `int` theo kiểu dữ liệu tự nhiên của Tag.
- [ ] **CHKL-17: Hardware Capabilities Declaration (0x0A08)**: MCU phản hồi chính xác giá trị 32-bit (2 holding registers, High Word trước) tại `0x0A08..0x0A09` theo bitmask `SPLC_HardwareCapabilities_t` (Logic, Retain, RTC, Ethernet, MQTT PUB, MQTT SUB, TLS CERT). Không gây ngoại lệ `0x02` khi đọc FC03.
- [ ] **CHKL-18: MQTT TLS/SSL Certificate Staging Conformance (0x0F00)**: Hỗ trợ nạp phân đoạn chứng chỉ số X.509 Root CA theo khối struct chuẩn 128 bytes (64 thanh ghi Modbus `SPLC_CertChunk_t`) qua Staging Buffer `0x0F00..0x0F3F`. Phản hồi ACK `0x0F40` chỉ số khối nhận thành công; kiểm tra CRC-16 của từng khối; chốt lưu Flash vĩnh viễn và nạp vào TLS stack khi nhận lệnh Atomic Commit `0x0F42` với giá trị `0xA5A5` theo [`SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md`](SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md).
- [ ] **CHKL-19: Modbus Master RTU Sensor Poller Conformance (0x1000..0x10FF)**: Hỗ trợ nạp cấu hình 4 kênh Modbus Master tại `0x1000..0x10FF` (256 regs, SPLC-SPEC-MBM-001). Thực thi máy trạng thái polling không nghẽn (Non-blocking RTOS Task), xác thực CRC-16 của frame phản hồi, phân giải 4 chuẩn Endianness (ABCD, CDAB, BADC, DCBA), cập nhật nguyên tử vào `g_tag_store` tại ranh giới chu kỳ quét 10ms và phát tín hiệu `Valid` / `Err` / `Err_Code` chính xác theo [`SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md`](SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md).

---

## 13. Hướng Dẫn Kỹ Thuật Cho Kỹ Sư Firmware: Triển Khai Chuẩn IEC 61131-3 & MQTT Telemetry

Nhằm thống nhất 100% giữa Firmware MCU (C/C++) và SimplePLC Studio (C# .NET), các kỹ sư Firmware cần tuân thủ nghiêm ngặt các quy chuẩn sau:

### 13.1. Quản lý Bộ nhớ Tag Store 32-Bit (`0x0900..0x09FF`)
* Toàn bộ các Tag trong hệ thống đều chiếm đúng **4 bytes = 2 Holding Registers**.
* Khai báo mảng Tag Store trên MCU bằng union `SPLC_TagValue_t` (đã định nghĩa trong `simpleplc_protocol_v2_0.h`):

```c
#include "simpleplc_protocol_v2_0.h"

/* Bộ nhớ Tag Store tập trung trên MCU */
static SPLC_TagValue_t g_tag_store[SPLC_MAX_RUNTIME_TAGS];

/* Đọc giá trị Tag từ Modbus request (High Word trước, Low Word sau) */
void splc_tag_write_from_modbus(uint16_t tag_index, uint16_t high_word, uint16_t low_word) {
    if (tag_index >= SPLC_MAX_RUNTIME_TAGS) return;
    g_tag_store[tag_index].words.high_word = high_word;
    g_tag_store[tag_index].words.low_word  = low_word;
}

/* Đọc giá trị Tag gửi lên Modbus FC03 response */
void splc_tag_read_to_modbus(uint16_t tag_index, uint16_t *out_high_word, uint16_t *out_low_word) {
    if (tag_index >= SPLC_MAX_RUNTIME_TAGS) {
        *out_high_word = 0;
        *out_low_word  = 0;
        return;
    }
    *out_high_word = g_tag_store[tag_index].words.high_word;
    *out_low_word  = g_tag_store[tag_index].words.low_word;
}
```

### 13.2. Quy Tắc Xử Lý Kiểu Dữ Liệu (IEC 61131-3 Data Types)
1. **`BOOL` (1-bit):**
   * Áp dụng cho: `DI0..DI7`, `DO0..DO7`, `VFLAG0..VFLAG31`, `ModbusCoil`.
   * Giá trị hợp lệ: `0` (FALSE) hoặc `1` (TRUE).
   * Trong thanh ghi 32-bit: `as_raw32 == 0` hoặc `as_raw32 == 1`.
2. **`DINT` / `INT32` (32-bit Signed Integer):**
   * Áp dụng cho: `COUNTER0..COUNTER15`, `VREG` (mặc định), `VREG_RETAIN` (mặc định), `AI` (chế độ ADC thô 0..4095).
   * Giá trị: Số nguyên có dấu bù 2 từ $-2,147,483,648$ đến $+2,147,483,647$.
   * Thao tác: Truy cập qua trường `g_tag_store[tag_idx].as_int32`.
3. **`REAL` (32-bit Floating Point IEEE-754):**
   * Áp dụng cho: `AI` (chế độ số thực đã scale °C, bar, RPM,...), `VREG` (chế độ số thực), `VREG_RETAIN` (chế độ số thực).
   * Giá trị: Số thực dấu phẩy động 32-bit chuẩn IEEE-754.
   * Thao tác: Truy cập trực tiếp qua trường `g_tag_store[tag_idx].as_real`.
   * **Lưu ý cốt lõi:** Tuyệt đối không nhân chia hệ số $10^{dec}$ tạm bợ vào thanh ghi Modbus. Client và Server trao đổi trực tiếp biểu diễn nhị phân 32-bit của số thực.

### 13.3. Quy Chuẩn Xuất Bản MQTT Telemetry (`0x0C00`)
1. **Khắc phục cấu trúc thanh ghi:**
   * `Offset +15`: Được chuẩn hóa thành `reserved_header` (giá trị mặc định `0x0000`). Firmware **không** đọc trường này để lấy số chữ số lẻ nữa.
2. **Thuật toán Phân giải Placeholder JSON:**
   Khi sinh chuỗi JSON từ `payload_template`:
   * Khi gặp `{val1}`..`{val8}` hoặc `{val}`:
     * Nếu Tag cấu hình là `REAL`: Sử dụng `snprintf(buf, len, "%.4g", tag.as_real)` (loại bỏ số 0 thừa ở đuôi, ví dụ `24.5` thay vì `24.5000`).
     * Nếu Tag cấu hình là `DINT`/`INT32`: Sử dụng `snprintf(buf, len, "%ld", (long)tag.as_int32)`.
     * Nếu Tag cấu hình là `BOOL`: Sử dụng `snprintf(buf, len, "%d", tag.as_int32 ? 1 : 0)`.
3. **Bảo vệ Bộ nhớ Đệm:**
   * Giới hạn `topic`: Tối đa 32 bytes (đã bao gồm byte null).
   * Giới hạn `payload_template`: Tối đa 128 bytes.
   * Giới hạn buffer xuất bản: Khuyến nghị buffer truyền nhận MQTT tối thiểu 256 bytes để đủ dung lượng ghép chuỗi JSON hoàn chỉnh.

### 13.4. Khai Báo Năng Lực Phần Cứng `HardwareCapabilities` (`0x0A08`)
* Địa chỉ: `0x0A08` (32-bit uint32, 2 holding registers Big-Endian: `0x0A08` High Word, `0x0A09` Low Word, FC03 Read-Only).
* Bitmask định nghĩa (`SPLC_HardwareCapabilities_t` trong `simpleplc_protocol_v2_0.h`):
  * `0x0001` (`SPLC_CAP_BASE_LOGIC`): Hỗ trợ Rule Engine FBD.
  * `0x0002` (`SPLC_CAP_RETENTIVE`): Hỗ trợ lưu trữ Flash/NVRAM/FRAM.
  * `0x0004` (`SPLC_CAP_RTC`): Có IC RTC DS3231 hoặc LSE crystal.
  * `0x0008` (`SPLC_CAP_ETHERNET`): Có cổng mạng vật lý (LAN/WiFi).
  * `0x0010` (`SPLC_CAP_MQTT_PUB`): Hỗ trợ phân hệ MQTT Telemetry (0x0C00).
  * `0x0020` (`SPLC_CAP_MQTT_SUB`): Hỗ trợ nhận lệnh điều khiển Inbound MQTT SUB (0x0D80).
  * `0x0040` (`SPLC_CAP_SD_DATALOG`): Có khe cắm thẻ nhớ MicroSD.
  * `0x0080` (`SPLC_CAP_MQTT_TLS_CERT`): Hỗ trợ nạp và giải mã chứng chỉ bảo mật TLS/SSL theo khối struct (0x0F00, SPLC-SPEC-MQTT-003).
  * `0x0100` (`SPLC_CAP_MODBUS_MASTER`): Hỗ trợ phân hệ RS-485 Modbus Master Poller (`MB_READ`, 0x1000..0x10FF, SPLC-SPEC-MBM-001).

* **Quy chuẩn Fail-Safe khi Deploy từ Host (SynaptiX IDE / SimplePLC Studio):**
  1. **Bắt buộc đọc kiểm tra năng lực trước khi ghi cấu hình nâng cao:**
     - Khi thực hiện nạp chương trình (`DeployRulesAsync`), Host luôn gửi lệnh FC03 đọc 2 thanh ghi tại `0x0A08..0x0A09` để xác định chính xác năng lực thực tế của phần cứng.
     - Host ghi log công khai: `[CAPS] Read 0x0A08 = 0x... (MQTT_PUB=True/False, MQTT_SUB=True/False)`.
  2. **Cơ chế Fail-Fast chặn nạp ảo (No Silent Failure):**
     - Nếu dự án có sử dụng khối `MQTT_PUB`, `MQTT_SUB` hoặc kích hoạt MQTT Broker:
       - **Lỗi giao tiếp (Timeout / CRC / Mất kết nối):** Host tuyệt đối không nuốt lỗi; lập tức hủy bỏ quy trình Deploy, không ghi Staging, không commit `0xA000`, báo Deploy THẤT BẠI kèm thông báo chi tiết: `[CAPS] Failed to read hardware capabilities from 0x0A08: <lý do>`.
       - **Thiếu năng lực phần cứng:** Nếu đọc `0x0A08` thành công nhưng firmware không bật cờ `SPLC_CAP_MQTT_PUB (0x0010)` (hoặc `SPLC_CAP_MQTT_SUB (0x0020)`), Host từ chối nạp ngay lập tức, trả về mã lỗi chuẩn `UNSUPPORTED (0x0002)`, báo rõ lý do thiết bị không hỗ trợ MQTT.
  3. **Tương thích ngược đối với bo mạch Legacy:**
     - Nếu dự án **KHÔNG** sử dụng khối MQTT:
       - Trường hợp bo mạch phần cứng cũ không hỗ trợ thanh ghi `0x0A08` và phản hồi Modbus Exception `0x02 (Illegal Data Address)`, Host mặc định thiết bị sở hữu năng lực cơ bản (`SPLC_CAP_BASE_LOGIC`), ghi nhận cảnh báo và tiếp tục quy trình nạp logic FBD bình thường mà không làm gián đoạn vận hành.
  4. **Quy chuẩn đối soát thời gian thực (Traceable Logging):**
     - Trước mỗi frame ghi Modbus FC16 xuống các khối MQTT, Host ghi log rõ địa chỉ và số lượng thanh ghi:
       - `[MQTT PUB] Writing 96 registers to 0x0C00 (Block 0)...`
       - `[MQTT SUB] Writing 64 registers to 0x0D80 (Block 0)...`
       - `[MQTT Broker] Writing 96 registers to 0x0E00...`
     - Firmware xử lý xong sẽ ghi log tương ứng (`broker write`, `app_configured=1`, `generation=...`) giúp kỹ sư bảo trì đối chiếu 1-1 giữa Host và MCU.

### 13.5. Quy Chuẩn Nhận Lệnh Điều Khiển MQTT Subscribe (`0x0D80..0x0DFF`)
* Chi tiết đặc tả xem tài liệu: [`SPLC-SPEC-MQTT-002_SUBSCRIBE_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_002_SUBSCRIBE_ICD.md).
1. **Phân vùng thanh ghi:**
   * `0x0D80 - 0x0DFF` (128 registers = 256 bytes): Hỗ trợ tối đa 2 khối Subscribe độc lập (mỗi khối 64 registers = 128 bytes).
   * Block 0: `0x0D80..0x0DBF`, Block 1: `0x0DC0..0x0DFF`.
2. **Cấu trúc mỗi khối Subscribe:**
   * `Offset +0`: Control Flags (Enable, QoS, QueueMode, IgnoreRetained, FailSafeAction).
   * `Offset +2..+3`: FailSafe Timeout tính bằng mili-giây (32-bit uint32).
   * `Offset +4`: Pin Count (1..4).
   * `Offset +8..+23`: Topic (32 bytes ASCII null-padded).
   * `Offset +24..+63`: 4 Pin Bindings (mỗi pin 10 registers = 20 bytes: Target TagIndex, DataType, DefaultValue, 12 bytes JsonKey).
3. **Nguyên tắc an toàn thời gian thực:**
   * Tuyệt đối không ghi trực tiếp vào `g_tag_store` trong callback MQTT.
   * Sử dụng FreeRTOS Queue để đệm bản tin.
   * Chỉ cập nhật vào `g_tag_store` tại đầu chu kỳ quét (Scan Pass Boundary) 10ms của PLC Engine để tránh xung đột dữ liệu (Thread Safety).
   * Kiểm tra Fail-Safe Watchdog: Nếu quá `failsafe_timeout_ms` không nhận được tin, tự động đưa các chân tín hiệu về giá trị an toàn (Reset về 0 hoặc Set về Default).

### 13.6. Quy Chuẩn Nạp Chứng Chỉ TLS/SSL Certificate Theo Khối Struct (`0x0F00`)
* Chi tiết đặc tả xem tài liệu: [`SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md).
1. **Phân vùng thanh ghi:**
   * `0x0F00..0x0F3F` (64 registers = 128 bytes): Vùng đệm Staging nhận khối struct `SPLC_CertChunk_t`.
   * `0x0F40`: Thanh ghi phản hồi ACK `chunk_index` vừa lưu thành công.
   * `0x0F41`: Trạng thái lỗi (0=OK, 1=Lỗi CRC, 2=Tràn bộ nhớ, 3=Lỗi ghi Flash).
   * `0x0F42`: Lệnh chốt nguyên tử (ghi `0xA5A5` để MCU lưu vào Flash và nạp vào mbedTLS/WolfSSL).
   * `0x0F43`: Mặt nạ loại chứng chỉ đang kích hoạt (Bit 0: Root CA, Bit 1: Client Cert, Bit 2: Client Key).
2. **Cấu trúc nhị phân `SPLC_CertChunk_t` (128 bytes, `#pragma pack(1)`):**
   * `cert_type` (1B), `chunk_index` (1B), `total_chunks` (1B), `chunk_len` (1B), `total_cert_size` (2B), `chunk_crc16` (2B), `payload[120]` (120B).
3. **Nguyên tắc triển khai trên Firmware:**
   * MCU nhận từng khối qua Modbus FC16, tính toán CRC-16 của payload khối đó (đa thức Modbus `0xA001`).
   * Nếu CRC hợp lệ, lưu vào RAM buffer tạm và cập nhật thanh ghi ACK `0x0F40 = chunk_index`.
   * Khi nhận lệnh Commit `0xA5A5` tại `0x0F42`, MCU kiểm tra tổng kích thước đã nhận so với `total_cert_size`, lưu toàn bộ chứng chỉ vào Flash và khởi tạo context SSL/TLS.

### 13.7. Quy Chuẩn Thu Thập Dữ Liệu Ngoại Vi Modbus Master RTU (`0x1000..0x10FF`)
* Chi tiết đặc tả xem tài liệu: [`SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md) và [`SimplePLC_App_MCU_Structs_Modbus_Master_Profile.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/platform/SimplePLC_App_MCU_Structs_Modbus_Master_Profile.md).
1. **Phân vùng thanh ghi:**
   * `0x1000 - 0x10FF` (256 registers = 512 bytes): 4 kênh Modbus RTU Master độc lập (mỗi kênh 64 registers = 128 bytes).
   * Kênh 0: `0x1000..0x103F`, Kênh 1: `0x1040..0x107F`, Kênh 2: `0x1080..0x10BF`, Kênh 3: `0x10C0..0x10FF`.
2. **Cấu trúc mỗi kênh Modbus Master:**
   * `Offset +0`: Control Flags (Bit 0: Enable).
   * `Offset +1`: Function Code (High Byte) | Slave ID (Low Byte).
   * `Offset +2`: Starting Address (Địa chỉ thanh ghi/coil bắt đầu trên Slave).
   * `Offset +3`: Quantity of Registers (1..125, số lượng phần tử cần đọc).
   * `Offset +4..+5`: Poll Interval (ms, uint32 Big-Endian).
   * `Offset +6..+7`: Timeout (ms, uint32 Big-Endian).
   * `Offset +8..+10`: Valid Tag Index, Error Tag Index, ErrCode Tag Index (0xFFFF nếu không nối).
   * `Offset +11`: Pin Count (0..4).
   * `Offset +16..+63`: 4 Dynamic Pin Bindings (mỗi pin 12 registers = 24 bytes: TargetTagIndex, RegOffset, DataType, Endianness, Multiplier, PinLabel 12 bytes Packed ASCII).
3. **Nguyên tắc vận hành trên Firmware:**
   * Task giao tiếp RS-485 chạy ngầm không nghẽn chu kỳ quét 10ms.
   * Tính toán và đối chiếu mã kiểm tra CRC-16 trên từng frame phản hồi.
   * Tự động chuyển đổi thứ tự byte theo cấu hình Endianness (`ABCD`, `CDAB`, `BADC`, `DCBA`) và nhân hệ số `Multiplier`.
   * Cập nhật nguyên tử vào `g_tag_store` tại ranh giới chu kỳ quét, phát xung mức 1 cho chân `Valid` và xuất mã chẩn đoán `Err_Code` (1=Timeout, 2=CRC, 3=Exception, 4=Malformed).

### 13.8. Quy Chuẩn Điều Khiển Trực Tiếp Cọc Ngõ Ra Vật Lý DIRECT_WRITE (`0x1200..0x12FF`)
* Chi tiết đặc tả xem tài liệu: [`SPLC_SPEC_DIRECT_WRITE_003_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_DIRECT_WRITE_003_ICD.md).
1. **Phân vùng thanh ghi:**
   * `0x1200 - 0x12FF` (256 registers = 512 bytes): Tối đa 8 khối cấu hình độc lập (mỗi khối 16 registers = 32 bytes, định dạng nhị phân `#pragma pack(1)`).
   * Khối $k$ ($0 \le k \le 7$) bắt đầu tại địa chỉ $\text{Address} = 0\text{x}1200 + (k \times 16)$.
2. **Cấu trúc nhị phân `SPLC_DirectWriteBlock_t` (32 bytes):**
   * `control_flags` (2B): Bit 0: Enabled, Bit 1: IsExclusiveChannel, Bit 2: ActiveHigh.
   * `target_pin` (1B): Cọc ngõ ra số vật lý mục tiêu `DO0..DO7` (`SPLC_DirectPin_t`: `0=DO0` .. `7=DO7`). Toàn hệ thống sử dụng duy nhất chuẩn cọc số DO.
   * `mode` (1B): Chế độ hoạt động (`0=DIGITAL_OUTPUT`, `1=PWM_DUTY`, `2=PULSE_TRAIN`, `3=ADDRESSABLE_BUS`).
   * `trig_tag_index` (2B): TagIndex xung kích hoạt (0xFFFF nếu không nối, chạy liên tục).
   * `channel_tag_index` (2B): TagIndex kênh chọn (Channel) hoặc chu kỳ xung Duty / Tần số.
   * `param_tag_index` (2B): TagIndex tham số phụ trợ (Màu sắc, số xung PTO, v.v.).
   * `channel_count` (2B): Tổng số kênh hoặc số lượng phần tử chấp hành (1..256).
   * `pulse_width_us` (4B): Độ rộng xung micro-giây cho PTO / Trigger pulse.
   * `pwm_frequency_hz` (4B): Tần số băm xung PWM phần cứng (Hz, uint32 Big-Endian).
   * `trigger_mode` (1B): Chế độ kích hoạt (`0=OnTriggerRisingEdge`, `1=Continuous`).
   * `failsafe_state` (1B): Trạng thái an toàn ngõ ra khi mất tín hiệu hoặc dừng Engine (0=Mức thấp, 1=Mức cao).
   * `default_color_rgb` (4B): Màu sắc mặc định định dạng 24-bit 0x00RRGGBB (chế độ Bus Addressable).
   * `timeout_ms` (4B): Thời gian Watchdog an toàn (ms, uint32 Big-Endian).
   * `reserved` (2B): Dự phòng căn chỉnh 32 bytes.
3. **Nguyên tắc vận hành thời gian thực trên MCU:**
   * **Định danh cọc ngõ ra**: Thiết bị chỉ sở hữu duy nhất các cọc ngõ ra số `DO0..DO7`. Firmware ánh xạ trực tiếp `target_pin` vào GPIO/Timer phần cứng tương ứng của MCU (STM32 HAL GPIO/TIM, ESP32 GPIO/LEDC/RMT).
   * **Thực thi Deterministic**: Việc xuất xung sườn lên hoặc cập nhật PWM Duty được xử lý ngay tại scan pass 10ms của PLC Engine, đảm bảo phản hồi tức thì dưới 1ms mà không gây nghẽn CPU.
   * **Fail-Safe Watchdog**: Nếu cấu hình `timeout_ms > 0` và không có xung kích hoạt mới quá thời gian này, ngõ ra sẽ tự động rơi về trạng thái `failsafe_state`.



