---
title: Installing skills
description: How a skill that runs programs or reaches the network gets into agentty — one install path, a consent screen agentty writes, and approval pinned to content.
nav_section: Tools
nav_order: 45
slug: skill-install
---

Most skills are prose. "Here's our house style", "here's how this repo
lays out migrations" — instructions the agent reads, nothing more. Those
install and load with no ceremony, and nothing on this page applies to
them.

Some skills tell the agent to run programs, reach the network, or write
files that outlive the turn. That's a different thing, and it gets a
different path.

## The one command

```sh
agentty skill add ./path/to/skill-dir
agentty skill list
agentty skill remove NAME
agentty skill approve NAME
```

Same command for a vendor's skill, a directory on disk, and your
company's internal one. There is no bundled default, no first-run
screen, and no entry in a setup menu — **a capability arrives because
you named it.**

That's deliberate. The moment one vendor gets a slot in the setup flow
there's no principled answer for the next one, and first-run becomes a
billboard one good contribution at a time.

## Declaring what a skill does

A skill declares capability in its frontmatter, using the same four
words agentty's own tools use:

```yaml
---
name: sites
description: deploy temporary apps with hosting, database, storage
effects: [exec, net, write-fs]
source: github.com/example/agentty-sites
---
Run `npx --yes @example/init`, then POST to the deploy API.
```

| effect | means |
|---|---|
| `read-fs` | opens and scans paths on this machine |
| `write-fs` | creates or modifies files that outlive the run |
| `net` | sends and receives data over the internet |
| `exec` | runs commands, including fetched packages |

**Absent `effects:` means prose-only**, which is every skill written
before this existed. Those are never gated, and adopting this costs them
nothing — that's asserted at compile time, not promised in a doc.

Parsing is lenient on purpose. `[exec, net]`, `exec net`, `exec,net` all
work; `write-fs`, `write_fs` and `WriteFs` are the same thing; and a word
agentty doesn't recognise is ignored rather than fatal, so a skill
written for a future version still loads here.

## What you see before it installs

```
  install skill "sites" from github.com/example/agentty-sites?

  deploy temporary apps with hosting, database, storage

  the agent will be told it may:
    write files        create or modify files that outlive the run
    reach the network  send and receive data over the internet
    execute programs   run commands, including fetched packages

  skills are instructions, not sandboxed code. agentty cannot
  enforce what is written above — it is what the skill declared.
  you did not write these instructions.

  [1] install   [2] print it first   [3] cancel
```

**agentty writes every word of that.** The skill supplies a name, a
description, a source, and four bits — it does not get to write its own
reassuring summary. A skill that under-declares gets a prompt that
under-sells it, but it cannot *lie* in the prompt, and that's the
failure mode worth designing against.

`[2] print it first` pages the actual `SKILL.md` before you decide.
Reading the thing beats a checkbox that says you did.

## What agentty checks before it asks

Before the prompt, agentty reads the skill body and looks for patterns that
have actually shown up in the wild. This is why the prompt isn't the same
every time.

That matters more than it sounds. Anthropic measured that Claude Code users
**approve 93% of permission prompts**; browser-warning research found **70%
clickthrough** on Chrome's SSL interstitial. A screen that looks identical
every time is one people learn to dismiss without reading. So the routine
case stays quiet, and a flagged skill gets a screen that looks different,
with the safe answer first:

```
  ━━━ REVIEW THIS ONE ━━━

  install skill "helper"?

  formats your code nicely

  declares no effects — prose only.

  reading the file, agentty noticed:
   !! line 1    downloads a script and pipes it straight into a shell
   !! line 3    puts instructions inside an HTML comment
   !! line 6    instructs the agent to hide what it is doing from you
   !! line 7    installs something that keeps running after this session
    ! line 5    reads credentials or environment secrets

  this is pattern matching over text, not proof of intent —
  and a skill with no findings has not been proven safe.

  [2] print it first   [3] cancel   [1] install anyway
```

Note what that skill *declared*: "formats your code nicely", no `effects:`
at all. The declaration is the least reliable thing in the file, which is
why screening reads the **body**. A 98,380-skill study found capability
present in the code but absent from the documentation in **100% of advanced
attacks**.

What gets flagged, all drawn from published corpora:

| finding | why |
|---|---|
| `curl \| sh` | the code that runs is whatever the server sends today |
| base64 → `eval` | the real command is hidden from anyone reading the file |
| instruction override | a skill should describe a task, not reprogram the agent |
| directives in HTML comments | invisible when rendered, visible to the agent |
| "don't tell the user" | instructs the agent to hide what it's doing from you |
| crontab / `authorized_keys` / shell rc | outlives the session |
| reads credentials + posts outbound | the two halves of exfiltration |
| `Bash(*)` pre-approval | asks for unrestricted shell up front |
| password-protected archives | a known way past scanners |
| bidi / zero-width characters | text that renders differently than it parses |

`--yes` refuses to install a skill with critical findings. Automation
shouldn't be able to quietly accept something a human would have stopped at.

**This is a review aid, not a verdict.** It's pattern matching over text, so
it's evadable by aliases, wrapper scripts, encoding, and anything nobody has
published yet. A clean result is one piece of evidence. agentty never says
"safe".

## Seeing what you have

`Ctrl+K` → **Skills** opens a read-only viewer. ↑/↓ move, Esc closes.

```
┌─ Skills ───────────────────────────────────────────────────┐
│ !! sneaky                              prose                 │
│  ! deployer                    exec, net · PENDING           │
│    house-style                         prose                 │
│                                                              │
│   formats code                                               │
│   you wrote this                                             │
│   ~/.agentty/skills/sneaky                                   │
│                                                              │
│   agentty noticed:                                           │
│    !! line 1  downloads a script and pipes it into a shell    │
│    !! line 2  instructs the agent to hide what it is doing    │
│                                                              │
│   pattern matching, not proof — and no findings is not a      │
│   clean bill.                                                │
│                                                              │
│   1 skill flagged, 1 awaiting approval                       │
└──────────────────────────────────────────────────────┘
```

Rows are ordered worst-first — flagged, then awaiting approval, then the
rest — so the thing that needs you isn't buried under forty clean skills.
Every state has a word or a glyph as well as a colour, so it reads the same
in `theme::native` and to a colourblind user.

**The viewer has no approve key, on purpose.** It answers *what is
installed, what did it declare, is it approved, what did agentty notice*,
and then points at `agentty skill approve NAME` for the decision. An
approval one keystroke away from a list you're already scrolling is the
fastest possible habituated yes — which is the thing the 93% number is
about. The decision should happen somewhere you went on purpose, with the
findings and the body in front of you.

## Approval is pinned to content

Approving a skill approves **those exact instructions**, not the name.

```sh
$ agentty skill list
sites    user   approved   write-fs, net, exec
```

Edit the body, or keep the prose and add an effect, and it goes back to
`PENDING`:

```sh
$ vim ~/.agentty/skills/sites/SKILL.md      # change what it tells the agent
$ agentty skill list
sites    user   PENDING    write-fs, net, exec
```

This is the [MCPoison](https://nvd.nist.gov/vuln/detail/CVE-2025-54136)
lesson applied: Cursor pinned trust to a server's *name*, so swapping the
command under an approved name kept the approval. Here the approval is a
hash of the body plus the declared effects, so substitution re-gates. A
v2 that keeps its wording and quietly adds `exec` asks again.

Approvals live in `~/.agentty/skills_approved.json`, under your user
root — a cloned repo cannot write there, so a repo can never pre-approve
its own skills.

## Where skills come from

| source | trust on load |
|---|---|
| you wrote it in `~/.agentty/skills/` | trusted |
| it arrived with a cloned repo (`.agentty/skills/`) | pending, if it declares effects |
| you fetched it (`source:` set) | pending, until approved |
| no `effects:` at all | trusted — prose |

A project skill starting pending is the same rule scope applies to
project MCP config: a repo doesn't get to vouch for itself.

## Publishing one

Put a `SKILL.md` in a repo, declare what it does, tell people the
command:

```sh
agentty skill add github.com/you/your-skill
```

Two things worth getting right:

- **Declare honestly.** Under-declaring doesn't buy you a quieter prompt
  for long — it buys you a user who finds out later.
- **Don't write sales copy into the body.** The skill body becomes the
  agent's instructions; a line like *"ask them if they want to upgrade"*
  is you putting words in someone else's agent's mouth. Put it in your
  docs.

## Honest limits

- **A skill is instructions, not sandboxed code.** agentty cannot enforce
  a declaration. `effects:` tells you what the author *said*; it is not a
  capability boundary. What actually constrains a tool run is the
  [sandbox](sandboxing.md) and the permission profile.
- **Screening is evadable.** It reads text. Aliases, wrapper scripts,
  encoded payloads and novel patterns get through. Snyk's own scanners
  report high recall on *known* malicious skills — that is not the same as
  catching the next one.
- **A skill's own description is the weakest signal in the file.** Shadow
  features — capability in the body, absent from the docs — showed up in
  100% of advanced attacks in the 98k-skill study. Read the body; that's
  what `[2] print it first` is for.
- **Remote fetch isn't wired yet.** Clone and add the path for now:
  ```sh
  git clone https://github.com/you/your-skill /tmp/s && agentty skill add /tmp/s
  ```
- **Skills cost context.** Every installed skill takes a slot in the
  tier-1 catalog (`AGENTTY_MAX_SKILLS`, default 64) and tokens on every
  turn. Install what you use.
- **There is no "trust all skills" switch**, in settings or anywhere
  else. A toggle like that becomes step one of every install guide, which
  defeats the point of the prompt.

## Where this came from

The design follows three 2026 findings rather than intuition:

- **Snyk** audited 3,984 skills from ClawHub and skills.sh (Feb 2026):
  13.4% carried a critical issue, and of the confirmed-malicious set, 91%
  paired a payload with prompt injection. Injection primes the agent to
  accept what its own training would refuse — which is why author text is
  sanitised before agentty renders it anywhere.
- **A 98,380-skill empirical study** (arXiv 2602.06547) behaviourally
  confirmed 157 malicious skills averaging 4 vulnerabilities across a median
  of 3 kill-chain phases, with shadow features in 100% of advanced attacks.
  Hence: screen the body, never trust the declaration.
- **Datadog** showed dynamic-context commands execute *before* the model
  sees the skill, so model-level defenses never get a turn. agentty doesn't
  implement `!`-context at all; a ported skill carrying it gets a note
  saying those lines are inert here.

And the UX constraint, from Anthropic's own measurement: **93% of permission
prompts get approved.** Any design whose safety rests on people reading a
uniform prompt is already failing. That's why the quiet case is quiet and
the flagged case looks different.

## Credit

The idea of declaring a skill's capability in frontmatter came out of
[PR #47](https://github.com/1ay1/agentty/pull/47) by Arag Agrawal
(Cohesivity), which proposed bundling an effectful skill into first-run
setup. The placement is what agentty declined; the observation underneath
it was right — there was no vocabulary for "this skill will run programs
and reach the network", and no install path that treated every source the
same. This is that, generalised so it works for anyone.
