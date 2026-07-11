add_library(opennova_parity_windows STATIC
    src/named_pipe_event_sink.cpp)

target_include_directories(opennova_parity_windows
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include)
target_link_libraries(opennova_parity_windows PUBLIC opennova_parity)
target_compile_features(opennova_parity_windows PUBLIC cxx_std_17)
target_compile_definitions(opennova_parity_windows PRIVATE UNICODE _UNICODE)

if(MSVC)
    target_compile_options(opennova_parity_windows PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_parity_windows
        PRIVATE -Wall -Wextra -Wpedantic)
endif()
