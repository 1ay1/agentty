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
    custom_host_key_prompt_test decomposition_memory_test dispatch_route_test
    model_label_test cache_anchor_test composer_edit_test hooks_gate_test
    midrun_freeze_test smart_mode_test stream_liveness_test wire_golden_test
    wire_shared_test complexity_test copilot_token_test routing_memory_test
    smart_cascade_gate_test chatgpt_bundled_models_test settings_default_test
    update_check_test
    workspace_index_test
    dup_tool_call_id_test salvage_dedup_test compaction_wire_test
    plugins_in_model_test tool_stream_snapshot_test tool_timeline_adapter_test
    anthropic_sse_golden_test codex_login_flow_test mcp_reload_race_test
    persistence_proactive_test proactive_deferred_test rag_adapter_test
    scheduler_path_test tool_result_budget_test tool_wedge_liveness_test
    transcript_bound_test turn_settle_test midrun_seam_test midrun_wire_test
    codex_responses_test doom_loop_test visual_hash_coverage_test
    ollama_transport_test openai_transport_test code_block_extract_test
    command_palette_test compaction_threshold_test fsm_test model_caps_test
    param_tag_repair_test sandbox_escape_test scope_test table_render_test
    plugin_config_test skills_engine_test slash_commands_test fuzzy_match_smoke
    provider_model_switch_test speculative_dispatch_test)
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
agentty_fold_test(fork_test                TIMEOUT 30)
agentty_fold_test(reveal_freeze_gate_probe TIMEOUT 30)
agentty_fold_test(toolset_e2e_test         TIMEOUT 120)
agentty_fold_test(subagent_report_test     TIMEOUT 60)
agentty_fold_test(plugin_disabled_tools_test TIMEOUT 60)
agentty_fold_test(frozen_invariant_fuzz)
agentty_fold_test(scrollback_wire_fuzz     TIMEOUT 120)
agentty_fold_test(reveal_scrollback_test   TIMEOUT 180 UNIX_LIBS util)
agentty_fold_test(scrollback_oracle_test   TIMEOUT 600 UNIX_LIBS util)
agentty_fold_test(external_acp_backend_test TIMEOUT 60)
agentty_fold_test(md_shape_sweep           TIMEOUT 120)
agentty_fold_test(md_cache_probe           TIMEOUT 120)
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
agentty_finalize_fold(
    OBJS $<TARGET_OBJECTS:agentty_acp_obj>
    LIBS acp::acp)

# agents_md_test — locks wire::agents_md_block (AAIF AGENTS.md standard).
# Kept as its OWN binary: it chdir()s into temp workspaces, and it's new enough
# that folding it hasn't been validated.
agentty_test(agents_md_test          MODE standalone TIMEOUT 30)
agentty_test(checkpoint_test         MODE standalone TIMEOUT 60)

# ── Narrow-source sanitizer tests (raw: must NOT link the full shared set) ──
# They exercise agentty's own logic and link cleanly under asan/ubsan without
# pulling maya's un-instrumented renderer. Registered raw + marked sanitizer.
agentty_test(concurrency_primitives_test MODE raw LABELS sanitizer)
add_executable(concurrency_primitives_test EXCLUDE_FROM_ALL
    tests/concurrency_primitives_test.cpp src/util/dbglog.cpp)
target_include_directories(concurrency_primitives_test PRIVATE include)
add_test(NAME concurrency_primitives_test COMMAND concurrency_primitives_test)
set_tests_properties(concurrency_primitives_test PROPERTIES TIMEOUT 30 LABELS sanitizer)

agentty_test(cred_crypt_test MODE raw LABELS sanitizer)
add_executable(cred_crypt_test EXCLUDE_FROM_ALL
    tests/cred_crypt_test.cpp src/io/cred_crypt.cpp src/util/base64.cpp)
target_include_directories(cred_crypt_test PRIVATE include)
target_link_libraries(cred_crypt_test PRIVATE
    nlohmann_json::nlohmann_json OpenSSL::SSL OpenSSL::Crypto)
add_test(NAME cred_crypt_test COMMAND cred_crypt_test)
set_tests_properties(cred_crypt_test PROPERTIES TIMEOUT 60 LABELS sanitizer)

agentty_test(keystore_test MODE raw LABELS sanitizer)
add_executable(keystore_test EXCLUDE_FROM_ALL
    tests/keystore_test.cpp src/io/keystore.cpp src/tool/util/subprocess.cpp
    src/tool/util/fs_helpers.cpp src/tool/util/utf8.cpp src/tool/progress.cpp)
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
