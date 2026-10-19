# TÀI LIỆU ĐẶC TẢ GIAO DIỆN TRUYỀN THÔNG (ICD)
## PHÂN HỆ MODBUS RTU MASTER WRITE / ACTUATOR CONTROLLER (MB_WRITE) CHO FIRMWARE MCU
**Mã tài liệu:** `SPLC-SPEC-MBM-002`  
**Phiên bản:** `2.0.0`  
**Ngày ban hành:** `2026-10-16`  
**Đơn vị ban hành:** Ban Công Nghệ & Tự Động Hóa SynaptiX  
**Tiêu chuẩn liên quan:** IEC 61131-3, Modbus Application Protocol Specification V1.1b3, Modbus over Serial Line Specification and Implementation Guide V1.02  

---

## 1. TỔNG QUAN HỆ THỐNG (SYSTEM OVERVIEW)

Tài liệu này đặc tả giao diện nạp cấu hình nhị phân và kiến trúc điều phối truyền thông **Modbus Master Actuator Controller (`MB_WRITE`)** chạy trên vi điều khiển mục tiêu (**MCU STM32 / ESP32 / Industrial Gateway**) trong hệ sinh thái **SimplePLC / SynaptiX IDE**.

Khối chức năng **`MB_WRITE`** cho phép vi điều khiển chủ động đóng vai trò **Modbus RTU Master** trên cổng RS-485 (hoặc UART/USB CDC), gửi lệnh điều khiển tới các cơ cấu chấp hành công nghiệp (Biến tần VFD, Đèn chỉ thị thông minh Sort-to-Light, Module Relay mở rộng, Van tuyến tính Modbus) theo hai phương thức:
1. **Direct Registers (FC05/06/15/16):** Ghi trực tiếp giá trị từ các Tag/Chân vào một dải thanh ghi/cuộn dây cố định.
2. **Channel-Indexed Target:** Điều khiển động cơ cấu chấp hành nhiều kênh (ví dụ 18 ô Sort-to-Light) qua địa chỉ cơ sở `Base Command` và `Base Param`, hỗ trợ cờ độc quyền `ExclusiveChannel` tự động triệt tiêu các kênh còn lại khi một kênh mới được kích hoạt.

```
   ┌─────────────────────────────────────────────────────────────────┐
   │                    SynaptiX IDE / Studio                        │
   │  - Logic Editor: Khối [MB_WRITE] Node (0x1100..0x11FF)          │
   │  - Hộp thoại Cấu hình: Write Mode, Base Addresses, Channel Pins │
   └──────────────────────────────┬──────────────────────────────────┘
                                  │ Modbus RTU FC16 Staging Chunks
                                  ▼
   ┌─────────────────────────────────────────────────────────────────┐
   │                  MCU STM32 / ESP32 Firmware                     │
   │  ┌───────────────────────────────────────────────────────────┐  │
   │  │ 0x1100: 4x Modbus Write Blocks (256 regs / 512 bytes)     │  │
   │  │ (Slave ID, FC, Mode, Base Addr, Pins, Interval, Timeouts) │  │
   │  └───────────────────────────────────────────────────────────┘  │
   │                                                                 │
   │  ┌───────────────────────────┬───────────────────────────────┐  │
   │  │ Modbus RTU Write Task     │ Bộ Đệm Khung Tin UART RS-485  │  │
   │  │ (State Machine FreeRTOS)  │ (TX Request -> RX Response)   │  │
   │  └─────────────┬─────────────┴───────────────┬───────────────┘  │
   │                │ RS-485 Half-Duplex (DE/RE)  │                  │
   │                ▼                             ▼                  │
   │   [ Modbus Actuators / VFD / Sort-To-Light LED Modules ]        │
   │                                              │                  │
   │  ┌───────────────────────────────────────────┼───────────────┐  │
   │  │ 10ms PLC Scan Pass Loop                   ▼               │  │
   │  │ 1. Phát hiện sự kiện Trigger / Data Change ──► Task Queue │  │
   │  │ 2. Cập nhật cờ Done / Err / ErrCode                       │  │
   │  │ 3. Đánh giá chuỗi liên động & Logic FBD                   │  │
   │  │ 4. Xuất Physical Outputs (DO / AO)                        │  │
   │  └───────────────────────────────────────────────────────────┘  │
   └─────────────────────────────────────────────────────────────────┘
```

---

## 2. PHÂN BỔ BỘ NHỚ MODBUS HOLDING REGISTERS (`0x1100..0x11FF`)

### 2.1. Địa chỉ và Phân vùng
- **Dải địa chỉ Modbus:** `0x1100 - 0x11FF` (256 Holding Registers = 512 Bytes)
- **Số lượng khối cấu hình:** 4 Blocks (Block 0: `0x1100`, Block 1: `0x1140`, Block 2: `0x1180`, Block 3: `0x11C0`)
- **Độ dài mỗi khối:** 64 Holding Registers = 128 Bytes
- **Cơ chế nạp an toàn:** Modbus FC16 qua vùng nhớ Staging `0x9010` $\rightarrow$ Kiểm tra CRC-16 $\rightarrow$ Kích hoạt nguyên tử `0xA000` (`0xA5A5`).
- **Cơ chế đọc cấu hình:** Modbus FC03 (Read Holding Registers).

```
0x1100 ┌───────────────────────────────────────────────────────────┐
       │ Block 0: Modbus Master Write 0 (64 regs / 128 bytes)      │
0x1140 ├───────────────────────────────────────────────────────────┤
       │ Block 1: Modbus Master Write 1 (64 regs / 128 bytes)      │
0x1180 ├───────────────────────────────────────────────────────────┤
       │ Block 2: Modbus Master Write 2 (64 regs / 128 bytes)      │
0x11C0 ├───────────────────────────────────────────────────────────┤
       │ Block 3: Modbus Master Write 3 (64 regs / 128 bytes)      │
0x11FF └───────────────────────────────────────────────────────────┘
```

---

## 3. CẤU TRÚC CHI TIẾT MỘT KHỐI MODBUS WRITE (64 REGISTERS = 128 BYTES)

Mỗi khối 64 thanh ghi được phân tách rõ ràng:
- **Header cấu hình điều khiển (Offset `+0..+15`):** 16 Registers (32 Bytes).
- **Danh sách chân ghi động Pin Binding (Offset `+16..+63`):** 4 Pins $\times$ 12 Registers = 48 Registers (96 Bytes).

### 3.1. Header Cấu hình Khối (`+0..+15`)

| Offset | Tên trường | Kiểu dữ liệu | Phạm vi giá trị | Mô tả kỹ thuật |
| :--- | :--- | :--- | :--- | :--- |
| `+0` | `CONTROL_FLAGS` | `uint16` | Bitmask | **Bit 0:** Block Enabled (1 = Bật, 0 = Tắt)<br>**Bit 1:** Exclusive Channel (1 = Tự động xóa kênh khác khi bật kênh mới)<br>**Bit 2..3:** Trigger Mode:<br>`0` = OnTriggerRisingEdge (Xung cạnh lên)<br>`1` = OnDataChange (Ghi khi dữ liệu thay đổi)<br>`2` = Cyclic (Ghi tuần hoàn)<br>**Bit 4..15:** Dự phòng |
| `+1` | `FC_AND_SLAVE_ID` | `uint16` | Pack Byte | **High Byte:** Function Code (`0x05`, `0x06`, `0x0F`, `0x10`)<br>**Low Byte:** Slave ID (`1..247`) |
| `+2` | `WRITE_MODE` | `uint16` | Enum | `0` = DirectRegisters (Ghi theo thanh ghi cố định)<br>`1` = ChannelIndexed (Ghi theo chỉ số kênh Sort-to-Light) |
| `+3` | `START_OR_BASE_CMD`| `uint16` | `0..65535` | Direct: Starting Address.<br>Channel-Indexed: Base Command Address (VD: `0x0100`). |
| `+4` | `BASE_PARAM_ADDR` | `uint16` | `0..65535` | Channel-Indexed: Base Parameter Address (VD: `0x0120` cho màu RGB). Direct: Không sử dụng (`0x0000`). |
| `+5` | `POINTS_OR_CHANNELS`| `uint16` | `1..256` | Direct: Quantity of Coils/Registers.<br>Channel-Indexed: Total Channel Count (VD: `18`). |
| `+6..+7`| `INTERVAL_MS` | `uint32` | `50..60000` | Chu kỳ gửi lệnh lặp lại nếu ở chế độ Cyclic (ms), Big-Endian Word |
| `+8..+9`| `TIMEOUT_MS` | `uint32` | `50..10000` | Thời gian chờ phản hồi tối đa trước khi báo lỗi Timeout (ms) |
| `+10` | `DONE_TAG_INDEX` | `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag nhận xung TRUE khi ghi thành công (`0xFFFF` = Unmapped) |
| `+11` | `ERR_TAG_INDEX` | `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag nhận cờ báo lỗi (`0xFFFF` = Unmapped) |
| `+12` | `ERR_CODE_TAG_INDEX`| `uint16` | `0..N` hoặc `0xFFFF` | Chỉ số Tag lưu mã lỗi chẩn đoán (`0xFFFF` = Unmapped) |
| `+13` | `PIN_COUNT` | `uint16` | `0..4` | Số lượng chân dữ liệu được ánh xạ |
| `+14..+15`| `RESERVED` | `uint16[2]` | `0x0000` | Vùng đệm mở rộng tương lai |

---

### 3.2. Cấu trúc Mỗi Chân Dữ Liệu Ghi Pin Binding (12 Registers = 24 Bytes)

Mỗi chân $i \in [0..3]$ bắt đầu tại offset $16 + (i \times 12)$:

| Offset trong Pin | Tên trường | Kiểu dữ liệu | Mô tả kỹ thuật |
| :--- | :--- | :--- | :--- |
| `+0` | `SOURCE_TAG_INDEX` | `uint16` | Chỉ số Tag trong `Tag Catalog` chứa giá trị cần ghi (`0xFFFF` = Không gán) |
| `+1` | `REG_OFFSET` | `uint16` | Vị trí thanh ghi tương đối tính từ địa chỉ bắt đầu ($0 \le \text{Offset} < \text{Quantity}$) |
| `+2` | `DATA_TYPE` | `uint16` | `0` = Bool, `1` = Int16, `2` = UInt16, `3` = Float32 (2 regs), `4` = Int32 (2 regs) |
| `+3` | `ENDIANNESS` | `uint16` | `0` = Big-Endian (ABCD), `1` = Word-Swap (CDAB), `2` = Byte-Swap (BADC), `3` = Little-Endian (DCBA) |
| `+4..+5` | `MULTIPLIER` | `float32` | Hệ số chia/nhân nghịch đảo trước khi gửi (IEEE 754 Big-Endian) |
| `+6..+11` | `PIN_LABEL` | `char[12]` | Tên nhãn kỹ thuật của chân (Tối đa 12 ký tự ASCII) |

---

## 4. QUY TRÌNH THỰC THI CỦA FIRMWARE MCU

### 4.1. Máy Trạng Thái Tác Vụ Ghi (Modbus Master Write State Machine)

MCU quản lý việc phát lệnh ghi trong tác vụ FreeRTOS chuyên biệt (`mb_master_write_task`) để không gây block vòng quét logic 10ms:

```
        ┌────────────────────────────────────────────────────────┐
        │                      STATE: IDLE                       │
        │   Lắng nghe hàng đợi sự kiện (Event Queue):           │
        │   - Cạnh lên Trig từ FBD canvas                        │
        │   - Giá trị Source Tag thay đổi (OnDataChange)         │
        │   - Hết hạn chu kỳ CyclicTimer                         │
        └───────────────────────────┬────────────────────────────┘
                                    │ Có sự kiện ghi
                                    ▼
        ┌────────────────────────────────────────────────────────┐
        │                  STATE: ASSEMBLE_PDU                   │
        │   1. Đọc giá trị tức thời từ Tag Store                 │
        │   2. Chuyển đổi Endianness & Áp dụng Multiplier        │
        │   3. Tạo khung Modbus PDU (FC05/06/15/16)              │
        │   4. Kèm theo lệnh Clear cho các kênh khác nếu        │
        │      cờ ExclusiveChannel = True                        │
        └───────────────────────────┬────────────────────────────┘
                                    │ Bật RS-485 Driver Enable (DE)
                                    ▼
        ┌────────────────────────────────────────────────────────┐
        │                    STATE: TRANSMIT                     │
        │   Gửi chuỗi byte qua UART DMA/IT. Khi hoàn tất TX:     │
        │   Chuyển DE về Receive, nạp bộ đếm Timeout Watchdog    │
        └───────────────────────────┬────────────────────────────┘
                                    │
                    ┌───────────────┴───────────────┐
                    │ Hết Timeout                   │ Nhận đủ Bytes Echo
                    ▼                               ▼
        ┌───────────────────────┐       ┌────────────────────────┐
        │  STATE: HANDLE_ERROR  │       │  STATE: PROCESS_RESP   │
        │  - Cập nhật Err = 1   │       │  - Kiểm tra CRC-16     │
        │  - ErrCode = TIMEOUT  │       │  - Kiểm tra Exception  │
        │  - Báo động hệ thống  │       │  - Cập nhật Done = 1   │
        └───────────┬───────────┘       └───────────┬────────────┘
                    │                               │
                    └───────────────┬───────────────┘
                                    ▼
                         Quay lại STATE: IDLE
```

### 4.2. Bảng Mã Lỗi Chẩn Đoán (Diagnostic Error Codes)

| Mã Lỗi (`ERR_CODE`) | Tên Định Danh | Nguyên Nhân & Hướng Xử Lý Khắc Phục |
| :---: | :--- | :--- |
| `0` | `ERR_NONE` | Giao dịch truyền thông Modbus Write thành công hoàn toàn. |
| `1` | `ERR_TIMEOUT` | Thiết bị Slave không phản hồi trong ngưỡng `TimeoutMs` (Đứt cáp RS-485, sai Slave ID hoặc baud rate). |
| `2` | `ERR_CRC` | Khung tin phản hồi bị sai lệch mã kiểm tra CRC-16 (Nhiễu điện từ trường môi trường nhà xưởng). |
| `3` | `ERR_EXCEPTION` | Thiết bị Slave trả về mã ngoại lệ Modbus Exception (0x01: Illegal FC, 0x02: Illegal Data Address, 0x03: Illegal Data Value). |
| `4` | `ERR_BUS_BUSY` | Xung đột đường truyền RS-485 khi có nhiều trạm Master hoặc nhiệm vụ khác đang chiếm giữ UART. |
| `5` | `ERR_CHANNEL_OUT_OF_BOUNDS` | Kênh điều khiển chỉ định vượt quá giới hạn cấu hình `ChannelCount`. |

---

## 5. ĐỊNH NGHĨA CẤU TRÚC DỮ LIỆU C/C++ MẪU CHO FIRMWARE

```c
#ifndef SPLC_MODBUS_WRITE_WIRE_H
#define SPLC_MODBUS_WRITE_WIRE_H

#include <stdint.h>
#include <stdbool.h>

#define SPLC_MB_WRITE_TABLE_BASE   0x1100
#define SPLC_MB_WRITE_BLOCK_COUNT  4
#define SPLC_MB_WRITE_BLOCK_REGS   64
#define SPLC_MB_WRITE_MAX_PINS     4

// Chế độ ghi
typedef enum {
    MB_WRITE_MODE_DIRECT_REGISTERS = 0,
    MB_WRITE_MODE_CHANNEL_INDEXED  = 1
} splc_mb_write_mode_t;

// Chế độ kích hoạt
typedef enum {
    MB_WRITE_TRIG_RISING_EDGE = 0,
    MB_WRITE_TRIG_ON_CHANGE   = 1,
    MB_WRITE_TRIG_CYCLIC      = 2
} splc_mb_write_trigger_t;

// Cấu trúc chân dữ liệu
typedef struct __attribute__((packed)) {
    uint16_t source_tag_index;
    uint16_t reg_offset;
    uint16_t data_type;
    uint16_t endianness;
    float    multiplier;
    char     pin_label[12];
} splc_mb_write_pin_t;

// Cấu trúc khối 64 Holding Registers
typedef struct __attribute__((packed)) {
    // Header (16 regs = 32 bytes)
    uint16_t control_flags;
    uint8_t  slave_id;
    uint8_t  function_code;
    uint16_t write_mode;
    uint16_t start_or_base_cmd;
    uint16_t base_param_addr;
    uint16_t points_or_channels;
    uint32_t interval_ms;
    uint32_t timeout_ms;
    uint16_t done_tag_index;
    uint16_t err_tag_index;
    uint16_t err_code_tag_index;
    uint16_t pin_count;
    uint16_t reserved[2];

    // Dynamic Pins (4 pins x 12 regs = 48 regs = 96 bytes)
    splc_mb_write_pin_t pins[SPLC_MB_WRITE_MAX_PINS];
} splc_mb_write_block_t;

#endif // SPLC_MODBUS_WRITE_WIRE_H
```
