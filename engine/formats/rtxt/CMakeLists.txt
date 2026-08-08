add_library(opennova_rtxt STATIC
    src/rtxt.cpp
)

target_include_directories(opennova_rtxt
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_rtxt PUBLIC cxx_std_17)

target_link_libraries(opennova_rtxt PRIVATE opennova_io)
