# tests/lint/layering.cmake — agentty → maya → jaal, strictly.
#
# agentty depends on maya and never on jaal: every runtime piece it uses
# reaches it as a maya:: name from <maya/runtime.hpp>. docs/LAYERING.md.
#
# Fails on, in agentty's src/ include/ tests/ cmake/:
#   * #include <jaal/...>
#   * a jaal:: name or a JAAL_ macro in code (comments and strings may say it)
# and one layer down:
#   * maya naming agentty (maya is a library agentty uses, not the reverse)
#   * jaal naming maya or agentty
#
# Run with: cmake -DROOT=<agentty source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

# scan(<dirs> <word-regex> <include-regex> <label>): appends to `hits`.
function(scan dirs word inc label)
    set(out "")
    foreach(dir IN LISTS dirs)
        file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h
                                ${ROOT}/${dir}/*.cmake ${ROOT}/${dir}/CMakeLists.txt)
        foreach(f IN LISTS files)
            if(f MATCHES "/tests/lint/" OR f MATCHES "/_deps/" OR f MATCHES "/build[^/]*/")
                continue()
            endif()
            file(STRINGS ${f} lines REGEX "${word}|${inc}")
            if(NOT lines)
                continue()
            endif()
            file(RELATIVE_PATH rel ${ROOT} ${f})
            foreach(l IN LISTS lines)
                # Drop // and # comments, then check includes on that (an
                # "..." include path is itself a string literal), then blank
                # string literals and look for real uses of the names.
                string(REGEX REPLACE "//.*$" "" code "${l}")
                if(f MATCHES "\\.cmake$|CMakeLists\\.txt$")
                    string(REGEX REPLACE "#.*$" "" code "${code}")
                endif()
                set(is_inc FALSE)
                if(code MATCHES "${inc}")
                    set(is_inc TRUE)
                endif()
                string(REGEX REPLACE "\"[^\"]*\"" "\"\"" code "${code}")
                if(is_inc OR code MATCHES "(^|[^A-Za-z0-9_])(${word})")
                    string(STRIP "${l}" s)
                    list(APPEND out "${label}: ${rel}: ${s}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(hits ${hits} ${out} PARENT_SCOPE)
endfunction()

set(hits "")
scan("src;include;tests;cmake" "jaal::|JAAL_" "#[ \t]*include[ \t]*[<\"]([^>\"]*/)?jaal/" "agentty->jaal")
scan("third_party/maya/include;third_party/maya/src" "agentty::" "#[ \t]*include[ \t]*[<\"]([^>\"]*/)?agentty/" "maya->agentty")
scan("third_party/maya/third_party/jaal/include;third_party/maya/third_party/jaal/src"
     "maya::|agentty::" "#[ \t]*include[ \t]*[<\"]([^>\"]*/)?(maya|agentty)/" "jaal->up")

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "agentty reaches past maya into jaal:\n  ${msg}\n\n"
        "Use the maya:: name from <maya/runtime.hpp>. If maya doesn't expose "
        "it yet, add it there (docs/LAYERING.md).")
endif()
message(STATUS "layering ok: agentty uses the runtime only through maya")
