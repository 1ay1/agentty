# paste_page_sync.cmake — the website's paste page is what agentty prints.
#
# Usage: cmake -DBIN=<agentty> -DPAGE=<docs/website/static/paste.html> -P paste_page_sync.cmake

# `paste page` only prints a constant; it reads no settings and writes nothing.
execute_process(COMMAND ${BIN} paste page
                OUTPUT_VARIABLE want RESULT_VARIABLE rc)
if(NOT rc EQUAL 0)
    message(FATAL_ERROR "`agentty paste page` failed (${rc})")
endif()
file(READ ${PAGE} have)
if(NOT have STREQUAL want)
    message(FATAL_ERROR
        "docs/website/static/paste.html is not what `agentty paste page` prints.\n"
        "The page encodes and agentty decodes, so they must match. Run:\n"
        "  agentty paste page > docs/website/static/paste.html")
endif()
message(STATUS "paste page in sync")
