# SPLC-SPEC-MQTT-003: MQTT TLS/SSL Certificate Staging & Wire Contract ICD

**Tài liệu:** `SPLC-SPEC-MQTT-003`  
**Phiên bản:** `1.0`  
**Tiêu chuẩn liên quan:** `SPLC-SPEC-MQTT-001`, `SPLC-SPEC-MQTT-002`, `IEC 61131-3`  
**Đối tượng:** Kỹ sư Firmware Vi điều khiển (STM32, ESP32, FreeRTOS/Zephyr) & Kỹ sư Phần mềm Studio  
**Tệp Header C quy chuẩn:** [`docs/firmware/splc_mqtt_wire.h`](file:///g:/HoaNV/Projects/SimplePLC/docs/firmware/splc_mqtt_wire.h)  

---

## 1. Mục Đích & Nguyên Lý Kiến Trúc (Architectural Overview)

Trong các hệ thống tự động hóa công nghiệp và kho vận hiện đại (như SPX Logistics), việc kết nối MQTTS qua cổng bảo mật `8883` đòi hỏi thiết bị Gateway phải sở hữu chứng chỉ gốc **CA Certificate** (hoặc chứng chỉ Client mTLS) để xác thực máy chủ Broker nội bộ.

Do giới hạn bộ nhớ của khung truyền thông Modbus RTU, một tệp chứng chỉ PEM dung lượng 1.5 KB – 2.5 KB không thể nạp trong một khung Modbus đơn lẻ. Tiêu chuẩn **SPLC-SPEC-MQTT-003** xác lập cơ chế **Truyền tải phân mảnh theo Struct (Chunked Struct Transfer Protocol)**:
- Chia tệp chứng chỉ thành $N$ khối nhỏ ($N \le 32$).
- Mỗi khối được đóng gói vào một struct cố định **128 bytes = đúng 64 thanh ghi Modbus** (`SPLC_CertChunk_t`), bao gồm 8 bytes Header và tối đa 120 bytes Payload.
- Sử dụng vùng đệm Staging chuyên dụng tại địa chỉ **`0x0F00`** kết hợp với cơ chế phản hồi xác nhận **ACK (`0x0F40`)** và lệnh kích hoạt nguyên tử **COMMIT (`0x0F42`)**.

---

## 2. Bản Đồ Thanh Ghi Modbus Phân Vùng Certificate (Register Map)

| Địa chỉ Holding Reg | Tên Thanh Ghi | Chiều | Kiểu Dữ Liệu | Kích Thước | Chức Năng Kỹ Thuật |
| :--- | :--- | :---: | :---: | :---: | :--- |
| `0x0F00..0x0F3F` | `CERT_STAGING_BUFFER` | W | `SPLC_CertChunk_t` | 64 regs (128 B) | Vùng đệm nhận struct khối dữ liệu chứng chỉ (FC16 Write). |
| `0x0F40` | `CERT_ACK_CHUNK_INDEX` | R | UINT16 | 1 reg | MCU phản hồi chỉ số `chunk_index` vừa nhận và lưu thành công. |
| `0x0F41` | `CERT_STATUS_ERROR` | R | UINT16 | 1 reg | Trạng thái: `0` = OK / Ready, `1` = Lỗi CRC khối, `2` = Tràn bộ nhớ, `3` = Lỗi ghi Flash. |
| `0x0F42` | `CERT_COMMIT_COMMAND` | W | UINT16 | 1 reg | Lệnh chốt nguyên tử: Ghi `0xA5A5` để MCU lưu vĩnh viễn vào Flash và nạp vào mbedTLS. |
| `0x0F43` | `CERT_ACTIVE_TYPE_MASK`| R | UINT16 | 1 reg | Cờ phản ánh: Bit 0: Root CA active, Bit 1: Client Cert active, Bit 2: Client Key active. |

---

## 3. Cấu Trúc Nhị Phân Byte-Exact Của Struct (`SPLC_CertChunk_t`)

```c
#pragma pack(push, 1)

typedef struct SPLC_PACKED {
    /* --- HEADER: 8 Bytes = 4 Thanh Ghi Modbus (0x0F00..0x0F03) --- */
    uint8_t  cert_type;        /* 0x01: Root CA, 0x02: Client Cert, 0x03: Client Key */
    uint8_t  chunk_index;      /* Chỉ số khối: 0, 1, 2, ... (total_chunks - 1) */
    uint8_t  total_chunks;     /* Tổng số khối cần nạp (1 <= N <= 32) */
    uint8_t  chunk_len;        /* Số byte dữ liệu thực trong payload (1..120) */
    uint16_t total_cert_size;  /* Tổng kích thước toàn bộ tệp Certificate (bytes) */
    uint16_t chunk_crc16;      /* Checksum CRC-16 (Modbus polynomial 0xA001) của payload khối */

    /* --- PAYLOAD: 120 Bytes = 60 Thanh Ghi Modbus (0x0F04..0x0F3F) --- */
    uint8_t  payload[120];     /* Dữ liệu byte chuỗi ASCII PEM hoặc nhị phân DER */
} SPLC_CertChunk_t;

#pragma pack(pop)
```

---

## 4. Quy Trình Vận Hành Bắt Tay Nạp Chứng Chỉ (Handshake Sequence)

```text
Host (Studio)                                                      MCU Gateway
     │                                                                  │
     │ 1. Đọc HardwareCapabilities (0x0A08)                             │
     ├─────────────────────────────────────────────────────────────────>│
     │<─────────────────────────────────────────────────────────────────┤ (Có cờ 0x0080: MQTT_TLS_CERT)
     │                                                                  │
     │ 2. Gửi Chunk 0: FC16 Write 64 regs vào 0x0F00                   │
     ├─────────────────────────────────────────────────────────────────>│
     │                                                                  │ Kiểm tra chunk_crc16
     │                                                                  │ Ghi nối tiếp vào Flash buffer
     │                                                                  │ Set CERT_ACK = 0 (0x0F40)
     │ 3. Đọc CERT_ACK (0x0F40)                                         │
     ├─────────────────────────────────────────────────────────────────>│
     │<─────────────────────────────────────────────────────────────────┤ ACK = 0, Status = 0
     │                                                                  │
     │ ... Lặp lại cho đến khi gửi xong Chunk (N - 1) ...               │
     │                                                                  │
     │ 4. Ghi COMMIT_COMMAND: FC16 Write 0xA5A5 vào 0x0F42              │
     ├─────────────────────────────────────────────────────────────────>│
     │                                                                  │ Chốt file /spiffs/ca.crt
     │                                                                  │ Nạp vào mbedTLS context
     │                                                                  │ Set CERT_ACTIVE_MASK (0x0F43)
     │ 5. Đọc xác nhận hoàn tất                                         │
     │<─────────────────────────────────────────────────────────────────┤ Status = OK, ActiveMask = 0x01
```

---

## 5. Xử Lý Ngoại Lệ & Cơ Chế Fail-Safe

1. **Lỗi CRC Khối (`CERT_STATUS_ERROR = 1`):**
   Nếu MCU tính lại CRC-16 của 120 bytes payload không khớp với trường `chunk_crc16`, MCU giữ nguyên `CERT_ACK` cũ và đặt `CERT_STATUS_ERROR = 1`. Host sẽ tự động thử lại (Retry) khối đó tối đa 3 lần.
2. **Hết thời gian chờ (Timeout):**
   Mỗi lệnh ghi khối có timeout 1.500 ms. Nếu sau 3 lần retry không nhận được ACK, Host hủy bỏ quá trình nạp và giữ nguyên chứng chỉ cũ trên MCU.
3. **Rollback an toàn:**
   Chứng chỉ mới chỉ được kích hoạt thay thế chứng chỉ cũ sau khi lệnh COMMIT `0xA5A5` được thực thi thành công. Nếu quá trình truyền bị ngắt quãng giữa chừng, MCU tự động hủy bỏ vùng đệm tạm thời sau 30 giây.
