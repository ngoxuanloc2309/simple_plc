# SPLC-SPEC-MATH-007 – Thiết kế firmware khối MATH (action_type = 8)

Bản v0.3 – 20/11/2026 – đã thống nhất với đội App. Các điểm chưa có phản hồi của App ghi ở mục 9.2; firmware code theo hướng mặc định ghi ở đó, không chờ phản hồi.
Đường dẫn trong repo: `simple_plc/docs/SPLC_SPEC_MATH007.md` (nhánh `use_mb_new`).

Trạng thái: **chưa viết code, chưa build ARM, chưa chạy board.** Mọi hành vi dưới đây là thiết kế.
Các bảng vector ở mục 8 được tính bằng chương trình tham chiếu viết theo mục 4 và mục 7, chạy trên host (x86). Đó chưa phải `plc_math.c` và chưa phải ARM.

Nhãn: **[ĐÃ THỐNG NHẤT]** hai bên đã đồng ý. **[FIRMWARE]** firmware tự quyết, App chỉ cần biết. **[MẶC ĐỊNH]** App chưa phản hồi; firmware code theo hướng mặc định, đổi sau nếu App trả lời khác (mục 9.2).

---

## 1. Phạm vi và nguyên tắc

- Khối MATH tính `OUT = Op1 <toán tử> Op2` mỗi scan, trên số nguyên 32-bit có dấu (INT32) hoặc float32 (REAL).
- Toán tử: `ADD=0 SUB=1 MUL=2 DIV=3 MOD=4`. Không có MIN/MAX/CLAMP. **[ĐÃ THỐNG NHẤT]**
- Option biên dịch (đặt trong `config/splc_opt.h`) **[FIRMWARE]**:
  - `SPLC_MATH_ENABLE`: thư viện mặc định **1**. Khi đặt 0, binary phải giống hệt bản trước khi có MATH (so md5 file `.bin`). Sản phẩm không tự đặt option sẽ nhận code MATH nên binary khác bản cũ; muốn so md5 với bản cũ phải đặt `SPLC_MATH_ENABLE 0` tường minh.
  - Yêu cầu `SPLC_REAL_ENABLE=1`, nếu không `#error` lúc biên dịch (trường `flags` chỉ tồn tại khi REAL bật; mặc định của thư viện là `SPLC_REAL_ENABLE 1`). Sản phẩm đặt `SPLC_REAL_ENABLE 0` phải đặt `SPLC_MATH_ENABLE 0`.
  - `SPLC_MATH_FAULT_HOLD_MS`: thời gian giữ health bit 3 (mục 6), mặc định **3000**.
- Không cấp phát heap, không dùng bẫy chia 0 của CPU.
- MOD float dùng hàm `splc_fmodf` (lấy từ musl libc, giấy phép MIT), đặt `static` trong `plc_math.c`, nên không phụ thuộc libm. **[FIRMWARE]**

## 2. Hợp đồng wire (bản ghi rule 32 byte = 16 thanh ghi, Big-Endian)

| Thanh ghi | Trường | Ý nghĩa với MATH |
|---|---|---|
| Reg 0..5 | threshold_lo/hi, for_ms | Firmware bỏ qua. App nên ghi 0. Vẫn được lưu và trả nguyên văn như mọi rule. |
| Reg 6..7 | action_param | Firmware bỏ qua và **không kiểm tra**. **App ghi 0** (không trùng aux). **[ĐÃ THỐNG NHẤT]** |
| Reg 8 | trigger_tag | Chỉ số tag Op1. |
| Reg 9 | action_tag | Chỉ số tag OUT. |
| Reg 10 | guard_tag | Chỉ số tag Op2 (lấy `giá trị & 0x7FFF`, bit 15 bị bỏ qua). `0x7FFF` = Op2 là hằng, lấy từ aux. |
| Reg 11 | enabled / trigger_type | `enabled` (byte cao) được tôn trọng. `trigger_type` bị bỏ qua. |
| Reg 12 | compare_op / action_type | Byte cao = toán tử 0..4. Byte thấp = 8. |
| Reg 13 | flags / reserved | Byte cao: `0x00` = INT32, `0x03` = REAL; giá trị khác bị từ chối lúc COMMIT (mục 5). Byte thấp: **lưu nguyên văn**, App ghi 0. |
| Reg 14..15 | aux | Hằng Op2 khi `guard_tag == 0x7FFF`. int32 hoặc float32 IEEE-754; **Reg 14 = word cao, Reg 15 = word thấp**. Op2 là tag thì App ghi 0. |

Điểm quan trọng về CRC:

- Firmware tính CRC-16/MODBUS trên ảnh wire 32 byte của từng rule. **Hiện tại** firmware ghi Reg 13 byte thấp và Reg 14..15 là 0 trong ảnh này, và đọc lại cũng ra 0.
- Hệ quả (suy từ đọc code, chưa chạy): rule có Reg 14..15 khác 0 sẽ lệch CRC giữa hai bên và COMMIT trả lỗi CRC (mã 4).
- Thiết kế mới: khi `SPLC_MATH_ENABLE=1`, firmware **lưu và trả nguyên văn** Reg 13 byte thấp và Reg 14..15 của mọi rule, qua đủ các chặng: ghi staging, đọc staging, đọc active, tính CRC, lưu Flash, nạp lại Flash. Định dạng Flash không đổi (đang lưu đúng ảnh wire 32 byte/rule), bản ghi cũ có 0 ở các byte này vẫn nạp được.
- Khi `SPLC_MATH_ENABLE 0` hoặc firmware cũ: các byte này bị bỏ và đọc ra 0. Rule MATH có hằng sẽ bị lỗi CRC. **App phải đọc capability bit 16 trước khi nạp rule MATH** (mục 6).

## 3. Ngữ nghĩa thực thi

1. Vị trí trong chu kỳ quét: `input_scan` → system tags → MQTT SUB → **`rule_scan`** → function block → `output_scan`. MATH nằm trong `rule_scan`, nên nhìn thấy tag MQTT SUB vừa ghi trong cùng scan.
2. Rule chạy **tuần tự theo thứ tự trong bảng**, ghi tag ngay. Rule đứng sau thấy kết quả rule đứng trước trong cùng scan. **[MẶC ĐỊNH: Q6]** Thứ tự rule = thứ tự thực thi: App xếp rule tạo kết quả trung gian đứng trước rule dùng nó. Firmware không tự sắp xếp. Nếu đảo thứ tự, kết quả trễ 1 scan (10 ms).
3. MATH là tổ hợp, không có trạng thái. Mỗi scan, rule bật đều được tính. Không dùng `trigger_type`, `for_ms`, ngưỡng, `compare_op` kiểu so sánh, guard như điều kiện chặn.
4. `enabled = 0`: bỏ qua hoàn toàn (không ghi tag, không báo lỗi, không kiểm tra lúc COMMIT).
5. Khi một phiên chẩn đoán (DIAG_CONTROL) đang giữ engine, `rule_scan` không chạy: ngõ ra giữ nguyên. Khi engine chạy lại, trạng thái runtime của rule được reset, MATH không bị ảnh hưởng vì không có trạng thái.
6. Op1 và Op2 có thể là tag thường hoặc **tag hệ thống (1000..1007, chỉ đọc)**. OUT phải là tag trong bảng tag (mục 5).
7. Hai toán hạng luôn cùng kiểu theo `flags`. Firmware **không** đổi kiểu toán hạng; việc đổi INT32 sang REAL do App dựng bằng một rule SCALE_TAG đứng trước (mục 7).
8. Không ghi log mỗi scan (100 lần/giây).

## 4. Quy tắc số học **[ĐÃ THỐNG NHẤT]**

Kiểu chọn theo byte cao Reg 13: `0x00` = INT32, `0x03` = REAL.

| Toán tử | INT32 | REAL (float32) |
|---|---|---|
| ADD | Quấn mod 2³² (tính bằng `uint32_t` rồi ép về `int32_t`). | IEEE-754. |
| SUB | Quấn mod 2³². | IEEE-754. |
| MUL | **Kẹp** `[INT32_MIN, INT32_MAX]` (tính qua `int64_t`). | IEEE-754. |
| DIV | `Op2 == 0` → lỗi. `INT32_MIN / -1` → ghi `INT32_MAX` và báo lỗi. Còn lại chia cắt về 0 (C99). | `Op2 == 0.0f` (kể cả `-0.0f`) → lỗi. Còn lại `Op1 / Op2`. |
| MOD | `Op2 == 0` → lỗi. `INT32_MIN % -1` → 0, **không** lỗi. Còn lại theo C99 (dấu theo số bị chia; `-7 % 3 = -1`). | `Op2 == 0.0f` → lỗi. Còn lại `splc_fmodf` (dấu theo số bị chia). |

Quy tắc lỗi (chia 0, hoặc kết quả REAL là NaN/Inf):

- **Không ghi đè** `action_tag` (giữ giá trị cũ). Ngoại lệ duy nhất: `INT32_MIN / -1` ghi `INT32_MAX`.
- Bật health bit 3 (mục 6).
- So sánh số chia với 0 là so sánh **đúng 0**, không dùng ngưỡng (bỏ ngưỡng 1e-6 của ICD v2.0.0).
- Kết quả REAL là NaN hoặc ±Inf (ví dụ tràn `1e30 * 1e30`, `Inf - Inf`, đầu vào đã là NaN/Inf) cũng là lỗi: giữ ngõ ra cũ, bật bit 3. (ICD v2.0.0 ghi ép về 0.0; ICD v2.1.0 sẽ đổi theo mục này.)
- `splc_fmodf` cho kết quả chính xác tuyệt đối (không làm tròn), nên cách cài đặt không ảnh hưởng App.

## 5. Kiểm tra lúc COMMIT **[FIRMWARE]**

Chỉ áp dụng khi `SPLC_MATH_ENABLE=1`, chỉ với rule MATH **đang bật**. Vi phạm → từ chối **toàn bộ COMMIT**, `CONFIG_ERROR_CODE = 2` (INVALID_PARAMETER), như kiểm tra tag hiện có:

1. `compare_op > 4`.
2. Byte cao Reg 13 (`flags`) khác `0x00` và `0x03`, tức mọi giá trị khác, gồm `0x01`, `0x02`, `0x04..0x07` và các bit cao hơn. Đây là ngoại lệ so với quy ước hiện tại "COMMIT không từ chối cờ lạ"; chỉ áp cho rule MATH đang bật. Các loại rule khác vẫn lưu nguyên văn bit 2..7.
3. Op1 hoặc Op2 (khi `guard_tag & 0x7FFF != 0x7FFF`) có chỉ số ngoài bảng tag và không phải tag hệ thống (kiểm tra có sẵn; chỉ cần coi action 8 là "có dùng trigger_tag").
4. `action_tag` ngoài bảng tag, kể cả tag hệ thống (chỉ đọc).
5. DIV hoặc MOD với Op2 là **hằng bằng 0** (int 0, hoặc float +0.0/-0.0).

Không kiểm tra, cũng không từ chối:

- Reg 6..7 (firmware bỏ qua).
- `action_tag` trùng một toán hạng (ví dụ `x = x + 1`): hợp lệ, chạy mỗi scan (tăng 100 lần/giây). Nếu App muốn cảnh báo thì làm ở Studio (Q8).
- `action_tag` là VREG_RETAIN: hợp lệ. Snapshot retain chỉ ghi khi giá trị khác bản đã lưu, theo chu kỳ mặc định 5 phút (`RETAIN_SNAPSHOT_PERIOD_MS`, đổi được theo sản phẩm), tối đa 12 lần/giờ, không phụ thuộc rule nào ghi tag. **[ĐÃ THỐNG NHẤT: Q13a]**
- `action_tag` là DI/AI: firmware không từ chối, nhưng `input_scan` ghi đè mỗi scan nên rule vô tác dụng. App chặn trong Studio. **[ĐÃ THỐNG NHẤT: Q13a]**
- Kiểu tag OUT so với `flags` (Q13b, mục 9.2).

## 6. Chẩn đoán và khai báo khả năng

- **`DEVICE_HEALTH.health_flags` (0x0803) bit 3 = `MATH_FAULT`.** **[ĐÃ THỐNG NHẤT: Q10]** Bit 3 báo "có phép MATH tính lỗi gần đây" (chia 0, `INT32_MIN / -1`, kết quả REAL là NaN/Inf).
  - Không dính. Bật khi có lỗi MATH trong `SPLC_MATH_FAULT_HOLD_MS` gần nhất (mặc định 3000 ms), tự hạ sau đó nếu không có lỗi mới. Một lỗi có thể chỉ kéo dài một scan (10 ms), nên bit phải giữ đủ lâu để SCADA/App đọc được. Studio hỏi mỗi 100 ms (tối đa 50 ms); thời gian giữ 3000 ms chừa chỗ cho SCADA hỏi chậm hơn. App đã chấp nhận 1000 ms hoặc 3000 ms; sản phẩm nào cần ngắn hơn thì đổi `SPLC_MATH_FAULT_HOLD_MS`.
  - Không có thanh ghi đếm riêng. Bản ICD cũ nhắc `0x0A24`; **firmware không dùng**, ICD v2.1.0 sẽ bỏ.
  - Cách làm: engine lưu thời điểm lỗi cuối (lấy từ `now_ms` đã truyền vào `rule_scan`, so bằng số học unsigned), tầng Modbus OR bit 3 vào giá trị đọc.
  - Hiện chưa có code nào ghi `health_flags` (bit 0..2 chỉ mới khai báo); đây là chỗ ghi đầu tiên.
- **`HARDWARE_CAPABILITIES` (0x0A08) bit 16 (`0x00010000`) = MATH**, chỉ khai khi `SPLC_MATH_ENABLE=1`. Bit này chưa dùng trong firmware. Bit 16 cũng cho biết thiết bị hiểu cờ `INT_TO_FLOAT` của SCALE_TAG (mục 7).
- Khi `SPLC_MATH_ENABLE 0`: rule action 8 vẫn được nhận và lưu (nhánh `default` của engine không làm gì), nhưng không chạy; riêng rule có hằng sẽ lỗi CRC như mục 2. Mã lỗi 5 (UNSUPPORTED) **không** được firmware dùng ở đâu hiện nay và không thể thêm khi option tắt mà không đổi binary. **App phải dựa vào capability bit 16, không dựa vào lỗi COMMIT.**

## 7. Mở rộng SCALE_TAG: cờ `INT_TO_FLOAT` **[ĐÃ THỐNG NHẤT: Q1, phương án C]**

Mục đích: đưa giá trị INT32 (ví dụ mã ADC thô `100`) thành float32 để dùng làm toán hạng REAL của MATH. Compiler của App sinh một rule SCALE_TAG trung gian (gain 1.0, offset 0.0) ghi vào một VREG thường (không RETAIN), rồi MATH dùng VREG đó.

**Layout rule SCALE_TAG (`action_type = 7`) chế độ float:**

| Thanh ghi | Trường | Ý nghĩa |
|---|---|---|
| Reg 8 | trigger_tag | Tag nguồn `x`. |
| Reg 9 | action_tag | Tag đích. |
| Reg 2..3 | threshold_hi | **Offset**, bit float32. |
| Reg 6..7 | action_param | **Gain**, bit float32. |
| Reg 13 (byte cao) | flags | `0x03` float, nguồn là bit float32 (như hiện nay). `0x07` float, nguồn là INT32 (mới). |
| Reg 14..15 | aux | Không dùng. |

Lưu ý: khác với MATH, micro-rule SCALE có Reg 6..7 là bit của gain (ví dụ 1.0 = `0x3F800000`), không phải 0.

**Quy tắc:**

- `(flags & 0x07) == 0x07`: `x = (float)(int32_t)tag_read(trigger_tag)`, `y = x * gain + offset`, ghi bit float32 của `y` vào `action_tag`. Cách tính tích và cộng giữ như SCALE float hiện có (tích qua biến `volatile` để không bị gộp thành FMA; NaN/Inf ghi nguyên như SCALE hiện nay).
- `(flags & 0x03) == 0x03` và bit 2 = 0: như hiện nay (nguồn là bit float32).
- Mọi tổ hợp khác (kể cả bit 2 đứng một mình): SCALE số nguyên như hiện nay, bit 2 bị bỏ qua.
- `(float)` làm tròn về gần nhất khi `|x| > 2²⁴ = 16 777 216` (vector S-03).
- Nhánh `0x07` chỉ có trong build `SPLC_MATH_ENABLE=1`. Thiết bị không khai bit 16 coi `0x07` như `0x03`: đọc giá trị nguyên như bit float, ra số rác mà không báo lỗi. **App chỉ phát `0x07` khi thiết bị khai capability bit 16.**
- Rule MATH không bao giờ mang `0x07` (COMMIT từ chối, mục 5 ý 2).

## 8. Vector kiểm thử **[MẶC ĐỊNH: Q2]**

Mọi vector chạy từ giá trị `OUT trước` ghi trong bảng. Cột "Bit 3" cho biết health bit 3 có bật trong scan đó hay không. "Op2 (tag)" nghĩa là Op2 lấy từ một tag đang mang giá trị đó. Bit float32 viết dạng hex Big-Endian `0xAABBCCDD`.

Các bảng này do firmware tính bằng chương trình tham chiếu trên host và dùng để test host ngay, không chờ App. App đối chiếu với bộ vector của mình; chỗ nào khác nhau thì thêm hoặc sửa dòng tương ứng. Bảng chưa gắn chỉ số tag và giá trị khởi tạo tag cụ thể; khi App gửi bộ vector của họ thì hai bên cùng chọn chỉ số tag theo layout board (Q7).

### 8.1 INT32 (`flags = 0x00`, `OUT trước = 777`)

| # | Toán tử | Op1 | Op2 (tag) | OUT trước | OUT sau | Bit 3 | Ghi chú |
|---|---|---|---|---|---|---|---|
| I-01 | ADD | 100 | 23 | 777 | 123 | 0 | Cơ bản. |
| I-02 | ADD | 2000000000 | 2000000000 | 777 | -294967296 | 0 | Quấn mod 2³² (`0xEE6B2800`). |
| I-03 | SUB | -2000000000 | 2000000000 | 777 | 294967296 | 0 | Quấn mod 2³². |
| I-04 | SUB | 5 | 8 | 777 | -3 | 0 | Kết quả âm. |
| I-05 | MUL | 6 | 7 | 777 | 42 | 0 | Cơ bản. |
| I-06 | MUL | 1000000000 | 10 | 777 | 2147483647 | 0 | Kẹp `INT32_MAX`. |
| I-07 | MUL | -1000000000 | 10 | 777 | -2147483648 | 0 | Kẹp `INT32_MIN`. |
| I-08 | DIV | 7 | 2 | 777 | 3 | 0 | Chia cắt về 0. |
| I-09 | DIV | -7 | 2 | 777 | -3 | 0 | Chia cắt về 0 (C99). |
| I-10 | DIV | 5 | 0 | 777 | 777 | 1 | Số chia là tag bằng 0: giữ ngõ ra, bật bit 3. |
| I-11 | DIV | -2147483648 | -1 | 777 | 2147483647 | 1 | Ghi `INT32_MAX` và bật bit 3. |
| I-12 | MOD | 7 | 3 | 777 | 1 | 0 | Cơ bản. |
| I-13 | MOD | -7 | 3 | 777 | -1 | 0 | Dấu theo số bị chia (C99). |
| I-14 | MOD | 7 | -3 | 777 | 1 | 0 | Dấu theo số bị chia (C99). |
| I-15 | MOD | -2147483648 | -1 | 777 | 0 | 0 | Trả 0, **không** lỗi. |
| I-16 | MOD | 5 | 0 | 777 | 777 | 1 | Số chia là tag bằng 0: giữ ngõ ra, bật bit 3. |

### 8.2 REAL (`flags = 0x03`, `OUT trước = 777.0` = `0x44424000`)

| # | Toán tử | Op1 | Op2 (tag) | OUT trước | OUT sau | Bit 3 | Ghi chú |
|---|---|---|---|---|---|---|---|
| R-01 | ADD | 1.5 (`0x3FC00000`) | 2.25 (`0x40100000`) | 777 (`0x44424000`) | 3.75 (`0x40700000`) | 0 | Cơ bản. |
| R-02 | SUB | 10 (`0x41200000`) | 0.25 (`0x3E800000`) | 777 (`0x44424000`) | 9.75 (`0x411C0000`) | 0 | Cơ bản. |
| R-03 | MUL | 23.5 (`0x41BC0000`) | 2 (`0x40000000`) | 777 (`0x44424000`) | 47 (`0x423C0000`) | 0 | Cơ bản. |
| R-04 | DIV | 1 (`0x3F800000`) | 3 (`0x40400000`) | 777 (`0x44424000`) | 0.333333343 (`0x3EAAAAAB`) | 0 | Làm tròn float32. |
| R-05 | DIV | 1 (`0x3F800000`) | 0 (`0x00000000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Số chia +0.0: giữ ngõ ra, bật bit 3. |
| R-06 | DIV | 1 (`0x3F800000`) | -0.0 (`0x80000000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Số chia -0.0: giữ ngõ ra, bật bit 3 (so đúng 0). |
| R-07 | MUL | ≈1e30 (`0x7149F2CA`) | ≈1e30 (`0x7149F2CA`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Tràn ra +Inf: giữ ngõ ra, bật bit 3. |
| R-08 | DIV | ≈1e30 (`0x7149F2CA`) | 1e-30 (`0x0DA24260`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Tràn ra +Inf: giữ ngõ ra, bật bit 3. |
| R-09 | ADD | +Inf (`0x7F800000`) | -Inf (`0xFF800000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | +Inf + -Inf = NaN: giữ ngõ ra, bật bit 3. |
| R-10 | ADD | NaN (`0x7FC00000`) | 1 (`0x3F800000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Đầu vào NaN: giữ ngõ ra, bật bit 3. |
| R-11 | MOD | 7.5 (`0x40F00000`) | 2 (`0x40000000`) | 777 (`0x44424000`) | 1.5 (`0x3FC00000`) | 0 | `fmodf`. |
| R-12 | MOD | -7.5 (`0xC0F00000`) | 2 (`0x40000000`) | 777 (`0x44424000`) | -1.5 (`0xBFC00000`) | 0 | Dấu theo số bị chia. |
| R-13 | MOD | 5 (`0x40A00000`) | 0 (`0x00000000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | Số chia bằng 0: giữ ngõ ra, bật bit 3. |
| R-14 | MOD | +Inf (`0x7F800000`) | 2 (`0x40000000`) | 777 (`0x44424000`) | 777 (`0x44424000`) | 1 | +Inf mod 2.0 = NaN: giữ ngõ ra, bật bit 3. |

### 8.3 Op2 là hằng (`guard_tag = 0x7FFF`, hằng nằm ở Reg 14..15)

| # | Kiểu | Toán tử | Op1 | `guard_tag` | Reg 14 | Reg 15 | Op2 hằng | OUT sau |
|---|---|---|---|---|---|---|---|---|
| K-01 | INT32 | ADD | 10 | `0x7FFF` | `0x0000` | `0x0005` | 5 | 15 |
| K-02 | REAL | MUL | 4.0 | `0x7FFF` | `0x4020` | `0x0000` | 2.5 | 10 (`0x41200000`) |
| K-03 | INT32 | SUB | 0 | `0x7FFF` | `0xFFFF` | `0xFFFF` | -1 | 1 |

### 8.4 SCALE_TAG với `INT_TO_FLOAT` (mục 7)

| # | `flags` | Nguồn (int32) | Gain (Reg 6..7) | Offset (Reg 2..3) | Đích (float32) | Ghi chú |
|---|---|---|---|---|---|---|
| S-01 | `0x07` | 100 | 1 (`0x3F800000`) | 0 (`0x00000000`) | 100 (`0x42C80000`) | Micro-rule đổi kiểu của compiler (gain 1, offset 0). |
| S-02 | `0x07` | -5 | 2 (`0x40000000`) | 0.5 (`0x3F000000`) | -9.5 (`0xC1180000`) | Gain và offset khác mặc định. |
| S-03 | `0x07` | 16777217 | 1 (`0x3F800000`) | 0 (`0x00000000`) | 16777216 (`0x4B800000`) | Vượt 2²⁴: `(float)` làm tròn về gần nhất. |

### 8.5 COMMIT (mục 5)

| # | Rule MATH đang bật có | Kết quả |
|---|---|---|
| C-01 | `flags = 0x01` | Từ chối, mã 2 |
| C-02 | `flags = 0x02` | Từ chối, mã 2 |
| C-03 | `flags = 0x04` | Từ chối, mã 2 |
| C-04 | `flags = 0x07` | Từ chối, mã 2 |
| C-05 | `compare_op = 5` | Từ chối, mã 2 |
| C-06 | DIV, INT32, Op2 hằng = 0 | Từ chối, mã 2 |
| C-07 | DIV, REAL, Op2 hằng = -0.0 (`aux = 0x80000000`) | Từ chối, mã 2 |
| C-08 | MOD, INT32, Op2 hằng = 0 | Từ chối, mã 2 |
| C-09 | `action_tag` là tag hệ thống (ví dụ 1000) | Từ chối, mã 2 |
| C-10 | `trigger_tag` ngoài bảng tag, không phải tag hệ thống | Từ chối, mã 2 |
| C-11 | Rule `enabled = 0` mang `compare_op = 5` hoặc `flags = 0x01` | Chấp nhận (rule tắt không bị kiểm) |
| C-12 | Reg 6..7 khác 0 | Chấp nhận (firmware bỏ qua) |

## 9. Quyết định và các điểm chờ App xác nhận

### 9.1 Nhật ký quyết định

| # | Quyết định | Kết quả | Bên |
|---|---|---|---|
| D1 | Cờ `flags` của MATH | Chỉ `0x00` hoặc `0x03`; khác thì COMMIT mã 2 | Firmware |
| D2 | Tràn int32 (Q4) | ADD/SUB quấn mod 2³², MUL kẹp qua int64 | Hai bên |
| D3 | `INT32_MIN % -1` (Q11) | Trả 0, không lỗi. `INT32_MIN / -1` ghi `INT32_MAX` và bật bit 3 | Hai bên |
| D4 | NaN/Inf, chia 0 (Q12) | Giữ ngõ ra cũ và bật bit 3 | Hai bên |
| D5 | `SPLC_MATH_ENABLE` | Mặc định 1 trong thư viện; yêu cầu `SPLC_REAL_ENABLE=1` | Firmware |
| D6 | MOD float | `splc_fmodf` (musl, MIT), `static` trong `plc_math.c` | Firmware |
| D7 | Loại tag OUT (Q13a) | Firmware không thêm hạn chế; App chặn DI/AI/tag hệ thống ở compile; VREG_RETAIN cho phép | Hai bên |
| D8 | Health bit 3 (Q10) | Không dính; giữ `SPLC_MATH_FAULT_HOLD_MS` (mặc định 3000); không có thanh ghi đếm `0x0A24` | Hai bên (số 3000 do firmware chọn, App chấp nhận 1000 hoặc 3000) |
| D9 | INT32 → REAL (Q1) | Phương án C: cờ `INT_TO_FLOAT = 0x04`, SCALE_TAG `flags = 0x07` | Hai bên |
| D10 | Quy trình giao | Sửa ở simple_plc; đồng bộ sang `SynaptixSDK/libs/simple_plc` của meshwf theo từng file; ICD v2.1.0 giao dạng file | Firmware |
| D11 | Các điểm App chưa phản hồi (Q2, Q5, Q6, Q7, Q8, Q9, Q13b, Q14) | Firmware code theo hướng mặc định ở mục 9.2, không chờ App | Firmware |

### 9.2 Các điểm App chưa phản hồi: firmware code theo hướng mặc định

Quyết định D11: không để việc code chờ App. Mỗi điểm dưới đây firmware chọn hướng dễ nhất và code luôn; nếu App trả lời khác thì sửa theo cột phải. Các điểm này vẫn gửi App để xác nhận.

| Câu hỏi | Hướng mặc định của firmware | Nếu App trả lời khác |
|---|---|---|
| **Q13(b)** kiểu tag OUT so với `flags` | **Firmware không kiểm.** App tự bảo đảm khai đúng kiểu trong cùng lần Deploy: `flags = 0x03` thì tag OUT khai REAL, `flags = 0x00` thì khai INT32. (Rule Engine không tra bảng kiểu `0x0080`; kiểu tính do `flags` của rule quyết định. Khai sai thì nơi đọc tag hiểu bit float thành số nguyên và ngược lại, chỉ ảnh hưởng giá trị hiển thị hoặc publish.) | Hướng này khó đổi sau nhất: COMMIT phải đối chiếu bảng kiểu `0x0080` với bảng rule, mà hai bảng nạp cùng một lần Deploy. Khuyến nghị giữ "không kiểm". |
| **Q2** bộ vector | Dùng bộ vector mục 8 để test host ngay, không chờ App. App gửi bộ của họ (kèm chỉ số tag và giá trị khởi tạo) thì đối chiếu. Cần có các ca: số chia là tag bằng 0, `INT32_MIN % -1`, MUL kẹp, REAL tràn ra Inf, NaN đầu vào, MOD âm, Op2 là tag hệ thống. | Chỉ thêm hoặc sửa vài dòng trong bảng test. |
| **Q6** thứ tự rule | Không cần làm gì. `rule_scan` đã chạy tuần tự theo thứ tự bảng (mục 3.2). App xếp rule theo thứ tự phụ thuộc, và chuỗi N > 2 toán hạng dùng VREG thường, không dùng VREG_RETAIN làm biến trung gian. | Không ảnh hưởng code. |
| **Q7** layout tag board WIFI_IO (meshwf), 112 tag | Không cần làm gì. Layout nằm ở file board, MATH chỉ nhận chỉ số tag. Theo thứ tự board Zigbee-IO: 4 DI (0–3), 4 DO (4–7), 0 AI, 32 VFLAG (8–39), 32 VREG (40–71), 32 VREG_RETAIN (72–103), 8 COUNTER (104–111). Các vector cũ dùng tag `0x20..0x22` gọi là VREG0..2, nhưng trên board này đó là VFLAG24..26. | Không ảnh hưởng code. |
| **Q8** `x = x + 1` | Cho phép, không kiểm (mục 5). | Nếu App muốn cảnh báo thì làm ở Studio, firmware không đổi. |
| **Q9** upload rule (Ctrl+4) | Đã nằm trong bước 1 (mục 10): lưu và trả nguyên văn Reg 13 byte thấp và Reg 14..15 (mục 2). | Không có phương án nào khác đơn giản hơn. |
| **Q14** cờ và Reg 6..7 của MATH | Code đúng quyết định D1: rule MATH chỉ nhận `flags` `0x00` hoặc `0x03`, khác thì COMMIT mã 2. Reg 6..7 bằng 0 và firmware không kiểm. Cờ `0x07` chỉ gắn vào rule SCALE_TAG và chỉ phát khi thiết bị khai capability bit 16. | Nếu App cần cho thêm giá trị, sửa một dòng trong hàm kiểm COMMIT. |
| **Q5** ICD | Firmware phát hành `SPLC-SPEC-MATH-007` v2.1.0 sau khi code xong: bỏ MIN/MAX/CLAMP (mã 5..7), bỏ `0x0A24` và ngưỡng 1e-6, bỏ hành vi "ghi đè f1", thêm health bit 3, capability bit 16, Reg 6..7 = 0, mục lưu nguyên văn Reg 13 thấp + Reg 14..15, quy tắc COMMIT (mục 5), cờ `INT_TO_FLOAT` (mục 7). App ký sau khi firmware cập nhật. | Không ảnh hưởng code. |

## 10. Kế hoạch triển khai trong firmware

Tất cả đặt trong `#if SPLC_MATH_ENABLE` để option tắt cho binary giống hệt.

| Tệp (trong `simple_plc/`) | Thay đổi |
|---|---|
| `config/splc_opt.h` | Option `SPLC_MATH_ENABLE` (mặc định 1) và `SPLC_MATH_FAULT_HOLD_MS` (mặc định 3000); `#error` nếu thiếu `SPLC_REAL_ENABLE`. |
| `core/plc_rule/plc_rule.h`, `.c` | Hằng `SPLC_ACT_MATH_OP = 8`, cờ `SPLC_RULE_FLAG_INT_TO_FLOAT = 0x04`; nhánh MATH trong `rule_state_machine_step` đặt ngay sau kiểm `enabled`, trước trigger/compare/guard; `trigger_tag_is_used` và `action_tag_is_used` coi action 8 là "có dùng"; hàm kiểm COMMIT cho MATH (mục 5). |
| `core/plc_internal_rule/plc_rule_action.c` | Nhánh `INT_TO_FLOAT` trong `action_scale_tag` (mục 7). |
| `core/plc_math/plc_math.c`, `.h` (mới) | ALU theo mục 4; `splc_fmodf` (kèm header MIT của musl); lưu thời điểm lỗi cuối; hằng capability bit 16. |
| `core/plc_device/plc_device.h` | `SPLC_HEALTH_MATH_FAULT = 1 << 3`. |
| `services/plc_modbus_cfg/plc_modbus_cfg.c` | Lưu/đọc nguyên văn Reg 13 thấp và Reg 14..15 (ghi staging, đọc staging, đọc active, `rule_record_to_wire`); OR bit 3 khi đọc `0x0803`; thêm capability bit 16. |
| `services/plc_rule_flash/plc_rule_flash.c` | `rule_wire_to_record` giữ các byte đó thay vì xoá về 0. |
| meshwf `SynaptixSDK/ports/splc_port/splcopts.h` | Không cần đặt `SPLC_MATH_ENABLE` (mặc định 1); đồng bộ file thư viện do người dùng làm (D10). |
| `docs/` | ICD `docs/03-firmware/14-SPLC_SPEC_MATH_007_ICD.md` v2.1.0 (giao vào repo paa sau khi code xong). |

Các bước (mỗi bước giao nguyên file, nêu đường dẫn đích, nói rõ đã chạy gì):

1. Lưu nguyên văn Reg 13 thấp + Reg 14..15 qua wire/staging/CRC/Flash/đọc lại. Không phụ thuộc các câu hỏi mở.
2. `plc_math.c` (ALU, `splc_fmodf`) và nhánh trong `rule_scan`.
3. Kiểm tra COMMIT, health bit 3, capability bit 16.
4. Nhánh `INT_TO_FLOAT` trong SCALE_TAG.
5. Kiểm thử.

Kiểm thử:

- **Host (gcc x86, có stub; không phải ARM):** chạy toàn bộ bảng mục 8 với code thật của `plc_math.c`; parser wire/CRC cho Reg 13/14/15; validate COMMIT.
- **Build ARM trên Windows (người dùng chạy):** `SPLC_MATH_ENABLE 0` so md5 `.bin` với bản trước khi có MATH; `SPLC_MATH_ENABLE 1` xem `arm-none-eabi-size -A` và `arm-none-eabi-nm --size-sort -S build\<tên>.elf | findstr /i "fmod"` để đo chi phí `splc_fmodf`. Không có số đo thì không ghi con số nào vào tài liệu.
- **Board:** chưa chạy. Cần thử COMMIT, đọc lại rule, health bit 3, các vector của App.

## 11. Chưa kiểm chứng

- **Cập nhật 20/11/2026:** bước 1–4 của mục 10 đã code (xem handoff 101): đã build ARM (`MATH=0` md5 giống hệt trước MATH; `MATH=1` +864 B FLASH), vector mục 8 đã chạy bằng code thật trên host (PASS). CHƯA chạy board, CHƯA có `test_math.py`.
- Bảng vector ở mục 8 do chương trình tham chiếu trên host tính; App chưa đối chiếu.
- Kích thước phần MATH đã đo (+864 B FLASH, +8 B RAM so với `MATH=0`, gồm cả `splc_fmodf`). Thời gian chạy `splc_fmodf` trên H523: chưa đo.
- Q13(b): firmware không kiểm kiểu tag OUT (D11); việc đối chiếu bảng kiểu với bảng rule lúc COMMIT chưa được xét về khả thi.
- Thời gian giữ 3000 ms được chọn theo lập luận; chu kỳ hỏi của SCADA bên thứ ba chưa biết.
- Phía App: các điểm về cách App sinh rule và xếp thứ tự là thỏa thuận qua phản hồi (hoặc hướng mặc định ở mục 9.2), firmware chưa kiểm tra thực tế.