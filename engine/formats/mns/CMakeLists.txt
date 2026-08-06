add_library(opennova_mns STATIC
    src/mns.cpp
    src/mns_document.cpp
)

target_include_directories(opennova_mns
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_mns PUBLIC cxx_std_17)
