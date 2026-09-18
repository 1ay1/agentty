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

## Credit

The idea of declaring a skill's capability in frontmatter came out of
[PR #47](https://github.com/1ay1/agentty/pull/47) by Arag Agrawal
(Cohesivity), which proposed bundling an effectful skill into first-run
setup. The placement is what agentty declined; the observation underneath
it was right — there was no vocabulary for "this skill will run programs
and reach the network", and no install path that treated every source the
same. This is that, generalised so it works for anyone.
