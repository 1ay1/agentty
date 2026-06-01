#!/usr/bin/env python3
"""Render scripts/gen_demo_svg.py's animation to a sequence of PNG frames
(by emitting one static SVG per timestamp with the right rows revealed),
then the wrapper shell script stitches them into assets/demo.gif via
ffmpeg. GIF is the only format GitHub reliably animates inside a README.

This imports the schedule + content from gen_demo_svg so the GIF and the
SVG stay in lockstep.

Run via scripts/gen_demo_gif.sh
"""
import os
import sys
import re

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_demo_svg as g  # noqa: E402

OUT = sys.argv[1] if len(sys.argv) > 1 else "/tmp/demo_frames"
FPS = 8

# Rebuild once to get the full SVG, then we parse each <text id=...> row's
# fade `begin` time out of its keyTimes so we can decide per-frame
# visibility. Simpler: regenerate but capture (id -> appear_time).
appear = {}

# Monkeypatch g.build's `add` is awkward; instead replicate the timing by
# re-running build and scraping animate keyTimes * loop.
svg = g.build()

# loop duration is encoded in every animate dur=".."
m = re.search(r'dur="([\d.]+)s"', svg)
loop = float(m.group(1))

# parse rows: id, full element, second keyTime (= appear/loop)
rows = re.findall(
    r'(<text id="([^"]+)"[^>]*opacity="0">.*?</text>)', svg, re.S)
row_appear = {}
for full, rid in rows:
    km = re.search(r'keyTimes="0;([\d.]+);', full)
    row_appear[rid] = float(km.group(1)) * loop if km else 0.0

# Build a "static skeleton" = everything except the animated text rows,
# plus the rows with opacity controlled per-frame.
# Strip <animate> children and the opacity attr; we'll set opacity inline.
def frame_svg(t):
    out = svg
    # remove all <animate ...> tags
    out = re.sub(r'<animate[^>]*/>', '', out)
    # for each animated row, set opacity 1 if t>=appear else 0
    def repl(mo):
        full, rid = mo.group(1), mo.group(2)
        op = "1" if t >= row_appear.get(rid, 0.0) else "0"
        return full.replace('opacity="0"', f'opacity="{op}"')
    out = re.sub(r'(<text id="([^"]+)"[^>]*opacity="0">.*?</text>)',
                 repl, out, flags=re.S)
    return out


os.makedirs(OUT, exist_ok=True)
n = int(loop * FPS)
for i in range(n):
    t = i / FPS
    with open(f"{OUT}/f{i:04d}.svg", "w") as fh:
        fh.write(frame_svg(t))

print(loop)  # the wrapper reads this
