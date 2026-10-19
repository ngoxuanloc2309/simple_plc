# TÀI LIỆU ĐẶC TẢ GIAO DIỆN TRUYỀN THÔNG (ICD)
## PHÂN HỆ MQTT IIOT GATEWAY & TELEMETRY CHO FIRMWARE MCU
**Mã tài liệu:** `SPLC-SPEC-MQTT-001`  
**Phiên bản:** `2.0.0`  
**Ngày ban hành:** `2026-10-10`  
**Đơn vị ban hành:** Ban Công Nghệ & Tự Động Hóa SynaptiX  
**Tiêu chuẩn liên quan:** IEC 61131-3, Modbus Application Protocol V1.1b3, OASIS MQTT V3.1.1/V5.0  

---

## 1. TỔNG QUAN HỆ THỐNG (SYSTEM OVERVIEW)

Tài liệu này đặc tả giao diện nạp cấu hình nhị phân qua giao thức **Modbus RTU (RS-485 / USB CDC)** giữa phần mềm kỹ thuật **SynaptiX IDE (SimplePLC Studio)** và vi điều khiển mục tiêu (**MCU STM32 / ESP32 / Industrial Gateway**).

Phân hệ MQTT IIoT chia làm 2 tầng cấu hình độc lập nhưng phối hợp đồng bộ:
1. **Tầng Máy Chủ Kết Nối (System Broker Endpoint - `0x0E00`)**: Thiết lập Host, Port, TLS, Client ID, Username, Password, Keep-Alive dùng chung cho thiết bị.
2. **Tầng Xuất Bản Dữ Liệu (Telemetry Blocks - `0x0C00`)**: Định nghĩa tối đa 4 khối xuất bản độc lập (Topic, Trigger Mode, COV Deadband, Danh sách Tag liên kết, Bản mẫu Payload JSON).

```
   ┌─────────────────────────────────────────────────────────────────┐
   │                    SynaptiX IDE / Studio                        │
   │  - Menu Thiết bị: Cấu hình Broker (0x0E00)                      │
   │  - Logic Editor: Khối MQTT Publish Node (0x0C00..0x0D7F)        │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ Modbus RTU FC16 Chunks
                                  ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                  MCU STM32 / ESP32 Firmware                     │
   │  ┌───────────────────────────────────────────────────────────┐  │
   │  │ 0x0E00: System Broker Config (96 regs / 192 bytes)        │  │
   │  │ (Host, Port, TLS, ClientID, User, Pass, KeepAlive)        │  │
   │  └───────────────────────────────────────────────────────────┘  │
   │  ┌───────────────────────────────────────────────────────────┐  │
   │  │ 0x0C00: 4x Telemetry Blocks (384 regs / 768 bytes)        │  │
   │  │ (Topic, COV Deadband, Bound Tags, 128B JSON Template)     │  │
   │  └───────────────────────────────────────────────────────────┘  │
   │  ┌───────────────────────────┬───────────────────────────────┐  │
   │  │ Modbus Slave Engine       │ MQTT Client Task (FreeRTOS)   │  │
   │  └───────────────────────────┴───────────────────────────────┘  │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ TCP / TLS (Port 1883 / 8883)
                                  ▼
                     [ MQTT Broker / Cloud SCADA ]
```

---

## 2. PHÂN BỔ BỘ NHỚ MODBUS HOLDING REGISTERS

### 2.1. Phân vùng Cấu hình MQTT Broker (`0x0E00..0x0E5F`)
- **Địa chỉ cơ sở:** `0x0E00` (Holding Register)
- **Độ dài:** 96 Registers = 192 Bytes
- **Cơ chế ghi:** Modbus FC16 (Write Multiple Registers)
- **Cơ chế đọc:** Modbus FC03 (Read Holding Registers)

| Địa chỉ Offset | Tên trường | Kiểu dữ liệu | Mô tả chi tiết |
| :--- | :--- | :--- | :--- |
| `+0` (`0x0E00`) | `BROKER_CONTROL` | `uint16` | Bit 0: Enable (1=Bật, 0=Tắt)<br>Bit 1: TLS/SSL (0=Plain 1883, 1=TLS 8883)<br>Bit 2: Clean Session (1=True, 0=False)<br>Bit 3..4: Protocol Version (0=v3.1.1, 1=v5.0) |
| `+1` (`0x0E01`) | `BROKER_PORT` | `uint16` | Cổng kết nối TCP (1883 hoặc 8883) |
| `+2` (`0x0E02`) | `KEEPALIVE_SEC` | `uint16` | Chu kỳ PINGREQ gửi tới Broker (mặc định 60s) |
| `+3` (`0x0E03`) | `RESERVED_0` | `uint16` | Dự phòng (Giá trị 0x0000) |
| `+4..+35` (`0x0E04`) | `BROKER_HOST` | `char[64]` | Địa chỉ máy chủ Broker (IP hoặc FQDN domain), Big-Endian ASCII, đệm 0 |
| `+36..+51` (`0x0E24`) | `CLIENT_ID` | `char[32]` | Client Identifier gửi trong gói MQTT CONNECT |
| `+52..+67` (`0x0E34`) | `USERNAME` | `char[32]` | Tên đăng nhập xác thực Broker |
| `+68..+83` (`0x0E44`) | `PASSWORD` | `char[32]` | Mật khẩu xác thực Broker |
| `+84..+95` (`0x0E54`) | `BROKER_STATUS` | `12 regs` | Vùng MCU phản hồi trạng thái Live (Read-Only):<br>+84: Status Flags (Bit 0: Connected, Bit 1: TLS, Bit 2: AuthOK)<br>+85: Latency ms (Ping RTT)<br>+86..87: Tx Msg Count (uint32)<br>+88..89: Tx Byte Count (uint32)<br>+90: Last Error Code (0=OK, 1=DNS, 2=Reset, 3=AuthFail) |

---

### 2.2. Phân vùng Các Khối Xuất Bản Telemetry (`0x0C00..0x0D7F`)
- **Địa chỉ cơ sở:** `0x0C00`
- **Số lượng khối tối đa:** 4 khối (`Block 0 .. Block 3`)
- **Dung lượng mỗi khối:** 96 Registers = 192 Bytes
- **Địa chỉ Block:** $\text{Address} = 0\text{x}0C00 + (\text{BlockIndex} \times 96)$

#### Cấu trúc mỗi Khối (96 Registers = 192 Bytes) - Khớp 100% `splc_mqtt_wire.h`:
1. **Phần Đầu Khối (Header & Điều khiển - 32 Registers = 64 Bytes):**
   - `Offset +0` (`uint16`): `CONTROL_FLAGS`:
     - `Bit 0`: Enable (`0`=Tắt, `1`=Bật)
     - `Bit 1..2`: Publish Mode (`0`: Event / On-Change, `1`: COV, `2`: Periodic)
     - `Bit 3..4`: QoS (`0`, `1`, `2`)
     - `Bit 5`: Retain Flag (`0`=False, `1`=True)
     - `Bit 6..7`: Payload Mode (`0`: Compact JSON, `1`: Raw, `2`: Custom Template)
   - `Offset +1` (`uint16`): `EN_TAG_INDEX`: Tag cho phép kích hoạt (`0xFFFF` = Luôn bật/Không dùng)
   - `Offset +2..3` (`uint32`, Big-Endian): `INTERVAL_MS`: Chu kỳ xuất bản định kỳ (ms)
   - `Offset +4..5` (`float32`, IEEE 754 Big-Endian): `DEADBAND`: Ngưỡng biến thiên COV
   - `Offset +6` (`uint16`): `TAG_COUNT`: Số lượng Tag liên kết (1..8)
   - `Offset +7..14` (`8x uint16`): `BOUND_TAGS[8]`: Danh sách 8 TagIndex (`0xFFFF` nếu không dùng)
   - `Offset +15` (`uint16`): `RESERVED_HEADER`: Vùng dự phòng (luôn ghi `0x0000` theo `SPLC-SPEC-IEC-002`).
   - `Offset +16..31` (`16x uint16` = `char[32]`): `TOPIC`: Chuỗi topic MQTT (Packed ASCII 2 ký tự/thanh ghi Big-Endian)
2. **Phần Bản Mẫu JSON (Payload Template - 64 Registers = 128 Bytes):**
   - `Offset +32..95` (`64x uint16` = `char[128]`): `PAYLOAD_TEMPLATE`: Chuỗi JSON mẫu ASCII đệm `0x00`.

---

## 3. THUẬT TOÁN XỬ LÝ TRÊN FIRMWARE MCU (EMBEDDED LOGIC)

### 3.1. Thuật toán Lọc Ngưỡng Biến Thiên (COV - Change of Value)
Khi `Publish Mode == 1 (COV)`:
MCU theo dõi giá trị của các Tag trong `BOUND_TAGS`. Tại mỗi chu kỳ quét logic PLC (10ms):
1. Với mỗi Tag $K$ ($K = 1..\text{tag\_count}$):
   - Nếu Tag kiểu `REAL` (Float32): $\Delta V_K = |V_{K, \text{curr}} - V_{K, \text{last}}| \ge \text{Deadband}$
   - Nếu Tag kiểu `DINT` (Số nguyên): $\Delta V_K = |V_{K, \text{curr}} - V_{K, \text{last}}| \ge \text{round}(\text{Deadband})$
2. Nếu $\exists K: \Delta V_K \ge \text{Threshold}$:
   Đưa thông điệp vào hàng đợi xuất bản MQTT và cập nhật $V_{K, \text{last}} = V_{K, \text{curr}}$ cho tất cả các tag.
3. *Khuyến nghị:* Chế độ COV nên áp dụng cho luồng viễn trắc đơn biến hoặc các biến cùng đơn vị vật lý.

### 3.2. Thuật toán Render Payload JSON
Khi một sự kiện kích hoạt (Event / COV / Periodic):
1. Nếu `Payload Mode == 0 (Compact JSON)`: MCU sinh JSON tự động, các giá trị được format trực tiếp theo kiểu dữ liệu của Tag (REAL -> float, DINT -> int, BOOL -> bool):
   ```json
   {"device":"PLC-01","ts":1773228945,"values":{"tag1":24.5,"tag2":10}}
   ```
2. Nếu `Payload Mode == 2 (Custom)`: MCU quét chuỗi `PAYLOAD_TEMPLATE` và thay thế các placeholder:
   - `{val1}`..`{val8}` $\rightarrow$ Giá trị tag thứ $K$ theo kiểu dữ liệu (REAL -> float, DINT -> int).
   - `{val}` $\rightarrow$ Alias cho `{val1}`.
   - `{vals}` $\rightarrow$ Mảng JSON các giá trị `[val1, val2, ...]`.
   - `{tag1}`..`{tag8}` / `{tag}` $\rightarrow$ Tên định danh tag fallback.
   - `{ts}` $\rightarrow$ Unix Timestamp từ bộ đếm thời gian thực RTC nội bộ.
   - `{device}` $\rightarrow$ Chuỗi `CLIENT_ID` đã cấu hình tại `0x0E24`.

---

## 4. MÃ NGUỒN C MẪU CHO FIRMWARE (C REFERENCE CODE)

Toàn bộ struct và hằng số đã được chuẩn hóa tại header file:
[`docs/firmware/splc_mqtt_wire.h`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/splc_mqtt_wire.h)

Ví dụ tích hợp trong Modbus Server callback của MCU STM32:

```c
#include "splc_mqtt_wire.h"

static SPLC_MqttBrokerConfig_t g_mqtt_broker_cfg;
static SPLC_MqttTelemetryBlock_t g_mqtt_telemetry_blocks[SPLC_MQTT_MAX_TELEMETRY_BLOCKS];

/* Callback xử lý ghi Modbus FC16 */
int splc_modbus_handle_fc16(uint16_t start_addr, uint16_t reg_count, const uint16_t *data)
{
    /* 1. Xử lý phân vùng Broker Config (0x0E00) */
    if (start_addr >= SPLC_MODBUS_REG_MQTT_BROKER_BASE && 
        start_addr < SPLC_MODBUS_REG_MQTT_BROKER_BASE + SPLC_MQTT_BROKER_CONFIG_REGS)
    {
        uint16_t offset = start_addr - SPLC_MODBUS_REG_MQTT_BROKER_BASE;
        uint16_t *dest = (uint16_t *)&g_mqtt_broker_cfg;
        memcpy(&dest[offset], data, reg_count * sizeof(uint16_t));
        
        /* Đánh dấu cờ cần kết nối lại Broker nếu có thay đổi */
        splc_mqtt_notify_config_changed();
        return 0; /* SUCCESS */
    }

    /* 2. Xử lý phân vùng Telemetry Blocks (0x0C00) */
    if (start_addr >= SPLC_MODBUS_REG_MQTT_TELEMETRY_BASE &&
        start_addr < SPLC_MODBUS_REG_MQTT_TELEMETRY_BASE + (SPLC_MQTT_MAX_TELEMETRY_BLOCKS * SPLC_MQTT_REGS_PER_BLOCK))
    {
        uint16_t offset = start_addr - SPLC_MODBUS_REG_MQTT_TELEMETRY_BASE;
        uint16_t *dest = (uint16_t *)g_mqtt_telemetry_blocks;
        memcpy(&dest[offset], data, reg_count * sizeof(uint16_t));
        return 0; /* SUCCESS */
    }

    return -1; /* Out of range */
}
```

---

## 5. KẾT QUẢ NGHIỆM THU KIỂM THỬ TỰ ĐỘNG PHẦN CỨNG

Hệ thống đã trải qua kiểm thử độc lập Clean Code với **10/10 test cases vượt qua 100%** trên phần cứng ảo hai cổng UART thực tế `COM7` <-> `COM9`:
- `TC01`: Ký tự thứ 128 (Off-by-one check) $\rightarrow$ **PASS**
- `TC02`: Ký tự thứ 32 của Topic $\rightarrow$ **PASS**
- `TC03`: Cắt an toàn khi người dùng cố tình nhập chuỗi quá dài $\rightarrow$ **PASS**
- `TC04`: Đồng thời 4 khối xuất bản (384 holding registers) $\rightarrow$ **PASS**
- `TC05`: Vô hiệu hóa an toàn khối cũ khi người dùng xóa trên App $\rightarrow$ **PASS**
- `TC06`: Ký tự đặc biệt và dấu ngoặc JSON lồng $\rightarrow$ **PASS**
- `TC07`: Chặn truy cập vùng nhớ bất hợp pháp ngoài `0x0D80` $\rightarrow$ **PASS**
- `TC08`: Stress test nạp liên tục 5 lần tốc độ cao $\rightarrow$ **PASS**
- `TC09`: Nạp và đọc lại phân vùng Broker `0x0E00` (96 registers) $\rightarrow$ **PASS**
- `TC10`: Nạp đồng thời Rule + Timer TON + Telemetry + Broker trong 1 giao dịch $\rightarrow$ **PASS**
