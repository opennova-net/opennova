# Every game's own code (games/CLAUDE.md), one directory per title. engine/ holds the types, the
# registries and the lookups; games.h's two installs fill the registries from an explicit call at
# each program's start. The repository root is the include root: <games/<code>/<file>.h>.
#
# Two targets, so a program that never runs a match carries no in-match runtime:
#   opennova_games        every title and the protocol it speaks (games::install()): base and net
#                         alone (the updater, the gate, the data program link this alone).
#   opennova_games_rules  the TitleRules every title runs (games::install_rules()): the runtime
#                         (every program that runs a match, the editor's included, links both).

add_library(opennova_games STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/games.cpp
    # novalogic_2009 — the network code generation Jointops.exe and dfx2.exe share
    ${CMAKE_CURRENT_SOURCE_DIR}/novalogic_2009/generation.cpp
    # jo — Joint Operations: Typhoon Rising
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_protocol.cpp
    # dfx2 — Delta Force Xtreme 2
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_protocol.cpp
    # acidgambit — OP: Acid Gambit, JO's protocol and rules
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_protocol.cpp
    # the unwitnessed titles, refused by name
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx/dfx_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx/dfx_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bhd/bhd_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bhd/bhd_protocol.cpp
)

target_include_directories(opennova_games
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(opennova_games PUBLIC opennova_net)

target_compile_features(opennova_games PUBLIC cxx_std_17)

set_target_properties(opennova_games PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)

add_library(opennova_games_rules STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/games_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx/dfx_rules.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bhd/bhd_rules.cpp
)

target_link_libraries(opennova_games_rules PUBLIC opennova_games opennova_runtime)

target_compile_features(opennova_games_rules PUBLIC cxx_std_17)

set_target_properties(opennova_games_rules PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
