"""Soak watcher for crash hunting: exits as soon as a board shows a reboot.

- USB board (--log): tails a tools/serial_monitor.py capture for a panic
  dump or a new ROM boot banner (UART0 — panics print there even on
  production builds).
- LAN board (--board): polls its /status every --poll-min minutes and flags
  a reboot (uptime short of where it should be) or a stored crash that
  belongs to the running firmware (lastCrash.thisFirmware). Each poll
  counts as portal activity and pauses that board's feeds for a couple of
  minutes, so keep polls sparse.

Exit code 1 = USB board event, 2 = LAN board event, 0 = window finished clean.

  python tools/soak_watch.py --log soak.log --skip-lines 22 \
      --board 192.168.1.20 --poll-min 20 --minutes 240
"""
import argparse
import json
import re
import sys
import time
import urllib.request

ap = argparse.ArgumentParser()
ap.add_argument("--log", default="", help="serial_monitor.py capture to tail")
ap.add_argument("--skip-lines", type=int, default=0,
                help="log lines already reviewed (e.g. the boot banner)")
ap.add_argument("--board", default="", help="LAN board IP to poll")
ap.add_argument("--poll-min", type=float, default=20)
ap.add_argument("--minutes", type=float, default=240)
args = ap.parse_args()
if not args.log and not args.board:
    ap.error("give --log, --board, or both")

TRIGGERS = re.compile(r"Guru Meditation|rst:0x|abort\(\)|Backtrace:|"
                      r"Task watchdog|stack overflow|CORRUPT HEAP|assert failed")
deadline = time.time() + args.minutes * 60
next_poll = time.time()
last_uptime = None
last_poll_at = 0.0
seen = args.skip_lines


def stamp():
    return time.strftime("%H:%M:%S")


while time.time() < deadline:
    if args.log:
        with open(args.log, encoding="utf-8", errors="replace") as f:
            lines = f.readlines()
        for line in lines[seen:]:
            if TRIGGERS.search(line):
                print(f"{stamp()} USB BOARD EVENT: {line.rstrip()}")
                print("".join(lines[max(seen, len(lines) - 40):]))
                sys.exit(1)
        seen = len(lines)

    if args.board and time.time() >= next_poll:
        next_poll = time.time() + args.poll_min * 60
        try:
            with urllib.request.urlopen(f"http://{args.board}/status",
                                        timeout=10) as r:
                s = json.load(r)
            up = s.get("uptimeSec", 0)
            crash = s.get("lastCrash") or {}
            print(f"{stamp()} board up={up}s reset={s.get('resetReason')} "
                  f"sched={s['feeds']['schedOk']}/{s['feeds']['schedAttempts']} "
                  f"heap={s['feeds']['freeHeap']} "
                  f"crashThisFw={crash.get('thisFirmware')}", flush=True)
            # A reboot early in a poll gap still leaves uptime above the last
            # reading — compare against the uptime the board SHOULD have.
            if last_uptime is not None:
                expected = last_uptime + (time.time() - last_poll_at)
                if up + 60 < expected:
                    booted = time.strftime("%H:%M:%S",
                                           time.localtime(time.time() - up))
                    print(f"{stamp()} BOARD REBOOTED at ~{booted} "
                          f"(expected up~{int(expected)}s): {json.dumps(s)}")
                    sys.exit(2)
            if crash.get("thisFirmware"):
                print(f"{stamp()} BOARD CRASHED ON THIS FIRMWARE: "
                      f"{json.dumps(s)}")
                sys.exit(2)
            last_uptime = up
            last_poll_at = time.time()
        except Exception as e:  # one unreachable poll is not a verdict
            print(f"{stamp()} board poll failed: {e}", flush=True)
    time.sleep(5)

print(f"{stamp()} soak window finished with no reboot")
