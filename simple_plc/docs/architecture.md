# SimplePLC — Ghi chú kiến trúc Firmware (đầy đủ, dùng để tham chiếu lại)

> File này tổng hợp toàn bộ nội dung đã bàn bạc về kiến trúc phân lớp (layered
> architecture) của SimplePLC — dùng để:
> - Tiếp tục vibe-code dựa trên khung đã chốt.
> - Đưa cho 1 Claude/AI khác đọc để nắm ngữ cảnh mà không cần giải thích lại từ đầu.
> - Tra cứu lại khi quên 1 khái niệm nào đó.
>
> **Cập nhật lần này:** đồng bộ lại toàn bộ nội dung theo tài liệu chính thức
> mới nhất `SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md` (data
> contract + Modbus register map V1, bản 1.9, update của v1.7) — tài liệu
> này là **nguồn sự thật cao nhất hiện có**, ưu tiên hơn mọi con số/quyết
> định cũ trong các bản trước của file này, kể cả v1.7. Những chỗ khác biệt
> so với v1.7 đều được đánh dấu rõ bằng nhãn **"V1.9:"**. Những đoạn nào
> trong bản v1.7 vẫn còn đúng (không đổi giữa 2 bản) thì giữ nguyên, không
> lặp lại nhãn V1.9.
>
> **Cũng đã đồng bộ 1 bugfix thật đã áp dụng vào code** (`plc_rule.c`/
> `plc_rule.h`, xem mục 2.2 và mục 9): sentinel "không có guard" trong
> `guard_tag` đổi từ `0` sang `GUARD_TAG_NONE` (`0x7FFF`) — xem mục 2.2 để
> biết lý do đầy đủ.

---

## 0. Bối cảnh dự án

SimplePLC là firmware lõi cho dòng Remote I/O 8DI/8DO/4AI, chạy trên
STM32H523CCU6 (Cortex-M33, 250MHz, 256KB Flash / 272KB SRAM). Đây là bản đầu
tiên của 1 nền tảng Rule Engine dùng chung cho cả họ sản phẩm IIoT sau này
(Datalogger, Gateway, Controller...), nên nguyên tắc **"không heap động,
struct nén cố định, tách lớp rạch ròi"** là bắt buộc để port sang MCU khác
sau này (kể cả MCU nhỏ hơn như STM32G0/F1).

**Nguồn tài liệu, theo đúng thứ tự ưu tiên (mới hơn ghi đè cũ hơn):**
1. `SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md` — **data
   contract + Modbus register map V1 chính thức, bản 1.9 (update của
   v1.7)**, quy định wire format thật giữa App và MCU. Đây là bản quan
   trọng nhất, mọi con số địa chỉ/kích thước struct phải khớp đúng file
   này. Thay đổi chính so với v1.7: bỏ hẳn `CapabilityFlags`, thêm block
   `SPLC_DeviceResourceInfo`/`SPLC_WireProfile` để App tự dựng
   `ProductDefinition`/`TagCatalog` runtime thay vì lookup cứng theo
   `device_variant`; tag layout cố định lại (DI=8, DO=8, AI=4, VFLAG=32,
   VREG=32, VREG_RETAIN=32, COUNTER=8 — **không còn ô sentinel `TAG_NONE`
   ở index 0** như v1.7); thêm `SPLC_TagKind` mới `COUNTER=9`;
   `SPLC_DeviceHealth.scan_time_us/max_scan_time_us` đổi tên thành
   `scan_time_ms/max_scan_time_ms` (wire size không đổi, xác nhận đơn vị
   là **ms**). Xem mục 1, 2.1, 2.5, 2.6 để biết chi tiết từng phần.
2. `SimplePLC_App_MCU_Structs_v1.7.md` — vẫn giữ làm tài liệu tham chiếu
   lịch sử (để hiểu vì sao 1 số quyết định cũ trong code/comment nhắc tới
   "v1.7"), nhưng **bất kỳ chỗ nào v1.9 nói khác thì v1.9 thắng**. Không
   dùng v1.7 để tra số liệu mới.
3. `SimplePLC_RuleStruct_MCU_Spec_v0_1.md` — tài liệu spec gốc, vẫn đúng cho
   phần **logic nghiệp vụ Rule Engine** (5 bước Trigger→Compare→Dwell→Guard→
   Action, ý nghĩa từng ActionType/TriggerType/CompareOp) — riêng **kích
   thước struct và CRC đã bị v1.7/v1.9 ghi đè** (xem mục 2.2), và riêng
   **cách encode sentinel "không có guard" trong `guard_tag` đã bị 1 bugfix
   thật ghi đè thêm lần nữa** (xem mục 2.2, không phải `0` như spec gốc/v1.7
   mà là `GUARD_TAG_NONE = 0x7FFF`).
4. File này (`architecture.md`) — diễn giải kiến trúc layer, không tự đặt ra
   con số nào mâu thuẫn với các nguồn trên.

**Transport vật lý giữa App và MCU: USB (KHÔNG phải RS485/UART).** Đây là
điểm quan trọng đã đổi so với giả định ban đầu trong dự án — v1.7/v1.9 đều
ghi rõ *"USB là transport vật lý; NanoModbus xử lý Modbus RTU"*. RS485 chỉ
còn xuất hiện ở 1 chỗ khác hẳn: 1 **variant của sản phẩm Gateway**
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
/*
 * V1.9: numeric value chuan hoa theo
 * docs/SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md muc 1.2 --
 * pin cung tung gia tri, KHONG dung enum tu tang. TAG_COUNTER=9 la them
 * moi so voi v1.7 (v1.7 chi co 0..8).
 */
typedef enum {
    TAG_NONE = 0, TAG_DI, TAG_DO, TAG_AI, TAG_VFLAG, TAG_VREG,
    TAG_MB_COIL, TAG_MB_HOLDING, TAG_VREG_RETAIN, TAG_COUNTER,
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

Tag Table cố định lúc dev viết code, KHÔNG cấu hình lại qua Modbus.

**V1.9 -- THAY ĐỔI QUAN TRỌNG so với v1.7:** không còn ô sentinel `TAG_NONE`
chiếm index 0 nữa. Layout mới (per
`SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md` mục 5.1, "FIXED
TAG LAYOUT — WIRE PROFILE V1"): **0-7=DI, 8-15=DO, 16-19=AI, 20-51=VFLAG (32
slot, không phải 16 như v1.7), 52-83=VREG (32 slot), 84-115=VREG_RETAIN (32
slot), 116-123=COUNTER (8 slot, hoàn toàn mới so với v1.7), 124-127
reserved** (dự trù Gateway remote Modbus tag). Remote I/O SKU hiện tại dùng
124/128 slot. `TAG_DI0` giờ nằm ở index 0 — **là 1 tag thật, không còn là ô
trống**. Điều này có hệ quả trực tiếp tới cách encode `guard_tag` trong Rule
— xem mục 2.2 ngay dưới đây, đã có 1 bug thật liên quan tới đúng điểm này,
đã sửa.

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
    uint16_t guard_tag;      // bit 0-14: idx; bit 15: NEGATE. Sentinel
                              // "khong co guard" = GUARD_TAG_NONE (0x7FFF),
                              // KHONG PHAI 0 -- xem canh bao ngay duoi day.

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

**BUGFIX ĐÃ ÁP DỤNG — sentinel "không có guard" trong `guard_tag`.**

```c
#define GUARD_TAG_NEGATE_BIT   0x8000u
#define GUARD_TAG_INDEX_MASK   0x7FFFu
#define GUARD_TAG_NONE         0x7FFFu   // MOI — thay cho 0
```

**Vấn đề:** Trước bugfix này, `RULE_STATE_GUARD_CHECK` so sánh
`guard_idx == TAG_NONE` (tức `== 0`) để quyết định "rule này không có
guard". Điều đó đúng dưới layout v1.7 (index 0 là ô sentinel vô nghĩa),
nhưng sai dưới layout v1.9 (mục 2.1 ở trên) — index 0 giờ là `TAG_DI0`, một
tag thật. Hệ quả: **`TAG_DI0` là tag duy nhất trong toàn hệ thống không thể
dùng làm `guard_tag`** — bất kỳ rule nào set `guard_tag = TAG_DI0` (= 0) với
ý định dùng DI0 làm điều kiện guard sẽ bị hiểu nhầm thành "không có guard"
và luôn cho fire, bất kể giá trị thật của DI0.

**Đã verify bằng compile + chạy thật** (không chỉ đọc code): 1 rule với
`guard_tag = TAG_DI0`, DI0 giữ ở mức 0 — trước fix, rule vẫn fire (guard bị
bỏ qua); sau fix, rule bị guard chặn đúng như kỳ vọng.

**Cách sửa:** đổi sentinel "không có guard" từ `0` sang `GUARD_TAG_NONE`
(`0x7FFF`) — giá trị này an toàn tuyệt đối vì `GUARD_TAG_INDEX_MASK` chỉ 15
bit, còn `MAX_TAGS=128` (index 0..127), nên không tag hợp lệ nào có thể
trùng `0x7FFF`. Code cấu hình rule (App/UI) từ giờ phải dùng
`GUARD_TAG_NONE`, không phải `0`, để nói "rule này không cần guard".

**Việc còn để ngỏ (chưa tự ý sửa):** `rule_table_commit()` hiện chưa
validate `guard_tag` — nếu App gửi 1 giá trị index nằm trong khoảng
128..32766 (không phải tag hợp lệ, cũng không phải `GUARD_TAG_NONE`),
`tag_read()` sẽ tự chặn (trả về 0 vì `idx >= MAX_TAGS`) nên không crash,
nhưng rule đó sẽ luôn bị coi guard "đóng" một cách âm thầm, không báo lỗi.
Việc validate input từ Modbus thuộc phạm vi `plc_modbus_cfg.c` (Layer 3,
chưa viết) — xem mục 10.

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

#### 2.5 `plc_device.h` — MỚI so với bản gốc, theo v1.7/v1.9 (chỉ header, không có `.c`)

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
} SPLC_DeviceDescriptor;   // 20 byte — RO, KHONG DOI giua v1.7 va v1.9

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
    uint32_t scan_time_ms, max_scan_time_ms;   // V1.9: doi ten tu *_us
} SPLC_DeviceHealth;   // 20 byte — RO, wire size KHONG DOI

/*
 * V1.9 -- MOI HOAN TOAN, khong ton tai trong v1.7. App doc block nay ngay
 * sau SPLC_DeviceDescriptor va tu dung ProductDefinition/TagCatalog runtime,
 * khong lookup cung theo device_variant nua. Thay the hoan toan cach tiep
 * can CapabilityFlags (da bi bo trong v1.9).
 */
typedef enum {
    SPLC_WIRE_PROFILE_UNKNOWN = 0,
    SPLC_WIRE_PROFILE_V1      = 1,
} SPLC_WireProfile;

typedef struct {
    uint16_t wire_profile;          // SPLC_WireProfile; V1 = 1
    uint16_t max_rules;             // 0..100; >0 => co Rule Engine
    uint16_t runtime_tag_count;     // Tong tag hop le; KHONG lien tuc

    uint16_t di_count;              // 0..8
    uint16_t do_count;              // 0..8
    uint16_t ai_count;              // 0..4
    uint16_t vflag_count;           // 0..32
    uint16_t vreg_count;            // 0..32
    uint16_t vreg_retain_count;     // 0..32; >0 => co Retentive Memory
    uint16_t counter_count;         // 0..8
} SPLC_DeviceResourceInfo;          // 20 byte — RO, MOI trong V1.9

extern SPLC_DeviceDescriptor    g_device_descriptor;   // hằng số biên dịch
extern SPLC_DeviceHealth        g_device_health;       // cập nhật liên tục
extern SPLC_DeviceResourceInfo  g_device_resource_info; // hằng số biên dịch
```

Thay thế đề xuất `DeviceModel`/`DeviceRuntime` tự nghĩ ban đầu — v1.7 chi
tiết hơn, v1.9 thêm hẳn 1 block mới (`SPLC_DeviceResourceInfo`). `uptime_s`/
`scan_time_ms` do Layer 4 ghi; `cpu_load_percent`/`ram_usage_percent` do
Layer 3 tính. Rule Engine được suy ra từ `max_rules > 0`; Retentive Memory
suy ra từ `vreg_retain_count > 0` — **không dùng bitmask riêng cho các tính
năng này nữa** (khác v1.7-style CapabilityFlags từng được đề xuất, đã bị bỏ
hẳn ở v1.9). Runtime Tags/Device Health/System Commands mặc định luôn có
sẵn với mọi thiết bị dùng Wire Profile V1, không cần khai báo lại.

**Quyết định "chỉ header, không có `.c`" cho `plc_device.h`:** xem
`docs/handoff.md` mục 1.1 để biết lý do đầy đủ — tóm tắt: các struct này
không có logic thuần Layer 2, mọi hàm đọc/ghi thật (Flash, board init) đều
cần Layer 0/1, nên việc khai báo instance thật thuộc về Layer 3/4.

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

## 2.6 Modbus Register Map (V1) — CHÍNH THỨC, theo v1.9 (update của v1.7)

Đây là bảng địa chỉ CHÍNH THỨC, ưu tiên tuyệt đối so với mọi con số minh hoạ
ở bản trước. 1 register = 16 bit. Rule = 32 byte = 16 register. Toàn bộ
vùng config transfer (0x9000-0xA001, mục 2.6.2) giữ nguyên giữa v1.7 và
v1.9 — chỉ vùng Core/Runtime (2.6.1) có thêm block mới.

### 2.6.1 Core / Runtime

| Address | R/W | Block | Length (reg) | Ý nghĩa |
|---|---|---|---|---|
| 0x0000-0x0009 | RO | DEVICE_DESCRIPTOR | 10 | `SPLC_DeviceDescriptor` |
| 0x0010 | RO | RULE_TABLE_INFO | 1 | Số rule active |
| 0x0011-0x001F | - | RESERVED | 15 | Reserved cho core profile V1 |
| **0x0020-0x0029** | **RO** | **DEVICE_RESOURCE_INFO** | **10** | **V1.9 — MỚI. `SPLC_DeviceResourceInfo`: wire profile, max_rules, resource counts. Thay thế vị trí từng dự trù cho `CapabilityFlags` (đã bị bỏ hẳn ở v1.9).** |
| 0x0100-0x073F | RO | ACTIVE_RULE_TABLE | 1600 max | `SPLC_RuleRecord[100]` — App đọc lại toàn bộ Rule Table |
| 0x0800-0x0809 | RO | DEVICE_HEALTH | 10 | `SPLC_DeviceHealth` — **V1.9: scan_time đổi đơn vị hiển thị tên field thành `_ms`, wire size không đổi** |
| 0x0900-0x09FF | RO | RUNTIME_TAG_VALUES | 256 max | `int32_t[128]`, **2 register/tag** (kể cả DI/DO — không dùng Coil ở đâu cả). V1.9: 128 là wire capacity cố định; sản phẩm cụ thể có thể dùng ít hơn, phần còn lại là slot invalid/reserved theo `SPLC_DeviceResourceInfo` |
| 0x0A00 | WO | SYSTEM_COMMAND | 1 | Xem 2.6.4 |
| 0x0A01-0x0A02 | RO | SYSTEM_COMMAND_RESULT | 2 | Xem 2.6.4 |

**Flow discovery mới (V1.9):** sau khi connect, App đọc DEVICE_DESCRIPTOR
(0x0000) rồi DEVICE_RESOURCE_INFO (0x0020) NGAY SAU ĐÓ, validate
`wire_profile == SPLC_WIRE_PROFILE_V1`, rồi tự dựng `ProductDefinition` +
`TagCatalog` runtime từ các count trong `SPLC_DeviceResourceInfo` — không
còn lookup cứng theo `device_variant` như cách tiếp cận v1.7 từng ngụ ý.
`device_variant` từ V1.8/V1.9 trở đi chỉ còn ý nghĩa identity/diagnostics.
**Unknown `device_variant` không đồng nghĩa Unsupported** — App vẫn chấp
nhận thiết bị miễn `protocol_version`/`wire_profile`/`resource profile` hợp
lệ.

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
    -> fallthrough GUARD_CHECK (guard=GUARD_TAG_NONE, mo -- xem canh bao ve
       gia tri sentinel nay o muc 2.2, KHONG PHAI 0)
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
| `g_device_resource_info` | V1.9 — wire profile + resource counts | Hằng số biên dịch | Modbus |

---

## 6. Mô hình "kéo" (poll) vs "đẩy" (push)

Không đổi — `tag_write()` ghi xong là dừng, không tự lan truyền. Phải có 1
bên khác chủ động đọc lại theo lịch cố định.

---

## 7. Tag Index — `#define` số cố định (Cách A), không dùng `enum` tự đánh số

**ĐÃ SỬA — không còn đúng nữa, xem mục 2.2 để biết bugfix đầy đủ:**
`guard_tag = 0` (`TAG_NONE`) TỪNG là sentinel "không guard" dưới layout
v1.7 (index 0 là ô trống). Dưới layout v1.9 (mục 2.1), index 0 là `TAG_DI0`
— một tag thật — nên sentinel "không guard" chính thức giờ là
**`GUARD_TAG_NONE` (`0x7FFF`)**, KHÔNG phải `0` và cũng KHÔNG phải
`0xFFFF`. Đây là 1 bug thật đã được phát hiện bằng compile+chạy thật và đã
sửa trong `plc_rule.h`/`plc_rule.c` — trước khi sửa, `TAG_DI0` là tag duy
nhất trong hệ thống không thể dùng làm `guard_tag`.

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
- [ ] `guard_tag` dùng sentinel `GUARD_TAG_NONE` (`0x7FFF`) cho "không có
      guard" — KHÔNG dùng `0` (đó là `TAG_DI0` thật dưới layout v1.9).
- [ ] Khi Modbus config service (Layer 3, chưa viết) nhận `guard_tag` từ
      App, nên validate index nằm trong `0..MAX_TAGS-1` hoặc đúng bằng
      `GUARD_TAG_NONE` — hiện `rule_table_commit()` chưa làm việc này (xem
      mục 10).

---

## 9. Câu hỏi/hiểu nhầm đã xử lý — tham khảo nhanh

| Hiểu nhầm | Thực tế đúng |
|---|---|
| App-MCU qua RS485 | **USB** (Modbus RTU / USB-CDC / TinyUSB); RS485 chỉ cho Gateway variant (Modbus Master) |
| Rule 28 byte | **32 byte chính thức**, có `reserved[6]` |
| CRC32 verify Rule Transfer | **CRC-16/MODBUS**, 1 register |
| Cần `rule_id` | v1.7: KHÔNG cần — dùng RuleIndex (vị trí) |
| `guard_tag = 0` luôn là sentinel "không guard" | **CHỈ đúng dưới v1.7.** Dưới v1.9 (index 0 = `TAG_DI0` thật), sentinel chính thức là `GUARD_TAG_NONE` (`0x7FFF`) — bug thật đã phát hiện + sửa bằng compile/chạy thật, xem mục 2.2 |
| Tag layout v1.9 giống hệt v1.7, chỉ đổi số lượng slot | Sai — v1.9 còn **bỏ hẳn ô sentinel `TAG_NONE` ở index 0**, khiến mọi index dịch xuống 1 so với file `plc_tag_def.h` bản v1.7 cũ. Giá trị `TAG_DI0..TAG_VREG_R15` cũ (nếu có lưu ở đâu) phải tính lại, không tái sử dụng được |
| `SPLC_DeviceHealth.scan_time_us` | V1.9 đổi tên field thành `scan_time_ms`/`max_scan_time_ms` — xác nhận đơn vị **milliseconds** (đúng tên field), KHÔNG phải microsecond như comment cũ trong docx v1.7 |
| `SPLC_DeviceResourceInfo` dùng `CapabilityFlags` để khai báo tính năng | V1.9 bỏ hẳn `CapabilityFlags` — Rule Engine suy từ `max_rules > 0`, Retain suy từ `vreg_retain_count > 0`, còn lại (Runtime Tags/Health/System Commands) mặc định luôn có với Wire Profile V1 |
| switch/case + state machine luôn an toàn hơn if/continue | Không tự nhiên đúng — bản đầu có 3 bug tinh vi hơn, phải test thực nghiệm |
| Dwell chỉ 2 kết quả | Có 3: `dwell_ok`, `dwell_waiting`, `dwell_interrupted` |
| DI/DO nên dùng Coil (FC01/05) | v1.7/v1.9 đều: dùng Holding Register (FC03/16) cho MỌI tag — không đổi giữa 2 bản |
| Cần API "Load Rule from Device" riêng | Không cần — `ACTIVE_RULE_TABLE` (0x0100) đã cho đọc lại toàn bộ |
| "Layer 2 nên tự gọi tiếp Layer 3 khi có tag_write" | `tag_write` chỉ ghi RAM rồi dừng — mô hình kéo |
| "rule_scan ghi kết quả vào g_do_tag_map" | `g_do_tag_map` chỉ là bảng tra cứu; `rule_scan` chỉ ghi `g_tag_value[]` qua `tag_write()` |
| "g_tag_value có mảng riêng từng loại DI/DO/AI" | Chỉ 1 mảng `g_tag_value[128]` DUY NHẤT dùng chung |
| "rule_scan cần biết tag là DI hay DO" | KHÔNG cần — chỉ thực thi theo idx định sẵn trong `Rule` |

---

## 10. Việc còn để ngỏ / có thể làm tiếp

**Đã cập nhật lại theo tình trạng code thật đã đọc/verify** (không chỉ đọc
`docs/handoff.md`, vốn đã lạc hậu so với code trên 1 số điểm — xem
`docs/handoff.md` để biết phiên bản của chính nó có được cập nhật theo hay
chưa). Những mục dưới đây **đã xong** ở phiên soát code gần nhất, khác với
danh sách cũ:
- Layer 0/1 cho GPIO, ADC, Flash, UART, Time: **đã có implementation thật**
  (`platforms/stm32/stm32h5/{gpio,adc,flash,uart,time}/*.c`), không còn là
  file rỗng/chưa tồn tại như 1 số ghi chú cũ (kể cả `docs/handoff.md`) từng
  liệt kê.
- USB CDC (Layer 1, `components/usb_cdc/sx_usb_cdc.c`): **đã có
  implementation thật dùng TinyUSB** (`tusb_init`, `tud_cdc_read/write`,
  `tud_task`), không còn "`.c` rỗng" như từng ghi.
- Bugfix `guard_tag` sentinel (mục 2.2 ở trên): **đã sửa và verify bằng
  compile+chạy thật.**

Việc thật sự còn mở, theo đúng thứ tự phụ thuộc:

- [ ] **Layer 3 (`services/`) HOÀN TOÀN RỖNG** (`plc_io.c/h`,
      `plc_retain.c/h`, `plc_modbus_cfg.c/h` — cả 3 module đều 0 dòng thật
      sự, đã verify bằng `wc -l`). Đây là khoảng trống lớn nhất hiện tại.
      Layer 0/1 mà `plc_io.c`/`plc_retain.c` cần (GPIO, ADC, Flash) giờ đã
      sẵn sàng, không còn là lý do chặn nữa — có thể bắt đầu viết Layer 3.
- [ ] **Layer 4 (`app/plc_app/plc_engine.c/h`) HOÀN TOÀN RỖNG** (0 dòng).
      Chỉ nên viết sau khi ít nhất `plc_io.c` xong.
- [ ] **Pin mapping thật** (DI0-7/DO0-7 → chân GPIO nào, AI0-3 → ADC
      channel nào) — cần đọc `RS485_IO_RF_V2.ioc` hoặc hỏi trực tiếp trước
      khi viết phần map cụ thể trong `plc_io.c`.
- [ ] **Vị trí Flash lưu Active Rule Table CHƯA đặc tả** — địa chỉ, kích
      thước (tối thiểu `100*32=3200 byte`), có cần wear-leveling như dự
      kiến cho `plc_retain.c` hay ghi đè 1 chỗ cố định là đủ.
- [ ] **Cách Layer 4 truyền tick ms vào `rule_scan()`** — hiện `plc_rule.c`
      dùng `static uint32_t s_rule_scan_now_ms` nội bộ, luôn = 0 (xem TODO
      trong chính file đó). Thêm tham số hay setter function — chưa quyết.
- [ ] **Nguồn RTC cho `TRG_TIME_WINDOW` chưa quyết định** — `plc_rule.c`
      hiện hardcode `now_hhmm = 0`.
- [ ] **Modbus Master cho Gateway — `plc_modbus_master.c` CHƯA TỒN TẠI.**
      Cần quyết định RTU thôi hay cả TCP. `port/modbus_serial/` hiện chỉ có
      2 file stub gần như rỗng (`modbus_serial.h` 5 dòng, `.c` 3 dòng).
- [ ] **Event Log (`ACT_LOG_EVENT`) — buffer/format CHƯA THIẾT KẾ.**
- [ ] **Alarm (`ACT_SEND_ALARM`) — cơ chế CHƯA THIẾT KẾ** (mức độ nghiêm
      trọng? cơ chế ACK?).
- [ ] **`SYSTEM_COMMAND` — chưa có implementation Layer 3/4** (`plc_device.h`
      và `plc_system_cmd.h` chỉ có type definition, có chủ đích không có
      `.c` — xem `docs/handoff.md` mục 1.1 để biết lý do).
- [ ] **API đăng ký kênh cho `plc_io.c`** (`plc_io_register_di/do/ai`) — đề
      xuất thay thế 2 mảng song song dễ lệch index — chưa triển khai (vì
      `plc_io.c` chưa được viết).
- [ ] **Validate `guard_tag`/`trigger_tag`/`action_tag` trong biên
      `MAX_TAGS`** trước khi `rule_table_commit()` — chưa có. Cụ thể: nếu
      App gửi `guard_tag` với index bits nằm trong khoảng
      `128..(0x7FFF - 1)` (không phải tag hợp lệ, cũng không phải
      `GUARD_TAG_NONE`), hiện `tag_read()` tự chặn (trả 0) nên không crash,
      nhưng rule sẽ luôn bị coi guard "đóng" một cách âm thầm, không báo
      lỗi gì. Nên validate ở `plc_modbus_cfg.c` (Layer 3, khi giải mã dữ
      liệu từ Modbus) — chưa quyết định có nên validate thêm lần nữa ở
      `rule_table_commit()` (Layer 2) hay không.
- [ ] `port/modbus_usb/` (Layer 3.5, kênh App-MCU chính thức qua USB) —
      hiện chỉ có `port/usb/` với `tusb_config.h`/`usb_descriptors.c` (cấu
      hình TinyUSB CDC), CHƯA có file `modbus_usb.c/.h` nối nanoMODBUS với
      `sx_usb_tiny_read/write` như kiến trúc mục 2.4b mô tả.
- [ ] nanoMODBUS (`libs/nanomodbus/`, ~3000 dòng, đã có sẵn code) — root
      `CMakeLists.txt` mới chỉ thêm `libs/` vào include path chung (comment
      "Third-party (nanoMODBUS submodule, etc.)"), **chưa có
      `add_library`/`target_sources` nào thật sự compile file `.c` của
      nanoMODBUS vào bất kỳ target nào.**
- [ ] Xác nhận tốc độ poll App cần cho `RUNTIME_TAG_VALUES` — 2 register/tag
      cho mọi tag có thể là nút thắt cổ chai với RTU baudrate thấp.
- [ ] Xác nhận RAM đủ cho `ACTIVE_RULE_TABLE`+`STAGING_RULE_TABLE` (tổng
      6400 byte) với biến thể STM32H523 cụ thể.

---

*File này được biên soạn lại từ toàn bộ nội dung hỏi-đáp giữa người dùng và
Claude qua nhiều phiên làm việc. Bản cập nhật gần nhất đồng bộ theo tài liệu
chính thức `SimplePLC_App_MCU_Structs_v1.9_Self_Describing_Profile.md`
(update của v1.7), và theo kết quả đọc/verify code thật (compile + chạy
thật, không chỉ đọc mắt) trên toàn bộ 7 layer tại thời điểm cập nhật, bao
gồm 1 bugfix thật đã áp dụng (`guard_tag` sentinel, mục 2.2).*