# opennova_games: every game's own code (games/CLAUDE.md), one directory per title. engine/ holds the
# types, the registries and the lookups; games::install() (games.h) fills the registries from an explicit
# call at each program's start. The repository root is its include root: <games/<code>/<file>.h>.

add_library(opennova_games STATIC
    ${CMAKE_CURRENT_SOURCE_DIR}/games.cpp
    # novalogic_2009 — the network code generation Jointops.exe and dfx2.exe share
    ${CMAKE_CURRENT_SOURCE_DIR}/novalogic_2009/generation.cpp
    # jo — Joint Operations: Typhoon Rising
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/jo/jo_rules.cpp
    # dfx2 — Delta Force Xtreme 2
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx2/dfx2_rules.cpp
    # acidgambit — OP: Acid Gambit, JO's protocol and rules
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_title.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_protocol.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/acidgambit/acidgambit_rules.cpp
    # the unwitnessed titles, one TU each, refused by name
    ${CMAKE_CURRENT_SOURCE_DIR}/dfx/dfx.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/bhd/bhd.cpp
)

target_include_directories(opennova_games
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(opennova_games PUBLIC opennova_runtime)

target_compile_features(opennova_games PUBLIC cxx_std_17)

set_target_properties(opennova_games PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
