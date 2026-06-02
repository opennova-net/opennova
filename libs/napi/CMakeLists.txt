add_library(opennova_napi STATIC
    src/envelope.cpp
    src/tlv.cpp
    src/session.cpp
)

target_include_directories(opennova_napi
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(opennova_napi PUBLIC opennova_novacrypto)

target_compile_features(opennova_napi PUBLIC cxx_std_17)
