# SimplePLC — Thiết kế `plc_indicator` (đèn báo kết nối)

> **Trạng thái:** thiết kế ĐÃ DUYỆT 06/11/2026 (người dùng). **Phần thư viện đã code 06/11** (`services/plc_indicator/`, option, engine, CMake; ARM `INDICATOR=0` md5 giống hệt baseline; kiểm host giả 71 kiểm tra). Chưa có port sản phẩm, chưa chạy board. Chi tiết số đo ở `handoff.md` quyết định 99. Option `SPLC_INDICATOR_ENABLE` mặc định 0.

## 1. Mục tiêu và phạm vi

Board có 3 LED không dùng: **power**, **status**, **link**. Thư viện quyết định MỖI LED hiển thị gì; nó KHÔNG biết
chân GPIO, mức tích cực hay loại LED (như `plc_rtc` với RTC ngoài: thư viện chỉ có móc, sản phẩm tự nối).

- **POWER:** sáng ngay khi khởi tạo, không đổi.
- **STATUS:** tình trạng kết nối của giao thức đang được hiển thị (đứng im / 1 s / 2 s, mục 3).
- **LINK:** GIAO THỨC nào đang được báo, bằng mẫu nháy do sản phẩm tự định nghĩa (mục 4).

Chỉ báo **kết nối** (Zigbee, Ethernet, Wi-Fi, Cellular, LoRa, MQTT, ...). KHÔNG báo sức khoẻ hệ thống (Flash, RTC,
điện áp, scan quá chu kỳ): đã cân nhắc rồi bỏ (06/11).

## 2. Mô hình

Sản phẩm đăng ký một **bảng giao thức** (thứ tự = ưu tiên). Mỗi dòng có hàm đọc trạng thái không chặn, trả một trong
4 trạng thái chung:

| `SPLC_LINK_STATE` | Ý nghĩa | Ví dụ ánh xạ từ `plc_zigbee` |
|---|---|---|
| `DISABLED` | không bật / không cấu hình: bỏ qua | `PLC_ZIGBEE_LINK_OFF` |
| `CONNECTING` | đang vào mạng, kết hợp, DHCP, đăng ký | `PLC_ZIGBEE_LINK_JOINING` |
| `CONNECTED` | dùng được | `PLC_ZIGBEE_LINK_UP` |
| `FAILED` | đã bật nhưng không dùng được | `PLC_ZIGBEE_LINK_ERROR` |

Thư viện KÉO trạng thái mỗi vòng scan (pull, không push): port Ethernet/cellular/LoRa không phải gọi ngược vào thư viện
từ ISR. Mã lỗi chi tiết (`SPLC_LINK_ERR_*`) giữ làm thông tin chẩn đoán, KHÔNG ảnh hưởng LED ở bản đầu.

## 3. Quy tắc hiển thị

**Tập báo cáo** = các giao thức đã bật, chưa `CONNECTED`, và không bị ẩn do phụ thuộc (mục 5).

| Tập báo cáo | LINK | STATUS |
|---|---|---|
| Rỗng (mọi thứ OK) | sáng đứng im | sáng đứng im |
| 1 giao thức | nháy theo mẫu của nó, liên tục | `CONNECTING`: nháy 1 s một lần; `FAILED`: nháy 2 s một lần |
| Nhiều giao thức | xoay vòng (mục 3.1) | theo giao thức đang hiển thị |

Chưa đăng ký giao thức nào: chỉ POWER sáng, STATUS và LINK TẮT (không để sáng giả "mọi thứ OK").

"Nháy một lần" là xung ngắn (`SPLC_LED_FLASH_ON_MS`, mặc định 100 ms), không phải 50 % duty.

### 3.1 Xoay vòng khi có nhiều giao thức chưa OK

1. Hiển thị giao thức đầu tiên trong tập (theo thứ tự bảng) trong một **slot**: `SPLC_IND_SLOT_MS` (mặc định 8000 ms),
   LÀM TRÒN LÊN cho đủ số chu kỳ trọn của mẫu nháy (không cắt mẫu giữa chừng).
2. **Khoảng nghỉ** `SPLC_IND_GAP_MS` (mặc định 1000 ms): **CẢ LINK VÀ STATUS đều TẮT** để người xem biết đã chuyển sang
   giao thức khác.
3. Sang giao thức kế tiếp trong tập theo thứ tự bảng; hết vòng thì quay lại đầu.
4. Mỗi slot bắt đầu mẫu nháy từ đầu (đèn sáng trước). Chỉ còn MỘT giao thức trong tập ⇒ không xoay, không có khoảng nghỉ.
5. Giao thức đang hiển thị chuyển sang `CONNECTED` giữa slot ⇒ KẾT THÚC slot ngay (không chờ hết 8 s): qua khoảng nghỉ,
   sang giao thức kế; hết tập ⇒ sáng đứng im. Giao thức đang hiển thị đổi `CONNECTING` ↔ `FAILED` giữa slot ⇒ LED STATUS
   đổi nhịp ngay, LED LINK giữ mẫu.
6. Chưa có bộ lọc chống nhấp nháy (hysteresis): link chập chờn ⇒ LED đổi theo. Thêm sau nếu thực tế cần (mục 10).

Ví dụ (Zigbee + Ethernet đều lỗi, mẫu Zigbee = 2 nháy, Ethernet = 3 nháy):
`[8 s Zigbee: LINK 2 nháy/chu kỳ, STATUS 2 s] [1 s tắt cả hai] [8 s Ethernet: LINK 3 nháy, STATUS 2 s] [1 s tắt] ...`
Ethernet lên OK giữa chừng ⇒ slot Ethernet kết thúc, nghỉ 1 s, chỉ còn Zigbee ⇒ báo liên tục không xoay.

## 4. Mẫu nháy của LED LINK (sản phẩm tự định nghĩa)

```c
typedef struct {
    uint16_t on_ms, off_ms;   /* one flash                                */
    uint8_t  count;           /* flashes per group (1 = plain periodic)   */
    uint16_t pause_ms;        /* pause after the group                    */
} splc_led_pattern_t;         /* cycle = count*(on_ms+off_ms) + pause_ms  */
```

Một struct phủ cả hai kiểu: "mỗi giao thức một chu kỳ" (`count = 1`, `pause_ms = 0`) và "mỗi giao thức một số lần nháy"
(`count = N`, `pause_ms` dài). Thư viện không áp đặt. Mẫu của LED STATUS (1 s / 2 s) là hằng của thư viện, KHÔNG đổi theo
board, để user manual nhất quán.

## 5. Phụ thuộc giữa giao thức

Trường `depends_on` (chỉ số dòng + 1, 0 = không). Giao thức bị phụ thuộc bị ẨN khỏi tập báo cáo khi giao thức gốc chưa
`CONNECTED` (Wi-Fi mất ⇒ MQTT chắc chắn không lên, báo cả hai chỉ gây nhiễu). Giao thức gốc `DISABLED` ⇒ coi như không có
phụ thuộc. Các giao thức độc lập thì xoay vòng bình thường.

## 6. API (đúng với code 06/11; kiểm trùng symbol với host `wg_ue` CHƯA làm — quyết định 21)

```c
typedef struct {
    const char        *name;                        /* log only                  */
    splc_led_pattern_t pattern;                     /* LINK LED pattern          */
    SPLC_LINK_STATE  (*get_state)(void *ctx);       /* non-blocking, no I/O wait */
    void              *ctx;
    uint8_t            depends_on;                  /* row index + 1, 0 = none   */
} splc_ind_proto_t;

typedef struct {
    void (*set)(SPLC_LED_ID led, bool on, void *ctx);   /* GPIO write only */
    void  *ctx;
} splc_ind_ops_t;

bool splc_indicator_init(const splc_ind_ops_t *ops);       /* lights POWER; false if ops/set NULL        */
int  splc_indicator_register(const splc_ind_proto_t *p);   /* row index, or -1 when refused; order = priority */
void splc_indicator_scan(uint32_t now_ms);                 /* once per engine cycle                      */
int  splc_indicator_shown(void);                           /* row on LED LINK, -1 = none (tests/diag)    */
```

- Bảng tĩnh `SPLC_INDICATOR_MAX_PROTOCOLS` dòng (mặc định 4); không heap; một ngữ cảnh gọi.
- `register` quá số dòng (tối đa 8), sau vòng scan đầu, mẫu nháy sai (`on_ms`/`count` = 0, hoặc `count` > 1 mà `off_ms` = 0), `depends_on` không trỏ dòng trước, hoặc thiếu `get_state` ⇒ từ chối (trả -1 và log), không âm thầm bỏ.
- Mốc thời gian lấy từ `now_ms` của vòng scan (10 ms) ⇒ độ phân giải nháy 10 ms.
- `splc_indicator_scan()` chạy MỌI vòng, KỂ CẢ khi diag treo engine (LED vẫn phải báo kết nối). Vị trí chính xác trong
  `scan_cycle()` chốt khi code (không được nằm sau `plc_system_cmd_service`, vì hàm đó có thể reset).

## 7. Option (trong `splcopts.h`, mặc định ở `config/splc_opt.h`)

| Option | Mặc định | Ý nghĩa |
|---|---|---|
| `SPLC_INDICATOR_ENABLE` | 0 | 0 ⇒ binary GIỐNG HỆT bản trước (phải chứng minh bằng md5) |
| `SPLC_INDICATOR_MAX_PROTOCOLS` | 4 | số dòng bảng |
| `SPLC_IND_SLOT_MS` | 8000 | thời gian hiển thị mỗi giao thức khi xoay vòng |
| `SPLC_IND_GAP_MS` | 1000 | khoảng nghỉ cả hai LED tắt giữa hai giao thức |
| `SPLC_IND_STATUS_CONNECTING_MS` | 1000 | chu kỳ nháy STATUS khi đang kết nối |
| `SPLC_IND_STATUS_FAILED_MS` | 2000 | chu kỳ nháy STATUS khi lỗi |
| `SPLC_LED_FLASH_ON_MS` | 100 | độ dài một xung nháy |

Mỗi option mới phải có mặc định + `#error` khi sai trong `splc_opt.h` và dòng trong `splcopts_template.h`.

## 8. Hợp đồng port (phần sản phẩm tự viết: `splc_indicators_port.{h,c}`)

Nằm NGOÀI thư viện, cùng mẫu `Zigbee_IO/port/splc_zigbee_port.{h,c}`. Sản phẩm viết 4 việc:

1. **Hàm bật/tắt LED** (`splc_ind_ops_t.set`): nhận `led` (enum) + `on`; ghi GPIO, tự xử lý mức tích cực. Không chặn, không log.
2. **Hàm `get_state` cho từng giao thức:** chỉ đọc biến trạng thái có sẵn của stack, trả 4 trạng thái chung; không chờ, không gửi lệnh.
   Zigbee (và, nếu có sẵn, Wi-Fi/MQTT) thư viện cung cấp hàm chuyển đổi; Ethernet/cellular/LoRa sản phẩm tự viết.
3. **Bảng giao thức:** tên, mẫu nháy LED LINK, `get_state`, `ctx`, `depends_on`; thứ tự = ưu tiên.
4. **`splc_indicators_port_init()`:** cấu hình GPIO (hoặc CubeMX), gọi `splc_indicator_init()` rồi `splc_indicator_register()`
   từng dòng; chạy xong TRƯỚC vòng scan đầu.

Quyết định mặc định (người dùng duyệt chung 06/11): `set` nhận enum `SPLC_LED_ID` (một hàm cho cả ba LED); gọi
`splc_indicators_port_init()` từ file board (như `splc_zigbee_port_init()`, prototype lặp lại trong header board). CMake:
target port là **OBJECT library** (bài học 87; bản STATIC fail link vì `libsplc_board.a` bị kéo lần hai).

## 9. Header `plc_indicator_err_define.h` sau khi viết lại

Giữ: `SPLC_LED_ID`, `SPLC_LINK_STATE`, `SPLC_LINK_ERR` (chẩn đoán, chưa dùng cho LED), hằng thời gian. Thêm: `splc_led_pattern_t`.
BỎ khỏi bản nháp 06/11: `SPLC_FAULT_*`, `SPLC_IND_MODE`, `SPLC_LED_BLINK_FAST`, `SPLC_LINK_TYPE` (giao thức do sản phẩm tự
đặt tên, chỉ là chuỗi để log), alias `SPLC_STATUS_LINK` (không nơi nào dùng).

## 10. Còn mở

- Hysteresis cho link chập chờn (chưa làm; thêm thời gian giữ tối thiểu nếu thực tế cần).
- Adapter trạng thái có sẵn trong thư viện: Zigbee (`plc_zigbee`) rõ; Wi-Fi (`plc_wifi`) và MQTT (`plc_mqtt`) CHƯA kiểm xem
  đã lộ hàm trạng thái nào chưa — kiểm trước khi viết adapter.
- Lệnh "tìm thiết bị" từ App (nháy cả ba LED) cần thanh ghi mới: chưa làm, nằm ngoài phạm vi bản đầu.
- Chưa có gì được test; mỗi bước: code → test host giả (đồng hồ giả + LED giả, có đột biến) → người dùng build ARM và báo
  kết quả (Claude không build ARM, ghi chú 02/11) → so md5 `INDICATOR=0` với baseline → nạp board.

## 11. Mẫu bảng LED cho user manual

Mỗi sản phẩm chỉ liệt kê giao thức của nó. Mẫu nháy dưới đây là VÍ DỤ; giá trị thật do port của sản phẩm định.

**Chung cho mọi sản phẩm**

| LED | Trạng thái | Ý nghĩa |
|---|---|---|
| POWER | sáng | có nguồn |
| STATUS | sáng đứng im | mọi kết nối OK |
| STATUS | nháy 1 giây 1 lần | đang kết nối (giao thức đang hiển thị trên LED LINK) |
| STATUS | nháy 2 giây 1 lần | không kết nối được (giao thức đang hiển thị trên LED LINK) |
| LINK | sáng đứng im | mọi kết nối OK |
| LINK + STATUS | cùng tắt khoảng 1 giây | chuyển sang báo giao thức khác |

**Zigbee-IO** (một giao thức)

| LED LINK | Giao thức |
|---|---|
| nháy 1 lần mỗi 1,5 giây (ví dụ) | Zigbee |

**WiFi-IO** (Wi-Fi, rồi MQTT; MQTT chỉ hiện khi Wi-Fi đã lên)

| LED LINK | Giao thức |
|---|---|
| nháy 2 lần rồi nghỉ (ví dụ) | Wi-Fi |
| nháy 3 lần rồi nghỉ (ví dụ) | MQTT |

Lưu ý cho người dùng: khi có nhiều giao thức chưa OK, LED LINK báo lần lượt từng giao thức (khoảng 8 giây mỗi giao thức,
cách nhau một lần tắt cả hai đèn).