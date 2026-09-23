# AgenttyTests.cmake — the declarative test table.
#
# One agentty_test() per test. MODE consolidated folds into the agentty_tests
# doctest binary; standalone builds its own exe (forkers/PTY/fuzzers/benches);
# raw = caller-defined (narrow-source sanitizer tests). Aggregates (`tests`,
# `tests_gating`, `sanitizer_tests`) are DERIVED at finalize — no hand-listing.
#
# Requires (set by the root before include): AGENTTY_SHARED_OBJECTS,
# AGENTTY_HAS_RAGCPP, AGENTTY_HAS_MIMALLOC, AGENTTY_MCP, and the imported
# targets (maya::maya, mcp::*, acp::acp, doctest::doctest, OpenSSL, …).

include(AgenttyTestRegistry)

# ── Consolidated unit tests (doctest TEST_CASEs in agentty_tests) ───────────
# Pure/logic tests that link the shared object set once. Each was migrated off
# a per-exe build; see git history for the per-test rationale comments.
set(_AGENTTY_CONSOLIDATED
    error_class_test accounts_registry_test acp_agents_test acp_integration_test
    custom_host_key_prompt_test dispatch_route_test provider_keys_seal_test
    model_label_test cache_anchor_test composer_edit_test hooks_gate_test
    tool_result_image_test
    image_dims_test
    quit_cancels_stream_test
    midrun_freeze_test smart_mode_test stream_liveness_test wire_golden_test
    smart_tuning_settings_test smart_routing_card_test settings_nav_test
    wire_shared_test complexity_test copilot_token_test kimi_token_test
    chatgpt_bundled_models_test settings_default_test
    turn_provenance_test subagent_pin_test
    update_check_test update_ux_test mcp_result_type_test
    workspace_index_test
    teardown_test
    snapshot_picker_test
    dup_tool_call_id_test salvage_dedup_test compaction_wire_test
    plugins_in_model_test tool_stream_snapshot_test tool_timeline_adapter_test
    anthropic_sse_golden_test codex_login_flow_test mcp_reload_race_test
    persistence_proactive_test proactive_deferred_test rag_adapter_test
    scheduler_path_test tool_result_budget_test tool_wedge_liveness_test
    transcript_bound_test turn_settle_test midrun_seam_test midrun_wire_test
    codex_responses_test doom_loop_test visual_hash_coverage_test
    wire_fragmentation_test provider_identity_test provider_conformance_test
    provider_matrix_test tool_call_identity_test attribution_discipline_test
    empty_tool_args_test responses_log_test reasoning_ssot_test
    copilot_item_id_test wire_audit_test
    login_routing_test tool_advertisement_test gateway_context_window_test
    smart_settings_roundtrip_test sandbox_parity_test context_ladder_test
    capability_conformance_test
    ollama_transport_test openai_transport_test code_block_extract_test
    openai_dialect_test host_probe_taxonomy_test
    command_palette_test compaction_threshold_test fsm_test model_caps_test
    dialect_test
    embed_backend_test form_test embed_form_test escape_guarantee_test
    theme_discipline_test
    key_routing_test
    native_visibility_test
    palette_nav_test panel_test panel_nav_test status_bar_cache_test
    appearance_rows_test
    issue37_terminal_respect_test
    param_tag_repair_test sandbox_escape_test scope_test table_render_test
    ssrf_guard_test render_key_coverage_test reasoning_render_test
    plugin_config_test skills_engine_test skill_effects_trust_test
    skill_screen_test
    slash_commands_test fuzzy_match_smoke
    provider_model_switch_test
    oauth_proactive_refresh_test maya_host_sequence_test
    smart_slot_panel_stack_test account_switch_refresh_test fused_models_test
    panel_sections_render_test
    credentials_test entitlement_test inflate_test
    settings_list_scroll_test visual_walk_test md_robustness_test
    stats_test
    ui_prefs_test
    stats_scroll_test
    stats_visual_hash_test
    panel_overflow_probe
    panel_draw_budget_test
    stats_golden_test
    stats_render_probe
    context_window_test
    thread_blob_test lazy_image_test thread_log_test
    thread_migration_test blob_gc_test)
foreach(_t ${_AGENTTY_CONSOLIDATED})
    agentty_test(${_t} MODE consolidated)
endforeach()

# ── Standalone full-stack tests ─────────────────────────────────────────────
# Forkers / PTY / fuzzers / e2e / benches that can't share the doctest process.
# ── Folded standalone tests ────────────────────────────────────────────
# These can't be doctest cases in a shared process (fork/exec, PTY, fuzz seed
# loops, subprocess e2e) — but they DON'T each need their own 100 MB+ link.
# agentty_fold_test() puts them all in ONE binary (agentty_standalone_tests)
# and gives each its own ctest entry that runs it as a separate PROCESS:
#   add_test(NAME x COMMAND agentty_standalone_tests x)
# so process isolation is identical to before, at 1 link instead of ~16.
# Each TU's main() is renamed to <name>_main via a per-source -Dmain=; the
# dispatcher (tests/agentty_standalone_tests_main.cpp + .def) calls it.
agentty_fold_test(long_session_bench       TIMEOUT 600 LABELS perf)
agentty_fold_test(cross_process_lock_test  TIMEOUT 30)
# Drives skills::catalog_block()/activation_payload() with a COLD all() cache
# and its own HOME — in the shared binary another case has already warmed the
# mtime cache, so the interesting case (unapproved skill hidden) can't be set
# up honestly there.
agentty_fold_test(skill_catalog_trust_test TIMEOUT 30)
# Own HOME + cold skills cache, same reason as skill_catalog_trust_test.
agentty_fold_test(skills_panel_test        TIMEOUT 30)
agentty_fold_test(catalog_cost_probe       TIMEOUT 60)
# Compiles AtomicSnapshot's mutex fallback (the libc++/Termux path) on a
# toolchain that HAS the atomic specialisation, so it can't rot unnoticed.
# A BARE binary: snapshot.hpp is header-only, and this test must link NO
# agentty objects. AGENTTY_FORCE_SNAPSHOT_MUTEX changes AtomicSnapshot's
# layout, so a TU compiled with it linked against objects compiled without it
# is an ODR violation — same class, two definitions, linker picks one and the
# program hangs in static init. Found the hard way; hence MODE raw.
# snapshot.hpp's mutex fallback (the libc++/Termux path) as a BARE binary.
#
# It must link NO agentty objects: AGENTTY_FORCE_SNAPSHOT_MUTEX changes
# AtomicSnapshot's layout, so a TU compiled with it linked against objects
# compiled without it is an ODR violation — same class, two definitions. The
# linker picks one and the program hangs in static init. Found the hard way.
add_executable(snapshot_mutex_fallback_test EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/snapshot_mutex_fallback_test.cpp)
target_include_directories(snapshot_mutex_fallback_test PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_compile_definitions(snapshot_mutex_fallback_test PRIVATE
    AGENTTY_FORCE_SNAPSHOT_MUTEX=1)
find_package(Threads REQUIRED)
target_link_libraries(snapshot_mutex_fallback_test PRIVATE Threads::Threads)
add_test(NAME snapshot_mutex_fallback_test
         COMMAND snapshot_mutex_fallback_test)
set_tests_properties(snapshot_mutex_fallback_test PROPERTIES TIMEOUT 60)
# EXCLUDE_FROM_ALL keeps it out of a plain `make`, which is right — it must
# not be linked into anything. But add_test() registers it with ctest
# regardless, so if nothing ever builds it the suite reports "Not Run" and
# fails the job. That is what it did: a red CI job for a test whose binary
# was never produced.
#
# It cannot go through agentty_test() (that helper links the agentty objects
# this test must avoid), so it joins the DERIVED aggregates by hand here —
# the one place a hand-rolled target has to opt in, next to the reason why.
set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_STANDALONE
             snapshot_mutex_fallback_test)
set_property(DIRECTORY APPEND PROPERTY AGENTTY_T_SANITIZER
             snapshot_mutex_fallback_test)
agentty_fold_test(fork_test                TIMEOUT 30)
agentty_fold_test(palette_render_probe     TIMEOUT 30)
agentty_fold_test(embed_render_probe       TIMEOUT 30)
agentty_fold_test(form_edit_nav_test       TIMEOUT 30)
agentty_fold_test(login_render_probe       TIMEOUT 30 ARGS)
agentty_fold_test(settings_add_render_probe TIMEOUT 30 ARGS)
agentty_fold_test(thread_delete_test       TIMEOUT 30)
agentty_fold_test(diff_review_test         TIMEOUT 30)
agentty_fold_test(reveal_freeze_gate_probe TIMEOUT 30)
# Regression for maya 54ad00d: the settled markdown tree outlives its widget
# (frozen scrollback stashes it), so its layout lambda must not write through
# a captured `this`. Standalone because it deliberately destroys widgets and
# renders the orphaned trees.
agentty_fold_test(streaming_markdown_lifetime_test TIMEOUT 60)
# Crash probe: renders a REAL thread file passed on the command line.
# ARGS so ctest runs it with none (it exits 2 and passes trivially there);
# the point is running it BY HAND against ~/.agentty/threads/<id>.json when
# a user reports a render crash the synthetic bench doesn't reproduce.
agentty_fold_test(real_thread_render_probe TIMEOUT 60 ARGS)
# Round-trips every REAL thread in ~/.agentty/threads through ThreadLog.
# ARGS + no-op when the corpus is absent, so CI passes trivially; the
# value is running it BY HAND before trusting the migration with history.
agentty_fold_test(thread_log_corpus_probe TIMEOUT 300 ARGS)
# Proves the STORE SEAM prefers the log: converts a real thread, reads it
# back through load_thread_by_id, and compares. Needs AGENTTY_HOME + an id.
agentty_fold_test(thread_log_seam_probe   TIMEOUT 120 ARGS)
# Runs the REAL save path over a REAL thread and proves the legacy file is
# retired only after every message, tool output and image byte reads back.
agentty_fold_test(thread_migration_probe  TIMEOUT 300 ARGS)
# Migrates a whole threads/ directory (a COPY) and proves every thread's
# content is identical afterwards. The rehearsal before touching real data.
agentty_fold_test(thread_migration_bulk_probe TIMEOUT 900 ARGS)
# Splits a thread switch into worker-thread vs UI-thread cost, so the next
# optimisation targets what the user actually waits on.
agentty_fold_test(thread_switch_prof_probe TIMEOUT 120 ARGS)
# Mark-and-sweep the blob store of a real threads dir. DRY RUN unless
# --apply, because the files at stake hold images and tool output.
agentty_fold_test(blob_gc_probe TIMEOUT 300 ARGS)
# What a terminal RESIZE costs: a width change invalidates every cached
# layout, so the whole frozen canvas re-lays-out at the new width.
agentty_fold_test(resize_prof_probe TIMEOUT 120 ARGS)
# Does browsing themes DEGRADE? "Laggy after a while" is the signature of
# unbounded growth, not slow code, so this walks the browser hundreds of
# times and reports per-switch cost + RSS in buckets: flat = no leak.
agentty_fold_test(theme_switch_leak_probe TIMEOUT 300 ARGS)
# The other half: flat-and-expensive still feels laggy if keys arrive faster
# than a switch costs. Reports whether one arrow fits in a key-repeat slot.
agentty_fold_test(theme_input_lag_probe TIMEOUT 120 ARGS LABELS perf)
agentty_fold_test(theme_lag_repro TIMEOUT 180 ARGS NO_TEST)
agentty_fold_test(theme_memo_stale_probe TIMEOUT 120 NO_TEST)
# THE invariant every list overlay owes its user: the highlighted row is on
# screen. Written against FilteredPicker, so it holds for every picker built
# on it rather than only the theme browser that broke.
agentty_fold_test(picker_cursor_visible_test TIMEOUT 120)
# The reported gesture, against a PERSISTENT renderer. Every other theme
# probe renders through render_to_string (fresh pool + cache per call), which
# structurally cannot catch a stale cross-frame cache entry.
agentty_fold_test(theme_alternating_key_test TIMEOUT 180)
# The OTHER half of "live theme switch is flaky", and the one that is
# PERMANENT rather than late: a committed markdown block is stored as an
# already-RENDERED Element, so it keeps the palette that was live when its
# text was committed. Everything still animating repaints correctly, which is
# what made it look intermittent.
agentty_fold_test(theme_settled_recolour_test TIMEOUT 180)
# Every panel must answer a keypress inside one key-repeat slot, AND a
# burst delivered in one read must land where single-stepping lands.
agentty_fold_test(panel_input_snappiness_test TIMEOUT 180 LABELS perf)
# Does every arrow get a FRAME? Batched input reduces N events and paints
# once, so rows the user passes through are computed but never shown.
agentty_fold_test(frame_per_key_probe TIMEOUT 120 ARGS)
# A theme change recolours every cell, so the whole viewport goes on the
# wire per keypress. Compares that against a plain cursor move.
agentty_fold_test(theme_wire_cost_probe TIMEOUT 120 ARGS)
if(UNIX)
    # PTY-driven (openpty); full-runtime ghost-caret repro — see the
    # header of tests/test_ghost_caret_runtime.cpp (credit: davidwed).
    agentty_fold_test(test_ghost_caret_runtime TIMEOUT 60 SKIP_CODE 77 UNIX_LIBS util)
endif()
agentty_fold_test(toolset_e2e_test         TIMEOUT 120)
agentty_fold_test(subagent_report_test     TIMEOUT 60)
agentty_fold_test(plugin_disabled_tools_test TIMEOUT 60)
agentty_fold_test(tool_budget_env_test     TIMEOUT 60)
agentty_fold_test(frozen_invariant_fuzz)
agentty_fold_test(scrollback_wire_fuzz     TIMEOUT 120)
agentty_fold_test(reveal_scrollback_test   TIMEOUT 180 UNIX_LIBS util)
agentty_fold_test(scrollback_oracle_test   TIMEOUT 600 UNIX_LIBS util)
agentty_fold_test(external_acp_backend_test TIMEOUT 60)
agentty_fold_test(md_shape_sweep           TIMEOUT 120)
agentty_fold_test(reveal_headroom_test     TIMEOUT 60)
agentty_fold_test(md_cache_probe           TIMEOUT 120 LABELS perf)
if(AGENTTY_MCP)
    agentty_fold_test(mcp_bridge_test      TIMEOUT 60)
    set_tests_properties(mcp_bridge_test PROPERTIES ENVIRONMENT
        "AGENTTY_MCP_E2E_SERVER=${CMAKE_BINARY_DIR}/mcp-cpp/examples/mcp_server_example")
    agentty_fold_test(mcp_http_test        TIMEOUT 60)
endif()
# anthropic_md_stream is a capture/replay HARNESS, not a ctest entry of its own:
# the reveal_stream_gate* arms below invoke it (with args) through the folded
# binary. Registered in the .def; no add_test here.
set_source_files_properties(${CMAKE_SOURCE_DIR}/tests/anthropic_md_stream.cpp
    PROPERTIES COMPILE_DEFINITIONS "main=anthropic_md_stream_main")
set_property(DIRECTORY APPEND PROPERTY AGENTTY_FOLD_NAMES anthropic_md_stream)

# Build the one binary: union of every folded test's extra objs/libs.
# persistence_race: the REAL async save queue under TSan, not a model of it.
# Folded (it owns main + AGENTTY_HOME, and needs the full io object set), so
# it is registered in agentty_standalone_tests.def alongside its siblings. It
# only runs instrumented in the TSan tree, where everything is rebuilt anyway.
agentty_fold_test(persistence_race_test TIMEOUT 180 LABELS race)
agentty_fold_test(theme_preview_cost_probe TIMEOUT 120 NO_TEST)

agentty_finalize_fold(
    OBJS $<TARGET_OBJECTS:agentty_acp_obj>
    LIBS acp::acp)

# agents_md_test — locks wire::agents_md_block (AAIF AGENTS.md standard).
# Kept as its OWN binary: it chdir()s into temp workspaces, and it's new enough
# that folding it hasn't been validated.
agentty_test(agents_md_test          MODE standalone TIMEOUT 30)
agentty_test(checkpoint_test         MODE standalone TIMEOUT 60)

# stats_visual — dump every stats tab in real ANSI, for a human to LOOK at.
# NO_TEST on purpose: its output is colour and layout, and a test asserting
# "this looks nice" asserts nothing. Build it explicitly:
#   cmake --build build --target stats_visual && ./build/stats_visual
agentty_test(stats_visual            MODE standalone NO_TEST)
# NO_TEST too: it prints timings for a human. An assertion on microseconds
# would be a flaky test of the machine it runs on, not of the code.
agentty_test(stats_refresh_bench     MODE standalone NO_TEST)


# ── Narrow-source sanitizer tests (raw: must NOT link the full shared set) ──
# They exercise agentty's own logic and link cleanly under asan/ubsan without
# pulling maya's un-instrumented renderer. Registered raw + marked sanitizer.
agentty_test(concurrency_primitives_test MODE raw LABELS sanitizer)
add_executable(concurrency_primitives_test EXCLUDE_FROM_ALL
    tests/concurrency_primitives_test.cpp src/util/dbglog.cpp src/util/logx.cpp)
target_include_directories(concurrency_primitives_test PRIVATE include)
add_test(NAME concurrency_primitives_test COMMAND concurrency_primitives_test)
set_tests_properties(concurrency_primitives_test PROPERTIES TIMEOUT 30 LABELS sanitizer)

# race_harness: the THREAD-sanitizer lane. Registered raw + narrow-source for
# the same reason as its neighbours (no maya renderer to ODR-clash), and
# labelled BOTH `sanitizer` and `race` so CI can run it under TSan separately
# — ASan and TSan cannot be combined in one binary.
#
# Why it exists: every concurrency bug this codebase shipped was invisible to
# asan+ubsan (a shared_ptr cycle that made a worker join itself, a worker left
# joinable in a static, N threads racing to publish one map). Races need a
# race detector.
agentty_test(race_harness_test MODE raw LABELS sanitizer)
add_executable(race_harness_test EXCLUDE_FROM_ALL
    tests/race_harness_test.cpp src/util/teardown.cpp
    src/util/logx.cpp src/util/dbglog.cpp)
target_include_directories(race_harness_test PRIVATE include)
add_test(NAME race_harness_test COMMAND race_harness_test)
set_tests_properties(race_harness_test PROPERTIES TIMEOUT 120 LABELS "sanitizer;race")


agentty_test(cred_crypt_test MODE raw LABELS sanitizer)
add_executable(cred_crypt_test EXCLUDE_FROM_ALL
    tests/cred_crypt_test.cpp src/io/cred_crypt.cpp src/util/base64.cpp)
target_include_directories(cred_crypt_test PRIVATE include)
target_link_libraries(cred_crypt_test PRIVATE
    nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME cred_crypt_test COMMAND cred_crypt_test)
set_tests_properties(cred_crypt_test PROPERTIES TIMEOUT 60 LABELS sanitizer)

# logx: standalone binary ON PURPOSE — the log system latches its env config
# on first use (magic static), so the test must own its process to set
# AGENTTY_LOG/_FILE before anything logs.
agentty_test(logx_test MODE raw)
add_executable(logx_test EXCLUDE_FROM_ALL
    tests/logx_test.cpp src/util/logx.cpp src/util/dbglog.cpp)
target_include_directories(logx_test PRIVATE include)
add_test(NAME logx_test COMMAND logx_test)
set_tests_properties(logx_test PROPERTIES TIMEOUT 30)

# logx redaction/format: standalone for the SAME reason as logx_test above —
# the sink latches on first use, so a test that needs logging ON must own its
# process. These were briefly folded into the consolidated binary, where the
# guard `if (!logging_on()) return;` made all 8 cases pass with ZERO
# assertions in CI: a green suite proving nothing. ENV makes ctest configure
# the log before the binary starts, so the assertions actually run.
foreach(_logx_t logx_redaction_test logx_format_test logx_lifecycle_test)
    agentty_test(${_logx_t} MODE raw)
    add_executable(${_logx_t} EXCLUDE_FROM_ALL
        tests/${_logx_t}.cpp tests/test_main.cpp
        src/util/logx.cpp src/util/dbglog.cpp)
    target_include_directories(${_logx_t} PRIVATE include tests)
    # nlohmann for the HEADERS, not for a symbol: these tests include
    # agtest.hpp, which reaches tool/util/fs_helpers.hpp -> tool/registry.hpp
    # -> <nlohmann/json.hpp>. A raw target gets no include paths from the
    # object libraries, so without this the build fails at the SCAN step
    # (C++20 module dependency scanning), which is why it surfaced in CI as a
    # fatal "no such file" on a test that links nothing json-related.
    target_link_libraries(${_logx_t} PRIVATE
        doctest::doctest maya::maya nlohmann_json::nlohmann_json)
    add_test(NAME ${_logx_t} COMMAND ${_logx_t})
    set_tests_properties(${_logx_t} PROPERTIES TIMEOUT 30
        ENVIRONMENT "AGENTTY_LOG=trace;AGENTTY_LOG_FILE=${CMAKE_CURRENT_BINARY_DIR}/${_logx_t}.log")
endforeach()

# logx rotation: same standalone-process reason as the block above. It also
# needs a rotation threshold small enough to actually CROSS — at the
# shipping 32 MB the mid-run rotation seam takes minutes to reach, which is
# precisely how a use-after-close in it survived: writers read the sink fd
# without the rotate lock, so the old swap-then-close published a descriptor
# the kernel could recycle under them. The test lowers the threshold itself
# via an internal symbol, so there is no env knob to configure here.
agentty_test(logx_rotation_test MODE raw)
add_executable(logx_rotation_test EXCLUDE_FROM_ALL
    tests/logx_rotation_test.cpp tests/test_main.cpp
    src/util/logx.cpp src/util/dbglog.cpp)
target_include_directories(logx_rotation_test PRIVATE include tests)
# Same transitive nlohmann include as the logx block above (agtest.hpp).
target_link_libraries(logx_rotation_test PRIVATE
    doctest::doctest maya::maya nlohmann_json::nlohmann_json)
add_test(NAME logx_rotation_test COMMAND logx_rotation_test)
set_tests_properties(logx_rotation_test PROPERTIES TIMEOUT 30
    ENVIRONMENT "AGENTTY_LOG=trace;AGENTTY_LOG_FILE=${CMAKE_CURRENT_BINARY_DIR}/logx_rotation_test.log")

agentty_test(keystore_test MODE raw LABELS sanitizer)
add_executable(keystore_test EXCLUDE_FROM_ALL
    tests/keystore_test.cpp src/io/keystore.cpp src/tool/util/subprocess.cpp
    src/tool/util/fs_helpers.cpp src/tool/util/utf8.cpp src/tool/progress.cpp
    src/util/home_dir.cpp)   # fs_helpers.cpp → util::home_dir(); undefined ref
                             # only surfaces in the -fno-lto sanitizer link
target_include_directories(keystore_test PRIVATE include)
target_link_libraries(keystore_test PRIVATE maya::maya nlohmann_json::nlohmann_json)
if(TARGET mcp::tools)
    target_link_libraries(keystore_test PRIVATE mcp::tools)  # fs_helpers → mcp util include
endif()
if(WIN32)
    target_link_libraries(keystore_test PRIVATE advapi32)
endif()
add_test(NAME keystore_test COMMAND keystore_test)
set_tests_properties(keystore_test PROPERTIES TIMEOUT 60 LABELS sanitizer)

agentty_test(host_escape_test MODE raw)
add_executable(host_escape_test EXCLUDE_FROM_ALL
    tests/host_escape_test.cpp src/runtime/view/host_escape.cpp)
target_include_directories(host_escape_test PRIVATE include)
add_test(NAME host_escape_test COMMAND host_escape_test)
set_tests_properties(host_escape_test PROPERTIES TIMEOUT 30)

# Single-root layout + legacy ~/.config/agentty migration. Narrow link —
# user_root.cpp + home_dir.cpp only — so the sandboxed $HOME manipulation
# can't interact with any other subsystem's statics.
agentty_test(user_root_test MODE raw)
add_executable(user_root_test EXCLUDE_FROM_ALL
    tests/user_root_test.cpp src/util/user_root.cpp src/util/home_dir.cpp)
target_include_directories(user_root_test PRIVATE include)
add_test(NAME user_root_test COMMAND user_root_test)
set_tests_properties(user_root_test PROPERTIES TIMEOUT 30)

# ── CLI argument order ──────────────────────────────────────────────────────
# A shell test because parse_args lives inside main.cpp and the contract worth
# pinning is the BINARY's: `agentty run` must accept --agent, -w/-m and the
# prompt in any order. The parser used to stop at the first flag it didn't own,
# so a prompt after -w died as "unknown arg: <prompt>" — an invisible rule,
# since the error named the prompt rather than the position.
add_test(NAME cli_arg_order_test
         COMMAND ${CMAKE_COMMAND} -E env sh
                 ${CMAKE_SOURCE_DIR}/tests/cli_arg_order_test.sh
                 $<TARGET_FILE:agentty>)
set_tests_properties(cli_arg_order_test PROPERTIES TIMEOUT 60)

# ── Finalize: build agentty_tests + derived aggregates ──────────────────────
agentty_finalize_tests()

# ── reveal_stream_gate arms — ctest entries running anthropic_md_stream ──────
# Regression gate on the live reveal glide over a recorded Anthropic stream.
set(_RSG_FIXTURE ${CMAKE_SOURCE_DIR}/tests/fixtures/anthropic_md_smoke.jsonl)
agentty_add_ctest(reveal_stream_gate COMMAND
    agentty_standalone_tests anthropic_md_stream det ${_RSG_FIXTURE}
    --assert-max-delta 40 --assert-finalize-max 40 --assert-finalize-ms 3600)
agentty_add_ctest(reveal_stream_gate_prod COMMAND
    agentty_standalone_tests anthropic_md_stream det ${_RSG_FIXTURE}
    --cps 45 --drain 0.40 --adaptive
    --assert-max-delta 40 --assert-finalize-max 40 --assert-finalize-ms 3600)
agentty_add_ctest(reveal_stream_gate_snap COMMAND
    agentty_standalone_tests anthropic_md_stream det ${_RSG_FIXTURE}
    --cps 45 --drain 0.40 --adaptive --snap-at 40 --snap-glide 150
    --assert-max-delta 40 --assert-finalize-max 40 --assert-finalize-ms 3600)
