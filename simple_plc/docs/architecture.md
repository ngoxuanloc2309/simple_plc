# SimplePLC — Ghi chú kiến trúc Firmware (đầy đủ, dùng để tham chiếu lại)

> File này tổng hợp toàn bộ nội dung đã bàn bạc về kiến trúc phân lớp (layered
> architecture) của SimplePLC — dùng để:
> - Tiếp tục vibe-code dựa trên khung đã chốt.
> - Đưa cho 1 Claude/AI khác đọc để nắm ngữ cảnh mà không cần giải thích lại từ đầu.
> - Tra cứu lại khi quên 1 khái niệm nào đó.
>
> **Cập nhật lần này:** đồng bộ lại toàn bộ nội dung theo tài liệu chính thức
> mới nhất `SimplePLC_App_MCU_Structs_v1.7.docx` (data contract + Modbus
> register map V1) — tài liệu này là **nguồn sự thật cao nhất hiện có**, ưu
> tiên hơn mọi con số/quyết định cũ trong các bản trước của file này. Những
> chỗ khác biệt so với bản trước đều được đánh dấu rõ.

---

## 0. Bối cảnh dự án

SimplePLC là firmware lõi cho dòng Remote I/O 8DI/8DO/4AI, chạy trên
STM32H523CCU6 (Cortex-M33, 250MHz, 256KB Flash / 272KB SRAM). Đây là bản đầu
tiên của 1 nền tảng Rule Engine dùng chung cho cả họ sản phẩm IIoT sau này
(Datalogger, Gateway, Controller...), nên nguyên tắc **"không heap động,
struct nén cố định, tách lớp rạch ròi"** là bắt buộc để port sang MCU khác
sau này (kể cả MCU nhỏ hơn như STM32G0/F1).

**Nguồn tài liệu, theo đúng thứ tự ưu tiên (mới hơn ghi đè cũ hơn):**
1. `SimplePLC_App_MCU_Structs_v1.7.docx` — **data contract + Modbus register
   map V1 chính thức**, quy định wire format thật giữa App và MCU. Đây là bản
   quan trọng nhất, mọi con số địa chỉ/kích thước struct phải khớp đúng file
   này.
2. `SimplePLC_RuleStruct_MCU_Spec_v0_1.md` — tài liệu spec gốc, vẫn đúng cho
   phần **logic nghiệp vụ Rule Engine** (5 bước Trigger→Compare→Dwell→Guard→
   Action, ý nghĩa từng ActionType/TriggerType/CompareOp) — chỉ riêng **kích
   thước struct và CRC đã bị v1.7 ghi đè** (xem mục 2.2).
3. File này (`architecture.md`) — diễn giải kiến trúc layer, không tự đặt ra
   con số nào mâu thuẫn với 2 nguồn trên.

**Transport vật lý giữa App và MCU: USB (KHÔNG phải RS485/UART).** Đây là
điểm quan trọng đã đổi so với giả định ban đầu trong dự án — v1.7 ghi rõ
*"USB là transport vật lý; NanoModbus xử lý Modbus RTU"*. RS485 chỉ còn xuất
hiện ở 1 chỗ khác hẳn: 1 **variant của sản phẩm Gateway**
(`SPLC_GATEWAY_VARIANT_RS485_ETH`) — dùng để Gateway (đóng vai Modbus
**Master**) nói chuyện với các thiết bị Modbus khác ở hạ tầng bên dưới, không
liên quan gì tới kênh App↔MCU. Xem mục 2.4b để biết ảnh hưởng cụ thể tới
Layer 0/1/3.5.

---

## 1. Tổng quan — 7 layer (5 layer nghiệp vụ gốc + Utils + Protocol Porting)

```
Layer 4     Engine & Application entry        app/
Layer 3     PLC Application Services          services/
Layer 3.5   Protocol / Library Porting        port/         <- xem mục 2.4b
Layer 2     PLC Core                          core/
Layer 1     SX Driver Core                    components/
Layer 0     Platform                          platforms/
Layer U     Utils (+ thư viện ngoài)          utils/, libs/  (nền)
```

### Quy tắc xuyên suốt cả 7 layer

1. **Include chỉ chạy 1 chiều, từ trên xuống.** Layer cao gọi layer thấp,
   không bao giờ ngược lại.
2. **Mô hình "kéo" (poll), không phải "đẩy" (push/event).** Ghi giá trị vào 1
   nơi thì dừng lại, không tự động kích hoạt bước tiếp theo ngay lập tức.
3. **Layer 2 là ranh giới port (ranh giới build).** Không include gì từ
   Layer 0/1/3.5 -> build/test được trên PC thuần, không cần phần cứng thật.
4. **Layer U (Utils) không include ngược lên ai.**
5. **Layer 3.5 (Protocol Porting) chỉ đứng giữa đúng Layer U và Layer 1.**

---

## 2. Chi tiết từng layer

### Layer U — Utils

| File | Việc |
|---|---|
| `cqueue.h` / `cqueue.c` | Hàng đợi vòng — buffer RX/TX cho UART lẫn USB CDC. Đã có sẵn code. |
| `filter.h` / `filter.c` | Bộ lọc tín hiệu — làm mượt giá trị AI. Đã có sẵn code. |
| `crc16_modbus.h` / `.c` | **CRC-16/MODBUS** — dùng cho khung Modbus RTU LẪN CRC riêng của SimplePLC trên Rule Table. **Sửa so với bản trước:** trước đây ghi CRC32, đã xác nhận sai theo v1.7. |
| `logger.h` / `.c` | Port từ SynaptiX FDK (repo `WS_v1`). Khi port: bỏ mutex FreeRTOS vì SimplePLC chạy bare-metal đơn luồng. |

---

### Layer 0 — Platform

| File | Việc |
|---|---|
| `sx_gpio_stm32.c` | `HAL_GPIO_ReadPin/WritePin` |
| `sx_adc_stm32.c` | `HAL_ADC_*` |
| `sx_flash_stm32.c` | `HAL_FLASH_*` |
| `sx_usb_tiny_stm32.c` | **MỚI — thay cho `sx_uart_stm32.c` cũ trong vai trò kênh App-MCU.** Dựa trên TinyUSB (`tusb_init`, `tud_cdc_read/write`). Port từ `WS_v1/SynaptiX_FDK/components/peripherals/usb_cdc_tiny/`. |

**Vì sao đổi sang USB CDC:** v1.7 xác nhận App-MCU đi qua USB. `sx_uart` cũ
KHÔNG bị xoá — vẫn cần cho Gateway variant (RS485 làm Modbus Master).

---

### Layer 1 — SX Driver Core

```c
// sx_usb_tiny.h — MỚI
void sx_usb_tiny_init(sx_usb_tiny_t *usb, sx_usb_tiny_config_t *config);  // Layer 0
void sx_usb_tiny_process(sx_usb_tiny_t *usb);                             // Layer 0 (tud_task())
void sx_usb_tiny_write(sx_usb_tiny_t *usb, const uint8_t *data, uint32_t len); // Layer 0
int  sx_usb_tiny_read(sx_usb_tiny_t *usb, uint8_t *data, uint32_t len, uint32_t timeout_ms); // Layer 1 core
int  sx_usb_tiny_available(sx_usb_tiny_t *usb);                           // Layer 1 core

// sx_uart.h — VẪN GIỮ, dùng cho Gateway variant (RS485, Modbus Master)
int  sx_uart_init(sx_uart_t *uart, uint32_t baudrate);
int  sx_uart_write(sx_uart_t *uart, const uint8_t *data, int len);
int  sx_uart_read(sx_uart_t *uart, uint8_t *data, int len, uint32_t timeout_ms);

// sx_gpio.h / sx_adc.h / sx_flash.h — không đổi
```

**USB CDC và UART đều có "core" dùng chung** (`read/available` chỉ đọc
`cqueue`) — byte được đẩy vào hàng đợi từ Layer 0 (ISR/`tud_task()`), đọc ra
là logic thuần, giống nhau dù giao thức vật lý là gì.

---

### Layer 2 — PLC Core (ranh giới port)

#### 2.1 `plc_tag.h` / `plc_tag.c`

```c
typedef enum {
    TAG_NONE = 0, TAG_DI, TAG_DO, TAG_AI, TAG_VFLAG, TAG_VREG,
    TAG_MB_COIL, TAG_MB_HOLDING, TAG_VREG_RETAIN,
} TagKind;

typedef struct {
    uint8_t  kind;
    uint8_t  channel;
    uint16_t reg_addr;
} Tag;   // 4 byte/tag

#define MAX_TAGS 128
extern Tag     g_tag_table[MAX_TAGS];
extern int32_t g_tag_value[MAX_TAGS];

void     tag_table_load_from_flash(void);
int32_t  tag_read(uint16_t idx);
void     tag_write(uint16_t idx, int32_t value);
TagKind  tag_get_kind(uint16_t idx);
```

Tag Table cố định lúc dev viết code, KHÔNG cấu hình lại qua Modbus. Index 0 =
`TAG_NONE` (sentinel "không tham chiếu", dùng làm default cho `guard_tag`).
Phân bổ: 1-8=DI, 9-16=DO, 17-20=AI, 21-36=VFLAG, 37-52=VREG, 53-68=VREG_R,
69-127 dự trù Gateway.

#### 2.2 `plc_rule.h` / `plc_rule.c` — struct Rule chính thức 32 byte

**QUAN TRỌNG — sửa so với các bản trước:** kích thước Rule chính thức là
**32 byte** (`SPLC_RuleRecord`, theo v1.7), KHÔNG phải 28 byte như spec gốc/
bản cũ từng ghi.

```c
typedef enum { TRG_ON_CHANGE=0, TRG_ON_RISE, TRG_ON_FALL, TRG_TIME_WINDOW, TRG_INTERVAL } TriggerType;
typedef enum { OP_NONE=0, OP_EQ, OP_NEQ, OP_GT, OP_LT, OP_GTE, OP_LTE, OP_BETWEEN } CompareOp;
typedef enum { ACT_SET_TAG=0, ACT_TOGGLE_TAG, ACT_INC_COUNTER, ACT_WRITE_REMOTE,
               ACT_LOG_EVENT, ACT_SEND_ALARM, ACT_ADD_TAG, ACT_SCALE_TAG } ActionType;

/*
 * THỨ TỰ FIELD BẮT BUỘC để sizeof(Rule)==32, không padding lãng phí.
 * Verify bằng compile thật: xen kẽ field 1-byte/2-byte trước field 4-byte
 * sẽ đội sizeof lên 40 byte. Quy tắc: 4-byte truoc, roi 2-byte, roi 1-byte,
 * reserved cuoi cung.
 */
typedef struct {
    int32_t  threshold_lo;
    int32_t  threshold_hi;
    uint32_t for_ms;
    int32_t  action_param;

    uint16_t trigger_tag;
    uint16_t action_tag;
    uint16_t guard_tag;      // bit 0-14: idx (TAG_NONE=0 = khong co guard); bit 15: NEGATE

    uint8_t  enabled;
    uint8_t  trigger_type;
    uint8_t  compare_op;
    uint8_t  action_type;

    uint8_t  reserved[6];    // Sender PHAI ghi 0 — de CRC-16 deterministic
} Rule;   // 32 byte CHINH THUC = 16 Modbus register

/*
 * KHONG co field rule_id — v1.7: "V1 khong luu rule_id ben trong
 * SPLC_RuleRecord". Dinh danh duy nhat la vi tri (RuleIndex, zero-based).
 */

typedef struct {
    RuleExecState state;
    int32_t  prev_value;
    uint32_t dwell_start_tick;
    uint32_t last_fire_tick;
} RuleRuntime;   // KHONG luu Flash

#define MAX_RULES 100
extern Rule        g_rule_table[MAX_RULES];
extern RuleRuntime g_rule_runtime[MAX_RULES];
extern int         g_rule_count;

void rule_table_load_from_flash(void);
void rule_scan(void);
bool rule_table_commit(const uint8_t *raw_data, uint16_t rule_count);
```

**Vị trí Flash lưu Rule Table: CHƯA ĐƯỢC ĐẶC TẢ CỤ THỂ** (xem mục 10) —
khoảng trống kiến trúc thật sự.

#### 2.2b `plc_rule_state_machine.h` / `.c` — State Machine #1 (đã kiểm chứng)

**Quyết định:** `rule_scan()` implement bằng state machine tường minh
(switch/case theo `RuleExecState`), thay cho `if...continue` tuyến tính.

```c
typedef enum {
    RULE_STATE_IDLE = 0, RULE_STATE_TRIGGERED, RULE_STATE_COMPARED,
    RULE_STATE_DWELLING,   // TRẠNG THÁI DUY NHẤT sống qua nhiều vòng quét
    RULE_STATE_GUARD_CHECK, RULE_STATE_FIRE, RULE_STATE_BLOCKED,
} RuleExecState;

#define DWELL_NOT_STARTED 0xFFFFFFFFu

bool rule_state_machine_step(Rule *rule, RuleRuntime *rt, uint32_t now_ms);
```

5 trạng thái `IDLE->TRIGGERED->COMPARED->GUARD_CHECK->FIRE` giải quyết xong
TRONG CÙNG 1 LẦN GỌI HÀM (fallthrough có chủ đích). Chỉ `DWELLING` thực sự
"đứng lại" qua nhiều lần gọi.

`NEXT_RULE` KHÔNG có trong enum thật — chỉ nên xuất hiện trên lưu đồ vẽ tay,
vì nó mô tả hành vi vòng lặp `for` bên ngoài, không phải trạng thái 1 Rule.

**Lịch sử debug (bài học, không xoá):** bản đầu tiên có 3 bug bị bắt bằng
test thực nghiệm, không phải đọc code:
1. 1 dấu `break` sai vị trí phá chuỗi fallthrough.
2. "Mức còn giữ" trong `DWELLING` đoản mạch về true khi `compare_op==OP_NONE`
   — không phát hiện được tín hiệu gốc đổi ngược giữa lúc dwell. Sửa: suy
   trực tiếp từ `current` theo hướng `trigger_type`.
3. `prev_value` chỉ cập nhật ở case `IDLE/BLOCKED`, không cập nhật ở
   `DWELLING` — mất edge mới ngay sau khi dwell bị huỷ.

Đã sửa và verify bằng 2 kịch bản (dwell đạt liên tục; dwell gián đoạn giữa
chừng rồi đếm lại từ đầu) — cả 2 PASS.

**Bài học:** `switch/case + fallthrough` KHÔNG tự nhiên an toàn hơn
`if/continue` — bản đầu có nhiều bug tinh vi hơn. Lý do chọn state machine
vẫn đúng (tường minh, dễ debug), nhưng phải test thực nghiệm kỹ.

#### 2.2c Enum kết quả kiểm tra (status) — khác State, dễ nhầm

```
trigger_ok / trigger_fail
compare_ok / compare_fail
dwell_ok / dwell_waiting / dwell_interrupted   <- Dwell co 3 ket qua
guard_check_ok / guard_check_fail
```

`dwell_waiting` (chưa đủ giờ, còn giữ) khác `dwell_interrupted` (bị gián
đoạn, huỷ hẳn, đếm lại từ đầu) — điểm dễ nhầm nhất khi tự vẽ lại state
machine.

#### 2.3 `plc_rule_eval.c`

```c
bool check_trigger_edge(TriggerType type, int32_t prev, int32_t current);
bool trigger_timing_ok(TriggerType type, uint32_t now_ms, uint32_t now_hhmm,
                        int32_t threshold_lo, int32_t threshold_hi,
                        uint32_t for_ms, uint32_t last_fire_tick);
bool compare_ok(CompareOp op, int32_t current, int32_t lo, int32_t hi);
```

`TRG_TIME_WINDOW` cần nguồn RTC (giờ:phút thật), KHÔNG phải tick uptime —
vẫn là khoảng trống (mục 10).

#### 2.4 `plc_rule_action.c`

```c
void execute_action(Rule *r);
```

5/8 action chỉ thao tác `g_tag_value[]`: `ACT_SET_TAG`, `ACT_TOGGLE_TAG`,
`ACT_INC_COUNTER`, `ACT_ADD_TAG` (tách rule riêng mỗi nguồn cộng dồn),
`ACT_SCALE_TAG` (dùng cả `action_param` lẫn `threshold_hi`).

**3/8 action vẫn treo, cần thành phần chưa tồn tại:**
- `ACT_WRITE_REMOTE` — cần `plc_modbus_master.c` (Modbus Master, chưa có),
  chạy TRƯỚC `rule_scan()` trong Layer 4. Chỉ cho Gateway.
- `ACT_LOG_EVENT` — Event Log buffer/format chưa thiết kế.
- `ACT_SEND_ALARM` — cơ chế Alarm chưa thiết kế.

#### 2.5 `plc_device.h` / `plc_device.c` — MỚI, theo v1.7

```c
typedef enum {
    SPLC_DEVICE_CLASS_UNKNOWN = 0, SPLC_DEVICE_CLASS_REMOTE_IO = 1,
    SPLC_DEVICE_CLASS_DATALOGGER = 2, SPLC_DEVICE_CLASS_GATEWAY = 3,
    SPLC_DEVICE_CLASS_CONTROLLER = 4,
} SPLC_DeviceClass;

typedef struct {
    uint16_t device_class, device_variant;
    uint16_t hw_version_major, hw_version_minor, hw_version_patch;
    uint16_t fw_version_major, fw_version_minor, fw_version_patch;
    uint16_t protocol_version, rule_format_version;
} SPLC_DeviceDescriptor;   // 20 byte — RO

typedef enum {
    SPLC_HEALTH_NONE = 0,
    SPLC_HEALTH_CPU_HIGH      = 1u << 0,
    SPLC_HEALTH_RAM_HIGH      = 1u << 1,
    SPLC_HEALTH_SCAN_OVERRUN  = 1u << 2,
} SPLC_HealthFlags;

typedef struct {
    uint32_t uptime_s;
    uint16_t reset_reason, health_flags;
    uint16_t cpu_load_percent, ram_usage_percent;
    uint32_t scan_time_us, max_scan_time_us;
} SPLC_DeviceHealth;   // 20 byte — RO

extern SPLC_DeviceDescriptor g_device_descriptor;  // hằng số biên dịch
extern SPLC_DeviceHealth     g_device_health;      // cập nhật liên tục
```

Thay thế đề xuất `DeviceModel`/`DeviceRuntime` tự nghĩ ban đầu — v1.7 chi
tiết hơn. `uptime_s`/`scan_time_us` do Layer 4 ghi; `cpu_load_percent`/
`ram_usage_percent` do Layer 3 tính.

---

### Layer 3.5 — Protocol / Library Porting (`port/`)

#### 2.4b `port/modbus_usb/` (đổi tên từ `modbus_serial`)

```c
int32_t modbus_usb_read(uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg);
int32_t modbus_usb_write(const uint8_t *buf, uint16_t count, int32_t timeout_ms, void *arg);
```

Gọi xuống `sx_usb_tiny_read/write` (Layer 1) thay vì `sx_uart_read/write`.

`port/modbus_serial` (UART/RS485) VẪN GIỮ — cần cho Gateway variant (Modbus
Master qua RS485). 2 lớp porting độc lập, không include lẫn nhau.

```
port/
├── modbus_usb/       (MỚI — kênh App-MCU chính thức)
├── modbus_serial/     (GIỮ — Gateway, Modbus Master qua RS485)
├── mqtt_transport/    (dự trù)
└── canopen_transport/ (dự trù)
```

---

### Layer 3 — PLC Application Services

#### 3.1 `plc_io.h` / `plc_io.c` — không đổi cấu trúc, xem đề xuất mục 10

#### 3.2 `plc_retain.h` / `plc_retain.c` — không đổi. Chỉ cho `TAG_VREG_RETAIN`,
KHÔNG dùng chung với Rule Table.

#### 3.3 `plc_modbus_cfg.h` / `plc_modbus_cfg.c` — State Machine #2

Dùng nanoMODBUS, platform conf trỏ `modbus_usb_read/write` thay vì
`modbus_serial_read/write`.

```c
typedef enum {
    CFG_STATE_IDLE = 0, CFG_STATE_RECEIVING = 1,
    CFG_STATE_VERIFYING = 2,   // DOI TEN so voi ban truoc ("checking CRC")
    CFG_STATE_READY = 3, CFG_STATE_ERROR = 4,
} ConfigStatus;
```

CRC dùng CRC-16/MODBUS trên `rule_count * 32` byte — KHÔNG phải CRC32.

---

### Layer 4 — Engine & Application entry

```c
void plc_engine_init(void) {
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    plc_io_init();
    retain_store_restore();
    plc_modbus_cfg_init();   // ben trong gio dung sx_usb_tiny
    pvd_init();
    watchdog_init();
}

void plc_engine_scan_once(void) {
    input_scan();
    // TODO (Gateway, chua trien khai): modbus_master_poll() PHAI o day,
    // TRUOC rule_scan()
    rule_scan();
    output_scan();
    modbus_config_service();
    retain_service();
    watchdog_kick();
}
```

---

## 2.6 Modbus Register Map (V1) — CHÍNH THỨC, theo v1.7

Đây là bảng địa chỉ CHÍNH THỨC, ưu tiên tuyệt đối so với mọi con số minh hoạ
ở bản trước. 1 register = 16 bit. Rule = 32 byte = 16 register.

### 2.6.1 Core / Runtime

| Address | R/W | Block | Length (reg) | Ý nghĩa |
|---|---|---|---|---|
| 0x0000-0x0009 | RO | DEVICE_DESCRIPTOR | 10 | `SPLC_DeviceDescriptor` |
| 0x0010 | RO | RULE_TABLE_INFO | 1 | Số rule active |
| 0x0100-0x073F | RO | ACTIVE_RULE_TABLE | 1600 max | `SPLC_RuleRecord[100]` — App đọc lại toàn bộ Rule Table |
| 0x0800-0x0809 | RO | DEVICE_HEALTH | 10 | `SPLC_DeviceHealth` |
| 0x0900-0x09FF | RO | RUNTIME_TAG_VALUES | 256 max | `int32_t[128]`, **2 register/tag** (kể cả DI/DO — không dùng Coil ở đâu cả) |
| 0x0A00 | WO | SYSTEM_COMMAND | 1 | Xem 2.6.4 |
| 0x0A01-0x0A02 | RO | SYSTEM_COMMAND_RESULT | 2 | Xem 2.6.4 |

### 2.6.2 Rule transfer / commit

| Address | R/W | Block | Length (reg) | Ý nghĩa |
|---|---|---|---|---|
| 0x9000 | RO | CONFIG_STATUS | 1 | 0=IDLE,1=RECEIVING,2=VERIFYING,3=READY,4=ERROR |
| 0x9001 | RO | CONFIG_ERROR_CODE | 1 | `SPLC_ErrorCode` (2.6.5) |
| 0x9002 | RW | RULE_COUNT_STAGED | 1 | |
| 0x9003 | RW | EXPECTED_CRC16 | 1 | **1 register, không phải CRC32 2-register** |
| 0x9004 | RO | ACTIVE_RULE_COUNT | 1 | |
| 0x9005 | RO | ACTIVE_RULE_CRC16 | 1 | |
| 0x9006-0x900F | - | RESERVED | 10 | |
| 0x9010-0x964F | RW | STAGING_RULE_TABLE | 1600 max | |
| 0xA000 | WO | COMMIT_COMMAND | 1 | Ghi 0xA5A5 |
| 0xA001 | RO | ACTIVE_RULE_VERSION | 1 | |

**Công thức địa chỉ:** `ActiveRule[i] = 0x0100 + i*16`, `StagingRule[i] = 0x9010 + i*16`.

**5 bước:** (1) App ghi RULE_COUNT_STAGED+EXPECTED_CRC16 TRƯỚC, (2) ghi
STAGING_RULE_TABLE (nhiều khung), (3) đọc lại CONFIG_STATUS xác nhận đủ,
(4) ghi COMMIT_COMMAND=0xA5A5, (5) đọc ACTIVE_RULE_VERSION xác nhận.

### 2.6.3 Register encoding

32-bit -> 2 register, **High Word trước, Low Word sau**. 2x uint8_t -> 1
register, High byte trước. `reserved[]` — sender ghi 0.

### 2.6.4 System Commands — MỚI

```c
typedef enum {
    SPLC_SYSTEM_CMD_NONE = 0, SPLC_SYSTEM_CMD_REBOOT = 1,
    SPLC_SYSTEM_CMD_FACTORY_RESET = 2, SPLC_SYSTEM_CMD_CLEAR_RULES = 3,
    SPLC_SYSTEM_CMD_CLEAR_RETAIN = 4,
} SPLC_SystemCommand;
```

CHƯA có implementation Layer 3 tương ứng (`plc_system_cmd.c`?).

### 2.6.5 Common Error Codes — chính thức

```c
typedef enum {
    SPLC_ERROR_NONE = 0, SPLC_ERROR_INVALID_COMMAND = 1,
    SPLC_ERROR_INVALID_PARAMETER = 2, SPLC_ERROR_BUSY = 3,
    SPLC_ERROR_CRC_MISMATCH = 4, SPLC_ERROR_UNSUPPORTED = 5,
    SPLC_ERROR_FLASH = 6,
} SPLC_ErrorCode;
```

Thay thế hoàn toàn danh sách mã lỗi tự đề xuất ở bản trước.

---

## 3. Bảng tổng hợp — Ai được include ai

| File | Được include bởi |
|---|---|
| `plc_tag.h` | Layer 3, Layer 2 nội bộ |
| `plc_rule.h` | `plc_modbus_cfg.c`, Layer 4 |
| `plc_rule_eval.h`, `plc_rule_action.h`, `plc_rule_state_machine.h` | Chỉ `plc_rule.c` |
| `plc_device.h` | `plc_modbus_cfg.c`, Layer 4 |
| `plc_io.h`, `plc_retain.h`, `plc_modbus_cfg.h` | Chỉ Layer 4 |
| `modbus_usb.h` (3.5) | Chỉ `plc_modbus_cfg.c` |
| `modbus_serial.h` (3.5, giữ cho Gateway) | Chỉ `plc_modbus_master.c` (chưa tồn tại) |
| `sx_usb_tiny.h` (Layer 1) | `port/modbus_usb` |
| `sx_uart.h` (Layer 1, giữ cho Gateway) | `port/modbus_serial` |

---

## 4. Ví dụ luồng dữ liệu — CẬP NHẬT theo USB + state machine

**Tình huống:** App gửi Rule qua USB: "khi DI1 vừa rise thì bật DO2".

### 4.1 Nạp Rule qua Modbus-over-USB

```
[App] --USB (Modbus RTU / USB-CDC)--> [MCU]
Layer 0   tud_task() xu ly USB stack, byte CDC day vao cqueue
Layer 1   sx_usb_tiny_read() lay byte ra
Layer 3.5 modbus_usb_read() cap byte tho cho nanoMODBUS
Layer 3   nanoMODBUS parse khung, goi callback -> ghi staging, verify CRC16
Layer 2   rule_table_commit() atomic-swap vao g_rule_table[]
```

### 4.2 Vòng quét (mỗi 10ms) — dùng state machine

```
input_scan(): DI1 chan that HIGH -> tag_write(tag_DI1, 1)
rule_scan():
    state=IDLE: check_trigger_edge(ON_RISE,0,1)=true
    -> fallthrough TRIGGERED (compare_op=NONE, dat)
    -> fallthrough COMPARED (for_ms=0, khong can dwell)
    -> fallthrough GUARD_CHECK (guard=TAG_NONE, mo)
    -> fallthrough FIRE: execute_action() -> tag_write(tag_DO2, 1)
    state ve IDLE
output_scan(): doc tag_DO2=1 -> sx_gpio_write(pin_DO2,1) -> den sang that
```

Toàn bộ xảy ra gọn trong 1 vòng quét vì không có Dwell — nếu `for_ms>0`,
dừng ở `DWELLING`, cần nhiều vòng quét mới tới `FIRE`.

---

## 5. Các mảng dữ liệu

| Mảng | Chứa gì | Ai ghi | Ai đọc |
|---|---|---|---|
| `g_tag_value[128]` | Giá trị sống | `input_scan`; `rule_scan`/Modbus | `rule_scan`, `output_scan`, Modbus |
| `g_tag_table[128]` | Ý nghĩa tag | Boot 1 lần | `tag_get_kind()` |
| `g_rule_table[100]` | Rule tĩnh (32B/rule) | `rule_table_commit()` | `rule_scan` |
| `g_rule_runtime[100]` | Trạng thái động | `rule_state_machine_step()` | Chính nó, vòng sau |
| `g_device_descriptor` | Nhận dạng thiết bị | Hằng số biên dịch | Modbus |
| `g_device_health` | Sức khoẻ runtime | Layer 3/4 | Modbus |

---

## 6. Mô hình "kéo" (poll) vs "đẩy" (push)

Không đổi — `tag_write()` ghi xong là dừng, không tự lan truyền. Phải có 1
bên khác chủ động đọc lại theo lịch cố định.

---

## 7. Tag Index — `#define` số cố định (Cách A), không dùng `enum` tự đánh số

`guard_tag = 0` (`TAG_NONE`) là sentinel chính thức "không guard" — đã xác
nhận từ spec gốc, KHÔNG phải `0xFFFF`.

---

## 8. Checklist khi code

- [ ] `sizeof(Rule)` PHẢI = 32 — verify bằng compile thật mỗi khi sửa thứ tự
      field.
- [ ] Mọi CRC dùng CRC-16/MODBUS, không dùng CRC32 ở đâu cả.
- [ ] `reserved[6]` trong `Rule` phải được App ghi 0 trước khi tính CRC16.
- [ ] KHÔNG thêm `rule_id` vào `Rule` — dùng RuleIndex (vị trí).
- [ ] Cặp mảng song song (`g_do_tag_map[]`/`g_do_gpio_pin[]`) phải khớp index.
- [ ] `idx` trong mảng ánh xạ Layer 3 phải khớp `g_tag_table[idx].kind`.
- [ ] Layer 2 KHÔNG include Layer 0/1.
- [ ] Truy cập `g_tag_value[]` từ Layer 3+ PHẢI qua `tag_read()`/`tag_write()`.
- [ ] File Layer 3 KHÔNG gọi lẫn nhau trực tiếp.
- [ ] `ACT_ADD_TAG`: tách rule riêng mỗi nguồn cộng dồn.
- [ ] `RUNTIME_TAG_VALUES` dùng đúng 2 register/tag cho MỌI loại tag — không
      dùng Coil (FC01/05) ở đâu.
- [ ] `modbus_master_poll()` (khi có, cho Gateway) PHẢI chạy TRƯỚC `rule_scan()`.
- [ ] `DWELLING`: kiểm tra "mức còn giữ" phải suy từ `current` theo hướng
      `trigger_type`, KHÔNG chỉ dựa `compare_ok()` (sai khi `OP_NONE`).

---

## 9. Câu hỏi/hiểu nhầm đã xử lý — tham khảo nhanh

| Hiểu nhầm | Thực tế đúng |
|---|---|
| App-MCU qua RS485 | **USB** (Modbus RTU / USB-CDC / TinyUSB); RS485 chỉ cho Gateway variant (Modbus Master) |
| Rule 28 byte | **32 byte chính thức**, có `reserved[6]` |
| CRC32 verify Rule Transfer | **CRC-16/MODBUS**, 1 register |
| Cần `rule_id` | v1.7: KHÔNG cần — dùng RuleIndex (vị trí) |
| switch/case + state machine luôn an toàn hơn if/continue | Không tự nhiên đúng — bản đầu có 3 bug tinh vi hơn, phải test thực nghiệm |
| Dwell chỉ 2 kết quả | Có 3: `dwell_ok`, `dwell_waiting`, `dwell_interrupted` |
| DI/DO nên dùng Coil (FC01/05) | v1.7: dùng Holding Register (FC03/16) cho MỌI tag |
| Cần API "Load Rule from Device" riêng | Không cần — `ACTIVE_RULE_TABLE` (0x0100) đã cho đọc lại toàn bộ |
| "Layer 2 nên tự gọi tiếp Layer 3 khi có tag_write" | `tag_write` chỉ ghi RAM rồi dừng — mô hình kéo |
| "rule_scan ghi kết quả vào g_do_tag_map" | `g_do_tag_map` chỉ là bảng tra cứu; `rule_scan` chỉ ghi `g_tag_value[]` qua `tag_write()` |
| "g_tag_value có mảng riêng từng loại DI/DO/AI" | Chỉ 1 mảng `g_tag_value[128]` DUY NHẤT dùng chung |
| "rule_scan cần biết tag là DI hay DO" | KHÔNG cần — chỉ thực thi theo idx định sẵn trong `Rule` |

---

## 10. Việc còn để ngỏ / có thể làm tiếp

- [ ] **Vị trí Flash lưu Active Rule Table CHƯA đặc tả** — địa chỉ, kích
      thước (tối thiểu `100*32=3200 byte`), có cần wear-leveling như
      `plc_retain.c` hay ghi đè 1 chỗ cố định là đủ.
- [ ] **Modbus Master cho Gateway — `plc_modbus_master.c` CHƯA TỒN TẠI.**
      Cần quyết định RTU thôi hay cả TCP.
- [ ] **Event Log (`ACT_LOG_EVENT`) — buffer/format CHƯA THIẾT KẾ.**
- [ ] **Alarm (`ACT_SEND_ALARM`) — cơ chế CHƯA THIẾT KẾ** (mức độ nghiêm
      trọng? cơ chế ACK?).
- [ ] **Nguồn RTC cho `TRG_TIME_WINDOW` chưa quyết định.**
- [ ] **`SYSTEM_COMMAND` — chưa có implementation Layer 3.**
- [ ] **API đăng ký kênh cho `plc_io.c`** (`plc_io_register_di/do/ai`) — đề
      xuất thay thế 2 mảng song song dễ lệch index — chưa triển khai.
- [ ] **Validate `guard_tag`/`trigger_tag`/`action_tag` trong biên
      `MAX_TAGS`** trước khi `rule_table_commit()` — chưa có.
- [ ] Soạn `plc_tag_def.h` đầy đủ 69 tag — chưa làm.
- [ ] Port `sx_usb_tiny_cdc.c/.h` và `logger.c/.h` từ `WS_v1` vào đúng cấu
      trúc layer `simple_plc` (bỏ `sx_malloc`, bỏ mutex FreeRTOS) — đã lên kế
      hoạch, CHƯA thực hiện.
- [ ] Xác nhận tốc độ poll App cần cho `RUNTIME_TAG_VALUES` — 2 register/tag
      cho mọi tag có thể là nút thắt cổ chai với RTU baudrate thấp.
- [ ] Xác nhận RAM đủ cho `ACTIVE_RULE_TABLE`+`STAGING_RULE_TABLE` (tổng
      6400 byte) với biến thể STM32H523 cụ thể.

---

*File này được biên soạn lại từ toàn bộ nội dung hỏi-đáp giữa người dùng và
Claude qua nhiều phiên làm việc, đồng bộ theo tài liệu chính thức
`SimplePLC_App_MCU_Structs_v1.7.docx`.*