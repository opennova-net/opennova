add_library(opennova_threedi STATIC
    src/threedi.cpp
    src/threedi_3di3_read.cpp   # 3DI3 -> Threedi3di3
    src/threedi_3di3_write.cpp  # Threedi3di3 -> 3DI3 (parity writer, ADR 0003)
    src/threedi_compare.cpp
    src/threedi_ctrl_catalog.cpp
    src/threedi_panm.cpp
    src/threedi_panm_runtime.cpp
    src/threedi_panm_wave.cpp
    src/threedi_panm_matrices.cpp
    src/threedi_ir.cpp
    src/threedi_ir_ground_anchor.cpp
    src/threedi_ir_from_3di3.cpp
    src/threedi_gp_read.cpp     # GP (GPM/GPS/GPP) -> ThreediGpFile
    src/threedi_gp_write.cpp    # ThreediGpFile -> GP (parity writer, ADR 0003)
    src/threedi_ir_from_gp.cpp
)

target_include_directories(opennova_threedi
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Link math library (needed for sin/cos/floorf in pure C builds; not needed on Windows/MSVC)
if(NOT WIN32 OR MINGW)
    target_link_libraries(opennova_threedi PUBLIC m)
endif()

# C++17 is required by the shared ASCII string helpers used by the CTRL catalog.
set_target_properties(opennova_threedi PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_threedi PUBLIC opennova_io)
