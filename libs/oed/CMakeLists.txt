add_library(opennova_oed STATIC
    src/oed.cpp
    src/convert_internal.cpp
    src/export_3di.cpp
    src/rdta.cpp
    src/material_utils.cpp
)

target_include_directories(opennova_oed
    PUBLIC  include
    PRIVATE
        src   # for threedi_model.h (private)
        ${CMAKE_CURRENT_SOURCE_DIR}/../threedi/include
)

# Dependencies: ASE parser, TDP project structs
# Note: threedi write/free symbols resolve at final link time (via --whole-archive
# in the shared lib, or direct linking in executables). Adding opennova_threedi here
# would cause CMake to de-duplicate it out of the --whole-archive block.
target_link_libraries(opennova_oed
    PUBLIC opennova_ase opennova_tdp
)

target_compile_features(opennova_oed PRIVATE cxx_std_17)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_oed PUBLIC opennova_io)
