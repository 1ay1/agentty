---
title: Clipboard & Images
description: How image paste works locally and over SSH — kitty's read permission, tmux passthrough, and the clipboard ferry for every other terminal.
nav_section: User Manual
nav_order: 25
slug: clipboard
---

Paste an image straight into the composer with [[Ctrl+V]] (or [[Alt+V]] where
your terminal intercepts Ctrl+V). Screenshots, diagrams, a failing UI — they
attach as real image content, not a file path.

Locally this just works. **Over SSH it needs one setting**, and which setting
depends on your terminal. This page is the whole story.

## Why SSH is different

Your clipboard lives on the machine your **terminal** runs on. When agentty
runs on a remote host, that host's clipboard is empty — your screenshot never
left your laptop.

So agentty asks the *terminal* instead, over the same pty the session already
uses. Two escape dialects exist, and it sends both at once:

| Dialect | Carries | Supported by |
|---------|---------|--------------|
| **OSC 5522** | images **and** text | **kitty only** |
| **OSC 52** | text only | iTerm2, WezTerm, Ghostty, foot, Terminal.app, xterm |

A terminal that doesn't know OSC 5522 ignores it and answers the text request.
That is why an image paste can silently arrive as text: nothing failed, the
image dialect simply went unanswered.

:::note
agentty tells you when this happens — the toast names your exact situation
rather than leaving the paste unexplained. If you see one, the fix is below.
:::

## kitty — allow clipboard reads

**This is the most common surprise.** kitty implements OSC 5522 fully, but its
`clipboard_control` option defaults to **write-only**:

```conf
# kitty's default — note the absence of any read-* verb
clipboard_control write-clipboard write-primary
```

A read request is answered `EPERM`, the image never arrives, and the text reply
lands instead. Image paste appears broken on the one terminal that supports it.

Add the read verbs to `~/.config/kitty/kitty.conf` (the same path on Linux and
macOS — on macOS kitty also honours the legacy
`~/Library/Preferences/kitty/kitty.conf`, but only if the `~/.config` one is
absent):

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
```

Then **fully restart kitty** — a config reload does not re-negotiate this.

:::tip
`read-clipboard-ask` / `read-primary-ask` are the paranoid variants: kitty
prompts you per read instead of allowing it outright. Both work with agentty.
:::

### Inside tmux, also enable passthrough

tmux swallows escape sequences it doesn't recognise. agentty wraps the request
in tmux's passthrough envelope, but that envelope is **disabled by default**:

```bash
tmux set -g allow-passthrough on
```

Make it permanent in `~/.tmux.conf`:

```conf
set -g allow-passthrough on
```

Existing tmux sessions pick this up immediately — no restart needed.

:::note
Contrary to a widespread belief that tmux 3.4 turned this on, it still defaults
to **off** (verified on tmux 3.7 with a stock config). If image paste works
outside tmux but not inside, this is why.
:::

### The full kitty-over-SSH checklist

Running agentty on a remote host, from kitty on your laptop, inside tmux:

1. `clipboard_control … read-clipboard read-primary` in `kitty.conf` — **on the laptop**, where kitty runs
2. Restart kitty completely
3. `set -g allow-passthrough on` in tmux
4. Paste with [[Ctrl+V]]

Step 1 catches almost everyone: it is configured on the **local** machine, not
the host agentty runs on.

## Every other terminal — the clipboard ferry

Terminals without OSC 5522 cannot send image bytes at all. Two options.

### Attach by path

Save the screenshot and reference it with `@`:

```
@~/Desktop/screenshot.png  what's wrong with this layout?
```

Zero setup, works everywhere, and is often faster for a file you already have
on disk.

### Ferry the clipboard over SSH

`AGENTTY_CLIPBOARD_CMD` names a command that writes **raw image bytes to
stdout**. agentty runs it on the remote host; it reaches back to your laptop
over the connection you already have open:

```bash
# macOS laptop (brew install pngpaste)
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop pngpaste -'

# Wayland laptop
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop wl-paste -t image/png'

# X11 laptop
export AGENTTY_CLIPBOARD_CMD='ssh your-laptop xclip -selection clipboard -t image/png -o'
```

This requires the laptop to be reachable from the remote host — a reverse
tunnel, a VPN, or a mesh network like Tailscale.

The override takes **priority over every other clipboard path**, so it also
works as an escape hatch when a local tool is misbehaving.

:::tip
Test it independently before blaming agentty:

```bash
ssh your-laptop pngpaste - | file -
# expected: /dev/stdin: PNG image data, 1234 x 567, ...
```

If that doesn't print image data, fix the command first.
:::

## Local setups

### Linux

Install a clipboard tool for your session type — agentty shells out to it:

```bash
# Wayland
sudo pacman -S wl-clipboard     # or: apt install wl-clipboard

# X11
sudo pacman -S xclip            # or: apt install xclip
```

agentty detects Wayland via `XDG_SESSION_TYPE` / `WAYLAND_DISPLAY` and prefers
`wl-paste`, falling back to `xclip`.

### macOS and Windows

No setup. Both expose image clipboards natively.

## Troubleshooting

| Symptom | Cause | Fix |
|---------|-------|-----|
| Paste inserts **text** where you expected an image | Terminal answered OSC 52, not 5522 | kitty: allow reads (above). Others: ferry or `@path` |
| Nothing happens at all | Request never reached the terminal | In tmux: `allow-passthrough on` |
| "tmux answers reads from its own paste buffer" | tmux is serving its internal buffer | `tmux set -g get-clipboard both` |
| "no clipboard tool" on Linux | No `wl-paste` / `xclip` | Install one (above) |
| Works outside tmux, not inside | Passthrough disabled | `set -g allow-passthrough on` |
| Ferry set but still text | The command failed | Test it with `\| file -`, then check `AGENTTY_LOG=general=debug` |

Still stuck? Run with logging and include the output in an issue:

```bash
AGENTTY_LOG=general=debug AGENTTY_LOG_FILE=/tmp/agentty.log agentty
```

The log records which clipboard path was attempted and why it gave up — which
is usually enough to identify the cause without guessing.
