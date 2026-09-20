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
    # project — project.opennova, the cache directory, machine-local settings, file plumbing
    ${CMAKE_CURRENT_SOURCE_DIR}/project/local_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_files.cpp
    # requirements — the manifest rows evaluated against the project
    ${CMAKE_CURRENT_SOURCE_DIR}/requirements/requirements.cpp
)

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
