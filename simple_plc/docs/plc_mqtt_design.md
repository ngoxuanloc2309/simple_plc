# SimplePLC — Thiết kế chi tiết `plc_mqtt` (phần Publish)

> **Trạng thái:** thiết kế đã chốt các câu hỏi Q1–Q8 (xem §13). **Chưa có dòng code nào.** Quyết định tóm tắt ở
> `handoff.md` quyết định 31–35. Nhãn: **[ĐÃ KIỂM]** đọc code/spec thật · **[ĐỀ XUẤT]** chờ đối chiếu ·
> **[CHƯA KIỂM]** suy luận. Đọc kèm: `SimplePLC_App_MCU_Structs_MQTT_IIoT_Profile.md`, `firmware/splc_mqtt_wire.h`.
> Phạm vi: chỉ **Publish**; Subscribe chưa có wire (quyết định 34).

## 1. Điều đã xác minh ở host `wg_ue` (`main` = `83c2205`)

| # | Phát hiện | Hệ quả |
|---|---|---|
| H1 | `mqtt_client_publish(const char *topic, const char *payload)`: hai chuỗi NUL-terminated, trả `0` = đã xếp hàng, `-1` = lỗi (hết heap / hàng đợi tcpip đầy). | Ống publish của lib đưa payload dạng chuỗi NUL-terminated, không có NUL ở giữa. |
| H2 | **QoS và retain là CÀI ĐẶT TOÀN CỤC** (`g_sys_config.mqtt.qos`, `.publish_retain`); hàm publish không nhận tham số này. | QoS/retain từng khối cần host thêm hàm → **Q1**. |
| H3 | Host copy topic+payload vào heap trước khi trả về. | Lib dùng lại buffer payload ngay sau `publish`. |
| H4 | Khi chưa kết nối, host xếp hàng offline (8 × 128 B topic × 512 B payload) và chờ mutex tới 20 ms. | Lib chỉ gọi khi đang kết nối; không dùng hàng đợi offline của host. |
| H5 | `mqtt_client.c` không có TLS. | Cờ `USE_TLS` chưa thực hiện được → **Q2**. |
| H6 | `mqtt_config_t`: `broker_url[64]`, `port`, `username[32]`, `password[32]`, `client_id[48]`, `keepalive_s`, `clean_session`. Có `app_mqtt_request_reconnect()`, `app_mqtt_is_connected()`. | Struct broker của lib nhét vừa; cần cơ chế đưa cấu hình App vào host → **Q3**. |
| H7 | `data_broker_task` của host đã publish telemetry_store định kỳ cùng broker. | Topic của lib (App đặt) phải khác. |
| H8 | Gateway: `sx_rtc_*` là stub "chưa có giờ" (quyết định 23). | `{ts}` = 0 → **Q7**. |

Không có file/symbol `plc_mqtt*`/`splc_mqtt*` trong `wg_ue` [ĐÃ KIỂM] — tên mới an toàn (quyết định 21).

## 2. Quyết định áp dụng

- Lib chỉ làm **logic và giữ dữ liệu**, không có client mạng (31); host cắm đường truyền qua ống `publish`.
- Lib giữ cấu hình broker, `app_configured`, `generation`; chỉ publish khi `CONNECTED` **và**
  `status.generation == generation` (31).
- `{ts}` chưa có giờ → `0`; gateway bật `SPLC_MQTT_ENABLE=1`, Zigbee-IO mặc định `0`.
- `{val}` ra số thập phân bằng **số nguyên có hệ số**, không dùng float. Số chữ số lẻ khai **từng tag**, 2 bit mỗi tag
  trong `reserved_header` (+15; App hiện ghi 0 ⇒ số nguyên như cũ), `{valK:N}` trong template Custom làm ghi đè.
- Bản nháp + COMMIT, lưu Flash cùng Rule Table, một CRC (32).

## 3. Cấu trúc file

```
simple_plc/
├── services/plc_mqtt/        Layer 3
│   ├── plc_mqtt.h/.c         config running+draft, COMMIT, scan COV/chu kỳ, ống publish, status
│   └── splc_mqtt_wire.h      bản chép của docs/firmware/splc_mqtt_wire.h (struct 192 B, _Static_assert)
├── utils/mqtt_fmt/           Layer U — thuần, test được trên PC
│   └── splc_mqtt_fmt.h/.c    in số nguyên có dấu + n chữ số lẻ; render template/compact; kiểm template
├── services/plc_rtc/         + plc_rtc_get_epoch(uint32_t *epoch) (false nếu chưa có giờ)
├── services/plc_rule_flash/  + đoạn MQTT trong bản ghi (§6)
├── services/plc_modbus_cfg/  + định tuyến 0x0C00..0x0D7F, 0x0E00..0x0E5F vào plc_mqtt (§5)
├── config/splc_opt.h         + option mới (§9) và splcopts_template.h
└── app/plc_engine.c          + plc_mqtt_scan() (§7)
```

`plc_mqtt` (Layer 3) đọc tag qua `tag_read()`; dùng `plc_rtc_get_epoch()`; formatter ở Layer U. Không `#include`
nanomodbus; `plc_modbus_cfg.c` ánh xạ kết quả sang exception như `plc_fb`.

## 4. API công khai

```c
/* Ống publish — host đăng ký tường minh (kiểu netif_add, quyết định 16). Chạy ở ngữ cảnh ENGINE,
 * KHÔNG chặn. payload kết thúc NUL, len = strlen. Trả 0 = đã nhận, <0 = bận/lỗi (lib thử lại ở
 * scan sau, không mất dữ liệu). Host phải copy nếu cần giữ (lib dùng lại buffer ngay). */
typedef int (*splc_mqtt_publish_fn)(const char *topic, const char *payload, size_t len,
                                    uint8_t qos, bool retain, void *ctx);
bool splc_mqtt_register(splc_mqtt_publish_fn fn, void *ctx);   /* trước plc_engine_init() */

typedef struct {                 /* host điền; lib phản chiếu ra 0x0E54..0x0E5F */
    uint16_t flags;              /* bit0 CONNECTED, bit1 TLS_ACTIVE, bit2 AUTH_OK */
    uint16_t latency_ms;
    uint16_t last_error;         /* SPLC_MqttBrokerErrorCode_t */
    uint32_t generation;         /* generation của cấu hình mà kết nối được dựng theo */
} splc_mqtt_status_t;
void splc_mqtt_set_status(const splc_mqtt_status_t *st);

typedef struct { /* host/client_id/user/pass/port/keepalive/clean_session/version/use_tls */ } splc_mqtt_broker_t;
void splc_mqtt_get_broker(splc_mqtt_broker_t *out, bool *app_configured, uint32_t *generation);
void splc_mqtt_set_default_device_name(const char *name);      /* dự phòng cho {device}, §8 */
```

**MỌI hàm `splc_mqtt_*` chỉ gọi từ ngữ cảnh engine** (task `SPLC`). Port đọc trạng thái host
(`app_mqtt_is_connected()` là cờ `volatile bool`) và gọi `set_status`/`get_broker` trong hàm bơm của task engine ⇒ giữ
"thư viện đơn luồng, không khoá". `tx_message_count`/`tx_byte_count` (+86..89) do lib tự đếm = số lần `publish` trả `0`
(host **đã nhận**, chưa phải broker đã ack) — ghi rõ khi giao tài liệu.

## 5. Bản đồ thanh ghi, nháp và COMMIT

- `0x0C00 + i*96` (i = 0..3): khối Telemetry. `0x0E00..0x0E53`: cấu hình broker (84 reg). `0x0E54..0x0E5F`: trạng thái RO.
- **Ghi (FC16) vào nháp**, giống `plc_fb`: lần ghi đầu vào một khối thì nháp chép từ cấu hình đang chạy; mỗi khối có cờ
  "đã ghi". Địa chỉ ngoài bản đồ → `0x02`. Ghi trùm lên vùng RO (`0x0E54..`) → **chấp nhận và bỏ qua**.
- **Vùng `0x0E00..0x0E53` luôn ACK**, không trả exception; lỗi nội dung bắt ở COMMIT.
- **Đọc (FC03)** luôn trả cấu hình ĐANG CHẠY (+ trạng thái sống ở `0x0E54..`). Password đọc lại luôn `0`.
- **COMMIT** (`0xA5A5`): kiểm **cả bộ** rồi áp nguyên tử; sai chỗ nào → không áp gì, nháp bị xoá, mã lỗi
  `SPLC_ERROR_INVALID_PARAMETER` (2) ở `0x0A02`. Khối/broker KHÔNG được ghi lần này **giữ cấu hình đang chạy** (khác FB).
- Nháp bị xoá sau COMMIT (kể cả lỗi), khi boot, và khi `CLEAR_RULES`/`FACTORY_RESET`. `CLEAR_RULES`/`FACTORY_RESET` còn
  xoá cấu hình đang chạy (mọi khối `ENABLED=0`, broker về "chưa cấu hình"). `CLEAR_RETAIN` không đụng MQTT.
- `app_configured` = đã nhận broker từ App **và** `ENABLED=1` **và** host không rỗng. `generation` tăng khi
  `app_configured` đổi (hai chiều) hoặc nội dung broker THẬT SỰ đổi (so byte, loại trường status).

**Quy tắc kiểm lúc COMMIT:**

| Mục | Điều kiện hợp lệ |
|---|---|
| `control_flags` | `MODE` ≤ 2, `QOS` ≤ 2, `PAYLOAD` ≤ 2 |
| `tag_count` | 1..8 (khối `ENABLED=0` bỏ qua các kiểm tra dưới) |
| `bound_tags[0..tag_count-1]` | mỗi tag < `MAX_TAGS`; phần còn lại phải `0xFFFF` |
| `en_tag_index` | `0xFFFF` hoặc < `MAX_TAGS` |
| `interval_ms` | khi `PERIODIC`: ≥ `SPLC_MQTT_MIN_INTERVAL_MS` (mặc định 100) |
| `deadband` | không NaN/Inf, ≥ 0; hữu hạn sau khi đổi sang thang số nguyên (clamp `INT32_MAX`) |
| `topic` | khác rỗng khi `ENABLED=1`; ASCII in được; không có `+`, `#`; không bắt đầu bằng `/` |
| `payload_template` | khi `PAYLOAD=CUSTOM`: token hợp lệ (`{val}`, `{valK}`, `{valK:N}` N=0..3, `{tag}`, `{tagK}`, `{vals}`, `{ts}`, `{device}`); `K` ≤ `tag_count`; ngoặc cân; kích thước tệ nhất sau mở rộng ≤ `SPLC_MQTT_PAYLOAD_MAX` |
| `reserved_header` | mọi giá trị hợp lệ (xem Q8 §13; ô của tag ≥ `tag_count` bỏ qua) |
| Broker | khi `app_configured`: `port` ≠ 0; `use_tls` chỉ hợp lệ nếu `SPLC_MQTT_TLS_SUPPORTED=1`; version theo enum wire |

## 6. Lưu Flash

- Thêm **`SPLC_RULE_FLASH_MQTT_FLAG = 0x2000` (bit 13)** vào `rule_count`; thu `SPLC_RULE_FLASH_COUNT_MASK` từ `0x3FFF`
  về `0x1FFF` (MAX_RULES ≤ 100 nên dư) và sửa `_Static_assert` ở `plc_rule_flash.c` thành
  `FB_FLAG|FB_V2_FLAG|MQTT_FLAG|COUNT_MASK == 0xFFFF`. Bản ghi cũ bit 13 = 0 vẫn nạp được.
- Bố cục: `header 8 B | rule × 32 B | FB 128 B | MQTT 960 B` (4 × 192 + broker 192), **một CRC phủ tất cả**, cơ chế A/B +
  khôi phục B→A như cũ. Tối đa 3 336 + 960 = 4 296 B → làm tròn **4 304 B** (bội 16 B) < sector 8 KB.
- **Password nằm nguyên văn trong Flash** — ghi rõ khi giao tài liệu.
- Mọi thao tác Flash đi qua đường `plc_rule_flash` hiện có (đã bọc `retain_flash_op_begin/end()`).
- [ĐÃ KIỂM ở E.3] Độ dài bản ghi = `8 + rule_count×32 + FB (0/112/128) + MQTT (0/960)`; đoạn MQTT nằm SAU đoạn FB, nên
  offset = `8 + rule_count×32 + fb_size`. Bản `SPLC_MQTT_ENABLE=0` giữ mask 0x3FFF (bit 13 vẫn là số rule) nên coi bản ghi
  có MQTT là hỏng (hạ firmware cần Deploy lại) và không tốn thêm byte code/RAM. Đoạn MQTT = 4×192 B telemetry + 192 B
  broker (84 thanh ghi cấu hình + 12 thanh ghi dành riêng luôn 0); `plc_mqtt_import()` kiểm cả bộ như COMMIT, sai thì
  không áp gì.

## 7. Thuật toán mỗi scan

`plc_mqtt_scan(now_ms)` gọi trong `scan_cycle()` **ngay sau `output_scan()`**, trước `modbus_config_service()`.
**Khi `diag` đang treo engine → không publish (Q4)** — giá trị lúc đó do host ép, không phải dữ liệu thật.

1. **Sẵn sàng:** broker `CONNECTED` **và** `status.generation == generation`. Không sẵn sàng → không làm gì (lib KHÔNG tự
   xếp hàng; dữ liệu lúc mất kết nối bị bỏ, lần publish sau mang giá trị mới nhất).
2. Mỗi scan **tối đa 1 publish**, quay vòng 4 khối (≤ 100 bản tin/giây).
3. Khối bật khi: `ENABLED=1` và (`en_tag_index == 0xFFFF` hoặc `tag_read(en_tag_index) != 0`).
4. Điều kiện bắn theo `MODE`:
   - **ON_CHANGE:** có tag ≠ giá trị đã publish lần trước.
   - **COV:** có tag `|cur − last| ≥ ngưỡng_tag`, `ngưỡng_tag = ceil(deadband × 10^dec)` (≥ 1), **tính một lần lúc
     COMMIT**; scan chỉ so số nguyên. `deadband` theo **đơn vị kỹ thuật** (0.5 = 0.5 °C với tag `dec=1`).
   - **PERIODIC:** `now − last_pub_ms ≥ interval_ms` (so unsigned, an toàn khi tràn).
   - **Lần đầu sau khi "sẵn sàng"** (boot, đổi broker, COMMIT cấu hình khối): bắn **một lần** để cloud có giá trị nền (Q5).
5. Khi bắn: dựng payload (§8) → `publish(topic, payload, len, qos, retain, ctx)`.
   - Trả `0`: cập nhật `last_value` của **mọi** tag trong khối và `last_pub_ms`; đếm tx.
   - Trả `<0`: **không** cập nhật; thử lại sau `SPLC_MQTT_RETRY_MS` (mặc định 200 ms).
6. RAM runtime mỗi khối: `last_value[8]` + `last_pub_ms` + cờ + ngưỡng đã tính. Một buffer payload dùng chung.
7. [Bổ sung khi code E.4] "Sẵn sàng" chuyển false→true (nối lại broker, hoặc host vừa báo đúng `generation` mới) thì mọi
   khối bắn lại baseline một lần; enable tag chuyển 0→khác 0 cũng vậy. Khối refuse (`<0`) hoặc lỗi dựng payload chờ
   `SPLC_MQTT_RETRY_MS`. Ngưỡng COV tính theo từng tag (mỗi tag có `dec` riêng) tại COMMIT/nạp Flash. `plc_mqtt_scan()` được
   `scan_cycle()` gọi, KHÔNG tự kiểm diag (việc của engine, Q4).

## 8. Dựng payload (toàn số nguyên, không heap, không printf float)

In số: `int32` + `dec` (0..3) → dấu `-` riêng, `|v|` bằng `uint32` (đúng cả `INT32_MIN`), phần nguyên = `|v|/10^dec`,
phần lẻ = `|v|%10^dec` đệm `0` bên trái. `-5, dec=1` → `-0.5`; `2457, dec=2` → `24.57`; `812, dec=0` → `812`.

| `PAYLOAD` | Kết quả |
|---|---|
| 0 Compact | 1 tag: `{"tag":16,"val":24.5,"ts":1773228945}` · nhiều tag: `{"d":[24.5,101.3],"ts":1773228945}`; mỗi giá trị theo `dec` của tag đó |
| 1 Raw | chuỗi giá trị của tag đầu tiên (`24.5`, `1`) |
| 2 Custom | `{valK}` = giá trị tag thứ K (theo `dec` của tag đó); `{valK:N}` ghi đè `dec`; `{val}` ≡ `{val1}`; `{tagK}` = chỉ số tag thứ K, `{tag}` ≡ `{tag1}`; `{vals}` = mọi tag cách nhau dấu phẩy; `{ts}` = epoch UTC (`0` nếu chưa có giờ); `{device}` = tên thiết bị |

- Ví dụ Custom: `{"t":{val1},"h":{val2},"co2":{val3},"ts":{ts}}`. `K > tag_count` bị từ chối lúc COMMIT.
- **`{device}`:** spec nói = `client_id` (`0x0E24`). Khi App chưa cấu hình broker thì `client_id` của lib rỗng → dùng chuỗi
  do port đặt bằng `splc_mqtt_set_default_device_name()`. **Port lấy từ `g_sys_config.device.device_id`, KHÔNG phải
  `g_sys_config.mqtt.client_id`** (mặc định rỗng [ĐÃ KIỂM `config_manager.c:71`]). Dài hơn buffer thì cắt.
- Tràn buffer (không xảy ra nếu COMMIT đã kiểm tệ nhất, vẫn phải kiểm khi render): không publish, tăng `format_errors`,
  `log_warn` một lần.

## 9. Option mới (`config/splc_opt.h` + `splcopts_template.h`)

| Option | Mặc định | Ý nghĩa |
|---|---|---|
| `SPLC_MQTT_ENABLE` | 0 | 1 = build `plc_mqtt` + định tuyến thanh ghi; 0 = không đổi gì (Zigbee-IO phải ra ĐÚNG số đo chuẩn) |
| `SPLC_MQTT_PAYLOAD_MAX` | 256 | cỡ buffer payload, `#error` nếu < 128 hoặc > 512 |
| `SPLC_MQTT_RETRY_MS` | 200 | chờ trước khi thử lại sau khi `publish` trả `<0` |
| `SPLC_MQTT_MIN_INTERVAL_MS` | 100 | sàn của `interval_ms` khi `PERIODIC` |
| `SPLC_MQTT_TLS_SUPPORTED` | 0 | host có TLS thật hay không; 0 → COMMIT từ chối `use_tls=1` |

Khi `SPLC_MQTT_ENABLE=0`: `0x0C00..0x0E5F` giữ hành vi hiện tại (`0x02` địa chỉ sai).

## 10. Ước lượng RAM / Flash — **[ƯỚC LƯỢNG, chưa đo]**

| Thành phần | RAM tĩnh |
|---|---|
| Cấu hình đang chạy (4 × 192 + 192) | 960 B |
| Bản nháp (cùng cỡ) | 960 B |
| `s_rule_flash_buf` lớn thêm | 960 B |
| Runtime COV/chu kỳ (4 khối) | ~180 B |
| Buffer payload | 256 B |
| **Tổng** | **~3,3 KB** |

**[ĐÃ ĐO ở E.4, Zigbee-IO, GCC 13.2.1]** `SPLC_MQTT_ENABLE=1` so với baseline: **RAM +4 440 B, FLASH +5 712 B** (cao hơn ước
lượng vì còn backup 960 B cho `CLEAR_RULES` và runtime 4 khối ~300 B thay vì ~180 B). Với host `wg_ue` dư 2 992 B RAM
(quyết định 26) thì phải hạ `configTOTAL_HEAP_SIZE` nhiều hơn dự kiến — đo lại bằng map thật của gateway.

Cao hơn mức 2,3–2,5 KB ghi trước đây vì chưa tính buffer Flash lớn thêm. Host `wg_ue` chỉ dư **2 992 B** (quyết định 26)
⇒ gần như chắc chắn phải hạ thêm `configTOTAL_HEAP_SIZE` ~4 KB (296 → 288 KB; từng link được ở 288 KB ở sandbox). Số thật
chỉ biết sau khi build và đọc map; `lowest free` trên board vẫn chưa kiểm. Flash code ước ~4–6 KB. Cách giảm nếu cần
(không làm trước): hạ `SPLC_MQTT_PAYLOAD_MAX` về 192.

## 11. Việc ở phía host (port), ngoài lib

1. `splc_port.c`: `splc_mqtt_register(port_publish, NULL)` trong `splc_host_start()`; trong hàm bơm của task engine:
   `set_status()` từ `app_mqtt_is_connected()` + `generation` đã áp.
2. `port_publish()` gọi `mqtt_client_publish_ex(topic, payload, qos, retain)` (Q1).
3. **Áp cấu hình App vào client host (Q3):** host thêm hàm "cấu hình hiệu lực" cho `mqtt_client.c` (thay các chỗ đọc
   `g_sys_config.mqtt`); port đặt ghi đè RAM khi `app_configured`, xoá khi về mặc định, rồi
   `app_mqtt_request_reconnect()`. Không đụng `g_sys_config` (vì `config_save()` ghi cả struct xuống file [ĐÃ KIỂM]).
   [CHƯA KIỂM] chi tiết các chỗ đọc cấu hình trong `mqtt_client.c` (~660–676) và `config_apply_manager.c`.
4. Báo `generation` của cấu hình mà kết nối được dựng theo, để lib tự dừng publish tới broker cũ khi App đổi broker.
5. Sửa `wg_ue` chỉ làm trên bản clone ở sandbox; người dùng tự chuyển sang repo chính.

## 12. Kế hoạch code (mỗi bước dừng để người dùng xem)

1. Option mới + `splc_mqtt_fmt` (Layer U) + wire header.
2. `plc_mqtt` config running/draft/COMMIT + định tuyến Modbus + `plc_rtc_get_epoch`.
3. Đoạn MQTT trong `plc_rule_flash` (bit 13) + `CLEAR_*`.
4. `plc_mqtt_scan` (COV/chu kỳ), ống publish, status.
5. Port phía `wg_ue` (bản clone).
6. `test/test_mqtt.py` (nạp khối + broker qua Modbus, đọc lại, từ chối sai, persistence qua reboot, đọc `0x0E54..`) +
   kiểm ranh giới layer bằng `nm`.

Công việc chỉ kiểm được trên mô hình giả (formatter, scan với đồng hồ giả) phải nói rõ là chưa chạy board. Bước "build
baseline ARM + so số đo chuẩn khi `SPLC_MQTT_ENABLE=0`" tạm hoãn theo yêu cầu người dùng; làm khi được bảo.

## 13. Các quyết định đã chốt

| # | Nội dung | Kết quả |
|---|---|---|
| Q1 | QoS/retain từng khối | Host thêm `mqtt_client_publish_ex(topic, payload, qos, retain)`, giữ hàm cũ gọi hàm mới với giá trị toàn cục. |
| Q2 | TLS | Chưa dùng (host chưa hỗ trợ): `SPLC_MQTT_TLS_SUPPORTED=0`; COMMIT từ chối `use_tls=1` để không ai tưởng đã mã hoá. Host có TLS thật thì đổi option sang 1. |
| Q3 | Áp cấu hình App vào host | Lớp ghi đè chỉ trong RAM, không chép vào `g_sys_config`. |
| Q4 | Publish khi `diag` treo engine | **Không.** |
| Q5 | Bắn một lần khi sẵn sàng | **Có.** |
| Q6 | Token Custom cho mọi tag | `{valK}`, `{tagK}`, `{vals}` (§8). |
| Q7 | `{ts}` trên gateway | = 0 cho tới khi chốt chủ đồng hồ (quyết định 23; NTP host trả giờ địa phương, SimplePLC cần UTC). |
| Q8 | Phía firmware dùng `reserved_header` cho số chữ số lẻ; `deadband` theo đơn vị kỹ thuật; App thăm dò `0x0E54`; sửa ICD | **Firmware làm theo như vậy; đội App chưa được báo.** |

**Bố cục bit của `reserved_header` [ĐỀ XUẤT, đội App xác nhận]:** tag thứ K (K = 1..8) chiếm bit `2(K−1)` và `2(K−1)+1`
(LSB trước), giá trị 0..3 = số chữ số lẻ. Bit của tag ≥ `tag_count` bỏ qua.

**Đề nghị gửi đội App (người dùng sẽ báo sau):**
1. Ghi `reserved_header` (+15) theo bố cục trên (hiện App ghi 0 — `MqttTelemetryCodec.cs:94`); cho phép nhập số chữ số lẻ
   cho từng tag trong UI.
2. Cho phép token `{valK}`, `{valK:N}`, `{tagK}`, `{vals}`, `{ts}`, `{device}` trong `CustomTemplate` (App hiện chỉ
   chuyển tiếp chuỗi; không thấy validator placeholder).
3. Spec ví dụ `24.5` nhưng tag là `int32`: xác nhận quy đổi là hệ số 10^dec theo từng tag; `deadband` tính theo đơn vị kỹ thuật.
4. Thăm dò `0x0E54` để biết thiết bị hỗ trợ MQTT, chỉ gửi `0x0C00`/`0x0E00` khi hỗ trợ; đọc và hiển thị trạng thái thật
   `0x0E54..0x0E5F`; đổi nhãn nút Test Connection thành "PC tới broker".
5. Sửa ICD `SPLC_SPEC_MQTT_001` cho khớp Header/Structs; sửa Conformance 11.2 (FC16 = `0x10`) và thêm mục MQTT.
6. Chốt wire format Subscribe (thanh ghi nào nạp xuống MCU) — chặn việc làm Subscribe ở firmware.