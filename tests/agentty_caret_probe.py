#!/usr/bin/env python3
"""Drive agentty in a pty and report the cursor escapes it writes per step.

Usage: agentty_caret_probe.py BINARY STEP...
A step is a literal string to type, a key name (ENTER ESC UP DOWN CK
FOCUSIN FOCUSOUT), or WAITn.n to just listen. Set DUMP=<step> to print
that step's raw bytes.
"""
import os, pty, sys, time, select, re, fcntl, termios, struct

binary = sys.argv[1]
steps = sys.argv[2:]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(binary, [binary, "--sandbox", "off", "-w", "/tmp"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", 40, 120, 0, 0))

def pump(secs):
    buf = b""
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                break
            if not d:
                break
            buf += d
    return buf

KEYS = {"ENTER": b"\r", "ESC": b"\x1b", "DOWN": b"\x1b[B", "UP": b"\x1b[A",
        "CK": b"\x0b", "FOCUSIN": b"\x1b[I", "FOCUSOUT": b"\x1b[O"}
pump(2.0)
os.write(fd, b"\r")          # dismiss the sandbox-off splash
pump(1.5)
for s in steps:
    if s.startswith("WAIT"):
        b = pump(float(s[4:]))
    else:
        os.write(fd, KEYS.get(s, s.encode()))
        b = pump(0.6)
    ops = re.findall(rb"\x1b\[\?25[hl]|\x1b\[\d? q|\x1b\]11?2[^\x1b]*\x1b\\", b)
    print(f"== {s}: {len(b)} bytes; cursor ops: {[o.decode('latin1') for o in ops]}")
    if os.environ.get("DUMP") == s:
        print(b.decode("latin1").encode("unicode_escape").decode()[-3000:])
os.kill(pid, 9)
