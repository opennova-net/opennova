# opennova_base — the engine base services (project, vfs, resource_index, gameprofile,
# title, pcapio, os_random, os_registry, proc, version), one STATIC group target (Shape A). The header-only
# base/io leaf keeps its own INTERFACE target in its subdirectory.
#
# base sits ABOVE formats in the Shape A chain (the deliberate inversion): vfs
# consumes pff/scr/bfc1, so this group PUBLIC-links opennova_formats.

add_library(opennova_base STATIC
    # project — a project revision's manifest and path rules (ADR 0069), the service's and the editor's
    ${CMAKE_CURRENT_SOURCE_DIR}/project/case_fold.cpp # Unicode's simple case folding (scripts/project/case_fold_table.py)
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_manifest.cpp
    # gameprofile — required-resource tables, the player's own files, the graphics log
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/graphics_log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/player_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/gameprofile/required_resources.cpp
    # title — the titles a game runs as (each game's rows are games/<code>/'s, games::install())
    ${CMAKE_CURRENT_SOURCE_DIR}/title/title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/title/controls_catalog.cpp # a title's static binding catalog's data
    # os_random — the operating system's CSPRNG (the NovaWorld service's secrets and ids)
    ${CMAKE_CURRENT_SOURCE_DIR}/os_random/os_random.cpp
    # os_registry — a registry string value as NovaLogic's executables read and write one (a DFX2
    # client's CD-key activation); a file stands in where the platform has no registry
    ${CMAKE_CURRENT_SOURCE_DIR}/os_registry/os_registry.cpp
    # pcapio — pcap/pcapng reading (shared by capture-driven tests and tools)
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_reader.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/pcapio/pcap_writer.cpp
    # version — Version, ONE's build (one_build.cpp, the one stamped TU) and the combined line
    ${CMAKE_CURRENT_SOURCE_DIR}/version/one_build.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/version/version.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/version/version_line.cpp
    # resource_index — the mounted resource directory index
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/resource_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/resource_kind.cpp # the kind classifier, shared with the editor
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/file_kind.cpp # the typed kinds and their loader facts
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/boot_policy.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/resource_index/mod_listing.cpp # a game's mods on NovaWorld and their folders (ADR 0074)
    # vfs — the engine-faithful PFF+loose+expansion mount stack
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/vfs_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/vfs/dir_case_index.cpp # a loose folder's files by name without case
)

# The ONE stamp (base/version/one_build.h): ONE's version, commit, staging flag and build time,
# compiled into version/one_build.cpp alone, so a new stamp rebuilds one TU. Both CMake roots
# (the repo root and godot/src) add engine/, so both read these. CI passes all four
# (scripts/ci/one_stamp.sh: a tag's build its tag, a pull request's <tag>.<run> and staging;
# the Docker builds, whose context holds no .git, take them as ARGs); what is not given is
# taken from git:
#   OPENNOVA_VERSION     the latest v* tag reachable from HEAD without its v (v0.0.22: 0.0.22)
#   OPENNOVA_COMMIT      HEAD's sha (a given sha, full or short, keeps its first seven)
#   OPENNOVA_BUILD_TIME  HEAD's commit time, Unix seconds: never the configure's or the
#                        compile's clock, which would miss the compiler cache every build
# and with no git (or no tag) 0.0.0, no commit and 0. The version is 1 to 4 numbers, the
# first three at most 255 (retail's engine-build slots carry them a byte each).
set(OPENNOVA_VERSION "" CACHE STRING "ONE's version, 1 to 4 numbers (empty: the latest v* tag reachable from HEAD)")
set(OPENNOVA_COMMIT "" CACHE STRING "ONE's commit sha (empty: HEAD's)")
option(OPENNOVA_STAGING "ONE is a staging build (a pull request's CI build)" OFF)
set(OPENNOVA_BUILD_TIME "0" CACHE STRING "ONE's build time, Unix seconds (0: HEAD's commit time)")

set(one_version "${OPENNOVA_VERSION}")
set(one_commit "${OPENNOVA_COMMIT}")
set(one_time "${OPENNOVA_BUILD_TIME}")
find_package(Git QUIET)
if(GIT_FOUND)
    if(one_version STREQUAL "")
        execute_process(COMMAND "${GIT_EXECUTABLE}" describe --tags --abbrev=0 --match "v[0-9]*"
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE git_result OUTPUT_VARIABLE git_tag
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(git_result EQUAL 0)
            string(REGEX REPLACE "^v" "" one_version "${git_tag}")
        endif()
    endif()
    if(one_commit STREQUAL "")
        execute_process(COMMAND "${GIT_EXECUTABLE}" rev-parse HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE git_result OUTPUT_VARIABLE git_sha
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(git_result EQUAL 0)
            set(one_commit "${git_sha}")
        endif()
    endif()
    if(one_time STREQUAL "" OR one_time STREQUAL "0")
        execute_process(COMMAND "${GIT_EXECUTABLE}" log -1 --format=%ct HEAD
            WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
            RESULT_VARIABLE git_result OUTPUT_VARIABLE git_time
            OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
        if(git_result EQUAL 0)
            set(one_time "${git_time}")
        endif()
    endif()
endif()
if(one_version STREQUAL "")
    set(one_version "0.0.0")
endif()
if(one_time STREQUAL "")
    set(one_time "0")
endif()
string(LENGTH "${one_version}" one_version_length)
if(one_version_length GREATER 32 OR
        NOT one_version MATCHES "^[0-9]+(\.[0-9]+)?(\.[0-9]+)?(\.[0-9]+)?$")
    message(FATAL_ERROR "OPENNOVA_VERSION '${one_version}' is not 1 to 4 numbers between dots (0.0.22)")
endif()
string(REPLACE "." ";" one_version_parts "${one_version}")
list(LENGTH one_version_parts one_version_count)
foreach(index RANGE 0 3)
    if(index LESS one_version_count)
        list(GET one_version_parts ${index} part)
        string(LENGTH "${part}" part_length)
        if(part_length GREATER 10 OR part GREATER 4294967295 OR (index LESS 3 AND part GREATER 255))
            message(FATAL_ERROR "OPENNOVA_VERSION '${one_version}': its first three numbers are at "
                "most 255, its fourth at most 4294967295")
        endif()
    endif()
endforeach()
string(TOLOWER "${one_commit}" one_commit)
if(NOT one_commit MATCHES "^[0-9a-f]*$")
    message(FATAL_ERROR "OPENNOVA_COMMIT '${one_commit}' is not a commit's sha")
endif()
string(SUBSTRING "${one_commit}" 0 7 one_commit)
if(NOT one_time MATCHES "^[0-9]+$" OR one_time GREATER 253402300799)
    message(FATAL_ERROR "OPENNOVA_BUILD_TIME '${one_time}' is not a time in Unix seconds")
endif()
if(OPENNOVA_STAGING)
    set(one_staging 1)
else()
    set(one_staging 0)
endif()
set_source_files_properties(${CMAKE_CURRENT_SOURCE_DIR}/version/one_build.cpp PROPERTIES
    COMPILE_DEFINITIONS
        "OPENNOVA_VERSION=\"${one_version}\";OPENNOVA_COMMIT=\"${one_commit}\";OPENNOVA_STAGING=${one_staging};OPENNOVA_BUILD_TIME=${one_time}")
message(STATUS "ONE ${one_version} (commit '${one_commit}', staging ${one_staging}, time ${one_time})")

# engine/ is the one public include root (ADR 0040): <base/<lib>/file.h>.
target_include_directories(opennova_base
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# proc — child processes from an argv array, no shell (the hosted-server
# supervisor's opennova-serve, a front end's game). Its POSIX side blocks
# signals around the fork with pthread_sigmask.
target_sources(opennova_base PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/proc/child_process.cpp)
find_package(Threads REQUIRED)
target_link_libraries(opennova_base PRIVATE Threads::Threads)

target_link_libraries(opennova_base PUBLIC opennova_io opennova_formats)
if(WIN32)
    # os_random.cpp: BCryptGenRandom. `bcrypt` names the system import library;
    # the vendored password-hash library is the distinct opennova_bcrypt target.
    # proc/child_process.cpp: terminate()'s WM_CLOSE (EnumWindows,
    # PostMessageW) is user32's. os_registry/os_registry.cpp: the Reg* calls are advapi32's.
    target_link_libraries(opennova_base PRIVATE bcrypt user32 advapi32)
endif()

target_compile_features(opennova_base PUBLIC cxx_std_17)

set_target_properties(opennova_base PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
