# opennova_editor — the OpenNova Editor's portable core (ADR 0046 d3), one STATIC
# group target: the project model, the asset registry, the requirements checklist and,
# slice by slice, the blank factories, the build, the play session, the documents, the
# graph, the importers and the ImGui windows. It sits ABOVE runtime so its validators
# reuse the engine's own load paths; nothing under base/formats/runtime/net may include
# or link it (scripts/lint/include_graph_check.py, link_graph_check.py). Tooling, not a
# port: every source here is in the citation allowlist by prefix ("editor/").

add_library(opennova_editor STATIC
    # assets — what a project file is to the engine
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_type_registry.cpp
    # blank — the from-scratch files behind Create / Create all missing
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_coo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_defs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_strings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_style.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/create_missing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/editable_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/catalog_validation.cpp
    # project — project.opennova, the cache directory, machine-local settings, file plumbing
    ${CMAKE_CURRENT_SOURCE_DIR}/project/local_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_files.cpp
    # project_build — routing into the boot-table archives, the plan, the immutable build
    # (the directory is not "build*": the lints skip those)
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/archive_routing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/build_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/build_session.cpp
    # requirements — the manifest rows evaluated against the project
    ${CMAKE_CURRENT_SOURCE_DIR}/requirements/requirements.cpp
    # run — the Play launch plan and the one managed child over the platform seam
    ${CMAKE_CURRENT_SOURCE_DIR}/run/launch_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/run/play_session.cpp
    # session — the one open project and everything the editor does to it (ADR 0046 d10)
    ${CMAKE_CURRENT_SOURCE_DIR}/session/editor_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/project_session.cpp
)

# ui — the Dear ImGui windows on the engine's ImGui pass (ADR 0046 d11). Built when the
# pass is (OPENNOVA_IMGUI: the dev-tools or the editor flavour); OPENNOVA_EDITOR_UI tells a
# consumer whether the windows exist in this build.
if(OPENNOVA_IMGUI)
    target_sources(opennova_editor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/assets_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/catalog_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/editor_windows.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/output_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/problems_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/project_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/requirements_window.cpp
    )
    target_link_libraries(opennova_editor PRIVATE opennova_imgui)
    target_compile_definitions(opennova_editor PUBLIC OPENNOVA_EDITOR_UI=1)
else()
    target_compile_definitions(opennova_editor PUBLIC OPENNOVA_EDITOR_UI=0)
endif()

# engine/ is the one public include root (ADR 0040): <editor/<lib>/file.h>.
target_include_directories(opennova_editor
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/..
)

# The chain: editor -> runtime -> net -> base -> formats -> io (all PUBLIC).
target_link_libraries(opennova_editor PUBLIC opennova_runtime)

target_compile_features(opennova_editor PUBLIC cxx_std_17)

set_target_properties(opennova_editor PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
