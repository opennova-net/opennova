# mbedTLS (third_party/mbedtls, a pinned shallow submodule at the mbedtls-3.6.7 release tag, with
# its own framework submodule; Apache-2.0, the license OpenNova takes of its dual Apache-2.0 /
# GPL-2.0-or-later grant), the TLS under the engine's HTTPS stream (engine/net/http/tls_stream.cpp,
# ADR 0077 R4). Built static with its own CMake, its programs, tests, file generators and install
# rules left out, over the library's default configuration plus mbedtls_user_config.h beside this
# file (thread safety, the one change). Only opennova_net links it, PRIVATE
# (scripts/lint/link_graph_check.py), and only tls_stream.cpp includes its headers
# (scripts/lint/include_graph_check.py), so no engine interface names an mbedTLS type. Both CMake
# roots (the repository root and godot/src) add this directory. A bump moves the submodule to
# another release tag and nothing else: it is never edited in place.
if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/../mbedtls/framework/CMakeLists.txt")
    message(FATAL_ERROR "opennova_net needs the mbedTLS submodule and its framework: run "
        "`git submodule update --init --recursive third_party/mbedtls`")
endif()

set(ENABLE_PROGRAMS OFF CACHE BOOL "" FORCE)
set(ENABLE_TESTING OFF CACHE BOOL "" FORCE)
set(GEN_FILES OFF CACHE BOOL "" FORCE) # a release tag carries its generated sources
set(MBEDTLS_FATAL_WARNINGS OFF CACHE BOOL "" FORCE)
set(USE_STATIC_MBEDTLS_LIBRARY ON CACHE BOOL "" FORCE)
set(USE_SHARED_MBEDTLS_LIBRARY OFF CACHE BOOL "" FORCE)
set(DISABLE_PACKAGE_CONFIG_AND_INSTALL ON CACHE BOOL "" FORCE)
set(MBEDTLS_USER_CONFIG_FILE "${CMAKE_CURRENT_SOURCE_DIR}/mbedtls_user_config.h"
    CACHE FILEPATH "" FORCE)
if(NOT WIN32)
    # MBEDTLS_THREADING_PTHREAD (mbedtls_user_config.h).
    set(LINK_WITH_PTHREAD ON CACHE BOOL "" FORCE)
endif()
add_subdirectory(../mbedtls ${CMAKE_CURRENT_BINARY_DIR}/mbedtls EXCLUDE_FROM_ALL)

# Every library it defines: position-independent (the GDExtension is a shared library), its
# symbols hidden there (a host that carries its own mbedTLS, as the Godot editor does, never
# binds ours or we its), and threading_alt.h on the include path (MBEDTLS_THREADING_ALT).
foreach(target mbedcrypto mbedx509 mbedtls everest p256m)
    if(TARGET ${target})
        set_target_properties(${target} PROPERTIES
            POSITION_INDEPENDENT_CODE ON
            C_VISIBILITY_PRESET hidden
            FOLDER "third_party")
        target_include_directories(${target} PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
endforeach()
