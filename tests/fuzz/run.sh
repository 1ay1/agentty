#!/bin/sh
# Build and run agentty's libFuzzer harnesses (clang, ASan+UBSan).
#   sh tests/fuzz/run.sh [seconds per harness, default 60]
# Crash inputs land in build-fuzz/crashes/<harness>/; rerun one with
#   build-fuzz/fuzz_text build-fuzz/crashes/fuzz_text/crash-...
# Needs clang and nlohmann/json: system headers, or NLOHMANN_INCLUDE=<dir>.
set -eu
cd "$(dirname "$0")/../.."
SECS=${1:-60}
OUT=build-fuzz
mkdir -p "$OUT"

# nlohmann/json: a system copy needs no flag (adding /usr/include as
# -isystem breaks libstdc++'s #include_next); else one cmake fetched.
NJ=${NLOHMANN_INCLUDE:-}
if [ -z "$NJ" ]; then
    for d in build/_deps/nlohmann_json-src/include \
             third_party/rag-cpp/build/_deps/nlohmann_json-src/include; do
        [ -f "$d/nlohmann/json.hpp" ] && { NJ=$d; break; }
    done
fi
if [ -n "$NJ" ]; then NJ_FLAG="-isystem $NJ"
elif echo '#include <nlohmann/json.hpp>' | clang++ -std=c++20 -fsyntax-only -x c++ - 2>/dev/null; then NJ_FLAG=""
else echo "run.sh: nlohmann/json not found; set NLOHMANN_INCLUDE" >&2; exit 2; fi

INC="-Iinclude -Ithird_party/maya/include -Ithird_party/maya/third_party/jaal/include \
 -isystem third_party/mcp-cpp/include $NJ_FLAG"
FLAGS="-std=c++26 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined -fno-sanitize-recover=undefined"
printf '%s\n' "$INC" > "$OUT/include_flags"   # for one-off repros: eval clang++ $(cat ...)

clang++ $FLAGS $INC tests/fuzz/fuzz_text.cpp \
    src/domain/model_name.cpp src/tool/util/partial_json.cpp \
    src/tool/util/utf8.cpp src/util/base64.cpp \
    -o "$OUT/fuzz_text"

# richpaste: compile the whole library (minus other platforms) into the
# harness, instrumented, so its parsers are covered end to end.
RP=third_party/richpaste
RP_SRC=""
for f in core sha256 io io_posix wire clipboard wayland x11 image inband armor agent store term report; do
    RP_SRC="$RP_SRC $RP/src/$f.cpp"
done
clang++ -std=c++23 -O1 -g -fno-omit-frame-pointer -fsanitize=fuzzer,address,undefined \
    -fno-sanitize-recover=undefined -I$RP/include tests/fuzz/fuzz_richpaste.cpp $RP_SRC \
    -lpthread -ldl -o "$OUT/fuzz_richpaste"

# Seed the corpora with well-formed examples of each format.
clang++ -std=c++23 -O1 -I$RP/include tests/fuzz/seeds.cpp $RP_SRC -lpthread -ldl -o "$OUT/seeds"
"$OUT/seeds" "$OUT/corpus/fuzz_richpaste" "$OUT/corpus/fuzz_text"

fails=0
for h in fuzz_text fuzz_richpaste; do
    mkdir -p "$OUT/corpus/$h" "$OUT/crashes/$h"
    echo "== $h ($SECS s)"
    if ! "$OUT/$h" -max_total_time="$SECS" -max_len=4096 -timeout=10 \
           -artifact_prefix="$OUT/crashes/$h/" -print_final_stats=1 \
           "$OUT/corpus/$h" > "$OUT/$h.log" 2>&1; then
        echo "FAIL $h:"; grep -E "ERROR|runtime error|SUMMARY|#[0-9]+ 0x.* in " "$OUT/$h.log" | head -20
        fails=$((fails + 1))
    else
        tail -4 "$OUT/$h.log"
    fi
done
exit $fails
