# tests/lint/layering.cmake — agentty → maya → jaal, strictly.
#
# agentty depends on maya and never on jaal: every runtime piece it uses
# reaches it as a maya:: name from <maya/runtime.hpp>. docs/LAYERING.md.
#
# Fails on, in agentty's src/ include/ tests/:
#   * #include <jaal/...>
#   * the word jaal anywhere in code: jaal::x, `jaal :: x`, `using namespace
#     jaal;`, `namespace j = jaal;`
#   * a JAAL_ macro
# maya and jaal check their own direction with their own layering.cmake;
# agentty runs those too (layering_maya, layering_jaal).
#
# Comments and string literals may say anything. They are stripped per file,
# block comments included, so prose that wraps onto a line by itself is not
# mistaken for code.
#
# cmake/ is not scanned: the build is the one place that has to name jaal's
# tree, to run jaal's own lints over agentty.
#
# Run with: cmake -DROOT=<agentty source dir> -P layering.cmake

if(NOT DEFINED ROOT)
    message(FATAL_ERROR "ROOT is required")
endif()

# The code of one C++ file with comments and string/char literals blanked,
# newlines kept so line numbers still match. Regex passes, not a char loop:
# a CMake loop over every character took most of a minute on jaal's tree.
function(code_of path out)
    file(READ "${path}" s)
    # Literals first, so a // or /* inside one isn't taken for a comment.
    string(REGEX REPLACE "R\"([^(]*)\\([^)]*\\)[^\"]*\"" "\"\"" s "${s}")
    string(REGEX REPLACE "\"([^\"\\\\\n]|\\\\.)*\"" "\"\"" s "${s}")
    string(REGEX REPLACE "'([^'\\\\\n]|\\\\.)*'" "''" s "${s}")
    string(REGEX REPLACE "//[^\n]*" "" s "${s}")
    # /* ... */ may span lines: replace each block with as many newlines.
    while(s MATCHES "/\\*")
        string(FIND "${s}" "/*" a)
        string(SUBSTRING "${s}" ${a} -1 tail)
        string(FIND "${tail}" "*/" b)
        if(b EQUAL -1)
            set(b_len -1)
            set(body "${tail}")
        else()
            math(EXPR b_len "${b} + 2")
            string(SUBSTRING "${tail}" 0 ${b_len} body)
        endif()
        string(REGEX REPLACE "[^\n]" "" nl "${body}")
        string(SUBSTRING "${s}" 0 ${a} head)
        if(b_len EQUAL -1)
            set(s "${head}${nl}")
        else()
            string(SUBSTRING "${tail}" ${b_len} -1 rest)
            set(s "${head}${nl}${rest}")
        endif()
    endwhile()
    set(${out} "${s}" PARENT_SCOPE)
endfunction()

# scan(<dirs> <word-regex> <include-regex> <label>): appends to `hits`.
# `word` is matched as a whole identifier.
function(scan dirs word inc label)
    set(out "")
    foreach(dir IN LISTS dirs)
        file(GLOB_RECURSE files ${ROOT}/${dir}/*.cpp ${ROOT}/${dir}/*.hpp ${ROOT}/${dir}/*.h
                                ${ROOT}/${dir}/*.cc ${ROOT}/${dir}/*.ipp ${ROOT}/${dir}/*.inl
                                ${ROOT}/${dir}/*.tpp ${ROOT}/${dir}/*.mm)
        foreach(f IN LISTS files)
            if(f MATCHES "/tests/lint/" OR f MATCHES "/_deps/" OR f MATCHES "/build[^/]*/")
                continue()
            endif()
            # Cheap pre-filter: most files never mention the name at all.
            file(STRINGS ${f} any REGEX "${word}|${inc}")
            if(NOT any)
                continue()
            endif()
            file(READ ${f} raw_text)                  # for include lines
            string(REPLACE ";" "\\;" raw_text "${raw_text}")
            string(REPLACE "\n" ";" raw "${raw_text}")
            code_of(${f} code)
            string(REPLACE ";" "\\;" code "${code}")
            string(REPLACE "\n" ";" lines "${code}")
            file(RELATIVE_PATH rel ${ROOT} ${f})
            set(ln 0)
            foreach(l IN LISTS lines)
                math(EXPR ln "${ln} + 1")
                # An include path is a "string", already blanked: check the
                # directive against the raw line instead.
                set(hit FALSE)
                if(l MATCHES "^[ \t]*#[ \t]*include")
                    math(EXPR k "${ln} - 1")
                    list(LENGTH raw rn)
                    if(k LESS rn)
                        list(GET raw ${k} r)
                        if(r MATCHES "${inc}")
                            set(hit TRUE)
                        endif()
                    endif()
                endif()
                if(hit OR l MATCHES "(^|[^A-Za-z0-9_])(${word})([^A-Za-z0-9_]|$)")
                    string(STRIP "${l}" s)
                    list(APPEND out "${label}: ${rel}:${ln}: ${s}")
                endif()
            endforeach()
        endforeach()
    endforeach()
    set(hits ${hits} ${out} PARENT_SCOPE)
endfunction()

set(hits "")
scan("src;include;tests" "jaal|JAAL_[A-Za-z0-9_]+"
     "#[ \t]*include[ \t]*[<\"]([^>\"]*/)?jaal/" "agentty->jaal")

if(hits)
    list(JOIN hits "\n  " msg)
    message(FATAL_ERROR
        "agentty reaches past maya into jaal:\n  ${msg}\n\n"
        "Use the maya:: name from <maya/runtime.hpp>. If maya doesn't expose "
        "it yet, add it there (docs/LAYERING.md).")
endif()
message(STATUS "layering ok: agentty uses the runtime only through maya")
