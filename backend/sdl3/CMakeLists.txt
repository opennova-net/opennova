# opennova_backend_sdl3: the SDL3 platform backend (ADR 0077 d9), the engine's platform API
# (engine/base/platform) over SDL3 and, from R4, the SDL_GPU device R5's engine GPU API runs on.
# The ONE library that links SDL3 (scripts/lint/link_graph_check.py rule 4): its public headers
# (backend.h, gpu.h) name no SDL type, and only sources under backend/sdl3 include <SDL3/...>
# (scripts/lint/include_graph_check.py rule 11). The repository root is its include root:
# <backend/sdl3/backend.h>. Platform plumbing, not a port: no source here cites the original.

add_library(opennova_backend_sdl3 STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/backend.cpp # SDL's init, the platform record, the clock and its pump, files, OS
    ${CMAKE_CURRENT_SOURCE_DIR}/window.cpp  # platform::Window over an SDL_Window
    ${CMAKE_CURRENT_SOURCE_DIR}/input.cpp   # platform::InputDevice: SDL's events onto the raw input sample
    ${CMAKE_CURRENT_SOURCE_DIR}/audio.cpp   # platform::AudioDevice over an SDL_AudioStream
    ${CMAKE_CURRENT_SOURCE_DIR}/gpu.cpp     # the SDL_GPU device over the window (R4: clear and present)
)

target_include_directories(opennova_backend_sdl3
    PUBLIC
        ${CMAKE_SOURCE_DIR}
)

target_link_libraries(opennova_backend_sdl3
    PUBLIC
        opennova_base        # base/platform's interfaces, base/proc's child and command line
    PRIVATE
        SDL3::SDL3-static    # third_party/sdl3, built static by the root CMakeLists.txt
)

target_compile_features(opennova_backend_sdl3 PUBLIC cxx_std_17)
