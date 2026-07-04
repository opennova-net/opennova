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
)

# Dependencies: ASE parser, TDP project structs, 3DI writer (oed.h includes
# threedi_3di3.h). The shared lib force-loads core archives by file path, so
# declaring the real dependency no longer drops threedi from whole-archive.
target_link_libraries(opennova_oed
    PUBLIC opennova_ase opennova_tdp opennova_threedi
)

target_compile_features(opennova_oed PRIVATE cxx_std_17)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_oed PUBLIC opennova_io)
