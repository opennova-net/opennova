add_library(opennova_sbf STATIC
    src/sbf.cpp
)

target_include_directories(opennova_sbf
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
)

set_target_properties(opennova_sbf PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
)
