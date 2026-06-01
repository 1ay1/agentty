#!/usr/bin/env bash
# Render the README demo animation to assets/demo.gif.
# Pipeline: gen_demo_frames.py -> per-frame SVGs -> rsvg-convert PNGs ->
# ffmpeg palettegen + paletteuse -> optimized looping GIF.
#
# Requires: python3, rsvg-convert (librsvg), ffmpeg.
set -euo pipefail
cd "$(dirname "$0")/.."

FRAMES=/tmp/demo_frames
FPS=8
SCALE=680   # output width; height auto

rm -rf "$FRAMES"
LOOP=$(python3 scripts/gen_demo_frames.py "$FRAMES")
echo "loop=${LOOP}s  frames=$(ls "$FRAMES"/*.svg | wc -l)"

# rasterize each frame
for f in "$FRAMES"/*.svg; do
  rsvg-convert "$f" -o "${f%.svg}.png"
done

mkdir -p assets

# palette for clean colors, then assemble
ffmpeg -y -loglevel error -framerate "$FPS" -i "$FRAMES/f%04d.png" \
  -vf "scale=${SCALE}:-1:flags=lanczos,palettegen=max_colors=64:stats_mode=diff" \
  "$FRAMES/palette.png"

ffmpeg -y -loglevel error -framerate "$FPS" -i "$FRAMES/f%04d.png" \
  -i "$FRAMES/palette.png" \
  -lavfi "scale=${SCALE}:-1:flags=lanczos[x];[x][1:v]paletteuse=dither=none:diff_mode=rectangle" \
  -loop 0 \
  assets/demo.gif

echo "wrote assets/demo.gif ($(du -h assets/demo.gif | cut -f1))"
