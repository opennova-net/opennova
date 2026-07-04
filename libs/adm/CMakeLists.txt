add_library(opennova_adm STATIC
    src/adm.cpp
)

target_include_directories(opennova_adm
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_adm PUBLIC cxx_std_11)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_adm PUBLIC opennova_io)
