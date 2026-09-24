# shell detours

how agentty gets the model to use `read`, `grep`, `list_dir`, `glob` and the
`git_*` tools instead of `cat`, `sed -n`, `grep -rn`, `ls` and `git log` in
the shell. and why it does it with advice, not by rewriting shell calls.

code: `mcp-cpp/src/tools/util/bash_validate.cpp` (`analyze_detour`,
`bash_tool_suggestion`), `mcp-cpp/src/tools/util/shellx.cpp` (parser),
`mcp-cpp/src/tools/shell.cpp` (where the tip is added),
`src/provider/prompt.cpp` (the `<shell>` section),
`src/runtime/app/cmd_factory.cpp` (the `shell=` log field).

## why it matters

a native call gets a real card in the tui: a Read card with the file lines,
a Grep card with hits grouped by function, a git card. a shell call gets a
terminal card with raw text. native calls are also faster, skip build and
vendor trees, and page their own output.

## the problem, measured

from 11,499 unique shell calls in `~/.agentty/threads` (sep 2026):

- 55% end in `| head`, 20% in `| tail`, 13% have `2>/dev/null`.
- 19% are `cd X && <inspect>`.
- 28% glue steps together with `echo "=== x ==="`.
- 24% of grep calls use BRE `\|`. ripgrep, which backs the native grep,
  reads `\|` as a literal pipe and finds nothing.
- shell begets shell. over 9,437 inspection steps: after a shell
  inspection the next one is shell 81% of the time; after a native one,
  22%.
- per session, inspection through the shell grows as the session gets
  longer. it is drift under load, not a model that can't use the tools.

claude code has the same problem. its prompt says "do NOT use bash to
run commands when a relevant dedicated tool is provided", and there are
open issues where the model uses `cat`/`grep` anyway (anthropics/claude-code
#21696, #39979).

## the design (sep 16, hardened sep 24)

four parts. each one does a different job.

### 1. the prompt names the parameter

"prefer the dedicated tools" is advice the model already agrees with and
then ignores. it pipes to `head` because it doesn't believe the tool can
bound itself. so the `<shell>` section names the exact replacement for each
idiom:

```
cmd | head -20        → limit: 20 on grep/read (list_dir and glob are already bounded)
make | tail -20       → shell's own tail_lines: 20
tail -n 50 f          → read with offset: -50
sed -n '10,40p'       → read with start_line/end_line, or symbol: "name"
cmd 2>/dev/null       → nothing: native tools report "no matches"
cd X && grep …        → grep with path: "X"
a; echo ===; b        → two native calls in one turn, they run in parallel
grep -A 8 --include=… → context: "8", glob: "*.cpp"; pattern is a|b, not a\|b
grep … | grep -v PAT  → grep with exclude: "PAT"
```

every line is true for the current tools. that was not always so: the sep
16 line said "every search/read tool bounds its own output" but grep had no
`limit`. grep now takes `limit` (default 20, max 200), and its context cap
went from 10 to 60 lines because a third of real `-A/-B/-C` values were
over 10.

### 2. the detector reads the parse, not the bytes

`analyze_detour(cmd)` returns a `Detour`: an `Intent` (ReadFile, Search,
FindFiles, ListDir, CountOnly, GitRead, Write, Other), the native tool, the
exact parameter, any `| head -N` bound, and the per-step list for chained
calls.

it runs on the tree-sitter bash parse from shellx. so quotes, `$()`,
heredocs, redirects and nested commands come from the parser, not a byte
scanner. `grep -rn '>' src` is a search. `echo "grep x"` runs echo.
`cat <<EOF > f` is a write.

the steps:

1. writes first, over every command in the script, nested ones too:
   `> f`, `>>`, `&>`, `>|`, `tee`, `sponge`, `dd`, `truncate`,
   `sed -i`/`-i.bak`/`-ni`/`--in-place`, `perl -pi`, `sort -o`,
   `awk -i inplace`, and a write inside `$()`, `if`, `bash -c`,
   `find -exec`. any write means no advice at all.
2. a parse that isn't clean gets no advice.
3. anything nested (`$()`, loops, subshells, `&`) is shell work.
4. the script is split into top-level pipelines. `cd`, `pwd`, `true` and
   plain `echo`/`printf` of literal text are scaffolding and carry no
   intent. `cd` targets are tracked so each step can say where it runs.
5. every other pipeline is judged on its own. `| head -N`, `| tail -N` and
   `| wc -l` fold into the verdict. any other stage (`sort`, `awk`, a
   second `grep`) is a composition the native tools can't express: silent.
6. the call is a detour only if every real pipeline is inspection a native
   tool answers. one build or loop anywhere and it is silent. this is the
   same all-or-nothing rule codex uses in `parse_command`.

`substitutable()` is the one read/write gate. it is false for Write, Other
and anything that needs the shell. any code that ever wants to act on a
verdict must go through it.

it stays silent when the native tool can't do the same thing: sed programs
other than one range print, `sed -n 5p a b`, `ls -t`/`-S`,
`find -newer/-mtime/-size`, `head -c`, `tail -f`/`-F`, reading stdin,
`$VAR` and `~` in arguments, `git log -p/--stat/--format/--grep/-S`,
`git show --stat`, `git diff --word-diff`, and every git command that
writes.

### 3. the tip names the exact parameter

the shell always runs. after it finishes, `bash_tool_suggestion` puts one
line in front of the output. it names the tool and the parameter read off
the model's own command:

```
sed -n '147,162p' a.cpp         → `read` with `start_line: 147, end_line: 162`
head -50 log                    → `read` with `limit: 50`
tail -n 30 log                  → `read` with `offset: -30`
grep -rn -A 8 --include=*.cpp x → `grep` with `case_sensitive: true, context: "8", glob: "*.cpp"`
grep -rl x src                  → `grep` with `output: "files"`
grep -rn x src | wc -l          → `grep` with `output: "count"`
git log --oneline -5            → `git_log` with `count: 5, oneline: true`
git show HEAD:a.cpp             → `git_show` with `ref: "HEAD", path: "a.cpp", format: "file"`
```

a chained call lists each call it should have been, up to four:

```
tip: these are 2 native calls: `git_log count: 3, oneline: true (in maya)`;
`git_status` — make them in one turn and they run in parallel, each with
its own card.
```

a grep pattern with `\|` gets a warning that the native pattern is `a|b`.

it never names a parameter a tool doesn't have. `ls | head -5` says
list_dir is already bounded; it does not say `limit`.

### 4. head_lines / tail_lines on the shell tool

for real shell work (a build, a test run) there is no native tool. but
`make 2>&1 | tail -20` still filters at the wrong layer: the pipe drops
lines before the terminal card sees them, so the user loses output they
were watching to save the model's context. the shell tool takes
`head_lines`/`tail_lines`, which bound only what goes back to the model.
the detector keeps the bound even when the call is shell work, so
`cd build && ctest | tail -5` gets a `tail_lines: 5` tip. (from zed.)

## why not translate

on sep 24 we tried running native tools in place of the shell call. it was
reverted. inspection through the shell went from about 25% to 60-68%.

- a tip that is wrong costs nothing. code that acts on a wrong verdict can
  turn a write into a read and report success.
- the semantics don't carry: `sed` ranges, BRE vs ripgrep regex,
  `cat a b` concatenates, `ls` sorts by locale.
- worst of all, it made shell free. the model got native-quality answers
  through the shell and a note saying it worked. it had no reason to
  switch, and shell begets shell.

zed has a real bash parser (brush_parser) and uses it only for security.
codex parses commands only to label them. neither rewrites them.

## results

- detours caught on the 11.5k real calls: 233 with the sep 16 scanner,
  2,879 now (25.0%). another 3,172 get the `head_lines`/`tail_lines` tip.
- safety sweep: 0 tipped calls have a write, exec or network op outside
  quotes. (a naive regex flags 110; all are words like `rm` or `>` inside
  quoted grep patterns or echo text.)
- fuzz: 60k mutated inputs (byte flips, splices, quote/paren injection,
  300-deep parens, 60-deep `bash -c`) under ASan+UBSan: no crash, no
  write ever tipped, no tipped call with a write redirect.
- speed, -O2, per call, parse + verdict + tip: p50 40us, p90 128us,
  p99 217us, max 465us. a shell spawn is about 1-20ms. the tree-sitter
  parse is the whole cost (~0.2us/byte, linear); our own analysis is under
  1% of it. commands over 16 KiB get no advice (the longest real tipped
  call was 7 KB), which caps the worst case.
- tests: `mcp-cpp/tests/bash_validate_test.cpp` (writes in every shape,
  silent cases, exact params, git, chained steps, exclude),
  `search_tools_test.cpp` (grep limit, wide context, exclude),
  `tests/shell_detour_streak_test.cpp` (the drift reminder).

## coverage accounting

every silent verdict carries a typed `Silence` reason, so coverage is a
table, not a guess. on the real calls:

| reason | share | what it is |
|---|---|---|
| work | 36.0% | a program no native tool replaces: cmake, python, rm, tests |
| nested | 14.6% | `$()`, loops, `if`, subshells |
| filter | 12.0% | inspection piped into sort/awk/cut/`grep -i` |
| sed-program | 1.0% | sed that isn't one range print |
| expansion | 0.6% | `$VAR`, `~` in an inspection |
| git-shape | 0.6% | `git log --format/--stat/-p` and similar |
| other | <1% | unparsed, flags, stdin, redirects, scaffold-only |

work and nested are real shell work and should stay silent. filter is the
only big recoverable bucket; its biggest shape, `| grep -v PAT`, is now
folded into grep's `exclude`.

## the drift reminder

the tip is easy to skim past once the habit sets in. so when detours
chain, the reducer (`update/tool.cpp`) puts a plain line on top of the tip
from the third detour in a row:

```
[reminder] shell detour #3 in a row. the tip below names the exact native
call; make that call next. native calls run in parallel and each gets its
own card.
```

detour-ness comes from `substitutable()`, the same gate as the tip, so a
build or a write never counts. any other call resets the streak. this is
the event-driven reminder idea from opendev (arxiv 2603.05344, §2.3.4):
guidance at the moment of decision, not only up front.

## measuring it

every shell call logs its verdict in `~/.agentty/logs/agentty.log`:

```
tool.exec: name=shell … shell=read+detour …
```

`shell=` is one of read, search, find, list, count, git, write, other,
with `+detour` when a native tool does it better. the share of `+detour`
among inspection calls, per session, is the number to watch. baseline:
about 25% on sep 19-22, 60-68% on sep 23-24 with translation on.

to re-run the corpus check, dump shell commands from the threads to
`/tmp/shell_corpus.txt` (one JSON string per line) and run `analyze_detour`
over it. see the eval sketch in the sep 24 session.

## next

1. **measure live.** a few long sessions on the new binary, then the
   `+detour` share per session against the baselines above, and how often
   the reminder fires and whether the next call is native.
2. **refuse for the clearest cases.** only if 1 shows tips plus the
   reminder don't break the chain. refuse when `substitutable()` is true
   and the tip names an exact parameter, return the call to make, run
   nothing in its place. one round trip, breaks the chain every time.
3. **more of the filter bucket.** after `grep -v`, the next shapes are
   `| grep -E PAT` (a second positive filter), `| cut -c`, `| sort -u`.
   each needs a native param that is exactly equivalent, or it stays
   silent.
4. **windows.** the shell there is cmd.exe. the bash parse mostly fails
   cleanly and gives no advice, which is safe. not checked.
