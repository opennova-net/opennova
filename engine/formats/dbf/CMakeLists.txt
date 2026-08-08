add_library(opennova_dbf STATIC
    src/dbf.cpp
)

target_include_directories(opennova_dbf
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_dbf PUBLIC cxx_std_17)
target_link_libraries(opennova_dbf PRIVATE opennova_io)

set_target_properties(opennova_dbf PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
