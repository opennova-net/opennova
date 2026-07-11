add_library(opennova_parity STATIC
    src/capture.cpp
    src/compare.cpp
    src/file_event_sink.cpp
    src/parity.cpp)

target_include_directories(opennova_parity
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(opennova_parity PUBLIC opennova_io)
target_compile_features(opennova_parity PUBLIC cxx_std_17)

if(MSVC)
    target_compile_options(opennova_parity PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_parity PRIVATE -Wall -Wextra -Wpedantic)
endif()

if(WIN32)
    add_subdirectory(windows)
endif()
