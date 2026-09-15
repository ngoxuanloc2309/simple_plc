TÀI LIỆU KỸ THUẬT: SIMPLEPLC APP-MCU DATA STRUCTS & MODBUS REGISTER MAP

Data Contract V1 | Modbus Register Map V1 | Phiên bản 1.7

# 0\. QUY ƯỚC DÙNG CHUNG

• Các field truyền qua Modbus dùng kiểu có kích thước cố định: uint8_t / uint16_t / uint32_t / int32_t.

• Enum chỉ định giá trị symbolic. Trong wire struct vẫn dùng integer cố định để tránh phụ thuộc kích thước enum của compiler C.

• Field dạng bitmask có thể đồng thời chứa nhiều cờ. Giá trị 0 nghĩa là không có cờ nào được set.

• Tên field dùng snake_case; type dùng tiền tố SPLC_.

# 1\. DEVICE DESCRIPTOR

**Vai trò:** Nhận dạng loại thiết bị và phiên bản mà App đang kết nối. Đây là dữ liệu chỉ đọc từ MCU; không phải cấu hình người dùng.

/\* Device class: family cấp cao của sản phẩm. \*/  
typedef enum {  
SPLC_DEVICE_CLASS_UNKNOWN = 0,  
SPLC_DEVICE_CLASS_REMOTE_IO = 1,  
SPLC_DEVICE_CLASS_DATALOGGER = 2,  
SPLC_DEVICE_CLASS_GATEWAY = 3,  
SPLC_DEVICE_CLASS_CONTROLLER = 4  
} SPLC_DeviceClass;  
<br/>/\* Remote I/O variant: chỉ dùng khi device_class = REMOTE_IO. \*/  
typedef enum {  
SPLC_REMOTE_IO_VARIANT_UNKNOWN = 0,  
SPLC_REMOTE_IO_VARIANT_8DI_8DO_4AI = 1,  
SPLC_REMOTE_IO_VARIANT_16DI_16DO = 2  
} SPLC_RemoteIoVariant;  
<br/>/\* Datalogger variant: chỉ dùng khi device_class = DATALOGGER. \*/  
typedef enum {  
SPLC_DATALOGGER_VARIANT_UNKNOWN = 0,  
SPLC_DATALOGGER_VARIANT_8AI = 1  
} SPLC_DataloggerVariant;  
<br/>/\* Gateway variant: chỉ dùng khi device_class = GATEWAY. \*/  
typedef enum {  
SPLC_GATEWAY_VARIANT_UNKNOWN = 0,  
SPLC_GATEWAY_VARIANT_RS485_ETH = 1  
} SPLC_GatewayVariant;  
<br/>/\*  
\* Descriptor App đọc ngay sau khi kết nối.  
\* HW/FW hiển thị theo major.minor.patch, ví dụ 1.1.0 / 1.3.2.  
\*/  
typedef struct {  
uint16_t device_class; // SPLC_DeviceClass  
uint16_t device_variant; // Variant enum tương ứng device_class  
<br/>uint16_t hw_version_major; // HW major  
uint16_t hw_version_minor; // HW minor  
uint16_t hw_version_patch; // HW patch  
<br/>uint16_t fw_version_major; // FW major  
uint16_t fw_version_minor; // FW minor  
uint16_t fw_version_patch; // FW patch  
<br/>uint16_t protocol_version; // Version contract App &lt;-&gt; MCU  
uint16_t rule_format_version; // Version layout/semantic RuleRecord  
} SPLC_DeviceDescriptor; // 20 byte

# 2\. RULE TABLE

| /\* Metadata tối thiểu của Active Rule Table. \*/ <br>typedef struct { <br>uint16_t rule_count; // Số RuleRecord hợp lệ đang active <br>} SPLC_RuleTableInfo; <br><br/>/\* Kiểu trigger của một rule. \*/ <br>typedef enum { <br>SPLC_TRG_ON_CHANGE = 0, // Giá trị trigger_tag thay đổi <br>SPLC_TRG_ON_RISE = 1, // Sườn lên / vượt ngưỡng theo hướng tăng <br>SPLC_TRG_ON_FALL = 2, // Sườn xuống / vượt ngưỡng theo hướng giảm <br>SPLC_TRG_TIME_WINDOW = 3, // Trigger theo thời gian trong ngày <br>SPLC_TRG_INTERVAL = 4 // Trigger định kỳ theo for_ms <br>} SPLC_TriggerType; <br><br/>/\* Phép so sánh bổ sung trên trigger value. \*/ <br>typedef enum { <br>SPLC_OP_NONE = 0, <br>SPLC_OP_EQ = 1, <br>SPLC_OP_NEQ = 2, <br>SPLC_OP_GT = 3, <br>SPLC_OP_LT = 4, <br>SPLC_OP_GTE = 5, <br>SPLC_OP_LTE = 6, <br>SPLC_OP_BETWEEN = 7 <br>} SPLC_CompareOp; <br><br/>/\* Hành động được thực thi khi rule thỏa điều kiện. \*/ <br>typedef enum { <br>SPLC_ACT_SET_TAG = 0, // action_tag = action_param <br>SPLC_ACT_TOGGLE_TAG = 1, // Đảo trạng thái action_tag <br>SPLC_ACT_INC_COUNTER = 2, // action_tag += action_param <br>SPLC_ACT_WRITE_REMOTE = 3, // Ghi giá trị sang remote tag <br>SPLC_ACT_LOG_EVENT = 4, // Ghi event nội bộ <br>SPLC_ACT_SEND_ALARM = 5, // Phát alarm code <br>SPLC_ACT_ADD_TAG = 6, // action_tag += trigger_tag value <br>SPLC_ACT_SCALE_TAG = 7 // Scale trigger value sang action_tag <br>} SPLC_ActionType; <br><br/>/\* <br>\* Wire record của một rule. Kích thước cố định 32 byte. <br>\* Enum được lưu bằng integer cố định để không phụ thuộc compiler. <br>\*/ <br>typedef struct { <br>int32_t threshold_lo; // Ngưỡng chính / cận dưới <br>int32_t threshold_hi; // Cận trên / field phụ theo action <br>uint32_t for_ms; // Thời gian giữ điều kiện liên tục <br>int32_t action_param; // Giá trị/hệ số của action <br><br/>uint16_t trigger_tag; // Tag nguồn phát sinh trigger <br>uint16_t action_tag; // Tag đích của action <br>uint16_t guard_tag; // bit 0..14: tag index; bit 15: NEGATE <br><br/>uint8_t enabled; // 0 = disable, 1 = enable <br>uint8_t trigger_type; // SPLC_TriggerType <br>uint8_t compare_op; // SPLC_CompareOp <br>uint8_t action_type; // SPLC_ActionType <br><br/>uint8_t reserved\[6\]; // Sender ghi 0; receiver bỏ qua <br>} SPLC_RuleRecord; // 32 byte |
| -------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
|                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                                    |

# 3\. RULE TRANSFER

**Vai trò:** Mô tả metadata của một lần ghi Rule Table từ App xuống vùng staging; trên Modbus map tương ứng RULE_COUNT_STAGED (0x9002) và EXPECTED_CRC16 (0x9003).

/\* Metadata của một lần transfer Rule Table từ App xuống MCU. \*/  
typedef struct {  
uint16_t rule_count; // Số RuleRecord App sẽ ghi vào staging  
uint16_t crc16; // CRC-16/MODBUS của toàn bộ RuleRecord\[\] đã serialize  
} SPLC_RuleTransferInfo; // 4 byte = 2 Modbus registers  
<br/>/\*  
\* Quy tắc CRC cho Rule Transfer:  
\* - Mỗi frame Modbus RTU vẫn dùng CRC16 chuẩn của Modbus; NanoModbus xử lý ở tầng frame.  
\* - App tính thêm CRC-16/MODBUS trên toàn bộ Rule Table đã serialize.  
\* - MCU tính lại CRC-16/MODBUS trên staging buffer trước khi commit.  
\* - Chỉ commit khi CRC khớp; nếu sai trả SPLC_ERROR_CRC_MISMATCH.  
\* - Phạm vi CRC = rule_count \* sizeof(SPLC_RuleRecord) byte.  
\* - reserved\[\] trong SPLC_RuleRecord phải được sender ghi 0 để dữ liệu CRC là deterministic.  
\*/

# 4\. DEVICE HEALTH

**Vai trò:** Cung cấp health tối thiểu cho commissioning/service: uptime, nguyên nhân reset, cảnh báo tổng hợp, CPU, RAM và thời gian scan.

/\* Nguyên nhân chính của lần reset gần nhất. Chỉ một giá trị tại một thời điểm. \*/  
typedef enum {  
SPLC_RESET_UNKNOWN = 0,  
SPLC_RESET_POWER_ON = 1,  
SPLC_RESET_SOFTWARE = 2,  
SPLC_RESET_WATCHDOG = 3,  
SPLC_RESET_BROWNOUT = 4,  
SPLC_RESET_EXTERNAL = 5  
} SPLC_ResetReason;  
<br/>/\*  
\* Health bitmask: có thể OR nhiều cờ đồng thời.  
\* SPLC_HEALTH_NONE = 0 nghĩa là không có cảnh báo health.  
\*/  
typedef enum {  
SPLC_HEALTH_NONE = 0,  
SPLC_HEALTH_CPU_HIGH = 1u << 0,  
SPLC_HEALTH_RAM_HIGH = 1u << 1,  
SPLC_HEALTH_SCAN_OVERRUN = 1u << 2  
} SPLC_HealthFlags;  
<br/>/\* Snapshot health tối thiểu để App hiển thị Health/Diagnostics. \*/  
typedef struct {  
uint32_t uptime_s; // Thời gian chạy từ boot gần nhất, đơn vị giây  
<br/>uint16_t reset_reason; // SPLC_ResetReason  
uint16_t health_flags; // Bitmask SPLC_HealthFlags  
<br/>uint16_t cpu_load_percent; // 0..100 %  
uint16_t ram_usage_percent; // 0..100 %  
<br/>uint32_t scan_time_ms; // Thời gian scan gần nhất, microsecond  
uint32_t max_scan_time_ms; // Scan lớn nhất ghi nhận từ boot  
} SPLC_DeviceHealth; // 20 byte

# 5\. RUNTIME TAG MONITOR

**Vai trò:** Đọc giá trị sống của toàn bộ Tag Table để debug/monitor mà không đọc lại Flash hay Rule config.

/\* Giá trị runtime của một tag; mảng truyền theo tag_index, App resolve bằng ProductDefinition. \*/  
typedef int32_t SPLC_TagRuntimeValue;  
<br/>/\* Ví dụ: \[1\]=DI0, \[9\]=DO0, \[37\]=VREG0. \*/

# 6\. SYSTEM COMMANDS

**Vai trò:** Các lệnh maintenance do App gửi xuống MCU. Đây không phải config persistent.

/\* Lệnh maintenance cấp hệ thống do App gửi xuống MCU. \*/  
typedef enum {  
SPLC_SYSTEM_CMD_NONE = 0,  
SPLC_SYSTEM_CMD_REBOOT = 1, // Reboot MCU, giữ config/rule  
SPLC_SYSTEM_CMD_FACTORY_RESET = 2, // Khôi phục mặc định theo policy sản phẩm  
SPLC_SYSTEM_CMD_CLEAR_RULES = 3, // Xóa Active Rule Table  
SPLC_SYSTEM_CMD_CLEAR_RETAIN = 4 // Xóa dữ liệu retain  
} SPLC_SystemCommand;  
<br/>/\* Trạng thái thực thi System Command. \*/  
typedef enum {  
SPLC_CMD_STATUS_IDLE = 0,  
SPLC_CMD_STATUS_ACCEPTED = 1,  
SPLC_CMD_STATUS_BUSY = 2,  
SPLC_CMD_STATUS_DONE = 3,  
SPLC_CMD_STATUS_ERROR = 4  
} SPLC_CommandStatus;  
<br/>/\* Request command tối giản. \*/  
typedef struct {  
uint16_t command; // SPLC_SystemCommand  
} SPLC_SystemCommandRequest; // 2 byte  
<br/>/\* Kết quả command do MCU trả về. \*/  
typedef struct {  
uint16_t status; // SPLC_CommandStatus  
uint16_t error_code; // SPLC_ErrorCode; 0 = không lỗi  
} SPLC_SystemCommandResult; // 4 byte

# 7\. COMMON ERROR CODES

**Vai trò:** Một bộ mã lỗi dùng chung cho command/config operation để App và MCU không phải định nghĩa error riêng cho từng chức năng.

/\* Mã lỗi chung cho command/config operation. \*/  
typedef enum {  
SPLC_ERROR_NONE = 0, // Không lỗi  
SPLC_ERROR_INVALID_COMMAND = 1, // Command id không hợp lệ  
SPLC_ERROR_INVALID_PARAMETER = 2, // Tham số không hợp lệ  
SPLC_ERROR_BUSY = 3, // MCU đang bận xử lý operation khác  
SPLC_ERROR_CRC_MISMATCH = 4, // CRC dữ liệu không khớp  
SPLC_ERROR_UNSUPPORTED = 5, // Chức năng không được hỗ trợ  
SPLC_ERROR_FLASH = 6 // Lỗi đọc/ghi Flash  
} SPLC_ErrorCode;

Phạm vi: data contract V1 + Modbus register map V1. Vùng config transfer giữ nền địa chỉ 0x9000...0xA001 theo firmware spec hiện tại và chỉ mở rộng khi có requirement rõ ràng.

# 8\. MODBUS REGISTER MAP (V1)

**Mục đích:** cố định địa chỉ, quyền truy cập và định dạng dữ liệu giữa App và MCU. USB là transport vật lý; NanoModbus xử lý Modbus RTU.

Quy ước: 1 register = 16 bit. FC03 = read. FC16 = write nhiều register. RuleRecord = 32 byte = 16 register.

## 8.1 Core / Runtime register map

| **Address**   | **R/W** | **Register / Block**  | **Length (reg)** | **Data contract**         | **Technical meaning**                            |
| ------------- | ------- | --------------------- | ---------------- | ------------------------- | ------------------------------------------------ |
| 0x0000-0x0009 | RO      | DEVICE_DESCRIPTOR     | 10               | SPLC_DeviceDescriptor     | Nhận dạng device, HW/FW và protocol.             |
| 0x0010        | RO      | RULE_TABLE_INFO       | 1                | SPLC_RuleTableInfo        | Số rule đang active.                             |
| 0x0100-0x073F | RO      | ACTIVE_RULE_TABLE     | 1600 max         | SPLC_RuleRecord\[100\]    | Rule Table đã commit, Rule Engine đang thực thi. |
| 0x0800-0x0809 | RO      | DEVICE_HEALTH         | 10               | SPLC_DeviceHealth         | Uptime, reset, CPU/RAM, scan time.               |
| 0x0900-0x09FF | RO      | RUNTIME_TAG_VALUES    | 256 max          | int32_t\[128\]            | Giá trị runtime của Tag; 2 reg/tag.              |
| 0x0A00        | WO      | SYSTEM_COMMAND        | 1                | SPLC_SystemCommandRequest | App gửi lệnh hệ thống.                           |
| 0x0A01-0x0A02 | RO      | SYSTEM_COMMAND_RESULT | 2                | SPLC_SystemCommandResult  | Trạng thái và error_code của command.            |

## 8.2 Rule transfer / commit register map

| **Address**   | **R/W** | **Register / Block** | **Length (reg)** | **Data contract**      | **Technical meaning**                               |
| ------------- | ------- | -------------------- | ---------------- | ---------------------- | --------------------------------------------------- |
| 0x9000        | RO      | CONFIG_STATUS        | 1                | uint16_t               | 0=IDLE, 1=RECEIVING, 2=VERIFYING, 3=READY, 4=ERROR. |
| 0x9001        | RO      | CONFIG_ERROR_CODE    | 1                | SPLC_ErrorCode         | Lỗi của transfer / commit gần nhất.                 |
| 0x9002        | RW      | RULE_COUNT_STAGED    | 1                | uint16_t               | Số RuleRecord App sẽ ghi vào staging.               |
| 0x9003        | RW      | EXPECTED_CRC16       | 1                | uint16_t               | CRC-16/MODBUS của staged RuleRecord\[0..N-1\].      |
| 0x9004        | RO      | ACTIVE_RULE_COUNT    | 1                | uint16_t               | Số rule đang active.                                |
| 0x9005        | RO      | ACTIVE_RULE_CRC16    | 1                | uint16_t               | CRC16 của Active Rule Table.                        |
| 0x9006-0x900F | \-      | RESERVED             | 10               | \-                     | Reserved V1; ghi/đọc bỏ qua.                        |
| 0x9010-0x964F | RW      | STAGING_RULE_TABLE   | 1600 max         | SPLC_RuleRecord\[100\] | Buffer nhận rule mới; chưa được thực thi.           |
| 0xA000        | WO      | COMMIT_COMMAND       | 1                | uint16_t               | Ghi 0xA5A5 để verify CRC và commit.                 |
| 0xA001        | RO      | ACTIVE_RULE_VERSION  | 1                | uint16_t               | Tăng sau mỗi commit thành công.                     |

Active và Staging dùng cùng SPLC_RuleRecord\[100\]. Khác biệt: Active = bảng đang chạy (RO); Staging = vùng tạm nhận cấu hình mới (RW). MCU chỉ swap Staging -> Active sau verify + commit thành công.

## 8.3 RuleIndex và địa chỉ RuleRecord

RuleIndex là vị trí zero-based của record trong Rule Table; V1 không lưu rule_id bên trong SPLC_RuleRecord.

| **Offset (from Rule Base)** | **Field**                | **Type**    | **Encoding**                                   |
| --------------------------- | ------------------------ | ----------- | ---------------------------------------------- |
| +0..+1                      | threshold_lo             | int32_t     | High Word -> Low Word                          |
| +2..+3                      | threshold_hi             | int32_t     | High Word -> Low Word                          |
| +4..+5                      | for_ms                   | uint32_t    | High Word -> Low Word                          |
| +6..+7                      | action_param             | int32_t     | High Word -> Low Word                          |
| +8                          | trigger_tag              | uint16_t    | 1 register                                     |
| +9                          | action_tag               | uint16_t    | 1 register                                     |
| +10                         | guard_tag                | uint16_t    | 1 register                                     |
| +11                         | enabled / trigger_type   | 2 x uint8_t | High byte = enabled; Low byte = trigger_type   |
| +12                         | compare_op / action_type | 2 x uint8_t | High byte = compare_op; Low byte = action_type |
| +13..+15                    | reserved\[6\]            | 6 x uint8_t | Sender ghi 0; receiver bỏ qua                  |

Địa chỉ: ActiveRule\[i\] = 0x0100 + i x 16; StagingRule\[i\] = 0x9010 + i x 16. Ví dụ i=2 -> Active 0x0120-0x012F; Staging 0x9030-0x903F. UI có thể hiển thị RuleIndex 2 là R3.

## 8.4 Register encoding

| **Data**              | **Registers**        | **Rule**                      | **Example**                                      |
| --------------------- | -------------------- | ----------------------------- | ------------------------------------------------ |
| 32-bit value          | 2 register           | High Word trước, Low Word sau | 0x12345678 -> Reg\[N\]=0x1234, Reg\[N+1\]=0x5678 |
| 2 x uint8_t           | 1 register           | High byte / Low byte          | enabled=1, trigger_type=2 -> 0x0102              |
| reserved\[\]          | fixed bytes          | Sender ghi 0                  | Đảm bảo wire format và CRC deterministic         |
| CRC16 toàn Rule Table | rule_count x 32 byte | CRC-16/MODBUS                 | 37 rule -> CRC trên 1184 byte                    |

## 8.5 Rule Table read / write

| **Direction** | **Step** | **Operation**                           | **Result**                                                    |
| ------------- | -------- | --------------------------------------- | ------------------------------------------------------------- |
| Read          | 1        | FC03 RULE_TABLE_INFO                    | Lấy rule_count.                                               |
| Read          | 2        | FC03 ACTIVE_RULE_TABLE                  | Đọc RuleRecord\[0..rule_count-1\].                            |
| Write         | 1        | FC16 RULE_COUNT_STAGED + EXPECTED_CRC16 | Khai báo số rule và CRC toàn bảng.                            |
| Write         | 2        | FC16 STAGING_RULE_TABLE                 | Ghi RuleRecord\[\] theo đúng RuleIndex.                       |
| Write         | 3        | FC16/FC06 COMMIT_COMMAND = 0xA5A5       | Yêu cầu MCU verify CRC và commit.                             |
| MCU           | 4        | Verify + swap                           | CRC đúng -> swap Active; CRC sai -> giữ Active cũ và báo lỗi. |

Ví dụ 37 rule: App serialize 37 x 32 = 1184 byte, ghi staging theo nhiều frame FC16. Mỗi frame có CRC Modbus riêng; MCU chỉ commit khi CRC16 của toàn bộ 1184 byte khớp EXPECTED_CRC16.