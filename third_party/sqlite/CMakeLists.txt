# SQLite amalgamation, fetched from sqlite.org at configure time (public domain).
#
# Pinned to an exact release and verified by hash, so the build is reproducible and
# the ~9 MB sqlite3.c does not live in the repo. The official sqlite.org artifact is
# the authoritative source: SQLite's only upstream git repo is the full Fossil source
# tree, not this generated amalgamation. To bump SQLite, change the version in the URL
# and the URL_HASH below. For offline/air-gapped builds, point
# -DFETCHCONTENT_SOURCE_DIR_SQLITE_AMALGAMATION=<dir> at a pre-extracted copy.
#
# Built as a small static library so libs/novaworld/db can link it without every
# consumer recompiling sqlite3.c. Compile-time defines follow the standard
# "embedded library" recipe:
#   THREADSAFE=1                — multi-threaded; we serialize via SQLITE_OPEN_NOMUTEX in code
#   ENABLE_FTS5                 — full-text search may be useful for log inspection
#   ENABLE_JSON1                — JSON1 extension for JSON /api responses
#   OMIT_LOAD_EXTENSION         — no dlopen/LoadLibrary attack surface
#   OMIT_DEPRECATED             — keep the binary lean
#   DEFAULT_FOREIGN_KEYS=1      — foreign key constraints enforced by default
#   DEFAULT_MEMSTATUS=0         — small perf gain, we don't query memstatus
#   USE_URI=1                   — needed for ":memory:" + URI-style filenames in tests

include(FetchContent)

# SQLite 3.47.0 (2024-10-21). Amalgamation zip from sqlite.org, verified byte-for-byte
# identical to the previously vendored copy.
FetchContent_Declare(sqlite_amalgamation
    URL      https://sqlite.org/2024/sqlite-amalgamation-3470000.zip
    URL_HASH SHA256=2842fddbb1cc33f66c7da998a57535f14a6bfee159676a07bb4bf3e59375d93e
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(sqlite_amalgamation)

# The amalgamation zip ships no CMakeLists; locate sqlite3.c within the populated
# tree (robust to whether CMake strips the single top-level directory).
file(GLOB_RECURSE _opennova_sqlite_src "${sqlite_amalgamation_SOURCE_DIR}/sqlite3.c")
list(GET _opennova_sqlite_src 0 _opennova_sqlite_src)
get_filename_component(_opennova_sqlite_dir "${_opennova_sqlite_src}" DIRECTORY)

add_library(opennova_sqlite STATIC "${_opennova_sqlite_src}")

target_include_directories(opennova_sqlite
    PUBLIC
        "${_opennova_sqlite_dir}"
)

target_compile_definitions(opennova_sqlite
    PRIVATE
        SQLITE_THREADSAFE=1
        SQLITE_ENABLE_FTS5
        SQLITE_ENABLE_JSON1
        SQLITE_OMIT_LOAD_EXTENSION
        SQLITE_OMIT_DEPRECATED
        SQLITE_DEFAULT_FOREIGN_KEYS=1
        SQLITE_DEFAULT_MEMSTATUS=0
        SQLITE_USE_URI=1
)

set_target_properties(opennova_sqlite PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

# Suppress the noise that comes with single-file amalgamation sources.
if(MSVC)
    target_compile_options(opennova_sqlite PRIVATE /W0 /wd4244 /wd4267)
else()
    target_compile_options(opennova_sqlite PRIVATE -w)
endif()

# Required platform deps.
if(UNIX)
    target_link_libraries(opennova_sqlite PUBLIC dl pthread m)
endif()
