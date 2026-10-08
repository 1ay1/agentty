# tests/lint/layering.cmake — agentty → maya → jaal, strictly.
#
# agentty depends on maya and never on jaal: every runtime piece it uses
# reaches it as a maya:: name from <maya/runtime.hpp>. docs/LAYERING.md.
#
# Fails on, in agentty's src/ include/ tests/:
#   * #include <jaal/...>
#   * a jaal:: name in code (comments and string literals may mention jaal)
#
# Run with: cmake -DROOT=<agentty source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

set(hits "")
foreach(dir src include tests)
    file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h)
    foreach(f IN LISTS files)
        file(STRINGS ${f} lines REGEX "jaal")
        if(NOT lines)
            continue()
        endif()
        file(RELATIVE_PATH rel ${ROOT} ${f})
        foreach(l IN LISTS lines)
            # Drop // comments and string literals, then look for real uses.
            string(REGEX REPLACE "//.*$" "" code "${l}")
            string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
            if(code MATCHES "#[ \t]*include[ \t]*<jaal/" OR code MATCHES "(^|[^A-Za-z0-9_])jaal::")
                string(STRIP "${l}" s)
                list(APPEND hits "${rel}: ${s}")
            endif()
        endforeach()
    endforeach()
endforeach()

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "agentty reaches past maya into jaal:\n  ${msg}\n\n"
        "Use the maya:: name from <maya/runtime.hpp>. If maya doesn't expose "
        "it yet, add it there (docs/LAYERING.md).")
endif()
message(STATUS "layering ok: agentty uses the runtime only through maya")
