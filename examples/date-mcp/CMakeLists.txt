cmake_minimum_required(VERSION 3.24)
project(date_mcp CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# mcp-cpp is header-only. Point at the copy already in this repo's
# submodule; a standalone consumer would use FetchContent (see README).
if(NOT TARGET mcp::mcp)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../../mcp-cpp
                     ${CMAKE_BINARY_DIR}/_mcp EXCLUDE_FROM_ALL)
endif()

add_executable(date_server date_server.cpp)
target_link_libraries(date_server PRIVATE mcp::mcp)
