# assert_static_pie.cmake — build-time ELF-shape guard for the fully-static
# Linux binary. Invoked as a POST_BUILD step (see the AGENTTY_FULLY_STATIC
# block in CMakeLists.txt) with -DBIN=<binary> -DREADELF=<readelf>.
#
# WHY THIS EXISTS: agentty v0.2.7 shipped a Linux x86_64 "static" binary that
# was actually a *dynamic*-PIE — the musl/Alpine GCC driver resolved the CRT
# startup to Scrt1.o and emitted an ET_DYN that kept `NEEDED libc.musl-*.so`
# but DROPPED PT_INTERP. It ran on Alpine (loader present at the baked path)
# and SIGSEGV'd instantly on glibc/Debian (no interp → kernel maps it at a
# random base and jumps to an *unrelocated* entry point). Nothing in the build
# caught it: CI only ran readelf on aarch64, as non-fatal warnings, and never
# executed the binary.
#
# A CORRECT static-PIE is:
#   * ET_DYN            (Type: DYN)             — so it's PIE / self-relocating
#   * has NO PT_INTERP  (no external loader)    — fully self-contained
#   * has NO NEEDED     (no dynamic libraries)  — truly static
# (A literal PT_PHDR program-header row is NOT required: glibc static-PIE
#  self-relocates without emitting one, so the discriminators are INTERP
#  and NEEDED — both present in v0.2.7's crash shape, both absent when static.)
#
# Any deviation means the link degraded and the artifact will crash on some
# foreign libc. Fail the BUILD (message FATAL_ERROR → non-zero exit) so the
# broken binary can never be packaged, uploaded, or installed — on the release
# runner, a local AGENTTY_FULLY_STATIC build, or the installer's source build.

if(NOT DEFINED BIN OR NOT EXISTS "${BIN}")
    message(FATAL_ERROR "assert_static_pie: binary not found: '${BIN}'")
endif()
if(NOT DEFINED READELF)
    set(READELF readelf)
endif()

# --- ELF file type: must be DYN (position-independent / self-relocating) ------
execute_process(
    COMMAND ${READELF} -h "${BIN}"
    OUTPUT_VARIABLE _hdr
    RESULT_VARIABLE _hrc
    ERROR_VARIABLE  _herr)
if(NOT _hrc EQUAL 0)
    message(FATAL_ERROR "assert_static_pie: readelf -h failed on '${BIN}':\n${_herr}")
endif()
# The Type line looks like: "  Type:  DYN (Position-Independent Executable ...)"
if(NOT _hdr MATCHES "Type:[ \t]*DYN")
    message(FATAL_ERROR
        "assert_static_pie: FATAL — '${BIN}' is not ET_DYN (static-PIE). A plain "
        "ET_EXEC won't load on Android/Bionic; the -static-pie link degraded.\n"
        "readelf -h said:\n${_hdr}")
endif()

# --- Program headers: must have PT_PHDR, must NOT have PT_INTERP ---------------
execute_process(
    COMMAND ${READELF} -l "${BIN}"
    OUTPUT_VARIABLE _phdrs
    RESULT_VARIABLE _prc
    ERROR_VARIABLE  _perr)
if(NOT _prc EQUAL 0)
    message(FATAL_ERROR "assert_static_pie: readelf -l failed on '${BIN}':\n${_perr}")
endif()
# NOTE: we deliberately do NOT require a literal `PT_PHDR` *program-header*
# row here. A perfectly valid static-PIE (e.g. glibc's, which self-relocates
# via _dl_relocate_static_pie) often emits no explicit PHDR line in
# `readelf -l`, so requiring it would false-fail good binaries. The two
# signals that actually distinguish v0.2.7's broken dynamic-PIE from a real
# static-PIE are the ABSENCE of PT_INTERP (below) and the ABSENCE of NEEDED
# (further down) — both present in the crash shape, both absent when static.
if(_phdrs MATCHES "INTERP")
    message(FATAL_ERROR
        "assert_static_pie: FATAL — '${BIN}' has a PT_INTERP program header, i.e. "
        "it wants an external dynamic loader. This is the v0.2.7 crash shape "
        "(a dynamic-PIE masquerading as static): it will SIGSEGV on any host "
        "whose loader path differs from the build image.\n"
        "readelf -l said:\n${_phdrs}")
endif()

# --- Dynamic section: must have NO NEEDED entries (truly static) --------------
# readelf -d exits non-zero / prints nothing when there is no dynamic section;
# either way, the absence of NEEDED is what we require.
execute_process(
    COMMAND ${READELF} -d "${BIN}"
    OUTPUT_VARIABLE _dyn
    RESULT_VARIABLE _drc
    ERROR_VARIABLE  _derr)
if(_dyn MATCHES "NEEDED")
    message(FATAL_ERROR
        "assert_static_pie: FATAL — '${BIN}' has NEEDED dynamic-library entries, "
        "so libc (and friends) are NOT statically linked. This is exactly the "
        "v0.2.7 x86_64 failure: NEEDED libc.musl-*.so + no PT_INTERP → runs on "
        "the build image, SIGSEGVs everywhere else.\n"
        "readelf -d said:\n${_dyn}")
endif()

message(STATUS
    "agentty: static-PIE ELF shape OK — ET_DYN, no PT_INTERP, no NEEDED. "
    "Runs on glibc, musl, Termux/Bionic, and Pi OS.")
