# tests/lint/submodule_runtime.cmake — the submodules own no runtime.
#
# docs/LAYERING.md rule 5: mcp-cpp, acp-cpp, rag-cpp and claybin are
# libraries. They start no threads of their own and run nothing in the
# background; what they need, agentty provides through maya → jaal.
#
# Runs jaal's concurrency ban-list over each library's src/ and include/
# against the library's own tests/lint/concurrency_allowlist.txt (paths
# relative to the library root), and prune_allowlist.cmake over the same, so
# a grant nobody needs fails too.
#
# Run with: cmake -DROOT=<agentty source dir> -P submodule_runtime.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(banlist ${ROOT}/third_party/maya/third_party/jaal/tests/lint/banlist.cmake)
set(failed "")

foreach(lib mcp-cpp acp-cpp rag-cpp claybin)
    set(libdir ${ROOT}/third_party/${lib})
    set(allow ${libdir}/tests/lint/concurrency_allowlist.txt)
    if(NOT EXISTS ${allow})
        list(APPEND failed "${lib}: no tests/lint/concurrency_allowlist.txt")
        continue()
    endif()
    file(STRINGS ${allow} lines)
    foreach(sub src include)
        if(NOT IS_DIRECTORY ${libdir}/${sub})
            continue()
        endif()
        # The library's allowlist names paths from its root; the ban-list
        # wants them relative to the directory it scans.
        set(scoped "")
        foreach(l IN LISTS lines)
            if(l MATCHES "^${sub}/(.*)$")
                string(APPEND scoped "${CMAKE_MATCH_1}\n")
            endif()
        endforeach()
        set(tmp ${CMAKE_CURRENT_BINARY_DIR}/submodule_runtime_${lib}_${sub}.txt)
        file(WRITE ${tmp} "${scoped}")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -DROOT=${libdir}/${sub} -DALLOW=${tmp}
                    -DTIGHT=1 -P ${banlist}
            RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(NOT rc EQUAL 0)
            list(APPEND failed "${lib}/${sub}:\n${out}")
        endif()
        # And no grant outlives the code that needed it.
        execute_process(
            COMMAND ${CMAKE_COMMAND} -DROOT=${libdir}/${sub} -DALLOW=${tmp}
                    -P ${ROOT}/tests/lint/prune_allowlist.cmake
            RESULT_VARIABLE rc OUTPUT_VARIABLE out ERROR_VARIABLE out)
        if(NOT rc EQUAL 0)
            list(APPEND failed "${lib}/${sub} (stale grants):\n${out}")
        endif()
    endforeach()
endforeach()

if(failed)
    list(JOIN failed "\n" msg)
    message(FATAL_ERROR
        "a submodule owns runtime it shouldn't (docs/LAYERING.md rule 5):\n${msg}")
endif()
message(STATUS "submodules own no runtime: mcp-cpp acp-cpp rag-cpp claybin")
