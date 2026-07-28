add_library(opennova_tdp STATIC src/tdp.cpp)
target_include_directories(opennova_tdp PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(opennova_tdp PRIVATE opennova_threedi)
target_compile_features(opennova_tdp PUBLIC cxx_std_17)

# io/export.h (and for some libs io/le.h) is used from public headers.
target_link_libraries(opennova_tdp PUBLIC opennova_io)
