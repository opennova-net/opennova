add_library(opennova_threedi STATIC
    src/threedi.cpp
    src/threedi_3di3.cpp
    src/threedi_compare.cpp
    src/threedi_panm.cpp
    src/threedi_panm_runtime.cpp
    src/threedi_panm_wave.cpp
    src/threedi_panm_matrices.cpp
    src/threedi_ir.cpp
    src/threedi_ir_from_3di3.cpp
    src/threedi_gp.cpp
    src/threedi_ir_from_gp.cpp
    src/threedi_lw.cpp
    src/threedi_ir_from_lw.cpp
    src/threedi_lw_anim.cpp
)

target_include_directories(opennova_threedi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link math library (needed for sin/cos/floorf in pure C builds; not needed on Windows/MSVC)
if(NOT WIN32 OR MINGW)
    target_link_libraries(opennova_threedi PUBLIC m)
endif()

# C99 standard for the library
set_target_properties(opennova_threedi PROPERTIES
    CXX_STANDARD 11
    CXX_STANDARD_REQUIRED ON
)
