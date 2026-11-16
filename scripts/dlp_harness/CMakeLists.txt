cmake_minimum_required(VERSION 3.16)
project(dlp_harness LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(MELONDS_SRC "/opt/melonds-src" CACHE PATH "Path to melonDS source tree")
set(MELONDS_BUILD "/opt/melonds-build" CACHE PATH "Path to melonDS static build tree")

find_package(Threads REQUIRED)

add_executable(dlp_harness
    dlp_harness.cpp
    PlatformImpl.cpp
    ${MELONDS_SRC}/src/net/LocalMP.cpp
)

target_include_directories(dlp_harness PRIVATE
    ${MELONDS_SRC}/src
    ${MELONDS_SRC}/src/net
)

target_link_libraries(dlp_harness PRIVATE
    ${MELONDS_BUILD}/src/libcore.a
    ${MELONDS_BUILD}/src/teakra/src/libteakra.a
    Threads::Threads
    dl
)
