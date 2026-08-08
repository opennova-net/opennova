add_library(opennova_pcapio STATIC
    src/pcap_reader.cpp
)

target_include_directories(opennova_pcapio
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_pcapio PUBLIC cxx_std_17)
