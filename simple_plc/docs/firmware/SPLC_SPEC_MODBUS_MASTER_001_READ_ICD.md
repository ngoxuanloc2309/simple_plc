# TÀI LIỆU ĐẶC TẢ GIAO DIỆN TRUYỀN THÔNG (ICD)
## PHÂN HỆ MODBUS RTU MASTER / SENSOR POLLER (MB_READ) CHO FIRMWARE MCU
**Mã tài liệu:** `SPLC-SPEC-MBM-001`  
**Phiên bản:** `2.0.0`  
**Ngày ban hành:** `2026-10-16`  
**Đơn vị ban hành:** Ban Công Nghệ & Tự Động Hóa SynaptiX  
**Tiêu chuẩn liên quan:** IEC 61131-3, Modbus Application Protocol Specification V1.1b3, Modbus over Serial Line Specification and Implementation Guide V1.02  

---

## 1. TỔNG QUAN HỆ THỐNG (SYSTEM OVERVIEW)

Tài liệu này đặc tả giao diện nạp cấu hình nhị phân và kiến trúc điều phối truyền thông **Modbus Master Poller (`MB_READ`)** chạy trên vi điều khiển mục tiêu (**MCU STM32 / ESP32 / Industrial Gateway**) trong hệ sinh thái **SimplePLC / SynaptiX IDE**.

Khối chức năng **`MB_READ`** cho phép vi điều khiển chủ động đóng vai trò **Modbus RTU Master** trên cổng RS-485 (hoặc UART/USB CDC), tự động gửi yêu cầu định kỳ (Cyclic Polling) tới các thiết bị ngoại vi (Slave Sensors, Biến tần VFD, Đồng hồ đo điện năng Multifunction Meter, Bộ điều khiển nhiệt độ PID) và giải mã payload nhị phân trả về thành các giá trị số thực tế (Floating-point / Integer) để đưa vào sơ đồ điều khiển FBD.

```
   ┌─────────────────────────────────────────────────────────────────┐
   │                    SynaptiX IDE / Studio                        │
   │  - Logic Editor: Khối [MB_READ] Node (0x1000..0x10FF)           │
   │  - Hộp thoại Cấu hình: Starting Address, Quantity, Dynamic Pins │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ Modbus RTU FC16 Staging Chunks
                                  ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                  MCU STM32 / ESP32 Firmware                     │
   │  ┌───────────────────────────────────────────────────────────┐  │
   │  │ 0x1000: 4x Modbus Master Blocks (256 regs / 512 bytes)    │  │
   │  │ (Slave ID, FC, Starting Address, Quantity, Interval, Pins)│  │
   │  └───────────────────────────────────────────────────────────┘  │
   │                                                                 │
   │  ┌───────────────────────────┬───────────────────────────────┐  │
   │  │ Modbus RTU Master Task    │ Bộ Đệm Khung Tin UART RS-485  │  │
   │  │ (State Machine FreeRTOS)  │ (TX Request -> RX Response)   │  │
   │  └─────────────┬─────────────┴───────────────┬───────────────┘  │
   │                │ RS-485 Half-Duplex (DE/RE)  │                  │
   │                ▼                             ▼                  │
   │   [ Modbus Slave Sensors / VFD ]      [ Bộ Giải Mã Data Parser ]│
   │   (Nhiệt độ, Độ ẩm, Dòng áp)                 │                  │
   │                                              │                  │
   │  ┌───────────────────────────────────────────┼───────────────┐  │
   │  │ 10ms PLC Scan Pass Loop                   ▼               │  │
   │  │ 1. Cập nhật Pin Values (Atomic Copy) ──► g_tag_store      │  │
   │  │ 2. Cập nhật cờ Valid / Err / ErrCode                      │  │
   │  │ 3. Đánh giá chuỗi liên động & Logic FBD                   │  │
   │  │ 4. Xuất Physical Outputs (DO / AO)                        │  │
   │  └───────────────────────────────────────────────────────────┘  │
   └─────────────────────────────────────────────────────────────────┘
```

---

## 2. PHÂN BỔ BỘ NHỚ MODBUS HOLDING REGISTERS (`0x1000..0x10FF`)

### 2.1. Địa chỉ và Phân vùng
- **Dải địa chỉ Modbus:** `0x1000 - 0x10FF` (256 Holding Registers = 512 Bytes)
- **Số lượng khối cấu hình:** 4 Blocks (Block 0: `0x1000`, Block 1: `0x1040`, Block 2: `0x1080`, Block 3: `0x10C0`)
- **Độ dài mỗi khối:** 64 Holding Registers = 128 Bytes
- **Cơ chế ghi:** Modbus FC16 (Write Multiple Registers) qua quy trình Staging `0x9010` -> Atomic Commit `0xA000`
- **Cơ chế đọc:** Modbus FC03 (Read Holding Registers)

```
0x1000 ┌───────────────────────────────────────────────────────────┐
       │ Block 0: Modbus Master Channel 0 (64 regs / 128 bytes)    │
0x1040 ├───────────────────────────────────────────────────────────┤
       │ Block 1: Modbus Master Channel 1 (64 regs / 128 bytes)    │
0x1080 ├───────────────────────────────────────────────────────────┤
       │ Block 2: Modbus Master Channel 2 (64 regs / 128 bytes)    │
0x10C0 ├───────────────────────────────────────────────────────────┤
       │ Block 3: Modbus Master Channel 3 (64 regs / 128 bytes)    │
0x10FF └───────────────────────────────────────────────────────────┘
```

---

## 3. CẤU TRÚC CHI TIẾT MỘT KHỐI MODBUS MASTER (64 REGISTERS = 128 BYTES)

Mỗi khối 64 thanh ghi được chia thành 2 phần:
- **Header cấu hình chung (Offset `+0..+15`):** 16 Registers (32 Bytes).
- **Danh sách chân ngõ ra động (Offset `+16..+63`):** 4 Pins x 12 Registers = 48 Registers (96 Bytes).

### 3.1. Header Cấu hình Khối (`+0..+15`)

| Offset | Tên trường | Kiểu dữ liệu | Phạm vi giá trị | Mô tả kỹ thuật |
| :--- | :--- | :--- | :--- | :--- |
| `+0` | `CONTROL_FLAGS` | `uint16` | Bitmask | Bit 0: Block Enabled (1 = Bật, 0 = Tắt)<br>Bit 1..15: Dự phòng (Reserved = 0) |
| `+1` | `FC_AND_SLAVE_ID` | `uint16` | Pack Byte | **High Byte:** Function Code (`0x01`, `0x02`, `0x03`, `0x04`)<br>**Low Byte:** Slave ID (`1..247`) |
| `+2` | `START_ADDRESS` | `uint16` | `0..65535` | Địa chỉ thanh ghi/coil bắt đầu trên Slave (0-based hoặc 1-based theo thiết bị) |
| `+3` | `REG_COUNT` | `uint16` | `1..125` | **Quantity of Registers / Coils:** Số lượng phần tử cần đọc theo tiêu chuẩn Modbus |
| `+4..+5`| `POLL_INTERVAL_MS`| `uint32` | `50..60000` | Chu kỳ gửi yêu cầu đọc (ms), mã hóa Big-Endian Word |
| `+6..+7`| `TIMEOUT_MS` | `uint32` | `50..10000` | Thời gian chờ phản hồi tối đa trước khi báo lỗi Timeout (ms) |
| `+8` | `VALID_TAG_INDEX` | `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag nhận xung TRUE khi frame trả về hợp lệ (`0xFFFF` = Không nối) |
| `+9` | `ERROR_TAG_INDEX` | `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag nhận cờ báo lỗi (`0xFFFF` = Không nối) |
| `+10` | `ERR_CODE_TAG_INDEX`| `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag lưu mã lỗi chẩn đoán (1=Timeout, 2=CRC, 3=Exception, 4=Malformed) |
| `+11` | `PIN_COUNT` | `uint16` | `0..4` | Số lượng chân động được cấu hình sử dụng trong khối |
| `+12..+15`| `RESERVED` | `uint16[4]` | `0x0000` | Vùng đệm dự phòng tương lai |

---

### 3.2. Cấu trúc Mỗi Chân Ngõ Ra Động Pin Binding (12 Registers = 24 Bytes)

Mỗi chân tại vị trí $i \in [0..3]$ bắt đầu tại offset $16 + (i \times 12)$:

| Offset trong Pin | Tên trường | Kiểu dữ liệu | Mô tả kỹ thuật |
| :--- | :--- | :--- | :--- |
| `+0` | `TARGET_TAG_INDEX` | `uint16` | Chỉ số Tag trong `Tag Catalog` được ghi giá trị tự động (`0xFFFF` = Unmapped) |
| `+1` | `REG_OFFSET` | `uint16` | Vị trí thanh ghi tương đối tính từ `START_ADDRESS` ($0 \le \text{Offset} < \text{Quantity}$) |
| `+2` | `DATA_TYPE` | `uint16` | Mã kiểu dữ liệu giải mã:<br>`0` = Bool<br>`1` = Int16 (Signed 16-bit)<br>`2` = UInt16 (Unsigned 16-bit)<br>`3` = Real (Float32 IEEE 754, 2 regs)<br>`4` = Int32 (Signed 32-bit, 2 regs)<br>`5` = UInt32 (Unsigned 32-bit, 2 regs) |
| `+3` | `ENDIANNESS` | `uint16` | Thứ tự sắp xếp byte & word:<br>`0` = Big-Endian (ABCD)<br>`1` = Word-Swap (CDAB - Phổ biến trong PLC/Modbus)<br>`2` = Byte-Swap (BADC)<br>`3` = Little-Endian (DCBA) |
| `+4..+5` | `MULTIPLIER` | `float32` | Hệ số tỷ lệ nhân tuyến tính (IEEE 754 Big-Endian Word). Ví dụ: `0.1` để đổi $250 \rightarrow 25.0^\circ\text{C}$ |
| `+6..+11` | `PIN_LABEL` | `char[12]` | Tên nhãn kỹ thuật của chân (Tối đa 12 ký tự ASCII, đóng gói 2 ký tự/thanh ghi) |

---

## 4. QUY TRÌNH THỰC THI TRUYỀN THÔNG CỦA FIRMWARE MCU

### 4.1. Máy trạng thái (Master Polling State Machine)
MCU chạy một tác vụ RTOS độc lập (`mb_master_task`) để không gây nghẽn chu kỳ quét 10ms của Logic Engine:

```
        ┌────────────────────────────────────────────────┐
        │                 STATE: IDLE                    │
        │   Đếm thời gian chờ đến chu kỳ PollInterval    │
        └───────────────────────┬────────────────────────┘
                                │ Đến hạn PollInterval
                                ▼
        ┌────────────────────────────────────────────────┐
        │                 STATE: SEND_REQ                │
        │   Bật chân RS-485 DE/RE -> Gửi Frame Modbus    │
        │   (SlaveID + FC + StartAddr + Qty + CRC16)     │
        └───────────────────────┬────────────────────────┘
                                │ Chuyển DE/RE sang Receive
                                ▼
        ┌────────────────────────────────────────────────┐
        │                 STATE: WAIT_RESP               │
        │   Lắng nghe UART RX với Timeout Watchdog       │
        └───────────────┬────────────────┬───────────────┘
                        │ Hết Timeout    │ Nhận đủ Bytes
                        ▼                ▼
        ┌────────────────────────┐  ┌───────────────────────────────────┐
        │   STATE: ON_TIMEOUT    │  │       STATE: PARSE_RESPONSE       │
        │  Err = 1; ErrCode = 1  │  │  1. Kiểm tra SlaveID & FC         │
        │  Valid = 0             │  │  2. Tính toán & đối chiếu CRC-16  │
        └────────────────────────┘  │  3. Kiểm tra Exception Code       │
                                    └─────────────┬─────────────────────┘
                                                  │
                                ┌─────────────────┴─────────────────┐
                                │ CRC Error / Exception             │ CRC OK & Payload Hợp lệ
                                ▼                                   ▼
                   ┌────────────────────────┐          ┌───────────────────────────────────┐
                   │    STATE: ON_ERROR     │          │         STATE: ON_SUCCESS         │
                   │ Err = 1                │          │ Valid = 1 (Xung 1 chu kỳ); Err = 0│
                   │ ErrCode = 2 (CRC)      │          │ Giải mã Pin values theo Endianness│
                   │ ErrCode = 3 (Exception)│          │ Áp dụng Multiplier                │
                   └────────────────────────┘          │ Cập nhật nguyên tử vào g_tag_store│
                                                       └───────────────────────────────────┘
```

### 4.2. Bảng Mã Lỗi Chẩn Đoán (`Err_Code`)

| Mã lỗi (`Err_Code`) | Định danh | Nguyên nhân |
| :---: | :--- | :--- |
| `0` | `ERR_NONE` | Giao tiếp bình thường, không có lỗi. |
| `1` | `ERR_TIMEOUT` | Thiết bị Slave không phản hồi trong khoảng `TimeoutMs` (đứt cáp, sai Baudrate, sai Slave ID). |
| `2` | `ERR_CRC` | Khung tin nhận được có mã kiểm tra CRC-16 không khớp (nhiễu tín hiệu đường truyền RS-485). |
| `3` | `ERR_EXCEPTION` | Thiết bị Slave phản hồi khung Exception (Function Code `0x80 + FC`, ví dụ `0x02` Illegal Data Address). |
| `4` | `ERR_MALFORMED` | Số lượng byte dữ liệu trong phản hồi không khớp với `Quantity of Registers` đã yêu cầu. |

---

## 5. CÔNG THỨC GIẢI MÃ DỮ LIỆU & BẢNG THỨ TỰ BYTE (ENDIANNESS)

Khi nhận được 2 thanh ghi $R_0, R_1$ tương ứng với 4 bytes $B_0, B_1, B_2, B_3$ từ đường truyền:
- $R_0 = (B_0 \ll 8) \mid B_1$
- $R_1 = (B_2 \ll 8) \mid B_3$

Giá trị 32-bit (cho kiểu `Real`, `Int32`, `UInt32`) được tái tạo theo công thức:

| Thứ tự Byte (Endianness) | Thứ tự Ghép Byte | Biểu thức Bitwise tương đương |
| :--- | :---: | :--- |
| **Big-Endian (ABCD)** | $B_0, B_1, B_2, B_3$ | $(B_0 \ll 24) \mid (B_1 \ll 16) \mid (B_2 \ll 8) \mid B_3$ |
| **Word-Swap (CDAB)** | $B_2, B_3, B_0, B_1$ | $(B_2 \ll 24) \mid (B_3 \ll 16) \mid (B_0 \ll 8) \mid B_1$ |
| **Byte-Swap (BADC)** | $B_1, B_0, B_3, B_2$ | $(B_1 \ll 24) \mid (B_0 \ll 16) \mid (B_3 \ll 8) \mid B_2$ |
| **Little-Endian (DCBA)** | $B_3, B_2, B_1, B_0$ | $(B_3 \ll 24) \mid (B_2 \ll 16) \mid (B_1 \ll 8) \mid B_0$ |

Sau khi trích xuất giá trị số học thô $V_{\text{raw}}$, giá trị cuối cùng được tính toán:
$$V_{\text{final}} = V_{\text{raw}} \times \text{Multiplier}$$
và được ghi vào Tag tương ứng để xuất ra đồ thị logic FBD.
