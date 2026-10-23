# SimplePLC Platform V2.0 — App-to-MCU Binary Structs & Self-Describing Device Profile Master Specification

**Document ID:** `SPLC-DOC-SPEC-V20-MASTER`  
**Standard Release:** SimplePLC Platform V2.0 (Superset of Wire Profile V1.9)  
**Protocol Version:** `2`  
**Rule Format Version:** `7` (32-byte canonical records)  
**Wire Profile:** `2` (Diagnostic Control, IEC 61131-3 FBs, MQTT IIoT, Modbus Master & Direct Write)  
**Target Audience:** Senior Embedded Firmware Engineers (STM32, ESP32, RP2040, GD32), System Integrators & Automation Engineers  

> [!IMPORTANT]
> **TÀI LIỆU CHUẨN CỐT LÕI DUY NHẤT (AUTHORITATIVE MASTER SPECIFICATION)**  
> Đây là bản đặc tả kỹ thuật chuẩn tổng quan duy nhất của nền tảng SimplePLC Platform V2.0. Toàn bộ kiến trúc, bản đồ thanh ghi Modbus, cấu trúc nhị phân C, quy chuẩn an toàn và checklist nghiệm thu đều được quy định tại đây. Khi cần triển khai sâu vào driver từng phân hệ chuyên biệt, kỹ sư tham khảo các tài liệu ICD chi tiết theo danh mục liên kết bên dưới.

### Tài Liệu & Mã Nguồn Giao Thức Chuẩn Đi Kèm:
* **Tệp C/C++ Header Nhúng Duy Nhất (Single Authoritative C Header):**  
  [`src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h) (Chứa 100% struct byte-exact `#pragma pack(push, 1)`, hằng số thanh ghi và macro kiểm tra tĩnh `_Static_assert`).
* **Bộ Kiểm Thử Đối Chiếu Byte-Exact (Golden Test Vectors V2.0):**  
  [`tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json)
* **Tập Tài Liệu Đặc Tả Giao Diện Kỹ Thuật Chuyên Sâu (Subsystem Detailed ICDs):**
  1. *Xuất bản Đo xa MQTT (0x0C00):* [`docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md)
  2. *Nhận lệnh Điều khiển MQTT (0x0D80):* [`docs/firmware/SPLC_SPEC_MQTT_002_SUBSCRIBE_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_002_SUBSCRIBE_ICD.md)
  3. *Nạp Chứng chỉ TLS Root CA (0x0F00):* [`docs/firmware/SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_003_TLS_CERTIFICATE_WIRE_ICD.md)
  4. *Đọc Cảm biến Modbus Master (0x1000):* [`docs/firmware/SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md)
  5. *Xuất Lệnh Chấp hành Modbus Master (0x1100):* [`docs/firmware/SPLC_SPEC_MODBUS_MASTER_002_WRITE_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MODBUS_MASTER_002_WRITE_ICD.md)
  6. *Điều khiển Cọc DO Trực tiếp Addressable Bus (0x1200):* [`docs/firmware/SPLC_SPEC_DIRECT_WRITE_003_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_DIRECT_WRITE_003_ICD.md)

---

## 1. Mục đích & Nguyên lý Thiết kế (Executive Summary & Paradigm)

Tài liệu này xác lập đặc tả cấu trúc dữ liệu nhị phân cấp thấp (**Byte-Exact Binary Structs**), bố cục bộ nhớ thanh ghi Modbus (**Holding Register Layout**) và khế ước hồ sơ tự mô tả (**Self-Describing Device Profile Contract**) giữa phần mềm máy tính **SimplePLC Studio** (Host Modbus Master) và các bộ vi điều khiển nhúng **MCU** (Modbus Slave).

```text
+-----------------------------------------------------------------------------------+
|                         SimplePLC Studio (Host Application)                       |
+-----------------------------------------------------------------------------------+
                                         ▲
                                         │ Standard Modbus RTU (FC03, FC06, FC16)
                                         │ Wire Endianness: 16-bit Big-Endian
                                         │ 32-bit: [High16, Low16] | CRC16: Little-Endian
                                         ▼
+-----------------------------------------------------------------------------------+
|                  MCU Firmware Engine (STM32, ESP32, RP2040, GD32)                 |
|   ┌───────────────────────────────┐     ┌─────────────────────────────────────┐   |
|   │ DeviceDescriptor (0x0000)     │     │ Diagnostic Override Engine (0x0A20) │   |
|   │ Protocol V2, RuleFormat V7    │     │ 3000ms Lease Watchdog & Failsafe DO │   |
|   ├───────────────────────────────┤     ├─────────────────────────────────────┤   |
|   │ Self-Describing Info (0x0020) │     │ IEC 61131-3 Timers/Counters (0x0B00)│   |
|   │ Wire Profile V2, Dynamic Tags │     │ TON, TOF, TP, CTU, CTD Subsystem    │   |
|   ├───────────────────────────────┤     ├─────────────────────────────────────┤   |
|   │ Real-Time Clock RTC (0x0810)  │     │ Atomic Staging & Commit (0x9000)    │   |
|   │ Epoch UTC + Timezone Offset   │     │ Double-Buffer RAM & Flash Retain    │   |
|   ├───────────────────────────────┼─────────────────────────────────────┤   |
|   │ MQTT Broker Config (0x0E00)   │     │ MQTT Telemetry Blocks (0x0C00)      │   |
|   │ Host, Port, TLS, Credentials  │     │ 4x Blocks, COV Deadband, JSON Mẫu   │   |
|   ├───────────────────────────────┼─────────────────────────────────────┤   |
|   │ Modbus Master Poller (0x1000) │     │ MQTT Inbound Subscribe (0x0D80)     │   |
|   │ 4x Channels, RS-485 Sensors   │     │ 2x Blocks, 4 Pins, FailSafe Watchdog│   |
|   └───────────────────────────────┴─────────────────────────────────────┘   |
+-----------------------------------------------------------------------------------+
```

### Các nguyên tắc bất biến cấp kiến trúc (Architectural Invariants)

1. **MCU là Nguồn Chân lý Duy nhất cho Năng lực Thiết bị (Single Source of Truth)**:
   Host Application tuyệt đối không hard-code số lượng chân hay cấu hình I/O trong mã nguồn. Khi kết nối, Host đọc khối `DeviceResourceInfo` (`0x0020..0x0029`) để tự động sinh ra danh mục Tag (`TagCatalog`) tương thích 100% với phần cứng thực tế.
2. **Tuân thủ Chuẩn Modbus RTU Thuần túy (Zero Proprietary Function Codes)**:
   Chỉ sử dụng 3 mã hàm Modbus công nghiệp tiêu chuẩn:
   * **`FC03 (0x03)`**: Read Holding Registers
   * **`FC06 (0x06)`**: Write Single Register
   * **`FC16 (0x10)`**: Write Multiple Registers
3. **Quy ước Thứ tự Byte & Từ trên Đường truyền (Wire Endianness & Word Ordering)**:
   * Thanh ghi 16-bit: **Big-Endian** (Byte cao truyền trước, Byte thấp truyền sau).
   * Giá trị 32-bit (`int32_t`, `uint32_t`): Chiếm 2 thanh ghi liên tiếp, **High Word truyền trước, Low Word truyền sau** (`[Reg0: High16, Reg1: Low16]`).
   * Mã kiểm tra frame Modbus CRC-16 (Đa thức `0xA001`, Init `0xFFFF`): **Little-Endian** ở 2 byte cuối cùng của frame RTU (`[CRC_Lo, CRC_Hi]`).
4. **Đóng gói Byte-Exact Tuyệt đối (Strict Packing)**:
   Mọi cấu trúc dữ liệu C/C++ đều phải sử dụng chỉ thị `#pragma pack(push, 1)` và được kiểm tra kích thước tĩnh tại thời điểm biên dịch bằng `_Static_assert` (C11) hoặc `static_assert` (C++11).

---

## 2. Bản đồ Bộ nhớ Thanh ghi Modbus Toàn diện (Master Memory Map V2.0)

| Vùng thanh ghi | Tên phân vùng | Chiều | FC | Kích thước | Mô tả kiến trúc V2.0 |
| :--- | :--- | :---: | :---: | :--- | :--- |
| `0x0000..0x0009` | **Device Descriptor** | R | FC03 | 10 regs (20 B) | Thông tin định danh: Class=1, Variant=1, HW, FW, ProtocolVersion=2, RuleFormatVersion=7. |
| `0x0010` | **Rule Table Info** | R | FC03 | 1 reg (2 B) | Số lượng Rule đang chạy (`active_rule_count`, 0..100). |
| `0x0020..0x0029` | **Device Resource Info** | R | FC03 | 10 regs (20 B) | Hồ sơ tự mô tả: WireProfile=2, MaxRules=100, ActiveTags=124, 8DI/8DO/4AI/32VFLAG/32VREG/32RETAIN/8COUNTER. |
| `0x0100..0x073F` | **Active Rule Table** | R | FC03 | Max 1600 regs | Bảng Rule logic thực thi (tối đa 100 rules × 16 thanh ghi = 3200 bytes). |
| `0x0800..0x0809` | **Device Health** | R | FC03 | 10 regs (20 B) | Uptime (s), Reset Reason, Health Flags, CPU load, RAM load, Scan Time (ms), Max Scan Time (ms). |
| `0x0810..0x0813` | **Real-Time Clock (RTC)** | R/W | FC03/FC16 | 4 regs (8 B) | Đồng hồ thời gian thực: `epoch_utc_s` (u32), `tz_offset_min` (i16), `status_flags` (u16). |
| `0x0900..0x09FF` | **Runtime Tag Values** | R/W | FC03/FC16 | 256 regs (128 tags)| Giá trị nhị phân 32-bit của 128 Tags. Chỉ được phép ghi khi ở trạng thái `DIAG_CONTROL`. |
| `0x0A00` | **System Command** | W | FC06/FC16 | 1 reg (2 B) | Lệnh bảo dưỡng: REBOOT (1), FACTORY_RESET (2), CLEAR_RULES (3), CLEAR_RETAIN (4). |
| `0x0A01..0x0A02` | **System Command Result** | R | FC03 | 2 regs (4 B) | Kết quả lệnh bảo dưỡng: `[0]=CommandStatus`, `[1]=ErrorCode`. |
| `0x0A08..0x0A09` | **Hardware Capabilities** | R | FC03 | 2 regs (4 B) | Khai báo 32-bit năng lực phần cứng thực tế (`SPLC_HardwareCapabilities_t`, Big-Endian): Logic (0x1), Retain (0x2), RTC (0x4), Ethernet (0x8), MQTT PUB (0x10), MQTT SUB (0x20), SD (0x40), TLS CERT (0x80), Modbus Master (0x100). |
| `0x0A20..0x0A24` | **Diagnostic Control Block**| R/W | FC03/FC06/FC16| 5 regs (10 B) | Phân hệ chẩn đoán: `Command` (0x0A20), `State` (0x0A21), `Flags` (0x0A22), `LeaseMs` (0x0A23), `ErrorCode` (0x0A24). |
| `0x0B00..0x0B3F` | **FB Timers (TON/TOF/TP)**| R/W | FC03/FC16 | 64 regs (128 B) | 8 Timers IEC 61131-3, mỗi khối 8 thanh ghi. Ghi cấu hình (Mode, PT) qua FC16; đọc viễn trắc (Status, ET) qua FC03. |
| `0x0B40..0x0B7F` | **FB Counters (CTU/CTD)** | R/W | FC03/FC16 | 64 regs (128 B) | 8 Counters IEC 61131-3, mỗi khối 8 thanh ghi. Ghi cấu hình (Mode, PV, RetainTag) qua FC16; đọc viễn trắc (Status, CV) qua FC03. |
| `0x0C00..0x0D7F` | **MQTT Telemetry Blocks**  | R/W | FC03/FC16 | 384 regs (768 B) | Cấu hình 4 khối xuất bản dữ liệu viễn trắc MQTT IIoT (SPLC-SPEC-MQTT-001). Mỗi khối 96 thanh ghi (192 bytes) gồm: Flags/Topic/Timing (32 regs) và Payload Template JSON (64 regs). |
| `0x0D80..0x0DFF` | **MQTT Subscribe Blocks**  | R/W | FC03/FC16 | 128 regs (256 B) | Cấu hình 2 khối nhận lệnh điều khiển Inbound MQTT SUB (SPLC-SPEC-MQTT-002). Mỗi khối 64 thanh ghi (128 bytes) gồm: Control/Timing/Topic và 4 Pin Bindings. |
| `0x0E00..0x0E5F` | **MQTT Broker Configuration**| R/W | FC03/FC16 | 96 regs (192 B) | Cấu hình kết nối mạng máy chủ MQTT Broker cấp hệ thống: Host/IP (64B), Port, TLS/SSL, Client ID, Username, Password, KeepAlive và 12 regs phản hồi trạng thái Live. |
| `0x0F00..0x0F43` | **MQTT TLS Certificate Staging**| R/W | FC03/FC16 | 68 regs (136 B) | Vùng đệm nạp phân đoạn chứng chỉ X.509 Root CA theo khối struct 128 bytes (SPLC-SPEC-MQTT-003): Staging Buffer (`0x0F00..0x0F3F`), ACK (`0x0F40`), Error (`0x0F41`), Atomic Commit (`0x0F42`, `0xA5A5`). |
| `0x1000..0x10FF` | **Modbus Master Poller (MB_READ)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình 4 kênh Modbus RTU Master thu thập cảm biến (SPLC-SPEC-MBM-001). Mỗi kênh 64 thanh ghi (128 bytes) gồm: Header cấu hình (+0..+15) và 4 Dynamic Pin Bindings (+16..+63). |
| `0x1100..0x11FF` | **Modbus Master Writer (MB_WRITE)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình 4 kênh phát lệnh Modbus RTU Master xuất tín hiệu xuống thiết bị chấp hành (SPLC-SPEC-MBM-002, mỗi kênh 64 registers = 128 bytes). |
| `0x1200..0x12FF` | **Direct Hardware Output (DIRECT_WRITE)**| R/W | FC03/FC16 | 256 regs (512 B) | Cấu hình tối đa 8 khối điều khiển cọc ngõ ra số vật lý DO0..DO7 (SPLC-SPEC-DIRECT-WRITE-003, mỗi khối 16 registers = 32 bytes). |
| `0x9000..0x9005` | **Staging Handshake** | R/W | FC03/FC16 | 6 regs (12 B) | Trạng thái bắt tay nạp Rule: Status, ErrorCode, StagedCount, ExpectedCRC, ActiveCount, ActiveCRC. |
| `0x9010..0x964F` | **Staging Rule Buffer** | W | FC16 | Max 1600 regs | RAM đệm nạp Rule mới (nạp theo từng block tối đa 120 thanh ghi). |
| `0xA000` | **Commit Command** | W | FC06/FC16 | 1 reg (2 B) | Ghi `0xA5A5` (`SPLC_COMMIT_MAGIC`) để MCU kiểm tra CRC và hoán đổi con trỏ bảng Rule. |
| `0xA001` | **Active Rule Version** | R | FC03 | 1 reg (2 B) | Phiên bản Rule đang hoạt động (tăng 1 sau mỗi lần commit thành công). |

---

## 3. Đặc tả Cấu trúc Nhị phân Byte-Exact (Binary Structs Specification)

### 3.1. `SPLC_DeviceDescriptor_t` (0x0000, 10 thanh ghi = 20 Bytes)
Khối thông tin định danh bất biến của thiết bị.

```c
typedef struct SPLC_PACKED {
    uint16_t device_class;        /* 0x00: SPLC_DeviceClass_t (1 = REMOTE_IO) */
    uint16_t device_variant;      /* 0x02: SPLC_RemoteIoVariant_t (1 = 8DI_8DO_4AI) */
    uint16_t hw_version_major;    /* 0x04: Hardware Major (e.g. 1) */
    uint16_t hw_version_minor;    /* 0x06: Hardware Minor (e.g. 0) */
    uint16_t hw_version_patch;    /* 0x08: Hardware Patch (e.g. 0) */
    uint16_t fw_version_major;    /* 0x0A: Firmware Major (e.g. 2) */
    uint16_t fw_version_minor;    /* 0x0C: Firmware Minor (e.g. 0) */
    uint16_t fw_version_patch;    /* 0x0E: Firmware Patch (e.g. 0) */
    uint16_t protocol_version;    /* 0x10: Bắt buộc = 2 trong Platform V2.0 */
    uint16_t rule_format_version; /* 0x12: Bắt buộc = 7 (Bản ghi 32-byte) */
} SPLC_DeviceDescriptor_t;
```

### 3.2. `SPLC_DeviceResourceInfo_t` (0x0020, 10 thanh ghi = 20 Bytes)
Hồ sơ tự mô tả tài nguyên nhúng của thiết bị. Căn cứ vào khối này, Host tự động thiết lập giao diện và phân bổ TagCatalog.

```c
typedef struct SPLC_PACKED {
    uint16_t wire_profile;        /* 0x00: Bắt buộc = 2 cho Wire Profile V2 */
    uint16_t max_rules;           /* 0x02: Dung lượng Rule tối đa (100) */
    uint16_t runtime_tag_count;   /* 0x04: Tổng số Tag hoạt động (124) */
    uint16_t di_count;            /* 0x06: Số cổng DI vật lý (8) */
    uint16_t do_count;            /* 0x08: Số cổng DO vật lý (8) */
    uint16_t ai_count;            /* 0x0A: Số cổng AI vật lý (4) */
    uint16_t vflag_count;         /* 0x0C: Số cờ Virtual Flag (32) */
    uint16_t vreg_count;          /* 0x0E: Số thanh ghi Virtual Register (32) */
    uint16_t vreg_retain_count;   /* 0x10: Số thanh ghi Retain Flash (32) */
    uint16_t counter_count;       /* 0x12: Số bộ đếm Counter (8) */
} SPLC_DeviceResourceInfo_t;
```

### 3.3. `SPLC_DeviceHealth_t` (0x0800, 10 thanh ghi = 20 Bytes)
Khối giám sát sức khỏe và vi sai chu kỳ quét định thời của MCU.

```c
typedef struct SPLC_PACKED {
    uint32_t uptime_s;            /* 0x00: Thời gian hoạt động tính bằng giây (High Word trước) */
    uint16_t reset_reason;        /* 0x04: SPLC_ResetReason_t (1=PowerOn, 2=SW, 3=WDG, 4=Brownout) */
    uint16_t health_flags;        /* 0x06: Bit 0: CPU High, Bit 1: RAM High, Bit 2: Scan Overrun */
    uint16_t cpu_load_percent;    /* 0x08: Tải vi xử lý: 0..100 % */
    uint16_t ram_usage_percent;   /* 0x0A: Sử dụng bộ nhớ RAM: 0..100 % */
    uint32_t scan_time_ms;        /* 0x0C: Chu kỳ quét danh định gần nhất (ms, High Word trước) */
    uint32_t max_scan_time_ms;    /* 0x10: Chu kỳ quét đỉnh lớn nhất từng ghi nhận (ms) */
} SPLC_DeviceHealth_t;
```

### 3.4. `SPLC_RtcClock_t` (0x0810, 4 thanh ghi = 8 Bytes)
Khối đồng bộ thời gian thực cho tự động hóa Time Window.

```c
typedef struct SPLC_PACKED {
    uint32_t epoch_utc_s;         /* 0x00: Unix Epoch UTC (giây tính từ 1970-01-01, High Word trước) */
    int16_t  tz_offset_min;       /* 0x04: Độ lệch múi giờ theo phút (+420 = UTC+7 Việt Nam) */
    uint16_t status_flags;        /* 0x06: Cờ trạng thái RTC */
} SPLC_RtcClock_t;
```
* **Bitmask `status_flags`**:
  * `0x0001` (`SPLC_RTC_FLAG_SYNCED`): `1` = Đã đồng bộ với Host PC; `0` = Chưa đồng bộ. (Host PC bật cờ này khi gửi giờ chuẩn).
  * `0x0002` (`SPLC_RTC_FLAG_HW_PRESENT`): `1` = Có IC phần cứng RTC rời (DS3231, PCF8563...) hoặc thạch anh 32.768kHz (LSE); `0` = Đếm giờ bằng phần mềm SysTick. *(Do MCU tự xác định lúc boot, Host PC không được tự ý xóa).*
  * `0x0004` (`SPLC_RTC_FLAG_BATTERY_LOW`): `1` = Pin nuôi RTC bị yếu/hết pin (< 2.0V) hoặc mất pin; `0` = Pin tốt (> 2.5V). *(Do MCU đo đạc / đọc từ thanh ghi cảnh báo của IC RTC để báo cáo cho Host PC).*
* **Quy tắc Read-Before-Write**: Host PC **phải đọc FC03** tại `0x0810` trước khi kết nối để lấy thông tin phần cứng và cờ pin. Khi gửi lệnh FC16 đồng bộ, Host PC **bắt buộc phải bảo toàn** cờ `HW_PRESENT` và `BATTERY_LOW` từ MCU, chỉ cập nhật `SYNCED = 1`.

### 3.5. `SPLC_DiagBlock_t` (0x0A20, 5 thanh ghi = 10 Bytes)
Khối chẩn đoán, cưỡng bức ngõ ra và kiểm soát Watchdog Lease.

```c
typedef struct SPLC_PACKED {
    uint16_t command;             /* 0x00: SPLC_DiagCommand_t (WO) */
    uint16_t state;               /* 0x02: SPLC_DiagState_t (RO) */
    uint16_t flags;               /* 0x04: SPLC_DiagFlags_t (RO) */
    uint16_t lease_remaining_ms;  /* 0x06: Bộ đếm lùi thời gian thuê (ms, đếm từ 3000 về 0) */
    uint16_t error_code;          /* 0x08: SPLC_DiagErrorCode_t (RO) */
} SPLC_DiagBlock_t;
```
* **`SPLC_DiagCommand_t` (Ghi vào 0x0A20)**:
  * `1 = ENTER_DIAG`: Xin quyền điều khiển chẩn đoán thủ công.
  * `2 = HEARTBEAT`: Gia hạn thời gian thuê (reset Lease về 3000 ms).
  * `3 = EXIT_DIAG`: Nhả quyền chẩn đoán, trả quyền tự động cho Rule Engine.
  * `4 = COMMIT_RETAIN`: Ghi toàn bộ dữ liệu Retain từ RAM vào Flash.
  * `5 = DISCARD_RETAIN`: Hủy bỏ thay đổi Retain trong RAM, nạp lại dữ liệu cũ từ Flash.
* **`SPLC_DiagState_t` (Đọc từ 0x0A21)**:
  * `1 = ENGINE_RUNNING`: Chế độ tự động bình thường.
  * `2 = DIAG_CONTROL`: Chế độ chẩn đoán thủ công (Host có quyền ghi cưỡng bức).
  * `3 = TRANSITIONING`: Đang chuyển tiếp tại scan boundary.
  * `4 = FAULT`: Lỗi phần cứng nghiêm trọng.
* **Bitmask `flags` (Đọc từ 0x0A22)**:
  * `0x0001` (`RETAIN_DIRTY`): `1` = Dữ liệu Retain trong RAM bị sửa đổi nhưng chưa lưu Flash.
  * `0x0002` (`LEASE_ACTIVE`): `1` = Bộ đếm thời gian thuê Lease đang hoạt động.
* **`SPLC_DiagErrorCode_t` (Đọc từ 0x0A24)**:
  * `0 = NONE`, `1 = DENIED_FAULT`, `2 = LEASE_EXPIRED`, `3 = FLASH_CRC_MISMATCH`, `4 = INVALID_COMMAND`, `5 = RETAIN_DIRTY`.

### 3.6. `SPLC_FbTimerRecord_t` (0x0B00..0x0B3F, 8 Timers × 8 regs = 16 Bytes)
Bảng viễn trắc 8 khối Timer chuẩn IEC 61131-3 (TON, TOF, TP). Địa chỉ Timer `i`: `0x0B00 + (i * 8)`.

```c
typedef struct SPLC_PACKED {
    uint16_t status_bits;         /* 0x00: Bit 0: IN, Bit 1: Q, Bit 2: RESET, Bit 3: RUNNING (Read-Only) */
    uint16_t mode;                /* 0x02: SPLC_TimerMode_t (1=TON, 2=TOF, 3=TP) */
    uint32_t pt_ms;               /* 0x04: Preset Time tính bằng ms (High Word trước) */
    uint32_t et_ms;               /* 0x08: Elapsed Time tính bằng ms (High Word trước, Read-Only) */
    uint16_t rule_ref;            /* 0x0C: 1-based index của rule đếm giờ (1..100, 0=unlinked/không dùng, R/W) */
    uint16_t reserved;            /* 0x0E: Dự phòng, luôn ghi 0x0000 */
} SPLC_FbTimerRecord_t;
```
* **Ý nghĩa trường `rule_ref` (Offset +6 / 0x0C)**:
  * Được App tính toán sau khi đã chốt thứ tự cuối cùng của bảng Rule: `rule_ref = (vị trí rule đếm giờ trong bảng rule gửi xuống, tính từ 0) + 1` (1-based index: 1..100).
  * Timer không dùng hoặc không có rule đếm giờ: ghi `0`.
  * Dải hợp lệ: `0..100` (ngoài dải firmware trả exception `0x03`).
  * Firmware dùng `rule_ref` để tra cứu bộ đếm thời gian thực của Rule Engine và phản hồi `ET`, `RUNNING`, `Q`, `IN` về thanh ghi `0x0B00 + (8 * i)`.


### 3.7. `SPLC_FbCounterRecord_t` (0x0B40..0x0B7F, 8 Counters × 8 regs = 16 Bytes)
Bảng viễn trắc 8 khối Counter chuẩn IEC 61131-3 (CTU, CTD). Địa chỉ Counter `i`: `0x0B40 + (i * 8)`.

```c
typedef struct SPLC_PACKED {
    uint16_t status_bits;         /* 0x00: Bit 0: CU, Bit 1: CD, Bit 2: RESET, Bit 3: Q */
    uint16_t mode;                /* 0x02: SPLC_CounterMode_t (1=CTU, 2=CTD) */
    int32_t  preset_value;        /* 0x04: Preset Value PV (High Word trước) */
    int32_t  current_value;       /* 0x08: Current Value CV (High Word trước) */
    uint16_t retain_tag_index;    /* 0x0C: TagIndex của thanh ghi lưu CV (VREG, VREG_RETAIN, VFLAG, COUNTER) hoặc 0xFFFF nếu Unbound */
    uint16_t reserved;            /* 0x0E: Dự phòng, luôn ghi 0x0000 */
} SPLC_FbCounterRecord_t;
```

### 3.8. `SPLC_RuleRecord_t` (0x0100 & 0x9010, 16 thanh ghi = 32 Bytes)
Bản ghi logic nhị phân chuẩn hóa thực thi trên MCU Rule Engine, hỗ trợ kép: **Chế độ Số nguyên (Legacy Int32)** và **Chế độ Số thực dấu phẩy động (IEEE-754 Float32 / REAL)**:

```c
/* Cờ định dạng dữ liệu (Rule Flags) lưu tại Reg 13 High */
typedef enum {
    SPLC_RULE_FLAG_NONE           = 0x00, /* 0x00: Chế độ số nguyên Int32 (Mặc định) */
    SPLC_RULE_FLAG_TRIGGER_FLOAT  = 0x01, /* bit 0: threshold_lo/hi (và giá trị tag trigger) là Float32 */
    SPLC_RULE_FLAG_ACTION_FLOAT   = 0x02, /* bit 1: action_param là Float32 */
    SPLC_RULE_FLAG_FLOAT_DATA     = 0x03  /* alias cũ = cả hai bit (bắt buộc cho SCALE_TAG float) */
} SPLC_RuleFlags_t;

typedef struct SPLC_PACKED {
    union {
        int32_t  threshold_lo;    /* [0..1]   Ngưỡng dưới Int32 / Giờ bắt đầu HHmm (High Word trước) */
        float    threshold_lo_f;  /* [0..1]   Ngưỡng dưới số thực Float32 IEEE-754 (khi flags & 0x01) */
    };
    union {
        int32_t  threshold_hi;    /* [2..3]   Ngưỡng trên Int32 / Giờ kết thúc HHmm (High Word trước) */
        float    threshold_hi_f;  /* [2..3]   Ngưỡng trên số thực Float32 IEEE-754 (khi flags & 0x01) */
    };
    uint32_t for_ms;              /* [4..5]   Thời gian duy trì / chu kỳ định thời (ms) */
    union {
        int32_t  action_param;    /* [6..7]   Giá trị tham số tác động Int32 */
        float    action_param_f;  /* [6..7]   Giá trị tham số tác động Float32 (Hệ số góc k khi SCALE_TAG) */
    };
    uint16_t trigger_tag;         /* [8]      TagIndex kích hoạt (0..127) */
    uint16_t action_tag;          /* [9]      TagIndex tác động (0..127) */
    uint16_t guard_tag;           /* [10]     Bit 15: NEGATE, Bit 0..14: TagIndex (0x7FFF=None) */
    uint8_t  enabled;             /* [11H]    1 = Kích hoạt, 0 = Vô hiệu */
    uint8_t  trigger_type;        /* [11L]    SPLC_TriggerType_t */
    uint8_t  compare_op;          /* [12H]    SPLC_CompareOp_t */
    uint8_t  action_type;         /* [12L]    SPLC_ActionType_t */
    uint8_t  flags;               /* [13H]    SPLC_RuleFlags_t (bit0 TRIGGER_FLOAT, bit1 ACTION_FLOAT; 0x03 = FLOAT_DATA) */
    uint8_t  reserved[5];         /* [13L..15] Luôn ghi 0x00; receiver bỏ qua */
} SPLC_RuleRecord_t;
```
* **Chế độ Số thực Float32**: Khi bit 0 của trường `flags` (`0x01`) được bật, bộ nạp và MCU Rule Engine đối sánh các giá trị ngưỡng dưới dạng chuẩn số thực IEEE-754 với dung sai $\epsilon = 1 \times 10^{-5}$. Hành động `SCALE_TAG` thực thi công thức tuyến tính chính xác cao: $y_{\text{float}} = x_{\text{float}} \times \text{action\_param\_f} + \text{threshold\_hi\_f}$.
* **Tương thích ngược**: Nếu `flags == 0x00`, cấu trúc vận hành hoàn toàn tương thích với mã nguồn và dữ liệu số nguyên cũ.

### 3.9. `SPLC_MqttTelemetryBlock_t` (0x0C00..0x0D7F, 4 blocks × 96 regs = 192 Bytes/block)
Cấu trúc khối xuất bản dữ liệu viễn trắc MQTT IIoT qua mạng công nghiệp (SPLC-SPEC-MQTT-001). Địa chỉ Block $i$: `0x0C00 + (i * 96)`.

```c
typedef struct SPLC_PACKED {
    /* --- HEADER (32 thanh ghi = 64 Bytes) --- */
    uint16_t control_flags;        /* 0x00: Bit 0: Enable, Bit 1..2: Mode (0=Event, 1=COV, 2=Periodic), Bit 3..4: QoS, Bit 5: Retain, Bit 6..7: Payload */
    uint16_t en_tag_index;         /* 0x02: Tag cho phép kích hoạt (0xFFFF nếu luôn cho phép) */
    uint32_t interval_ms;          /* 0x04: Chu kỳ xuất bản định kỳ (ms, High Word trước) */
    float    deadband;             /* 0x08: Ngưỡng biến thiên COV (Float32 IEEE 754, High Word trước) */
    uint16_t tag_count;            /* 0x0C: Số lượng Tag liên kết (1..8) */
    uint16_t bound_tags[8];        /* 0x0E: Mảng 8 TagIndex liên kết (0xFFFF nếu slot trống) */
    uint16_t reserved_header;      /* 0x1E: Dự phòng (0x0000) */
    char     topic[32];            /* 0x20: Topic MQTT (32 ký tự ASCII, 2 bytes/reg Big-Endian null-padded) */

    /* --- PAYLOAD TEMPLATE (64 thanh ghi = 128 Bytes) --- */
    char     payload_template[128];/* 0x40: Bản mẫu JSON tùy chỉnh (128 ký tự ASCII null-padded) */
} SPLC_MqttTelemetryBlock_t;
```
*Chi tiết cơ chế COV Deadband, Token Replacement (`{val}`, `{tag}`, `{ts}`, `{device}`) xem tại [`docs/platform/SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/platform/SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md).*

### 3.10. `SPLC_MqttBrokerConfig_t` (0x0E00..0x0E5F, 96 thanh ghi = 192 Bytes)
Cấu trúc cấu hình máy chủ MQTT Broker cấp hệ thống và phản hồi trạng thái viễn trắc trực tiếp từ MCU:

```c
typedef struct SPLC_PACKED {
    uint16_t broker_control;       /* 0x00: Bit 0: Enable, Bit 1: TLS (8883), Bit 2: CleanSession, Bit 3..4: Protocol (0=v3.1.1, 1=v5.0) */
    uint16_t broker_port;          /* 0x02: Port TCP (1883 hoặc 8883) */
    uint16_t keepalive_sec;        /* 0x04: Chu kỳ Ping PINGREQ (mặc định 60s) */
    uint16_t reserved_0;           /* 0x06: Dự phòng (0x0000) */
    char     broker_host[64];      /* 0x08: Host IP/Domain (64 ký tự ASCII null-padded) */
    char     client_id[32];        /* 0x48: Client ID gửi trong CONNECT (32 ký tự ASCII) */
    char     username[32];         /* 0x68: Tên đăng nhập xác thực (32 ký tự ASCII) */
    char     password[32];         /* 0x88: Mật khẩu xác thực (32 ký tự ASCII) */

    /* --- LIVE FEEDBACK (MCU cập nhật trực tiếp tại 0x0E54..0x0E5F, Read-Only) --- */
    uint16_t status_flags;         /* 0xA8 (0x0E54): Bit 0: Connected, Bit 1: TLS Active, Bit 2: Auth OK */
    uint16_t connection_latency;   /* 0xAA (0x0E55): Round-trip Ping latency (ms) */
    uint32_t tx_message_count;     /* 0xAC (0x0E56): Tổng số gói tin đã xuất bản (High Word trước) */
    uint32_t tx_byte_count;        /* 0xB0 (0x0E58): Tổng dung lượng byte đã truyền (High Word trước) */
    uint16_t last_error_code;      /* 0xB4 (0x0E5A): 0=OK, 1=DNS, 2=TCP Reset, 3=Auth Fail, 4=Timeout */
    uint16_t reserved_status[5];   /* 0xB6 (0x0E5B): Dự phòng */
} SPLC_MqttBrokerConfig_t;
```

### 3.11. `SPLC_CertChunk_t` (0x0F00..0x0F43, 68 thanh ghi, SPLC-SPEC-MQTT-003)
Cấu trúc khối dữ liệu nạp phân đoạn chứng chỉ bảo mật TLS/SSL (Root CA, Client Cert, Client Key) qua Modbus RTU:

```c
#pragma pack(push, 1)

typedef struct SPLC_PACKED {
    /* --- HEADER (4 thanh ghi = 8 Bytes tại 0x0F00..0x0F03) --- */
    uint8_t  cert_type;        /* 0x00: Loại chứng chỉ (0x01: Root CA, 0x02: Client Cert, 0x03: Client Key) */
    uint8_t  chunk_index;      /* 0x01: Chỉ số khối (0, 1, ..., total_chunks - 1) */
    uint8_t  total_chunks;     /* 0x02: Tổng số khối cấu thành toàn bộ chứng chỉ (1..32) */
    uint8_t  chunk_len;        /* 0x03: Số byte dữ liệu thực tế trong payload của khối này (1..120) */
    uint16_t total_cert_size;  /* 0x04: Tổng dung lượng (bytes) của toàn bộ chứng chỉ PEM/DER */
    uint16_t chunk_crc16;      /* 0x06: Checksum CRC-16 (Modbus polynomial 0xA001) tính trên payload */

    /* --- PAYLOAD (60 thanh ghi = 120 Bytes tại 0x0F04..0x0F3F) --- */
    uint8_t  payload[120];     /* 0x08: Dữ liệu phân mảnh ASCII PEM hoặc nhị phân DER (0x00-padded) */
} SPLC_CertChunk_t;

#pragma pack(pop)
```

#### Thanh ghi điều khiển & bắt tay nạp chứng chỉ:
* `0x0F40` (`CERT_ACK_CHUNK_INDEX`): MCU phản hồi chỉ số `chunk_index` vừa nạp và lưu thành công vào RAM buffer tạm.
* `0x0F41` (`CERT_STATUS_ERROR`): Trạng thái lỗi (0=Ready, 1=Lỗi CRC khối, 2=Tràn bộ nhớ, 3=Lỗi ghi Flash).
* `0x0F42` (`CERT_COMMIT_COMMAND`): Ghi `0xA5A5` để MCU đối soát toàn vẹn, ghi vĩnh viễn vào Flash và nạp vào mbedTLS/WolfSSL.
* `0x0F43` (`CERT_ACTIVE_TYPE_MASK`): Cờ phản ánh chứng chỉ đang hiệu lực (Bit 0: Root CA, Bit 1: Client Cert, Bit 2: Client Key).

#### Quy trình Bắt tay Nạp Chứng chỉ (Handshake Sequence):
```text
Host (Studio)                                                      MCU Gateway
     │                                                                  │
     │ 1. Đọc HardwareCapabilities (0x0A08)                             │
     ├─────────────────────────────────────────────────────────────────>│
     │<─────────────────────────────────────────────────────────────────┤ (Có cờ 0x0080: MQTT_TLS_CERT)
     │                                                                  │
     │ 2. Gửi Chunk 0: FC16 Write 64 regs vào 0x0F00                   │
     ├─────────────────────────────────────────────────────────────────>│
     │                                                                  │ Kiểm tra chunk_crc16
     │                                                                  │ Ghi nối tiếp vào Flash/RAM buffer
     │                                                                  │ Set CERT_ACK = 0 (0x0F40)
     │ 3. Đọc CERT_ACK (0x0F40)                                         │
     ├─────────────────────────────────────────────────────────────────>│
     │<─────────────────────────────────────────────────────────────────┤ ACK = 0, Status = 0
     │                                                                  │
     │ ... Lặp lại cho đến khi gửi xong Chunk (N - 1) ...               │
     │                                                                  │
     │ 4. Ghi COMMIT_COMMAND: FC16 Write 0xA5A5 vào 0x0F42              │
     ├─────────────────────────────────────────────────────────────────>│
     │                                                                  │ Chốt file /spiffs/ca.crt
     │                                                                  │ Nạp vào mbedTLS context
     │                                                                  │ Set CERT_ACTIVE_MASK (0x0F43)
     │ 5. Đọc xác nhận hoàn tất                                         │
     │<─────────────────────────────────────────────────────────────────┤ Status = OK, ActiveMask = 0x01
```

#### Cơ chế Xử lý Ngoại lệ & Fail-Safe:
1. **Lỗi CRC Khối (`CERT_STATUS_ERROR = 1`):** Nếu MCU tính lại CRC-16 của 120 bytes payload không khớp với trường `chunk_crc16`, MCU giữ nguyên `CERT_ACK` cũ và đặt `CERT_STATUS_ERROR = 1`. Host sẽ tự động thử lại (Retry) tối đa 3 lần.
2. **Hết thời gian chờ (Timeout):** Mỗi lệnh ghi khối có timeout 1.500 ms. Nếu sau 3 lần retry không nhận được ACK, Host hủy bỏ quá trình nạp và giữ nguyên chứng chỉ cũ.
3. **Rollback an toàn:** Chứng chỉ mới chỉ được kích hoạt thay thế chứng chỉ cũ sau khi lệnh COMMIT `0xA5A5` được thực thi thành công. Nếu quá trình truyền bị ngắt quãng giữa chừng, MCU tự động hủy bỏ vùng đệm tạm thời sau 30 giây.

---

### 3.12. `SPLC_ModbusMasterBlock_t` (0x1000..0x10FF, SPLC-SPEC-MBM-001)
Cấu trúc 4 kênh Modbus RTU Master Polling thu thập dữ liệu cảm biến ngoại vi qua cổng RS-485. Mỗi kênh chiếm đúng 64 thanh ghi (128 bytes):

```c
#pragma pack(push, 1)

typedef struct SPLC_PACKED {
    uint16_t target_tag_index;  /* +0: Chỉ số Tag nhận dữ liệu (0xFFFF nếu chưa gán) */
    uint16_t reg_offset;        /* +1: Độ lệch thanh ghi tính từ StartAddress */
    uint16_t data_type;         /* +2: 0=Bool, 1=Int16, 2=UInt16, 3=Real, 4=Int32, 5=UInt32 */
    uint16_t endianness;        /* +3: 0=ABCD, 1=CDAB (Word-Swap), 2=BADC, 3=DCBA */
    float    multiplier;        /* +4..+5: Hệ số nhân tỷ lệ (Float32 IEEE 754 Big-Endian) */
    char     pin_label[12];     /* +6..+11: Nhãn kỹ thuật của chân (12 bytes Packed ASCII) */
} SPLC_ModbusMasterPinBinding_t;

typedef struct SPLC_PACKED {
    /* --- HEADER (16 thanh ghi = 32 Bytes tại Offset +0..+15) --- */
    uint16_t control_flags;     /* +0: Bit 0 = Enabled (1=Bật, 0=Tắt) */
    uint16_t fc_and_slave_id;   /* +1: High Byte = FunctionCode, Low Byte = SlaveID (1..247) */
    uint16_t start_address;     /* +2: Starting Address trên Slave */
    uint16_t reg_count;         /* +3: Quantity of Registers (1..125) */
    uint32_t poll_interval_ms;  /* +4..+5: Chu kỳ quét (ms), uint32 Big-Endian */
    uint32_t timeout_ms;        /* +6..+7: Thời gian chờ phản hồi tối đa (ms) */
    uint16_t valid_tag_index;   /* +8: Tag nhận xung Valid (0xFFFF = Unmapped) */
    uint16_t error_tag_index;   /* +9: Tag nhận cờ Err (0xFFFF = Unmapped) */
    uint16_t err_code_tag_index;/* +10: Tag nhận mã lỗi ErrCode (1=Timeout, 2=CRC, 3=Exc, 4=Malformed) */
    uint16_t pin_count;         /* +11: Số lượng chân cấu hình (0..4) */
    uint16_t reserved[4];       /* +12..+15: Dự phòng (= 0x0000) */

    /* --- PINS (48 thanh ghi = 96 Bytes tại Offset +16..+63) --- */
    SPLC_ModbusMasterPinBinding_t pins[4];
} SPLC_ModbusMasterBlock_t;

#pragma pack(pop)
```

*Chi tiết quy trình quét, xử lý ngoại lệ và chuyển đổi Endianness xem tại [`docs/firmware/SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MODBUS_MASTER_001_READ_ICD.md).*

---

### 3.13. `SPLC_ModbusMasterWriteBlock_t` (0x1100..0x11FF, SPLC-SPEC-MBM-002)
Cấu trúc 4 kênh Modbus RTU Master Writer xuất tín hiệu điều khiển xuống thiết bị chấp hành qua cổng RS-485. Mỗi kênh chiếm đúng 64 thanh ghi (128 bytes):

```c
#pragma pack(push, 1)

typedef struct SPLC_PACKED {
    /* --- HEADER (16 thanh ghi = 32 Bytes tại Offset +0..+15) --- */
    uint16_t control_flags;      /* +0: Bit 0=Enabled, Bit 1=Continuous, Bit 2..15=Reserved */
    uint16_t fc_and_slave_id;    /* +1: High Byte = FunctionCode (FC05/FC06/FC15/FC16), Low Byte = SlaveID (1..247) */
    uint16_t target_address;     /* +2: Starting Address trên Slave đích */
    uint16_t quantity;           /* +3: Số lượng Coils/Registers ghi xuống */
    uint32_t trigger_timeout_ms; /* +4..+5: Thời gian chờ phản hồi tối đa (ms), uint32 Big-Endian */
    uint16_t trig_tag_index;     /* +6: Tag kích hoạt xung ghi (0xFFFF = Unmapped) */
    uint16_t done_tag_index;     /* +7: Tag nhận xung Done hoàn thành (0xFFFF = Unmapped) */
    uint16_t err_tag_index;      /* +8: Tag nhận cờ báo lỗi Err (0xFFFF = Unmapped) */
    uint16_t err_code_tag_index; /* +9: Tag nhận mã lỗi (1=Timeout, 2=CRC, 3=Exc, 4=Malformed) */
    uint16_t pin_count;          /* +10: Số lượng chân nguồn dữ liệu (1..4) */
    uint16_t reserved[5];        /* +11..+15: Dự phòng (= 0x0000) */

    /* --- PINS (48 thanh ghi = 96 Bytes tại Offset +16..+63) --- */
    SPLC_ModbusMasterPinBinding_t pins[4];
} SPLC_ModbusMasterWriteBlock_t;

#pragma pack(pop)
```

---

### 3.14. `SPLC_DirectWriteBlock_t` (0x1200..0x12FF, SPLC-SPEC-DIRECT-WRITE-003)
Cấu trúc tối đa 8 khối điều khiển cọc ngõ ra số vật lý DO0..DO7 (Addressable Bus UCS16703/WS2812B hoặc Direct Terminal). Mỗi khối chiếm 16 thanh ghi (32 bytes):

```c
#pragma pack(push, 1)

typedef struct SPLC_PACKED {
    uint16_t control_flags;       /* +0: Bit 0=Enabled, Bit 1=PulseMode, Bit 2=InvertPolarity, Bit 3=AddressableBus */
    uint16_t target_terminal_id;  /* +1: Cọc ngõ ra vật lý (0=DO0 .. 7=DO7) */
    uint16_t trig_tag_index;      /* +2: Tag kích hoạt (xung cạnh lên hoặc liên tục) */
    uint16_t duration_ms;         /* +3: Độ rộng xung phát ra (ms, dùng khi PulseMode=1) */
    uint16_t data_source_tag;     /* +4: Tag nguồn dữ liệu (ví dụ: RGB color hoặc Index) */
    uint16_t dynamic_channel_tag; /* +5: Tag chọn kênh động (Addressable LED index 1..N) */
    uint16_t max_channel_count;   /* +6: Số lượng kênh LED tối đa (ví dụ: 18 ô Sort-to-Light) */
    uint16_t reserved[9];         /* +7..+15: Dự phòng (= 0x0000) */
} SPLC_DirectWriteBlock_t;

#pragma pack(pop)
```

---

### 3.15. Khai Báo Năng Lực Phần Cứng `HardwareCapabilities` (`0x0A08`) & Quy Chuẩn Fail-Safe
Địa chỉ: `0x0A08` (32-bit uint32, 2 holding registers Big-Endian: `0x0A08` High Word, `0x0A09` Low Word, FC03 Read-Only).

Bitmask định nghĩa (`SPLC_HardwareCapabilities_t` trong `simpleplc_protocol_v2_0.h`):
* `0x0001` (`SPLC_CAP_BASE_LOGIC`): Hỗ trợ Rule Engine FBD.
* `0x0002` (`SPLC_CAP_RETENTIVE`): Hỗ trợ lưu trữ Flash/NVRAM/FRAM.
* `0x0004` (`SPLC_CAP_RTC`): Có IC RTC DS3231 hoặc LSE crystal.
* `0x0008` (`SPLC_CAP_ETHERNET`): Có cổng mạng vật lý (LAN/WiFi).
* `0x0010` (`SPLC_CAP_MQTT_PUB`): Hỗ trợ xuất bản dữ liệu viễn trắc MQTT PUB (`0x0C00`).
* `0x0020` (`SPLC_CAP_MQTT_SUB`): Hỗ trợ nhận lệnh điều khiển Inbound MQTT SUB (`0x0D80`).
* `0x0040` (`SPLC_CAP_SD_DATALOG`): Có khe cắm thẻ nhớ MicroSD.
* `0x0080` (`SPLC_CAP_MQTT_TLS_CERT`): Hỗ trợ nạp chứng chỉ TLS/SSL qua struct chunk (`0x0F00`).
* `0x0100` (`SPLC_CAP_MODBUS_MASTER`): Hỗ trợ phân hệ RS-485 Modbus Master (`0x1000..0x11FF`).

**Quy chuẩn Fail-Safe khi Deploy từ App (SimplePLC Studio):**
1. **Bắt buộc đọc 0x0A08 trước khi Deploy:** App luôn gửi lệnh FC03 đọc `0x0A08..0x0A09` để xác minh cờ hỗ trợ trên vi điều khiển.
2. **Fail-Fast (Chống nạp ảo):** Nếu sơ đồ có dùng khối MQTT (hoặc Broker Enabled) mà đọc `0x0A08` lỗi (Timeout, CRC) hoặc firmware thiếu cờ `0x0010 / 0x0020`, App lập tức **DỪNG NẠP**, trả về mã lỗi `UNSUPPORTED (0x0002)` và báo Deploy THẤT BẠI. App không nuốt lỗi, không bỏ rơi cấu hình MQTT và không commit `0xA000`.
3. **Tương thích ngược bo mạch cũ:** Nếu sơ đồ không dùng MQTT và thiết bị cũ không có thanh ghi `0x0A08` (phản hồi Modbus Exception `0x02`), App cảnh báo và vẫn nạp logic FBD bình thường.

---

## 4. Phân bổ `TagIndex` Remote I/O V1 (124 Active Tags)

Địa chỉ thanh ghi Modbus của mỗi Tag được ánh xạ cố định theo công thức bất biến:
`Modbus Address = 0x0900 + (TagIndex * 2)`

```text
TagIndex    Ký hiệu Tag     Số lượng   Loại Tag                  Địa chỉ Modbus     Quyền truy cập
--------------------------------------------------------------------------------------------------
0   .. 7    DI0 .. DI7      8          DiscreteInput (DI)        0x0900 .. 0x090F   Read-Only vật lý
8   .. 15   DO0 .. DO7      8          DiscreteOutput (DO)       0x0910 .. 0x091F   Chỉ ghi trong DIAG
16  .. 19   AI0 .. AI3      4          AnalogInput (AI)          0x0920 .. 0x0927   Read-Only vật lý
20  .. 51   VFLAG0 .. 31    32         VirtualFlag (VFLAG)       0x0928 .. 0x0967   Chỉ ghi trong DIAG
52  .. 83   VREG0 .. 31     32         VirtualRegister (VREG)    0x0968 .. 0x09A7   Chỉ ghi trong DIAG
84  .. 115  VREG_R0 .. 31   32         VREG_RETAIN (Flash)       0x09A8 .. 0x09E7   Chỉ ghi trong DIAG
116 .. 123  COUNTER0 .. 7   8          Counter (Bộ đếm)          0x09E8 .. 0x09F7   Chỉ ghi trong DIAG
124 .. 127  RESERVED        4          Dự phòng                  0x09F8 .. 0x09FF   Khóa truy cập
--------------------------------------------------------------------------------------------------
TỔNG HOẠT ĐỘNG: 124 Tags (248 thanh ghi) | DUNG LƯỢNG KHUNG WIRE: 128 Tags (256 thanh ghi)
```

### 4.2. Chuẩn Hóa Kiểu Dữ Liệu IEC 61131-3 & Cấu Trúc Tag Store 32-bit (`SPLC_TagValue_t`)

Kể từ Platform Release V2.0 (v2.6.0), toàn bộ các biến trong Tag Store (`0x0900..0x09FF`) được chuẩn hóa theo chuẩn quốc tế IEC 61131-3:

```c
/* Định nghĩa kiểu dữ liệu theo chuẩn IEC 61131-3 */
typedef enum {
    SPLC_DATA_TYPE_BOOL   = 1, /* IEC 61131-3 BOOL: 1-bit logic (0 hoặc 1) */
    SPLC_DATA_TYPE_INT32  = 2, /* IEC 61131-3 DINT: Số nguyên có dấu bù hai 32-bit */
    SPLC_DATA_TYPE_REAL   = 3  /* IEC 61131-3 REAL: Số thực dấu phẩy động 32-bit IEEE-754 */
} SPLC_TagDataType_t;

/* Cấu trúc hợp nhất 32-bit đại diện cho một Tag trong bộ nhớ MCU */
typedef union SPLC_PACKED {
    int32_t  as_int32;         /* Truy cập dạng số nguyên có dấu 32-bit (DINT) */
    int32_t  as_dint;          /* Tên chuẩn IEC 61131-3 */
    float    as_real;          /* Truy cập dạng số thực dấu phẩy động IEEE-754 32-bit (REAL) */
    uint32_t as_raw32;         /* Truy cập dạng raw 32-bit không dấu */
    struct {
        uint16_t high_word;    /* Thanh ghi Modbus địa chỉ thấp (Offset 0) - Big-Endian */
        uint16_t low_word;     /* Thanh ghi Modbus địa chỉ cao (Offset 1) */
    } words;
} SPLC_TagValue_t;
```

#### Quy Tắc Phân Bổ Kiểu Dữ Liệu:
1. **`BOOL` (1-bit logic)**:
   - Áp dụng bất biến cho: `DI0..DI7`, `DO0..DO7`, `VFLAG0..VFLAG31`.
   - Lưu trữ trong Tag Store dưới dạng `0` hoặc `1`. Cố định, người dùng không thể đổi kiểu.
2. **`DINT` / `INT32` (32-bit Signed Integer)**:
   - Áp dụng cho: `COUNTER0..COUNTER7` (bộ đếm), `AI` (chế độ ADC raw 0..4095), `VREG` (số nguyên), `VREG_RETAIN` (số nguyên), `ModbusHolding`.
   - Trực tiếp chuyển thành số nguyên hai bù mà không cần qua bất kỳ hệ số nhân ảo nào.
3. **`REAL` / `Float32` (32-bit Floating Point IEEE-754)**:
   - Áp dụng cho: `AI` (khi gán đo lường nhiệt độ, áp suất, lưu lượng...), `VREG` (biến tính toán dấu phẩy động), `VREG_RETAIN` (ngưỡng setpoint thực), `ModbusHolding`.
   - **Giao thức đường truyền**: Giá trị được truyền trực tiếp dạng nhị phân 32-bit IEEE-754 qua 2 thanh ghi Modbus liên tiếp (High Word trước, Low Word sau). **Tuyệt đối không nhân chia $10^{dec}$ tạm bợ**.

---

## 5. Quy trình Bắt tay Khám phá Tự mô tả (Self-Describing Handshake)

Khi SimplePLC Studio kết nối vào cổng COM của MCU:

```text
Host (Studio)                                    MCU Firmware
     │                                                │
     ├─ 1. FC03 Đọc 0x0000 (10 regs) ────────────────>│ Trả về DeviceDescriptor:
     │                                                │ ProtocolVersion = 2, RuleFormatVersion = 7
     │                                                │
     ├─ 2. FC03 Đọc 0x0020 (10 regs) ────────────────>│ Trả về DeviceResourceInfo:
     │                                                │ WireProfile = 2, ActiveTags = 124, 8DI/8DO...
     │                                                │
     ├─ 3. FC03 Đọc 0x0A08 (2 regs) ─────────────────>│ Trả về HardwareCapabilities (32-bit):
     │                                                │ Bitmask: BASE_LOGIC | RETENTIVE | RTC | ETHERNET | MQTT_PUB
     │                                                │
     ├─ [Studio kiểm tra WireProfile == 2 & Caps]:    │
     │  - Sinh động TagCatalog (124 tags)             │
     │  - Nhận diện năng lực MQTT PUB/SUB, RTC, NVRAM │
     │  - Mở khóa phân hệ Chẩn đoán (0x0A20)          │
     │  - Mở khóa phân hệ Function Block (0x0B00)     │
     │                                                │
     ├─ 4a. FC03 Đọc 0x0810..0x0813 (4 regs) ────────>│ Đọc trạng thái RTC (Giờ hiện tại, HW_PRESENT, BATTERY_LOW, SYNCED)
     │                                                │ Studio chỉ Read-Only để hiển thị giờ thực tế lên thanh trạng thái.
     │                                                │ (Tuyệt đối KHÔNG tự ý ghi đè giờ PC xuống MCU khi vừa kết nối).
     │                                                │
     ├─ 4b. [Kỹ sư kích hoạt] FC16 Ghi 0x0810..0x0813>│ Chỉ ghi khi Kỹ sư mở hộp thoại Sync RTC & chọn Múi giờ:
     │     [EpochUtcSeconds, TzOffset, StatusFlags]   │ Studio BẢO TOÀN cờ HW_PRESENT & BATTERY_LOW, bật SYNCED=1
     │                                                │
     └─ 5. Bắt đầu chu kỳ quét viễn trắc (100ms) ────>│ MCU phản hồi Health (0x0800) & Tags (0x0900)
```

---

## 6. Cơ chế An toàn Chẩn đoán & Watchdog Lease Failsafe

### 6.1. Phân quyền Loại trừ Lẫn nhau (Mutual Exclusion)
* Khi `DIAG_STATE == ENGINE_RUNNING (1)`: Rule Engine có toàn quyền trên vùng nhớ Tag Store `0x0900..0x09FF`. Mọi frame FC16 cố tình ghi vào vùng này bị MCU **từ chối ngay lập tức với Modbus Exception `0x02 (Illegal Data Address)`**.
* Khi Host gửi `CMD_ENTER_DIAG (1)`: MCU chuyển sang `DIAG_CONTROL (2)`, Rule Engine tạm dừng logic tại scan boundary kế tiếp, nhường quyền ghi cho Host.

### 6.2. Vòng đời Watchdog Lease & Failsafe DO
* Mỗi lệnh `HEARTBEAT (2)` từ Host gia hạn bộ đếm `lease_remaining_ms = 3000`.
* Định kỳ 10ms, MCU giảm `lease_remaining_ms -= 10`.
* **Sự cố mất kết nối**: Nếu sau 3000ms Host không gửi nhịp tim (do rút cáp, đơ máy):
  1. MCU lập tức chuyển `DIAG_STATE = ENGINE_RUNNING (1)`.
  2. Bật mã lỗi `DIAG_ERROR_CODE = LEASE_EXPIRED (2)`.
  3. **Tự động đưa toàn bộ các ngõ ra DO vật lý (`DO0..DO(do_count-1)` bắt đầu từ chỉ số `DoBase = di_count`) về 0 an toàn** nhằm ngăn chặn tai nạn kẹt cơ cấu chấp hành.

---

## 7. Đánh giá Thuật toán Time Window RTC

MCU tính toán giờ địa phương danh định `HHmm` trong chu kỳ quét 10ms:

```text
local_epoch = epoch_utc_s + (tz_offset_min * 60)
seconds_of_day = local_epoch % 86400
current_hhmm = (floor(seconds_of_day / 3600) * 100) + floor((seconds_of_day % 3600) / 60)
```

* **Khung giờ trong ngày (`Lo <= Hi`, ví dụ 07:00..17:00 → `700..1700`):**
  - Điều kiện đúng: `700 <= current_hhmm <= 1700`.
* **Khung giờ qua nửa đêm (`Lo > Hi`, ví dụ 18:00..06:00 sáng hôm sau → `1800..600`):**
  - Điều kiện đúng: `current_hhmm >= 1800 || current_hhmm <= 600`.
* **Điểm thời gian chính xác (`Op == EQ`, `Lo == Hi`, ví dụ đúng 08:30 → `830`):**
  Chỉ kích hoạt tại sườn lên chuyển phút (minute edge transition), không lặp lại trong suốt 60 giây của phút đó.

---

## 8. Đối chiếu Golden Vectors V2.0 Chuẩn

Mọi triển khai firmware phải so khớp chính xác từng byte frame Modbus RTU với [`tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json):

1. **`GV-001` (DeviceDescriptor)**: FC03 tại `0x0000` (10 regs) → `ProtocolVersion=2`, `RuleFormatVersion=7`.
2. **`GV-002` (DeviceResourceInfo)**: FC03 tại `0x0020` (10 regs) → `WireProfile=2`, `MaxRules=100`, `ActiveTags=124`.
3. **`GV-003` (DeviceHealth)**: FC03 tại `0x0800` (10 regs) → `scan_time_ms=10ms`.
4. **`GV-004` (RTC Clock)**: FC03 tại `0x0810` (4 regs) → Epoch UTC, Timezone Offset +420, Flags=3.
5. **`GV-005` (Diagnostic Block)**: FC03 tại `0x0A20` (5 regs) → `CMD=2`, `STATE=2`, `FLAGS=2`, `LEASE=3000ms`.
6. **`GV-006` (FB Timer 0)**: FC03 tại `0x0B00` (8 regs) → TON, `PT=5000ms`, `ET=2500ms`, `IN=1`, `RUNNING=1`.
7. **`GV-007` (FB Counter 0)**: FC03 tại `0x0B40` (8 regs) → CTU, `PV=10`, `CV=4`, `RetainTag=84`, `CU=1`.
8. **`GV-008` (Time Window Rule)**: 32-byte binary payload cho khung giờ 07:00..17:00 → CRC-16 payload `0xAB68`.
9. **`GV-009` (Diag Heartbeat Write)**: FC06 tại `0x0A20` ghi giá trị `2` → Frame: `01 06 0A 20 00 02 0A 19`.
10. **`GV-010` (Commit Command)**: FC06 tại `0xA000` ghi giá trị `0xA5A5` → Frame: `01 06 A0 00 A5 A5 10 E1`.

---

## 9. Bảng Kiểm Tra Tuân Thủ Toàn Diện (Conformance Checklist CHKL-01 .. CHKL-20)

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
- [ ] **CHKL-14: Hardware Capabilities (0x0A08)**: MCU phản hồi 32-bit tại `0x0A08..0x0A09` theo bitmask `SPLC_HardwareCapabilities_t`. Khi build có MQTT (`MQTT=1`), BẮT BUỘC bật cờ `SPLC_CAP_MQTT_PUB (0x0010)` và `SPLC_CAP_MQTT_SUB (0x0020)`. Nếu không có, App sẽ từ chối nạp an toàn (Fail-Fast).
- [ ] **CHKL-15: MQTT IIoT Gateway & Telemetry Wire Conformance**: Đọc/ghi cấu hình Broker tại `0x0E00` (96 regs) và Telemetry Block tại `0x0C00` (96 regs) bằng FC03/FC16 đúng byte-exact; thanh ghi `+15` là `reserved_header` (luôn ghi/đọc `0x0000`); phản hồi trạng thái Live Broker tại `0x0E54..0x0E5F`.
- [ ] **CHKL-16: IEC 61131-3 Data Types Conformance**: Toàn bộ giá trị 32-bit tại `0x0900..0x09FF` tuân thủ union `SPLC_TagValue_t`. Các kênh số thực `REAL` (`AI`, `VREG`, `VREG_RETAIN`) lưu nhị phân chuẩn IEEE-754 Single Precision (High Word trước, Low Word sau), tuyệt đối không nhân chia $10^{dec}$; bộ phân giải JSON MQTT tự động format số thực `float` hay số nguyên `int` theo kiểu dữ liệu tự nhiên của Tag.
- [ ] **CHKL-17: MQTT Inbound Subscribe Conformance (0x0D80)**: Lắng nghe bản tin JSON, giải mã key-value vào buffer RTOS Queue và cập nhật vào `g_tag_store` tại đầu chu kỳ quét 10ms. Tự động đưa chân tín hiệu về giá trị mặc định khi quá `failsafe_timeout_ms`.
- [ ] **CHKL-18: MQTT TLS/SSL Certificate Staging Conformance (0x0F00)**: Hỗ trợ nạp phân đoạn chứng chỉ số X.509 Root CA theo khối struct chuẩn 128 bytes (64 thanh ghi Modbus `SPLC_CertChunk_t`) qua Staging Buffer `0x0F00..0x0F3F`. Phản hồi ACK `0x0F40` chỉ số khối nhận thành công; kiểm tra CRC-16 của từng khối; chốt lưu Flash vĩnh viễn và nạp vào TLS stack khi nhận lệnh Atomic Commit `0x0F42` với giá trị `0xA5A5`.
- [ ] **CHKL-19: Modbus Master RTU Sensor Poller Conformance (0x1000..0x10FF)**: Hỗ trợ nạp cấu hình 4 kênh Modbus Master tại `0x1000..0x10FF` (256 regs, SPLC-SPEC-MBM-001). Thực thi polling không nghẽn (Non-blocking RTOS Task), xác thực CRC-16 của frame phản hồi, phân giải 4 chuẩn Endianness (ABCD, CDAB, BADC, DCBA), cập nhật nguyên tử vào `g_tag_store` tại ranh giới chu kỳ quét 10ms.
- [ ] **CHKL-20: Direct Hardware Output Conformance (0x1200..0x12FF)**: Hỗ trợ nạp cấu hình điều khiển 8 cọc ngõ ra DO vật lý và dải đèn LED địa chỉ hóa (Addressable Bus UCS16703/WS2812B) theo đúng ICD SPLC-SPEC-DIRECT-WRITE-003.

---

## 10. Hướng Dẫn Kỹ Thuật Lập Trình Mẫu Cho Kỹ Sư Firmware

Nhằm thống nhất 100% giữa Firmware MCU (C/C++) và SimplePLC Studio (C# .NET), các kỹ sư Firmware cần tuân thủ nghiêm ngặt các quy chuẩn sau:

### 10.1. Quản lý Bộ nhớ Tag Store 32-Bit (`0x0900..0x09FF`)
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

### 10.2. Quy Tắc Xử Lý Kiểu Dữ Liệu (IEC 61131-3 Data Types)
1. **`BOOL` (1-bit):**
   * Áp dụng cho: `DI0..DI7`, `DO0..DO7`, `VFLAG0..VFLAG31`.
   * Giá trị hợp lệ: `0` (FALSE) hoặc `1` (TRUE).
2. **`DINT` / `INT32` (32-bit Signed Integer):**
   * Áp dụng cho: `COUNTER0..COUNTER15`, `VREG` (mặc định), `VREG_RETAIN` (mặc định).
   * Giá trị: Số nguyên có dấu bù 2 từ $-2,147,483,648$ đến $+2,147,483,647$. Truy cập qua `g_tag_store[tag_idx].as_int32`.
3. **`REAL` (32-bit Floating Point IEEE-754):**
   * Áp dụng cho: `AI` (chế độ số thực đã scale °C, bar,...), `VREG` (chế độ số thực), `VREG_RETAIN` (chế độ số thực).
   * Truy cập trực tiếp qua `g_tag_store[tag_idx].as_real`.
   * **Lưu ý cốt lõi:** Tuyệt đối không nhân chia hệ số $10^{dec}$ tạm bợ vào thanh ghi Modbus. Client và Server trao đổi trực tiếp biểu diễn nhị phân 32-bit của số thực.

### 10.3. Quy Chuẩn Xuất Bản MQTT Telemetry (`0x0C00`)
Khi sinh chuỗi JSON từ `payload_template`:
* Khi gặp `{val1}`..`{val8}` hoặc `{val}`:
  * Nếu Tag cấu hình là `REAL`: Sử dụng `snprintf(buf, len, "%.4g", tag.as_real)` (loại bỏ số 0 thừa ở đuôi, ví dụ `24.5` thay vì `24.5000`).
  * Nếu Tag cấu hình là `DINT`/`INT32`: Sử dụng `snprintf(buf, len, "%ld", (long)tag.as_int32)`.
  * Nếu Tag cấu hình là `BOOL`: Sử dụng `snprintf(buf, len, "%d", tag.as_int32 ? 1 : 0)`.
* Giới hạn `topic`: Tối đa 32 bytes (đã bao gồm byte null).
* Giới hạn `payload_template`: Tối đa 128 bytes.
* Giới hạn buffer xuất bản: Khuyến nghị tối thiểu 256 bytes để đủ dung lượng ghép chuỗi JSON hoàn chỉnh.