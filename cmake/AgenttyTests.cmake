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
    effort_auto_test settings_multiproc_test
    wire_shared_test complexity_test copilot_token_test kimi_token_test
    chatgpt_bundled_models_test settings_default_test
    turn_provenance_test subagent_pin_test
    program_hooks_test login_cancel_test subs_key_test reducer_effects_test
    sim_turn_test view_purity_test
    update_check_test update_ux_test mcp_result_type_test
    workspace_index_test
    teardown_test
    snapshot_picker_test
    dup_tool_call_id_test salvage_dedup_test compaction_wire_test shell_detour_streak_test wire_tool_order_test
    subprocess_group_kill_test
    plugins_in_model_test tool_stream_snapshot_test tool_timeline_adapter_test
    anthropic_sse_golden_test codex_login_flow_test mcp_reload_race_test
    persistence_proactive_test proactive_deferred_test rag_adapter_test
    scheduler_path_test tool_result_budget_test tool_wedge_liveness_test
    transcript_bound_test turn_settle_test stream_clock_test midrun_seam_test midrun_wire_test
    codex_responses_test doom_loop_test visual_hash_coverage_test
    wire_fragmentation_test wire_supersede_test provider_identity_test provider_conformance_test
    provider_matrix_test tool_call_identity_test attribution_discipline_test
    empty_tool_args_test responses_log_test reasoning_ssot_test
    copilot_item_id_test wire_audit_test
    login_routing_test tool_advertisement_test gateway_context_window_test
    smart_settings_roundtrip_test sandbox_parity_test context_ladder_test
    capability_conformance_test
    ollama_transport_test openai_transport_test code_block_extract_test
    openai_dialect_test host_probe_taxonomy_test openai_conformance_policy_test
    openai_request_body_test context_window_robustness_test
    command_palette_test compaction_threshold_test fsm_test model_caps_test
    dialect_test
    embed_backend_test form_test embed_form_test escape_guarantee_test
    theme_discipline_test
    key_routing_test
    native_visibility_test
    palette_nav_test panel_test panel_nav_test status_bar_cache_test
    appearance_rows_test
    issue37_terminal_respect_test
    param_tag_repair_test shell_cmd_alias_test sandbox_escape_test sandbox_pane_test sandbox_broker_test handoff_gate_test handoff_shell_test scope_test table_render_test
    empty_turn_test i18n_test
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
    thread_migration_test thread_save_incremental_test blob_gc_test)
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
# The thread benches take a thread file; symbol_read_corpus takes a TSV of
# <path>\t<symbol> pairs. FIXTURE feeds each a small checked-in input so ctest
# actually EXERCISES them instead of running them bare, watching them print
# usage, and calling that a pass. Run by hand against a real thread
# (~/.agentty/threads/<id>.jsonl) when you want numbers that reflect your own
# history — the fixture is for catching breakage, not for benchmarking.
agentty_fold_test(tool_latency_bench ARGS  TIMEOUT 600 LABELS perf)
agentty_fold_test(turn_prep_bench          TIMEOUT 600 LABELS perf
                  FIXTURE tests/fixtures/bench_thread.jsonl)
agentty_fold_test(cache_churn_bench        TIMEOUT 600 LABELS perf
                  FIXTURE tests/fixtures/bench_thread.jsonl)
agentty_fold_test(save_bench               TIMEOUT 600 LABELS perf
                  FIXTURE tests/fixtures/bench_thread.jsonl)
agentty_fold_test(wire_encode_bench        TIMEOUT 600 LABELS perf
                  FIXTURE tests/fixtures/bench_thread.jsonl)
agentty_fold_test(symbol_read_corpus       TIMEOUT 600 LABELS perf
                  FIXTURE tests/fixtures/symbol_read_corpus.tsv)
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
# sandbox_live_check: does the sandbox actually ENFORCE, on this host, right
# now. Not a ctest, and deliberately: it needs working user+mount namespaces,
# it makes a real outbound connection to check that the permissive setting
# still permits, and both of those are properties of the machine rather than
# of the code. In CI it would either skip itself (proving nothing, greenly) or
# fail for reasons unrelated to the change.
#
# So it is a target you run by hand when touching the sandbox:
#     cmake --build build --target sandbox_live_check && ./build/sandbox_live_check
#
# What the automated suite covers instead: sandbox_escape_test asserts the
# ARGV/plan we build (no namespaces needed), and claybin's own 20-test suite
# covers the engine. This closes the last gap between "the plan says X" and
# "the child experiences X" -- which is exactly where the mremap bug lived:
# every table-level test passed while realloc() failed in the guest.
# Compiles the two backend sources straight in rather than linking
# agentty_tool_obj: that objlib drags the whole tool layer (mcp config,
# auth, teardown) behind it, and this check only needs the spawn paths.
# sandbox.cpp comes along for build_bwrap_argv -- the bwrap masking case
# drives the REAL argv rather than a hand-built one, so it cannot drift from
# what production passes to bwrap.
add_executable(sandbox_live_check EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/sandbox_live_check.cpp
    ${CMAKE_SOURCE_DIR}/tests/sandbox_config_race_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_claybin.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_broker.cpp
    # The gate owns the ONE table of host-trusted paths, and sandbox.cpp now
    # reads it to build the read-only binds (the prevention half). Linked here
    # so the live check drives the real list rather than a copy that can drift.
    ${CMAKE_SOURCE_DIR}/src/tool/util/handoff_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/sandbox_provenance.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/subprocess.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/utf8.cpp
    ${CMAKE_SOURCE_DIR}/src/util/logx.cpp
    ${CMAKE_SOURCE_DIR}/src/util/dbglog.cpp
    ${CMAKE_SOURCE_DIR}/src/util/home_dir.cpp
    ${CMAKE_SOURCE_DIR}/src/util/user_root.cpp
    ${CMAKE_SOURCE_DIR}/src/util/teardown.cpp)
target_include_directories(sandbox_live_check PRIVATE
    ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sandbox_live_check PRIVATE
    claybin nlohmann_json::nlohmann_json Threads::Threads)
if(TARGET maya::app)
    target_include_directories(sandbox_live_check SYSTEM PRIVATE
        $<TARGET_PROPERTY:maya::app,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
target_compile_definitions(sandbox_live_check PRIVATE
    AGENTTY_MCP=0 AGENTTY_VERSION="${PROJECT_VERSION}")

# ws_bind_probe: a hand-run answer to "is the workspace actually bound, and is
# the rest of $HOME actually read-only" -- driven through run_shell_command
# (the shipped path) rather than a hand-built posture, because a bug report
# claimed the two disagree. Same build shape and same rationale as
# sandbox_live_check above (needs real namespaces; not a ctest).
#
#     cmake --build build --target ws_bind_probe && ./build/ws_bind_probe <dir>
add_executable(ws_bind_probe EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/ws_bind_probe.cpp
    ${CMAKE_SOURCE_DIR}/tests/sandbox_config_race_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_claybin.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_broker.cpp
    # The gate owns the ONE table of host-trusted paths, and sandbox.cpp now
    # reads it to build the read-only binds (the prevention half). Linked here
    # so the live check drives the real list rather than a copy that can drift.
    ${CMAKE_SOURCE_DIR}/src/tool/util/handoff_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/sandbox_provenance.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/subprocess.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/utf8.cpp
    ${CMAKE_SOURCE_DIR}/src/util/logx.cpp
    ${CMAKE_SOURCE_DIR}/src/util/dbglog.cpp
    ${CMAKE_SOURCE_DIR}/src/util/home_dir.cpp
    ${CMAKE_SOURCE_DIR}/src/util/user_root.cpp
    ${CMAKE_SOURCE_DIR}/src/util/teardown.cpp)
target_include_directories(ws_bind_probe PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(ws_bind_probe PRIVATE
    claybin nlohmann_json::nlohmann_json Threads::Threads)
if(TARGET maya::app)
    target_include_directories(ws_bind_probe SYSTEM PRIVATE
        $<TARGET_PROPERTY:maya::app,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
target_compile_definitions(ws_bind_probe PRIVATE
    AGENTTY_MCP=0 AGENTTY_VERSION="${PROJECT_VERSION}")

# sandbox_audit: what does agentty ACTUALLY enforce on THIS host?
#
# Not a test -- it asserts nothing. It prints the real posture, capability by
# capability, with the mechanism behind each one. The point is that "is our
# sandbox any good" stops being a matter of opinion: every `none` in its output
# is either a deliberate trade or a gap, and the tool is what tells you which.
#
# Separate from sandbox_live_check because it answers a different question.
# The live check asks "does the wall we claim actually hold in the child"; this
# asks "which walls do we claim at all, and how strongly". A capability can
# pass the live check and still be `partial` here.
#
#     cmake --build build --target sandbox_audit && ./build/sandbox_audit
add_executable(sandbox_audit EXCLUDE_FROM_ALL
    ${CMAKE_SOURCE_DIR}/tests/sandbox_audit.cpp
    ${CMAKE_SOURCE_DIR}/tests/sandbox_config_race_stubs.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_claybin.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/sandbox_broker.cpp
    # The gate owns the ONE table of host-trusted paths, and sandbox.cpp now
    # reads it to build the read-only binds (the prevention half). Linked here
    # so the live check drives the real list rather than a copy that can drift.
    ${CMAKE_SOURCE_DIR}/src/tool/util/handoff_gate.cpp
    ${CMAKE_SOURCE_DIR}/src/domain/sandbox_provenance.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/subprocess.cpp
    ${CMAKE_SOURCE_DIR}/src/tool/util/utf8.cpp
    # logx: sandbox_claybin.cpp logs the broker's decisions, so the audit
    # needs the real logger rather than a stub -- a stubbed one would compile
    # and then not prove the audit trail exists.
    ${CMAKE_SOURCE_DIR}/src/util/logx.cpp
    ${CMAKE_SOURCE_DIR}/src/util/dbglog.cpp
    ${CMAKE_SOURCE_DIR}/src/util/home_dir.cpp
    ${CMAKE_SOURCE_DIR}/src/util/user_root.cpp
    ${CMAKE_SOURCE_DIR}/src/util/teardown.cpp)
target_include_directories(sandbox_audit PRIVATE ${CMAKE_SOURCE_DIR}/include)
target_link_libraries(sandbox_audit PRIVATE claybin nlohmann_json::nlohmann_json
    Threads::Threads)
if(TARGET maya::app)
    target_include_directories(sandbox_audit SYSTEM PRIVATE
        $<TARGET_PROPERTY:maya::app,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
target_compile_definitions(sandbox_audit PRIVATE
    AGENTTY_MCP=0 AGENTTY_VERSION="${PROJECT_VERSION}")

agentty_fold_test(fork_test                TIMEOUT 30)
agentty_fold_test(palette_render_probe     TIMEOUT 30)
agentty_fold_test(embed_render_probe       TIMEOUT 30)
agentty_fold_test(form_edit_nav_test       TIMEOUT 30)
agentty_fold_test(login_render_probe       TIMEOUT 30 ARGS)
agentty_fold_test(settings_add_render_probe TIMEOUT 30 ARGS)
agentty_fold_test(thread_delete_test       TIMEOUT 30)
# Pins that every threaded subsystem registers its OWN join, instead of
# main() keeping a hand-written list of them. That list already lost one
# (settings_cache's worker sat joinable in a static until ~std::thread
# terminated the process); this fails loudly instead.
agentty_fold_test(teardown_registration_test TIMEOUT 60)
# seam_stress_test: the process-wide seams (provider selection, the tool
# catalog, the capability registry) driven from many threads at once. NOT
# labelled `race` — it folds, and a label inside the fold drags all 375 TUs
# into the TSan lane (see persistence_race_test below). The TSan lane gets
# it through the narrow target instead.
agentty_fold_test(seam_stress_test TIMEOUT 120)
# Pins that a resumed thread leaves real history in native scrollback: the
# rehydrate seed is wider than the live canvas, and the post-paint trim
# COMMITS the difference rather than dropping it. Before this split, a
# thread switch showed one turn with nothing scrollable above it.
agentty_fold_test(rehydrate_scrollback_test TIMEOUT 60)
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
# ARGS + no-op when the corpus is absent, so CI passes trivially; the value
# is running it BY HAND before trusting the migration with history.
#
# LABELS perf: on a machine that HAS a real corpus this walks the whole thing
# (measured: ~75 s for 609 threads / 594 MB). RUN_SERIAL follows from perf and
# is what keeps a slow probe from starving faster tests at -j8 — without it
# the probe blew through the 60 s ctest run budget and cascaded 15 later
# tests into Not Run.
agentty_fold_test(thread_log_corpus_probe TIMEOUT 300 ARGS LABELS perf)
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
# it is registered in agentty_standalone_tests.def alongside its siblings.
#
# NOT labelled `race`: that label is what the TSan CI lane selects on, and
# this being the only `race` test inside the fold forced that lane to build
# all 375 TUs of agentty_standalone_tests under -fsanitize=thread to run one
# test. persistence_race_test_narrow (below) is the same source built from
# the ~11 TUs it actually needs, and carries the `race` label instead. This
# entry still runs uninstrumented in the normal suite, which is where its
# non-race assertions (flush drains, late writes land) belong anyway.
agentty_fold_test(persistence_race_test TIMEOUT 180)
# thread_index: the mtime cache actually caches. Folded for the same reason
# as persistence_race — it owns AGENTTY_HOME and needs the io object set.
agentty_fold_test(thread_index_test TIMEOUT 60)
agentty_fold_test(theme_preview_cost_probe TIMEOUT 120 NO_TEST)

# agents_md_test — locks wire::agents_md_block (AAIF AGENTS.md standard).
# checkpoint_test — git-backed worktree snapshots.
#
# Both FOLDED. They were their own binaries on the grounds that they chdir()
# into temp workspaces, but the fold already runs each entry as a separate
# PROCESS (agentty_standalone_tests <name>), so a chdir is no more shared
# than it was before — and ten already-folded tests do exactly the same
# thing (skills_engine, toolset_e2e, workspace_index, ...). Two 531 MB links
# for that was the last of the per-test link cost the fold exists to remove.
agentty_fold_test(agents_md_test  TIMEOUT 30)
agentty_fold_test(checkpoint_test TIMEOUT 60)

# Build the one binary: union of every folded test's extra objs/libs.
agentty_finalize_fold(
    OBJS $<TARGET_OBJECTS:agentty_acp_obj>
    LIBS acp::acp)

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

# subagent_lifetime: the provider seam must survive an ABANDONED worker.
#
# Subagents run on Cmd::task_isolated threads, which jaal detaches and never
# waits for; shutdown's wait is bounded on purpose, so abandonment is a
# DESIGNED outcome. The providers therefore cannot be stack objects captured
# by reference (they were) — an abandoned worker mid-stream called into freed
# stack while main unwound, which is the intermittent "subagents crash"
# report. Ownership is the fix; this pins it.
#
# ASan lane because the broken shape is a hard stack-use-after-scope there
# and merely probable elsewhere. Narrow-source like its neighbours: no maya
# renderer to ODR-clash with the instrumented build.
agentty_test(subagent_lifetime_test MODE raw LABELS sanitizer)
add_executable(subagent_lifetime_test EXCLUDE_FROM_ALL
    tests/subagent_lifetime_test.cpp)
target_include_directories(subagent_lifetime_test PRIVATE include)
add_test(NAME subagent_lifetime_test COMMAND subagent_lifetime_test)
set_tests_properties(subagent_lifetime_test PROPERTIES TIMEOUT 30 LABELS sanitizer)

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

# real_subsystem_race_test: the same TSan lane, but driving the REAL shared
# seams instead of hand-written models of them. race_harness_test exercises a
# model of the snapshot cell / teardown registry / write-behind queue, which
# catches a mistake in the PATTERN; this one links the actual translation
# units, which catches a mistake in the CODE. The two drift apart otherwise,
# and it is the code that ships.
agentty_test(real_subsystem_race_test MODE raw LABELS sanitizer)
add_executable(real_subsystem_race_test EXCLUDE_FROM_ALL
    tests/real_subsystem_race_test.cpp src/util/teardown.cpp
    src/util/logx.cpp src/util/dbglog.cpp)
target_include_directories(real_subsystem_race_test PRIVATE include)
add_test(NAME real_subsystem_race_test COMMAND real_subsystem_race_test)
set_tests_properties(real_subsystem_race_test PROPERTIES TIMEOUT 120 LABELS "sanitizer;race")

# persistence_race_test_narrow — the SAME test as the folded
# `persistence_race_test` entry, built from the ~11 TUs it actually needs
# instead of the whole 375-TU standalone fold.
#
# Why: it is the only `race`-labelled test living in agentty_standalone_tests,
# so the TSan CI lane was compiling that entire binary — every provider, the
# ACP server, the RAG adapter, all of maya — under -fsanitize=thread to run
# ONE test. TSan roughly doubles compile time, so that was the single most
# expensive thing in CI, and none of it was instrumented code the test reads.
#
# Registered under `race` only. The folded entry keeps the `sanitizer` label
# for the asan lane (which builds that binary anyway for its own reasons), so
# coverage is identical and neither lane loses a check.
agentty_test(persistence_race_test_narrow MODE raw LABELS race)
add_executable(persistence_race_test_narrow EXCLUDE_FROM_ALL
    tests/persistence_race_test.cpp tests/persistence_race_stubs.cpp
    src/io/persistence.cpp src/io/blob_store.cpp src/io/thread_log.cpp
    src/runtime/settings_registry.cpp src/tool/util/utf8.cpp
    src/util/logx.cpp src/util/dbglog.cpp src/util/home_dir.cpp
    src/util/user_root.cpp src/util/base64.cpp src/util/teardown.cpp)
target_include_directories(persistence_race_test_narrow PRIVATE include)
# jaal's headers: user_root.cpp uses jaal::guarded for its warn-once set (the
# concurrency banlist forbids a raw std::mutex). Header-only here -- guarded<T>
# is all inline -- so the include path is enough and nothing new is linked.
# Kept as a bare include rather than linking maya::maya on purpose: this target
# exists to keep the TSan lane at ~11 TUs instead of 375.
target_include_directories(persistence_race_test_narrow PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party/maya/third_party/jaal/include)
target_link_libraries(persistence_race_test_narrow PRIVATE
    nlohmann_json::nlohmann_json simdjson::simdjson Threads::Threads)
target_compile_definitions(persistence_race_test_narrow PRIVATE
    AGENTTY_MCP=0 AGENTTY_VERSION="${PROJECT_VERSION}")
add_test(NAME persistence_race_test_narrow COMMAND persistence_race_test_narrow)
set_tests_properties(persistence_race_test_narrow PROPERTIES TIMEOUT 120 LABELS "race")

# sandbox_config_race_test — publishing the sandbox policy while tools read it.
#
# Narrow on purpose, same reasoning as persistence_race_test_narrow above: it
# needs sandbox.cpp and the handful of utils it pulls, not the 375-TU fold,
# so the TSan lane instruments ~6 TUs to check the thing that was actually
# racy.
#
# What it guards: the settings pane made the sandbox config a
# published-at-runtime value, and it had been a plain global on a "set once
# before maya starts" premise. Assigning a struct of three vectors out from
# under a worker thread is a use-after-free with a very narrow window — the
# kind that ships and then crashes on someone else's machine.
agentty_test(sandbox_config_race_test MODE raw LABELS race)
add_executable(sandbox_config_race_test EXCLUDE_FROM_ALL
    tests/sandbox_config_race_test.cpp tests/test_main.cpp
    tests/sandbox_config_race_stubs.cpp
    src/tool/util/sandbox.cpp src/tool/util/subprocess.cpp
    src/tool/util/sandbox_claybin.cpp
    src/tool/util/sandbox_broker.cpp
    src/domain/sandbox_provenance.cpp
    src/tool/util/utf8.cpp
    src/util/logx.cpp src/util/dbglog.cpp src/util/home_dir.cpp
    src/util/user_root.cpp src/util/teardown.cpp)
target_include_directories(sandbox_config_race_test PRIVATE include)
target_link_libraries(sandbox_config_race_test PRIVATE
    doctest::doctest nlohmann_json::nlohmann_json Threads::Threads claybin)
# maya headers only (sandbox.cpp's transitive includes reach scroll_state /
# anim_clock); no maya linking, so the TSan build stays small.
if(TARGET maya::app)
    target_include_directories(sandbox_config_race_test SYSTEM PRIVATE
        $<TARGET_PROPERTY:maya::app,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
target_compile_definitions(sandbox_config_race_test PRIVATE
    AGENTTY_MCP=0 AGENTTY_VERSION="${PROJECT_VERSION}")
add_test(NAME sandbox_config_race_test COMMAND sandbox_config_race_test)
set_tests_properties(sandbox_config_race_test PROPERTIES TIMEOUT 120 LABELS "race")

# NOTE: seam_stress_test gets no narrow TSan target, on purpose.
#
# The seams it drives — provider::select/active, the tool catalog, the
# capability registry — are the ones the whole app converges on, so they
# transitively need auth, accounts, the keystore, cred_crypt, the vault and
# the HTTP client. A "narrow" build of it is most of the fold with extra
# steps, and the stub file needed to fake that chain would be large enough
# to be its own maintenance hazard (and would fake exactly the code whose
# locking is under test).
#
# So it runs UNINSTRUMENTED in the normal suite, where its assertions (no
# torn Selection, no malformed snapshot) still hold, and TSan coverage of
# these seams comes from real_subsystem_race_test, which drives the same
# publish/consume shapes through narrow TUs. If that stops feeling like
# enough, the move is to give the TSan lane the fold and accept the cost —
# not to stub out the subsystem being tested.


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

# clipboard display-server discovery: guards the fix for image paste failing
# inside tmux (a pane inherits the tmux SERVER's env, so WAYLAND_DISPLAY is
# missing and the wl-paste backend used to be skipped). Standalone and
# self-contained — it builds a FAKE runtime dir, so it passes on a headless
# CI box with no compositor, and needs no agentty objects.
agentty_test(clipboard_display_env_test MODE raw)
add_executable(clipboard_display_env_test EXCLUDE_FROM_ALL
    tests/clipboard_display_env_test.cpp)
target_include_directories(clipboard_display_env_test PRIVATE include)
add_test(NAME clipboard_display_env_test COMMAND clipboard_display_env_test)
set_tests_properties(clipboard_display_env_test PROPERTIES TIMEOUT 30)

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
    # AGENTTY_HOME, not the retired AGENTTY_LOG_FILE: the destination is
    # `--log-file` now, and these binaries are doctest mains that never see
    # agentty's argv parser. Pointing the user root at the build dir makes
    # the sink resolve to <root>/logs/agentty.log, which is what the tests
    # read back through logx::log_file().
    add_test(NAME ${_logx_t} COMMAND ${_logx_t})
    set_tests_properties(${_logx_t} PROPERTIES TIMEOUT 30
        ENVIRONMENT "AGENTTY_LOG=trace;AGENTTY_HOME=${CMAKE_CURRENT_BINARY_DIR}/${_logx_t}.home")
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
    ENVIRONMENT "AGENTTY_LOG=trace;AGENTTY_HOME=${CMAKE_CURRENT_BINARY_DIR}/logx_rotation_test.home")

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

# ── Loop affinity: agentty's loop-only state really is loop-only ───────────
#
# jaal's own tests prove loop_bound<T> behaves. They say nothing about whether
# THIS tree uses it, and that gap is where the guarantee used to die: jaal
# shipped loop_bound, and the number of them in jaal + maya + agentty combined
# was zero, because the only way to get a token was to forge one. This test
# pins the consumer half — the forge is shut, agentty's render caches are
# reachable on the loop, and a worker that reaches for one ABORTS instead of
# painting from its own empty copy.
#
# raw/standalone, not consolidated: the last case asserts a SIGABRT in a
# forked child, which doctest cannot host. Header-only link (jaal + the test)
# so it stays a ~3 s compile.
agentty_test(loop_affinity_test MODE raw)
add_executable(loop_affinity_test EXCLUDE_FROM_ALL tests/loop_affinity_test.cpp)
target_include_directories(loop_affinity_test PRIVATE include)
# jaal directly, NOT maya: this test is about the runtime's loop-affinity
# guarantee, so it should not need a terminal framework to link. maya does not
# re-export jaal's include dir anyway.
target_link_libraries(loop_affinity_test PRIVATE jaal::jaal)
add_test(NAME loop_affinity_test COMMAND loop_affinity_test)
set_tests_properties(loop_affinity_test PROPERTIES TIMEOUT 60 LABELS sanitizer)

# Single-root layout + legacy ~/.config/agentty migration. Narrow link —
# user_root.cpp + home_dir.cpp only — so the sandboxed $HOME manipulation
# can't interact with any other subsystem's statics.
agentty_test(user_root_test MODE raw)
add_executable(user_root_test EXCLUDE_FROM_ALL
    tests/user_root_test.cpp src/util/user_root.cpp src/util/home_dir.cpp)
target_include_directories(user_root_test PRIVATE include)
add_test(NAME user_root_test COMMAND user_root_test)
set_tests_properties(user_root_test PROPERTIES TIMEOUT 30)

# The WRITE-side storage primitive (agentty::dirs), peer of scope. Same
# narrow-link reasoning as user_root_test: this test chdir()s and mutates
# $HOME, so it links only the TUs it exercises rather than dragging in a
# subsystem whose statics could observe either.
#
# logx is included because dirs.cpp instruments its resolutions; fs_helpers
# for project_root()/set_workspace_root(), which the anchor walk starts from.
agentty_test(dirs_test MODE raw)
add_executable(dirs_test EXCLUDE_FROM_ALL
    tests/dirs_test.cpp
    src/dirs/dirs.cpp
    src/util/user_root.cpp
    src/util/home_dir.cpp
    src/util/logx.cpp
    src/tool/util/fs_helpers.cpp)
target_include_directories(dirs_test PRIVATE include)
# Same transitive deps keystore_test needs for the same reason: logx pulls
# nlohmann + maya, and fs_helpers.cpp includes mcp-cpp's util header to
# mirror the workspace root into it.
target_link_libraries(dirs_test PRIVATE maya::maya nlohmann_json::nlohmann_json)
if(TARGET mcp::tools)
    target_link_libraries(dirs_test PRIVATE mcp::tools)
endif()
add_test(NAME dirs_test COMMAND dirs_test)
set_tests_properties(dirs_test PROPERTIES TIMEOUT 30)

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

# ── Concurrency ban-list ───────────────────────────────────────────
# Raw threads, locks, atomics and thread_locals only where a human signed
# off that they ARE the implementation of a safe type. Everything else goes
# through jaal (a task instead of a thread, guarded<T> instead of a mutex,
# loop_bound<T> instead of a thread_local).
#
# This is jaal's own check (third_party/maya/third_party/jaal/tests/lint/banlist.cmake)
# pointed at agentty. jaal runs it over ITS tree, which says nothing about a
# consumer: agentty could spawn a bare thread beside the loop or park
# reducer state in a thread_local and jaal would never notice. Two roots
# because the allowlist keys on paths relative to the root it scans.
#
# Cost: a file read, no compile. Adding an entry is the deliberate act — it
# is a claim that someone checked the use by hand.
set(_BANLIST ${CMAKE_SOURCE_DIR}/third_party/maya/third_party/jaal/tests/lint/banlist.cmake)
if(EXISTS ${_BANLIST})
    add_test(NAME concurrency_banlist_src
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_SOURCE_DIR}/src
                     -DALLOW=${CMAKE_SOURCE_DIR}/tests/lint/allowlist.txt
                     -P ${_BANLIST})
    add_test(NAME concurrency_banlist_include
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_SOURCE_DIR}/include
                     -DALLOW=${CMAKE_SOURCE_DIR}/tests/lint/allow_include.txt
                     -P ${_BANLIST})
    set_tests_properties(concurrency_banlist_src concurrency_banlist_include
                         PROPERTIES LABELS "static")
endif()

# ── util layering ─────────────────────────────────────────────────
# Two `util` namespaces exist (tools::util = the tool boundary, util = app-wide
# plumbing) and the ONE-WAY dependency is what keeps the collision tolerable.
# sail3r flagged the pair as hard to tell apart; the headers now explain the
# split, and this makes the rule a build error rather than a convention.
add_test(NAME util_layering
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/lint/util_layering.cmake)
set_tests_properties(util_layering PROPERTIES LABELS "static")

# ── project dotdir ─────────────────────────────────────────────
# The user root sorted itself into cache/ credentials/ logs/ threads/; the
# project root drifted to thirteen flat entries across four lifecycles. Each
# arrived legitimately one commit at a time, which is why a reviewer misses the
# next one. This makes the top level a declared set so the category is a
# decision rather than an accident. docs/design/dot-agentty.md.
add_test(NAME project_dotdir
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/lint/project_dotdir.cmake)
set_tests_properties(project_dotdir PROPERTIES LABELS "static")

# ── secret file modes ───────────────────────────────────────────
# credentials/ is the one directory where a 0600 audit should find no
# exceptions. accounts.json was 0644 among seven 0600 siblings because
# std::ofstream creates at 0666 & ~umask and nothing said otherwise. Contained
# by the 0700 directory, and still worth a structural guard: that containment
# is one chmod away from being the only thing left.
add_test(NAME secret_modes
         COMMAND ${CMAKE_COMMAND} -DROOT=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/lint/secret_modes.cmake)
set_tests_properties(secret_modes PROPERTIES LABELS "static")

# ── i18n catalog ───────────────────────────────────────────────────────────
# Three gates: every t() id exists in the English catalog, every catalog id is
# used, and nothing in src/provider or src/tool translates (model-facing
# strings stay English -- docs/design/i18n.md section 3).
#
# The first is what makes the string sweep reviewable: "did we drop one" is
# not a question a human can answer across hundreds of moved literals, and a
# missing id renders as the id on screen rather than failing loudly.
#
# Same shape and cost as the banlist above: a text scan, no compile.
add_test(NAME i18n_catalog_lint
         COMMAND ${CMAKE_COMMAND}
                 -DROOT=${CMAKE_SOURCE_DIR}
                 -P ${CMAKE_SOURCE_DIR}/tests/lint/i18n_lint.cmake)
set_tests_properties(i18n_catalog_lint PROPERTIES LABELS "static")

# ── Allowlist rot ──────────────────────────────────────────────────────────
# The banlist walks the files on disk and only ever LOOKS UP the allowlist, so
# a grant whose justification is gone is silently ignored rather than reported.
# That rots one way: the exemption outlives the code, and the next person to add
# a std::mutex to that file inherits a pass nobody granted them. Measured when
# this was added: 4 entries named files that no longer existed and 28 granted
# primitives the file had stopped using -- including provider/ollama/provider.cpp,
# renamed to transport.cpp long enough ago that the successor uses neither of
# the two primitives its entry still allowed.
#
# Cost is a file read, no compile. Narrowing a grant is the cheap half of
# keeping the banlist honest.
set(_PRUNE ${CMAKE_SOURCE_DIR}/tests/lint/prune_allowlist.cmake)
if(EXISTS ${_PRUNE})
    add_test(NAME allowlist_tight_src
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_SOURCE_DIR}/src
                     -DALLOW=${CMAKE_SOURCE_DIR}/tests/lint/allowlist.txt
                     -P ${_PRUNE})
    add_test(NAME allowlist_tight_include
             COMMAND ${CMAKE_COMMAND}
                     -DROOT=${CMAKE_SOURCE_DIR}/include
                     -DALLOW=${CMAKE_SOURCE_DIR}/tests/lint/allow_include.txt
                     -P ${_PRUNE})
    set_tests_properties(allowlist_tight_src allowlist_tight_include
                         PROPERTIES LABELS "static")
endif()

# local_stub_claims: does tests/fake_local_server.py still serve every shape
# tests/verify_stub_against_real.py claims a real server sends?
#
# Those two files are the only thing standing between us and a repeat of the
# LM Studio bug, where the reader required is_boolean() on a field the server
# sends as an object and every model silently fell through to guessing from
# its filename. The stub is where we write down what we believe each local
# server returns; CLAIMS is where we write down what to go check against a
# real one.
#
# They drifted, in the direction that hides: the stub's lmstudio mode had no
# capability object at all, so the reader that commit added never executed in
# any test, and CLAIMS never mentioned the field either. Nothing failed. The
# feature simply had no coverage and looked like it did.
#
# This asks the weaker of the two questions -- does the stub serve what we
# claim -- because it is the half that needs no hardware. It cannot tell you
# the claim matches reality; only pointing the script at a real llama.cpp or
# LM Studio does that. What it does buy: adding a shape to the stub without
# stating the claim (or vice versa) now fails here instead of years later.
find_package(Python3 COMPONENTS Interpreter QUIET)
set(_STUBCHK ${CMAKE_SOURCE_DIR}/tests/verify_stub_against_real.py)
if(Python3_Interpreter_FOUND AND EXISTS ${_STUBCHK})
    add_test(NAME local_stub_claims
             COMMAND ${Python3_EXECUTABLE} ${_STUBCHK} --self-check)
    # Binds loopback ports to run the stub, so keep it off the parallel
    # sanitizer lanes and give it room for five sequential server starts.
    set_tests_properties(local_stub_claims PROPERTIES
                         LABELS "static" TIMEOUT 120 RUN_SERIAL TRUE)
endif()

# packaging_submodule_parity: does the AUR source recipe know every submodule?
#
# packaging/arch/agentty-git/PKGBUILD maintains its submodule list BY HAND --
# makepkg forbids network access in build(), so each one is fetched as its own
# source=() entry and rewired to that local clone in prepare(). A hand-kept
# list does not track .gitmodules, so adding a submodule silently breaks the
# package: claybin landed, nothing updated the recipe, and because an empty
# third_party/claybin is a CMake FATAL_ERROR the package stopped CONFIGURING.
# No workflow referenced the file, so nothing noticed until someone tried it
# by hand.
#
# CI builds the recipe for real (the `AUR agentty-git configures` job) and
# that remains the authoritative check. This is the cheap half: a text
# comparison that runs in the normal suite and names exactly what to add, so
# the next submodule fails here in milliseconds rather than on the AUR.
set(_PKGPARITY ${CMAKE_SOURCE_DIR}/tests/packaging_submodule_parity.py)
if(Python3_Interpreter_FOUND AND EXISTS ${_PKGPARITY})
    add_test(NAME packaging_submodule_parity
             COMMAND ${Python3_EXECUTABLE} ${_PKGPARITY})
    set_tests_properties(packaging_submodule_parity PROPERTIES
                         LABELS "static" TIMEOUT 30)
endif()
