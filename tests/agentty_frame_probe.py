#!/usr/bin/env python3
"""Count frames agentty writes to its terminal over time.

Usage: agentty_frame_probe.py BINARY [SECONDS]
A frame is one synchronized-output block (ESC[?2026h ... ESC[?2026l), or,
for terminals without it, one write burst. Prints frames and bytes per
second so an idle redraw loop shows up as a nonzero rate.
"""
import os, pty, sys, time, select, signal

binary = sys.argv[1]
total = int(sys.argv[2]) if len(sys.argv) > 2 else 20
pid, fd = pty.fork()
if pid == 0:
    os.environ["TERM"] = "xterm-256color"
    os.execv(binary, [binary, "--sandbox", "off", "-w", "/tmp"])

BEGIN, END = b"\x1b[?2026h", b"\x1b[?2026l"
start = time.time()
sent = False
buckets = {}   # second -> [frames, bytes]
tail = b""
while time.time() - start < total:
    r, _, _ = select.select([fd], [], [], 0.05)
    now = time.time() - start
    if not sent and now > 1.5:
        os.write(fd, b"\r")
        sent = True
    if not r:
        continue
    try:
        chunk = os.read(fd, 65536)
    except OSError:
        break
    if not chunk:
        break
    sec = int(now)
    b = buckets.setdefault(sec, [0, 0])
    b[1] += len(chunk)
    data = tail + chunk
    b[0] += data.count(END)
    tail = data[-len(END):]

os.kill(pid, signal.SIGTERM)
time.sleep(0.3)
try: os.kill(pid, signal.SIGKILL)
except OSError: pass
os.waitpid(pid, 0)
print("sec  frames   bytes")
for s in range(total):
    f, n = buckets.get(s, [0, 0])
    print(f"{s:3d}  {f:6d}  {n:7d}")
print("total frames:", sum(v[0] for v in buckets.values()),
      " total bytes:", sum(v[1] for v in buckets.values()))
