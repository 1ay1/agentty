---
title: Clipboard & Images
description: Paste text, screenshots and copied files into agentty, locally and over ssh, with agentty paste.
nav_section: User Manual
nav_order: 25
slug: clipboard
---

Paste with [[Ctrl+V]] (or [[Alt+V]] where your terminal takes Ctrl+V). Text,
screenshots and copied files all work. An image attaches as a real image. A
small text file goes in as its text. Any other file is saved and its path goes
in the box.

The clipboard reader is [richpaste](https://github.com/agentty-org/richpaste),
built into agentty as `agentty paste`. It talks to the display server itself,
so there is nothing to install: no `wl-paste`, `xclip` or `pngpaste`.

## Locally

Nothing to set up. agentty reads Wayland (data-control), X11, the macOS
pasteboard or the Windows clipboard directly. Inside tmux it finds the display
by its socket, even when the pane lost `WAYLAND_DISPLAY`.

GNOME has no data-control protocol, so there it reads through XWayland.

## Over ssh: one-time setup

Your clipboard lives on your laptop, not on the server. Run this **once on the
laptop** (the machine you type on):

```bash
agentty paste setup
```

It adds a small block to `~/.ssh/config` and starts a clipboard agent. From
then on every ssh session forwards a socket to that agent, and [[Ctrl+V]] on the
server reads your laptop's clipboard: images, files, text. It works through
tmux and through several ssh hops. Reconnect any session that was already open.

The server needs agentty too, which it has if you run agentty there. Nothing
else to configure.

:::note
The hook skips github.com, gitlab.com, bitbucket.org and ssh.dev.azure.com, so
git over ssh is never touched.
:::

## When the laptop can't run the agent

Without the agent, agentty asks the terminal itself:

| Terminal | Over ssh you get |
|----------|------------------|
| kitty | images and text (OSC 5522) |
| iTerm2, WezTerm, Ghostty, foot, xterm | text (OSC 52) |

For kitty, allow clipboard reads in `kitty.conf` **on the laptop**, and restart
kitty:

```conf
clipboard_control write-clipboard write-primary read-clipboard read-primary
clipboard_max_size 0
```

Inside tmux, also `tmux set -g allow-passthrough on`.

### Images as text

If nothing above fits (say Terminal.app on a Mac you can't install on), turn
the image into one line of text on the laptop, and paste that. agentty turns
it back into the image.

```bash
agentty paste howto   # macOS and Windows one-liners, no install needed
agentty paste page    # a web page that does the same in any browser
```

A `richpaste:1:` line survives wrapping, CRLF changes, tmux and ssh.

## airgap

`agentty airgap user@host --clipboard-relay` forwards the clipboard agent for
that one session. If you already ran `agentty paste setup`, you don't need the
flag.

## Commands

| Command | What it does |
|---------|--------------|
| `agentty paste` | print the clipboard: text, or saved paths for images and files |
| `agentty paste -o DIR` | save into DIR |
| `agentty paste doctor` | show which route a paste would take, and why |
| `agentty paste setup` | ssh hook + agent, once on the laptop |
| `agentty paste agent` | run the agent in the foreground |
| `agentty paste howto` / `page` | image-as-text helpers |
| `agentty paste demo` | a tiny input box to try paste in |

Pasted files are kept in `~/.agentty/paste/pasted`.

## Troubleshooting

Start with `agentty paste doctor` in the same pane. It lists the display, any
forwarded agents, and what a paste returns.

| Symptom | Fix |
|---------|-----|
| "can't reach your laptop's clipboard" | run `agentty paste setup` on the laptop, then reconnect |
| an image comes in as text | the terminal sent OSC 52 only; use the agent, or the image-as-text helpers |
| "your terminal refused the image read" | kitty: `clipboard_control … read-clipboard` on the laptop, then restart kitty |
| works outside tmux, not inside | `set -g allow-passthrough on` |
| mosh | mosh drops clipboard replies; the agent route still works over plain ssh |

Still stuck? Run with `AGENTTY_LOG=general=debug` and look for `clipboard.read`
in the log: it names the route tried and why it gave up.
