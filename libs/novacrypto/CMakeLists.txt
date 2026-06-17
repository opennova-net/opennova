add_library(opennova_novacrypto STATIC
    src/url_cipher.cpp
    src/nwu.cpp
    src/pubcrypto.cpp
    src/epask.cpp
)

target_include_directories(opennova_novacrypto
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_novacrypto PUBLIC cxx_std_17)
