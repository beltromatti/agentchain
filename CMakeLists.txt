cmake_minimum_required(VERSION 3.16)
project(agentchain
    VERSION 1.0.15
    DESCRIPTION "AgentChain Engine — reference client for the AgentChain protocol"
    HOMEPAGE_URL "https://github.com/beltromatti/agentchain"
    LANGUAGES C)

# -----------------------------------------------------------------------------
# Build options
# -----------------------------------------------------------------------------
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

set(CMAKE_POSITION_INDEPENDENT_CODE ON)

option(AGENTCHAIN_STATIC_SODIUM "Statically link libsodium" OFF)
option(AGENTCHAIN_BUILD_TESTS "Build the unit-test suite" ON)
option(AGENTCHAIN_WARNINGS_AS_ERRORS "Treat warnings as errors" OFF)

# -----------------------------------------------------------------------------
# Compile flags
# -----------------------------------------------------------------------------
if(CMAKE_C_COMPILER_ID MATCHES "GNU|Clang|AppleClang")
    add_compile_options(
        -Wall -Wextra
        -Wpedantic
        -Wshadow
        -Wstrict-prototypes
        -Wmissing-prototypes
        -Wpointer-arith
        -Wcast-align
        -Wwrite-strings
        -Wunreachable-code
        -Wformat=2
        -Wformat-security
        -Wundef
        -Wno-unused-parameter
        -D_POSIX_C_SOURCE=200809L
        -D_DEFAULT_SOURCE
    )
    if(AGENTCHAIN_WARNINGS_AS_ERRORS)
        add_compile_options(-Werror)
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Release")
        add_compile_options(-O2)
        if(NOT WIN32)
            # _FORTIFY_SOURCE / stack-protector rely on glibc + libssp.
            add_compile_options(-fstack-protector-strong)
            add_compile_definitions(_FORTIFY_SOURCE=2)
        endif()
    endif()
    if(CMAKE_BUILD_TYPE STREQUAL "Debug")
        add_compile_options(-O0 -g3 -fno-omit-frame-pointer)
    endif()
endif()

# -----------------------------------------------------------------------------
# Dependencies
# -----------------------------------------------------------------------------
find_package(Threads REQUIRED)

# libsodium: honour explicit overrides; otherwise try pkg-config then a manual search.
if(SODIUM_INCLUDE_DIRS AND SODIUM_LIBRARIES)
    message(STATUS "Using explicit libsodium: ${SODIUM_LIBRARIES}")
else()
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(SODIUM QUIET libsodium)
    endif()

    if(NOT SODIUM_FOUND)
        find_path(SODIUM_INCLUDE_DIR sodium.h
            PATHS /opt/homebrew/include /usr/local/include /usr/include
            DOC "Path to sodium.h"
        )
        find_library(SODIUM_LIBRARY
            NAMES sodium libsodium
            PATHS /opt/homebrew/lib /usr/local/lib /usr/lib /usr/lib/x86_64-linux-gnu /usr/lib/aarch64-linux-gnu
            DOC "Path to libsodium"
        )
        if(NOT SODIUM_INCLUDE_DIR OR NOT SODIUM_LIBRARY)
            message(FATAL_ERROR "libsodium not found. Install it: 'brew install libsodium' or 'apt install libsodium-dev'")
        endif()
        set(SODIUM_INCLUDE_DIRS ${SODIUM_INCLUDE_DIR})
        set(SODIUM_LIBRARIES ${SODIUM_LIBRARY})
    endif()
endif()

# -----------------------------------------------------------------------------
# Generated version header
# -----------------------------------------------------------------------------
execute_process(
    COMMAND git rev-parse --short=12 HEAD
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}
    OUTPUT_VARIABLE AGENTCHAIN_GIT_COMMIT
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT AGENTCHAIN_GIT_COMMIT)
    set(AGENTCHAIN_GIT_COMMIT "unknown")
endif()

configure_file(
    "${CMAKE_SOURCE_DIR}/src/version.h.in"
    "${CMAKE_BINARY_DIR}/generated/version.h"
    @ONLY
)

# -----------------------------------------------------------------------------
# Engine library (re-used by the binary and the tests)
# -----------------------------------------------------------------------------
set(ENGINE_SOURCES
    src/common.c
    src/crypto.c
    src/codec.c
    src/state.c
    src/chain.c
)
if(EXISTS "${CMAKE_SOURCE_DIR}/src/consensus.c")
    list(APPEND ENGINE_SOURCES src/consensus.c)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/mempool.c")
    list(APPEND ENGINE_SOURCES src/mempool.c)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/net.c")
    list(APPEND ENGINE_SOURCES src/net.c)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/rpc.c")
    list(APPEND ENGINE_SOURCES src/rpc.c)
endif()
if(EXISTS "${CMAKE_SOURCE_DIR}/src/node.c")
    list(APPEND ENGINE_SOURCES src/node.c)
endif()

add_library(agentchain_engine STATIC ${ENGINE_SOURCES})
target_include_directories(agentchain_engine
    PUBLIC
        ${CMAKE_SOURCE_DIR}/src
        ${CMAKE_BINARY_DIR}/generated
        ${SODIUM_INCLUDE_DIRS}
)
target_link_libraries(agentchain_engine
    PUBLIC
        Threads::Threads
        ${SODIUM_LIBRARIES}
)
if(SODIUM_LIBRARY_DIRS)
    target_link_directories(agentchain_engine PUBLIC ${SODIUM_LIBRARY_DIRS})
endif()
if(APPLE)
    # nothing extra
elseif(WIN32)
    # Winsock + Win32 multimedia time + winpthreads.
    target_link_libraries(agentchain_engine PUBLIC ws2_32 winmm pthread)
elseif(UNIX)
    target_link_libraries(agentchain_engine PUBLIC m)
endif()

# -----------------------------------------------------------------------------
# Binary
# -----------------------------------------------------------------------------
if(EXISTS "${CMAKE_SOURCE_DIR}/src/main.c")
    add_executable(agentchain src/main.c)
    target_link_libraries(agentchain PRIVATE agentchain_engine)
    install(TARGETS agentchain RUNTIME DESTINATION bin)
endif()
install(FILES PROTOCOL.md TECHNICAL-IMPLEMENTATION.md README.md LICENSE
    DESTINATION share/doc/agentchain OPTIONAL)

# -----------------------------------------------------------------------------
# Tests
# -----------------------------------------------------------------------------
if(AGENTCHAIN_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()

# -----------------------------------------------------------------------------
# Summary
# -----------------------------------------------------------------------------
message(STATUS "")
message(STATUS "AgentChain ${PROJECT_VERSION} (${AGENTCHAIN_GIT_COMMIT})")
message(STATUS "  Build type:  ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:    ${CMAKE_C_COMPILER_ID} ${CMAKE_C_COMPILER_VERSION}")
message(STATUS "  libsodium:   ${SODIUM_LIBRARIES}")
message(STATUS "  Tests:       ${AGENTCHAIN_BUILD_TESTS}")
message(STATUS "")
