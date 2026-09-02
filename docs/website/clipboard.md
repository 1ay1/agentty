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

## The 30-second fix (kitty over SSH)

If you are on kitty and image paste fails over SSH, it is almost certainly
this. Add to **your laptop's** `~/.config/kitty/kitty.conf` — the machine
kitty runs on, *not* the host agentty runs on:

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
clipboard_max_size 0
```

Then **fully restart kitty**. Inside tmux, also:

```bash
tmux set -g allow-passthrough on
```

That is the entire fix. The rest of this page explains why, and what to do on
terminals that are not kitty.

:::warning
Both lines matter. `clipboard_control` without any `read-*` verb means kitty
never answers a read — the symptom is *silence*, not an error. And
`clipboard_max_size` defaults to **64 MB of base64 in some builds but much
lower in others**; a screenshot that exceeds it is dropped just as silently.
`0` means no limit.
:::

:::note
Editing `kitty.conf` on the **remote** host does nothing. This trips almost
everyone, including the author: the config that governs the answer belongs to
the kitty process drawing your window, which is on your laptop.
:::

## Diagnose it in 10 seconds

Every layer here fails the same way — **silence** — so guessing which one is
broken is hopeless. Ask the terminal directly instead. Paste this into a shell
in the same pane you'd use agentty in:

```bash
probe() {
  exec < /dev/tty                  # READ from the terminal
  old=$(stty -g); stty raw -echo min 0 time 0
  printf "$2" > /dev/tty           # WRITE the query to the terminal
  got=0; i=0
  while [ $i -lt 3 ]; do           # poll ~3s; a reply can arrive chunked
    n=$(dd bs=4096 count=1 2>/dev/null | wc -c)
    got=$((got + n)); i=$((i + 1)); sleep 1
  done
  stty "$old"; printf '%-22s %s bytes\n' "$1" "$got"
}
probe "control (DA1)"      '\033[c'
probe "clipboard (OSC 52)" '\033]52;c;?\033\\'
```

Save it and run `sh probe.sh` from a shell prompt in the pane. Both `/dev/tty`
redirections matter: the query has to reach the **terminal** and the reply has
to be read back from it. Without them, redirecting the script's output (or
running it under a wrapper) makes every probe report `0` and the result is
meaningless.

Read the result like this:

| control | clipboard | Meaning |
|---------|-----------|---------|
| **0** | 0 | Replies never reach you at all. Not a clipboard problem — suspect mosh, or a pty that isn't your terminal. |
| **>0** | **0** | The channel is fine; your terminal is **refusing clipboard reads**. This is the kitty `clipboard_control` case above. |
| >0 | >0 | Reads are permitted. If images still fail, the terminal speaks OSC 52 but not OSC 5522 — use the ferry or `@path`. |

A non-zero control reply with a zero clipboard reply is the signature of a
permission denial, and it is by far the most common outcome. No terminal
reports it as an error; it simply never answers.

:::note
The probe uses OSC 52 (text) because every clipboard-capable terminal speaks
it. kitty gates the text read and the **image** read (OSC 5522) separately, and
a long-lived pane can still be attached to a client that started before you
changed the config — so a `0` here while image paste actually works is possible
and harmless. Treat a successful [[Ctrl+V]] as the ground truth; use the probe
when paste is failing and you need to know *which* layer is silent.
:::

:::note
Inside tmux, wrap the clipboard probe in the passthrough envelope to test the
real path agentty uses:

```bash
probe "clipboard via tmux" '\033Ptmux;\033\033]52;c;?\033\033\\\033\\'
```

If the unwrapped probe answers and the wrapped one doesn't, `allow-passthrough`
is off.
:::

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

A denied read is not reported as an error — kitty simply **never answers**. The
request goes out, nothing comes back, and agentty waits until its deadline and
reports that the terminal didn't reply. That silence is why this is so hard to
diagnose from the remote side: it is indistinguishable from a dropped escape
until you probe a control sequence alongside it (see
[Diagnose it in 10 seconds](#diagnose-it-in-10-seconds)).

Add the read verbs to `~/.config/kitty/kitty.conf` (the same path on Linux and
macOS — on macOS kitty also honours the legacy
`~/Library/Preferences/kitty/kitty.conf`, but only if the `~/.config` one is
absent):

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
clipboard_max_size 0
```

Then **fully restart kitty** — a config reload does not re-negotiate this.

:::warning
`clipboard_max_size` is the second half of the fix and is easy to miss. It caps
the payload kitty will transfer; a screenshot is base64-encoded PNG and can run
to several megabytes, so a default cap silently truncates or drops it — again
with no error. `0` disables the limit.
:::

:::danger
Edit this file **on the machine kitty runs on** — your laptop. Editing
`~/.config/kitty/kitty.conf` on the remote host you SSH into has no effect
whatsoever: that host has no kitty process, and the terminal answering the read
is the one drawing your window. Hours have been lost to a correct config on the
wrong machine.
:::

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
| "reading clipboard…", then **"no reply"** after a long wait | Terminal is **silently refusing reads** — the most common cause | kitty: `clipboard_control … read-clipboard` **on your laptop** + restart. Confirm with the [probe](#diagnose-it-in-10-seconds) |
| Message blames tmux, but tmux is configured correctly | Same silent denial — the diagnostic names the nearest suspect, not the culprit | Run the [probe](#diagnose-it-in-10-seconds): control >0 with clipboard 0 means the terminal, not tmux |
| Small images work, large ones don't | `clipboard_max_size` cap | `clipboard_max_size 0` |
| Paste inserts **text** where you expected an image | Terminal answered OSC 52, not 5522 | kitty: allow reads (above). Others: ferry or `@path` |
| Nothing happens at all | Request never reached the terminal | In tmux: `allow-passthrough on` |
| "tmux answers reads from its own paste buffer" | tmux is serving its internal buffer | `tmux set -g get-clipboard both` |
| "no clipboard tool" on Linux | No `wl-paste` / `xclip` | Install one (above) |
| Works outside tmux, not inside | Passthrough disabled | `set -g allow-passthrough on` |
| Ferry set but still text | The command failed | Test it with `\| file -`, then check `AGENTTY_LOG=general=debug` |

:::tip
If the message names a tmux setting you have already verified, **believe the
probe, not the message**. agentty can only observe that no reply arrived; it
cannot see *who* dropped it, so it names the most likely gate in your topology.
A control sequence that answers while a clipboard read stays silent is proof
the terminal is the one refusing.
:::

Still stuck? Run with logging and include the output in an issue:

```bash
AGENTTY_LOG=general=debug AGENTTY_LOG_FILE=/tmp/agentty.log agentty
```

The log records which clipboard path was attempted and why it gave up — which
is usually enough to identify the cause without guessing.
