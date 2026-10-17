#!/usr/bin/env python3
"""Check agentty saves the window title at start and restores it at exit.

Usage: agentty_title_probe.py BINARY
Starts agentty in a pty, quits with ^C, and looks at the raw bytes: the
title push (CSI 22;0t) must come before our title (OSC 0) and the pop
(CSI 23;0t) must come at exit. Exit 0 when both are there in order.
"""
import os, pty, select, sys, time

binary = sys.argv[1]
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(binary, [binary, "--sandbox", "off", "-w", "/tmp"])


def pump(secs, buf):
    end = time.time() + secs
    while time.time() < end:
        r, _, _ = select.select([fd], [], [], 0.02)
        if r:
            try:
                d = os.read(fd, 65536)
            except OSError:
                return buf
            if not d:
                return buf
            buf += d
    return buf


out = pump(2.5, b"")
os.write(fd, b"\x03")          # ^C quits
out = pump(2.0, out)
try:
    os.waitpid(pid, 0)
except ChildProcessError:
    pass

push, title, pop = out.find(b"\x1b[22;0t"), out.find(b"\x1b]0;agentty"), out.rfind(b"\x1b[23;0t")
print(f"push={push} title={title} pop={pop} bytes={len(out)}")
ok = 0 <= push < title and pop > title
print("OK" if ok else "FAIL")
sys.exit(0 if ok else 1)
