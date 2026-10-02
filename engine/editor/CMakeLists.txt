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
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_kinds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/asset_type_registry.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/import_choice.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/player_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/project_asset_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/assets/project_scan.cpp
    # blank — the from-scratch files behind New, the required files' Create fixes and a
    # missing texture's placeholder
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_coo.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_defs.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_factory.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_font.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_menu.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_mission.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_strings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_style.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/blank_texture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/blank/create_missing.cpp
    # model — the neutral editing core: the document lifecycle and the undo journal, a batch's
    # rows before it commits, what a field shows, the find over a document's fields, and the
    # finding codes' rows (S13 A6: the tables' shape and the editor's own table)
    ${CMAKE_CURRENT_SOURCE_DIR}/model/document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/document_base.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/document_search.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/edit_history.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/field_text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/finding_code_row.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/staged_rows.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/table_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/text_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/model/text_history.cpp
    # documents — the document types: the def catalogs, their symbol tables and validation;
    # the menu clipboard's one rule for the tree and the canvas; the types' project checks, one
    # per type that has one, kept between validations
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/def_catalog_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/def_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/strings_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_clipboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mnu_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_clipboard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_logic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_sentence.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_uses.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_validation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_reads.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_file_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mission_labels.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/name_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/mns_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/model_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/model_document_edits.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/model_table.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/animation_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/animation_map_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/text_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/script_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/music_script_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/credits_type.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/document_types.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/project_checks.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/catalog_validation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/source_issue_findings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/documents/validation_cache.cpp
    # graph — the asset graph: the extractors, the reference kinds' table, the per-file slots and
    # their indexes, the one resolver, a base layer's names, the reference queries a document's
    # fields ask, the rename transaction; what other files make of what one defines (the use
    # checks, and what a stylesheet line's value is used as) and the one validation pass over the
    # project's files; the display names (a value as a modder reads it, the graph as the names)
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/asset_graph.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/display_names.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/extractors.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/graph_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/graph_layer.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/project_validation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/reference_kinds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/reference_queries.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/rename_transaction.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/style_value_use.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/graph/use_checks.cpp
    # import — the importers (a PNG to the game's PCX or TGA) and what they read (the import
    # context), the sidecars, the import pass, and the plan of an import with the files it needs
    ${CMAKE_CURRENT_SOURCE_DIR}/import/converter.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_context.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/importers.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_pass.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/import_run.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/mission_fixed_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/png_decode.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/quantize.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/import/sidecar.cpp
    # preview — the viewports (S13 V5): their kinds' table, the session's viewports and the preview
    # clock, the follow every kind shares (the files a picture read, the action its device takes),
    # the base every kind's viewport derives from, the Shell's devices (an LRU of four keyed by
    # document and kind) and the one envelope on the wire; the menu's viewport (its screen compiled
    # headless, the geometry its hit tests, handles and drags read) and the model's (its orbit camera
    # and the level the game draws, its rig and the clip it plays); the headless render of a screen,
    # the render check over every menu (the menu type's project check: the compiler notes as
    # findings), and the texture header probe it measures with; what a drag of a window's handles
    # writes, and what arranging several windows (align, distribute, drawing order) writes; a menu's
    # tree, its findings and a screen as the render check compiled it, as the menu_tree,
    # menu_findings and menu_render queries read them; the canvas's one gesture machine, and the
    # kinds' halves of it: what a press on the menu's or the model's picture takes, what its drag
    # writes and what is drawn over the picture; the script device's viewport (S13 V10: a text
    # document's text as its Godot control shows it, the span edit a control's text plans, the
    # keystroke burst its edits fold in, the gutter marks and the highlights it colours)
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/canvas_gesture.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_arrange.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_canvas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_layout_edit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_render_check.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_report.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_screen_render.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/menu_viewport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_canvas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_handle_edit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_items.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_options.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_scene.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_source.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/mission_viewport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_canvas.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_handle_edit.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_overlay.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_camera.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_preview_rig.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/model_viewport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/script_viewport.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/shown_text.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/text_burst.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/texture_header.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewport_device_cache.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewport_follow.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewport_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewport_kinds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewport_model.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/preview/viewports.cpp
    # project — project.opennova, the cache directory, machine-local settings, file plumbing,
    # and the one refresh (import, scan, requirements) and the one findings composer the
    # editor and the CLI share
    ${CMAKE_CURRENT_SOURCE_DIR}/project/local_settings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_document.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_files.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_findings.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project/project_refresh.cpp
    # project_build — routing into the boot-table archives, the plan, the immutable build
    # (the directory is not "build*": the lints skip those)
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/archive_routing.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/build_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/project_build/build_run.cpp
    # requirements — the manifest rows evaluated against the project
    ${CMAKE_CURRENT_SOURCE_DIR}/requirements/requirements.cpp
    # run — the Play launch plan, the run directory the game runs in, and the one managed child
    # over the platform seam
    ${CMAKE_CURRENT_SOURCE_DIR}/run/launch_plan.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/run/play_lease.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/run/play_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/run/run_directory.cpp
    # session — the one open project and everything the editor does to it (ADR 0046 d10):
    # the facade and its parts (S13 A2: the core, the open documents, the Problems rows, Play,
    # imports, renames, the unsaved guard, the preferences over their store), what Problems
    # shows of its findings and offers to do about them, the request and query tables (S13 A4,
    # A5) and their wire forms (session_json, view_json), the selection (S13 D7), and the
    # finding codes' lookup over every table (S13 A6)
    ${CMAKE_CURRENT_SOURCE_DIR}/session/build_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/document_set.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/editor_preferences.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/editor_queries.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/file_preferences_store.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/finding_codes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/import_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/import_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/import_plan_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/mission_logic_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/open_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/operation_progress.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/outcome_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/output_log.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/play_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/problem_fixes.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/problem_query.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/problems_service.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/project_session.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/record_batch.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/refresh_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/rename_controller.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/rename_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/request_fields.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/request_kinds.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/selection.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/session_core.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/session_json.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/session_operation.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/unsaved_guard.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view_json.cpp
    # session/view — the view the windows draw, an aggregate of sub-views (S13 V4: the project,
    # the documents, the findings, the activity, the dialogs), the events a request posts, and
    # the findings by file and record
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/activity_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/dialogs_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/findings_index.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/project_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/session_view.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/view_events.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/session/view/viewport_kind.cpp
)

# ui — the Dear ImGui windows on the engine's ImGui pass (ADR 0046 d11). Built when the
# pass is (OPENNOVA_IMGUI: the dev-tools or the editor flavour); OPENNOVA_EDITOR_UI tells a
# consumer whether the windows exist in this build.
if(OPENNOVA_IMGUI)
    target_sources(opennova_editor PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_toolbar.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_views.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/document_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/editor_windows.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/field_widgets.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/files_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/find_cursor.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/import_dialog.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/inspector_layout.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/inspector_window.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/main_viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/menu_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/menu_viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/mission_logic_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/mission_viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/model_viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/outline_model.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/outline_view.cpp
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
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/script_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/script_viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/styles_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/text_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/text_edit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/ui_kit.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/viewport_canvas.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/viewport_view.cpp
        ${CMAKE_CURRENT_SOURCE_DIR}/ui/viewport_views.cpp
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
