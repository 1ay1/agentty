#!/usr/bin/env python3
"""Drive agentty in a pty, feed keys, and print the rendered screen.

Usage: agentty_screen_probe.py BINARY STEP...
A step is a literal string to type, a key name (ENTER ESC UP DOWN TAB
BS CTRLV), PASTEFILE=path (bracketed paste of that file), or WAITn.n.
The screen after the last step is printed as text.
Needs pyte (pip install pyte).
"""
import fcntl, os, pty, select, struct, sys, termios, time

import pyte

COLS = int(os.environ.get("COLS", 100))
ROWS = int(os.environ.get("ROWS", 34))
KEYS = {"ENTER": b"\r", "ESC": b"\x1b", "UP": b"\x1b[A", "DOWN": b"\x1b[B",
        "TAB": b"\t", "BS": b"\x7f", "CTRLV": b"\x16"}

binary, steps = sys.argv[1], sys.argv[2:]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    extra = os.environ.get("PROBE_ARGS", "").split()
    os.execv(binary, [binary, "--sandbox", "off", "-w", "/tmp", *extra])
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
    if s.startswith("PASTEFILE="):
        # Bracketed paste of a file's contents.
        with open(s[len("PASTEFILE="):], "rb") as f:
            os.write(fd, b"\x1b[200~" + f.read() + b"\x1b[201~")
        pump(1.0)
        continue
    if s.startswith("PASTECHUNKS="):
        # The same, delivered in 4 KB pieces like a real terminal over ssh.
        with open(s[len("PASTECHUNKS="):], "rb") as f:
            data = b"\x1b[200~" + f.read() + b"\x1b[201~"
        for i in range(0, len(data), 4096):
            os.write(fd, data[i:i + 4096])
            pump(0.002)
        pump(1.0)
        continue
    if s.startswith("RAWFILE="):
        # Exactly these bytes, in 4 KB writes (a capture of what a terminal sent).
        with open(s[len("RAWFILE="):], "rb") as f:
            data = f.read()
        for i in range(0, len(data), 4096):
            os.write(fd, data[i:i + 4096])
            pump(0.002)
        pump(1.0)
        continue
    if s.startswith("PAUSE"):
        time.sleep(float(s[5:]))
        pump(0.01)
        continue
    if s.startswith("SLOWFILE="):
        # These bytes at ~64 KB/s, like a paste crossing ssh from a laptop.
        with open(s[len("SLOWFILE="):], "rb") as f:
            data = f.read()
        for i in range(0, len(data), 1024):
            os.write(fd, data[i:i + 1024])
            pump(0.016)
        pump(1.0)
        continue
    if s.startswith("TYPEFILE="):
        # No bracketed paste at all: the bytes arrive as if typed.
        with open(s[len("TYPEFILE="):], "rb") as f:
            data = f.read()
        for i in range(0, len(data), 4096):
            os.write(fd, data[i:i + 4096])
            pump(0.002)
        pump(1.0)
        continue
    os.write(fd, KEYS.get(s, s.encode()))
    pump(0.4)
pump(0.6)
full = os.environ.get("FULL")
for i, line in enumerate(screen.display):
    if full:
        print(f"{i:2d}|{line.rstrip()}")
    elif line.strip():
        print(line.rstrip())
os.kill(pid, 9)
