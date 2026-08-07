add_library(opennova_hud STATIC
    src/hud_math.cpp
)

target_include_directories(opennova_hud
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

# Pure witnessed HUD math (the hudpos.def design-space scaling, ALPHAFADE
# ramps, stance-frame fixed-point scale, crosshair spread projection, message
# tick policy, ammo formatting). No format or io dependencies — the shell owns
# every draw/Font blit.
target_compile_features(opennova_hud PUBLIC cxx_std_17)
