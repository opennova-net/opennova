if(NOT WIN32 OR NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR
        "The retail hook and injector require a 32-bit Windows toolchain")
endif()

add_library(opennova_retail_hook SHARED
    src/file_sha256.cpp
    src/hook_dll.cpp
    src/local_process_memory.cpp
    src/retail_capture_agent.cpp
    src/validation_window.cpp
    src/winsock_capture.cpp
    src/winsock_hooks.cpp
    src/winsock_parity.cpp
)

target_include_directories(opennova_retail_hook PRIVATE include)
target_compile_features(opennova_retail_hook PRIVATE cxx_std_17)
target_compile_definitions(opennova_retail_hook PRIVATE UNICODE _UNICODE)
target_link_libraries(opennova_retail_hook
    PRIVATE
        opennova_retail_hook_core
        opennova_parity_windows
        bcrypt
        gdi32
        user32
        ws2_32
)
set_target_properties(opennova_retail_hook PROPERTIES
    PREFIX ""
    OUTPUT_NAME "opennova_retail_hook"
)

add_executable(retail_hook_injector
    injector/main.cpp
    src/file_sha256.cpp
    src/retail_cli.cpp
)

target_include_directories(retail_hook_injector PRIVATE include)
target_compile_features(retail_hook_injector PRIVATE cxx_std_17)
target_compile_definitions(retail_hook_injector PRIVATE UNICODE _UNICODE)
target_link_libraries(retail_hook_injector
    PRIVATE
        opennova_retail_hook_core
        bcrypt
)

add_executable(retail_parity_inspector
    inspector/main.cpp
    src/named_pipe_trace.cpp
    src/retail_cli.cpp
)

target_include_directories(retail_parity_inspector PRIVATE include)
target_compile_features(retail_parity_inspector PRIVATE cxx_std_17)
target_compile_definitions(retail_parity_inspector PRIVATE UNICODE _UNICODE)
target_link_libraries(retail_parity_inspector PRIVATE opennova_parity)

if(MSVC)
    target_compile_options(opennova_retail_hook PRIVATE /W4 /permissive-)
    target_compile_options(retail_hook_injector PRIVATE /W4 /permissive-)
    target_compile_options(retail_parity_inspector PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_retail_hook
        PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(retail_hook_injector
        PRIVATE -Wall -Wextra -Wpedantic)
    target_compile_options(retail_parity_inspector
        PRIVATE -Wall -Wextra -Wpedantic)
endif()
