add_subdirectory(core)

option(OPENNOVA_ENABLE_RETAIL_HOOK_TOOLS
    "Build the Joint Operations retail parity validation tools when supported"
    ON)

if(OPENNOVA_ENABLE_RETAIL_HOOK_TOOLS)
    if(WIN32 AND CMAKE_SIZEOF_VOID_P EQUAL 4)
        add_subdirectory(windows)
    elseif(WIN32)
        message(STATUS
            "Skipping retail validation hook: a 32-bit Windows toolchain is required")
    endif()
endif()
