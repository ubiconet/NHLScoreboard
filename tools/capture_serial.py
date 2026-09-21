# Captures the ESP32 USB-CDC serial output to a file for a fixed duration.
# `pio device monitor` refuses to run non-interactively, so talk to the port
# with pyserial directly (usage: python capture_serial.py COM13 150 out.txt).
import sys
import time

import serial

port, seconds, out_path = sys.argv[1], int(sys.argv[2]), sys.argv[3]
deadline = time.time() + seconds
with serial.Serial(port, 115200, timeout=1) as ser, open(out_path, "wb") as out:
    while time.time() < deadline:
        data = ser.read(4096)
        if data:
            out.write(data)
            out.flush()
print("capture done")
