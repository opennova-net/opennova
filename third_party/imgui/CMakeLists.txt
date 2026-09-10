# Dear ImGui (docking branch), fetched from GitHub at configure time (MIT).
#
# Pinned to the EXACT commit the imgui-godot addon bundles — the addon's
# prebuilt GDExtension creates the ImGui context and hands its pointer to our
# engine copy (ImGuiGD.GetImGuiPtrs, which fingerprints IMGUI_VERSION and the
# sizes of ImGuiIO/ImDrawVert/ImDrawIdx/ImWchar), so both copies must share one
# struct layout. The pin lives in two places that move together:
#   scripts/bootstrap_imgui_godot.sh   the addon release (bundles imgui-version.txt)
#   this file                          the engine copy (URL + URL_HASH below)
# and one config that must match the addon's imconfig-godot.h byte for byte
# where layout is concerned: opennova_imconfig.h next to this file
# (IMGUI_DISABLE_OBSOLETE_FUNCTIONS removes three ImGuiIO members at 1.91.6).
#
# For offline/air-gapped builds, point
# -DFETCHCONTENT_SOURCE_DIR_IMGUI_DOCKING=<dir> at a pre-extracted copy.
#
# Built in every flavour: the ImGui pass hosts the game's debug-only dev-tool
# windows (engine/runtime/CMakeLists.txt gates those).

include(FetchContent)

# imgui v1.91.6-docking (2024-12-11), the commit imgui-godot 6.3.2 bundles.
FetchContent_Declare(imgui_docking
    URL      https://github.com/ocornut/imgui/archive/947aa9c9722bd6ff740dd757da609ff41f4d3ba3.zip
    URL_HASH SHA256=091b70d972347108ccde50b099c4688859b5d402b085fca82732c7d93c0d4676
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)
FetchContent_MakeAvailable(imgui_docking)

add_library(opennova_imgui STATIC
    "${imgui_docking_SOURCE_DIR}/imgui.cpp"
    "${imgui_docking_SOURCE_DIR}/imgui_demo.cpp"
    "${imgui_docking_SOURCE_DIR}/imgui_draw.cpp"
    "${imgui_docking_SOURCE_DIR}/imgui_tables.cpp"
    "${imgui_docking_SOURCE_DIR}/imgui_widgets.cpp"
    # std::string-backed InputText (the tool windows' text fields).
    "${imgui_docking_SOURCE_DIR}/misc/cpp/imgui_stdlib.cpp"
)

target_include_directories(opennova_imgui
    PUBLIC
        "${imgui_docking_SOURCE_DIR}"
        "${CMAKE_CURRENT_SOURCE_DIR}"
)

# The config header reaches every TU that includes imgui.h, ours and imgui's own.
target_compile_definitions(opennova_imgui
    PUBLIC
        IMGUI_USER_CONFIG="opennova_imconfig.h"
)

target_compile_features(opennova_imgui PUBLIC cxx_std_17)

set_target_properties(opennova_imgui PROPERTIES
    POSITION_INDEPENDENT_CODE ON
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
)

# Suppress the noise that comes with vendored sources.
if(MSVC)
    target_compile_options(opennova_imgui PRIVATE /W0)
else()
    target_compile_options(opennova_imgui PRIVATE -w)
endif()
