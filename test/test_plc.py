import serial, time
s = serial.Serial("COM8", 115200, 8, "N", 1, timeout=1.0)
s.reset_input_buffer()
s.write(bytes.fromhex("01 03 00 00 00 0A C5 CD"))   # đọc 10 reg từ 0x0000, unit 1
time.sleep(0.5)
print("RX:", s.read(64).hex(" "))