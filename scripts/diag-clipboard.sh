#!/usr/bin/env bash
# diag-clipboard.sh — pinpoint where kitty-through-tmux image paste breaks.
# Run this INSIDE your tmux pane in kitty. Copy an image to the clipboard first
# (e.g. screenshot), then run: bash diag-clipboard.sh
set -u

echo "=== Environment ==="
echo "TERM=$TERM"
echo "TMUX=${TMUX:-<unset>}"
echo "TERM_PROGRAM=${TERM_PROGRAM:-<unset>}"
echo "KITTY_WINDOW_ID=${KITTY_WINDOW_ID:-<unset>}"
echo

echo "=== 1. tmux passthrough (must be 'on') ==="
tmux show -g allow-passthrough 2>/dev/null || echo "  (not in tmux?)"
echo

echo "=== 2. Outer terminal (must be xterm-kitty) ==="
tmux display -p '#{client_termname}' 2>/dev/null || echo "  (not in tmux?)"
echo

echo "=== 3. kitty clipboard_control (must allow read-clipboard, NOT -ask) ==="
grep -E "^clipboard_control" ~/.config/kitty/kitty.conf 2>/dev/null || echo "  (default)"
echo

echo "=== 4. Live OSC 5522 round-trip test ==="
echo "Sending a tmux-wrapped OSC 5522 read request and waiting 2s for a reply…"
echo "(If kitty prompts for clipboard permission, ALLOW it.)"
echo

# Build the request: OSC 5522 ; type=read ; base64("image/png text/plain") ST
mimes="image/png text/plain"
b64=$(printf '%s' "$mimes" | base64 -w0)
osc=$'\e]5522;type=read;'"$b64"$'\e\\'

# Wrap for tmux: ESC P tmux ; <payload with ESCs doubled> ESC \
# Double every ESC (0x1b) in the payload.
wrapped_payload=$(printf '%s' "$osc" | sed 's/\x1b/\x1b\x1b/g')
wrapped=$'\ePtmux;'"$wrapped_payload"$'\e\\'

# Put terminal in raw mode, send request, capture reply.
old_stty=$(stty -g 2>/dev/null)
stty raw -echo min 0 time 20 2>/dev/null   # 2s read timeout
printf '%s' "$wrapped" > /dev/tty
reply=$(dd bs=4096 count=1 2>/dev/null < /dev/tty)
stty "$old_stty" 2>/dev/null

echo
if [ -z "$reply" ]; then
  echo "RESULT: NO REPLY. The request didn't round-trip. Most likely:"
  echo "  • allow-passthrough is off (see #1), or"
  echo "  • outer terminal isn't kitty (see #2), or"
  echo "  • kitty denied the read (see #3 — needs 'read-clipboard' without -ask), or"
  echo "  • kitty/tmux wasn't reloaded after config changes."
else
  # Show a safe, truncated view of the reply.
  echo "RESULT: GOT A REPLY (${#reply} bytes). Terminal side WORKS."
  printf '%s' "$reply" | head -c 200 | cat -v
  echo
  if printf '%s' "$reply" | grep -q "status=OK\|status=DATA"; then
    echo "→ kitty is answering with clipboard data. agentty should paste it."
  elif printf '%s' "$reply" | grep -q "status=EPERM"; then
    echo "→ kitty denied permission (EPERM). Set clipboard_control to include"
    echo "  'read-clipboard' (not read-clipboard-ask) and reload kitty."
  elif printf '%s' "$reply" | grep -q "status=ENOSYS"; then
    echo "→ No image on the clipboard (ENOSYS). Copy an image first."
  fi
fi
