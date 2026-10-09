#!/usr/bin/env python3
"""The libraries agentty vendors hold no runtime (docs/PROTOCOL_LIBRARIES.md).

  submodule_purity  R1-R5 over each library's src/ and include/, no allowlist
  no_duplication    R7: mcp-cpp and acp-cpp don't copy jsonrpc-cpp or each other
  jsonrpc_leaf      jsonrpc-cpp includes only std, nlohmann and itself; in
                    agentty only the rpc/mcp/acp integration modules include it
  spawn_via_jaal    agentty starts no process by hand: no fork, exec*,
                    posix_spawn, popen, system or waitpid outside _WIN32
                    branches (jaal has no Windows backend yet)
  no_sleep_poll     agentty waits on events, never sleep_for/usleep, outside
                    _WIN32 branches

Comments and string literals are stripped before matching, so docs and error
messages may name anything.

Usage: protocol_libraries.py ROOT {purity|duplication|jsonrpc_leaf|spawn_via_jaal|no_sleep_poll}
"""
import os
import re
import sys

LIBS = ["mcp-cpp", "acp-cpp", "rag-cpp", "jsonrpc-cpp", "claybin"]
EXTS = (".cpp", ".hpp", ".h", ".c", ".mm", ".cc")

# rule -> regex over stripped code. A leading space is added to every line so
# "not preceded by an identifier" can be written as [^\w.>:].
RULES = {
    "R1 thread":  r"std::j?thread\b(?!::)|\.detach\(\)|std::async\b|\bpthread_create\b|\bCreateThread\b"
                  r"|\b_beginthread(ex)?\b|#\s*pragma\s+omp\b|std::execution::par\b|\bdispatch_async\b",
    "R1 lock":    r"std::(shared_|recursive_|timed_|recursive_timed_)?mutex\b|condition_variable"
                  r"|std::(unique|scoped|shared)_lock\b",
    "R1 atomic":  r"std::atomic|\batomic_flag\b",
    "R1 tls":     r"\bthread_local\b",
    "R1 wait":    r"std::(shared_)?future\b|std::promise\b|std::packaged_task\b|\bstop_source\b"
                  r"|std::latch\b|std::barrier\b|_semaphore\b",
    "R2 hook":    r"\bset_runtime\b|\bset_executor\b|\b(struct|class)\s+(Runtime|Executor)\b",
    "R4 clock":   r"\b(steady|system|high_resolution|utc|file)_clock\s*::\s*now\b|\bclock_gettime\b"
                  r"|\bgettimeofday\b|\bGetTickCount|\bQueryPerformanceCounter\b|[^\w.>:]time\s*\(\s*(nullptr|NULL|0)\s*\)",
    "R5 sleep":   r"\bsleep_for\b|\bsleep_until\b|[^\w.>:]usleep\s*\(|\bnanosleep\b|[^\w.>:]Sleep\s*\(",
    "R5 process": r"[^\w.>:]v?fork\s*\(|::v?fork\s*\(|[^\w.>]exec[lv]p?e?\s*\(|\bposix_spawn|\bCreateProcess"
                  r"|[^\w.>:]_?popen\s*\(|std::system\s*\(",
    "R5 socket":  r"[^\w.>]socket\s*\(\s*(AF_|PF_)|\bWSAStartup\b",
    "R5 env":     r"\b(secure_)?getenv\b|\b_wgetenv\b|[^\w.>]environ\b",
}

# claybin's job is to fork/exec into a jail, handing the guest an environment:
# R5's process rule (and passing environ to exec) is its purpose.
EXEMPT = {("claybin", "R5 process"), ("claybin", "R5 env")}

# A mutable static / namespace-scope variable (R3). Matched per statement on a
# line starting with `static`; const/constexpr ones and functions are fine.
STATIC_RE = re.compile(r"^\s*static\s+(?!(inline\s+)?(const|constexpr|consteval)\b|_assert\b)")


def strip(text):
    """Blank out comments and string/char literals, keeping line numbers."""
    out = []
    i, n = 0, len(text)
    while i < n:
        c = text[i]
        if text.startswith("//", i):
            j = text.find("\n", i)
            i = n if j < 0 else j
        elif text.startswith("/*", i):
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append("\n" * text.count("\n", i, j))
            i = j
        elif c == "R" and i + 1 < n and text[i + 1] == '"' and (i == 0 or not (text[i - 1].isalnum() or text[i - 1] == "_")):
            k = text.find("(", i + 2)
            delim = text[i + 2:k]
            end = text.find(")" + delim + '"', k)
            end = n if end < 0 else end + len(delim) + 2
            out.append('""' + "\n" * text.count("\n", i, end))
            i = end
        elif c in "\"'":
            j = i + 1
            while j < n and text[j] != c and text[j] != "\n":
                j += 2 if text[j] == "\\" else 1
            out.append(c + c)
            i = j + 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def lib_files(root, lib, subs=("src", "include")):
    for sub in subs:
        base = os.path.join(root, "third_party", lib, sub)
        for dp, _, fs in os.walk(base):
            for f in sorted(fs):
                if f.endswith(EXTS):
                    yield os.path.join(dp, f)


def purity(root):
    hits, nfiles = [], 0
    rules = {k: re.compile(v) for k, v in RULES.items()}
    for lib in LIBS:
        if not os.path.isdir(os.path.join(root, "third_party", lib)):
            hits.append(f"{lib}: not checked out")
            continue
        for path in lib_files(root, lib):
            nfiles += 1
            rel = os.path.relpath(path, os.path.join(root, "third_party"))
            code = strip(open(path, encoding="utf-8", errors="replace").read())
            for no, line in enumerate(code.split("\n"), 1):
                if not line.strip():
                    continue
                padded = " " + line
                for name, rx in rules.items():
                    if (lib, name) not in EXEMPT and rx.search(padded):
                        hits.append(f"{rel}:{no}: {name}: {line.strip()}")
                if STATIC_RE.match(line):
                    head = re.split(r"[={]", line, maxsplit=1)[0]
                    if "(" not in head and re.search(r"[=;{]", line):
                        hits.append(f"{rel}:{no}: R3 mutable static: {line.strip()}")
    if nfiles == 0:
        return ["scanned 0 files"]
    print(f"submodule_purity: {nfiles} files in {', '.join(LIBS)}")
    return hits


# Names jsonrpc-cpp owns; mcp-cpp and acp-cpp must use them, not redefine them.
CORE_NAMES = ["Engine", "RpcEngine", "Codec", "RpcError", "Newtype", "RequestId",
              "Router", "LineSplitter", "Message", "Phantom", "StaticString"]
DUP_WINDOW = 12   # identical normalised lines in a row that count as a copy


def normalise(code):
    """Lines of code with namespaces and blank lines dropped, spaces folded."""
    out = []
    for line in code.split("\n"):
        s = re.sub(r"\s+", " ", line).strip()
        s = re.sub(r"\b(jsonrpc|mcp|acp)::", "", s)
        if not s or s in ("{", "}", "};", "public:", "private:", "protected:") \
                or s.startswith(("#include", "#pragma", "namespace ", "} // namespace", "using ")):
            continue
        out.append(s)
    return out


def duplication(root):
    hits = []
    redefine = re.compile(r"\b(struct|class|using|enum\s+class|concept)\s+(" + "|".join(CORE_NAMES) + r")\b")
    windows = {}   # hash of DUP_WINDOW lines -> (lib, rel, line)
    for lib in ["jsonrpc-cpp", "mcp-cpp", "acp-cpp"]:
        for path in lib_files(root, lib, ("include",) if lib == "jsonrpc-cpp" else ("src", "include")):
            rel = os.path.relpath(path, os.path.join(root, "third_party"))
            code = strip(open(path, encoding="utf-8", errors="replace").read())
            if lib != "jsonrpc-cpp":
                for no, line in enumerate(code.split("\n"), 1):
                    m = redefine.search(line)
                    if m and not re.search(r"=\s*(::)?jsonrpc::", line):
                        hits.append(f"{rel}:{no}: redefines jsonrpc-cpp's {m.group(2)}")
            lines = normalise(code)
            for i in range(len(lines) - DUP_WINDOW + 1):
                chunk = lines[i:i + DUP_WINDOW]
                # Skip chunks that are mostly punctuation or one-word lines.
                if sum(len(x) for x in chunk) < 40 * DUP_WINDOW // 2:
                    continue
                key = "\n".join(chunk)
                prev = windows.get(key)
                if prev and prev[0] != lib:
                    hits.append(f"{rel}: {DUP_WINDOW} lines match {prev[1]}: {chunk[0][:60]}")
                    break   # one report per file is enough
                windows.setdefault(key, (lib, rel))
    print("no_duplication: jsonrpc-cpp, mcp-cpp, acp-cpp")
    return hits


# agentty modules that integrate a JSON-RPC library (docs/PROTOCOL_LIBRARIES.md §6).
JSONRPC_USERS = ("src/rpc/", "include/agentty/rpc/", "src/mcp/", "include/agentty/mcp/",
                 "src/acp/", "include/agentty/acp/", "src/provider/external_acp",
                 "include/agentty/provider/external_acp", "src/tool/mcp_")


# Standard headers jsonrpc-cpp may use: no threads, futures or io. <chrono> is
# here for durations and time points it is handed; reading a clock is R4.
STD_OK = {
    "algorithm", "array", "cassert", "cctype", "charconv", "chrono", "compare", "concepts",
    "cstddef", "cstdint", "cstring", "deque", "expected", "functional",
    "initializer_list", "iterator", "limits", "map", "memory", "optional",
    "set", "span", "stdexcept", "string", "string_view", "tuple", "type_traits",
    "unordered_map", "unordered_set", "utility", "variant", "vector",
}


def jsonrpc_leaf(root):
    hits = []
    inc = re.compile(r'^\s*#\s*include\s*[<"]([^>"]+)[>"]')
    for path in lib_files(root, "jsonrpc-cpp", ("include", "src")):
        rel = os.path.relpath(path, os.path.join(root, "third_party"))
        for no, line in enumerate(open(path, encoding="utf-8").read().split("\n"), 1):
            m = inc.match(line)
            if not m:
                continue
            h = m.group(1)
            local = '"' in line and os.path.exists(os.path.join(os.path.dirname(path), h))
            if local or h.startswith(("jsonrpc/", "nlohmann/")) or h in STD_OK:
                continue
            hits.append(f"{rel}:{no}: includes {h}")
    for sub in ("src", "include"):
        for dp, _, fs in os.walk(os.path.join(root, sub)):
            for f in fs:
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(dp, f)
                rel = os.path.relpath(path, root)
                if rel.startswith(JSONRPC_USERS):
                    continue
                for no, line in enumerate(open(path, encoding="utf-8", errors="replace").read().split("\n"), 1):
                    m = inc.match(line)
                    if m and m.group(1).startswith("jsonrpc/"):
                        hits.append(f"{rel}:{no}: includes {m.group(1)} outside the rpc/mcp/acp modules")
    print("jsonrpc_leaf: jsonrpc-cpp is a leaf; agentty uses it only in rpc/mcp/acp")
    return hits


def drop_win32(code):
    """Blank lines inside #if/#ifdef _WIN32 branches (and #else of #if !_WIN32)."""
    out, stack = [], []   # stack of (is_win_branch_now, kind)
    for line in code.split("\n"):
        s = line.strip()
        m = re.match(r"#\s*(if|ifdef|ifndef|elif|else|endif)\b(.*)", s)
        if m:
            d, rest = m.group(1), m.group(2)
            if d in ("if", "ifdef", "ifndef"):
                win = "_WIN32" in rest
                neg = d == "ifndef" or bool(re.search(r"!\s*defined\s*\(?\s*_WIN32", rest))
                stack.append([win and not neg, win])
            elif d in ("elif", "else") and stack:
                top = stack[-1]
                top[0] = top[1] and not top[0] if d == "else" else ("_WIN32" in rest)
            elif d == "endif" and stack:
                stack.pop()
            out.append("")
            continue
        out.append("" if any(t[0] for t in stack) else line)
    return "\n".join(out)


def spawn_via_jaal(root):
    return scan_agentty(root, re.compile(
        r"[^\w.>:]v?fork\s*\(|::v?fork\s*\(|\bexec[lv]p?e?\s*\(|\bposix_spawnp?\b"
        r"|[^\w.>]_?popen\s*\(|\bpclose\s*\(|\bwaitpid\s*\(|[^\w.>]system\s*\("),
        "spawn_via_jaal: every agentty child starts through jaal's posix_process")


def no_sleep_poll(root):
    # A wait is on the event: maya::delay_for on a stop_token, a reactor, or
    # guarded::wait_with(_for). A sleep in a loop that rechecks a flag is a
    # poll, and it is how cancel ended up taking a second to land.
    return scan_agentty(root, re.compile(r"\bsleep_for\b|\bsleep_until\b|[^\w.>:]usleep\s*\("),
        "no_sleep_poll: agentty waits on events, not sleeps")


def setenv_in_main(root):
    # setenv racing a getenv on another thread is undefined, so the process
    # environment is written only at the top of main(), before any thread.
    hits = scan_agentty(root, re.compile(r"[^\w.>]_?(setenv|unsetenv|putenv|putenv_s)\s*\("),
        "setenv_in_main: only main() writes the environment, before threads start")
    return [h for h in hits if "runtime/main.cpp" not in h.split(":", 1)[0]]


def scan_agentty(root, rx, banner):
    hits = []
    for sub in ("src", "include"):
        for dp, _, fs in os.walk(os.path.join(root, sub)):
            for f in sorted(fs):
                if not f.endswith(EXTS):
                    continue
                path = os.path.join(dp, f)
                code = drop_win32(strip(open(path, encoding="utf-8", errors="replace").read()))
                for no, line in enumerate(code.split("\n"), 1):
                    if rx.search(" " + line):
                        hits.append(f"{os.path.relpath(path, root)}:{no}: {line.strip()}")
    print(banner)
    return hits


def main():
    if len(sys.argv) != 3 or sys.argv[2] not in ("purity", "duplication", "jsonrpc_leaf",
                                                 "spawn_via_jaal", "no_sleep_poll",
                                                 "setenv_in_main"):
        print(__doc__)
        return 2
    root, what = sys.argv[1], sys.argv[2]
    hits = {"purity": purity, "duplication": duplication, "jsonrpc_leaf": jsonrpc_leaf,
            "spawn_via_jaal": spawn_via_jaal, "no_sleep_poll": no_sleep_poll,
            "setenv_in_main": setenv_in_main}[what](root)
    if hits:
        print(f"\n{what}: {len(hits)} violation(s) (docs/PROTOCOL_LIBRARIES.md):")
        for h in hits:
            print("  " + h)
        return 1
    print("ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
