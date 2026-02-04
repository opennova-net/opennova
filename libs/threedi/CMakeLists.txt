add_library(opennova_threedi STATIC
    src/threedi.c
    src/threedi_3di3.c
    src/threedi_panm.c
    src/threedi_panm_runtime.c
    src/threedi_panm_wave.c
    src/threedi_panm_matrices.c
    src/threedi_ir.c
    src/threedi_ir_from_3di3.c
    src/threedi_gp.c
    src/threedi_ir_from_gp.c
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
    C_STANDARD 99
    C_STANDARD_REQUIRED ON
)
