"""Long-running serial monitor for crash hunting.

Logs every line from the board's UART with a host timestamp, flushing as it
goes, and never toggles DTR/RTS (on the CH343 port those drive EN/IO0 and
would reset the board). Optional --reset pulses RTS once at start.

  python tools/serial_monitor.py COM59 --minutes 60 --log crash.log
"""
import argparse
import datetime
import sys
import time

import serial


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("port")
    ap.add_argument("--baud", type=int, default=115200)
    ap.add_argument("--minutes", type=float, default=30)
    ap.add_argument("--log", required=True)
    ap.add_argument("--reset", action="store_true")
    args = ap.parse_args()

    ser = serial.Serial()
    ser.port = args.port
    ser.baudrate = args.baud
    ser.timeout = 0.2
    ser.dtr = False
    ser.rts = False
    ser.open()
    if args.reset:
        ser.rts = True
        time.sleep(0.12)
        ser.rts = False

    deadline = time.time() + args.minutes * 60
    pending = b""
    with open(args.log, "a", encoding="utf-8", errors="replace") as log:
        log.write("=== monitor start %s port=%s ===\n" % (
            datetime.datetime.now().isoformat(timespec="seconds"), args.port))
        log.flush()
        while time.time() < deadline:
            data = ser.read(4096)
            if not data:
                continue
            pending += data
            *lines, pending = pending.split(b"\n")
            stamp = datetime.datetime.now().strftime("%H:%M:%S.%f")[:-3]
            for raw in lines:
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                log.write("%s %s\n" % (stamp, text))
            log.flush()
        if pending:
            log.write("%s %s\n" % (datetime.datetime.now().strftime("%H:%M:%S"),
                                   pending.decode("utf-8", errors="replace")))
        log.write("=== monitor end %s ===\n" % datetime.datetime.now().isoformat(
            timespec="seconds"))
    ser.close()


if __name__ == "__main__":
    sys.exit(main())
