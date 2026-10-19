# TÀI LIỆU ĐẶC TẢ GIAO DIỆN TRUYỀN THÔNG (ICD)
## PHÂN HỆ MQTT IIOT SUBSCRIBE & INBOUND COMMAND CONTROL CHO FIRMWARE MCU
**Mã tài liệu:** `SPLC-SPEC-MQTT-002`  
**Phiên bản:** `2.0.0`  
**Ngày ban hành:** `2026-10-14`  
**Đơn vị ban hành:** Ban Công Nghệ & Tự Động Hóa SynaptiX  
**Tiêu chuẩn liên quan:** IEC 61131-3, Modbus Application Protocol V1.1b3, OASIS MQTT V3.1.1/V5.0  

---

## 1. TỔNG QUAN HỆ THỐNG (SYSTEM OVERVIEW)

Tài liệu này đặc tả giao diện nạp cấu hình nhị phân và kiến trúc xử lý bản tin đến (**Inbound MQTT Subscribe**) cho vi điều khiển mục tiêu (**MCU STM32 / ESP32 / Industrial Gateway**) trong hệ sinh thái **SimplePLC / SynaptiX IDE**.

Trong khi phân hệ **MQTT Telemetry PUB (`0x0C00`)** thực hiện thu thập và xuất bản dữ liệu từ PLC lên Broker, phân hệ **MQTT Subscribe (`0x0D80`)** đóng vai trò:
1. Tiếp nhận các bản tin lệnh điều khiển từ xa (Remote Command / Setpoint) từ SCADA, MES, hoặc Cloud.
2. Trích xuất an toàn các trường dữ liệu (Tokens) từ chuỗi JSON hoặc chuỗi số thô (Raw string).
3. Áp dụng giá trị vào bảng biến runtime (`g_tag_store`) theo nguyên tắc **Thread-Safe tại ranh giới chu kỳ quét (Scan Pass Boundary)** của Rule Engine PLC.
4. Quản lý trạng thái an toàn (**Fail-Safe Watchdog**): Tự động khôi phục giá trị mặc định hoặc reset về 0 nếu mất kết nối mạng hoặc không nhận được tin trong khoảng thời gian quy định ($T_{\text{timeout}}$).

```
   ┌─────────────────────────────────────────────────────────────────┐
   │                    SynaptiX IDE / Studio                        │
   │  - Logic Editor: Khối [MQTT SUB] Node (0x0D80..0x0DFF)          │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ Modbus RTU FC16 Chunks
                                  ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                  MCU STM32 / ESP32 Firmware                     │
   │  ┌───────────────────────────────────────────────────────────┐  │
   │  │ 0x0D80: 2x Subscribe Blocks (128 regs / 256 bytes)        │  │
   │  │ (Topic, QoS, FailSafe Timeout, 4x Pin Bindings)           │  │
   │  └───────────────────────────────────────────────────────────┘  │
   │                                                                 │
   │  ┌───────────────────────────┬───────────────────────────────┐  │
   │  │ MQTT Client Callback Task │ FreeRTOS Inbound Queue (Ring) │  │
   │  └─────────────┬─────────────┴───────────────┬───────────────┘  │
   │                │ Nhận bản tin từ Broker      │                  │
   │                ▼                             ▼                  │
   │         [ MQTT Broker ]             [ Bộ Giải Mã cJSON/JSMN ]   │
   │                                              │                  │
   │  ┌───────────────────────────────────────────┼───────────────┐  │
   │  │ 10ms PLC Scan Pass Loop                   ▼               │  │
   │  │ 1. Cập nhật Inbound Tags (Atomic Copy) ──► g_tag_store     │  │
   │  │ 2. Kiểm tra FailSafe Timeout Watchdog                     │  │
   │  │ 3. Thực thi Đồ thị Luật FBD (Rule Execution)              │  │
   │  │ 4. Xuất Physical Outputs (DO / AO)                        │  │
   │  └───────────────────────────────────────────────────────────┘  │
   └─────────────────────────────────────────────────────────────────┘
```

---

## 2. PHÂN BỔ BỘ NHỚ MODBUS HOLDING REGISTERS (`0x0D80..0x0DFF`)

### 2.1. Địa chỉ và Phân vùng
- **Dải địa chỉ Modbus:** `0x0D80 - 0x0DFF` (128 Holding Registers = 256 Bytes)
- **Số lượng khối đăng ký:** 2 Blocks (Block 0: `0x0D80`, Block 1: `0x0DC0`)
- **Độ dài mỗi khối:** 64 Holding Registers = 128 Bytes
- **Cơ chế ghi:** Modbus FC16 (Write Multiple Registers)
- **Cơ chế đọc:** Modbus FC03 (Read Holding Registers)

### 2.2. Chi tiết Cấu trúc 1 Khối Subscribe (64 Registers = 128 Bytes)

| Địa chỉ Offset | Tên trường | Kiểu dữ liệu | Mô tả chi tiết |
| :---: | :--- | :--- | :--- |
| `+0` | `CONTROL_FLAGS` | `uint16` | **Bit 0:** Enable (1 = Bật khối nhận tin, 0 = Tắt)<br>**Bit 1..2:** QoS level (0 = At most once, 1 = At least once, 2 = Exactly once)<br>**Bit 3:** Queue Mode (0 = Latest Only - ghi đè bản tin mới nhất, 1 = FIFO Queue)<br>**Bit 4:** Ignore Retained (1 = Bỏ qua bản tin retained khi vừa boot, tránh nhận lệnh cũ)<br>**Bit 5..6:** Fail-Safe Action (0 = Giữ nguyên giá trị cũ, 1 = Reset về 0, 2 = Set về Default Value) |
| `+1` | `RESERVED` | `uint16` | Dự phòng căn gióng byte (`0x0000`) |
| `+2..+3` | `FAILSAFE_TIMEOUT_MS` | `uint32` | Thời gian timeout mất tin (ms) trước khi kích hoạt Fail-Safe (Big-Endian uint32). Giá trị `0` = Vô hiệu hóa watchdog timeout. |
| `+4` | `PIN_COUNT` | `uint16` | Số lượng chân dữ liệu trích xuất từ payload (1..4 chân) |
| `+5..+7` | `RESERVED_PINS` | `uint16[3]` | Dự phòng cho mở rộng tương lai (`0x0000`) |
| `+8..+23` | `TOPIC` | `char[32]` | Chuỗi MQTT Topic đăng ký (32 bytes ASCII null-padded, 2 ký tự/reg Big-Endian). Hỗ trợ các wildcard chuẩn MQTT `+` (đơn tầng) và `#` (đa tầng). |
| `+24..+33` | `PIN_0` | `PinBinding` | Cấu hình chân dữ liệu 0 (10 registers = 20 bytes) |
| `+34..+43` | `PIN_1` | `PinBinding` | Cấu hình chân dữ liệu 1 (10 registers = 20 bytes) |
| `+44..+53` | `PIN_2` | `PinBinding` | Cấu hình chân dữ liệu 2 (10 registers = 20 bytes) |
| `+54..+63` | `PIN_3` | `PinBinding` | Cấu hình chân dữ liệu 3 (10 registers = 20 bytes) |

### 2.3. Cấu trúc Chân Dữ Liệu `PinBinding` (10 Registers = 20 Bytes)

| Offset con | Tên trường | Kiểu dữ liệu | Mô tả chi tiết |
| :---: | :--- | :--- | :--- |
| `+0` | `TARGET_TAG_INDEX` | `uint16` | Chỉ số Tag mục tiêu trong `g_tag_store` (0..127) nhận giá trị (VFLAG, VREG, DO...). |
| `+1` | `DATA_TYPE` | `uint16` | Kiểu dữ liệu đích:<br>`1` = `BOOL` (0 hoặc 1)<br>`2` = `INT32` / `DINT` (Số nguyên có dấu 32-bit)<br>`3` = `REAL` (Số thực 32-bit IEEE 754) |
| `+2..+3` | `DEFAULT_VALUE_RAW` | `uint32` | Giá trị mặc định / Giá trị an toàn Fail-safe (Big-Endian uint32 hoặc float32). |
| `+4..+9` | `JSON_KEY` | `char[12]` | Khóa JSON cần bóc tách (12 bytes ASCII null-padded, ví dụ: `"cmd"`, `"speed"`, `"setpoint"`). Nếu payload nhận về là chuỗi số đơn (Raw value) thì để trống byte đầu tiên (`json_key[0] == '\0'`). |

---

## 3. CẤU TRÚC DỮ LIỆU NHỊ PHÂN C CHO FIRMWARE

Được định nghĩa trong header [`simpleplc_protocol_v2_0.h`](file:///g:/HoaNV/Projects/SimplePLC/src/SimplePLC.Protocol/Firmware/simpleplc_protocol_v2_0.h):

```c
#define SPLC_ADDR_MQTT_SUBSCRIBE          0x0D80  /* 128 regs: 2 Blocks * 64 regs (FC03/FC16, RW) */
#define SPLC_LEN_MQTT_SUBSCRIBE_TOTAL     128
#define SPLC_MQTT_MAX_SUBSCRIBE_BLOCKS    2
#define SPLC_MQTT_REGS_PER_SUB_BLOCK      64
#define SPLC_MQTT_BYTES_PER_SUB_BLOCK     128
#define SPLC_MQTT_MAX_PINS_PER_SUB_BLOCK  4
#define SPLC_MQTT_MAX_JSON_KEY_CHARS      12

/* Bitmask cờ điều khiển Subscribe Block */
#define SPLC_MQTT_SUB_CTRL_ENABLED_BIT       0x0001
#define SPLC_MQTT_SUB_CTRL_QOS_MASK          0x0006
#define SPLC_MQTT_SUB_CTRL_QOS_SHIFT         1
#define SPLC_MQTT_SUB_CTRL_QUEUE_FIFO_BIT    0x0008
#define SPLC_MQTT_SUB_CTRL_IGNORE_RETAIN_BIT 0x0010
#define SPLC_MQTT_SUB_CTRL_FAILSAFE_MASK     0x0060
#define SPLC_MQTT_SUB_CTRL_FAILSAFE_SHIFT    5

typedef struct SPLC_PACKED {
    uint16_t target_tag_index;
    uint16_t data_type;         /* 1=BOOL, 2=INT32, 3=REAL */
    union {
        uint32_t default_val_raw;
        int32_t  default_val_int;
        float    default_val_real;
    };
    char     json_key[SPLC_MQTT_MAX_JSON_KEY_CHARS]; /* 12 bytes null-padded */
} SPLC_MqttSubPinBinding_t;

typedef struct SPLC_PACKED {
    uint16_t control_flags;
    uint16_t reserved;
    uint32_t failsafe_timeout_ms;
    uint16_t pin_count;
    uint16_t reserved_pins[3];
    char     topic[SPLC_MQTT_MAX_TOPIC_CHARS]; /* 32 bytes null-padded */
    SPLC_MqttSubPinBinding_t pins[SPLC_MQTT_MAX_PINS_PER_SUB_BLOCK]; /* 4 pins * 20 bytes = 80 bytes */
} SPLC_MqttSubscriptionBlock_t;
```

---

## 4. QUY TRÌNH THỰC THI FIRMWARE (MCU IMPLEMENTATION GUIDE)

### 4.1. Khởi Tạo & Đăng Ký Topic Khi Có Kết Nối MQTT Broker
Khi MQTT Client Task của MCU kết nối thành công với Broker:
1. Duyệt qua 2 khối Subscribe tại `0x0D80`:
2. Nếu `(block.control_flags & SPLC_MQTT_SUB_CTRL_ENABLED_BIT) != 0`:
   - Lấy chuỗi `block.topic`.
   - Lấy QoS từ `(block.control_flags & SPLC_MQTT_SUB_CTRL_QOS_MASK) >> 1`.
   - Gọi hàm SDK: `mqtt_client_subscribe(client, block.topic, qos)`.

### 4.2. Xử Lý Bản Tin Nhận Về (Inbound Message Callback)
Khi Broker đẩy bản tin về qua hàm callback MQTT:
- **Tuyệt đối không giải mã và ghi thẳng vào `g_tag_store` ngay trong hàm ngắt/callback mạng!**
- Thay vào đó, đẩy gói tin vào hàng đợi trung gian (FreeRTOS Queue hoặc Ring Buffer):
  ```c
  typedef struct {
      uint8_t  sub_block_idx;      /* 0 hoặc 1 */
      uint16_t payload_len;
      char     payload[128];       /* Giới hạn payload buffer 128 bytes */
  } SPLC_MqttInboundMsg_t;
  ```

### 4.3. Cập Nhật An Toàn Tại Đầu Chu Kỳ Quét PLC (Scan Pass Boundary)
Tại đầu mỗi chu kỳ 10ms của PLC Engine, trước khi chạy Rule Logic:
1. Kiểm tra hàng đợi `SPLC_MqttInboundMsg_t`.
2. Lấy bản tin ra và giải mã:
   - **Chế độ Raw chuỗi đơn:** Nếu `pins[i].json_key[0] == '\0'` hoặc `json_key` bắt đầu bằng `"raw"` / `"pos["`: Trích xuất số thô trực tiếp (`strtod()` / `strtol()`).
   - **Chế độ Direct Path Walk (Zero-Allocation Token Walk):**
     - SimplePLC Studio đã tự động duyệt DFS/Lenient trên App và ghi trực tiếp đường dẫn chính xác (ví dụ `values.grid` hoặc `grid`) vào cấu trúc `pins[i].json_key` qua Modbus `0x0D80`.
     - Firmware MCU sử dụng bộ phân tích token zero-allocation `jsmn` (với mảng tĩnh `jsmntok_t tokens[32]` trên stack, **tuyệt đối không cấp phát heap `malloc`**).
     - MCU tách `json_key` theo dấu `.` để duyệt trực tiếp qua các cấp token tương ứng trong $O(1)$ đến $O(k)$ bước ($k \le 4$).
     - So khớp tên khóa không phân biệt hoa thường và bỏ qua ký tự `_`/`-` nếu cần.
3. Nếu tìm thấy giá trị hợp lệ:
   - Ghi vào `g_tag_store[pin.target_tag_index]`:
     - Nếu kiểu là `REAL`: Gán `g_tag_store[tag_idx].as_real = val_real`.
     - Nếu kiểu là `INT32`: Gán `g_tag_store[tag_idx].as_int32 = val_int`.
     - Nếu kiểu là `BOOL`: Gán `g_tag_store[tag_idx].as_int32 = (val != 0) ? 1 : 0`.
   - Đặt lại Timer Watchdog của khối SUB đó: `last_received_tick[block_idx] = xTaskGetTickCount()`.

### 4.4. Máy Trạng Thái Fail-Safe Watchdog (Mất Tin / Mất Mạng)
Trong mỗi chu kỳ scan pass:
- Nếu `failsafe_timeout_ms > 0`:
  - Tính thời gian trôi qua: $\Delta t = \text{CurrentTick} - \text{last\_received\_tick}$.
  - Nếu $\Delta t > \text{failsafe\_timeout\_ms}$:
    - Kích hoạt hành động Fail-Safe theo `control_flags`:
      - **Action 1 (Reset to Zero):** Gán toàn bộ target tags của khối về `0`.
      - **Action 2 (Set to Default):** Gán target tags về giá trị `default_val_raw`.
      - **Action 0 (Keep Last):** Giữ nguyên giá trị cũ.
