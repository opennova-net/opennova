# The retail-parity MSVC CRT rand()/srand() stream (one thread-local state
# shared by the ported render/effects consumers; the sim-visible draws own
# World::crt_rand — D-NET-115). A STATIC leaf below formats: both the
# formats group (threedi PANM runtime) and the runtime group consume it, so
# it sits beside base/io rather than inside opennova_base.
add_library(opennova_crt STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/crt_rng.cpp
)

target_include_directories(opennova_crt
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_compile_features(opennova_crt PUBLIC cxx_std_17)
set_target_properties(opennova_crt PROPERTIES POSITION_INDEPENDENT_CODE ON)
