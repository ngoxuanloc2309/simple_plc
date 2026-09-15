**TÀI LIỆU KỸ THUẬT: SIMPLEPLC APP-MCU DATA STRUCTS & MODBUS REGISTER MAP**

**Data Contract V1 | Modbus Register Map V1 | Phiên bản 1.9**

**Hệ thống:** SimplePLC Ecosystem

**Phạm vi:** App ↔ MCU

**Transport:** USB VCP

**Protocol:** Modbus RTU / NanoModbus

**Tài liệu:** Data Contract + Register Map

**Trạng thái: Technical Draft / Review — V1.9 Self-Describing Profile**

**THAY ĐỔI CONTRACT V1.9 — các nội dung thay đổi / bổ sung được in đậm trong tài liệu**

**CHANGED — DeviceResourceInfo được đơn giản hóa: bỏ CapabilityFlags. Wire Profile V1 mặc định bắt buộc Runtime Tags, Device Health và System Commands; Rule Engine suy ra từ max_rules > 0, Retain suy ra từ vreg_retain_count > 0.**

**CHANGED — Tag layout V1 cố định: VFLAG=32, VREG=32, VREG_RETAIN=32, COUNTER=8; Remote I/O hiện tại dùng 124/128 tag.**

**CHANGED — Chuẩn hóa TagKind numeric 0..9; giữ mã 6/7 cho MB_COIL / MB_HOLDING.**

**CHANGED — DeviceHealth đổi scan_time_us / max_scan_time_us thành scan_time_ms / max_scan_time_ms; wire size không đổi.**

**CHANGED — Modbus map DEVICE_RESOURCE_INFO chuyển thành 0x0020..0x0029 (10 register, RO) do bỏ capability_flags.**

**RULE — Unknown ProductVariant không đồng nghĩa Unsupported; App chấp nhận nếu protocol/wire profile/resource profile hợp lệ.**

# **0\. QUY ƯỚC DÙNG CHUNG**

• Các field truyền qua Modbus dùng kiểu có kích thước cố định: uint8_t / uint16_t / uint32_t / int32_t.

• Enum chỉ định giá trị symbolic. Trong wire struct vẫn dùng integer cố định để tránh phụ thuộc kích thước enum của compiler C.

• Field dạng bitmask có thể đồng thời chứa nhiều cờ. Giá trị 0 nghĩa là không có cờ nào được set.

• Tên field dùng snake_case; type dùng tiền tố SPLC_.

# **1\. DEVICE DESCRIPTOR**

Vai trò: Nhận dạng loại thiết bị và phiên bản mà App đang kết nối. Từ V1.8+, ProductVariant chỉ dùng cho identity/diagnostics; cấu hình tài nguyên thực tế được đọc từ DeviceResourceInfo, không lookup hard-code theo variant.

```
/* Device class: family cấp cao của sản phẩm. */
typedef enum {
    SPLC_DEVICE_CLASS_UNKNOWN    = 0,
    SPLC_DEVICE_CLASS_REMOTE_IO  = 1,
    SPLC_DEVICE_CLASS_DATALOGGER = 2,
    SPLC_DEVICE_CLASS_GATEWAY    = 3,
    SPLC_DEVICE_CLASS_CONTROLLER = 4
} SPLC_DeviceClass;

/* V1.8: device_variant là mã nhận dạng model do MCU khai báo.
 * App KHÔNG yêu cầu phải biết trước variant để dựng resource profile.
 * Unknown variant vẫn có thể được accept nếu protocol/profile hợp lệ.
 */

/* Descriptor App đọc ngay sau khi kết nối. */
typedef struct {
    uint16_t device_class;        // SPLC_DeviceClass
    uint16_t device_variant;      // Product identity; không quyết định resource layout trong App

    uint16_t hw_version_major;    // HW major
    uint16_t hw_version_minor;    // HW minor
    uint16_t hw_version_patch;    // HW patch

    uint16_t fw_version_major;    // FW major
    uint16_t fw_version_minor;    // FW minor
    uint16_t fw_version_patch;    // FW patch

    uint16_t protocol_version;    // Version contract App <-> MCU
    uint16_t rule_format_version; // Version layout/semantic RuleRecord
} SPLC_DeviceDescriptor;          // 20 byte
```

## 1.1 DEVICE RESOURCE INFO / SELF-DESCRIBING PROFILE

**CHANGED V1.9: MCU là nguồn mô tả tài nguyên thực tế. App đọc block này sau DeviceDescriptor, validate theo Wire Profile V1 rồi tự tạo ProductDefinition + TagCatalog runtime. Các dịch vụ nền tảng bắt buộc của V1 không cần khai báo lại bằng capability bitmask.**

```
/* Wire profile: layout/tag-addressing contract mà App và MCU cùng hiểu. */
typedef enum {
    SPLC_WIRE_PROFILE_UNKNOWN = 0,
    SPLC_WIRE_PROFILE_V1      = 1
} SPLC_WireProfile;

/* CHANGED V1.9: Wire Profile V1 mặc định hỗ trợ Runtime Tags, Device Health và System Commands.
 * Không dùng capability_flags để khai báo lại các chức năng bắt buộc.
 * Rule Engine: max_rules > 0.
 * Retentive Memory: vreg_retain_count > 0.
 */

/* Tài nguyên thực tế của device. Tất cả count là số slot hợp lệ mà product sử dụng. */
typedef struct {
    uint16_t wire_profile;          // SPLC_WireProfile; V1 = 1
    uint16_t max_rules;             // 0..100; >0 => có Rule Engine
    uint16_t runtime_tag_count;     // Tổng tag hợp lệ; không có nghĩa index 0..N-1 liên tục

    uint16_t di_count;              // 0..8
    uint16_t do_count;              // 0..8
    uint16_t ai_count;              // 0..4
    uint16_t vflag_count;           // 0..32
    uint16_t vreg_count;            // 0..32
    uint16_t vreg_retain_count;     // 0..32; >0 => có Retentive Memory
    uint16_t counter_count;         // 0..8
} SPLC_DeviceResourceInfo;          // CHANGED V1.9: 20 byte = 10 Modbus registers
```

## **1.2 TAG KIND — PLATFORM CONTRACT V1**

**THAY ĐỔI V1.8: Chuẩn hóa numeric value để Domain/App và firmware/spec không lệch mã; 6 và 7 được giữ cho remote Modbus kinds.**

```
typedef enum {
    SPLC_TAG_KIND_NONE        = 0,
    SPLC_TAG_KIND_DI          = 1,
    SPLC_TAG_KIND_DO          = 2,
    SPLC_TAG_KIND_AI          = 3,
    SPLC_TAG_KIND_VFLAG       = 4,
    SPLC_TAG_KIND_VREG        = 5,
    SPLC_TAG_KIND_MB_COIL     = 6,
    SPLC_TAG_KIND_MB_HOLDING  = 7,
    SPLC_TAG_KIND_VREG_RETAIN = 8,
    SPLC_TAG_KIND_COUNTER     = 9
} SPLC_TagKind;
```

# **2\. RULE TABLE**

**Vai trò:** Mô tả bảng rule đang active trên MCU để App có thể đọc toàn bộ logic hiện tại và dựng lại Rule Editor.

```
/* Metadata tối thiểu của Active Rule Table. */
typedef struct {
    uint16_t rule_count;          // Số RuleRecord hợp lệ đang active
} SPLC_RuleTableInfo;

/* Kiểu trigger của một rule. */
typedef enum {
    SPLC_TRG_ON_CHANGE   = 0,     // Giá trị trigger_tag thay đổi
    SPLC_TRG_ON_RISE     = 1,     // Sườn lên / vượt ngưỡng theo hướng tăng
    SPLC_TRG_ON_FALL     = 2,     // Sườn xuống / vượt ngưỡng theo hướng giảm
    SPLC_TRG_TIME_WINDOW = 3,     // Trigger theo thời gian trong ngày
    SPLC_TRG_INTERVAL    = 4      // Trigger định kỳ theo for_ms
} SPLC_TriggerType;

/* Phép so sánh bổ sung trên trigger value. */
typedef enum {
    SPLC_OP_NONE    = 0,
    SPLC_OP_EQ      = 1,
    SPLC_OP_NEQ     = 2,
    SPLC_OP_GT      = 3,
    SPLC_OP_LT      = 4,
    SPLC_OP_GTE     = 5,
    SPLC_OP_LTE     = 6,
    SPLC_OP_BETWEEN = 7
} SPLC_CompareOp;

/* Hành động được thực thi khi rule thỏa điều kiện. */
typedef enum {
    SPLC_ACT_SET_TAG      = 0,    // action_tag = action_param
    SPLC_ACT_TOGGLE_TAG   = 1,    // Đảo trạng thái action_tag
    SPLC_ACT_INC_COUNTER  = 2,    // action_tag += action_param
    SPLC_ACT_WRITE_REMOTE = 3,    // Ghi giá trị sang remote tag
    SPLC_ACT_LOG_EVENT    = 4,    // Ghi event nội bộ
    SPLC_ACT_SEND_ALARM   = 5,    // Phát alarm code
    SPLC_ACT_ADD_TAG      = 6,    // action_tag += trigger_tag value
    SPLC_ACT_SCALE_TAG    = 7     // Scale trigger value sang action_tag
} SPLC_ActionType;

/*
 * Wire record của một rule. Kích thước cố định 32 byte.
 * Enum được lưu bằng integer cố định để không phụ thuộc compiler.
 */
typedef struct {
    int32_t  threshold_lo;        // Ngưỡng chính / cận dưới
    int32_t  threshold_hi;        // Cận trên / field phụ theo action
    uint32_t for_ms;              // Thời gian giữ điều kiện liên tục
    int32_t  action_param;        // Giá trị/hệ số của action

    uint16_t trigger_tag;         // Tag nguồn phát sinh trigger
    uint16_t action_tag;          // Tag đích của action
    uint16_t guard_tag;           // bit 0..14: tag index; bit 15: NEGATE

    uint8_t  enabled;             // 0 = disable, 1 = enable
    uint8_t  trigger_type;        // SPLC_TriggerType
    uint8_t  compare_op;          // SPLC_CompareOp
    uint8_t  action_type;         // SPLC_ActionType

    uint8_t  reserved[6];         // Sender ghi 0; receiver bỏ qua
} SPLC_RuleRecord;                // 32 byte
```

# **3\. RULE TRANSFER**

**Vai trò:** Mô tả metadata của một lần ghi Rule Table từ App xuống vùng staging; trên Modbus map tương ứng RULE_COUNT_STAGED (0x9002) và EXPECTED_CRC16 (0x9003).

```
/* Metadata của một lần transfer Rule Table từ App xuống MCU. */
typedef struct {
    uint16_t rule_count;          // Số RuleRecord App sẽ ghi vào staging
    uint16_t crc16;               // CRC-16/MODBUS của toàn bộ RuleRecord[] đã serialize
} SPLC_RuleTransferInfo;          // 4 byte = 2 Modbus registers

/*
 * Quy tắc CRC cho Rule Transfer:
 * - Mỗi frame Modbus RTU vẫn dùng CRC16 chuẩn của Modbus; NanoModbus xử lý ở tầng frame.
 * - App tính thêm CRC-16/MODBUS trên toàn bộ Rule Table đã serialize.
 * - MCU tính lại CRC-16/MODBUS trên staging buffer trước khi commit.
 * - Chỉ commit khi CRC khớp; nếu sai trả SPLC_ERROR_CRC_MISMATCH.
 * - Phạm vi CRC = rule_count * sizeof(SPLC_RuleRecord) byte.
 * - reserved[] trong SPLC_RuleRecord phải được sender ghi 0 để dữ liệu CRC là deterministic.
 */
```

# **4\. DEVICE HEALTH**

**Vai trò:** Cung cấp health tối thiểu cho commissioning/service: uptime, nguyên nhân reset, cảnh báo tổng hợp, CPU, RAM và **thời gian scan theo millisecond (ms).**

```
/* Nguyên nhân chính của lần reset gần nhất. Chỉ một giá trị tại một thời điểm. */
typedef enum {
    SPLC_RESET_UNKNOWN   = 0,
    SPLC_RESET_POWER_ON  = 1,
    SPLC_RESET_SOFTWARE  = 2,
    SPLC_RESET_WATCHDOG  = 3,
    SPLC_RESET_BROWNOUT  = 4,
    SPLC_RESET_EXTERNAL  = 5
} SPLC_ResetReason;

/* Health bitmask: có thể OR nhiều cờ đồng thời. */
typedef enum {
    SPLC_HEALTH_NONE         = 0,
    SPLC_HEALTH_CPU_HIGH     = 1u << 0,
    SPLC_HEALTH_RAM_HIGH     = 1u << 1,
    SPLC_HEALTH_SCAN_OVERRUN = 1u << 2
} SPLC_HealthFlags;

/* Snapshot health tối thiểu để App hiển thị Health/Diagnostics. */
typedef struct {
    uint32_t uptime_s;            // Thời gian chạy từ boot gần nhất, đơn vị giây

    uint16_t reset_reason;        // SPLC_ResetReason
    uint16_t health_flags;        // Bitmask SPLC_HealthFlags

    uint16_t cpu_load_percent;    // 0..100 %
    uint16_t ram_usage_percent;   // 0..100 %

    uint32_t scan_time_ms;        // THAY ĐỔI V1.8: thời gian scan gần nhất, millisecond
    uint32_t max_scan_time_ms;    // THAY ĐỔI V1.8: scan lớn nhất từ boot, millisecond
} SPLC_DeviceHealth;              // 20 byte; wire layout không đổi
```

# **5\. RUNTIME TAG MONITOR**

**Vai trò:** Đọc giá trị sống của Tag Table để debug/monitor. **V1.8 dùng fixed TagIndex ranges; số slot hợp lệ của từng nhóm được MCU khai báo qua SPLC_DeviceResourceInfo.**

```
/* Giá trị runtime của một tag; mỗi TagIndex dùng int32 = 2 Modbus registers. */
typedef int32_t SPLC_TagRuntimeValue;

/* V1.8 fixed examples:
 * DI0              -> TagIndex 0
 * DO0              -> TagIndex 8
 * VFLAG0           -> TagIndex 20
 * VREG0            -> TagIndex 52
 * VREG_RETAIN0     -> TagIndex 84
 * COUNTER0         -> TagIndex 116
 * App build TagCatalog runtime từ SPLC_DeviceResourceInfo.
 */
```

## **5.1 FIXED TAG LAYOUT — WIRE PROFILE V1**

| **Resource**    | **TagIndex range** | **Capacity V1** |
| --------------- | ------------------ | --------------- |
| **DI**          | **0..7**           | **8**           |
| **DO**          | **8..15**          | **8**           |
| **AI**          | **16..19**         | **4**           |
| **VFLAG**       | **20..51**         | **32**          |
| **VREG**        | **52..83**         | **32**          |
| **VREG_RETAIN** | **84..115**        | **32**          |
| **COUNTER**     | **116..123**       | **8**           |
| **RESERVED**    | **124..127**       | **4**           |

**THAY ĐỔI V1.8: 128 là wire capacity. Product có thể dùng ít hơn; slot không khai báo là invalid/reserved. runtime_tag_count là tổng tag hợp lệ nhưng KHÔNG đại diện cho một dải index liên tục.**

# **6\. SYSTEM COMMANDS**

**Vai trò:** Các lệnh maintenance do App gửi xuống MCU. Đây không phải config persistent.

```
/* Lệnh maintenance cấp hệ thống do App gửi xuống MCU. */
typedef enum {
    SPLC_SYSTEM_CMD_NONE          = 0,
    SPLC_SYSTEM_CMD_REBOOT        = 1, // Reboot MCU, giữ config/rule
    SPLC_SYSTEM_CMD_FACTORY_RESET = 2, // Khôi phục mặc định theo policy sản phẩm
    SPLC_SYSTEM_CMD_CLEAR_RULES   = 3, // Xóa Active Rule Table
    SPLC_SYSTEM_CMD_CLEAR_RETAIN  = 4  // Xóa dữ liệu retain
} SPLC_SystemCommand;

/* Trạng thái thực thi System Command. */
typedef enum {
    SPLC_CMD_STATUS_IDLE     = 0,
    SPLC_CMD_STATUS_ACCEPTED = 1,
    SPLC_CMD_STATUS_BUSY     = 2,
    SPLC_CMD_STATUS_DONE     = 3,
    SPLC_CMD_STATUS_ERROR    = 4
} SPLC_CommandStatus;

/* Request command tối giản. */
typedef struct {
    uint16_t command;             // SPLC_SystemCommand
} SPLC_SystemCommandRequest;      // 2 byte

/* Kết quả command do MCU trả về. */
typedef struct {
    uint16_t status;              // SPLC_CommandStatus
    uint16_t error_code;          // SPLC_ErrorCode; 0 = không lỗi
} SPLC_SystemCommandResult;       // 4 byte
```

# **7\. COMMON ERROR CODES**

**Vai trò:** Một bộ mã lỗi dùng chung cho command/config operation để App và MCU không phải định nghĩa error riêng cho từng chức năng.

```
/* Mã lỗi chung cho command/config operation. */
typedef enum {
    SPLC_ERROR_NONE              = 0, // Không lỗi
    SPLC_ERROR_INVALID_COMMAND   = 1, // Command id không hợp lệ
    SPLC_ERROR_INVALID_PARAMETER = 2, // Tham số không hợp lệ
    SPLC_ERROR_BUSY              = 3, // MCU đang bận xử lý operation khác
    SPLC_ERROR_CRC_MISMATCH      = 4, // CRC dữ liệu không khớp
    SPLC_ERROR_UNSUPPORTED       = 5, // Chức năng không được hỗ trợ
    SPLC_ERROR_FLASH             = 6  // Lỗi đọc/ghi Flash
} SPLC_ErrorCode;
```

**Phạm vi: data contract V1 + Modbus register map V1. Vùng config transfer giữ nền địa chỉ 0x9000...0xA001 theo firmware spec hiện tại. V1.9 dùng self-describing device profile (DeviceResourceInfo + WireProfile), không dùng CapabilityFlags; RuleRecord 32 byte và Runtime Tag region 0x0900..0x09FF không đổi.**

# **8\. MODBUS REGISTER MAP (V1)**

**Mục đích:** cố định địa chỉ, quyền truy cập và định dạng dữ liệu giữa App và MCU. USB là transport vật lý; NanoModbus xử lý Modbus RTU.

```
Quy ước: 1 register = 16 bit. FC03 = read. FC16 = write nhiều register. RuleRecord = 32 byte = 16 register.
```

## **8.1 Core / Runtime register map**

| **Address**       | **R/W** | **Register / Block**     | **Length (reg)** | **Data contract**           | **Technical meaning**                                                                                                       |
| ----------------- | ------- | ------------------------ | ---------------- | --------------------------- | --------------------------------------------------------------------------------------------------------------------------- |
| 0x0000-0x0009     | RO      | DEVICE_DESCRIPTOR        | 10               | SPLC_DeviceDescriptor       | **Nhận dạng device, HW/FW và protocol; ProductVariant chỉ là identity.**                                                    |
| 0x0010            | RO      | RULE_TABLE_INFO          | 1                | SPLC_RuleTableInfo          | Số rule đang active.                                                                                                        |
| 0x0011-0x001F     | \-      | RESERVED                 | 15               | \-                          | Reserved cho core profile V1.                                                                                               |
| **0x0020-0x0029** | **RO**  | **DEVICE_RESOURCE_INFO** | **10**           | **SPLC_DeviceResourceInfo** | **CHANGED V1.9: Wire profile, max_rules và resource counts để App tự build ProductDefinition; không còn capability_flags.** |
| 0x0100-0x073F     | RO      | ACTIVE_RULE_TABLE        | 1600 max         | SPLC_RuleRecord\[100\]      | Rule Table đã commit, Rule Engine đang thực thi.                                                                            |
| 0x0800-0x0809     | RO      | DEVICE_HEALTH            | 10               | SPLC_DeviceHealth           | **Uptime, reset, CPU/RAM, scan time (ms).**                                                                                 |
| 0x0900-0x09FF     | RO      | RUNTIME_TAG_VALUES       | 256 max          | int32_t\[128\]              | **V1.8: fixed 128-slot wire region; product dùng subset theo DEVICE_RESOURCE_INFO; 2 reg/tag.**                             |
| 0x0A00            | WO      | SYSTEM_COMMAND           | 1                | SPLC_SystemCommandRequest   | App gửi lệnh hệ thống.                                                                                                      |
| 0x0A01-0x0A02     | RO      | SYSTEM_COMMAND_RESULT    | 2                | SPLC_SystemCommandResult    | Trạng thái và error_code của command.                                                                                       |

## **8.2 Rule transfer / commit register map**

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

```
Active và Staging dùng cùng SPLC_RuleRecord[100]. Khác biệt: Active = bảng đang chạy (RO); Staging = vùng tạm nhận cấu hình mới (RW). MCU chỉ swap Staging -> Active sau verify + commit thành công.
```

## **8.3 RuleIndex và địa chỉ RuleRecord**

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

<div class="joplin-table-wrapper"><table><thead><tr><th><pre><code>Địa chỉ: ActiveRule[i] = 0x0100 + i x 16; StagingRule[i] = 0x9010 + i x 16. Ví dụ i=2 -&gt; Active 0x0120-0x012F; Staging 0x9030-0x903F. UI có thể hiển thị RuleIndex 2 là R3.</code></pre></th></tr></thead></table></div>

## **8.4 Register encoding**

| **Data**              | **Registers**        | **Rule**                      | **Example**                                      |
| --------------------- | -------------------- | ----------------------------- | ------------------------------------------------ |
| 32-bit value          | 2 register           | High Word trước, Low Word sau | 0x12345678 -> Reg\[N\]=0x1234, Reg\[N+1\]=0x5678 |
| 2 x uint8_t           | 1 register           | High byte / Low byte          | enabled=1, trigger_type=2 -> 0x0102              |
| reserved\[\]          | fixed bytes          | Sender ghi 0                  | Đảm bảo wire format và CRC deterministic         |
| CRC16 toàn Rule Table | rule_count x 32 byte | CRC-16/MODBUS                 | 37 rule -> CRC trên 1184 byte                    |

## **8.5 Rule Table read / write**

| **Direction** | **Step** | **Operation**                           | **Result**                                                    |
| ------------- | -------- | --------------------------------------- | ------------------------------------------------------------- |
| Read          | 1        | FC03 RULE_TABLE_INFO                    | Lấy rule_count.                                               |
| Read          | 2        | FC03 ACTIVE_RULE_TABLE                  | Đọc RuleRecord\[0..rule_count-1\].                            |
| Write         | 1        | FC16 RULE_COUNT_STAGED + EXPECTED_CRC16 | Khai báo số rule và CRC toàn bảng.                            |
| Write         | 2        | FC16 STAGING_RULE_TABLE                 | Ghi RuleRecord\[\] theo đúng RuleIndex.                       |
| Write         | 3        | FC16/FC06 COMMIT_COMMAND = 0xA5A5       | Yêu cầu MCU verify CRC và commit.                             |
| MCU           | 4        | Verify + swap                           | CRC đúng -> swap Active; CRC sai -> giữ Active cũ và báo lỗi. |

<div class="joplin-table-wrapper"><table><thead><tr><th><pre><code>Ví dụ 37 rule: App serialize 37 x 32 = 1184 byte, ghi staging theo nhiều frame FC16. Mỗi frame có CRC Modbus riêng; MCU chỉ commit khi CRC16 của toàn bộ 1184 byte khớp EXPECTED_CRC16.</code></pre></th></tr></thead></table></div>

## **8.6 Device profile discovery / runtime build flow**

**CHANGED V1.9: App không lookup ProductDefinition theo ProductVariant. Sau connect, App đọc Descriptor + DeviceResourceInfo, validate WireProfile V1 rồi sinh ProductDefinition/TagCatalog runtime. Feature nền tảng mặc định theo Wire Profile V1; Rule Engine/Retain được suy ra từ resource profile.**

| **Step** | **Read / Validate**                      | **Result**                                                                                                                                              |
| -------- | ---------------------------------------- | ------------------------------------------------------------------------------------------------------------------------------------------------------- |
| **1**    | **FC03 DEVICE_DESCRIPTOR**               | **Identity + versions**                                                                                                                                 |
| **2**    | **FC03 DEVICE_RESOURCE_INFO**            | **CHANGED V1.9: Wire profile + max_rules + resource counts (không capability_flags)**                                                                   |
| 3        | Validate profile                         | Counts <= V1 limits; max_rules <= 100; runtime_tag_count consistent                                                                                     |
| **4**    | **Build ProductDefinition + TagCatalog** | **Fixed TagIndex ranges, only declared slots are valid**                                                                                                |
| **5**    | **Create session + start monitor**       | **CHANGED V1.9: Runtime Tags / Health / System Commands là mandatory của Wire Profile V1; Rule Engine = max_rules > 0; Retain = vreg_retain_count > 0** |