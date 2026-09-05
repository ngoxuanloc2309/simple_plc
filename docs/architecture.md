# SimplePLC — Ghi chú kiến trúc Firmware (đầy đủ, dùng để tham chiếu lại)

> File này tổng hợp toàn bộ nội dung đã bàn bạc về kiến trúc phân lớp (layered
> architecture) của SimplePLC — dùng để:
> - Tiếp tục vibe-code dựa trên khung đã chốt.
> - Đưa cho 1 Claude/AI khác đọc để nắm ngữ cảnh mà không cần giải thích lại từ đầu.
> - Tra cứu lại khi quên 1 khái niệm nào đó.

---

## 0. Bối cảnh dự án

SimplePLC là firmware lõi cho dòng Remote I/O 8DI/8DO/4AI, chạy trên
STM32H523CCU6 (Cortex-M33, 250MHz, 256KB Flash / 272KB SRAM). Đây là bản đầu
tiên của 1 nền tảng Rule Engine dùng chung cho cả họ sản phẩm IIoT sau này
(Datalogger, Gateway...), nên nguyên tắc **"không heap động, struct nén cố
định, tách lớp rạch ròi"** là bắt buộc để port sang MCU khác sau này.

Nguồn gốc: `SimplePLC_RuleStruct_MCU_Spec_v0_1.md` — tài liệu spec gốc, quy
định struct `Rule` 28 byte, Tag Table 128 slot (69 dùng cho Remote I/O, 59 dự
trù cho Gateway), retentive storage qua Flash, nạp cấu hình qua Modbus RTU.

---

## 1. Tổng quan — 6 layer (5 layer nghiệp vụ gốc + Utils)

```
Layer 4   Engine & Application entry
Layer 3   PLC Application Services
Layer 2   PLC Core
Layer 1   SX Driver Core
Layer 0   Platform
Layer U   Utils          (nền — không nằm trong thứ bậc 0..4, mọi layer có thể include)
```

### Quy tắc xuyên suốt cả 6 layer

1. **Include chỉ chạy 1 chiều, từ trên xuống.** Layer cao gọi layer thấp,
   không bao giờ ngược lại. Layer 0/1/2 không bao giờ chủ động gọi lên Layer 3/4.
2. **Mô hình "kéo" (poll), không phải "đẩy" (push/event).** Ghi giá trị vào 1
   nơi thì dừng lại, không tự động kích hoạt bước tiếp theo ngay lập tức. Phải
   có 1 bên khác, chạy độc lập theo lịch cố định, tự đi đọc lại sau.
3. **Layer 2 là ranh giới port.** Không include gì từ Layer 0/1 → build/test
   được trên PC thuần, không cần phần cứng thật.
4. **Layer U (Utils) không include ngược lên ai** — Layer 0, 1, 2 đều có thể
   include nó tự do mà không phá quy tắc 1 chiều.

---

## 2. Chi tiết từng layer

### Layer U — Utils (nền tảng thuần thuật toán)

**Chức năng:** struct, con trỏ, thuật toán thuần C — không đụng phần cứng,
không biết Tag/Rule là gì.

| File | Việc |
|---|---|
| `cqueue.h` / `cqueue.c` | Hàng đợi vòng (ring buffer) — dùng làm buffer RX/TX cho UART. Đã có sẵn code. |
| `filter.h` / `filter.c` | Bộ lọc tín hiệu — làm mượt giá trị AI. Đã có sẵn code. |

```c
// cqueue.h
void cqueue_init(cqueue_t *q, uint8_t *buf, size_t cap);
int  cqueue_push(cqueue_t *q, uint8_t byte);
int  cqueue_pop(cqueue_t *q, uint8_t *out);
int  cqueue_available(cqueue_t *q);

// filter.h
void    filter_init(filter_t *f, ...);
int32_t filter_apply(filter_t *f, int32_t raw_value);
```

---

### Layer 0 — Platform

**Chức năng:** tầng DUY NHẤT được phép include HAL của hãng chip
(`stm32h5xx_hal.h`). Nơi "chạm phần cứng thật sự".

| File | Việc |
|---|---|
| `sx_uart_stm32.c` | Implement init/write/abort — gọi `HAL_UART_Transmit()`, `HAL_UART_Abort()` |
| `sx_gpio_stm32.c` | Implement đọc/ghi chân — gọi `HAL_GPIO_ReadPin()`, `HAL_GPIO_WritePin()` |
| `sx_adc_stm32.c` | Implement đọc ADC — gọi `HAL_ADC_*` |
| `sx_flash_stm32.c` | Implement ghi/xoá Flash — gọi `HAL_FLASH_*` |

**Đặc điểm cốt lõi:** đổi chip (STM32 → ESP32) chỉ cần thêm file `_esp32.c`
mới, cạnh file cũ — không sửa gì layer trên.

---

### Layer 1 — SX Driver Core

**Chức năng:** API chung, không phụ thuộc chip cụ thể — "hợp đồng" mà Layer 0
phải tuân theo.

```c
// sx_uart.h
int  sx_uart_init(sx_uart_t *uart, uint32_t baudrate);              // Layer 0 implement
int  sx_uart_write(sx_uart_t *uart, const uint8_t *data, int len);  // Layer 0 implement
int  sx_uart_abort(sx_uart_t *uart);                                // Layer 0 implement
int  sx_uart_read(sx_uart_t *uart, uint8_t *data, int len, uint32_t timeout_ms);  // Layer 1 core
int  sx_uart_available(sx_uart_t *uart);                            // Layer 1 core
void sx_uart_flush(sx_uart_t *uart);                                // Layer 1 core
void sx_uart_rx_callback(sx_uart_t *uart, uint8_t byte);            // gọi từ ISR Layer 0

// sx_gpio.h — toàn bộ do Layer 0 implement (không có phần core dùng chung)
int sx_gpio_init(uint32_t pin, uint8_t mode);
int sx_gpio_write(uint32_t pin, int value);
int sx_gpio_read(uint32_t pin);

// sx_adc.h — toàn bộ do Layer 0 implement
int     sx_adc_init(uint32_t channel);
int32_t sx_adc_read(uint32_t channel);

// sx_flash.h — toàn bộ do Layer 0 implement
int sx_flash_erase_sector(uint32_t sector_addr);
int sx_flash_write(uint32_t addr, const uint8_t *data, size_t len);
int sx_flash_read(uint32_t addr, uint8_t *out, size_t len);

// sx_config.h
#define SX_PLATFORM  SX_PLATFORM_STM32H5   // 1 chỗ duy nhất chọn platform
```

**Vì sao UART có "core" dùng chung nhưng GPIO/ADC/Flash thì không:**
`sx_uart_read/available/flush` chỉ thao tác trên `cqueue` (Layer U) — byte đã
được đẩy vào hàng đợi từ ISR (Layer 0) từ trước, nên đọc ra là logic thuần,
giống hệt nhau dù chip nào. GPIO/ADC/Flash mọi thao tác đều chạm HAL trực
tiếp, không có phần logic trung gian nào tách được ra — nên toàn bộ nằm ở
Layer 0.

---

### Layer 2 — PLC Core (ranh giới port)

**Chức năng:** logic nghiệp vụ thuần — Tag Table + Rule Engine. Không biết
UART/GPIO/Modbus tồn tại.

#### 2.1 `plc_tag.h` / `plc_tag.c`

```c
typedef enum {
    TAG_NONE = 0, TAG_DI, TAG_DO, TAG_AI, TAG_VFLAG, TAG_VREG,
    TAG_MB_COIL, TAG_MB_HOLDING, TAG_VREG_RETAIN,
} TagKind;

typedef struct {
    uint8_t  kind;       // TagKind
    uint8_t  channel;
    uint16_t reg_addr;   // chỉ dùng cho TAG_MB_*
} Tag;   // 4 byte/tag — MÔ TẢ Ý NGHĨA, không đổi lúc chạy

#define MAX_TAGS 128
extern Tag     g_tag_table[MAX_TAGS];   // bảng mô tả ý nghĩa (nạp lúc boot)
extern int32_t g_tag_value[MAX_TAGS];   // bảng GIÁ TRỊ SỐNG — thay đổi liên tục

void     tag_table_load_from_flash(void);
int32_t  tag_read(uint16_t idx);
void     tag_write(uint16_t idx, int32_t value);
TagKind  tag_get_kind(uint16_t idx);
```

**Code thật của 2 hàm trung tâm nhất hệ thống:**
```c
int32_t tag_read(uint16_t idx) {
    return g_tag_value[idx];   // chỉ đọc, không hơn
}
void tag_write(uint16_t idx, int32_t v) {
    g_tag_value[idx] = v;      // chỉ ghi, KHÔNG gọi tiếp bất kỳ hàm nào khác
}
```

#### 2.2 `plc_rule.h` / `plc_rule.c`

```c
typedef enum { TRG_ON_CHANGE=0, TRG_ON_RISE, TRG_ON_FALL, TRG_TIME_WINDOW, TRG_INTERVAL } TriggerType;
typedef enum { OP_NONE=0, OP_EQ, OP_NEQ, OP_GT, OP_LT, OP_GTE, OP_LTE, OP_BETWEEN } CompareOp;
typedef enum { ACT_SET_TAG=0, ACT_TOGGLE_TAG, ACT_INC_COUNTER, ACT_WRITE_REMOTE,
               ACT_LOG_EVENT, ACT_SEND_ALARM, ACT_ADD_TAG, ACT_SCALE_TAG } ActionType;

typedef struct {
    int32_t  threshold_lo;
    int32_t  threshold_hi;   // tái dùng: cận trên BETWEEN/TIME_WINDOW, hoặc OFFSET cho SCALE_TAG
    uint32_t for_ms;         // dwell/debounce
    int32_t  action_param;
    uint16_t trigger_tag;    // idx tag làm điều kiện — DO NGƯỜI TẠO RULE CHỌN SẴN
    uint16_t action_tag;     // idx tag bị tác động — DO NGƯỜI TẠO RULE CHỌN SẴN
    uint16_t guard_tag;      // bit 0-14: idx, bit 15: NEGATE
    uint8_t  enabled;
    uint8_t  trigger_type;
    uint8_t  compare_op;
    uint8_t  action_type;
} Rule;   // 28 byte, layout tối ưu byte theo spec gốc

typedef struct {
    int32_t  prev_value;
    uint32_t condition_since_tick;
    uint32_t last_fire_tick;
} RuleRuntime;   // 12 byte, KHÔNG lưu Flash

#define MAX_RULES 100
extern Rule       g_rule_table[MAX_RULES];
extern RuleRuntime g_rule_runtime[MAX_RULES];
extern int         g_rule_count;

void rule_table_load_from_flash(void);
void rule_scan(void);
int  rule_table_commit(const uint8_t *raw_data, int rule_count);
```

**Code thật `rule_scan()`:**
```c
void rule_scan(void) {
    for (int i = 0; i < g_rule_count; i++) {
        Rule *r = &g_rule_table[i];
        if (!r->enabled) continue;

        int32_t current = tag_read(r->trigger_tag);   // đọc theo idx ĐÃ ĐỊNH SẴN trong rule
        if (!check_trigger_edge(...)) continue;
        if (!compare_ok(...)) continue;
        if (!dwell_ok(...)) continue;
        if (!guard_ok(...)) continue;

        execute_action(r);   // bên trong: tag_write(r->action_tag, ...)
    }
}
```

**Điểm quan trọng nhất về `rule_scan()`:** nó **KHÔNG cần biết** `trigger_tag`
hay `action_tag` là DI/DO/AI gì. Nó chỉ thực thi đúng theo idx đã được người
tạo rule (kỹ sư, qua công cụ cấu hình) chọn sẵn từ trước, gửi xuống qua
Modbus và lưu trong struct `Rule`. Ý nghĩa tag đã được "đóng băng" từ lúc
thiết kế, không phải việc `rule_scan()` tự suy luận lúc chạy.

`guard_tag` dùng thêm 1 bit ở vị trí cao nhất (bit 15) làm cờ NEGATE — cho
phép đảo ngược điều kiện guard (VD: chỉ chạy khi guard = 0 thay vì = 1) mà
không cần thêm field riêng.

#### 2.3 `plc_rule_eval.c` (chỉ dùng nội bộ `plc_rule.c`)

```c
bool check_trigger_edge(TriggerType type, int32_t prev, int32_t current, uint32_t now_ms);
bool compare_ok(CompareOp op, int32_t current, int32_t lo, int32_t hi);
```
Tách riêng vì là logic thuần toán học — dễ unit test độc lập.

#### 2.4 `plc_rule_action.c` (chỉ dùng nội bộ `plc_rule.c`)

```c
void execute_action(Rule *r);   // dispatch theo 8 ActionType
```
- `ACT_SET_TAG` → `tag_write(action_tag, action_param)`
- `ACT_TOGGLE_TAG` → đảo giá trị hiện tại
- `ACT_INC_COUNTER` → `+= action_param`
- `ACT_WRITE_REMOTE` → đánh dấu cần ghi Modbus tag từ xa (cờ, KHÔNG tự gọi UART)
- `ACT_LOG_EVENT` → ghi buffer log RAM nội bộ
- `ACT_SEND_ALARM` → set mã cảnh báo vào 1 tag riêng
- `ACT_ADD_TAG` → `+= tag_read(trigger_tag)` — **LƯU Ý:** khi cộng dồn nhiều
  nguồn, phải tách rule riêng cho mỗi nguồn, KHÔNG gộp chung 1 rule — nếu
  không sẽ đếm sai (ràng buộc thiết kế bắt buộc theo spec gốc, mục 4.6)
- `ACT_SCALE_TAG` → `tag_read(trigger_tag) * param/1000 + threshold_hi`

**Đặc điểm cốt lõi của cả Layer 2:** không include gì từ Layer 0/1. Build/test
được trên PC thuần, không cần phần cứng thật. Bản thân Layer 2 cũng không tự
gọi lên Layer 3 — chỉ "đứng yên" chờ ai đó (Layer 3) tới đọc/ghi (mô hình kéo).

---

### Layer 3 — PLC Application Services (lớp phiên dịch)

**Chức năng:** mỗi file nối RAM thuần (Layer 2) với 1 thứ cụ thể "ngoài đời"
(Layer 1). Layer DUY NHẤT biết cả 2 phía.

**Đặc điểm cốt lõi:** các file trong Layer 3 **KHÔNG gọi lẫn nhau** — chỉ gặp
nhau gián tiếp qua `g_tag_value[]` ở Layer 2.

#### 3.1 `plc_io.h` / `plc_io.c`

```c
void plc_io_init(void);   // thiết lập bảng ánh xạ tag<->pin trước vòng quét đầu
void input_scan(void);     // chân → RAM
void output_scan(void);    // RAM → chân
```

**4 mảng ánh xạ cần tự định nghĩa (hardcode), theo đúng sơ đồ mạch thật:**

```c
uint16_t g_di_tag_map[NUM_DI]   = {TAG_DI_SWITCH_1, TAG_DI_SWITCH_2, ...};
uint32_t g_di_gpio_pin[NUM_DI]  = {PIN_DI_SWITCH_1, PIN_DI_SWITCH_2, ...};
uint16_t g_do_tag_map[NUM_DO]   = {TAG_DO_LED_RED,  TAG_DO_LED_GREEN, ...};
uint32_t g_do_gpio_pin[NUM_DO]  = {PIN_DO_LED_RED,  PIN_DO_LED_GREEN, ...};
uint16_t g_ai_tag_map[NUM_AI]   = {...};
uint32_t g_ai_adc_channel[NUM_AI] = {...};
```

**QUY TẮC BẮT BUỘC (dễ sai, khó phát hiện):**
- Trong 1 cặp mảng song song, **vị trí `i` phải luôn mô tả cùng 1 kênh vật
  lý**. VD: `g_do_tag_map[0]` và `g_do_gpio_pin[0]` phải là cùng 1 DO, không
  được lệch thứ tự.
- `idx` dùng trong các mảng này phải **khớp với `g_tag_table[idx].kind`**
  đã khai ở Layer 2. VD: nếu `g_do_tag_map[0] = 9`, bắt buộc
  `g_tag_table[9].kind == TAG_DO`.
- Không có cơ chế runtime nào tự động kiểm tra 2 điều trên — sai là hệ thống
  chạy sai lặng lẽ, không crash, không log, rất khó debug. Nên cân nhắc viết
  thêm 1 hàm self-check lúc boot nếu cần chắc chắn hơn.

**Code thật:**
```c
void input_scan(void) {                          // chân → RAM
    for (int i = 0; i < NUM_DI; i++) {
        int val = sx_gpio_read(g_di_gpio_pin[i]); // (a) đọc chân THẬT — Layer 1
        tag_write(g_di_tag_map[i], val);          // (b) ghi vào RAM — Layer 2
    }
    for (int i = 0; i < NUM_AI; i++) {
        int val = sx_adc_read(g_ai_adc_channel[i]);
        tag_write(g_ai_tag_map[i], val);          // có thể qua filter_apply() (Layer U) trước khi ghi
    }
}

void output_scan(void) {                          // RAM → chân
    for (int i = 0; i < NUM_DO; i++) {
        int32_t val = tag_read(g_do_tag_map[i]);  // (a) đọc RAM — Layer 2
        sx_gpio_write(g_do_gpio_pin[i], val);      // (b) ghi chân THẬT — Layer 1
    }
}
```

**Lưu ý quan trọng đã làm rõ qua nhiều lượt hỏi — tránh hiểu nhầm:**
- `input_scan()` **chỉ ghi** vào ô DI/AI — đây là nguồn DUY NHẤT ghi vào các
  ô này (không ai khác được ghi, vì chỉ phần cứng thật mới biết giá trị input
  thật).
- Ô DO/VFLAG/VREG có thể được ghi bởi **NHIỀU nguồn khác nhau** cùng lúc:
  `rule_scan()` (tự động) và `modbus_config_service()` (App UI/SCADA/kỹ sư
  ghi thủ công qua Modbus). `output_scan()` không quan tâm ai vừa ghi.
- `output_scan()` **KHÔNG đọc chân GPIO rồi ghi lại chính chân đó** — nó đọc
  RAM (do rule/Modbus vừa quyết định) rồi mới áp ra chân vật lý. 2 việc khác
  hẳn nhau (đọc số trong RAM vs đặt điện áp ra chân), không phải thao tác dư
  thừa.
- `input_scan()`/`output_scan()` chạy lặp mỗi 10ms bất kể giá trị RAM có thay
  đổi hay không — đơn giản hoá code, đổi lấy 1 chút "lãng phí" nhỏ khi giá
  trị không đổi.

#### 3.2 `plc_retain.h` / `plc_retain.c`

```c
void retain_service(void);          // gọi mỗi vòng quét, tự kiểm tra chu kỳ 5 phút
void retain_snapshot_write(void);   // gọi từ CẢ 2 nơi: retain_service() và PVD_IRQHandler()
void retain_store_restore(void);    // gọi 1 lần lúc boot
void PVD_IRQHandler(void);          // ISR — NGOẠI LỆ DUY NHẤT trong Layer 3 chạy trong ngắt
```

Cơ chế: snapshot Flash định kỳ 5 phút (EEPROM-emulation, xoay vòng 8 sector
8KB) + PVD ngắt khẩn cấp lưu ngay khi phát hiện sụt áp. Trade-off chấp nhận:
có thể mất vài phút dữ liệu nếu mất điện đột ngột không kịp PVD.

#### 3.3 `plc_modbus_cfg.h` / `plc_modbus_cfg.c`

```c
void plc_modbus_cfg_init(void);
void modbus_config_service(void);
```

```c
void modbus_config_service(void) {
    uint8_t frame[256];
    int len = sx_uart_read(&g_rs485_uart, frame, sizeof(frame), 0);   // Layer 1

    uint16_t reg_addr = parse_modbus_register(frame);
    if (reg_addr == STAGING_BUFFER_OFFSET) {
        memcpy(&g_staging_buffer[...], frame + 6, len - 8);
    }
    else if (reg_addr == COMMIT_COMMAND_OFFSET) {
        uint32_t crc = crc32_calc(g_staging_buffer, g_staged_len);
        if (crc == g_expected_crc32) {
            rule_table_commit(g_staging_buffer, g_staged_rule_count);  // Layer 2
        }
    }
}
```

Nạp rule mới: staged upload vào buffer tạm → xác nhận CRC32 → commit
atomic-swap → mất kết nối giữa chừng không phá rule đang chạy.

---

### Layer 4 — Engine & Application entry

**Chức năng:** gói gọn thứ tự gọi tổng thể — KHÔNG chứa logic nghiệp vụ.

```c
// plc_engine.h
void plc_engine_init(void);
void plc_engine_scan_once(void);
```

```c
void plc_engine_init(void) {
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    plc_io_init();
    retain_store_restore();
    plc_modbus_cfg_init();
    pvd_init();
    watchdog_init();
}

void plc_engine_scan_once(void) {
    input_scan();              // 1. chân → RAM
    rule_scan();                // 2. xử lý logic thuần RAM
    output_scan();              // 3. RAM → chân
    modbus_config_service();    // 4. nhận cấu hình mới (nếu có)
    retain_service();           // 5. lưu trữ định kỳ
    watchdog_kick();            // 6. báo "tôi vẫn sống"
}
```

```c
// main.c — không có .h, là điểm vào chương trình
int main(void) {
    hal_init();
    plc_engine_init();
    while (1) {
        uint32_t start = g_system_tick_ms;
        plc_engine_scan_once();
        while (g_system_tick_ms - start < 10) { }   // canh đúng 10ms
    }
}
```

**Vì sao thứ tự `input_scan → rule_scan → output_scan` quan trọng:**
`rule_scan()` đứng giữa để Rule luôn thấy input MỚI NHẤT vừa đọc, và kết quả
Rule tính ra được đẩy ra chân NGAY trong cùng vòng quét — không bị trễ thêm
1 nhịp.

---

## 3. Bảng tổng hợp — Ai được include ai

| File | Được include bởi |
|---|---|
| `plc_tag.h` | Layer 3 (mọi file), Layer 2 nội bộ |
| `plc_rule.h` | `plc_modbus_cfg.c` (chỉ dùng `rule_table_commit`), Layer 4 |
| `plc_rule_eval.h`, `plc_rule_action.h` | Chỉ `plc_rule.c` |
| `plc_io.h`, `plc_retain.h`, `plc_modbus_cfg.h` | Chỉ Layer 4 (`plc_engine.c`) |
| `plc_engine.h` | Chỉ `main.c` |

---

## 4. Ví dụ luồng dữ liệu đầy đủ (xuyên suốt cả 6 layer)

**Tình huống:** Kỹ sư gửi rule mới qua RS485: "khi tag 9 (DI) = 1 thì bật
đèn đỏ (tag 21 → GPIO 19)".

### 4.1 Nạp rule qua Modbus (1 lần)

```
[Byte Modbus đến chân RX]
        │
Layer 0   ISR bắt byte → sx_uart_rx_callback()
        │
Layer 1   đẩy byte vào cqueue (Layer U)
        │
Layer 3   modbus_config_service() gọi sx_uart_read() lấy byte ra,
          tự parse thành khung Modbus, kiểm tra CRC32
        │
Layer 2   rule_table_commit() ghi rule mới vào g_rule_table[]
```

### 4.2 Vòng quét chạy (lặp lại mỗi 10ms)

```
BƯỚC 1  input_scan()
   Chân GPIO 5 (thật) = HIGH
        │  sx_gpio_read(5) → val = 1
        │  tag_write(g_di_tag_map[0] = 0, 1)
        ▼
   g_tag_value[0] = 1                    ← ghi vào Ô DI

BƯỚC 2  rule_scan()
   current = tag_read(0) = 1              ← đọc Ô DI (theo trigger_tag=0 định sẵn trong rule)
   → thoả điều kiện rule
   → execute_action() → tag_write(9, 1)
        ▼
   g_tag_value[9] = 1                    ← ghi vào Ô DO (theo action_tag=9 định sẵn)

BƯỚC 3  output_scan()
   val = tag_read(g_do_tag_map[0] = 9) = 1   ← đọc Ô DO
   sx_gpio_write(g_do_gpio_pin[0] = 19, 1)
        ▼
   Chân GPIO 19 (thật) = HIGH → đèn đỏ sáng thật
```

**Ghi nhớ:** `plc_modbus_cfg.c` và `plc_io.c` không gọi trực tiếp lẫn nhau —
chỉ gặp nhau gián tiếp qua `g_tag_value[]`. Đổi giao thức nạp cấu hình
(Modbus → MQTT) chỉ cần thay `plc_modbus_cfg.c`, không đụng `plc_io.c`.

---

## 5. Các mảng dữ liệu — dễ nhầm, cần phân biệt rõ

| Mảng | Layer | Chứa gì | Ai ghi | Ai đọc |
|---|---|---|---|---|
| `g_tag_value[128]` | 2 | **Giá trị sống** hiện tại của mỗi tag (chỉ là số) | `input_scan` (DI/AI); `rule_scan`/Modbus (DO/VFLAG/VREG) | `rule_scan`, `output_scan`, Modbus (đọc để hiển thị) |
| `g_tag_table[128]` | 2 | **Ý nghĩa** mỗi tag: `kind`, `channel`, `reg_addr` | Nạp 1 lần lúc boot, không đổi khi chạy | `tag_get_kind()` khi Layer 3 cần biết loại tag |
| `g_di_tag_map[]` / `g_do_tag_map[]` / `g_ai_tag_map[]` | 3 | Danh sách **idx tag nào** là DI/DO/AI (đã lọc sẵn) | Hardcode lúc viết firmware | `input_scan`/`output_scan` |
| `g_di_gpio_pin[]` / `g_do_gpio_pin[]` / `g_ai_adc_channel[]` | 3 | **Chân/kênh vật lý thật** tương ứng | Hardcode lúc viết firmware | `input_scan`/`output_scan` |

**`tag_read`/`tag_write` chỉ đụng `g_tag_value[]`** — không đụng `g_tag_table`
hay bất kỳ bảng ánh xạ nào khác. Chúng không biết và không cần biết ý nghĩa
tag là gì.

**`rule_scan()` không cần biết tag là DI/DO/AI** — nó chỉ thực thi đúng theo
idx đã được định sẵn trong struct `Rule` (`trigger_tag`, `action_tag`), do
người tạo rule chọn từ trước.

**`input_scan()`/`output_scan()` mới thực sự cần biết loại tag** — thông qua
các bảng `g_di_tag_map`/`g_do_tag_map` đã lọc sẵn theo `kind`, chuẩn bị từ lúc
khởi tạo (`plc_io_init()`), không phải tự dò `g_tag_table[]` mỗi vòng quét.

---

## 6. Mô hình "kéo" (poll) vs "đẩy" (push) — điểm hay hiểu nhầm nhất

**Push (KHÔNG phải cách SimplePLC dùng):** ghi xong 1 giá trị thì lập tức tự
động kích hoạt bước tiếp theo ngay, giống domino đổ.

```c
// PUSH — SimplePLC KHÔNG làm thế này
void tag_write(uint16_t idx, int32_t v) {
    g_tag_value[idx] = v;
    if (idx == 9) sx_gpio_write(19, v);   // tự động đẩy luôn — SAI với thiết kế thật
}
```

**Poll (cách SimplePLC THỰC SỰ dùng):** ghi xong là dừng, im lặng. Phải có 1
bên khác, chạy theo lịch cố định riêng, tự chủ động đi hỏi lại sau.

```c
void tag_write(uint16_t idx, int32_t v) {
    g_tag_value[idx] = v;   // GHI XONG LÀ DỪNG, không gọi ai khác
}
// ...10ms sau, KHÔNG liên quan trực tiếp đến lệnh ghi trên...
void output_scan(void) {
    int32_t val = tag_read(9);   // TỰ ĐI HỎI
    sx_gpio_write(19, val);
}
```

**Vì sao chọn poll:** vì `g_tag_value[9]` có thể bị ghi bởi NHIỀU nguồn khác
nhau (Modbus, Rule Engine...). Nếu chọn push, mỗi nguồn ghi phải tự biết tra
bảng GPIO — logic lặp lại nhiều nơi, dễ bug khi sửa 1 chỗ quên chỗ khác. Với
poll, dù có bao nhiêu nguồn cùng ghi trong 10ms đó, `output_scan()` chỉ cần
đọc giá trị CUỐI CÙNG đúng 1 lần, không cần biết ai vừa ghi.

**Cái giá phải trả:** có độ trễ tối đa gần 10ms giữa lúc ghi và lúc thực sự
ra chân — đây là đánh đổi có chủ ý, không phải lỗi.

---

## 7. Định nghĩa macro cho Tag Index — quyết định đã chốt

**Đã cân nhắc 2 cách:**

### Cách A — `#define` số cố định thủ công (ĐÃ CHỌN cho SimplePLC)
```c
#define TAG_DI_SWITCH_1     0
#define TAG_DO_LED_RED      9
#define PIN_DI_SWITCH_1     5
#define PIN_DO_LED_RED      19
```
**Lý do chọn cách này:** số idx cần ổn định VĨNH VIỄN qua các đợt cập nhật
firmware, không được để trình biên dịch tự dồn số lại. Spec đã quy hoạch cố
định 69/128 tag (59 chỗ dự trù cho Gateway sau này) — không phát sinh thêm
tag giữa chừng theo kiểu ngẫu nhiên.

### Cách B — `enum` tự động đánh số (KHÔNG dùng cho tag index cố định)
```c
typedef enum {
    TAG_DI_SWITCH_1 = 0,
    TAG_DI_SWITCH_2,
    TAG_DO_LED_RED,
    TAG_COUNT
} TagIndex;
```
**Rủi ro nếu dùng:** chèn thêm 1 tag mới vào giữa danh sách sẽ khiến toàn bộ
số phía sau tự dồn lại — không còn khớp với dữ liệu tag đã lưu sẵn trong
Flash từ đợt cấu hình trước. Chỉ nên cân nhắc cách này nếu tag index KHÔNG
cần ổn định qua các bản firmware (không phải trường hợp của SimplePLC).

**Việc cần làm tiếp (đã đề xuất, chưa triển khai):** soạn file `plc_tag_def.h`
đầy đủ 69 tag theo đúng cách phân bổ trong spec gốc.

---

## 8. Quy tắc bắt buộc dễ sai — checklist khi code

- [ ] Trong mỗi cặp mảng song song (`g_do_tag_map[]` / `g_do_gpio_pin[]`),
      vị trí `i` phải luôn mô tả đúng CÙNG 1 kênh vật lý.
- [ ] `idx` trong các mảng ánh xạ Layer 3 phải khớp với `g_tag_table[idx].kind`
      đã khai ở Layer 2.
- [ ] Layer 2 (`plc_tag.c`, `plc_rule.c`, `plc_rule_eval.c`, `plc_rule_action.c`)
      KHÔNG được include bất kỳ file nào của Layer 0/1.
- [ ] Mọi truy cập `g_tag_value[]` từ Layer 3 trở lên PHẢI qua `tag_read()`/
      `tag_write()`, không truy cập mảng trực tiếp.
- [ ] Các file trong Layer 3 (`plc_io.c`, `plc_retain.c`, `plc_modbus_cfg.c`)
      KHÔNG được gọi lẫn nhau trực tiếp.
- [ ] `execute_action()` với `ACT_ADD_TAG`: tách rule riêng cho mỗi nguồn
      cộng dồn, KHÔNG gộp chung 1 rule.
- [ ] Khi thêm SKU mới (Datalogger, Gateway...), chỉ Layer 0 và một phần
      Layer 3 cần viết lại — Layer 1, 2, U giữ nguyên.
- [ ] Tag index dùng `#define` số cố định (Cách A ở mục 7), không dùng `enum`
      tự động đánh số.

---

## 9. Câu hỏi/hiểu nhầm đã xử lý trong quá trình bàn bạc (tham khảo nhanh)

| Hiểu nhầm ban đầu | Thực tế đúng |
|---|---|
| "Layer 2 nên tự gọi tiếp Layer 3 khi có `tag_write`" | `tag_write` chỉ ghi RAM rồi dừng — mô hình kéo, không đẩy (mục 6) |
| "output_scan đọc chân GPIO rồi ghi lại chính chân đó — dư thừa?" | `output_scan` đọc RAM (kết quả Rule Engine vừa tính), rồi mới áp ra chân — không phải đọc-ghi cùng 1 nơi |
| "input_scan và output_scan dùng chung 1 vùng RAM, output đọc lại đúng chỗ input ghi" | 2 hàm ghi/đọc vào 2 NHÓM Ô KHÁC NHAU (DI/AI vs DO); phải qua `rule_scan()` ở giữa mới nối được 2 nhóm |
| "rule_scan ghi kết quả vào g_do_tag_map" | `g_do_tag_map` chỉ là bảng ánh xạ (tra cứu), không phải nơi lưu giá trị; `rule_scan` chỉ ghi vào `g_tag_value[]` qua `tag_write()` |
| "g_tag_value có mảng riêng cho từng loại DI/DO/AI" | Chỉ có 1 mảng `g_tag_value[128]` DUY NHẤT dùng chung cho mọi loại tag |
| "làm sao rule_scan biết tag là DI hay DO" | Nó KHÔNG cần biết — chỉ thực thi theo idx đã định sẵn trong struct `Rule`, do người tạo rule chọn |
| "chỉ Modbus mới ghi được vào tag, quên mất App UI cũng ghi được" | Ô DO/VFLAG/VREG có thể bị ghi bởi NHIỀU nguồn: Rule Engine tự động VÀ App UI/SCADA qua Modbus |
| Gộp Layer 2+3 thành 1 khối, bỏ ranh giới | Không nên — nhiều nguồn cùng ghi tag (UART, Rule Engine...) cần 1 điểm chung để tránh viết trùng logic ánh xạ GPIO ở nhiều nơi |
| "Thêm 1 layer mới giữa Modbus parse và GPIO tra bảng" | Thực chất đã có sẵn — chỉ là tách `modbus_config_service()` (chỉ parse, dừng ở `tag_write`) và `output_scan()` (chỉ tra bảng, gọi `sx_gpio_write`) thành 2 hàm rõ ràng trong CÙNG Layer 3, không phải 2 layer mới |
| Index giữa các mảng ánh xạ có thể lệch nhau không sao | BẮT BUỘC phải khớp tuyệt đối — sai thì hệ thống chạy sai lặng lẽ, không crash, khó debug |

---

## 10. Việc còn để ngỏ / có thể làm tiếp

- [ ] Soạn file `plc_tag_def.h` đầy đủ 69 tag theo đúng phân bổ spec gốc.
- [ ] Cân nhắc viết hàm self-check lúc boot: duyệt `g_do_tag_map[]`/`g_di_tag_map[]`,
      kiểm tra chéo với `g_tag_table[idx].kind` để bắt lỗi cấu hình sai sớm,
      thay vì để lỗi lặng lẽ tới lúc test mới phát hiện.
- [ ] Định nghĩa cụ thể format 7 thanh ghi cấu hình Modbus (CONFIG_STATUS,
      CONFIG_ERROR_CODE, RULE_COUNT_STAGED, EXPECTED_CRC32, STAGING_BUFFER,
      COMMIT_COMMAND, ACTIVE_RULE_VERSION) — đã nhắc trong spec gốc, chưa đi
      sâu trong quá trình bàn bạc này.
- [ ] AI scaling và Modbus register map cho SCADA — cố tình nằm ngoài phạm vi
      tài liệu này theo spec gốc, cần tài liệu riêng.

---

*File này được biên soạn lại từ toàn bộ nội dung hỏi-đáp giữa người dùng và
Claude, dùng làm ngữ cảnh đầy đủ cho việc tiếp tục code hoặc trao đổi với
AI khác về cùng dự án.*