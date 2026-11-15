# PLC Watchdog — tài liệu thiết kế (SPLC)

| Mục | Giá trị |
|---|---|
| Trạng thái | **DRAFT — chưa duyệt, chưa code, chưa build, chưa thử trên board** |
| Ngày soạn | 2026-11-15 |
| Phạm vi | Thư viện `simple_plc` (service `plc_wdt`) + host `meshwf` (port `splc_wdt_port`) |
| Board mục tiêu | STM32H523, wifi_io (ST67W611, FreeRTOS) |
| Phần cứng watchdog | IWDG (đã thống nhất) |

Quy ước nhãn trong tài liệu:

- **[Đã xác minh]**: đọc trực tiếp từ code/log của dự án.
- **[Suy luận]**: suy ra từ thiết kế hoặc từ code, chưa chạy thật.
- **[Chưa kiểm chứng]**: kiến thức chung (datasheet/RM) hoặc giá trị tạm, cần kiểm trên board hoặc trong tài liệu ST.

---

## 1. Mục tiêu và phạm vi

### 1.1 Mục tiêu

1. Tự reset board khi nó **treo hẳn** (ngắt bị tắt, vòng lặp vô hạn, assert).
2. Tự reset board khi **task PLC** hoặc **task ST67** bị kẹt dù hệ thống vẫn chạy.
3. Ghi lại **lý do** reset để chẩn đoán sau khi board lên lại.
4. Giữ thư viện dùng lại được: lõi giám sát không phụ thuộc HAL/ST67, host gán hàm phần cứng qua port.

### 1.2 Ngoài phạm vi (bản đầu)

- Giám sát task vendor `spi_xfer_engine`, `Modem_Process` (vendor `x-cube-st67w61`, không sửa).
- Giám sát USB (không có task riêng, đã nằm trong task SPLC) và MBTCP (hai task `io_task`, `accept_task`).
- Xử lý nút giữ 7 s vào ROM bootloader (xem mục 10, rủi ro R1) — có thể làm cùng đợt hoặc sau.
- Chống reset lặp vô hạn (bộ đếm reset liên tiếp) — mục 12, quyết định mở.

---

## 2. Bối cảnh và bằng chứng

- **[Đã xác minh]** Dự án chưa có watchdog.
- **[Đã xác minh]** Task của dự án: `SPLC` (`splc_task`), `ST67` (`wifi_task`), `ST67dbg` (tạo ở `ports/splc_port/splc_port.c`). `defaultTask` của CubeMX chỉ chạy `osDelay(1)`, priority `osPriorityNormal`.
- **[Đã xác minh]** Priority không được đổi trong `splc_port.h`: SPLC=5, ST67 wifi=6, `spi_xfer_engine`=7, `Modem_Process`=8.
- **[Đã xác minh]** Thư viện chạy đơn luồng: một task gọi `plc_engine_init()` rồi `plc_engine_poll()`; `tud_task()` (USB) cũng chạy trong task đó (`components/os/sx_os.h`, `port/usb/splc_usb_cdc.c`).
- **[Đã xác minh]** `splc_assert_failed` (HEAD) tắt ngắt rồi vòng lặp vô hạn, ghi `g_splc_assert`. Debugger của người dùng bắt được PC dừng trong `xQueueGenericSend` với `configASSERT(pxQueue)` handle NULL, ngắt tắt (nhiệm vụ A, mục 8 ghi chú dự án). Chỉ IWDG độc lập mới thoát được trạng thái này.
- **[Đã xác minh]** `configUSE_IDLE_HOOK` và `configUSE_TICK_HOOK` đều bật. Không kick ở hai hook này (che mất task treo).
- **[Đã xác minh]** Bring-up ST67 chặn khoảng 5,8 s (`st67w611_start returned 0 after 5821 ms`), nên kênh NET phải có `grace()`.
- **[Đã xác minh]** MBTCP có hai task (`io_task`, `accept_task`) trong `SynaptixSDK/components/st67/st67_modbus_tcp.c` (host), `accept_task` chặn chờ client (vendor poll mỗi 100 ms).
- **[Suy luận]** Treo ở nhiệm vụ A có thể do restart module (`soft_reset` → `W6X_Net_DeInit`) khi MBTCP còn chạy. Watchdog là lưới an toàn, **không thay thế** việc sửa nguyên nhân.

---

## 3. Kiến trúc

```
                       +-----------------------------+
                       |  defaultTask (CubeMX, prio  |
                       |  cao nhất)  mỗi ~500 ms     |
                       |  splc_wdt_port_service()    |
                       +--------------+--------------+
                                      |
                                      v
 HOST (meshwf)               plc_wdt_service(now_ms)             LIB (simple_plc)
 ports/splc_port/            ---------------------->             services/plc_wdt/
 splc_wdt_port.c   <-- kick_fn / fault_fn --  - bảng kênh {tên, hạn, alive, grace}
   |  HAL_IWDG_Refresh                         - mọi kênh còn trong hạn?  -> gọi kick_fn
   |  log reset cause                          - có kênh quá hạn -> log_error, fault_fn,
   |  lưu fault                                  NGỪNG kick (IWDG tự reset)
   |
   +-- alive(NET)/grace(NET)  <- wifi_task (host)
                              alive(PLC)  <- plc_engine_poll() (lib)
```

Nguyên tắc:

1. **Một chỗ kick duy nhất**: `plc_wdt_service()` → `kick_fn` → `HAL_IWDG_Refresh`. Không task nào khác gọi refresh.
2. **Thư viện không biết HAL/ST67/RTOS cụ thể**: nó nhận hàm kick, hàm lưu fault, (tùy chọn) nguồn thời gian từ host. Cùng kiểu `plc_systag_set_comm_ok_fn()`.
3. **Host sở hữu phần cứng và các kênh phụ thuộc hạ tầng** (NET). Kênh PLC do thư viện tự báo.
4. **Chỉ giám sát "tiến triển", không giám sát "im lặng bình thường"**: task chờ sự kiện không bao giờ bị bắt buộc báo alive.

---

## 4. Thành phần

### 4.1 Thư viện: service `plc_wdt` (đề xuất)

Vị trí dự kiến: `simple_plc/simple_plc/services/plc_wdt/plc_wdt.c|h`; bản trong meshwf: `meshwf/SynaptixSDK/libs/simple_plc/services/plc_wdt/`.
Lưu ý: hai bản phải giữ đồng bộ.

API dự kiến (phác, chưa chốt tên):

| API | Vai trò |
|---|---|
| `int plc_wdt_register(ch, timeout_ms, boot_grace_ms)` | Đăng ký kênh, hạn phần mềm, thời gian ân hạn lúc boot |
| `void plc_wdt_alive(ch)` | Kênh báo còn sống (ghi một mốc thời gian 32 bit; gọi được từ nhiều task) |
| `void plc_wdt_grace(ch, ms)` | Gia hạn tạm cho tác vụ dài đã biết |
| `void plc_wdt_service(now_ms)` | Kiểm hạn mọi kênh, quyết định kick |
| `void plc_wdt_set_kick_fn(fn)` | Host đăng ký hàm kick |
| `void plc_wdt_set_fault_fn(fn)` | Host đăng ký hàm lưu thông tin trước khi IWDG hết hạn |
| (tùy chọn) `plc_wdt_set_now_ms_fn(fn)` | Nguồn thời gian nếu thư viện không tự lấy |

Khi `SPLC_WDG_ENABLE=0` các hàm là no-op.

Hành vi `plc_wdt_service()`:

1. Với mỗi kênh: hạn hiệu lực = `timeout_ms` (+ phần `grace` còn lại, hoặc `boot_grace_ms` nếu chưa từng alive).
2. Nếu mọi kênh còn trong hạn → gọi `kick_fn`.
3. Nếu có kênh quá hạn → `log_error` (tên kênh, độ trễ), gọi `fault_fn(ch, late_ms)`, **ngừng kick** (chốt không hồi sinh: kênh đã hỏng thì không tự khỏi).

### 4.2 Host: `splc_wdt_port` (đề xuất)

Vị trí dự kiến: `meshwf/SynaptixSDK/ports/splc_port/splc_wdt_port.c|h`.

| Hàm | Việc làm |
|---|---|
| `splc_wdt_port_init()` | Gán `kick_fn`, `fault_fn`; đăng ký kênh NET; log lý do reset; kiểm `hiwdg.Init` so với `SPLC_WDG_HW_TIMEOUT_MS` |
| `splc_wdt_port_service()` | Gọi `plc_wdt_service(now_ms)`, được gọi từ `defaultTask` |
| `wdt_kick()` | `HAL_IWDG_Refresh(&hiwdg)` |
| `wdt_fault()` | Lưu kênh và độ trễ (TAMP hoặc `.noinit`, xem mục 9) |

Phác code (minh họa, **không phải code thật**):

```c
#if SPLC_WDG_ENABLE
#include "main.h"            /* CubeMX: extern IWDG_HandleTypeDef hiwdg; */
#include "plc_wdt.h"

static void wdt_kick(void) { (void)HAL_IWDG_Refresh(&hiwdg); }

void splc_wdt_port_init(void)
{
    plc_wdt_set_kick_fn(wdt_kick);
    plc_wdt_set_fault_fn(wdt_fault);
    (void)plc_wdt_register(PLC_WDT_NET, SPLC_WDG_NET_TIMEOUT_MS, SPLC_WDG_BOOT_GRACE_MS);
}
#endif
```

### 4.3 Điểm ghép (host)

| Chỗ | Việc |
|---|---|
| `splc_port.c` | Gọi `splc_wdt_port_init()` sau `plc_engine_init()`; `alive(NET)` cuối mỗi vòng `wifi_task`; `grace(NET, ms)` trước `start`, connect, scan, restart module |
| `Core/Src/app_freertos.c` (`defaultTask`, vùng USER CODE) | Thêm `splc_wdt_port_service()` — người dùng tự thêm, vì file do CubeMX quản lý |
| `splcopts.h` | Các macro `SPLC_WDG_*` |
| `ports/splc_port/CMakeLists.txt` | Thêm file nguồn mới |

---

## 5. Kênh giám sát

| Kênh | Chủ | Báo `alive()` ở | Hạn khởi điểm | `grace()` ở |
|---|---|---|---|---|
| PLC | thư viện | Cuối mỗi lần `plc_engine_poll()` | 2 s [Chưa kiểm chứng] | Đầu ghi/xóa Flash rule |
| NET | host | Cuối mỗi vòng `wifi_task` | 45 s [Chưa kiểm chứng] | Đầu `start` ST67, connect, scan, restart module |

Không có kênh cho: task vendor, USB, MBTCP, `ST67dbg` (mục 1.2).

Vì sao 2 s và 45 s chỉ là khởi điểm:

- Vòng SPLC bình thường chạy cỡ 1 ms nên 2 s là dư; nâng lên 5–10 s cho lần chạy thử đầu nếu lo reset oan.
- ST67 có thể chặn lâu (bring-up ~5,8 s, connect/scan/recover). Cần **đo** khoảng cách lớn nhất giữa hai lần `alive` (log mỗi 30 s, kết hợp log heap sẵn có) trong vài ngày rồi mới chốt.
- Phương châm: đặt rộng lúc đầu, siết dần.

---

## 6. Thời gian và quan hệ giữa các tham số

Hai tầng khác nhau, **không cần bằng nhau**:

- `SPLC_WDG_HW_TIMEOUT_MS` (IWDG, 5 s): bắt treo toàn hệ thống (ngắt tắt, `defaultTask` không chạy). Giá trị thật do CubeMX (prescaler, reload); macro chỉ khai báo ý định và dùng để kiểm.
- `SPLC_WDG_PLC_TIMEOUT_MS` / `SPLC_WDG_NET_TIMEOUT_MS`: hạn phần mềm của từng kênh, do `plc_wdt_service()` kiểm tra. Quá hạn → ngừng kick, **chưa reset ngay**.

Ràng buộc thật sự:

1. `HW_TIMEOUT` ≥ 4 × `SERVICE_MS` (kiểm bằng `static_assert`; quy tắc kinh nghiệm).
2. `HW_TIMEOUT` > thời gian dài nhất supervisor có thể không chạy hợp lệ (ví dụ CPU đứng khi ghi/xóa Flash — chưa đo).
3. Hạn kênh > khoảng cách `alive` hợp lệ lớn nhất; tác vụ dài đã biết dùng `grace()`.
4. `HW_TIMEOUT` nhỏ hơn trần IWDG, chừa sai số LSI (xem 6.2).

### 6.1 Dòng thời gian khi task PLC kẹt (mốc t0 = lần `alive(PLC)` cuối)

| Thời điểm | Sự kiện |
|---|---|
| t0 → t0 + 2 s | Supervisor vẫn kick (~500 ms/lần), kênh còn trong hạn |
| ≈ t0 + 2 s | Quá hạn: `log_error`, `fault_fn`, ngừng kick |
| ≈ t0 + 6,5…7,5 s | IWDG hết `HW_TIMEOUT` kể từ lần kick cuối → reset |

Thời gian phục hồi ước tính (với 2 s / 0,5 s / 5 s):

- Treo toàn hệ thống: ≈ `HW_TIMEOUT` (5 s).
- Task PLC kẹt: ≈ 7,5 s.
- Task ST67 kẹt (hạn 45 s): ≈ 50 s.

Các con số này là [Suy luận], chưa đo.

### 6.2 Trần của IWDG

- Công thức: thời gian = (reload + 1) × prescaler / f_LSI. Reload 12 bit (tối đa 4095), prescaler tối đa /256, LSI danh định 32 kHz → khoảng 32,8 s. [Chưa kiểm chứng] — cần xác nhận trong RM0481 và trong CubeMX.
- Prescaler /64 chỉ tới khoảng 8,2 s.
- LSI sai số lớn → không thiết kế sát trần. Ràng buộc đề xuất: `HW_TIMEOUT_MS` ≤ cỡ 30 s.
- Hạn kênh NET 45 s vượt trần IWDG — đó là lý do cần supervisor phần mềm (IWDG không tự đợi 45 s).

---

## 7. Macro cấu hình (`splcopts.h`, đề xuất)

Chọn **một** tiền tố cho thống nhất (tài liệu dùng `SPLC_WDG_*`; có thể đổi thành `SPLC_WDT_*`).

| Macro | Mặc định | Ý nghĩa |
|---|---|---|
| `SPLC_WDG_ENABLE` | 0 | Bật tính năng. Mặc định tắt để không đổi hành vi board hiện có |
| `SPLC_WDG_HW_TIMEOUT_MS` | 5000 | Giá trị IWDG khai báo (thật sự cấu hình trong CubeMX) |
| `SPLC_WDG_SERVICE_MS` | 500 | Chu kỳ `plc_wdt_service()` |
| `SPLC_WDG_PLC_TIMEOUT_MS` | 2000 | Hạn kênh PLC |
| `SPLC_WDG_NET_TIMEOUT_MS` | 45000 | Hạn kênh NET |
| `SPLC_WDG_BOOT_GRACE_MS` | (chưa chốt) | Ân hạn đầu cho kênh chưa từng alive |

Kiểm lúc biên dịch (`static_assert`): `SPLC_WDG_HW_TIMEOUT_MS >= 4 * SPLC_WDG_SERVICE_MS`; `SPLC_WDG_HW_TIMEOUT_MS < trần IWDG`.
Kiểm lúc boot (host): so `hiwdg.Init` (prescaler, reload) tính ra với `SPLC_WDG_HW_TIMEOUT_MS`, lệch thì `log_warn`.

---

## 8. Cấu hình CubeMX (người dùng tự làm)

1. IWDG: **Activated** (khởi động bằng phần mềm). Prescaler và reload cho khoảng 5 s. Window để tối đa (không dùng chế độ cửa sổ).
2. **Không** bật hardware watchdog bằng option byte (không tắt được, ảnh hưởng nạp firmware và bootloader).
3. Đóng băng IWDG khi gỡ lỗi (DBGMCU `DBG_IWDG_STOP`); macro HAL tên chính xác kiểm trong `stm32h5xx_hal.h`. Không có thì mỗi breakpoint reset board.
4. `MX_IWDG_Init()` do CubeMX sinh trong `main()` trước khi scheduler chạy; thời gian từ đó đến lần kick đầu (khi `defaultTask` chạy) phải nhỏ hơn `HW_TIMEOUT` — cần kiểm trên board.
5. `defaultTask`: thêm `splc_wdt_port_service()` (priority `osPriorityNormal`; **[Chưa kiểm chứng]** là cao hơn SPLC/ST67/engine/Modem_Process trong wrapper CMSIS — kiểm trong `cmsis_os2.c` và `FreeRTOSConfig.h`).

---

## 9. Boot, lý do reset và lưu thông tin fault

- Ngay đầu boot, `log_info` lý do reset (cờ `IWDGRST` trong RCC; đồng thời phân biệt power-on, brown-out, nút reset).
- Hiện `g_splc_assert` nằm trong `.bss` nên bị xóa khi reset. Hai cách giữ lại:
  1. Vùng `.noinit` trong linker script (cần kiểm project có sẵn `STM32H523xx_RAM.ld`/`FLASH.ld` ở gốc repo simple_plc và thêm vùng nếu cần).
  2. Thanh ghi backup TAMP (không cần sửa linker, cần bật clock/quyền truy cập backup domain).
- `fault_fn` ghi: tên kênh, độ trễ, (nếu có) `g_splc_assert`.
- Sau boot, log nội dung đã lưu rồi xóa.

---

## 10. Danh sách trường hợp reset

### A. Supervisor ngừng kick (phần mềm phát hiện), IWDG hết hạn

- Kênh PLC quá hạn: `plc_engine_poll()` hoặc `tud_task()` kẹt; Modbus master chặn quá lâu; ghi/xóa Flash rule vượt `grace`; deadlock queue/semaphore.
- Kênh NET quá hạn: lệnh vendor chặn không timeout; `spi_xfer_engine`/`Modem_Process` kẹt kéo `wifi_task` đứng; `maybe_recover_module()`/`st67w611_soft_reset()`/`W6X_Net_DeInit()` treo; `start`, connect, scan vượt `grace`.
- Kênh đã đăng ký nhưng chưa bao giờ alive trong `boot_grace` (task không chạy, hoặc `xTaskCreate` thất bại).
- `SPLC_WDG_ENABLE=1` nhưng chưa đăng ký hàm kick (lỗi cấu hình; nên có cảnh báo lúc boot).

### B. IWDG phần cứng hết hạn (supervisor không chạy được, ≈ `HW_TIMEOUT`)

- `splc_assert_failed`/`configASSERT` (ngắt tắt, vòng lặp vô hạn).
- `HardFault`/`MemManage`... nếu handler là vòng lặp vô hạn [Chưa kiểm tra handler của dự án].
- Stack overflow hook/malloc-failed hook nếu là vòng lặp vô hạn [Chưa kiểm tra].
- `defaultTask` không được chạy (ISR/task cao hơn chiếm CPU).
- Tắt ngắt hoặc CPU đứng khi ghi/xóa Flash vượt `HW_TIMEOUT` (chưa đo).

### C. Do vận hành/cấu hình

- Debugger dừng breakpoint khi chưa đóng băng IWDG.
- Giữ nút PA0 7 s vào ROM bootloader (rủi ro R1).
- `HW_TIMEOUT` hoặc hạn kênh đặt quá nhỏ.

### D. Không reset (theo thiết kế)

- Mất Wi-Fi, không có IP, MQTT lỗi/mất kết nối, publish lỗi liên tiếp (chỉ ngắt/connect lại MQTT).
- Module ST67 vắng mặt hoặc sai firmware (T01/T02, lỗi `-8`) miễn `wifi_task` vẫn quay vòng (cần xác nhận vòng probe 5 s gọi `alive()` đều khi viết code).
- Client MBTCP treo, không có client, USB rút/cắm.
- Mất nguồn, brown-out, nút reset: reset do nguyên nhân riêng, không phải `IWDGRST`.

---

## 11. Rủi ro và giải pháp

| # | Rủi ro | Giải pháp đề xuất |
|---|---|---|
| R1 | Nhảy vào ROM bootloader (nút PA0 giữ 7 s, `splc_boot_button_port.c`): IWDG đang chạy không dừng được, board có thể reset sau ~5 s và không nạp được qua USB. Cách ROM bootloader xử lý IWDG phần mềm [Chưa kiểm chứng] | Nút chỉ đặt cờ + `NVIC_SystemReset()`; đầu `main()` (trước `MX_IWDG_Init`) kiểm cờ để nhảy vào bootloader. Sửa `splc_boot_button_port.c` và `main.c` |
| R2 | Ghi/xóa Flash làm CPU đứng (chưa đo) | `HW_TIMEOUT` không nhỏ (5 s); `grace()` ở kênh PLC; đo thời gian thật |
| R3 | Reset lặp vô hạn nếu lỗi xác định xảy ra ngay sau mỗi lần boot | Bộ đếm reset liên tiếp (mục 12, quyết định mở) |
| R4 | Watchdog che nguyên nhân gốc (nhiệm vụ A) | Lưu fault + log lý do reset; vẫn sửa nguyên nhân gốc |
| R5 | Hạn kênh quá chặt gây reset oan khi ST67 chặn lâu | `grace()`, hạn NET rộng, đo rồi siết |
| R6 | Host quên `alive()` ở một vòng mới | Liệt kê điểm bắt buộc (mục 4.3, 5); `boot_grace` bắt kênh chưa từng alive |
| R7 | Hai bản thư viện lệch nhau | Đồng bộ `simple_plc/simple_plc/X` ↔ `meshwf/SynaptixSDK/libs/simple_plc/X` |
| R8 | Reset làm mất Wi-Fi/MQTT ~6–10 s | Chấp nhận; log lý do reset để biết |

---

## 12. Quyết định mở

Đã thống nhất: dùng IWDG; kiến trúc thư viện + port host; tên file host `splc_wdt_port`.

Cần duyệt:

1. Mặc định ở mục 7 (đặc biệt `PLC_TIMEOUT`: 2 s hay rộng hơn 5–10 s cho lần thử đầu).
2. Hai kênh PLC và NET; MBTCP, USB, task vendor để ngoài bản đầu.
3. Giám sát kênh từ lúc đăng ký (kèm `boot_grace`) hay từ lần `alive()` đầu tiên. Đề xuất: từ lúc đăng ký.
4. Lưu fault: `.noinit`, TAMP, hay chỉ log.
5. Nút bootloader: làm cùng đợt hay để sau.
6. Có làm bộ đếm chống reset lặp vô hạn không.
7. Tiền tố macro: `SPLC_WDG_*` hay `SPLC_WDT_*`.
8. Vị trí tài liệu này trong repo (đề xuất: `simple_plc/simple_plc/docs/PLC_Watchdog_design.md`, cùng chỗ với `PLC_Indicator_design.md`; chờ quyết định trước khi sửa docs của simple_plc).

---

## 13. Kế hoạch triển khai (sau khi duyệt)

1. Thư viện: `plc_wdt` (đồng bộ hai bản), tích hợp `alive(PLC)` vào `plc_engine_poll()`, `grace()` ở ghi/xóa Flash rule.
2. Host: `splc_wdt_port.c|h`, `splcopts.h`, CMake, `splc_port.c` (alive/grace NET).
3. Người dùng: cấu hình CubeMX (mục 8), thêm `splc_wdt_port_service()` vào `defaultTask`.
4. Build trên máy người dùng (Windows PowerShell) — container của Claude không có `arm-none-eabi-gcc`/`cmake`.
5. Thử với `SPLC_WDG_ENABLE=1` và giá trị rộng, đọc log, đo khoảng cách `alive` lớn nhất, siết dần.
6. Xử lý R1 (nút bootloader) nếu đã duyệt.

---

## 14. Kế hoạch kiểm thử (trên board, do người dùng chạy)

| # | Kịch bản | Kết quả mong đợi |
|---|---|---|
| T1 | `SPLC_WDG_ENABLE=0` | Hành vi giống bản hiện tại |
| T2 | Chạy bình thường 24 h | Không reset oan; log khoảng cách `alive` lớn nhất |
| T3 | Chặn tạm vòng SPLC (vòng lặp thử) | `log_error` kênh PLC, reset sau ≈ hạn + `HW_TIMEOUT` |
| T4 | Chặn tạm `wifi_task` | `log_error` kênh NET, reset theo dòng thời gian mục 6.1 |
| T5 | Vòng lặp vô hạn với ngắt tắt | Reset sau ≈ `HW_TIMEOUT` |
| T6 | Boot sau reset | Log `IWDGRST` và nội dung fault đã lưu |
| T7 | Breakpoint khi debug | Không reset (đã đóng băng IWDG) |
| T8 | Mất Wi-Fi / mất MQTT | **Không** reset |
| T9 | Giữ nút PA0 7 s | Kiểm hành vi (liên quan R1) |

---

## 15. Thuật ngữ

- **IWDG**: Independent Watchdog, chạy bằng LSI, độc lập với clock chính.
- **LSI**: dao động nội tần số thấp (danh định ~32 kHz, sai số lớn).
- **Kick / refresh**: nạp lại bộ đếm IWDG.
- **Kênh (channel)**: một thành phần được giám sát bằng `alive()`.
- **Grace**: khoảng gia hạn tạm cho thao tác dài đã biết.
- **Fault**: thông tin lưu ngay trước khi IWDG reset.