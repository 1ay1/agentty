---
title: Clipboard & Images
description: Paste text, screenshots and copied files into agentty with Ctrl+V, locally and over ssh. Every route, every setting, and what to do when one fails.
nav_section: User Manual
nav_order: 25
slug: clipboard
---

Press [[Ctrl+V]] in the composer (or [[Alt+V]], for terminals that keep Ctrl+V
for their own paste). What lands depends on what you copied:

| You copied | What goes into the message |
|------------|----------------------------|
| A screenshot or image | an image chip; the model sees the image |
| A small text file (`report.md`, `main.cpp`) | its text, headed `// file: <name>` |
| Any other file (a PDF, a zip, a big log) | saved to disk, and its path is typed into the box |
| Text | the text, as a paste chip that expands into the message on send |

Images larger than 8000 px on a side are attached but flagged: the provider
would refuse them, so resize first.

The reader is [richpaste](https://github.com/agentty-org/richpaste), built into
agentty. It speaks to the display server itself, so nothing is installed: no
`wl-paste`, `xclip` or `pngpaste`. `agentty paste …` is the same reader on the
command line.

## How a paste finds your clipboard

Ctrl+V only needs the terminal for the keypress. The clipboard is fetched by
the best route available, in this order:

| Where you are | Tried in order |
|---------------|----------------|
| On your own machine | 1. this machine's clipboard · 2. the terminal |
| Over ssh (or tmux inside ssh) | 1. your laptop, through the ssh forward · 2. the terminal · 3. the server's clipboard, only if you turn it on |

Each route can be turned off, see [Settings](#settings). Over ssh the
server's own clipboard is skipped by default: it isn't the one you copied
into, and pasting it gives stale text without saying so.

agentty decides "over ssh" by asking tmux for the pane's `SSH_CONNECTION` when
you are in tmux (a pane keeps stale variables), else by `SSH_CONNECTION` /
`SSH_TTY`.

### 1. This machine

Nothing to set up.

| Platform | How it reads |
|----------|--------------|
| Linux, Wayland | the data-control protocol, over the compositor socket |
| Linux, X11 | the X11 protocol over the socket, using your Xauthority cookie; large data via INCR |
| GNOME (no data-control) | through XWayland, which covers most apps |
| macOS | the pasteboard, through JavaScript for Automation (`osascript`, built in) |
| Windows | the Win32 clipboard: copied files, PNG, bitmaps (converted to PNG), Unicode text |

Inside tmux it finds the display by its socket on disk, so a pane that lost
`WAYLAND_DISPLAY` or `DISPLAY` still works.

When several formats are on the clipboard it picks copied files first, then an
image (PNG before JPEG, then WebP, GIF, AVIF, BMP, TIFF), then text.

### 2. Your laptop, over ssh

Run this **once, on the machine you type on**:

```bash
agentty paste setup
```

It does two things:

1. Adds this block to the **top** of `~/.ssh/config` (the top, so it can't end
   up inside one of your `Host` blocks):

   ```sshconfig
   # richpaste: forward your clipboard to every ssh session
   Match !host github.com,gitlab.com,bitbucket.org,ssh.dev.azure.com exec "\"/path/to/agentty\" paste ssh-hook"
     Include "~/.agentty/paste/ssh/forward.conf"
   # richpaste end
   ```

2. Starts the clipboard agent (`agentty paste agent`) in the background. It
   listens on `~/.agentty/paste/agent.sock` (Windows: loopback TCP, see below).

From then on, each `ssh` you start runs the hook. The hook restarts the agent
if it died and writes a fresh `RemoteForward` line, so the session carries a
socket from the server back to your laptop:

```
laptop                                      server
agentty paste agent  <── ssh forward ──  /tmp/richpaste-<you>-<random>.sock
reads your clipboard                         agentty asks it on Ctrl+V
```

On Ctrl+V the server lists `/tmp/richpaste-<you>-*.sock`, keeps the ones you
own, tries the newest first, and removes dead ones. The bytes travel on the
ssh connection, not through the terminal, so tmux can't interfere, and a tmux
pane that was opened during an older connection finds the new socket by
itself.

- **Reconnect** any session that was open before `setup`: the forward is added
  when ssh connects.
- **The server** only needs agentty, which it has if you run agentty there.
- **Git hosts** (github.com, gitlab.com, bitbucket.org, ssh.dev.azure.com) are
  skipped, so git over ssh never gets a forward.
- **Several hops** work: an agent on a middle machine passes the request on,
  up to 8 machines, which also catches a forwarding loop.
- **Run `setup` again** after moving the agentty binary. It updates the block
  in place, or replaces an older `richpaste` block.
- **Windows laptops** work with the built-in OpenSSH (`ssh.exe`). There the
  agent listens on `127.0.0.1:47233` (or any free port) and only answers your
  own Windows user, or a client holding the random token in
  `%USERPROFILE%\.agentty\paste\agent.tcp`.

To undo it, delete the block from `~/.ssh/config`.

:::warn
While you are connected, any process running as you (or root) on that server
can ask for your clipboard, the same trust as ssh-agent forwarding. Only
forward to servers you would forward your ssh-agent to. To limit it, edit the
`Match` line (for example `Match host devbox,buildbox exec …`), or turn the
route off with `paste.forwards`.
:::

### 3. The terminal

When nothing above answered, agentty asks the terminal itself. The terminal
runs on your laptop, so this works over ssh with nothing installed:

| Terminal | What it can send |
|----------|------------------|
| kitty | images, files and text (OSC 5522) |
| iTerm2, WezTerm, Ghostty, foot, xterm | text (OSC 52), if clipboard reads are allowed |
| Terminal.app, Windows Terminal, VS Code | nothing; use the agent, or [images as text](#4-images-as-text) |

agentty first asks whether the terminal supports OSC 5522, and uses OSC 52
otherwise. Each query is followed by a DA1 request that every terminal
answers, so a terminal that ignores the clipboard query costs one round trip,
not a timeout. If no answer comes, agentty tells you why (mosh, tmux settings,
a terminal that refused) in a toast.

**kitty** asks before a program may read the clipboard. Allow it in
`kitty.conf` on the laptop, then restart kitty fully:

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
```

**Inside tmux**, the request has to pass through to the outer terminal:

```bash
tmux set -g allow-passthrough on
```

For text, agentty also has tmux fetch the outer terminal's clipboard itself
(`tmux refresh-client -l`), which works with tmux's default options.

If a terminal reports the wrong thing, force a mode with
[`paste.terminal`](#settings): `osc5522`, `osc52` or `off`.

### 4. Images as text

Some terminals never hand the clipboard to programs (Terminal.app over ssh,
Windows Terminal), and sometimes you can't run the agent. Then turn the image
into one line of text **on the laptop** and paste that like any text; agentty
turns it back into the image or file.

Three ways to make the line, none needing an install:

- **macOS hotkey.** `agentty paste howto` prints a Shortcuts "Run Shell
  Script" to bind to [[Ctrl+Cmd+V]]. It encodes the clipboard, presses Cmd+V
  for you, and puts your clipboard back. Built-in tools only (`osascript`,
  `base64`, `shasum`, `pbcopy`).
- **Windows.** A PowerShell one-liner, also from `agentty paste howto`. Run
  it after copying, then paste with [[Ctrl+Shift+V]].
- **Any browser.** Open **[agentty.org/paste](https://agentty.org/paste)**,
  paste or drop the image or file there, and it copies the line for you. It
  runs entirely in the page; nothing is uploaded. Offline copy:
  `agentty paste page > paste.html`.

The line looks like this:

```
richpaste:1:<mime>:<name, base64>:<size>:<sha256>:<data, base64>
```

It is one line with no newline, so pasting it into a shell by mistake runs
nothing. The decoder ignores whitespace inside the data and stops after
`<size>` bytes, so a terminal that wraps the line or changes line endings
doesn't break it. A line cut short or changed fails the sha256 check and is
refused (with a toast saying why), never saved broken. Lines arriving by any
route are decoded, including a clipboard over ssh that holds one. Turn this off
with `paste.decode_text`.

## Pasted files

A copied text file up to 256 KB that is valid UTF-8 goes into the message as
its text, under a `// file: <name>` line. Anything else (binary files, bigger
files) is saved to `~/.agentty/paste/pasted` and its path is typed into the
box, where you and the model can see it.

Saved files are named by content: `<sha256 prefix>-<name>` for files,
`<sha256 prefix>.png` (etc.) for images. Pasting the same thing twice reuses
the same file. They are never deleted automatically; clear the folder
whenever you like.

`paste.inline_kb` sets the size limit (0 always saves), and `paste.save_dir`
the folder.

## Settings

Settings → **Paste** (`/settings`, then *Paste*) holds every switch. The same
rows are in `settings.json` under `"paste"`, `agentty paste config` edits them
from a shell, and each takes an `AGENTTY_PASTE_*` variable that wins for one
run. A change applies to the next paste.

| Key | Default | Meaning |
|-----|---------|---------|
| `forwards` | on | Over ssh, read your laptop's clipboard through the forwarded agent. |
| `display` | on | Read this machine's clipboard. |
| `terminal` | `auto` | Ask the terminal: `auto` (OSC 5522 if it has it, else OSC 52), `osc5522`, `osc52`, or `off`. |
| `server_display` | off | Over ssh, fall back to the server's own clipboard. |
| `decode_text` | on | Turn `richpaste:1:` lines back into the image or file. |
| `max_mb` | 32 | Largest paste read (1–1024 MB). |
| `inline_kb` | 256 | A text file up to this size pastes as its text (0–4096; 0 always saves). |
| `save_dir` | (empty) | Where pasted files go; empty means `~/.agentty/paste/pasted`. |
| `socket` | (empty) | Ask only this agent socket (a relay, a test). |

From a shell:

```bash
agentty paste config                    # every key, its value, and where it comes from
agentty paste config set max_mb 64
agentty paste config set terminal off
agentty paste config get inline_kb
agentty paste config reset all
AGENTTY_PASTE_DISPLAY=0 agentty         # one run, not saved
```

The variable names are in [Configuration → Paste](/docs/configuration#paste).
A row set by a variable shows locked in the pane and names it.

## airgap

[`agentty airgap`](/docs/airgap) runs agentty on a remote host over ssh. If you
ran `agentty paste setup` on the laptop, its sessions carry the clipboard like
any other ssh. Otherwise add `--clipboard-relay` for that session: it starts
the agent here and adds one forward, with nothing installed on the remote and
no sshd needed on the laptop.

## Copying out

Copying from agentty (a code block, an auth URL) goes the other way. agentty
writes it with the platform tool (`pbcopy`; `wl-copy`, `xclip` or `xsel` on
Linux) or the Win32 clipboard on Windows, and also sends OSC 52 so the copy
reaches your laptop's clipboard through ssh and tmux.

## Commands

| Command | What it does |
|---------|--------------|
| `agentty paste [-o DIR]` | Print text, or save images and files and print their paths. |
| `agentty paste doctor` | Settings in effect, the agent, display, ssh and forwards, and what a paste returns now. |
| `agentty paste setup` | The ssh hook and the agent. Once, on the machine you type on. |
| `agentty paste agent` | Run the agent in the foreground. |
| `agentty paste config [get\|set\|reset]` | The paste settings. |
| `agentty paste howto` | The macOS and Windows images-as-text snippets. |
| `agentty paste page` | The images-as-text web page, as a file. |
| `agentty paste encode FILE` | `FILE` as one `richpaste:1:` line. |
| `agentty paste decode [DIR]` | Turn such lines on stdin back into files. |

## Troubleshooting

Start with `agentty paste doctor`, **in the same pane** where Ctrl+V failed:

```
settings       forwards=on display=on server_display=off terminal=auto max=32.0 MB
state dir      /home/you/.agentty/paste
saves to       /home/you/.agentty/paste/pasted
agent          /home/you/.agentty/paste/agent.sock  (not running)
display env    WAYLAND_DISPLAY=- DISPLAY=-
ssh session    SSH_CONNECTION=10.0.0.5 51234 10.0.0.9 22  TMUX=yes
forwards       1 found
  /tmp/richpaste-you-3b14276c8b2ecf09.sock  (live)
paste          ok via ssh richpaste-you-3b14276c8b2ecf09.sock
  image/png 412.3 KB sha256 8f2c41d0a9e7b3c5
```

| Symptom | Fix |
|---------|-----|
| "can't reach the clipboard on the machine you're typing on" | `agentty paste setup` on the laptop, then reconnect. |
| `forwards 0 found` after setup | The session predates setup: reconnect. Or the host is a git host, or your `Match` line excludes it. |
| A forward shows `(dead)` | That ssh connection closed. The next paste removes it; reconnect if none are live. |
| An image comes in as text | The terminal answered with OSC 52, which is text only. Use the agent, kitty, or images as text. |
| "your terminal refused the image read" | kitty: add `read-clipboard` to `clipboard_control` on the laptop, restart kitty. |
| Works outside tmux, not inside | `tmux set -g allow-passthrough on`. |
| mosh | mosh drops clipboard replies and can't forward sockets. Use plain ssh for paste. |
| A route never runs | Check Settings → Paste, and `agentty paste config` for an `env` source. |
| Ctrl+V does nothing at all | The terminal kept the key. Use [[Alt+V]]. |
| A pasted `richpaste:1:` line is refused | It was cut short or changed on the way. Copy it again; if the clipboard is small, paste a smaller file. |

For the full story of one paste, run with the `ui` channel at `info` and look
for the `clipboard.read` and `paste.` lines; they name the route taken and why
others gave up:

```bash
AGENTTY_LOG=ui=info agentty
grep 'clipboard\.\|paste\.' ~/.agentty/logs/agentty.log
```

The agent writes its own log to `~/.agentty/paste/agent.log`.
