**SimplePLC — Yêu Cầu & Kiến Trúc Firmware**

_Tài liệu khung cho đội phát triển phần mềm/firmware — bản Remote I/O đầu tiên (STM32)_

_Phiên bản 1.0 — chốt thiết kế 02/09/2026 — TÀI LIỆU YÊU CẦU, dùng làm cơ sở triển khai firmware. Mọi quyết định trong tài liệu này là CHÍNH THỨC; thay đổi phải qua bản cập nhật có kiểm soát phiên bản._

Tài liệu đặc tả kiến trúc firmware lõi cho SimplePLC — nền tảng Rule Engine dùng chung cho dòng sản phẩm IIoT (Remote I/O, Datalogger, IoT Gateway, bộ điều khiển chuyên dụng), áp dụng cho bản Remote I/O 8DI/8DO/4AI đầu tiên trên STM32H523CCU6. Mọi ví dụ/case study trong tài liệu đều trình bày bài toán thực tế (cảm biến/nút bấm đấu vào đâu, yêu cầu vận hành là gì) trước khi vào chi tiết ánh xạ tag và struct.

# 0\. Tóm tắt kiến trúc

| **Hạng mục**              | **Quyết định**                                                                                                |
| ------------------------- | ------------------------------------------------------------------------------------------------------------- |
| Số kênh I/O bản đầu       | 8 DI + 8 DO + 4 AI                                                                                            |
| MCU                       | STM32H523CCU6 (Cortex-M33, 250MHz, 256KB Flash, 272KB SRAM) — CHỈ áp dụng cho bản Remote I/O đầu tiên (mục 8) |
| Mô hình lập trình         | Rule-based: Trigger → Guard (tuỳ chọn) → Action, 1 điều kiện chính/rule                                       |
| Struct Rule               | 28 byte/rule, tối đa 100 rule/thiết bị                                                                        |
| Đơn vị thời gian (for_ms) | Mili-giây, kiểu u32                                                                                           |
| Nạp cấu hình              | Qua Modbus RTU chuẩn — staged upload + CRC32 + atomic-commit (mục 5)                                          |
| Lưu trữ bền vững          | Snapshot định kỳ (5 phút) + phát hiện sụt áp PVD (mục 7)                                                      |
| Cập nhật firmware         | Qua ST-Link/UART tại xưởng — không OTA ở bản đầu                                                              |

# 1\. Người dùng cuối & Mô hình sử dụng

Mục này giúp đội phát triển hình dung sản phẩm được ai dùng và dùng như thế nào — vì đây là căn cứ cho nhiều quyết định kiến trúc trong tài liệu (tại sao bộ lệnh tối giản, tại sao guard chỉ 1 điều kiện phụ, tại sao có Blueprint dựng sẵn).

## 1.1 Hai nhóm người dùng

| **Nhóm**                               | **Vai trò**                                                                   | **Tương tác với thiết bị**                                                                         |
| -------------------------------------- | ----------------------------------------------------------------------------- | -------------------------------------------------------------------------------------------------- |
| Kỹ sư lắp đặt / tích hợp hệ thống (SI) | Cấu hình rule khi lắp đặt/commissioning, chỉnh sửa khi quy trình sản xuất đổi | Dùng công cụ cấu hình (PC Tool/Web UI) để tạo, sửa, nạp rule — KHÔNG viết code                     |
| Người vận hành nhà máy / SCADA         | Theo dõi sản xuất, phản ứng khi có cảnh báo                                   | Chỉ ĐỌC dữ liệu qua Modbus (HMI/SCADA) — không biết và không cần biết thiết bị có "rule" bên trong |

Kỹ sư lắp đặt là người dùng chính của mọi khái niệm mới trong tài liệu này (Tag, Rule, Guard, Retain). Người vận hành trải nghiệm thiết bị như 1 remote I/O + PLC nhỏ đã lập trình sẵn — không chạm vào các khái niệm đó.

## 1.2 Khái niệm mới kỹ sư lắp đặt cần nắm — khác gì so với PLC quen thuộc

| **Khái niệm**                   | **Khác gì so với PLC truyền thống**                                                                                                                                                       |
| ------------------------------- | ----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Tag                             | 1 chỉ số duy nhất cho MỌI thứ (I/O vật lý, biến nội bộ, điểm dữ liệu máy khác) — không cần phân biệt vùng nhớ kiểu %I/%Q/%M như PLC cũ                                                    |
| Rule = Trigger + Guard + Action | Câu "khi nào → nếu → thì làm gì", không phải ladder hay Structured Text — xem mục 4.6 để có ví dụ đầy đủ từng lệnh                                                                        |
| Guard                           | Chỉ 1 điều kiện phụ, không phải AND/OR đầy đủ — logic phức tạp phải chủ động chia nhỏ thành nhiều rule nối qua virtual flag (giống relay nội bộ, nhưng cách nối khác ladder truyền thống) |
| Retain (VREG_RETAIN)            | Phải CHỌN Ý THỨC tag nào cần sống qua mất điện khi tạo — khác PLC cũ nơi mọi vùng nhớ giữ hoặc không giữ được khai báo cố định trước theo địa chỉ                                         |
| Blueprint (mẫu dựng sẵn)        | Với các kịch bản phổ biến (Andon, đếm sản lượng), kỹ sư chọn mẫu và điền vài field thay vì tự ghép Trigger/Guard/Action từ đầu                                                            |

## 1.3 Quy trình sử dụng thực tế

- Bước 1 — Kết nối thiết bị qua RS485 (PC Tool) hoặc Web UI cục bộ do thiết bị tự host — không cần Internet/cloud.
- Bước 2 — Xem danh sách I/O có sẵn: thiết bị tự khai báo DI/DO/AI, kỹ sư không cần nhớ trước địa chỉ nào ứng với chân nào.
- Bước 3 — Chọn 1 trong 2 hướng: dùng Blueprint dựng sẵn (Andon, đếm sản lượng...) cho nhanh, hoặc tự tạo rule mới bằng form Trigger/Guard/Action.
- Bước 4 — Nạp cấu hình xuống thiết bị qua Modbus (mục 5) — có xác nhận CRC, an toàn ngay cả khi mất kết nối giữa chừng.
- Bước 5 — Xác nhận hoạt động: xem trạng thái thời gian thực ngay trên công cụ cấu hình, hoặc qua SCADA như vận hành bình thường.

_Đặc tả chi tiết giao diện cấu hình: xem tài liệu SimplePLC_Config_UI_Wireframes.pptx. Ví dụ đầy đủ từ màn hình cấu hình tới rule thực thi trên MCU: xem SimplePLC_Worked_Examples_UI_to_MCU.pptx._

# 2\. MCU mục tiêu: STM32H523CCU6

## 2.1 Thông số

| **Thông số**     | **Giá trị**                                                         |
| ---------------- | ------------------------------------------------------------------- |
| Lõi              | Arm Cortex-M33 (có FPU, DSP, TrustZone)                             |
| Xung nhịp tối đa | 250 MHz                                                             |
| Flash            | 256 KB                                                              |
| SRAM             | 272 KB                                                              |
| Gói              | UFQFPN48                                                            |
| Phát hiện sụt áp | PVD (Programmable Voltage Detector) — có sẵn trên chip, xem mục 7.3 |

_Nguồn: trang sản phẩm ST và các nhà phân phối (Mouser, Octopart, JLCPCB). Đối chiếu lại với datasheet đầy đủ (DS14540) khi chốt BOM sản xuất._

## 2.2 Ngân sách tài nguyên

| **Thành phần**                                         | **Kích thước** | **% của 272 KB SRAM** |
| ------------------------------------------------------ | -------------- | --------------------- |
| Rule table (100 rule × 28 byte)                        | 2.8 KB         | ~1.0%                 |
| Tag table (128 tag × 4 byte)                           | 0.5 KB         | ~0.2%                 |
| Tag Value Store — g_tag_value\[\] (128 × i32, mục 6.2) | 0.5 KB         | ~0.2%                 |
| Rule runtime state (100 rule × 12 byte)                | 1.2 KB         | ~0.4%                 |
| Buffer Modbus RTU (RX/TX)                              | ~1 KB          | ~0.4%                 |
| Stack + HAL + biến hệ thống (ước tính rộng rãi)        | ~8 KB          | ~2.9%                 |
| Tổng ước tính SRAM                                     | ~14 KB         | ~5.1%                 |

_Ngoài SRAM, mục 7.2 dành riêng 64 KB Flash (trong 256 KB) cho vùng lưu trữ bền vững._

**Yêu cầu bắt buộc — không phải khuyến nghị**

STM32H523 chỉ là baseline cho bản Remote I/O đầu tiên (mục 8). Các dòng sản phẩm khác (Datalogger, Gateway, bộ điều khiển chuyên dụng) sẽ đánh giá MCU riêng khi bắt đầu thiết kế — KHÔNG mặc định dùng chung H523.

Vì vậy, dù H523 dư tài nguyên ở bản đầu (~5% SRAM), nguyên tắc "không heap động, struct nén cố định" trong toàn bộ tài liệu là YÊU CẦU BẮT BUỘC khi triển khai code, không phải gợi ý — để lõi Rule Engine port được nguyên vẹn sang MCU nhỏ/rẻ hơn (STM32G0, STM32F1...) khi các SKU khác cần.

# 3\. Tag Table — bảng định danh thống nhất

Rule không thao tác trực tiếp lên chân GPIO hay địa chỉ Modbus — mọi trigger/action/guard trong Rule chỉ tham chiếu tới một chỉ số (tag_index) trong Tag Table.

## 3.1 Cấu trúc 1 tag

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>typedef enum {
    TAG_NONE        = 0,   // chỉ số 0 luôn để trống — dùng làm giá trị "không tham chiếu"
    TAG_DI          = 1,   // digital input nội bộ
    TAG_DO          = 2,   // digital output nội bộ
    TAG_AI          = 3,   // analog input nội bộ
    TAG_VFLAG       = 4,   // virtual flag nội bộ (1 bit, giống relay nội bộ của PLC)
    TAG_VREG        = 5,   // virtual register nội bộ (i32) — MẤT khi mất điện/reset
    TAG_MB_COIL     = 6,   // coil trên thiết bị Modbus khác (đọc/ghi bit)
    TAG_MB_HOLDING  = 7,   // holding register trên thiết bị Modbus khác
    TAG_VREG_RETAIN = 8,   // như VREG nhưng CÒN qua mất điện/reset (mục 7)
} TagKind;</code></pre><p></p><pre><code>typedef struct {
    uint8_t  kind;       // TagKind
    uint8_t  channel;    // DI/DO/AI/VFLAG/VREG: số kênh cục bộ | Modbus: địa chỉ slave
    uint16_t reg_addr;   // chỉ dùng cho TAG_MB_*: địa chỉ register/coil trên slave đó
} Tag;   // 4 byte / tag</code></pre></th></tr></tbody></table></div>

## 3.2 Phân bổ tag cho bản Remote I/O 8DI/8DO/4AI

| **Khoảng chỉ số** | **Ý nghĩa**                                                                      | **Số lượng** |
| ----------------- | -------------------------------------------------------------------------------- | ------------ |
| 0                 | TAG_NONE (dự trữ — nghĩa là "không có")                                          | 1            |
| 1 – 8             | DI0 – DI7                                                                        | 8            |
| 9 – 16            | DO0 – DO7                                                                        | 8            |
| 17 – 20           | AI0 – AI3                                                                        | 4            |
| 21 – 36           | Virtual flag VFLAG0 – VFLAG15                                                    | 16           |
| 37 – 52           | Virtual register VREG0 – VREG15 (timer, kết quả tạm — KHÔNG retain)              | 16           |
| 53 – 68           | Virtual register retain VREG_R0 – VREG_R15 (đếm sản lượng, tích luỹ — CÓ retain) | 16           |
| 69 – 127          | Dự trữ cho Modbus tag từ xa (chưa dùng ở Remote I/O, sẵn cho bản Gateway)        | 59           |

_Tổng dung lượng bảng: 128 tag × 4 byte = 512 byte. Đây là ánh xạ cố định do firmware định nghĩa — trên công cụ cấu hình, kỹ sư lắp đặt chỉ thấy tên gợi nhớ ("DO0 — Đèn đỏ", "VFLAG0 — in_shift"), không gõ trực tiếp con số chỉ số này; PC Tool tự tra bảng khi đóng gói rule._

# 4\. Rule Struct

## 4.1 Enum

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>typedef enum {
    TRG_ON_CHANGE   = 0,  // trigger_tag đổi giá trị (bất kỳ hướng nào)
    TRG_ON_RISE     = 1,  // 0-&gt;1 (digital) hoặc vượt threshold_lo theo hướng tăng (analog)
    TRG_ON_FALL     = 2,  // 1-&gt;0 (digital) hoặc vượt threshold_lo theo hướng giảm (analog)
    TRG_TIME_WINDOW = 3,  // theo giờ trong ngày — threshold_lo/hi là mốc HHMM, trigger_tag bỏ qua
    TRG_INTERVAL    = 4,  // định kỳ mỗi for_ms — dùng cho tính toán/polling định kỳ
} TriggerType;</code></pre><p></p><pre><code>typedef enum {
    OP_NONE    = 0,  // không so sánh — chỉ cần trigger xảy ra là đủ
    OP_EQ = 1, OP_NEQ = 2, OP_GT = 3, OP_LT = 4, OP_GTE = 5, OP_LTE = 6,
    OP_BETWEEN = 7,  // trong khoảng [threshold_lo, threshold_hi]
} CompareOp;</code></pre><p></p><pre><code>typedef enum {
    ACT_SET_TAG     = 0,  // action_tag = action_param
    ACT_TOGGLE_TAG  = 1,  // đảo trạng thái action_tag (bỏ qua action_param)
    ACT_INC_COUNTER = 2,  // action_tag += action_param (HẰNG SỐ)
    ACT_WRITE_REMOTE= 3,  // ghi action_param xuống 1 Modbus tag từ xa
    ACT_LOG_EVENT   = 4,  // ghi 1 dòng log nội bộ (timestamp, action_tag, action_param)
    ACT_SEND_ALARM  = 5,  // set mã cảnh báo action_param để lớp trên (SCADA/HMI) đọc
    ACT_ADD_TAG     = 6,  // action_tag += tag_read(trigger_tag)  (GIÁ TRỊ SỐNG, không phải hằng số)
    ACT_SCALE_TAG   = 7,  // action_tag = tag_read(trigger_tag) * action_param / 1000 + threshold_hi
} ActionType;</code></pre></th></tr></tbody></table></div>

## 4.2 Layout theo ý nghĩa (logic view)

| **Field**    | **Kiểu** | **Ý nghĩa**                                                                                          |
| ------------ | -------- | ---------------------------------------------------------------------------------------------------- |
| enabled      | u8       | 1 = rule đang hoạt động, 0 = tắt tạm mà không phải xoá                                               |
| trigger_type | u8       | Xem TriggerType                                                                                      |
| trigger_tag  | u16      | Chỉ số trong Tag Table — nguồn phát sinh trigger                                                     |
| compare_op   | u8       | Xem CompareOp                                                                                        |
| threshold_lo | i32      | Ngưỡng chính (hoặc cận dưới nếu BETWEEN / mốc giờ nếu TIME_WINDOW)                                   |
| threshold_hi | i32      | Cận trên nếu BETWEEN / giờ kết thúc nếu TIME_WINDOW / OFFSET nếu action_type=SCALE_TAG — còn lại = 0 |
| for_ms       | u32      | Phải giữ điều kiện liên tục bấy nhiêu ms trước khi coi là đã trigger (debounce/dwell)                |
| guard_tag    | u16      | Bit 0-14: chỉ số tag làm cổng phụ (0 = không có gate). Bit 15 (0x8000): NEGATE                       |
| action_type  | u8       | Xem ActionType                                                                                       |
| action_tag   | u16      | Chỉ số trong Tag Table — đích của action                                                             |
| action_param | i32      | Giá trị ghi / bước tăng / hệ số scale tuỳ action_type                                                |

**Ghi chú kiến trúc — threshold_hi mang 2 nghĩa tuỳ ngữ cảnh**

Đây là cách tái dùng field đã áp dụng cho guard_tag (bit NEGATE) — không tốn thêm byte nào trong struct 28 byte.

2 nghĩa không xung đột: threshold_hi chỉ có nghĩa BETWEEN/TIME_WINDOW khi compare_op=BETWEEN hoặc trigger_type=TIME_WINDOW; chỉ có nghĩa OFFSET khi action_type=SCALE_TAG. Công cụ sinh rule PHẢI đảm bảo không kết hợp 2 ngữ cảnh cùng lúc trong 1 rule — nếu cần cả 2, tách thành 2 rule.

## 4.3 Layout tối ưu byte (physical view — dùng khi viết code thật)

```
typedef struct {
    int32_t  threshold_lo;   // 4 byte
    int32_t  threshold_hi;   // 4 byte
    uint32_t for_ms;         // 4 byte
    int32_t  action_param;   // 4 byte   -- 16 byte, không padding
    uint16_t trigger_tag;    // 2 byte
    uint16_t action_tag;     // 2 byte
    uint16_t guard_tag;      // 2 byte   -- +6 byte (16..22)
    uint8_t  enabled;        // 1 byte
    uint8_t  trigger_type;   // 1 byte
    uint8_t  compare_op;     // 1 byte
    uint8_t  action_type;    // 1 byte   -- +4 byte (22..26)
                              // trình biên dịch thêm 2 byte đệm cuối để căn về bội số 4
} Rule;                     // TỔNG: 28 byte/rule (100 rule = 2.8 KB)
```

## 4.4 Ví dụ: 3 rule Andon

**Bài toán:** Một máy CNC trên dây chuyền có sẵn 1 tín hiệu số báo máy đang chạy, đấu vào 1 ngõ vào số (DI0), và 1 nút bấm để công nhân gọi hỗ trợ khẩn cấp, đấu vào ngõ vào số thứ 2 (DI1). Đấu thêm 1 đèn báo đỏ ra 1 ngõ ra số (DO0). Xưởng muốn: (1) nếu máy dừng liên tục quá 5 phút TRONG GIỜ CA sản xuất (07:00–16:00) thì tự động bật đèn đỏ để tổ trưởng biết; (2) công nhân có thể chủ động bấm nút gọi hỗ trợ để bật đèn đỏ ngay lập tức, không cần chờ đủ 5 phút; (3) khi máy chạy trở lại thì đèn tự tắt.

Ánh xạ vào Tag Table: DI0 = machine1_running (tag 1), DI1 = machine1_call_help (tag 2), DO0 = machine1_tower_red (tag 9), VFLAG0 = in_shift (tag 21).

| **#** | **Ý nghĩa**                        | **trigger_type** | **trigger_tag** | **compare_op** | **threshold_lo** | **for_ms** | **guard_tag** | **action_type** | **action_tag** | **action_param** |
| ----- | ---------------------------------- | ---------------- | --------------- | -------------- | ---------------- | ---------- | ------------- | --------------- | -------------- | ---------------- |
| R0a   | Vào ca 07:00 → bật cờ in_shift     | TIME_WINDOW      | —               | NONE           | 700              | 0          | —             | SET_TAG         | 21             | 1                |
| R0b   | Hết ca 16:00 → tắt cờ in_shift     | TIME_WINDOW      | —               | NONE           | 1600             | 0          | —             | SET_TAG         | 21             | 0                |
| R1    | Máy dừng >5' TRONG CA → bật đèn đỏ | ON_FALL          | 1               | NONE           | 0                | 300000     | 21            | SET_TAG         | 9              | 1                |
| R2    | Nút gọi hỗ trợ → bật đèn đỏ ngay   | ON_RISE          | 2               | NONE           | 0                | 0          | —             | SET_TAG         | 9              | 1                |
| R3    | Máy chạy lại → tắt đèn đỏ          | ON_RISE          | 1               | NONE           | 0                | 0          | —             | SET_TAG         | 9              | 0                |

## 4.5 Case study: Nút gọi cấp liệu + đèn cảnh báo + timer

**Bài toán:** Một trạm lắp ráp có tín hiệu báo máy đang chạy đấu vào 1 ngõ vào số (DI0), và 1 nút bấm để công nhân báo sắp hết nguyên liệu, cần cấp liệu, đấu vào ngõ vào số thứ 2 (DI1). Đấu thêm 2 đèn báo ra 2 ngõ ra số: đèn vàng (DO0) và đèn đỏ (DO1). Yêu cầu vận hành: khi công nhân bấm nút — nhưng CHỈ tính khi máy đang thực sự chạy (để tránh báo giả lúc bảo trì hoặc máy đã dừng hẳn) — đèn vàng bật lên báo đang chờ cấp liệu. Nếu sau 10 giây vẫn chưa ai xử lý (đèn vàng vẫn còn bật), đèn đỏ bật thêm để báo khẩn, tăng mức độ chú ý. Công nhân bấm nút lần nữa để xác nhận đã được cấp liệu xong: đèn vàng tắt, đồng hồ đếm giờ về 0, đèn đỏ (nếu đang bật) cũng tắt theo.

Ánh xạ vào Tag Table: DI0 = máy chạy (tag 1). DI1 = nút gọi cấp liệu (tag 2). DO0 = đèn vàng (tag 9). DO1 = đèn đỏ (tag 10). T1 = VREG0 (tag 37, KHÔNG retain — hợp lý vì đây là bộ đếm tạm thời trong 1 lượt chờ, không cần giữ qua mất điện).

| **#** | **Ý nghĩa**                                          | **trigger_type** | **trigger_tag** | **compare_op** | **threshold_lo** | **for_ms** | **guard_tag** | **action_type** | **action_tag** | **action_param** |
| ----- | ---------------------------------------------------- | ---------------- | --------------- | -------------- | ---------------- | ---------- | ------------- | --------------- | -------------- | ---------------- |
| R1    | Bấm DI1 → đảo đèn vàng, chỉ có tác dụng khi máy chạy | ON_RISE          | 2               | NONE           | 0                | 0          | 1             | TOGGLE_TAG      | 9              | —                |
| R2    | Mỗi giây, nếu đèn vàng đang bật thì T1 += 1          | INTERVAL         | —               | NONE           | 0                | 1000       | 9             | INC_COUNTER     | 37             | 1                |
| R3    | T1 vượt 10 → bật đèn đỏ                              | ON_CHANGE        | 37              | GT             | 10               | 0          | —             | SET_TAG         | 10             | 1                |
| R4    | Đèn vàng tắt → T1 reset về 0                         | ON_FALL          | 9               | NONE           | 0                | 0          | —             | SET_TAG         | 37             | 0                |
| R5    | Đèn vàng tắt → tắt luôn đèn đỏ                       | ON_FALL          | 9               | NONE           | 0                | 0          | —             | SET_TAG         | 10             | 0                |

_Giới hạn được chấp nhận theo thiết kế: guard áp dụng cho cả 2 chiều toggle. Nếu cần huỷ lệnh gọi bất chấp trạng thái máy, dùng thêm 1 DI reset thủ công riêng thay vì mở rộng struct._

## 4.6 Bộ lệnh đầy đủ — tham chiếu & ví dụ cho mỗi lệnh

**A. TRIGGER (trigger_type) — "khi nào" rule bắt đầu xét**

| **Tên**     | **Mã** | **Ý nghĩa**                                  | **Ví dụ cụ thể**                                                                             |
| ----------- | ------ | -------------------------------------------- | -------------------------------------------------------------------------------------------- |
| ON_CHANGE   | 0      | Bất kỳ thay đổi nào trên trigger_tag         | AI1 (áp suất, đã quy đổi) đổi giá trị bất kỳ lúc nào → ghi log liên tục để theo dõi xu hướng |
| ON_RISE     | 1      | 0→1 (digital) hoặc vượt ngưỡng tăng (analog) | DI1 (nút bấm) chuyển 0→1 → đảo đèn vàng DO0 (mục 4.5, R1)                                    |
| ON_FALL     | 2      | 1→0 hoặc giảm qua ngưỡng                     | DI0 (máy chạy) chuyển 1→0, giữ đủ 5 phút → bật đèn đỏ Andon (mục 4.4, R1)                    |
| TIME_WINDOW | 3      | Theo giờ trong ngày (threshold=HHMM)         | Đến đúng 07:00 → bật cờ in_shift (mục 4.4, R0a)                                              |
| INTERVAL    | 4      | Định kỳ mỗi for_ms                           | Mỗi 1000 ms → cộng dồn T1 nếu đèn vàng đang bật (mục 4.5, R2)                                |

**B. COMPARE (compare_op) — điều kiện áp thêm lên giá trị trigger_tag**

| **Tên** | **Mã** | **Ví dụ cụ thể**                                                                                                               |
| ------- | ------ | ------------------------------------------------------------------------------------------------------------------------------ |
| NONE    | 0      | Không so sánh gì thêm — đa số rule TOGGLE/SET đơn giản chỉ cần trigger xảy ra (mục 4.5, R1)                                    |
| EQ      | 1      | Mã lỗi động cơ (VREG) = 5 → gửi cảnh báo "quá nhiệt" (dùng với SEND_ALARM)                                                     |
| NEQ     | 2      | Trạng thái máy (VREG) khác 1 (không phải "đang chạy") → tạm dừng tính sản lượng                                                |
| GT      | 3      | T1 > 10 giây → bật đèn đỏ (mục 4.5, R3)                                                                                        |
| LT      | 4      | Áp suất khí nén (AI, đã quy đổi) < 4 bar → cảnh báo thiếu áp                                                                   |
| GTE     | 5      | Mức tồn kho nguyên liệu (VREG) ≥ mức đầy → dừng lệnh cấp liệu                                                                  |
| LTE     | 6      | Mức tồn kho nguyên liệu (VREG) ≤ mức đặt hàng lại → gửi cảnh báo nhập thêm                                                     |
| BETWEEN | 7      | Tốc độ đóng gói (AI) nằm trong 800–1200 sp/giờ → coi là hợp lệ, ghi nhận sản lượng; ngoài khoảng → bỏ qua vì nghi lỗi cảm biến |

**C. ACTION (action_type) — "làm gì" khi rule nổ**

| **Tên**      | **Mã** | **Công thức**                                                  | **Ví dụ cụ thể**                                                                                                        |
| ------------ | ------ | -------------------------------------------------------------- | ----------------------------------------------------------------------------------------------------------------------- |
| SET_TAG      | 0      | action_tag = action_param                                      | Máy dừng đủ lâu → SET DO0 (đèn đỏ) = 1 (mục 4.4, R1)                                                                    |
| TOGGLE_TAG   | 1      | Đảo trạng thái action_tag                                      | Bấm DI1 → TOGGLE DO0 (đèn vàng) (mục 4.5, R1)                                                                           |
| INC_COUNTER  | 2      | action_tag += action_param (hằng số)                           | DI2 lên sườn → Counter1 += 1 (mẫu cộng dồn nhiều nguồn bên dưới)                                                        |
| WRITE_REMOTE | 3      | Ghi action_param xuống Modbus tag từ xa                        | VFLAG cảnh báo cục bộ = 1 → ghi 1 vào coil địa chỉ 100 trên PLC khác (tag đích kind=TAG_MB_COIL) — dành cho bản Gateway |
| LOG_EVENT    | 4      | Ghi log nội bộ (timestamp+tag+giá trị)                         | DI0 đổi trạng thái bất kỳ → ghi 1 dòng log (thời điểm, DI0, giá trị mới) để tra cứu sau                                 |
| SEND_ALARM   | 5      | Set mã cảnh báo cho lớp trên đọc                               | Nhiệt độ vượt ngưỡng nguy hiểm → SET mã cảnh báo action_param=101, SCADA đọc mã này để hiển thị đúng loại lỗi           |
| ADD_TAG      | 6      | action_tag += tag_read(trigger_tag)                            | Mỗi giây cộng dồn công suất tức thời AI0 vào VREG_R (năng lượng tích luỹ thô)                                           |
| SCALE_TAG    | 7      | action_tag = tag_read(trigger_tag) × param/1000 + threshold_hi | Quy đổi tốc độ đếm xung/giây (VREG) sang lưu lượng lít/phút bằng hệ số + offset hiệu chỉnh                              |

**Ví dụ đầy đủ — ACT_SCALE_TAG (quy đổi tốc độ đếm sang lưu lượng)**

**Bài toán:** cần hiển thị lưu lượng chất lỏng (lít/phút) cho vận hành viên xem trên SCADA, nhưng cảm biến lưu lượng chỉ xuất ra dạng xung — đã có 1 rule INTERVAL riêng đếm xung/giây, ghi vào VREG10.

Công thức hiệu chuẩn thực tế của cảm biến: lít/phút = xung/giây × 2.5 − 3.

rule: trigger_tag=10 (VREG10), action_type=SCALE_TAG, action_tag=VREG11 (kết quả lít/phút), action_param=2500 (2.5×1000), threshold_hi=-3.

Kết quả: VREG11 = VREG10 × 2500/1000 + (−3) = VREG10×2.5 − 3.

**Ví dụ đầy đủ — ACT_ADD_TAG (tích luỹ theo giá trị sống)**

**Bài toán:** cần theo dõi tổng năng lượng tiêu thụ của 1 tải trong ngày, nhưng chỉ có sẵn cảm biến đo công suất tức thời (W) qua AI0, không có công tơ điện tử riêng cho tải đó.

rule: trigger_type=INTERVAL, for_ms=1000, trigger_tag=AI0 (công suất tức thời, W), action_type=ADD_TAG, action_tag=VREG_R (năng lượng tích luỹ).

Mỗi giây cộng dồn 1 lần — kết quả ra đơn vị thô (W×giây), quy đổi sang kWh ở lớp SCADA/HMI phía trên.

KHÔNG dùng ACT_ADD_TAG để cộng nhiều bộ đếm xung độc lập vào 1 tổng — sẽ đếm lặp; dùng ACT_INC_COUNTER (mẫu bên dưới).

**Mẫu thiết kế bắt buộc tuân theo — Cộng dồn nhiều nguồn vào 1 tổng**

**Bài toán:** 2 dây chuyền đóng gói riêng biệt, mỗi dây chuyền có 1 cảm biến quang đếm sản phẩm đi qua (DI2 cho dây chuyền 1, DI3 cho dây chuyền 2). Quản lý muốn xem được sản lượng riêng từng dây chuyền VÀ tổng sản lượng cả 2 dây chuyền cộng lại, cập nhật thời gian thực.

R1: trigger=DI2 ON_RISE → INC_COUNTER, action_tag=Counter1, param=1

R2: trigger=DI2 ON_RISE → INC_COUNTER, action_tag=Total, param=1 (CÙNG trigger với R1)

R3: trigger=DI3 ON_RISE → INC_COUNTER, action_tag=Counter2, param=1

R4: trigger=DI3 ON_RISE → INC_COUNTER, action_tag=Total, param=1 (CÙNG trigger với R3)

Mỗi input bơm vào 2 bộ đếm CÙNG LÚC bằng 2 rule chung 1 trigger — tránh đúng lỗi đếm lặp.

# 5\. Nạp rule (chương trình người dùng) qua RS485

**Quyết định thiết kế — dùng Modbus RTU chuẩn, không làm giao thức riêng**

Tái dùng nguyên Modbus slave stack đã phải viết cho việc đọc/ghi I/O — không cần bộ phân tích khung thứ hai, không cần "chuyển chế độ" giữa Modbus và giao thức riêng.

Dùng function code chuẩn (0x10 Write Multiple Registers, 0x03 Read Holding Registers) — bất kỳ công cụ Modbus nào cũng đọc được trạng thái nạp để debug.

Đánh đổi đã chấp nhận: mỗi khung Modbus RTU chỉ mang tối đa 123 register — với rule 28 byte (14 register), cần khoảng 13 khung để nạp đủ 100 rule.

## 5.1 Vùng thanh ghi cấu hình (minh hoạ — địa chỉ cụ thể chốt cùng bảng Modbus map đầy đủ)

| **Offset (minh hoạ)** | **Tên**             | **R/W** | **Ý nghĩa**                                                        |
| --------------------- | ------------------- | ------- | ------------------------------------------------------------------ |
| 0x9000                | CONFIG_STATUS       | RO      | 0=idle, 1=đang nhận, 2=đang kiểm tra CRC, 3=sẵn sàng commit, 4=lỗi |
| 0x9001                | CONFIG_ERROR_CODE   | RO      | Mã lỗi chi tiết khi CONFIG_STATUS = 4                              |
| 0x9002                | RULE_COUNT_STAGED   | RW      | Số rule sẽ nạp                                                     |
| 0x9003–0x9004         | EXPECTED_CRC32      | RW      | CRC32 của toàn bộ dữ liệu rule                                     |
| 0x9010…               | STAGING_BUFFER      | RW      | Vùng ghi rule thô, 14 register/rule                                |
| 0xA000                | COMMIT_COMMAND      | WO      | Ghi giá trị magic 0xA5A5 để yêu cầu kiểm tra CRC và áp dụng        |
| 0xA001                | ACTIVE_RULE_VERSION | RO      | Số phiên bản rule đang chạy                                        |

## 5.2 Quy trình nạp (5 bước)

- Bước 1 — Ghi RULE_COUNT_STAGED và EXPECTED_CRC32 (Modbus FC16).
- Bước 2 — Ghi toàn bộ dữ liệu rule vào STAGING_BUFFER, chia thành nhiều khung FC16 liên tiếp.
- Bước 3 — Đọc lại CONFIG_STATUS (FC03) để xác nhận đã nhận đủ.
- Bước 4 — Ghi COMMIT_COMMAND = 0xA5A5. Thiết bị tự tính CRC32, so khớp: khớp → atomic-swap + lưu Flash + tăng version; không khớp → lỗi, bảng đang chạy giữ nguyên.
- Bước 5 — Đọc ACTIVE_RULE_VERSION để xác nhận.

Rule table đang chạy chỉ bị thay ở bước 4 — mất kết nối RS485 giữa chừng không bao giờ làm hỏng rule đang vận hành.

# 6\. Rule Engine hoạt động trong chương trình chính (Main Loop)

## 6.1 Mô hình: vòng quét cố định (Scan Cycle)

Firmware chạy theo vòng quét chu kỳ cố định: đọc input → xử lý logic → ghi output → phục vụ giao tiếp → lưu trữ bền vững → lặp lại. Ngắt phần cứng chỉ dùng cho: 1 timer tick hệ thống (mỗi 1ms) và nhận byte UART. Mọi logic khác PHẢI chạy trong vòng lặp chính, không chạy trong ISR.

## 6.2 Tag Value Store

Tag (mục 3.1) chỉ MÔ TẢ 1 tag — giá trị HIỆN TẠI nằm ở 1 mảng riêng trong RAM:

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>int32_t g_tag_value[MAX_TAGS];   // 128 x i32 = 512 byte — giá trị sống của MỌI tag</code></pre><p></p><pre><code>int32_t tag_read(uint16_t idx)              { return g_tag_value[idx]; }
void    tag_write(uint16_t idx, int32_t v)  { g_tag_value[idx] = v; }</code></pre></th></tr></tbody></table></div>

Khi mất điện/reset: g_tag_value\[\] về 0 — đúng cho DI/DO/AI/VFLAG/VREG. Với VREG_RETAIN, retain_store_restore() (mục 7) PHẢI nạp lại giá trị trước vòng quét đầu tiên.

## 6.3 Rule Runtime State

```
typedef struct {
    int32_t  prev_value;
    uint32_t condition_since_tick;
    uint32_t last_fire_tick;
} RuleRuntime;   // 12 byte/rule, KHÔNG lưu Flash — reset về 0 mỗi lần khởi động
```

## 6.4 Trình tự 1 vòng quét

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>volatile uint32_t g_system_tick_ms;</code></pre><p></p><pre><code>void main(void) {
    hal_init();
    tag_table_load_from_flash();
    rule_table_load_from_flash();
    memset(g_tag_value, 0, sizeof(g_tag_value));
    memset(g_rule_runtime, 0, sizeof(g_rule_runtime));
    retain_store_restore();
    modbus_slave_init();
    pvd_init();
    watchdog_init();</code></pre><p></p><pre><code>    while (1) {
        uint32_t scan_start = g_system_tick_ms;
        input_scan();
        rule_scan();
        output_scan();
        modbus_service();
        retain_service();
        watchdog_kick();
        while (g_system_tick_ms - scan_start &lt; SCAN_PERIOD_MS) { /* chờ đủ chu kỳ */ }
    }
}</code></pre></th></tr></tbody></table></div>

SCAN_PERIOD_MS: 10 ms.

## 6.5 Rule Scan

rule_scan() là hàm trung tâm của Rule Engine — nó biến Rule Table (dữ liệu tĩnh 28 byte/rule nạp từ Flash) thành hành vi thời gian thực. Đặt tên "scan" chứ không phải "evaluate" để nhất quán với input_scan()/output_scan() (cùng là 3 giai đoạn của 1 vòng quét, mục 6.4) và phản ánh đúng việc hàm này làm — không chỉ XÉT rule mà còn THỰC THI rule đạt điều kiện, giống cách input_scan() không chỉ đọc mà còn ghi vào Tag Value Store.

Firmware gọi hàm này đúng 1 lần mỗi vòng quét (10ms), giữa input_scan() và output_scan(): với MỖI rule trong bảng, hàm chạy qua 1 chuỗi kiểm tra liên tiếp — Trigger (đổi cạnh?) → Compare (đạt ngưỡng?) → Dwell/debounce (giữ đủ lâu?) → Guard (được phép chạy?) — chỉ cần 1 bước không đạt là bỏ qua rule đó ngay lập tức (continue), chuyển sang rule kế tiếp mà không tốn công kiểm tra các bước còn lại. Chỉ khi TẤT CẢ các bước đều đạt, hàm mới gọi execute_action() để thực sự thực thi rule.

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>void rule_scan(void) {
    for (int i = 0; i &lt; g_rule_count; i++) {
        Rule *r = &amp;g_rule_table[i];
        RuleRuntime *rt = &amp;g_rule_runtime[i];
        if (!r-&gt;enabled) continue;</code></pre><p></p><pre><code>        int32_t current = tag_read(r-&gt;trigger_tag);
        bool edge = check_trigger_edge(r-&gt;trigger_type, rt-&gt;prev_value, current);
        rt-&gt;prev_value = current;
        if (!edge) continue;</code></pre><p></p><pre><code>        if (r-&gt;compare_op != OP_NONE &amp;&amp; !compare_ok(r-&gt;compare_op, current, r-&gt;threshold_lo, r-&gt;threshold_hi))
            continue;</code></pre><p></p><pre><code>        if (r-&gt;for_ms &gt; 0) {
            if (rt-&gt;condition_since_tick == 0) rt-&gt;condition_since_tick = g_system_tick_ms;
            if (g_system_tick_ms - rt-&gt;condition_since_tick &lt; r-&gt;for_ms) continue;
        }</code></pre><p></p><pre><code>        uint16_t guard_idx = r-&gt;guard_tag &amp; 0x7FFF;
        bool     negate    = r-&gt;guard_tag &amp; 0x8000;
        if (guard_idx != TAG_NONE) {
            int32_t gval = tag_read(guard_idx);
            bool gate_open = negate ? (gval == 0) : (gval != 0);
            if (!gate_open) continue;
        }</code></pre><p></p><pre><code>        execute_action(r);
        rt-&gt;condition_since_tick = 0;
    }
}</code></pre></th></tr></tbody></table></div>

| **Bước trong vòng lặp** | **Việc kiểm tra**                                                     | **Nếu không đạt**                        |
| ----------------------- | --------------------------------------------------------------------- | ---------------------------------------- |
| enabled                 | Rule có đang bật không                                                | Bỏ qua rule                              |
| Trigger                 | trigger_tag có vừa đổi đúng kiểu cạnh (ON_RISE/FALL/CHANGE/...) không | Bỏ qua, chờ vòng quét sau                |
| Compare                 | Giá trị hiện tại có thoả compare_op/threshold không (nếu có khai)     | Bỏ qua                                   |
| Dwell (for_ms)          | Điều kiện đã giữ liên tục đủ lâu chưa (nếu có khai)                   | Bỏ qua, tiếp tục đếm giờ                 |
| Guard                   | Tag phụ (nếu có khai) có đang "cho phép" không                        | Bỏ qua                                   |
| execute_action()        | — tất cả đã đạt —                                                     | Thực thi action, ghi vào Tag Value Store |

**Ghi chú kiến trúc — execute_action() không đụng phần cứng**

execute_action() chỉ ghi kết quả vào Tag Value Store (RAM) — KHÔNG ghi trực tiếp ra chân GPIO. Việc đẩy giá trị DO ra chân vật lý là việc riêng của output_scan(), chạy SAU rule_scan() trong cùng vòng quét (mục 6.4). Đây là lý do thứ tự rule trong bảng ảnh hưởng độ trễ phản ứng chuỗi (mục 6.6).

Hàm chạy tuần tự trong vòng lặp chính, không chạy trong ngắt — không cần khoá (mutex) khi đọc/ghi Rule Table hay Tag Value Store (mục 6.1).

Chi phí CPU: dù quét đủ 100 rule mỗi 10ms bất kể có nổ hay không, tổng chi phí ước tính chỉ vài chục µs — dưới 1% ngân sách 1 vòng quét (mục 2.2).

## 6.6 Thứ tự rule quyết định độ trễ phản ứng chuỗi

Action ghi thẳng vào Tag Value Store — rule đứng SAU đọc được ngay giá trị rule đứng TRƯỚC vừa ghi, trong CÙNG 1 vòng quét. Độ trễ thêm tối đa do thứ tự rule chưa tối ưu là 1 vòng quét (10ms) — không đáng kể so với phản xạ con người.

## 6.7 Watchdog & tương thích Gateway

watchdog_kick() mỗi vòng quét (IWDG) tự reset MCU nếu vòng lặp treo — bắt buộc cho thiết bị công nghiệp không người trông coi. Với tag TAG_MB_\* (bản Gateway), input_scan() KHÔNG đọc đồng bộ — cần 1 tiến trình Modbus Master polling riêng, Rule Engine chỉ đọc cache qua tag_read() không đổi API.

# 7\. Lưu trữ bền vững (Retentive Storage)

Tương đương nhóm thanh ghi "retentive/latching" của PLC truyền thống — giữ giá trị qua mất điện cho các tag VREG_RETAIN (mục 3).

**Quyết định thiết kế — snapshot định kỳ + phát hiện sụt áp (PVD), không dùng FRAM ngoài**

Dùng Flash nội bộ có sẵn, giữ BOM đơn giản — đánh đổi đã chấp nhận: có thể mất tối đa vài phút dữ liệu gần nhất nếu rút điện đột ngột KHÔNG kịp phát hiện sụt áp.

Bù lại bằng PVD (mục 7.3) — hầu hết mất điện thực tế có vài ms sụt áp trước khi mất hẳn, đủ để lưu khẩn cấp 1 lần trước khi mất.

## 7.1 Cơ chế: snapshot định kỳ kiểu EEPROM-emulation

```
typedef struct {
    uint32_t seq_num;
    uint16_t count;
    uint16_t crc16;
    // theo sau: count x { uint16_t tag_index; int32_t value; }   -- 6 byte/mục
} RetainSnapshotHeader;   // 8 byte header + 6 byte/tag retain
```

| **Thông số**            | **Giá trị**                                             |
| ----------------------- | ------------------------------------------------------- |
| Vùng Flash dành riêng   | 64 KB (8 sector × 8 KB)                                 |
| Chu kỳ snapshot định kỳ | 5 phút, cấu hình được qua Modbus                        |
| Tuổi thọ Flash ước tính | ~48 năm — dư nhiều so với vòng đời sản phẩm (10-15 năm) |

<div class="joplin-table-wrapper"><table><tbody><tr><th><pre><code>#define RETAIN_SNAPSHOT_PERIOD_MS   (5 * 60 * 1000)</code></pre><p></p><pre><code>void retain_service(void) {
    static uint32_t last_snapshot_tick = 0;
    if (g_system_tick_ms - last_snapshot_tick &gt;= RETAIN_SNAPSHOT_PERIOD_MS) {
        retain_snapshot_write();
        last_snapshot_tick = g_system_tick_ms;
    }
}</code></pre></th></tr></tbody></table></div>

## 7.2 Khôi phục lúc khởi động

retain_store_restore() quét toàn bộ vùng retain store, tìm bản ghi có seq_num LỚN NHẤT và CRC hợp lệ, nạp giá trị vào g_tag_value\[\] cho các tag VREG_RETAIN. Không tìm thấy bản ghi hợp lệ (lần đầu boot) → để 0.

Cơ chế xoay vòng: không lưu con trỏ/địa chỉ sector riêng ở đâu cả — mỗi bản ghi kích thước cố định 104 byte (8 byte header + 16 tag × 6 byte), nên 1 sector 8KB chứa được ~78 bản ghi. Vị trí ghi hiện tại được suy ra lại mỗi lần khởi động, không cần lưu trữ bền vững:

- Quét toàn bộ 64KB, tìm bản ghi CRC hợp lệ có seq_num lớn nhất → sector chứa nó là sector đang ghi (active sector).
- Trong sector đó, slot đầu tiên không hợp lệ (vùng Flash trắng, chưa ghi) chính là vị trí ghi kế tiếp.
- Khi sector active hết slot trống → erase sector kế tiếp theo thứ tự vòng (0→1→...→7→0) rồi ghi tiếp từ đó; sector này luôn là sector cũ nhất nên an toàn để xoá.

## 7.3 Phát hiện sụt áp (PVD) — lưới an toàn cho mất điện đột ngột

```
void PVD_IRQHandler(void) {
    retain_snapshot_write();   // ghi khẩn cấp NGAY LẬP TỨC, không chờ chu kỳ 5 phút
}
```

**Yêu cầu bàn giao cho đội thiết kế điện**

Cần đủ tụ dự trữ trên đường nguồn VDD để MCU hoàn tất 1 lần ghi Flash sau khi PVD báo động nhưng trước khi mất điện hẳn.

Công thức ước lượng: C = I × Δt / ΔV — I và Δt phải đo thực tế trên board mẫu, không tính bằng lý thuyết.

# 8\. Phạm vi tài liệu & hướng phát triển tiếp theo

## 8.1 Không thuộc phạm vi phiên bản này

| **Tính năng**                                                                 | **Lý do chưa đặc tả trong tài liệu này**                                                                                                |
| ----------------------------------------------------------------------------- | --------------------------------------------------------------------------------------------------------------------------------------- |
| Hiệu chuẩn kênh AI (AIScaleConfig — raw ADC → giá trị đo, kiểu 4-20mA/Pt100)  | Xác định là cấu hình cơ bản của thiết bị (giống mọi Remote I/O thương mại), KHÔNG PHẢI việc của Rule Engine — sẽ đặc tả thành mục riêng |
| Ánh xạ tag ra thanh ghi Modbus (Modbus Register Map cho SCADA)                | Cùng lý do trên — cấu hình cơ bản, tách khỏi màn hình tạo Rule                                                                          |
| Modbus Master polling cho tag TAG_MB_\* (đọc thiết bị khác trên bus)          | Chỉ cần cho bản Gateway — kiến trúc đã chừa chỗ (mục 6.7) nhưng chưa cần cho Remote I/O                                                 |
| OTA cập nhật toàn bộ firmware qua mạng                                        | Bản đầu nạp qua ST-Link/UART tại xưởng là đủ                                                                                            |
| MCU cho các dòng sản phẩm khác (Datalogger/Gateway/bộ điều khiển chuyên dụng) | STM32H523 chỉ xác nhận cho Remote I/O — MCU dòng khác đánh giá riêng khi bắt đầu thiết kế (mục 2.2)                                     |

## 8.2 Tài liệu liên quan

- IIoT_Firmware_Core_Architecture.pptx — kiến trúc phân lớp tổng quan (không đi vào chi tiết struct/firmware).
- SimplePLC_Firmware_Engineering_Overview.pptx — trình bày kỹ thuật trực quan cho đội phát triển.
- SimplePLC_Config_UI_Wireframes.pptx — ý tưởng giao diện cấu hình cho kỹ sư lắp đặt.
- SimplePLC_Worked_Examples_UI_to_MCU.pptx — 3 ví dụ đầy đủ từ giao diện tới thực thi trên MCU.