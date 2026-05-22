if(NOT WIN32)
    message(FATAL_ERROR "ModSuperOED automation tools require Windows")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ModSuperOED automation tools require a 32-bit Windows toolchain")
endif()

add_library(modsuperoed_hook SHARED
    modsuperoed_hook.cpp
)

add_executable(modsuperoed_injector
    injector.cpp
)

foreach(target modsuperoed_hook modsuperoed_injector)
    target_compile_features(${target} PRIVATE cxx_std_17)
    target_compile_definitions(${target} PRIVATE
        NOMINMAX
        WIN32_LEAN_AND_MEAN
        _WIN32_WINNT=0x0600
    )
    set_target_properties(${target} PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/tools/modsuperoed"
    )
    foreach(config DEBUG RELEASE RELWITHDEBINFO MINSIZEREL)
        string(TOUPPER "${config}" config_upper)
        set_target_properties(${target} PROPERTIES
            "RUNTIME_OUTPUT_DIRECTORY_${config_upper}" "${CMAKE_BINARY_DIR}/tools/modsuperoed"
        )
    endforeach()
endforeach()

target_link_libraries(modsuperoed_hook PRIVATE shell32)
target_link_libraries(modsuperoed_injector PRIVATE advapi32)
