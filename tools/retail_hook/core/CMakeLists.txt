add_library(opennova_retail_hook_core STATIC
    src/buffer_memory.cpp
    src/validation_session.cpp
)

target_include_directories(opennova_retail_hook_core
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(opennova_retail_hook_core PUBLIC cxx_std_17)
target_link_libraries(opennova_retail_hook_core PUBLIC opennova_retail_abi)

if(MSVC)
    target_compile_options(opennova_retail_hook_core PRIVATE /W4 /permissive-)
else()
    target_compile_options(opennova_retail_hook_core PRIVATE -Wall -Wextra -Wpedantic)
endif()
