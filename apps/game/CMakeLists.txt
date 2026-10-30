# opennova-game: the game over the SDL3 backend (ADR 0077 d9), the app shell that becomes
# opennova.exe at R8 (the Godot build keeps that name until then). main() brings the SDL3
# platform (backend/sdl3) and its SDL_GPU device up and runs the engine's frame over them; the
# mission, its session and its input frame are the engine's (runtime/), the app owns their order.
# Built only with OPENNOVA_GAME (the root CMakeLists.txt). godot-cpp is never on this graph.

add_executable(opennova_game
    main.cpp
    local_mission.cpp # the command line's mission through the engine's load executor and session
    local_input.cpp   # the platform's raw input into the engine's input frame
)

target_link_libraries(opennova_game
    PRIVATE
        opennova_backend_sdl3 # the platform and the GPU device (the one link to SDL3)
        opennova_runtime      # the mission kernel, the session, the input frame, the main loop clock
        opennova_games        # every game's titles, protocols and rules (games::install)
)

target_compile_features(opennova_game PRIVATE cxx_std_17)
set_target_properties(opennova_game PROPERTIES OUTPUT_NAME "opennova-game")

# The resource-dir and mission paths arrive as arguments: without a UTF-8 manifest Windows hands
# them over in the ANSI code page, and the engine opens files from UTF-8 strings (io/os_path.h).
# The same manifest as opennova-3di and opennova-serve.
if(MSVC)
    target_sources(opennova_game PRIVATE ${CMAKE_SOURCE_DIR}/apps/3di/utf8.manifest)
endif()
