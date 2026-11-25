"""
maxctl.py - talk to the T-Deck MAX firmware's serial test console.

  python maxctl.py status
  python maxctl.py keys "hello\\n"        (\\n=Enter \\b=Backspace ~w/~s/~a/~d=Alt+nav ~q=back ~$=menu)
  python maxctl.py shot screen.png         (saves the e-paper frame buffer as a PNG)
  python maxctl.py run "keys m" "shot a.png" ...

Default port COM4; override with MAXCTL_PORT.
"""
import os
import struct
import sys
import time
import zlib

import serial

PORT = os.environ.get("MAXCTL_PORT", "COM4")


def open_port():
    s = serial.Serial()
    s.port = PORT
    s.baudrate = 115200
    s.timeout = 0.2
    s.dtr = True      # USB-JTAG: keep DTR high, don't toggle RTS (avoids resetting the chip)
    s.rts = False
    s.open()
    time.sleep(0.3)
    s.reset_input_buffer()
    return s


def send(s, line):
    s.write((line + "\n").encode())
    s.flush()


def read_until(s, end_marker, timeout=8.0):
    out = []
    t0 = time.time()
    buf = b""
    while time.time() - t0 < timeout:
        chunk = s.read(4096)
        if chunk:
            buf += chunk
            while b"\n" in buf:
                ln, buf = buf.split(b"\n", 1)
                ln = ln.decode(errors="replace").rstrip("\r")
                out.append(ln)
                if end_marker(ln):
                    return out
    return out


def write_png(path, w, h, rows):
    # rows: list of bytes, 1bpp, 1 = white  -> PNG grayscale 1-bit (1 = white)
    raw = b"".join(b"\x00" + r for r in rows)
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xFFFFFFFF)
    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 1, 0, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    with open(path, "wb") as f:
        f.write(png)


def do(s, cmd, arg=None):
    if cmd == "status":
        send(s, "!status")
        for ln in read_until(s, lambda l: l == " end"):
            print(ln)
    elif cmd == "keys":
        send(s, "!keys " + arg)
        read_until(s, lambda l: l == "OK", 3)
    elif cmd == "shot":
        send(s, "!shot")
        lines = read_until(s, lambda l: l == "END", 15)
        try:
            i = next(n for n, l in enumerate(lines) if l.startswith("SHOT"))
        except StopIteration:
            print("no screenshot received"); return
        rows = [bytes.fromhex(l) for l in lines[i + 1:i + 321] if len(l) == 60]
        if len(rows) != 320:
            print("short screenshot (%d rows)" % len(rows)); return
        write_png(arg or "shot.png", 240, 320, rows)
        print("saved", arg or "shot.png")
    elif cmd == "sleep":
        time.sleep(float(arg))
    elif cmd == "watch":
        # log raw keyboard scan codes for N seconds
        send(s, "!keylog on"); send(s, "!pinwatch on")
        t_end = time.time() + float(arg or 60)
        t0 = time.time()
        while time.time() < t_end:
            ln = s.readline().decode(errors="replace").strip()
            if ln.startswith(("KEY", "PIN", "TOUCH", "OK touch")):
                print("%5.1fs %s" % (time.time() - t0, ln), flush=True)
        send(s, "!keylog off"); send(s, "!pinwatch off")
    elif cmd == "raw":
        send(s, arg)
        for ln in read_until(s, lambda l: False, 2):
            print(ln)
    else:
        print("unknown command", cmd)


def main():
    if len(sys.argv) < 2:
        print(__doc__); return
    s = open_port()
    if sys.argv[1] == "run":
        for item in sys.argv[2:]:
            cmd, _, arg = item.partition(" ")
            do(s, cmd, arg or None)
    else:
        do(s, sys.argv[1], sys.argv[2] if len(sys.argv) > 2 else None)
    s.close()


if __name__ == "__main__":
    main()
