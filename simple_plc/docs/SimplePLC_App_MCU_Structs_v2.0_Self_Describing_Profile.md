# SimplePLC Platform V2.0 — App-to-MCU Binary Structs & Self-Describing Device Profile Specification

**Document ID:** `SPLC-DOC-SPEC-V20-STRUCTS`  
**Standard Release:** SimplePLC Platform V2.0 (Superset of Wire Profile V1.9)  
**Protocol Version:** `2`  
**Rule Format Version:** `7` (32-byte canonical records)  
**Wire Profile:** `2` (Diagnostic Control & Dedicated IEC 61131-3 FB Subsystem)  
**Target Audience:** Senior Embedded Firmware Engineers (STM32, ESP32, RP2040, GD32) & System Integrators  
**Associated Deliverables:**
* Authoritative C/C++ Header: [`src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h)
* Conformance Specification: [`docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md)
* Golden Test Vectors: [`tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/GoldenVectors/golden_vectors_v2_0.json)

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
|   └───────────────────────────────┘     └─────────────────────────────────────┘   |
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
| `0x0A20..0x0A24` | **Diagnostic Control Block**| R/W | FC03/FC06/FC16| 5 regs (10 B) | Phân hệ chẩn đoán: `Command` (0x0A20), `State` (0x0A21), `Flags` (0x0A22), `LeaseMs` (0x0A23), `ErrorCode` (0x0A24). |
| `0x0B00..0x0B3F` | **FB Timers (TON/TOF/TP)**| R/W | FC03/FC16 | 64 regs (128 B) | 8 Timers IEC 61131-3, mỗi khối 8 thanh ghi. Ghi cấu hình (Mode, PT) qua FC16; đọc viễn trắc (Status, ET) qua FC03. |
| `0x0B40..0x0B7F` | **FB Counters (CTU/CTD)** | R/W | FC03/FC16 | 64 regs (128 B) | 8 Counters IEC 61131-3, mỗi khối 8 thanh ghi. Ghi cấu hình (Mode, PV, RetainTag) qua FC16; đọc viễn trắc (Status, CV) qua FC03. |
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
    uint16_t status_bits;         /* 0x00: Bit 0: IN, Bit 1: Q, Bit 2: RESET, Bit 3: RUNNING */
    uint16_t mode;                /* 0x02: SPLC_TimerMode_t (1=TON, 2=TOF, 3=TP) */
    uint32_t pt_ms;               /* 0x04: Preset Time tính bằng ms (High Word trước) */
    uint32_t et_ms;               /* 0x08: Elapsed Time tính bằng ms (High Word trước) */
    uint16_t reserved[2];         /* 0x0C: Dự phòng, luôn ghi 0x0000 */
} SPLC_FbTimerRecord_t;
```

### 3.7. `SPLC_FbCounterRecord_t` (0x0B40..0x0B7F, 8 Counters × 8 regs = 16 Bytes)
Bảng viễn trắc 8 khối Counter chuẩn IEC 61131-3 (CTU, CTD). Địa chỉ Counter `i`: `0x0B40 + (i * 8)`.

```c
typedef struct SPLC_PACKED {
    uint16_t status_bits;         /* 0x00: Bit 0: CU, Bit 1: CD, Bit 2: RESET, Bit 3: Q */
    uint16_t mode;                /* 0x02: SPLC_CounterMode_t (1=CTU, 2=CTD) */
    int32_t  preset_value;        /* 0x04: Preset Value PV (High Word trước) */
    int32_t  current_value;       /* 0x08: Current Value CV (High Word trước) */
    uint16_t retain_tag_index;    /* 0x0C: TagIndex của VREG_RETAIN (84..115) hoặc 0xFFFF nếu None */
    uint16_t reserved;            /* 0x0E: Dự phòng, luôn ghi 0x0000 */
} SPLC_FbCounterRecord_t;
```

### 3.8. `SPLC_RuleRecord_t` (0x0100 & 0x9010, 16 thanh ghi = 32 Bytes)
Bản ghi logic nhị phân chuẩn hóa thực thi trên MCU Rule Engine.

```c
typedef struct SPLC_PACKED {
    int32_t  threshold_lo;        /* [0..1]   Ngưỡng dưới / Giờ bắt đầu HHmm (High Word trước) */
    int32_t  threshold_hi;        /* [2..3]   Ngưỡng trên / Giờ kết thúc HHmm (High Word trước) */
    uint32_t for_ms;              /* [4..5]   Thời gian duy trì / chu kỳ định thời (ms) */
    int32_t  action_param;        /* [6..7]   Giá trị tham số tác động (High Word trước) */
    uint16_t trigger_tag;         /* [8]      TagIndex kích hoạt (0..127) */
    uint16_t action_tag;          /* [9]      TagIndex tác động (0..127) */
    uint16_t guard_tag;           /* [10]     Bit 15: NEGATE, Bit 0..14: TagIndex (0x7FFF=None) */
    uint8_t  enabled;             /* [11H]    1 = Kích hoạt, 0 = Vô hiệu */
    uint8_t  trigger_type;        /* [11L]    SPLC_TriggerType_t */
    uint8_t  compare_op;          /* [12H]    SPLC_CompareOp_t */
    uint8_t  action_type;         /* [12L]    SPLC_ActionType_t */
    uint8_t  reserved[6];         /* [13..15] Luôn ghi 0x00; receiver bỏ qua */
} SPLC_RuleRecord_t;
```

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
     ├─ [Studio kiểm tra WireProfile == 2]:           │
     │  - Sinh động TagCatalog (124 tags)             │
     │  - Mở khóa phân hệ Chẩn đoán (0x0A20)          │
     │  - Mở khóa phân hệ Function Block (0x0B00)     │
     │                                                │
     ├─ 3a. FC03 Đọc 0x0810..0x0813 (4 regs) ────────>│ Đọc trạng thái RTC (Giờ hiện tại, HW_PRESENT, BATTERY_LOW, SYNCED)
     │                                                │ Studio phân tích độ lệch giờ và cờ pin
     │                                                │
     ├─ 3b. [Có điều kiện] FC16 Ghi 0x0810..0x0813 ──>│ Đồng bộ giờ PC nếu lệch > 2s hoặc !IsSynced:
     │     [EpochUtcSeconds, TzOffset, StatusFlags]   │ Studio BẢO TOÀN cờ HW_PRESENT & BATTERY_LOW, bật SYNCED=1
     │                                                │
     └─ 4. Bắt đầu chu kỳ quét viễn trắc (100ms) ────>│ MCU phản hồi Health (0x0800) & Tags (0x0900)
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
  3. **Tự động đưa toàn bộ 8 ngõ ra DO0..DO7 (0x0910..0x091F) về 0 an toàn** nhằm ngăn chặn tai nạn kẹt cơ cấu chấp hành.

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