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
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_import.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_type_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/project_asset_source.cpp
    # blank — the from-scratch files behind New, the required files' Create fixes and a
    # missing texture's placeholder
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_coo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_defs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_strings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_style.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_texture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/create_missing.cpp
    # model — the neutral editing core: the document lifecycle and the undo journal, what a field
    # shows, and the find over a document's fields
    ${CMAKE_CURRENT_SOURCE_DIR}/model/document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/document_search.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/edit_history.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/field_text.cpp
    # documents — the document types: the def catalogs, their symbol tables and validation;
    # the menu clipboard's one rule for the tree and the canvas
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/def_catalog_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/strings_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_clipboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_ids.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mns_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/model_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/model_document_edits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/animation_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/animation_map_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/document_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/catalog_validation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/validation_cache.cpp
    # graph — the asset graph: the extractors, the reference kinds' table, the one resolver, the
    # rename transaction
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/asset_graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/extractors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/reference_kinds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/reference_resolution.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/rename_transaction.cpp
    # import — the importers (a PNG to the game's PCX), the sidecars, the import pass, and
    # the plan of an import with the files it needs
    ${CMAKE_CURRENT_SOURCE_DIR}/import/converter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/importers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_run.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/png_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/quantize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/sidecar.cpp
    # preview — the menu preview's portable half: what it shows and when to configure it
    # again, and its JSON (the device is the shell's MenuFrame); the headless render of a
    # screen, the render check over every menu (the compiler notes as findings), and the
    # texture header probe it measures with; what a drag of a window's handles writes, and
    # what arranging several windows (align, distribute, drawing order) writes; a menu's
    # tree and its findings as the editor MCP's editor_menu reads them; the model
    # preview's portable half: its camera and the level the game draws, what it shows and
    # when to build it again, and its JSON (the device is the shell's ObjectModel)
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_arrange.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_layout_edit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_preview_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_preview_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_render_check.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_report.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_screen_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_handle_edit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_rig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/texture_header.cpp
    # project — project.opennova, the cache directory, machine-local settings, file plumbing,
    # and the one refresh (import, scan, requirements) and the one findings composer the
    # editor and the CLI share
    ${CMAKE_CURRENT_SOURCE_DIR}/project/local_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_findings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_state.cpp
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
    # session — the one open project and everything the editor does to it (ADR 0046 d10),
    # what Problems shows of its findings and offers to do about them, and the findings by
    # file and record
    ${CMAKE_CURRENT_SOURCE_DIR}/session/editor_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/findings_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/problem_fixes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/problem_query.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/project_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/record_batch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/session_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/session_view.cpp
)

# ui — the Dear ImGui windows on the engine's ImGui pass (ADR 0046 d11). Built when the
# pass is (OPENNOVA_IMGUI: the dev-tools or the editor flavour); OPENNOVA_EDITOR_UI tells a
# consumer whether the windows exist in this build.
if(OPENNOVA_IMGUI)
    target_sources(opennova_editor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/catalog_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_outline.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_toolbar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/editor_windows.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/field_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/files_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/find_cursor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/import_dialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/inspector_layout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/inspector_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/menu_preview_pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/menu_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/model_preview_pane.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/output_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/preview_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/problems_list.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/problems_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/project_find.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/project_settings_dialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/record_reveal.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/record_tree.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/reference_picker.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/rename_dialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/strings_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/styles_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/table_cells.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/ui_kit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/welcome_view.cpp
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
# The image importer inflates PNG data through the vendored miniz (private: no
# consumer of the editor sees miniz.h).
target_link_libraries(opennova_editor PRIVATE opennova_miniz)

target_compile_features(opennova_editor PUBLIC cxx_std_17)

set_target_properties(opennova_editor PROPERTIES
    POSITION_INDEPENDENT_CODE ON
)
