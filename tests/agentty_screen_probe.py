#!/usr/bin/env python3
"""Drive agentty in a pty, feed keys, and print the rendered screen.

Usage: agentty_screen_probe.py BINARY STEP...
A step is a literal string to type, a key name (ENTER ESC UP DOWN TAB
BS), or WAITn.n. The screen after the last step is printed as text.
Needs pyte (pip install pyte).
"""
import fcntl, os, pty, select, struct, sys, termios, time

import pyte

COLS = int(os.environ.get("COLS", 100))
ROWS = int(os.environ.get("ROWS", 34))
KEYS = {"ENTER": b"\r", "ESC": b"\x1b", "UP": b"\x1b[A", "DOWN": b"\x1b[B",
        "TAB": b"\t", "BS": b"\x7f"}

binary, steps = sys.argv[1], sys.argv[2:]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(binary, [binary, "--sandbox", "off", "-w", "/tmp"])
fcntl.ioctl(fd, termios.TIOCSWINSZ, struct.pack("HHHH", ROWS, COLS, 0, 0))

screen = pyte.Screen(COLS, ROWS)
stream = pyte.ByteStream(screen)


def pump(secs):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return
            if not d:
                return
            stream.feed(d)


pump(2.5)
for s in steps:
    if s.startswith("WAIT"):
        pump(float(s[4:]))
        continue
    os.write(fd, KEYS.get(s, s.encode()))
    pump(0.4)
pump(0.6)
for line in screen.display:
    if line.strip():
        print(line.rstrip())
os.kill(pid, 9)
