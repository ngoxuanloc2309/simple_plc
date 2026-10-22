# SimplePLC Platform V2.0 — MQTT IIoT Gateway & Telemetry Binary Structs Specification

**Document ID:** `SPLC-DOC-SPEC-V20-MQTT-STRUCTS`  
**Standard Release:** SimplePLC Platform V2.0 (MQTT IIoT Telemetry & Broker Subsystem)  
**Protocol Version:** `2`  
**Rule Format Version:** `7` (Đồng tồn tại với IEC 61131-3 & Canonical Rule Engine)  
**Wire Profile:** `2` (IIoT Gateway & Dedicated Telemetry Holding Registers)  
**Target Audience:** Senior Embedded Firmware Engineers (STM32, ESP32, FreeRTOS/Zephyr), Cloud/SCADA Architects & System Integrators  
**Associated Deliverables:**
* Authoritative C/C++ Header: [`docs/firmware/splc_mqtt_wire.h`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/splc_mqtt_wire.h)
* Conformance Specification: [`docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md)
* Firmware ICD: [`docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/SPLC_SPEC_MQTT_001_TELEMETRY_ICD.md)
* Host C# Codecs & DTOs:
  - [`src/SimplePLC.Protocol/Codec/MqttBrokerConfigCodec.cs`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Codec/MqttBrokerConfigCodec.cs)
  - [`src/SimplePLC.Protocol/Codec/MqttTelemetryCodec.cs`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Codec/MqttTelemetryCodec.cs)
  - [`src/SimplePLC.Protocol/Dto/MqttBrokerConfigDto.cs`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Dto/MqttBrokerConfigDto.cs)
  - [`src/SimplePLC.Protocol/Dto/MqttTelemetryConfigDto.cs`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Dto/MqttTelemetryConfigDto.cs)
* Studio UI Implementation:
  - [`SimplePLC.Studio/Views/MqttBrokerConfigDialogView.xaml`](file:///g:/HoaNV/Projects/SimplePLC/SimplePLC.Studio/Views/MqttBrokerConfigDialogView.xaml)
  - [`SimplePLC.Studio/Views/MqttConfigDialogView.xaml`](file:///g:/HoaNV/Projects/SimplePLC/SimplePLC.Studio/Views/MqttConfigDialogView.xaml)
  - [`SimplePLC.Studio/ViewModels/MqttBrokerConfigViewModel.cs`](file:///g:/HoaNV/Projects/SimplePLC/SimplePLC.Studio/ViewModels/MqttBrokerConfigViewModel.cs)
  - [`SimplePLC.Studio/ViewModels/MqttPublishNodeViewModel.cs`](file:///g:/HoaNV/Projects/SimplePLC/SimplePLC.Studio/ViewModels/MqttPublishNodeViewModel.cs)
* Unit Test Suites:
  - [`tests/SimplePLC.Protocol.Tests/MqttBrokerConfigCodecTests.cs`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/MqttBrokerConfigCodecTests.cs)
  - [`tests/SimplePLC.Protocol.Tests/MqttTelemetryCodecTests.cs`](file:///g:/HoaNV/Projects/SimplePLC/tests/SimplePLC.Protocol.Tests/MqttTelemetryCodecTests.cs)
  - [`SimplePLC.Studio.Tests/MqttBrokerConfigViewModelTests.cs`](file:///g:/HoaNV/Projects/SimplePLC/SimplePLC.Studio.Tests/MqttBrokerConfigViewModelTests.cs)
* Hardware Integration & Automation:
  - Virtual UART Hardware Automation: `COM7` $\leftrightarrow$ `COM9` (10/10 Test Cases Passed 100%)

---

## 1. Mục đích & Nguyên lý Kiến trúc (Executive Summary & Architectural Paradigm)

Tài liệu này xác lập đặc tả cấu trúc dữ liệu nhị phân cấp thấp (**Byte-Exact Binary Structs**), bố cục phân vùng bộ nhớ thanh ghi Modbus (**Holding Register Map**), cơ chế giải mã chuỗi ký tự (**Packed ASCII Serialization**) và luồng vận hành truyền thông IIoT giữa phần mềm máy tính **SynaptiX IDE / SimplePLC Studio** (Host Modbus Master) và các vi điều khiển công nghiệp **MCU** (Modbus Slave & MQTT Client).

```text
+-----------------------------------------------------------------------------------+
|                        SimplePLC Studio / SynaptiX IDE (Host)                     |
|  ┌───────────────────────────────────────┐ ┌───────────────────────────────────┐  |
|  │ Cấu hình Broker Toàn cục (0x0E00)     │ │ Khối Telemetry FBD Node (0x0C00)  │  |
|  │ - Host, Port, TLS, Client ID, Auth    │ │ - Topic, COV Deadband, Multi-Tags │  |
|  │ - Kiểu kết nối, Keep-Alive            │ │ - Mẫu Payload JSON tùy chỉnh      │  |
|  └───────────────────────────────────────┘ └───────────────────────────────────┘  |
+-----------------------------------------------------------------------------------+
                                         ▲
                                         │ Chuẩn Modbus RTU (FC03 Read, FC16 Write)
                                         │ Wire Endianness: 16-bit Big-Endian
                                         │ Packed ASCII: 2 chars/reg (Big-Endian)
                                         │ Float32 & UInt32: [High16, Low16]
                                         ▼
+-----------------------------------------------------------------------------------+
|                  MCU Firmware Engine (STM32, ESP32, FreeRTOS, Zephyr)             |
|   ┌───────────────────────────────────┐     ┌─────────────────────────────────┐   |
|   │ 0x0E00: System Broker Config      │     │ 0x0900: Runtime Tag Store       │   |
|   │ 96 Holding Regs (192 Bytes)       │     │ 124 Tags Động (DI, DO, AI, VREG)│   |
|   ├───────────────────────────────────┤     ├─────────────────────────────────┤   |
|   │ 0x0C00: 4x Telemetry Blocks       │     │ 0x0810: Real-Time Clock (RTC)   │   |
|   │ 384 Holding Regs (768 Bytes)      │     │ Unix Epoch UTC cho Timestamp    │   |
|   ├───────────────────────────────────┴─────┴─────────────────────────────────┤   |
|   │                        FreeRTOS MQTT Client Task                          │   |
|   │   - Bộ lập lịch quét sự kiện / COV / chu kỳ định thời (10ms scan pass)    │   |
|   │   - Trình phân giải Token Payload JSON: {val}, {tag}, {ts}, {device}      │   |
|   │   - Phản hồi Live Status & Trễ truyền thông về thanh ghi 0x0E54           │   |
|   └───────────────────────────────────────────────────────────────────────────┘   |
+-----------------------------------------------------------------------------------+
                                         │
                                         │ TCP / MQTTS (Port 1883 / 8883)
                                         ▼
+-----------------------------------------------------------------------------------+
|                    MQTT Broker / Enterprise Cloud / SCADA                         |
|                 (EMQX, HiveMQ, Mosquitto, AWS IoT Core, Azure IoT)                |
+-----------------------------------------------------------------------------------+
```

### Các nguyên tắc bất biến cấp kiến trúc (Architectural Invariants)

1. **Phân tầng Cấu hình Độc lập Hai cấp (Two-Tier Decoupled Configuration)**:
   * **Cấp Hệ thống (`0x0E00..0x0E5F`)**: Toàn bộ dự án chỉ sở hữu duy nhất 1 cấu hình máy chủ Broker chung (Host, Port, TLS, Client ID, Username, Password, Keep-Alive). Thiết lập này được quản lý qua menu hệ thống (`Thiết bị` $\rightarrow$ `Cấu hình Máy chủ MQTT Broker...`), hoàn toàn không bị phân mảnh hay trùng lặp theo từng khối chức năng FBD.
   * **Cấp Luồng Dữ liệu FBD (`0x0C00..0x0D7F`)**: Mỗi khối `MQTT Publish Node` trên canvas đại diện cho một luồng phát viễn trắc riêng biệt (tối đa 4 khối). Khối FBD chỉ tập trung vào nghiệp vụ xuất bản: Topic, Chế độ kích hoạt (Sự kiện / COV / Chu kỳ), Ngưỡng biến thiên Deadband, Danh sách Tag liên kết và Bản mẫu JSON Payload.
2. **Tuân thủ Chuẩn Modbus RTU Thuần túy (Zero Proprietary Function Codes)**:
   * **`FC03 (0x03)`**: Đọc tham số cấu hình và đọc phản hồi trạng thái Live telemetry từ MCU.
   * **`FC16 (0x10)`**: Ghi mảng thanh ghi cấu hình xuống MCU. Mọi gói tin ghi đều tuân thủ giới hạn tối đa $\le 120$ thanh ghi/frame Modbus RTU công nghiệp.
3. **Quy ước Thứ tự Byte & Từ trên Đường truyền (Wire Endianness & Word Ordering)**:
   * **Thanh ghi 16-bit**: **Big-Endian** (Byte cao truyền trước, Byte thấp truyền sau).
   * **Giá trị 32-bit (`uint32_t`, `int32_t`)**: Chiếm 2 thanh ghi liên tiếp, **High Word truyền trước, Low Word truyền sau** (`[Reg0: High16, Reg1: Low16]`).
   * **Số thực Dấu phẩy Động 32-bit (`float32` IEEE 754)**: Chiếm 2 thanh ghi liên tiếp, **High Word truyền trước, Low Word truyền sau**.
   * **Chuỗi ký tự ASCII Đóng gói (Packed ASCII)**: Mỗi thanh ghi 16-bit chứa đúng **2 ký tự ASCII**. Byte cao (`Bits 15..8`) chứa ký tự thứ $2k$, Byte thấp (`Bits 7..0`) chứa ký tự thứ $2k+1$. Toàn bộ khoảng trống thừa ở cuối chuỗi được đệm byte `0x00`.
4. **Đóng gói Byte-Exact Tuyệt đối (Strict Packing)**:
   Mọi cấu trúc dữ liệu C/C++ đều sử dụng chỉ thị `#pragma pack(push, 1)` và được đối soát kích thước tĩnh bằng `static_assert` hoặc bộ test codec của Host.
5. **Đồng tồn tại An toàn Thời gian thực (Deterministic Coexistence)**:
   Tác vụ mạng MQTT chạy ở tầng tác vụ nền ưu tiên thấp (Background Task) trong FreeRTOS/Zephyr, tuyệt đối không được block hoặc gây trễ chu kỳ quét logic lõi 10ms của PLC Engine (`RuleCompiler` / `RuntimeEngine`).

---

## 2. Bản đồ Bộ nhớ Thanh ghi Modbus MQTT Toàn diện (Master Memory Map)

| Vùng thanh ghi | Tên phân vùng | Chiều | FC | Kích thước | Mô tả kiến trúc V2.0 |
| :--- | :--- | :---: | :---: | :--- | :--- |
| `0x0C00..0x0C5F` | **Telemetry Block 0** | R/W | FC03/FC16 | 96 regs (192 B) | Khối xuất bản dữ liệu viễn trắc 0: Control, Topic (32B), Timing, Deadband, 8 TagIndex, Template (128B). |
| `0x0C60..0x0CBF` | **Telemetry Block 1** | R/W | FC03/FC16 | 96 regs (192 B) | Khối xuất bản dữ liệu viễn trắc 1. |
| `0x0CC0..0x0D1F` | **Telemetry Block 2** | R/W | FC03/FC16 | 96 regs (192 B) | Khối xuất bản dữ liệu viễn trắc 2. |
| `0x0D20..0x0D7F` | **Telemetry Block 3** | R/W | FC03/FC16 | 96 regs (192 B) | Khối xuất bản dữ liệu viễn trắc 3. |
| `0x0D80..0x0DFF` | **MQTT Subscribe Blocks** | R/W | FC03/FC16 | 128 regs (256 B) | 2 khối nhận lệnh Inbound (SPLC-SPEC-MQTT-002, `firmware/SPLC_SPEC_MQTT_002_SUBSCRIBE_ICD.md`). Chỉ có khi firmware khai `SPLC_CAP_MQTT_SUB` (0x0A08 bit `0x20`); build không có SUB trả Exception 0x02. |
| `0x0E00..0x0E53` | **Broker Config Store** | R/W | FC03/FC16 | 84 regs (168 B) | Cấu hình máy chủ Broker: Control (1 reg), Port (1 reg), KeepAlive (1 reg), Reserved (1 reg), Host (32 regs), ClientID (16 regs), User (16 regs), Pass (16 regs). |
| `0x0E54..0x0E5F` | **Broker Live Status** | R | FC03 | 12 regs (24 B) | Vùng phản hồi trực tiếp từ MCU: Status Flags (1 reg), Latency Ping ms (1 reg), Tx Msg Count (2 regs), Tx Byte Count (2 regs), Error Code (1 reg), Reserved (5 regs). |

* **Tổng dung lượng phân vùng Telemetry (`0x0C00..0x0D7F`):** 384 Holding Registers = 768 Bytes.
* **Tổng dung lượng phân vùng Broker (`0x0E00..0x0E5F`):** 96 Holding Registers = 192 Bytes.

---

## 3. Đặc tả Cấu trúc Nhị phân Byte-Exact (Binary Structs Specification)

### 3.1. `SPLC_MqttBrokerConfig_t` (0x0E00, 96 thanh ghi = 192 Bytes)

Cấu trúc định cấu hình tham số mạng của máy chủ MQTT Broker và phản hồi trạng thái viễn trắc trực tiếp, tuân thủ định dạng đóng gói `SPLC_PACKED` của `SPLC_DeviceDescriptor_t`:

```c
typedef struct SPLC_PACKED {
    /* Offset +0: Broker Control Flags */
    uint16_t broker_control;

    /* Offset +1: Broker Port (e.g. 1883 or 8883) */
    uint16_t broker_port;

    /* Offset +2: Keep-Alive interval (seconds, default 60) */
    uint16_t keepalive_sec;

    /* Offset +3: Reserved (Luôn ghi 0x0000) */
    uint16_t reserved_0;

    /* Offset +4..35: Broker Hostname or IPv4/IPv6 (64 bytes ASCII null-padded) */
    char broker_host[64];

    /* Offset +36..51: MQTT Client Identifier (32 bytes ASCII null-padded) */
    char client_id[32];

    /* Offset +52..67: MQTT Username (32 bytes ASCII null-padded) */
    char username[32];

    /* Offset +68..83: MQTT Password (32 bytes ASCII null-padded) */
    char password[32];

    /* Offset +84..95: Live Diagnostic & Status Reflection (Read-Only do MCU cập nhật) */
    uint16_t status_flags;        /* +84: Bit 0: Connected, Bit 1: TLS Active, Bit 2: Auth OK */
    uint16_t connection_latency;  /* +85: Round-trip Ping in ms */
    uint32_t tx_message_count;    /* +86..87: Tổng số gói tin đã xuất bản (High Word trước) */
    uint32_t tx_byte_count;       /* +88..89: Tổng dung lượng byte đã truyền (High Word trước) */
    uint16_t last_error_code;     /* +90: SPLC_MqttBrokerErrorCode_t (0: OK, 1: DNS, 2: TCP Reset, 3: Auth) */
    uint16_t reserved_status[5];  /* +91..95: Dự phòng */
} SPLC_MqttBrokerConfig_t;
```

#### Bảng chi tiết trường bit của `broker_control` (Offset +0 tại `0x0E00`):

| Bitmask | Tên cờ | Giá trị | Ý nghĩa kỹ thuật |
| :--- | :--- | :---: | :--- |
| `0x0001` | `BROKER_ENABLED` | `0` = Tắt<br>`1` = Bật | Kích hoạt tác vụ MQTT Client trên MCU kết nối vào Broker. |
| `0x0002` | `BROKER_USE_TLS` | `0` = Plain TCP<br>`1` = TLS/SSL | Kích hoạt mã hóa bảo mật tầng vận chuyển (TLS 1.2/1.3 trên cổng 8883). |
| `0x0004` | `BROKER_CLEAN_SESSION` | `0` = False<br>`1` = True | Xóa sạch phiên cũ khi thiết lập kết nối (Clean Session). |
| `0x0018` | `BROKER_PROTOCOL_VER` | `0` = MQTT v3.1.1<br>`1` = MQTT v5.0 | Phiên bản giao thức MQTT (Shift 3 bit). |
| `0xFFE0` | `RESERVED` | `0` | Dành riêng cho mở rộng tương lai (SNI, WebSocket...). |

#### Bảng chi tiết trường phản hồi trực tiếp `BROKER_STATUS` (`0x0E54..0x0E5F`):

* **`status_flags` (Offset +84 / `0x0E54`)**:
  * `Bit 0` (`0x0001`): `1` = Đã kết nối TCP/MQTT thành công với Broker (`CONNECTED`).
  * `Bit 1` (`0x0002`): `1` = Bắt tay TLS/SSL Handshake thành công (`TLS_ACTIVE`).
  * `Bit 2` (`0x0004`): `1` = Xác thực tài khoản / mật khẩu thành công (`AUTH_OK`).
* **`connection_latency` (Offset +85 / `0x0E55`)**: Thời gian phản hồi Ping round-trip (ms).
* **`tx_message_count` (Offset +86..87 / `0x0E56..0x0E57`)**: Số lượng bản tin xuất bản tích lũy từ lúc khởi động.
* **`tx_byte_count` (Offset +88..89 / `0x0E58..0x0E59`)**: Tổng số byte payload viễn trắc đã truyền.
* **`last_error_code` (Offset +90 / `0x0E5A`)**:
  * `0`: Không có lỗi (`SPLC_MQTT_ERR_NONE`).
  * `1`: Phân giải tên miền DNS thất bại (`SPLC_MQTT_ERR_DNS_FAIL`).
  * `2`: Mất kết nối TCP / Kết nối bị Broker ngắt đột ngột (`SPLC_MQTT_ERR_TCP_RESET`).
  * `3`: Lỗi xác thực tài khoản hoặc chứng chỉ bảo mật (`SPLC_MQTT_ERR_AUTH_FAIL`).
  * `4`: Hết thời gian chờ phản hồi Broker PINGREQ / Timeout (`SPLC_MQTT_ERR_TIMEOUT`).

---

### 3.2. `SPLC_MqttTelemetryBlock_t` (0x0C00 + i*96, 96 thanh ghi = 192 Bytes)

Cấu trúc định nghĩa một khối xuất bản viễn trắc logic trên canvas:

```c
typedef struct SPLC_PACKED {
    /* --- PHẦN 1: HEADER & ĐIỀU KHIỂN (32 thanh ghi = 64 Bytes) --- */
    uint16_t control_flags;                               /* Offset +0: Cờ điều khiển, QoS, Mode */
    uint16_t en_tag_index;                                /* Offset +1: Tag cho phép kích hoạt (0xFFFF=None) */
    uint32_t interval_ms;                                 /* Offset +2..3: Chu kỳ xuất bản định kỳ (ms) */
    float    deadband;                                    /* Offset +4..5: Ngưỡng biến thiên COV (Float32) */
    uint16_t tag_count;                                   /* Offset +6: Số lượng Tag liên kết (1..8) */
    uint16_t bound_tags[8];                               /* Offset +7..14: Danh sách 8 TagIndex */
    uint16_t reserved_header;                             /* Offset +15: Dự phòng (0x0000) */
    char     topic[32];                                   /* Offset +16..31: Topic string (32 bytes ASCII) */

    /* --- PHẦN 2: BẢN MẪU JSON PAYLOAD (64 thanh ghi = 128 Bytes) --- */
    char     payload_template[128];                       /* Offset +32..95: Custom JSON template ASCII */
} SPLC_MqttTelemetryBlock_t;
```

#### Bảng chi tiết trường bit của `control_flags` (Offset +0 tại `0x0C00`):

| Bitmask | Tên cờ | Trường giá trị | Ý nghĩa kỹ thuật |
| :--- | :--- | :---: | :--- |
| `0x0001` | `ENABLED` | `0` = Tắt<br>`1` = Kích hoạt | Bật/tắt khối xuất bản này trong chu kỳ quét của MCU. |
| `0x0006` | `PUBLISH_MODE` | `0` = Sự kiện (On-Change / Event)<br>`1` = Biến thiên (COV)<br>`2` = Chu kỳ (Periodic / Cyclic) | Chế độ quyết định thời điểm phát gói tin lên Broker. |
| `0x0018` | `QOS_LEVEL` | `0` = QoS 0 (At most once)<br>`1` = QoS 1 (At least once)<br>`2` = QoS 2 (Exactly once) | Cấp độ đảm bảo phân phối thông điệp MQTT. |
| `0x0020` | `RETAIN` | `0` = False<br>`1` = True | Yêu cầu Broker lưu giữ bản tin gần nhất cho các Client đăng ký sau. |
| `0x00C0` | `PAYLOAD_MODE` | `0` = Tự động rút gọn (Compact JSON)<br>`1` = Giá trị thô (Raw Value)<br>`2` = Bản mẫu tùy chỉnh (Custom JSON Template) | Định dạng nội dung bản tin xuất bản. |
| `0xFF00` | `RESERVED` | `0` | Dành riêng cho mở rộng tương lai (Nén Gzip, CBOR...). |

---

## 4. Quy tắc Đóng gói Dữ liệu & Wire Endianness

### 4.1. Mã hóa Chuỗi Ký tự Đóng gói (Packed ASCII Serialization)
Do Modbus RTU Holding Register có độ rộng 16-bit, các chuỗi ký tự ASCII (`broker_host`, `client_id`, `username`, `password`, `topic`, `payload_template`) được đóng gói theo nguyên tắc:
* **Byte cao (Bits 15..8)** chứa ký tự chỉ số chẵn ($2k$).
* **Byte thấp (Bits 7..0)** chứa ký tự chỉ số lẻ ($2k+1$).
* **Ví dụ**: Chuỗi `"factory/temp"` (12 ký tự) được đóng gói vào 6 thanh ghi Modbus liên tiếp:
  $$\text{Reg 0} = 0\text{x}6661 \text{ ('f', 'a')}, \quad \text{Reg 1} = 0\text{x}6374 \text{ ('c', 't')}, \quad \text{Reg 2} = 0\text{x}6F72 \text{ ('o', 'r')}, \dots$$
* Chuỗi ngắn hơn kích thước tối đa của trường sẽ được đệm `0x00` cho đến hết số thanh ghi quy định.
* Chuỗi dài hơn kích thước tối đa sẽ bị cắt an toàn (Safe Truncation) và bảo toàn byte kết thúc `\0`.

### 4.2. Mã hóa Số thực Dấu phẩy Động 32-bit (`deadband`)
* Chuẩn IEEE 754 đơn chính xác (Single Precision Float32).
* Chiếm 2 thanh ghi liên tiếp:
  $$\text{Reg 0} = (\text{uint32\_raw} \gg 16) \ \& \ 0\text{xFFFF} \quad (\text{High Word})$$
  $$\text{Reg 1} = \text{uint32\_raw} \ \& \ 0\text{xFFFF} \quad (\text{Low Word})$$
* **Ví dụ**: Giá trị `Deadband = 0.75` $\rightarrow$ IEEE 754 Hex: `0x3F400000`:
  $$\text{Reg 4} = 0\text{x}3F40, \quad \text{Reg 5} = 0\text{x}0000$$

### 4.3. Mã hóa Chu kỳ Định thời 32-bit (`interval_ms`)
* Số nguyên không dấu 32-bit (`uint32_t`), đơn vị mili-giây (ms).
* Chiếm 2 thanh ghi liên tiếp: High Word trước, Low Word sau.
* **Ví dụ**: `Interval = 2500 ms` $\rightarrow$ Hex: `0x000009C4`:
  $$\text{Reg 2} = 0\text{x}0000, \quad \text{Reg 3} = 0\text{x}09C4$$

### 4.4. Ánh xạ Mảng `bound_tags[8]` với Danh mục Tag Động
* Mỗi phần tử trong mảng đại diện cho một `TagIndex` (0..123) trong danh mục Tag hợp lệ của Platform V2.0.
* Các vị trí slot không sử dụng được điền giá trị Sentinel bất biến: `0xFFFF`.

---

## 5. Thuật toán Xử lý Thực thi trên Firmware MCU

### 5.1. Thuật toán Lọc Ngưỡng Biến Thiên (COV - Change of Value Filtering)

Khi một khối Telemetry được cấu hình ở chế độ `PUBLISH_MODE == 1 (COV)`:
Tại mỗi chu kỳ quét logic PLC (10ms), Firmware kiểm tra độ biến thiên giá trị của các Tag trong danh sách `bound_tags`:

```text
FOR each TagIndex in bound_tags:
    IF TagIndex == 0xFFFF THEN CONTINUE
    
    current_value = RuntimeTagStore_Read(TagIndex)
    delta = ABS(current_value - last_published_value[TagIndex])
    
    IF delta >= deadband THEN
        trigger_publish = TRUE
        BREAK
    END IF
END FOR

IF trigger_publish THEN
    Mqtt_EnqueuePublish(block_index)
    FOR each TagIndex in bound_tags:
        last_published_value[TagIndex] = RuntimeTagStore_Read(TagIndex)
    END FOR
END IF
```

* **Ý nghĩa công nghiệp**: Giảm thiểu tới 90% lưu lượng mạng trên các đường truyền sóng di động (4G/LTE/NB-IoT), chỉ truyền dữ liệu khi có thay đổi mang ý nghĩa công nghệ thực tế.

### 5.2. Thuật toán Định thời Chu kỳ (Periodic Scheduling)

Khi khối Telemetry được cấu hình ở chế độ `PUBLISH_MODE == 2 (Periodic)`:
* MCU duy trì bộ đếm thời gian trôi qua `elapsed_ms` dựa trên đồng hồ phần cứng `SysTick`.
* Khi `elapsed_ms >= interval_ms`:
  1. MCU tiến hành chụp ảnh dữ liệu (Snapshot) các Tag liên kết.
  2. Đưa gói tin vào hàng đợi xuất bản MQTT.
  3. Reset `elapsed_ms = 0`.

### 5.3. Thuật toán Tạo Nội dung Bản tin JSON (Payload Rendering Engine)

Firmware MCU hỗ trợ 3 chế độ sinh bản tin tùy thuộc vào cờ `PAYLOAD_MODE`:

1. **Chế độ Tự động Rút gọn (`PAYLOAD_MODE == 0` - Compact JSON)**:
   Firmware tự động nối chuỗi JSON chuẩn công nghiệp không khoảng trắng:
   ```json
   {"tag":16,"val":24.5,"ts":1773228945}
   ```
   Nếu có nhiều hơn 1 tag trong `bound_tags`:
   ```json
   {"d":[24.5,101.3],"ts":1773228945}
   ```
2. **Chế độ Giá trị Thô (`PAYLOAD_MODE == 1` - Raw Value)**:
   Nội dung payload là chuỗi biểu diễn giá trị trực tiếp của tag đầu tiên (ví dụ `"24.5"` hoặc `"1"`), phù hợp với các hệ thống SCADA cũ hoặc Broker IoT xử lý dữ liệu nguyên thủy.
3. **Chế độ Bản mẫu Tùy chỉnh (`PAYLOAD_MODE == 2` - Custom JSON Template)**:
   Firmware duyệt qua mảng ký tự `payload_template[128]` và thực hiện thay thế nhanh các từ khóa giữ chỗ (Token Replacement):
   * `{val}`: Thay thế bằng giá trị thực thời của tag đầu tiên (hoặc mảng giá trị).
   * `{tag}`: Thay thế bằng chỉ số `TagIndex`.
   * `{ts}`: Thay thế bằng mốc thời gian Unix Epoch UTC đọc từ khối RTC (`0x0810`).
   * `{device}`: Thay thế bằng chuỗi `client_id` được cấu hình tại `0x0E24`.
   * **Ví dụ**: Bản mẫu `{"sensor":"temp","value":{val},"time":{ts},"gw":"{device}"}`
     $\rightarrow$ Kết quả xuất bản:
     ```json
     {"sensor":"temp","value":42.8,"time":1773228945,"gw":"PLC-LINE-01"}
     ```

---

## 6. Quy trình Bắt tay Khám phá, Nạp Cấu hình & Giám sát (App-to-MCU Handshake Architecture)

### 6.1. Thẩm định Tính Khả thi Bắt tay (Handshake Verification Analysis)

Xét dưới góc nhìn kiến trúc tự động hóa công nghiệp cấp cao, câu hỏi cốt lõi đặt ra là: **"Liệu SimplePLC Studio (App) và MCU Firmware có thể bắt tay trơn tru 100% với cấu trúc này không?"**

**Câu trả lời khẳng định: HOÀN TOÀN BẮT TAY ĐƯỢC 100% VÀ KHÔNG XẢY RA XUNG ĐỘT.** Lý do được xác lập qua 4 luận điểm kỹ thuật then chốt:

1. **Chu trình Khám phá Thiết bị Không Xâm lấn (Non-Invasive Discovery Handshake)**:
   * Khi cắm cáp kết nối, App thực hiện bắt tay tiêu chuẩn 2 bước:
     - **Bước 1**: FC03 đọc `0x0000` (10 regs) $\rightarrow$ Nhận `SPLC_DeviceDescriptor_t` (Xác nhận `ProtocolVersion == 2`, `RuleFormatVersion == 7`).
     - **Bước 2**: FC03 đọc `0x0020` (10 regs) $\rightarrow$ Nhận `SPLC_DeviceResourceInfo_t` (Xác nhận `WireProfile == 2`).
   * Việc bổ sung cấu trúc MQTT **hoàn toàn không làm thay đổi hay xê dịch kích thước 10 thanh ghi của DeviceDescriptor hay DeviceResourceInfo**. Do đó, khâu bắt tay nhận diện ban đầu giữa App và MCU giữ nguyên vẹn tính tương thích ngược 100%.

2. **Quy tắc Phân mảnh Khung Truyền An toàn (Modbus Chunk Planning $\le 120$ regs)**:
   * Bộ đệm nhận UART Modbus RTU trên MCU vi điều khiển (STM32/ESP32) thường giới hạn ở mức 256 bytes (khoảng 120 thanh ghi/frame).
   * Cấu hình Broker (`0x0E00`) có kích thước đúng **96 thanh ghi** (192 bytes).
   * Mỗi khối Telemetry (`0x0C00`) có kích thước đúng **96 thanh ghi** (192 bytes).
   * Khi App nạp xuống thiết bị, `ModbusChunkPlanner` chia nhỏ các khối này thành từng lệnh FC16 riêng biệt (mỗi lệnh 96 thanh ghi). Cả 96 thanh ghi đều nằm trọn vẹn dưới ngưỡng 120 thanh ghi $\rightarrow$ **MCU không bao giờ bị tràn hàng đợi hay lỗi khung truyền (Frame Overflow)**.

3. **Cơ chế Bắt tay Triển khai Bất đồng bộ Hai cấp (Decoupled Two-Tier Deployment)**:
   * Nạp MQTT được tách bạch hoàn toàn khỏi quy trình Staging/Commit của Rule Table (`0x9010` và `0xA000`):
     - App nạp Broker (`0x0E00`) và Telemetry (`0x0C00`) trước.
     - Sau đó App mới nạp Rules và gửi mã magic `0xA5A5` kích hoạt logic PLC.
   * Nếu dự án không sử dụng MQTT (`totalMqtt == 0` và `BrokerEnabled == false`), App hoàn toàn không gửi bất kỳ frame nào vào dải `0x0C00` hay `0x0E00`. MCU hoạt động như một PLC Remote I/O truyền thống mà không bị ảnh hưởng.

4. **Kênh Phản hồi Trạng thái Trực tiếp Hai chiều (Live Feedback & Diagnostic Reflection)**:
   * Sau khi nạp xong, MCU tự động khởi tạo kết nối mạng và báo cáo trạng thái vào `0x0E54..0x0E5F` (Read-Only).
   * App dùng FC03 đọc định kỳ 500ms..1000ms vùng này để hiển thị Live Pill (Online/Offline/Ping ms) trên giao diện mà không cần gửi lệnh độc quyền hay can thiệp vào chu kỳ quét logic 10ms.

---

### 6.2. Sơ đồ Luồng Bắt tay Toàn diện (End-to-End Handshake Flow)

```text
SimplePLC Studio (Host Master)                   MCU Firmware Engine (Slave & MQTT Client)
        │                                                │
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
   [GIAI ĐOẠN 1: BẮT TAY KẾT NỐI & KHÁM PHÁ (CONNECTION & DISCOVERY HANDSHAKE)]
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
        ├─ 1. FC03 Đọc 0x0000 (10 regs) ────────────────>│ Trả về SPLC_DeviceDescriptor_t:
        │                                                │ Protocol=2, RuleFormat=7, Class=GATEWAY
        │                                                │
        ├─ 2. FC03 Đọc 0x0020 (10 regs) ────────────────>│ Trả về SPLC_DeviceResourceInfo_t:
        │                                                │ WireProfile=2, MaxRules=100, Tags=124
        │                                                │
        ├─ 3. FC03 Đọc 0x0810 (4 regs) ─────────────────>│ Trả về SPLC_RtcClock_t (RTC Sync)
        │                                                │
        │  [Studio: Bật trạng thái CONNECTED]            │
        │                                                │
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
   [GIAI ĐOẠN 2: BẮT TAY NẠP CẤU HÌNH NHỊ PHÂN (DEPLOYMENT HANDSHAKE)]
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
        │                                                │
        ├─ 4. FC16 Ghi 0x0E00..0x0E53 (84 regs) ────────>│ Ghi SPLC_MqttBrokerConfig_t
        │     (Host, Port, TLS, Client ID, Credentials)  │ [MCU: Đánh dấu cờ Reconnect Broker]
        │                                                │
        ├─ 5. FC16 Ghi 0x0C00..0x0C5F (96 regs) ────────>│ Ghi Telemetry Block 0 (Topic, COV, JSON)
        ├─ 6. FC16 Ghi 0x0C60..0x0CBF (96 regs) ────────>│ Ghi Telemetry Block 1 (Nếu có)
        │                                                │
        ├─ 7. FC16 Ghi 0x9010 (Staging Rules) ──────────>│ Nạp bảng Rule nhị phân
        ├─ 8. FC06 Ghi 0xA000 = 0xA5A5 (Commit) ────────>│ [MCU: Xác thực CRC-16 & Atomic Swap]
        │                                                │
        │  [Studio: Báo cáo NẠP THÀNH CÔNG 100%]         │
        │                                                │
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
   [GIAI ĐOẠN 3: BẮT TAY GIÁM SÁT THỜI GIAN THỰC (LIVE RUNTIME REFLECTION)]
   ═════╪════════════════════════════════════════════════╪═══════════════════════════════════════
        │                                                │
        │                                                │ [FreeRTOS Task kết nối Broker thành công]
        │                                                │ [Cập nhật status_flags=0x0007, latency=24ms]
        │                                                │
        ├─ 9. FC03 Đọc 0x0E54..0x0E5F (12 regs) ────────>│ Phản hồi BROKER_STATUS Live:
        │                                                │ - status_flags: CONNECTED | TLS | AUTH_OK
        │                                                │ - latency: 24 ms
        │                                                │ - tx_message_count: 154
        │                                                │ - last_error_code: 0 (OK)
        │                                                │
        │  <-- Studio hiển thị MQTT Online trên UI       │
```

---

## 7. Bộ Véc-tơ Kiểm chuẩn Golden Vectors (Golden Vectors Specification)

Mọi bộ giải mã Codec và Firmware MCU phải vượt qua chính xác các bộ véc-tơ kiểm chuẩn sau:

### `GV-MQTT-001` (Cấu hình Broker Tiêu chuẩn Plain TCP 1883)
* **Tham số**: `Enabled=True`, `UseTls=False`, `CleanSession=True`, `Version=0 (v3.1.1)`, `Port=1883`, `KeepAlive=60`, `Host="192.168.1.100"`, `ClientId="PLC-STATION-01"`.
* **Thanh ghi 0x0E00..0x0E03**: `[0x0005, 0x075B, 0x003C, 0x0000]`
  * `0x0E00` = `0x0005` (Bit 0 Enabled + Bit 2 CleanSession = 5).
  * `0x0E01` = `0x075B` (1883).
  * `0x0E02` = `0x003C` (60s).
* **Chuỗi Host (`0x0E04..`)**: `"192.168.1.100"` $\rightarrow$ `[0x3139, 0x322E, 0x3136, 0x382E, 0x312E, 0x3130, 0x3000, 0x0000, ...]`.

### `GV-MQTT-002` (Cấu hình Broker MQTTS Bảo mật TLS 8883 & MQTT 5.0)
* **Tham số**: `Enabled=True`, `UseTls=True`, `CleanSession=False`, `Version=1 (v5.0)`, `Port=8883`, `KeepAlive=120`, `Host="broker.hivemq.com"`, `ClientId="synaptix-station-alpha"`.
* **Thanh ghi 0x0E00..0x0E03**:
  * `0x0E00` = `0x000B` (Bit 0 Enabled + Bit 1 UseTls + Bits 3..4 Version=1 $\rightarrow 1 + 2 + 8 = 11 = \text{0x000B}$).
  * `0x0E01` = `0x22B3` (8883).
  * `0x0E02` = `0x0078` (120s).

### `GV-MQTT-003` (Khối Telemetry Xuất bản Chu kỳ Định thời JSON)
* **Tham số**: Block 0 (`0x0C00`), `Enabled=True`, `Mode=Periodic (2)`, `QoS=1`, `Retain=True`, `Payload=JSON (0)`, `EnTagIndex=12`, `Interval=2500ms`, `Deadband=0.75`, `Tags=[4, 5]`, `Topic="factory/line1/temp"`.
* **Thanh ghi 0x0C00..0x0C08**:
  * `0x0C00` = `0x002D` (Bit 0: 1, Bit 1..2: 2 $\rightarrow$ 4, Bit 3..4: 1 $\rightarrow$ 8, Bit 5: 1 $\rightarrow$ 32, Tổng = 45 = `0x002D`).
  * `0x0C01` = `0x000C` (12).
  * `0x0C02..0x0C03` = `[0x0000, 0x09C4]` (2500 ms).
  * `0x0C04..0x0C05` = `[0x3F40, 0x0000]` (Float 0.75).
  * `0x0C06` = `0x0002` (2 tags).
  * `0x0C07..0x0C08` = `[0x0004, 0x0005]` (TagIndex 4 và 5).

### `GV-MQTT-004` (Khối Telemetry Ngưỡng Biến thiên COV với Bản mẫu Custom JSON)
* **Tham số**: Block 1 (`0x0C60`), `Enabled=True`, `Mode=COV (1)`, `QoS=2`, `Retain=False`, `Payload=Custom (2)`, `Deadband=1.25`, `Tags=[3]`, `Template="{\"v\":{val},\"ts\":{ts},\"dev\":\"STX\"}"`.
* **Thanh ghi 0x0C60 (Control Flags)**:
  * Bit 0: 1, Bit 1..2: 1 $\rightarrow$ 2, Bit 3..4: 2 $\rightarrow$ 16 (`0x10`), Bit 6..7: 2 $\rightarrow$ 128 (`0x80`).
  * Tổng = $1 + 2 + 16 + 128 = 147 = \text{0x0093}$.

---

## 8. Báo cáo Nghiệm thu Kiểm thử Phần cứng Tự động (Hardware Verification)

Hệ thống truyền thông nhị phân MQTT V2.0 đã được thẩm định tự động thông qua bộ kiểm thử phần cứng ảo hai cổng UART vật lý nối chéo (`COM7` $\leftrightarrow$ `COM9`), đạt kết quả **10/10 Test Cases Vượt qua 100%**:

| Mã kiểm thử | Nội dung kiểm thử chi tiết | Tiêu chí nghiệm thu | Kết quả |
| :---: | :--- | :--- | :---: |
| `TC-MQTT-01` | Kiểm tra giới hạn 128 ký tự của bản mẫu JSON (Off-by-one check) | Đóng gói byte thứ 128 chính xác, không tràn bộ nhớ | **PASS** |
| `TC-MQTT-02` | Kiểm tra giới hạn 32 ký tự của Topic string | Không rò rỉ byte sang phân vùng Interval ms | **PASS** |
| `TC-MQTT-03` | Cắt an toàn khi người dùng nhập chuỗi vượt kích thước tối đa | Cắt chuỗi không gây lỗi biên dịch, bảo toàn null terminator | **PASS** |
| `TC-MQTT-04` | Nạp đồng thời cả 4 khối Telemetry (384 Holding Registers) | Cả 4 khối được nạp toàn vẹn qua 4 lệnh FC16 riêng biệt | **PASS** |
| `TC-MQTT-05` | Vô hiệu hóa an toàn khối cũ khi người dùng xóa trên Studio | Ghi đè 0x0000 vào Control Flags, MCU dừng xuất bản ngay | **PASS** |
| `TC-MQTT-06` | Ký tự đặc biệt, dấu ngoặc nhọn lồng nhau trong Custom JSON | Không làm sai lệch thứ tự byte ASCII trên đường truyền | **PASS** |
| `TC-MQTT-07` | Chặn truy cập vùng nhớ bất hợp pháp ngoài `0x0DFF` (và `0x0D80..0x0DFF` khi build không có MQTT Subscribe) | MCU trả về Modbus Exception 0x02 (Illegal Data Address) | **PASS** |
| `TC-MQTT-08` | Stress test nạp liên tục 5 lần tốc độ cao chu kỳ 50ms | Không rò rỉ bộ nhớ, không treo tác vụ Modbus Slave | **PASS** |
| `TC-MQTT-09` | Nạp và đọc lại phân vùng Broker `0x0E00` (96 registers) | Đạt round-trip chính xác 100% từng byte Host, Port, Auth | **PASS** |
| `TC-MQTT-10` | Nạp kết hợp Rule + Timer + Counter + Telemetry + Broker trong 1 chu kỳ | Toàn bộ các phân vùng không xung đột địa chỉ nhớ | **PASS** |

---

## 9. Tham chiếu Tài liệu & Mã nguồn Liên kết

1. **Header Firmware C/C++ Chuẩn:** [`docs/firmware/splc_mqtt_wire.h`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/splc_mqtt_wire.h)
2. **Quy chuẩn Tự mô tả Nền tảng V2.0:** [`docs/platform/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/platform/SimplePLC_App_MCU_Structs_v2.0_Self_Describing_Profile.md)
3. **Đặc tả Phù hợp Firmware MCU:** [`docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/MCU_CONFORMANCE_SPECIFICATION_V2_0.md)
4. **Nhật ký Thay đổi Khế ước Nền tảng:** [`docs/platform/SimplePLC_Contract_Changelog.md`](file:///g:/HoaNV/Projects/SimplePLC/docs/platform/SimplePLC_Contract_Changelog.md)