#!/usr/bin/env python3
"""Fake rain-gauge device on a pseudo-terminal, for testing without hardware.
Behaviour mirrors what was observed on the real device (see docs/DESIGN.md).
  python3 tools/devsim.py [--endian le|be] [--history-hours 120] [--bad-crc 0.0]
Prints the pty path on the first line of stdout."""
import os, pty, sys, time, select, random, argparse, tty
from datetime import datetime, timedelta

MACHINE = b"SRY1C-2211-00001"

def crc16(data):
    c = 0
    for b in data:
        c ^= b
        for _ in range(8):
            c = (c >> 1) ^ 0x8408 if c & 1 else c >> 1
    return c

def frame(body):
    c = crc16(body)
    return bytes(body) + bytes([c & 255, c >> 8])

RAIN_EVERY = False        # set by --rain-every-hour: deterministic rain in minutes 0-2 of every hour

def hour_minutes(h):
    """Deterministic rain pattern: tip counts (10 = 0.1 mm) per minute of hour h."""
    m = [0] * 60
    if RAIN_EVERY:
        m[0] = 10; m[1] = 20; m[2] = 30
        return m
    if (h.day + h.hour) % 4 == 0:
        base = (h.hour * 7) % 40 + 2
        m[base] = 10; m[base + 1] = 20; m[base + 5] = 10
        if h.hour % 3 == 0: m[(base + 11) % 60] = 30
    return m

class Device:
    def __init__(self, a):
        self.a = a
        self.clock_off = timedelta(seconds=a.clock_offset)
        self.rollover = set()
        for item in (a.rollover or "").split(","):
            item = item.strip()
            if item:
                self.rollover.add(datetime.strptime(item, "%Y-%m-%d %H"))
    def now(self):
        return datetime.now() + self.clock_off
    def aa00_or_05(self, h):
        now = self.now()
        cur = now.replace(minute=0, second=0, microsecond=0)
        m = hour_minutes(h)
        partial = h == cur
        if partial:
            m = [v if i <= now.minute else 0 for i, v in enumerate(m)]
        temp = 0xFFFF if partial else 150 + h.hour
        date = bytes([h.year - 2000, h.month, h.day, h.hour])
        if sum(m) == 0:
            return frame(b"\xAA\x00" + date + bytes([temp & 255, temp >> 8]))
        body = b"\xAA\x05" + date
        for v in m:
            body += bytes([v >> 8, v & 255]) if self.a.endian == "be" else bytes([v & 255, v >> 8])
        return frame(body + bytes([temp & 255, temp >> 8]))
    def respond(self, pkt):
        code, sub = pkt[2], pkt[3]
        n = self.now()
        if code == 0x01: return frame(b"\xAA\x01" + MACHINE)
        if code == 0x03:
            return frame(bytes([0xAA, 0x03, n.year - 2000, n.month, n.day, n.hour, n.minute, n.second, (n.weekday() + 1) % 7]))
        if code == 0x06:
            body = bytearray([0xAA, 0x06, n.year - 2000, n.month, n.day, n.hour, n.minute, n.second, (n.weekday() + 1) % 7, 0, 0, 0, 0])
            for v in (7014, 7014, 9000, 5900, 5800): body += bytes([v & 255, v >> 8])
            body += bytes([22]) + bytes(26)
            return frame(bytes(body))
        if code == 0x00 and sub == 0x01:
            yy, mo, dd, hh, flag = pkt[4:9]
            END = frame(b"\xAA\x09\x00\x03")
            try: h = datetime(2000 + yy, mo, dd, hh)
            except ValueError: return END
            cur = self.now().replace(minute=0, second=0, microsecond=0)
            if flag == 0 or h > cur or (cur - h) > timedelta(hours=self.a.history_hours): return END
            if h in self.rollover:
                # firmware quirk observed in the field: this hour is answered with the
                # NEXT hour's record (date in the frame is h+1h, e.g. 23:00 -> 00:00)
                return self.aa00_or_05(h + timedelta(hours=1)) + END
            return self.aa00_or_05(h) + END
        return b""

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--endian", default="le"); ap.add_argument("--history-hours", type=int, default=120)
    ap.add_argument("--bad-crc", type=float, default=0.0); ap.add_argument("--clock-offset", type=int, default=11)
    ap.add_argument("--rollover", default="",
                    help="comma list of 'YYYY-MM-DD HH' hours answered with the next hour's record")
    ap.add_argument("--rain-every-hour", action="store_true",
                    help="rain in minutes 0-2 of every hour (deterministic, for live-mode tests)")
    a = ap.parse_args()
    global RAIN_EVERY
    RAIN_EVERY = a.rain_every_hour
    master, slave = pty.openpty()
    tty.setraw(master); tty.setraw(slave)
    print(os.ttyname(slave), flush=True)
    dev = Device(a); buf = b""; random.seed(1)
    while True:
        r, _, _ = select.select([master], [], [], 0.5)
        if not r: continue
        try: buf += os.read(master, 256)
        except OSError: break
        while True:
            i = buf.find(b"\x55")
            if i < 0: buf = b""; break
            buf = buf[i:]
            if len(buf) < 2 or len(buf) < buf[1]: break
            pkt, buf = buf[:buf[1]], buf[buf[1]:]
            if crc16(pkt[:-2]) != pkt[-2] | pkt[-1] << 8: continue
            out = dev.respond(pkt)
            if not out: continue
            if random.random() < a.bad_crc:
                out = bytearray(out); out[-1] ^= 0xFF; out = bytes(out)
            time.sleep(random.uniform(0.02, 0.06))
            cut = random.randint(1, len(out) - 1) if len(out) > 20 and random.random() < 0.5 else 0
            if cut:
                os.write(master, out[:cut]); time.sleep(0.015); os.write(master, out[cut:])
            else:
                os.write(master, out)

if __name__ == "__main__":
    main()
