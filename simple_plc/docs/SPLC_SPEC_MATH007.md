# SPLC-SPEC-MATH-007 – Thiết kế firmware khối MATH (action_type = 8)

Bản nháp v0.2 – 19/11/2026 – để đồng bộ với đội App trước khi code.
Đường dẫn trong repo: `simple_plc/docs/SPLC_SPEC_MATH007.md` (nhánh `use_mb_new`).

Trạng thái: **chưa viết code, chưa build, chưa chạy board.** Mọi hành vi dưới đây là thiết kế.
Mục có nhãn **[ĐỀ XUẤT]** là điều firmware muốn chốt cùng App. Mục có nhãn **[CÂU HỎI]** cần App trả lời.
Mỗi đề xuất chỉ là lựa chọn firmware đưa ra: App có thể chọn một phương án khác trong danh sách hoặc đề xuất cách khác. Hai bên thống nhất rồi mới code.

---

## 1. Phạm vi và nguyên tắc

- Khối MATH tính `OUT = Op1 <toán tử> Op2` mỗi scan, trên số nguyên 32-bit có dấu hoặc float32.
- Option biên dịch `SPLC_MATH_ENABLE`:
  - Thư viện mặc định **0**. Khi 0, binary phải giống hệt bản trước (so md5 file `.bin`).
  - Board meshwf (WIFI_IO) đặt 1 trong `splcopts.h`.
  - Yêu cầu `SPLC_REAL_ENABLE=1` (nếu không: `#error` lúc biên dịch), vì trường `flags` chỉ tồn tại khi REAL bật.
- Không cấp phát heap, không dùng bẫy chia 0 của CPU.
- Toán tử hỗ trợ: `ADD=0 SUB=1 MUL=2 DIV=3 MOD=4`. Không có MIN/MAX/CLAMP.

## 2. Hợp đồng wire (bản ghi rule 32 byte = 16 thanh ghi, Big-Endian)

| Thanh ghi | Trường | Ý nghĩa với MATH |
|---|---|---|
| Reg 0..5 | threshold_lo/hi, for_ms | Firmware bỏ qua. App nên ghi 0. Vẫn được lưu và trả nguyên văn như mọi rule. |
| Reg 6..7 | action_param | Firmware bỏ qua. **App ghi 0** (không trùng aux). |
| Reg 8 | trigger_tag | Chỉ số tag Op1. |
| Reg 9 | action_tag | Chỉ số tag OUT. |
| Reg 10 | guard_tag | Chỉ số tag Op2 (lấy `giá trị & 0x7FFF`, bit 15 bị bỏ qua). `0x7FFF` = Op2 là hằng, lấy từ aux. |
| Reg 11 | enabled / trigger_type | `enabled` (byte cao) được tôn trọng. `trigger_type` bị bỏ qua. |
| Reg 12 | compare_op / action_type | Byte cao = toán tử 0..4. Byte thấp = 8. |
| Reg 13 | flags / reserved | Byte cao: `0x00` = INT32, `0x03` = REAL. Byte thấp: **lưu nguyên văn**, App ghi 0. |
| Reg 14..15 | aux | Hằng Op2 khi `guard_tag == 0x7FFF`. int32 hoặc float32 IEEE-754; **Reg 14 = word cao, Reg 15 = word thấp**. Op2 là tag thì App ghi 0. |

Điểm quan trọng về CRC:

- Firmware tính CRC-16/MODBUS trên ảnh wire 32 byte của từng rule. **Hiện tại** firmware ghi Reg 13 byte thấp và Reg 14..15 là 0 trong ảnh này, và đọc lại cũng ra 0.
- Hệ quả (suy từ đọc code, chưa chạy): rule có Reg 14..15 khác 0 sẽ lệch CRC giữa hai bên và COMMIT trả lỗi CRC (mã 4).
- Thiết kế mới: khi `SPLC_MATH_ENABLE=1`, firmware **lưu và trả nguyên văn** Reg 13 byte thấp và Reg 14..15 của mọi rule, qua đủ các chặng: ghi staging, đọc staging, đọc active, tính CRC, lưu Flash, nạp lại Flash. Định dạng Flash không đổi (đang lưu đúng ảnh wire 32 byte/rule), bản ghi cũ có 0 ở các byte này vẫn nạp được.
- Khi option tắt, hoặc firmware cũ: các byte này bị bỏ và đọc ra 0. Rule MATH có hằng sẽ bị lỗi CRC. **App phải đọc capability bit 16 trước khi nạp rule MATH** (xem mục 6).

## 3. Ngữ nghĩa thực thi

1. Vị trí trong chu kỳ quét: `input_scan` → system tags → MQTT SUB → **`rule_scan`** → function block → `output_scan`. MATH nằm trong `rule_scan`, nên nhìn thấy tag MQTT SUB vừa ghi trong cùng scan.
2. Rule chạy **tuần tự theo thứ tự trong bảng**, ghi tag ngay. Rule đứng sau thấy kết quả rule đứng trước trong cùng scan.
   - **[ĐỀ XUẤT]** Thứ tự rule = thứ tự thực thi. App phải xếp rule tạo kết quả trung gian đứng trước rule dùng nó. Firmware không tự sắp xếp. Nếu đảo thứ tự, kết quả trễ 1 scan (10 ms).
3. MATH là tổ hợp, không có trạng thái. Mỗi scan, rule bật đều được tính. Không dùng `trigger_type`, `for_ms`, ngưỡng, `compare_op` kiểu so sánh, guard như điều kiện chặn.
4. `enabled = 0`: bỏ qua hoàn toàn (không ghi tag, không báo lỗi).
5. Khi một phiên chẩn đoán (DIAG_CONTROL) đang giữ engine, `rule_scan` không chạy: ngõ ra giữ nguyên. Khi engine chạy lại, trạng thái runtime của rule được reset, MATH không bị ảnh hưởng vì không có trạng thái.
6. Op1 và Op2 có thể là tag thường hoặc **tag hệ thống (1000..1007, chỉ đọc)**. OUT phải là tag trong bảng tag (xem mục 5).
7. Hai toán hạng luôn cùng kiểu theo `flags`. Firmware **không** đổi kiểu toán hạng (xem câu hỏi Q1).
8. Không ghi log mỗi scan (100 lần/giây).

## 4. Quy tắc số học

Kiểu chọn theo `flags & 0x03`: `0x00` = INT32, `0x03` = REAL.

| Toán tử | INT32 | REAL (float32) |
|---|---|---|
| ADD | Quấn mod 2³² (tính bằng `uint32_t` rồi ép về `int32_t`). | IEEE-754. |
| SUB | Quấn mod 2³². | IEEE-754. |
| MUL | **Kẹp** `[INT32_MIN, INT32_MAX]` (tính qua `int64_t`). | IEEE-754. |
| DIV | `Op2 == 0` → lỗi. `INT32_MIN / -1` → ghi `INT32_MAX` và báo lỗi. Còn lại chia cắt về 0 (C99). | `Op2 == 0.0f` (kể cả `-0.0f`) → lỗi. Còn lại `Op1 / Op2`. |
| MOD | `Op2 == 0` → lỗi. `INT32_MIN % -1` → 0 (không lỗi) **[ĐỀ XUẤT, Q11]**. Còn lại theo C99 (dấu theo số bị chia; `-7 % 3 = -1`). | `Op2 == 0.0f` → lỗi. Còn lại `fmodf` (dấu theo số bị chia). |

Quy tắc lỗi (chia 0, hoặc kết quả REAL là NaN/Inf):

- **Không ghi đè** `action_tag` (giữ giá trị cũ).
- Bật health bit 3 (mục 6).
- So sánh số chia với 0 là so sánh **đúng 0**, không dùng ngưỡng (bỏ ngưỡng 1e-6 của ICD v2.0.0).
- Kết quả REAL là NaN hoặc ±Inf (ví dụ tràn `1e30 * 1e30`, `Inf - Inf`, đầu vào đã là NaN/Inf): **[ĐỀ XUẤT, Q12]** coi là lỗi, giữ ngõ ra cũ, bật bit 3. (ICD v2.0.0 ghi ép về 0.0; `SCALE_TAG` float hiện ghi nguyên NaN/Inf. Các phương án khác xem Q12.)
- `fmodf` cho kết quả chính xác tuyệt đối (không làm tròn), nên dùng hàm thư viện hay tự viết đều cho cùng bit. Quyết định cách cài đặt không ảnh hưởng App.

ADD/SUB quấn, MUL kẹp: khớp vector 5 (ADD quấn) và ICD v2.0.0 mục 4 (MUL tính qua `int64_t` rồi kẹp). **[CÂU HỎI Q4]** App xác nhận hoặc chọn phương án khác.

## 5. Kiểm tra lúc COMMIT

Chỉ áp dụng khi `SPLC_MATH_ENABLE=1`, chỉ với rule MATH **đang bật**. Vi phạm → từ chối **toàn bộ COMMIT**, `CONFIG_ERROR_CODE = 2` (INVALID_PARAMETER), như kiểm tra tag hiện có:

1. `compare_op > 4`.
2. `flags & 0x03` khác 0x00 và 0x03 (tức 0x01 hoặc 0x02). Đây là ngoại lệ mới so với quy ước hiện tại "COMMIT không từ chối cờ lạ"; chỉ áp cho MATH. Các bit 2..7 của flags vẫn được lưu nguyên văn.
3. Op1 hoặc Op2 (khi `guard_tag & 0x7FFF != 0x7FFF`) có chỉ số ngoài bảng tag và không phải tag hệ thống (kiểm tra có sẵn; chỉ cần coi action 8 là "có dùng trigger_tag").
4. `action_tag` ngoài bảng tag, kể cả tag hệ thống (chỉ đọc).
5. DIV hoặc MOD với Op2 là **hằng bằng 0** (int 0, hoặc float +0.0/-0.0).

Không từ chối (ghi để hai bên biết; xem thêm Q13):

- `action_tag` trùng một toán hạng (ví dụ `x = x + 1`): hợp lệ, chạy mỗi scan (tăng 100 lần/giây). Nếu App muốn cảnh báo thì làm ở Studio.
- `action_tag` là VREG_RETAIN: hợp lệ. Snapshot retain chỉ ghi khi giá trị khác bản đã lưu, và theo chu kỳ mặc định 5 phút (`RETAIN_SNAPSHOT_PERIOD_MS`, đổi được theo sản phẩm).
- `action_tag` là DI/AI: không từ chối, nhưng `input_scan` ghi đè mỗi scan nên không có tác dụng. App nên chặn.

## 6. Chẩn đoán và khai báo khả năng

- **`DEVICE_HEALTH.health_flags` (0x0803) bit 3 = MATH_FAULT.**
  - Không dính. Bật khi có lỗi MATH trong 1000 ms gần nhất, tự hạ sau 1 s nếu hết lỗi.
  - Không có thanh ghi đếm riêng. Bản ICD cũ nhắc `0x0A24`; **firmware không dùng**, ICD mới sẽ bỏ.
  - Cách làm: engine lưu thời điểm lỗi cuối (lấy từ `now_ms` đã truyền vào `rule_scan`), tầng Modbus OR bit 3 vào giá trị đọc.
  - **[CÂU HỎI Q10]** Chu kỳ hỏi `0x0803` của SCADA/App có ngắn hơn 1 s không? Nếu dài hơn, lỗi thoáng qua có thể bị bỏ lỡ; khi đó cần kéo dài thời gian giữ hoặc bit dính có lệnh xoá (phải đổi ICD).
  - Hiện chưa có code nào ghi `health_flags`; đây là chỗ ghi đầu tiên.
- **`HARDWARE_CAPABILITIES` (0x0A08) bit 16 (`0x00010000`) = MATH**, chỉ khai khi `SPLC_MATH_ENABLE=1`. Bit này chưa dùng trong firmware.
- Khi option tắt: rule action 8 vẫn được nhận và lưu (nhánh `default` của engine không làm gì), nhưng không chạy; riêng rule có hằng sẽ lỗi CRC như mục 2. Mã lỗi 5 (UNSUPPORTED) **không** được firmware dùng ở đâu hiện nay và không thể thêm khi option tắt mà không đổi binary. **App phải dựa vào capability bit 16, không dựa vào lỗi COMMIT.**

## 7. Câu hỏi cho đội App

**Q1. Đổi kiểu INT32 → REAL (nguồn của SCALE và toán hạng của MATH).**

Hiện trạng trong firmware:
- MATH không đổi kiểu toán hạng: hai toán hạng luôn cùng kiểu theo `flags`.
- `SCALE_TAG` chạy float (flags `0x03`) coi giá trị tag nguồn là **bit float32**, không đổi từ số nguyên. Một tag INT32 (ví dụ mã ADC thô `100`) đưa vào sẽ bị hiểu thành khoảng `1.4e-43`.
- `SCALE_TAG` số nguyên (flags `0x00`) cho ra số nguyên (gain tính theo phần nghìn), không ra REAL.
- Chưa có chỗ nào trong firmware đổi số nguyên sang float.

Phía App đã nêu hướng dùng khối SCALE để lấy giá trị REAL từ tag số nguyên, nên hai bên cần thống nhất cách đổi kiểu.

**[ĐỀ XUẤT] Phương án C:** thêm bit 2 (`0x04`) vào `flags` của rule `SCALE_TAG`. `0x07` = như `0x03` nhưng nguồn là INT32: firmware đổi sang float trước khi nhân gain và cộng offset. Nghĩa của `0x00` và `0x03` không đổi. Bit 2..7 hiện được lưu nguyên văn và không diễn giải nên bit 2 còn trống. Chỉ có hiệu lực khi `SPLC_MATH_ENABLE=1`. Với MATH: người dùng đặt một rule SCALE (gain 1, offset 0, flags `0x07`) để đổi tag INT32 thành REAL vào một VREG, rồi dùng VREG đó làm toán hạng. Mỗi toán hạng cần đổi tốn thêm 1 rule và 1 VREG.

Phương án khác để App chọn:
- **A.** App không dùng `SCALE_TAG` để đổi kiểu và đề xuất cơ chế khác.
- **B.** Đổi nghĩa `0x03` của SCALE thành "nguồn INT32". Firmware không đề xuất vì làm hỏng SCALE có nguồn REAL.
- **D.** MATH tự đổi toán hạng INT32 sang REAL khi `flags = 0x03` (không cần rule SCALE phụ). Firmware sẽ phải tra bảng kiểu tag (0x0080) lúc chạy, trong khi Rule Engine hiện không tra bảng này: kiểu tính do `flags` của rule quyết định.
- **E.** App đề xuất cách khác.

**Cần App trả lời:** chọn một phương án, và nói rõ SCALE đổi kiểu của App sẽ phát công thức/flags nào.

**Q2. Vector kiểm thử.** Cần bộ vector mới:
   - Vector 3 và 4 dùng số chia hằng 0 nhưng kỳ vọng chạy lúc runtime; theo mục 5 chúng bị từ chối lúc COMMIT. Cần vector số chia là **tag bằng 0** (kỳ vọng: ngõ ra giữ nguyên, bit 3 bật).
   - Vector 4 kỳ vọng `+Infinity`, mâu thuẫn với "không ghi đè khi chia 0".
   - Thêm vector cho: `INT32_MIN % -1`, MUL kẹp, REAL tràn ra Inf, NaN đầu vào, MOD âm (C99), Op2 là tag hệ thống.
   - Vector hiện dùng tag `0x20..0x22` gọi là VREG0..2. Trên board WIFI_IO (xem Q7) đó là VFLAG24..26. Cần chỉ số tag đúng, kèm **giá trị khởi tạo tag** (hiện không ghi).

**Q3. Reg 6..7.** Trong vector 1, 5, 7 giá trị Reg 6..7 trùng Reg 14..15. Firmware bỏ qua Reg 6..7; đề nghị App ghi 0.

**Q4. Tràn số nguyên (ADD/SUB/MUL, kiểu INT32).**

Số int32 chỉ chứa từ −2.147.483.648 đến 2.147.483.647. Ví dụ ADD `2.000.000.000 + 2.000.000.000`:
- **Quấn (wrap):** kết quả `−294.967.296` (trừ 2³² = 4.294.967.296).
- **Kẹp (clamp):** kết quả `2.147.483.647`.

**[ĐỀ XUẤT]** ADD/SUB quấn, MUL kẹp (tính qua `int64_t`). Lý do: khớp vector 5 (ADD quấn) và ICD v2.0.0 mục 4 (MUL kẹp). Với chuỗi N > 2 toán hạng qua VREG trung gian, quy tắc áp dụng ở từng bước.

Phương án khác để App chọn:
- Kẹp cả ADD/SUB: kết quả vẫn sai nhưng vẫn là số lớn, thường dễ chấp nhận hơn về an toàn; cần đổi vector 5.
- Quấn cả MUL.
- App đề xuất cách khác.

**Cần App trả lời:** chọn phương án và cập nhật vector tương ứng.

**Q5. ICD.** Firmware sẽ phát hành `SPLC-SPEC-MATH-007` v2.1.0 sau khi code xong: bỏ MIN/MAX/CLAMP (mã 5..7), bỏ `0x0A24` và ngưỡng 1e-6, bỏ hành vi "ghi đè f1", thêm health bit 3, capability bit 16, Reg 6..7 = 0, mục lưu nguyên văn Reg 13 thấp + Reg 14..15, quy tắc COMMIT (mục 5). App ký sau khi firmware cập nhật.

**Q6. Thứ tự rule.** Xác nhận App xếp rule theo thứ tự phụ thuộc (mục 3.2), và kiểu chuỗi N > 2 toán hạng dùng VREG thường, không dùng VREG_RETAIN làm biến trung gian.

**Q7. Layout tag board WIFI_IO (meshwf), tổng 112 tag:** 4 DI, 4 DO, **0 AI**, 32 VFLAG, 32 VREG, 32 VREG_RETAIN, 8 COUNTER. Chỉ số suy từ số lượng theo cùng thứ tự với board Zigbee-IO: DI 0–3, DO 4–7, VFLAG 8–39, VREG 40–71, VREG_R 72–103, COUNTER 104–111. App kiểm tra lại với Tag Catalog; hết VREG thì khối MATH nhiều chân (cần N−2 VREG trung gian) không biên dịch được.

**Q8. Tự tăng `x = x + 1`.** Firmware cho phép (mục 5). App có cảnh báo trong Studio không?

**Q9. Đọc ngược.** Chức năng upload rule (Ctrl+4) dựa vào việc firmware trả đúng Reg 13 thấp và Reg 14..15. Thiết kế mục 2 bảo đảm điều này khi option bật.

**Q10. Thời gian giữ health bit 3 (0x0803).**

MATH chạy 100 lần/giây. Một lỗi (ví dụ số chia là tag bằng 0) có thể chỉ kéo dài một scan (10 ms), nên bit 3 phải giữ đủ lâu để SCADA/App đọc được.

**[ĐỀ XUẤT]** Không dính: giữ 1000 ms sau lỗi cuối rồi tự hạ nếu hết lỗi. Thời gian giữ là hằng số biên dịch (mặc định 1000 ms).

Phương án khác để App chọn:
- Giữ lâu hơn N giây (App cho biết chu kỳ hỏi).
- Bit dính có lệnh xoá (phải đổi ICD: thêm thanh ghi/lệnh).
- Thanh ghi đếm lỗi (ICD v2.0.0 nhắc `0x0A24`; bản trả lời trước của App cho là không cần).
- App đề xuất cách khác.

**Cần App trả lời:** chu kỳ hỏi `0x0803` của SCADA/App là bao lâu? Nếu dài hơn 1 s, chọn phương án giữ lâu hơn hoặc bit dính.

**Q11. `INT32_MIN % -1`.**

`INT32_MIN / -1` đã thống nhất (vector 6): ghi `INT32_MAX` và bật bit 3, vì 2³¹ không vừa int32. Với MOD, kết quả đúng toán học là 0 và biểu diễn được.

**[ĐỀ XUẤT]** Trả 0, không lỗi. (Firmware phải bắt riêng trường hợp này vì C coi là hành vi không xác định.)

Phương án khác để App chọn:
- Giữ ngõ ra cũ và bật bit 3 (giống chia 0).
- App đề xuất cách khác.

**Cần App trả lời:** xác nhận hoặc chọn phương án khác, và thêm vector cho trường hợp này.

**Q12. Kết quả REAL là NaN hoặc ±Inf.**

Ví dụ `1e30 * 1e30` vượt giá trị lớn nhất của float (khoảng 3,4e38) ra Inf; `Inf - Inf` ra NaN; đầu vào đã là NaN/Inf.

**[ĐỀ XUẤT]** Không ghi đè: giữ ngõ ra cũ và bật bit 3 (cùng quy tắc với chia 0). Ngõ ra luôn là giá trị hợp lệ gần nhất; nhược điểm là đứng yên ở giá trị cũ cho đến khi có ai đọc bit 3.

Phương án khác để App chọn:
- Ép về `0.0` (ICD v2.0.0 hiện ghi): `0.0` trông như số bình thường nên lỗi dễ bị che.
- Ghi nguyên NaN/Inf như `SCALE_TAG` float hiện nay: đồng nhất với SCALE, nhưng giá trị lan sang các khối sau.
- Inf kẹp về ±`FLT_MAX`; NaN giữ ngõ ra cũ và bật bit 3.
- App đề xuất cách khác.

Với mỗi phương án cần nói rõ có bật bit 3 hay không.

**Cần App trả lời:** chọn phương án; mô phỏng trong Studio (nếu có) và vector kiểm thử phải cho cùng kết quả. Cần vector cho Inf, −Inf, NaN và đầu vào NaN.

**Q13. Tag OUT: loại tag và kiểu tag.**

Hiện trạng: firmware kiểm chỉ số tag OUT trong dải và không phải tag hệ thống (mục 5). Ngoài ra không có hạn chế theo loại tag. Rule Engine hiện không tra bảng kiểu tag (0x0080): kiểu tính do `flags` của rule quyết định. Bảng kiểu chỉ cho khai REAL trên tag loại AI, VREG, VREG_RETAIN và MB_HOLDING.

**(a) Loại tag OUT.**

**[ĐỀ XUẤT]** Không thêm hạn chế riêng cho MATH.
- VREG_RETAIN: cho phép. Snapshot retain chỉ ghi khi giá trị khác bản đã lưu, chu kỳ mặc định 5 phút (tối đa 12 lần/giờ), không phụ thuộc rule nào ghi tag.
- DI/AI: firmware không từ chối, nhưng `input_scan` ghi đè mỗi scan nên rule vô tác dụng. App chặn trong Studio.

Phương án khác: firmware từ chối OUT là DI/AI (mã 2); từ chối cả VREG_RETAIN; App đề xuất cách khác.

**(b) Kiểu tag OUT so với `flags`.** Khi `flags = 0x03`, kết quả là bit float32 ghi vào tag. Nếu tag OUT khai INT32/BOOL (hoặc không khai) trong bảng kiểu thì nơi đọc tag sẽ hiểu bit float thành số nguyên; chiều ngược lại cũng vậy.

**[ĐỀ XUẤT]** Giống các rule hiện có, firmware không kiểm. App bảo đảm tag OUT được khai đúng kiểu (REAL với `flags = 0x03`, INT32 với `flags = 0x00`) trong cùng lần Deploy.

Phương án khác: COMMIT từ chối (mã 2) khi `flags = 0x03` mà OUT không phải REAL trong bảng kiểu, hoặc `flags = 0x00` mà OUT là REAL. Firmware chưa kiểm khả thi, vì bảng kiểu và bảng rule được nạp cùng một lần Deploy và cần xác định thứ tự kiểm tra.

**Cần App trả lời:** chọn phương án cho (a) và (b).

## 8. Thay đổi dự kiến trong firmware (nội bộ, chưa làm)

Tất cả đặt trong `#if SPLC_MATH_ENABLE` để option tắt cho binary giống hệt.

| Tệp (trong `simple_plc/`) | Thay đổi |
|---|---|
| `config/splc_opt.h` | Option `SPLC_MATH_ENABLE` (mặc định 0), `#error` nếu thiếu `SPLC_REAL_ENABLE`. |
| `core/plc_rule/plc_rule.h`, `.c` | Hằng `SPLC_ACT_MATH_OP = 8`; nhánh MATH trong `rule_state_machine_step` đặt ngay sau kiểm `enabled`, trước trigger/compare/guard; `trigger_tag_is_used` và `action_tag_is_used` coi action 8 là "có dùng"; hàm kiểm COMMIT cho MATH (mục 5). |
| `core/plc_math/plc_math.c`, `.h` (mới) | ALU theo mục 4; lưu thời điểm lỗi cuối; hằng capability bit 16. |
| `core/plc_device/plc_device.h` | `SPLC_HEALTH_MATH_FAULT = 1 << 3`. |
| `services/plc_modbus_cfg/plc_modbus_cfg.c` | Lưu/đọc nguyên văn Reg 13 thấp và Reg 14..15 (ghi staging, đọc staging, đọc active, `rule_record_to_wire`); OR bit 3 khi đọc `0x0803`; thêm capability bit 16. |
| `services/plc_rule_flash/plc_rule_flash.c` | `rule_wire_to_record` giữ các byte đó thay vì xoá về 0. |
| `config/` | Giá trị mặc định của option; meshwf `SynaptixSDK/ports/splc_port/splcopts.h` đặt `SPLC_MATH_ENABLE 1` (giao riêng cho bên meshwf). |
| `docs/` | ICD `docs/03-firmware/14-SPLC_SPEC_MATH_007_ICD.md` v2.1.0 (giao vào repo paa sau khi code xong). |

Kế hoạch triển khai (mỗi bước giao nguyên file, nêu đường dẫn đích, nói rõ đã chạy gì):

1. Lưu nguyên văn Reg 13 thấp + Reg 14..15 qua wire/staging/CRC/Flash/đọc lại. Không phụ thuộc các câu hỏi mở.
2. `plc_math.c` và nhánh trong `rule_scan`.
3. Kiểm tra COMMIT, health bit 3, capability bit 16.
4. Kiểm thử.

Kiểm thử:

- **Host (gcc x86, có stub; không phải ARM):** ALU và các biên (tràn, chia 0, NaN/Inf, `fmodf`), parser wire/CRC cho Reg 13/14/15, validate COMMIT.
- **Build ARM trên Windows (người dùng chạy):** option = 0 so md5 `.bin` với bản trước; option = 1 xem `arm-none-eabi-size -A` và `nm --size-sort`. Không có số đo thì không ghi con số nào vào tài liệu.
- **Board:** chưa chạy. Cần thử COMMIT, đọc lại rule, health bit 3, các vector mới của App.

## 9. Chưa kiểm chứng (trung thực về hiện trạng)

- Chưa build ARM, chưa chạy board, chưa chạy host test cho MATH.
- Kích thước phần MATH và chi phí `fmodf`: chưa đo.
- Q13(b): chưa kiểm khả thi việc kiểm kiểu tag OUT lúc COMMIT (thứ tự kiểm bảng kiểu tag và bảng rule).
- Phần App: tôi chưa kiểm cách App xếp thứ tự rule và sinh dữ liệu thực tế; mọi nội dung về phía App ở trên là câu hỏi, không phải khẳng định.

---

## Phụ lục nội bộ (XOÁ MỤC NÀY TRƯỚC KHI GỬI APP)

### A. Trạng thái các quyết định (cập nhật 19/11/2026)

Bạn đã chốt D1. D2, D3, D4, D7, D8, D9 là quyết định hai phía: đã đưa vào mục 7 dưới dạng đề xuất của firmware để App chọn hoặc đề xuất cách khác. D5 và D6 đang chờ bạn.

| # | Quyết định | Trạng thái | Nằm ở |
|---|---|---|---|
| D1 | Từ chối flags `0x01/0x02` ở COMMIT (chỉ MATH) | **ĐÃ CHỐT:** từ chối, mã 2. Ngoại lệ mới so với handoff mục 45. | Mục 5 ý 2 |
| D2 | Tràn int32 | Hai phía. Đề xuất: ADD/SUB quấn, MUL kẹp. | Q4 |
| D3 | `INT32_MIN % -1` | Hai phía. Đề xuất: trả 0, không lỗi. | Q11 |
| D4 | REAL NaN/Inf | Hai phía. Đề xuất: giữ ngõ ra cũ + bit 3. Lưu ý `SCALE_TAG` float hiện ghi nguyên NaN/Inf. | Q12 |
| D5 | `SPLC_MATH_ENABLE` mặc định | Bạn nói "mặc định bật", phạm vi chưa rõ (cả thư viện hay chỉ meshwf). `SPLC_REAL_ENABLE` mặc định thư viện đã là 1 (`config/splc_opt.h`), nên mặc định bật không gây `#error` trừ khi sản phẩm đặt REAL=0. Gợi ý: mặc định `SPLC_MATH_ENABLE = SPLC_REAL_ENABLE`. Mục 1 vẫn ghi "thư viện 0, meshwf 1" cho tới khi bạn trả lời. Sản phẩm không tự đặt option sẽ đổi binary; so md5 phải đặt `SPLC_MATH_ENABLE 0` tường minh. | Mục 1 |
| D6 | Cài đặt MOD float | Đã thử host (x86, không phải ARM) đoạn musl `fmodf`: 9.000.289 ca, 0 sai lệch bit so với `fmodf` của glibc, UBSan sạch. Chờ bạn chốt. Cần: header MIT của musl, bỏ `#include "libm.h"`, bỏ `;` thừa sau `}`, đặt `static splc_fmodf` (không cần `-lm`). Kích thước và tốc độ chưa đo (xem D). | Không ảnh hưởng App |
| D7 | OUT là loại tag nào, kiểu tag | Hai phía. Đề xuất: không thêm hạn chế riêng cho MATH. | Q13 |
| D8 | Bit 3 không dính, giữ 1 s | Hai phía, phụ thuộc chu kỳ hỏi `0x0803`. | Q10 |
| D9 | Đổi INT32→REAL cho SCALE/MATH | Hai phía. Đề xuất: bit 2 của flags (`0x07`). | Q1 |
| D10 | Quy trình giao | Sửa ở simple_plc, bạn đồng bộ sang SynaptixSDK. `splc_log.h` đã lệch; đồng bộ theo từng file. | |

### B. Những gì đã đọc trong code (làm căn cứ)

- `core/plc_rule/plc_rule.h`: `SPLC_RuleRecord` 32 byte (`_Static_assert`), `flags` + `reserved[5]` chỉ khi REAL; bit 2..7 của flags "lưu và trả nguyên văn, không diễn giải".
- `core/plc_internal_rule/plc_rule_action.c`: `execute_action` có `default: break`; `action_scale_tag_f` bit-cast khi `(flags & 3) == 3`.
- `core/plc_rule/plc_rule.c`: `rule_scan` duyệt tuần tự; guard 0 chặn rule; `rule_table_find_bad_tag` (action_tag ≥ MAX_TAGS bị từ chối, kể cả tag hệ thống; guard/trigger cho phép tag hệ thống).
- `services/plc_modbus_cfg/plc_modbus_cfg.c`: `write_staging_rule_table`, `read_*_rule_table`, `rule_record_to_wire` đều bỏ Reg 13 thấp và Reg 14..15; CRC trên ảnh wire; COMMIT từ chối tag sai bằng mã 2; `read_hw_capabilities`; `read_device_health`.
- `services/plc_rule_flash/plc_rule_flash.c`: lưu ảnh wire; `rule_wire_to_record` `memset` reserved về 0; `SPLC_RULE_FLASH_RECORD_WIRE_SIZE = 32` (`platforms/stm32/stm32h5/flash_define/splc_flash_define.h`).
- `app/plc_app/plc_engine.c`: thứ tự scan; rule engine tạm dừng khi DIAG giữ.
- `core/plc_error/plc_error.h`: mã 5 có trong enum nhưng không nơi nào dùng.
- `core/plc_device/plc_device.h`: `health_flags` là word thứ 3 của struct 20 byte (0x0803); chỉ khai báo bit 0..2, không code nào ghi.
- `services/plc_retain`: snapshot chỉ ghi khi khác, chu kỳ mặc định 5 phút (`config/splc_opt.h`).
- meshwf: `splcopts.h` có `SPLC_REAL_ENABLE 1`; `board_tag_define.h`/`board_wifi_io.h`: layout 112 tag; flags `-Os -g0`, hard-float `fpv5-sp-d16`.
- `docs/handoff.md` mục 45 (COMMIT không từ chối cờ lạ; flags chỉ tồn tại khi REAL=1).

### C. Chưa rõ / chưa đọc

- `board_wifi_io.c` (`s_tag_layout` thật) và chỉ số COUNTER.
- Vị trí chính xác `health_flags` trong lệnh đọc (suy từ struct, chưa chạy).
- `-ffast-math` / `-lm`: không thấy trong các file CMake đã tìm (`CMakeLists.txt` gốc, `cmake/`, `CMakePresets.json`, `SynaptixSDK/CMakeLists.txt`, `libs/`); chưa quét hết. Trong `core/services/app` của simple_plc không có chỗ nào gọi `fmodf/fabsf/sqrtf`, nên `fmodf` sẽ là phụ thuộc libm mới; thiếu `-lm` thì link báo ngay.
- Chu kỳ SCADA hỏi health; thời gian chạy `fmodf` trên H523.
- Phía App: bản trả lời A/B gốc, 8 vector gốc, lowering khối SCALE thật, validator, cách sắp xếp rule.

### D. Việc bạn chạy trên Windows (không đoán số)

1. `arm-none-eabi-nm --size-sort -S build\<tên>.elf | findstr /i "fmod fabs"` (bản hiện tại đã có chưa).
2. Sau khi code: `arm-none-eabi-size -A` trước/sau; md5 `.bin` khi `SPLC_MATH_ENABLE=0`.