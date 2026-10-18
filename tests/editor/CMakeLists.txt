# The OpenNova Editor's ctest rows (ADR 0046), added by tests/CMakeLists.txt. Each executable is built
# where that folder's own are (build/tests, under its config's folder on a multi-config generator) and
# each row runs from build/tests, as when the rows were that file's; tests/ is the include root, and a
# row's sources are this folder's unless its path says otherwise.
get_filename_component(OPENNOVA_TESTS_SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}" DIRECTORY)
get_filename_component(OPENNOVA_TESTS_BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}" DIRECTORY)

# The editor's test sources compile unoptimized in Release: each spent 30-60 s in MSVC's optimizer (in
# clang the same TU takes 27 s at -O2 and 10 s at -O0), while what they test, the engine libraries, keeps
# its optimization. The flags are this directory's own variable, so only the targets made here take them
# (editor_ui_test's tests/editor_ui sources among them); NDEBUG stays.
if(MSVC)
    string(REPLACE "/O2" "/Od" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
    string(REPLACE "/Ob2" "/Ob0" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
else()
    string(REGEX REPLACE "-O[23]" "-O0" CMAKE_CXX_FLAGS_RELEASE "${CMAKE_CXX_FLAGS_RELEASE}")
endif()

# opennova_add_editor_executable(<target> SOURCES <files...> LIBS <libs...>): an editor test's
# executable, linked PRIVATE against LIBS, tests/ its include root, built into build/tests.
function(opennova_add_editor_executable target)
    cmake_parse_arguments(E "" "" "SOURCES;LIBS" ${ARGN})
    if(E_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "opennova_add_editor_executable(${target}): unknown arguments ${E_UNPARSED_ARGUMENTS}")
    endif()
    add_executable(${target} ${E_SOURCES})
    target_link_libraries(${target} PRIVATE ${E_LIBS})
    target_include_directories(${target} PRIVATE ${OPENNOVA_TESTS_SOURCE_DIR})
    set_target_properties(${target} PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${OPENNOVA_TESTS_BINARY_DIR})
endfunction()

# opennova_add_editor_test(<stem> [MIXED [RETAIL_PARTS <part>...]] [NAME <ctest name>] [SOURCES <files...>]
# [LIBS <libs...>]): the ctest row NAME (editor_<stem> unless given) running the executable <NAME>_test,
# built from SOURCES (<stem>_test.cpp unless given) and linked against LIBS (opennova_editor unless given).
# MIXED registers it through opennova_add_mixed_test: the row and its _retail twin, which RETAIL_PARTS
# splits by the program's parts (tests/common/test_parts.h): the twin runs the first, and
# <NAME>_retail_<part> each other, side by side (every case makes its own project in a pid-unique
# TempDir, tests/common/temp_dir.h, so the parts need no lock between them). TIMEOUT gives a row
# its own bound in seconds, under ctest's default (600 for the rows whose bakes or build take every core
# and run beside the others: the longest took 51 s on CI).
function(opennova_add_editor_test stem)
    cmake_parse_arguments(T "MIXED" "NAME;TIMEOUT" "SOURCES;LIBS;RETAIL_PARTS" ${ARGN})
    if(T_UNPARSED_ARGUMENTS)
        message(FATAL_ERROR "opennova_add_editor_test(${stem}): unknown arguments ${T_UNPARSED_ARGUMENTS}")
    endif()
    if(NOT T_NAME)
        set(T_NAME editor_${stem})
    endif()
    if(NOT T_SOURCES)
        set(T_SOURCES ${stem}_test.cpp)
    endif()
    if(NOT T_LIBS)
        set(T_LIBS opennova_editor)
    endif()
    opennova_add_editor_executable(${T_NAME}_test SOURCES ${T_SOURCES} LIBS ${T_LIBS})
    if(T_RETAIL_PARTS AND NOT T_MIXED)
        message(FATAL_ERROR "opennova_add_editor_test(${stem}): RETAIL_PARTS splits a MIXED row's twin")
    endif()
    if(T_MIXED)
        opennova_add_mixed_test(${T_NAME} COMMAND ${T_NAME}_test RETAIL_PARTS ${T_RETAIL_PARTS}
                                WORKING_DIRECTORY ${OPENNOVA_TESTS_BINARY_DIR})
    else()
        add_test(NAME ${T_NAME} COMMAND ${T_NAME}_test WORKING_DIRECTORY ${OPENNOVA_TESTS_BINARY_DIR})
    endif()
    set(rows ${T_NAME})
    if(T_MIXED)
        list(APPEND rows ${T_NAME}_retail)
        if(T_RETAIL_PARTS)
            list(SUBLIST T_RETAIL_PARTS 1 -1 others)
            foreach(part IN LISTS others)
                list(APPEND rows ${T_NAME}_retail_${part})
            endforeach()
        endif()
    endif()
    if(T_TIMEOUT)
        set_tests_properties(${rows} PROPERTIES TIMEOUT ${T_TIMEOUT})
    endif()
endfunction()

# The OpenNova Editor's core (ADR 0046): the project file, the asset registry, the
# requirements checklist, and the opennova-project commands driven end to end.
opennova_add_editor_test(project_document)

# A project's expansion (ADR 0046 S16): the names the game takes for one, weighed against an install's;
# the files its name forms; the install as the game serves it; a session's expansion project.
opennova_add_editor_test(expansion)

opennova_add_editor_test(asset_registry)

# The asset kinds' own columns (S13 D5, ADR 0046 S13): every document type is a kind's, the build
# leaves out exactly the kinds of no slot, and a new file's name fits its kind's extensions (that
# every runtime format and required file has a kind and a place is file_kind's, above).
opennova_add_editor_test(asset_kinds)

opennova_add_editor_test(requirements)

opennova_add_editor_test(blank_factory)

# DI-33: every kind a reference or the boot manifest names that the engine has a writer for gets a blank,
# read back through the engine's reader of its format and opened by its document type.
opennova_add_editor_test(blank_writers)

# A terrain made from images (S20): the terrain set, the heightmap's depths, the new_terrain request
# over a project (the import, the scan's kinds, the build's packing), the outputs read back and loaded
# by the runtime's terrain field and frame compiler, the import rerun-stable, the blank environment
# and a mission made on the terrain; the surface map read, written and sampled by the runtime (its
# _retail entry reads a shipped char map as the importer does).
opennova_add_editor_test(terrain_import MIXED)

opennova_add_editor_test(create_missing)

opennova_add_editor_test(project_build TIMEOUT 600)

opennova_add_editor_test(play_session)

# Play's log as Problems rows (ADR 0046 DI-27): OpenNova's misses on the files and records naming
# them, with their Go to and fixes; the game install's file and graphics logs read after exit; each
# mode's rows kept until the next Play of that mode.
opennova_add_editor_test(play_log)

# The neutral editing core at any depth (S9g): a fake document type's walk, placements,
# locators, structural edits and refusals, batches, gestures, the clipboard seam and the
# selection model.
opennova_add_editor_test(document_core)

opennova_add_editor_test(session SOURCES editor_session_test.cpp)

# Per-file validation (S13 D4): each file's own findings kept in the validation cache by what
# they were made from, a closed file read and let go; what other files make of what one defines
# (graph/use_checks); the project's rows as the one pass makes them, the trunk's before the
# split pinned over the fixtures; every finding composed carries a finding code's row (S13 A6).
# Retail leg (OPENNOVA_JO_DIR): the install's files a document type opens validated, every
# finding composed a row's, then an edit of one validating it alone.
opennova_add_editor_test(project_validation MIXED)

# Long operations everywhere (S13 A3): the walks stepped by bytes (the scan, the import pass, a
# scan's targeted update), the validation a file at a time and a rename's commit a file at a time,
# each equal to its run in one call; the session's Open, Rescan, import and rename stepped a poll
# at a time (progress only rising, finished once, cancelled leaving nothing, the view a run to the
# end makes), a Save scanning and validating its one file, a stale import refused without writing.
# Retail leg (OPENNOVA_JO_DIR): a project of the JO install's files opened a step at a time at the
# editor's budget, its polls and wall time, and the install as the base layer under an expansion
# project's graph, a row of its own.
opennova_add_editor_test(long_operations MIXED RETAIL_PARTS main install_open)

# The document types' project checks (S13 V9): a type's own check across the project's files,
# made by its registry row (the menu type's render check) and kept by whoever validates; a
# stand-in type's check runs once per validation, its findings after the build's gate, and its
# answer that they moved is what composes the Problems rows again.
opennova_add_editor_test(project_checks)

# The session's operations (S13 A1): the slot stepping a fake operation within a poll's budget,
# cancelled between two steps, finished once; the busy gate's four answers over a real build
# (refused, joined, superseded as the table says, cancelled first); edits during a build.
opennova_add_editor_test(session_operation SOURCES editor_session_operation_test.cpp)

# The session's parts (S13 A2): the editor's preferences over their store (the memory store's
# round trip, the file store's settings file byte for byte, a session over each), the open
# documents' one position rule and remembered selections, and the unsaved-changes prompt driven
# by the request table's guard column for every request kind.
opennova_add_editor_test(preferences SOURCES editor_preferences_test.cpp)

# The project, the import and Files as a modder meets them (ADR 0046, the UX round's project lane): a kind
# named in words, the folders each kind lands in, the files a filter lists, a file's card and its requests.
opennova_add_editor_test(project_lane)

# What the windows show of their own, held by the session (ADR 0046, the MCP gaps lane): set_workspace and
# the workspace section, each part's wire form, the refusals.
opennova_add_editor_test(workspace_parts)

# The navigation history (CONTEXT.md "Navigation history"): the history alone on a stepped clock, what is
# a step of it in a session, Back and Forward showing a place again, the wire's requests and section.
opennova_add_editor_test(navigation)

# The jumps a keystroke or a right-click makes (DI-18, editor/graph/jump_queries): a search in one scope, ranked;
# Find usages of a file or a record; Go to definition from a record; the project_search query's scope, the usages
# query's locator and the finder's workspace part on the wire; an item record's Place in mission.
opennova_add_editor_test(jumps)

opennova_add_editor_test(document_set)

# A document type of another kind than records (S13 D6), a test's standing in for the
# stylesheet's: the graph, the validation, the rename and the session over it.
opennova_add_editor_test(non_record_document)

# The text documents and their types (S13 D9): the model's spans, history and change sets; the
# script's compiler findings and operand references with spans; the graph's span edges, the
# fallback name, Rename everywhere through a span, a Go to's RevealText; the wire's span edits and
# pages of lines; the credits, music script and shader codecs byte for byte; and the install's
# scripts, music scripts, credits and shaders (OPENNOVA_JO_DIR) as the retail leg.
opennova_add_editor_test(text_document MIXED)

# The environment document (the deep-integration plan's DI-19a): a .env through the engine's own
# reader and writer, its fields in the file's units and the game's rules, its keyframes, its source
# findings, its references through the graph, the missions that run on it (the environment_uses
# query); the retail leg (OPENNOVA_JO_DIR) reads every .env of the install through the document and
# writes it again, field for field.
opennova_add_editor_test(environment_document MIXED)

# The terrain document (the deep-integration plan's DI-30): a .trn through the engine's own reader and
# writer, its keys in the file's units and the game's rules, its grid rows and foliage definitions, its
# source findings, the gate's refusal and the inert definitions with their fixes, its references through
# the graph, the missions that run on it and the import that makes it (the terrain_uses query), an import's
# terrain refused every edit (document.imported); the retail leg (OPENNOVA_JO_DIR) reads every .trn of the
# install through the document and writes it again, field for field.
opennova_add_editor_test(terrain_document MIXED)

opennova_add_editor_test(unsaved_guard)

# The session's wire form (the editor MCP): request tokens both ways, the view, a
# document and a record as JSON over a real session.
opennova_add_editor_test(session_json)

# The finding codes (S13 A6): every token its own across the editor's table and the document
# types' as registered (a stand-in hiding none), each row found by its token, a type's rows its
# group's family, the render check's a row per compiler note and the stylesheet reader's a row
# per reader code, the columns deciding what the codes' spelling decided before (and the
# spelling agreeing), a finding keeping its row, and its subject on the wire under the keys it had.
opennova_add_editor_test(finding_codes)

# The Problems core (S11b): the query the window and the editor MCP share (severities,
# text, scope, only the fixable, grouping), the fixes for each finding and a Fix all
# merged from them, where a finding takes Problems, and the findings by file and record
# (S13 V1: FindingsIndex).
opennova_add_editor_test(problems)

# The fixes that edit a document (ADR 0046 DI-11): a finding's planned edits of its own file
# (an SSN, a zone id, a name or a key of its own, a string removed, an end pose's trigger
# moved), an item's id of its own and an unused stylesheet variable's line removed, each
# applied over a real session as one step Undo takes back.
opennova_add_editor_test(problem_edit_fixes)

# Missing names offer "Add it there" (ADR 0046 DI-15): a record of the name's kind added to the
# file where the game looks it up, named as referenced, born as the type's own Add makes one
# (a weapon, an ammo, a powerup, an item on its id by the reserved-id rule, a string id, a
# screen, a window, a style variable, a sound set, a sound profile, an effect), one step Undo
# takes back, over a real session.
opennova_add_editor_test(add_it_there)

# Definition fields complete their references (ADR 0046 DI-09): every def field the game resolves by name a
# reference of its kind in the scope its reader reads (user points by the lookup that scans them, an action's
# slot in its weapon's map, an ammo's tracer items by type id), and the complete query over them.
# The character attributes document (ADR 0046 DI-09's charattr follow-up; S23 B its rows): charattr.def's read
# classes as rows over the game's loader with the file's layout, a value's own line, a class added and the last
# removed, the camouflage items as references, the game's words for a missing one, a rename writing the type id
# back, the unread sections and the type's other findings; --retail: the install's charattr.def (base and each
# expansion) through the document byte for byte.
opennova_add_editor_test(charattr MIXED SOURCES charattr_document_test.cpp)

opennova_add_editor_test(def_references)

# The menu clipboard's one rule for the tree and the canvas (S13 V1, documents/mnu_clipboard):
# a window a part holds is copyable from neither, a Paste after the primary's window.
opennova_add_editor_test(mnu_clipboard)

# The viewports' one canvas with no ImGui (S13 V2, V5: preview/canvas_gesture, menu_canvas,
# model_canvas): the one gesture machine and its single EndEdit; the menu's canvas over a
# headless render (a click, a drag of a window and of a handle on the grid, the arrows, Esc,
# several windows, the marquee's one selection, the clipboard's rule, Arrange, a part's window,
# what it draws); the model's canvas over a real session's model viewport (F, a click on a marker,
# a marker's drag ended once, the orbit and the dolly as SetViewports of its camera).
opennova_add_editor_test(canvas)

# The windows' models with no ImGui (S13 V1): what the Problems window lists, folds, proposes
# and asks (ui/problems_list), where the find bar is among a document's hits (ui/find_cursor),
# and what a document's outline lists (ui/outline_model, S13 V3). Built with the windows (their
# sources are ui/'s).
if(OPENNOVA_IMGUI)
    opennova_add_editor_test(problems_list)

    opennova_add_editor_test(find_cursor)

    # A document's outline (S13 V3, ui/outline_model): its lines as a list, a tree and master and
    # detail, what is open, the filter, the order, and the lines made again only when what they read
    # moves.
    opennova_add_editor_test(outline_model)
endif()

# The asset graph (S7): the extractors, the resolver the badges and Problems share,
# the per-file slots an update patches (an incremental update equals a fresh build, S13 D3),
# the base layer, and the rename transaction; its retail legs over the JO install, the incremental
# leg's six fresh-graph comparisons split over three rows of their own (166 s in one row).
opennova_add_editor_test(asset_graph MIXED RETAIL_PARTS main incremental_1 incremental_2 incremental_3)

# A document's reference queries (S13 D3, graph/reference_queries): the badge, the missing
# finding, the picker, where Go to leads, and find_definition over the graph's slot or the
# document alone.
opennova_add_editor_test(reference_queries)

# Find (S12 D8): every document type's fields found as the Inspector shows them (a def's number
# in its written units, a choice by its name, any case unless it must match), and the project's
# files and symbols by name with their usages (editor/model/document_search, AssetGraph::search).
opennova_add_editor_test(document_search)

# Rename everywhere (S12 D9): one test per kind of name (a menu screen used from another menu, a
# style variable brand.mns defines again, a string key two sections define, a weapon's and an
# ammo's name, an item id a mission names, a model's user point), each renamed with exactly the
# uses that reach it, or refused naming the file (graph/rename_transaction, the session's
# PreviewRename and RenameSymbol).
opennova_add_editor_test(symbol_rename)

# The document type contract (S12 Z, ADR 0046 S12 "Adding a document type"): every registered
# type over a file of its kinds loads, serializes stably, labels its fields, keeps field_on's
# read-only, sets a field to its own value with no byte changed, and defines each name as a symbol
# find resolves, every reference kind with its reference_kinds row.
# The texture viewer (ADR 0046 S18): a texture file as the game's readers read it, its facts in words
# (documents/texture_image), the texture document, and its viewport through a session (Files'
# selection previewed from its file, the viewport query and SetViewport, the texture opened, the item
# using it); the retail leg (OPENNOVA_JO_DIR) a sample of the install's textures of each extension.
opennova_add_editor_test(effect_viewport)

opennova_add_editor_test(texture_document MIXED)

# ADR 0046 S18, the texture roles: the catalog (documents/texture_roles: every way the game uses a
# texture, its loader, formats, size rule and witness) and the loaders' name rules and transforms
# (documents/texture_load_rules), against vectors worked by hand; the texture_roles query; the graph
# resolving each texture reference by its loader, a terrain's colour map and blend map refusing a
# build; the retail leg (OPENNOVA_JO_DIR) the install's texture references by role, none refusing one.
opennova_add_editor_test(texture_roles MIXED)

# ADR 0046 S18, the texture thumbnails (preview/texture_thumbnails): a texture as a small picture, the
# PNG writer, the session's cache made by its poll, the texture_thumbnail query and a texture field's
# record and choices naming the file its loader opens.
opennova_add_editor_test(texture_thumbnails)

# ADR 0046 S18, the texture uses (graph/texture_uses, session/texture_use_index): each texture's uses by
# role over a minted project, a model row by its material, a .tga beside the .dds its loader opens, a
# terrain's, a sky's, a HUD image's, a name the game opens itself; the texture_uses query; a use shown
# where the game draws it (session/texture_show_use: on its model, in its menu, in a mission's view, as used).
opennova_add_editor_test(texture_uses)

# ADR 0046 S18, the texture checks: a texture file's own findings from its header and its uses' (the
# texture use check, graph/texture_checks) over a minted project, which refuse a build; the retail leg
# (OPENNOVA_JO_DIR) every shipped texture's own findings and every terrain's colour map, none refusing one.
opennova_add_editor_test(texture_checks MIXED)

# ADR 0046 S18, the texture budget (documents/texture_budget over renderer/device_texture): what a model
# row's texture costs the game at each object texture detail and as its .dds; over a minted project the
# texture_uses query's budgets, texture.memory and texture.normal_slot_loader with its fix, the
# texture_budget query (the project's textures and their totals); the retail leg (OPENNOVA_JO_DIR) every
# shipped model's texture rows costed, none past the warning.
opennova_add_editor_test(texture_budget MIXED)

# ADR 0046 S18, a texture as the game draws it: the chain the game builds, the object texture detail's halvings,
# a normal map lit, a model row's alpha by its technique; through a session the texture viewport's level, detail,
# normals and light, and a Phong diffuse shown as its use draws it.
opennova_add_editor_test(texture_game_view)

# ADR 0046 S18, each texture role in its own picture (preview/texture_role_view): a blend map's weights and the
# splat details they weigh, a foliage map's codes and what grows on them, a particle graphic as its atlas page holds
# it; through a session the texture viewport showing each as its use, its legend read again as its terrain moves.
opennova_add_editor_test(texture_role_view)

# ADR 0046 S18, a texture's import (import/texture_import, graph/texture_import_needs,
# session/texture_import_state): the image importer's option rows and output name, its sources imported
# (the pieces and formats are renderer_texture_authoring's), what a minted project's uses ask of an import,
# set_import_options and the import_options query.
opennova_add_editor_test(texture_import)

# ADR 0046 S18, the texture document's edits (documents/texture_operations, texture_operation): each
# whole-image operation one undo step written through the editor's writers, Undo, Redo and Save; an
# import's output and an operation it cannot do refused; the Problems fix of an upside-down TGA's rows.
# The HUD preview over hudpos.def (the deep-integration plan's DI-20): the HUD layout type and the HUD
# viewport through a session, its canvas.
opennova_add_editor_test(hud_viewport)

opennova_add_editor_test(line_ends)

# The editor's rule over the ConfigFile text reader's data-strings pool (documents/config_overrun.h,
# document.config_overrun; the pool itself, sized by the text values' bytes, rounded up to 64, cleared one byte
# per value [orig: ConfigFile_ParseText @ 0x7609e8], is configfile_data_strings_pool's): the finding and its fix
# on a charattr.def (fixtures/configfile/pool_past_line.def) and a credits file, the blanks, a session's row and
# the build it refuses;
# the retail leg every file JO:CA reads through the text reader (charattr.def, the menus' DATASOURCEs), none past
# its pool.
opennova_add_editor_test(config_overrun MIXED)

opennova_add_editor_test(texture_document_edit)

# ADR 0046 S18, a texture made from an image (import/texture_source, replace_texture) and split
# (plan_split, split_texture): the options that reproduce a texture; a plain texture and an import's output
# replaced, a missing name made, the refusals; a texture split for one referrer; the sources to edit.
opennova_add_editor_test(texture_replace)

# ADR 0046 S18, the external program round trip (edit_externally, refresh_changed_sources): a plain TGA
# given a source once and opened, a change saved there imported and its document read again, nothing done
# for nothing changed, an output's own source, a missing name; the Shell's check leaving a refusal's line.
opennova_add_editor_test(texture_external)

# A .tga a model's rows name stored as the .dds of its name, which their loader reads first (ADR 0046
# S18, store_as_dds): a plain file and an import's output; the uses that read the .tga itself refused;
# the build packing the .dds alone.
opennova_add_editor_test(texture_store_dds)

# ADR 0046 DI-01, outside saves come back (editor/assets/disk_changes.h, editor/session/disk_watch.h): a
# file read once it holds still, the folders' made and gone files, the focus-in sweep; an open catalog's
# outside save read again with its selection kept, a conflict raised at once with Keep my edits.
opennova_add_editor_test(disk_changes)

# A file's rename through the native texts (ADR 0046 S18, graph/native_text_sites): a terrain's, an
# environment's, a particle file's and the HUD layout's names rewritten as text, only the token the game
# reads at each site; a flipbook's frame refused; a file in the SCR form written back as plain text. The
# retail leg (OPENNOVA_JO_DIR): every name the install's native texts write found as its one token, the
# names dealt over four rows (132 s in one row).
opennova_add_editor_test(native_rename MIXED RETAIL_PARTS main share_2 share_3 share_4)

opennova_add_editor_test(document_contract SOURCES editor_document_contract_test.cpp)

# The project's files by logical name, the open documents standing in for theirs
# (editor/assets/project_asset_source.h).
opennova_add_editor_test(project_asset_source)

# The viewport seam (S13 V5, editor/preview/viewport_*, viewports, viewport_device_cache) over
# devices that draw nothing and a real session: the actions a device takes (Keep, Rebuild, Update,
# Clear) from the document's change, the files its picture read and the viewport's options; the
# failure latch; the preview clock; two menus' own options; the device cache's four, its least
# recently used given up and its viewport's camera kept; the envelope and its pages; a SetViewport,
# a drag and a command through the session; one undo step per gesture; S13 V6's builds over frames
# (loading then ready, a newer generation cancelling, an Update folded, a Clear and a failure
# mid-build, the frame's budget). S13 V8: the change sets a viewport follows (a later screen's edit
# and a variable only a later screen names Keep, a screen before the shown one a Rebuild, a user
# point's move an Update over the scene that stands, the menu read again everything), the model's
# scene held until the gesture's end, the menu's caret on the preview clock; its retail leg a
# shipped screen's edit box blinking its caret on the clock (OPENNOVA_JO_DIR).
opennova_add_editor_test(viewport MIXED)

# The script device's portable half (S13 V10, editor/preview/script_viewport, shown_text,
# text_burst): ViewportKind::Script in the kinds' table, the Main role of every text type; the span
# diff from a control's text, minimal and byte-exact against the TextDocument (an insert, a delete,
# a replace across lines, CR LF kept, a paste of several lines, a lone CR and a NUL kept, a code
# page's character stored, one it has none for refused); the keystroke burst; the gutter marks
# from the findings; the compiler's words highlighted; through a real session, a burst served as one
# undo step, the follow's actions, a Go to's span selected, a document held read only and the busy
# gate, the envelope and a SetViewport; and the install's scripts (OPENNOVA_JO_DIR) as the retail
# leg: their highlights tokens of their lines, their shown text's lines the documents'.
opennova_add_editor_test(script_viewport MIXED)

# Edits in a viewport over the wire (S13 V7: edit_in_viewport through a session's JSON requests): its
# refusals, a drag one batch over the selection, a command one request; the wire's gestures (a
# snapped gesture where a canvas's lands, refused and forged tokens, interleaving, what ends one,
# the 10 s lapse); the menu's rounding and its drag to a point; a model's drag by nothing, its frame
# beside any operation; the clock set with no document named; a model read at a canvas's size.
opennova_add_editor_test(viewport_wire)

# What a drag in a mission's viewport writes (editor/preview/mission_handle_edit, ADR 0046 S14) and the
# mission camera (editor/preview/mission_camera), over a fake record document that carries the
# mission's field tokens: a move on the plane and on a fake ground with and without stick, the snap,
# the offsets kept, a sample within a 16.16 step, the lift, the turn's wrap and snap, an area's
# edges; the camera's wire yaw and pitch against presentation_forward_from_angles, a look, a fly,
# the ground point, top, the wire form.
opennova_add_editor_test(mission_handle_edit)

# The mission viewport's portable half that reads no document type (ADR 0046 S14): the scene over
# a source a test holds (editor/preview/mission_scene: read, the patch reading the changed rows
# alone, a reshape reading everything), its marks, picks and boxes, the options' wire form
# (mission_options) and the overlays (mission_overlay: the glyphs, the rings, the primary's handles
# with a yaw-90 entity's east, an area's box, a path's lines, the labels, the marquee, a line clipped
# at the near plane).
opennova_add_editor_test(mission_scene)

# The mission viewport (editor/preview/mission_viewport, ADR 0046 S14) over a real session, a
# project holding the minted mission (fixtures/bms/synth_logic.bms) and fake devices: the kind's
# row, its status and follow, what each change set comes to (an entity's transform an Update, an
# event's field Keep with no row read, an Add and the header's terrain a Rebuild), a Rebuild held for
# a gesture, a file the device read moving, its camera on the wire, its hits and boxes, its envelope,
# a drop and the ground command; DI-07, the ground under a point in the game's words over a minted
# terrain with a char map (class, footstep slots, effects row, a placed tile, the water). Mixed: the
# retail row opens every shipped mission (OPENNOVA_JO_DIR) in its viewport, hits its marks and moves
# its first item and back.
opennova_add_editor_test(mission_viewport MIXED)

opennova_add_editor_test(mission_map MIXED)

# A mission's items' effects as its start attaches them (ADR 0046 DI-31, editor/preview/mission_effects) over
# a real session: each placed entity's particle slot by the start's pool gates and its model's points, the
# emitters under the entity's transform and the item's scale, played on the preview clock and spawned again
# pre-aged on a jump; a move carrying a slot's groups, an item swapped, a removal, a particle file edited, the
# layer switched off; the options' new layers and the wire.
opennova_add_editor_test(mission_effects)

# DI-36: the mission view's Listen (editor/preview/mission_listen, mission_script_run): the sources by the hour and
# their reach through the runtime's AmbientMixer, the eight channels, the rain beside the camera and the thunder of the
# script's weather run by the engine's kernel, the wire and the overlay.
opennova_add_editor_test(mission_listen)

# The surface classes and the foliage over a mission's terrain (ADR 0046 DI-29, editor/preview/
# mission_ground_overlay): the ground's foliage at a point and the two overlays over a minted Tmap.
opennova_add_editor_test(mission_ground_overlay MIXED)

# Place a model no item draws yet (DI-12, editor/preview/model_placement, editor/documents/model_item) over
# a real session and the synth models: the item a model is drawn as from its parts; a model no item draws
# dropped on a mission making its item in items.def (the next free id, its TYPE, its graphic), then placed,
# an undo step in each file; the refusals; a model's Place in mission arming the mission's Place tool. Mixed:
# the retail row holds the rule to every model the shipped ITEMS.DEF draws (OPENNOVA_JO_DIR).
opennova_add_editor_test(place_model MIXED)

# A mission's people posed as the game spawns them (DI-38, editor/preview/mission_poses) over a real
# session: each organic's spawn pose held to the world's own organic init over the same clips (pool
# order, the shared ring heads), why one is not posed, the route and Guarding read through an aidata
# definition's AI slot alone, the poses following the records and the catalog live, the wire. Mixed:
# the retail row poses every shipped base-game mission's people over the game's catalog and clips and
# holds each pose to the world init's (OPENNOVA_JO_DIR).
opennova_add_editor_test(mission_poses MIXED)

# An entity off the ground is a Problem with a fix (DI-28, editor/preview/mission_ground_rules and the
# mission type's project check, mission_ground_check): each entity class grounded by the game's own
# rule (a static drawn where stored on the terrain, on another's solid or on the water; a person set
# down by the warmup or standing on a floor; a mover the game's to place), the findings and their fixes
# over a real session, followed live. Mixed: the retail row runs the rules over every shipped mission
# of the base game and counts what they flag (OPENNOVA_JO_DIR).
opennova_add_editor_test(mission_ground MIXED SOURCES mission_ground_check_test.cpp)

# The menu's viewport (editor/preview/menu_viewport, S9j, S13 V5) over a headless device: its
# screen, its reason, when its device configures again, the envelope, its drag and arrange.
opennova_add_editor_test(menu_viewport)

# The model's viewport (editor/preview/model_viewport, S10p2, S13 V5): what it shows, when it
# builds again (a user point's edit builds nothing), the camera's projection, the level Auto picks,
# the envelope, what a SetViewport sets, the drag of a marker, a clip on its rig.
opennova_add_editor_test(model_viewport)

# DI-04: a clip's events heard in the model preview, on the game's ticks, through the game's rules.
opennova_add_editor_test(clip_sounds)

# DI-04's player: the preview's waves decoded once and kept until their files change; a clip sound's voices wait
# for their waves through a rescan (a sound fired in the frame a save lands still plays).
opennova_add_editor_test(preview_waves)

# DI-34: a menu's sounds heard in its preview: the game's mouse over the picture runs the game's pump of the
# windows' sounds (the runtime's MenuSoundPump), each row's set from its own bank at the menu's volume.
opennova_add_editor_test(menu_sounds)

# DI-35: a menu tried in its preview (Try mode): the picture behaving as the game's menu through the runtime's
# own driver, its sandbox, what would leave the menu said instead of done.
opennova_add_editor_test(menu_try)

# DI-24: a clip's fire events fire the item's ammo in the model preview (preview/preview_clip_fire, the weapon
# range's soldier shots), over minted fixtures alone.
opennova_add_editor_test(clip_fire)

# DI-13: a weapon's map in first person: the gun with the character's arms on its rig, the eye the game
# draws the view model before, the weapon's actions as the game's pump runs them and their sets.
opennova_add_editor_test(first_person)

# DI-10: a model's damage states named in the game's words and played as the game destroys the item (the
# husk swap, the pieces, the death sound, the destroy fade); the retail leg over JO's damageable items.
opennova_add_editor_test(model_damage MIXED)

# DI-21: a definition table's record drawn as the game draws the thing it defines (an item in its state with its
# effects and its death's sound, a person posed as it spawns, a weapon's models, an ammo's round); the engine's
# death banks lifted for it; the effects' playback over several closures.
opennova_add_editor_test(definition_viewport)

# A weapon fired in the definition preview as the game fires it (DI-22, editor/preview/weapon_range and
# definition_weapon): the game's local player in a range of its own, its legs, rounds, impacts and scars where the
# game's presenter draws them, the shots in first and third person, the gestures, the wire.
opennova_add_editor_test(weapon_fire)

# DI-23: the mission view's Shoot tool and an ammo's impact board, over minted fixtures.
opennova_add_editor_test(mission_shots)

# What a drag of a menu window's handles writes (editor/preview/menu_layout_edit, S9k1):
# the plan against the runtime's POSITION solve, the grid, the handles, the edits.
opennova_add_editor_test(menu_layout_edit)

# Arranging several menu windows (editor/preview/menu_arrange, S9k2): align, distribute
# and drawing order on synthetic rects, then on a real menu through its compiled screen
# (the hit walk pins the order), and a move of several windows.
opennova_add_editor_test(menu_arrange)

# The views the editor's effect schedule tests a NOVISNOUPDATE emitter against (editor/preview/effect_schedule): the
# orbit camera's planes, and the world pass's a mission's view takes (its far plane the fog's, S26).
opennova_add_editor_test(effect_schedule)

# The mission view's pick math (editor/preview/preview_pick, S14): a segment against a box, a triangle (either
# face), and the nearest face of a shape before a best parameter.
opennova_add_editor_test(preview_pick)

# The query seam (S13 A5): every list a query serves paged in C++ (the pages make the list,
# count the whole, an offset or a limit out of range refused naming the query), the state by
# section and since a revision, the catalog from the tables, the menu reads editor_menu served
# (its tree, its findings, the render check's render) and its batch as edit_record's wire form
# (records by identity or label, a list replaced, one undo step), over a real session.
opennova_add_editor_test(query SOURCES editor_query_test.cpp)

# The render check (S9j2): every menu screen compiled headless, its notes as Problems rows
# on their records (never a build's gate, never the graph's findings twice), rendered
# again only when something it read moved; a SKIP-LEG over OPENNOVA_JO_ASSETS' shipped
# menus (every screen renders in the document's order, every interned name a graph edge).
opennova_add_editor_test(menu_render MIXED SOURCES menu_screen_render_test.cpp)

# The imports (S8): the sidecars, the import pass, the scan and the build over the
# outputs, and the game install as an import source; the plan
# of an import with the files it needs (S11f, import_plan_test.cpp) and the session's
# import of it, staged and published (S11g, import_apply_test.cpp: its retail leg imports a
# packed install's menu with the files it needs, OPENNOVA_JO_DIR; 04TR's closure imported and built
# a row of its own).
opennova_add_editor_test(import MIXED RETAIL_PARTS main closure_04tr
                         SOURCES import_test.cpp import_plan_test.cpp import_apply_test.cpp)

# The font importer: a font set's glyph sheet made into a .fnt (formats/fnt fnt_sheet, whose advance
# modes, pages and refusals are the fnt_sheet ctest) and read back by fnt_parse: the set and the options,
# its run over an ImportContext, over a project (the set an import source, the .fnt packed into
# localres, a changed sheet imported again), and an import of a set from the disk bringing its sheet
# beside it.
opennova_add_editor_test(font_import)

# The editor's ImGui windows over a null backend (ADR 0046 d11): built with the pass. The
# workspace's shell (S11d: its layout, the Document tabs, Files, the menus, the modals) is
# workspace_test.cpp's; the change marks, the reveal, Output and the shortcuts (S11e)
# markers_test.cpp's; the bounds sweep (S11e: nothing runs past what shows of it)
# bounds_test.cpp's; the field controls (S12 D4) field_widgets_test.cpp's; the reference
# picker (S12 D7: each field's own filter, the keys, a Files row dropped on a value, a missing
# value's fixes) reference_picker_test.cpp's; find (S12 D8: the Document window's find bar, Find
# in project) find_test.cpp's; Rename everywhere from the Inspector (S12 D9) rename_test.cpp's; every
# control that raises a request enabled exactly when the busy gate takes it (S13 A1) gate_test.cpp's;
# a clip's timeline and the animation types' Inspector (S17) animation_test.cpp's; the project, the
# import, Files and the layout as a modder meets them (the UX round's project lane) project_test.cpp's;
# Back and Forward (the navigation history: the arrows, Alt+Left and Alt+Right, the mouse's buttons)
# navigation_test.cpp's; the jumps' keys and menus (DI-18: Go to file and name, Go to definition, Find usages, the Go
# menu, the right-click menus) shortcuts_test.cpp's; the mission logic (ADR 0046 S24: the Event board, the logic in
# the views, the live logic during Play) board_test.cpp's, space_test.cpp's and live_test.cpp's; the terrain tools in
# the views (ADR 0046 S25) terrain_test.cpp's; all share editor_ui/editor_ui_test_support.h. One ctest row per group (S13 V1: a failure names its
# group, the rows run in parallel), each running `editor_ui_test <group>`; editor_ui_groups holds
# this list and the program's table of groups to one list.
if(OPENNOVA_IMGUI)
    set(OPENNOVA_EDITOR_UI_DIR ${OPENNOVA_TESTS_SOURCE_DIR}/editor_ui)
    set(OPENNOVA_EDITOR_UI_SOURCES editor_windows_test.cpp workspace_test.cpp
                                   markers_test.cpp bounds_test.cpp
                                   field_widgets_test.cpp reference_picker_test.cpp
                                   find_test.cpp rename_test.cpp gate_test.cpp
                                   logic_test.cpp animation_test.cpp
                                   model_test.cpp project_test.cpp
                                   navigation_test.cpp shortcuts_test.cpp
                                   board_test.cpp space_test.cpp live_test.cpp
                                   terrain_test.cpp)
    list(TRANSFORM OPENNOVA_EDITOR_UI_SOURCES PREPEND ${OPENNOVA_EDITOR_UI_DIR}/)
    opennova_add_editor_executable(editor_ui_test SOURCES ${OPENNOVA_EDITOR_UI_SOURCES}
                                   LIBS opennova_editor opennova_imgui)
    set(OPENNOVA_EDITOR_UI_GROUPS workspace markers bounds field_widgets reference_picker find rename
                                  menu inspector preview styles problems gate logic animation model
                                  project navigation shortcuts board space live terrain)
    foreach(group IN LISTS OPENNOVA_EDITOR_UI_GROUPS)
        add_test(NAME editor_ui_${group} COMMAND editor_ui_test ${group}
                 WORKING_DIRECTORY ${OPENNOVA_TESTS_BINARY_DIR})
    endforeach()
    add_test(NAME editor_ui_groups COMMAND editor_ui_test --groups ${OPENNOVA_EDITOR_UI_GROUPS}
             WORKING_DIRECTORY ${OPENNOVA_TESTS_BINARY_DIR})

    # The document views' contract (S13 V3, ui/document_views): every DocumentTypeId's view over a
    # file of its type on the null backend, its row's role, no request raised by drawing, nothing
    # wider than its tab, a RevealRecord held until it draws; two open documents a view each.
    opennova_add_editor_test(view_contract SOURCES ${OPENNOVA_EDITOR_UI_DIR}/view_contract_test.cpp
                             LIBS opennova_editor opennova_imgui)
endif()

opennova_add_editor_test(project_cli NAME project_cli LIBS opennova_project_cli)

# Where a file lands and how it moves (DI-03): one placement rule for Create missing, New file and an
# import (a flat project stays flat), and a move between folders that rewrites no reference, in the
# session, over the wire's request and the command line's mv.
opennova_add_editor_test(file_placement LIBS opennova_project_cli)

# Files' chores (DI-25): a delete to the project's trash (who names the file asked first), a duplicate under the
# name the project's rules give, a new file in a folder, folders made, renamed and deleted (with what they hold),
# several files at once, the trash emptied, the file history's undo and redo of each, and the command line's
# verbs for them.
opennova_add_editor_test(file_chores LIBS opennova_project_cli)

# opennova-project as the editor's session, headless (S13 A7): the verb table against the request
# and query tables, each verb's --json the session's answer to the same requests, the request and
# query verbs, --install; its retail leg a project imported from the game install whose Problems
# the query verb reads as the session does.
opennova_add_editor_test(project_cli_session MIXED RETAIL_PARTS main install_import NAME project_cli_session
                         LIBS opennova_project_cli)

# opennova-project validate over the deepest menus the game reads (formats/mnu/mnu.cpp's bounds: 2017
# nested windows, the 4031-deep LAN_LIST chain): it completes on the test's own stack, in time linear in
# the windows, with the findings it always gave.
opennova_add_editor_test(menu_depth LIBS opennova_project_cli)

opennova_add_editor_test(catalog SOURCES catalog_document_test.cpp)

# The item ids the engine fixes, as the catalog keeps them (ADR 0046 S19, "Reserved item ids"): the
# ids a new item never takes, the edits refused, the findings and their fixes, the engine's rows added.
opennova_add_editor_test(reserved_items)
# The sound lane (ADR 0046; DI-02, DI-16): the bank and SndProf.def documents, the sound names in the
# graph by the game's bank order, the preview's picks, the session's play_sound; a SKIP-LEG without
# OPENNOVA_JO_ASSETS (every shipped bank and SndProf.def through the documents).
opennova_add_editor_test(sound_documents MIXED)
# Dialog banks followed (ADR 0046 DI-32): a .dbf as a document over the engine's reader and writer, its dialogs the
# Dialog symbols a mission's Play dialog names by number (Go to, used by, a rename writing the number, Add it there),
# a line's wave one of the bank's sounds, the session's play of a dialog line after line; a SKIP-LEG without
# OPENNOVA_JO_ASSETS (every shipped dialog bank through the document byte for byte, each line's wave in its sounds).
opennova_add_editor_test(dialog_bank MIXED SOURCES dialog_bank_document_test.cpp)
# The AI profile document (ADR 0046 S23 B): an .aip's one profile over the game's reader with the file's layout, its
# keys in the file's units by its type's set, a value read back as the game reads it, the save the file but for its
# lines; --retail: every shipped profile through the document byte for byte.
opennova_add_editor_test(ai_profile MIXED SOURCES ai_profile_document_test.cpp)
# The particle key panel (ADR 0046 S23 B): a particle file's blocks and keys as the game's reader reads them, a set
# rewriting one value's span, a key added where the line-ordered reader reads it, a value the reader takes otherwise
# refused, the particle_keys query; --retail: the install's particle files, each key's text at its span.
opennova_add_editor_test(particle_keys MIXED)

# The particle type's project check: an effect's particle and a particle's curve table that no particle file the game
# loads defines (every file's names, without case; the gore set the project picks), each a warning at the name.
opennova_add_editor_test(particle_project_check)

# The game's particle parse hazard: the line tokenizer's token slots a list reads past its line into, a fault where no
# line parsed before it filled the slot (the files in the order the game parses them), an error that gates the build.
opennova_add_editor_test(particle_token_slots)
# The HUD effects document (ADR 0046 S23 B): hudfx.def's model lines, the one the game reads, the game's failure
# where the HUD model is that line, a long name's run, an edit's one line, the blank. JO ships none.
opennova_add_editor_test(hudfx SOURCES hudfx_document_test.cpp)
# The score table document (ADR 0046 S23 B): score.ini's blocks, columns and points over the game's reader with the
# file's layout, a value's own line, a column and a block added, the game's reading as findings; --retail: the
# install's loose score.ini through the document byte for byte.
opennova_add_editor_test(score MIXED SOURCES score_document_test.cpp)
# The avatars table document (ADR 0046 S23 B): Avatars.def's parts and nationalities over the game's reader with the
# file's layout, the graph over its records, the reader's notes, a session's edit and undo; --retail: the install's
# Avatars.def (base and each expansion) through the document byte for byte.
opennova_add_editor_test(avatars MIXED SOURCES avatars_document_test.cpp)

# The string-table document (ADR 0046 S6b); --retail: every RTXT table of the install through the document, an unedited
# save the stored bytes or, for a table the game reads regrouped, the format's regrouped table.
opennova_add_editor_test(strings MIXED SOURCES strings_document_test.cpp)

# The menu document (S6c), the edits that must not lose authored data (S9d) and the
# retail sweep through it: a SKIP-LEG per root (OPENNOVA_JO_DIR's installed menus,
# OPENNOVA_JO_ASSETS' fifteen revx02 fixtures).
# The menu's table (editor/documents/mnu_table, ADR 0046 S13 D10): every member reached by
# exactly one field path or list, over a synthetic every-element menu and (a SKIP-LEG without
# OPENNOVA_JO_ASSETS) the fifteen reference menus rebuilt through the table.
opennova_add_editor_test(menu_table MIXED)

opennova_add_editor_test(menus MIXED SOURCES mnu_document_test.cpp)

# The model document (S10d): untouched saves, edits, structure and refusals, undo,
# save and reload, the validator (S11h: frames and registers as the runtime reads them);
# a SKIP-LEG per root: every retail model saved untouched (OPENNOVA_JO_ASSETS), every
# model of the game install validated with no error (OPENNOVA_JO_DIR).
# The model's table (editor/documents/model_table.cpp, ADR 0046 S13 D10; the format's
# threedi_schema test it was): a Set of every field of every record to itself changes no
# byte, over the synthetic models and (a SKIP-LEG without OPENNOVA_JO_DIR) every model the
# game install ships; the derived words, the refusals, the lists, the rows.
opennova_add_editor_test(model_table MIXED)

# The mission's table (editor/documents/mission_table.h, ADR 0046 S13 D10, S14): a mission's records
# as rows of the one table shape over the format's field rows, read through the mission document;
# the rules, the editor's ranges, the lists and the chains over the synthetic dense mission, and (a
# SKIP-LEG without OPENNOVA_JO_DIR) every mission the game install ships read and set to itself.
opennova_add_editor_test(mission_table MIXED)

# The mission document (editor/documents/mission_document.h, ADR 0046 S14) over the minted
# synth_logic mission: the rows in bands, every reference as a graph edge of its kind, the
# edits and their cascades, the parse's findings, the validator's findings and the mission's
# use check, the picture's reads, an item's TYPE on its symbol; and (a SKIP-LEG without
# OPENNOVA_JO_DIR) every shipped mission opened, its references counted and its findings by
# code.
opennova_add_editor_test(mission_document MIXED)

# A mission's logic in words (editor/documents/mission_sentence.h, ADR 0046 S15): each trigger and
# action worded from its type and its typed parameters, the chain's fold, an event's flags, delay and
# repeat, a type or a value naming nothing said so; the minted mission's titles; the types "Add
# trigger" and "Add action" offer; and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped event worded.
opennova_add_editor_test(mission_sentence MIXED)

# Adding and editing a mission's logic without its format (editor/documents/mission_logic.h,
# mission_uses.h, ADR 0046 S15): the forms, an add by type, the 20 an event holds refused before the
# edit, a retype keeping what applies, a move, a negation and a join, what names a record, the
# mission_logic and mission_uses queries; and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped record
# given a form and the defaults' measured values.
opennova_add_editor_test(mission_logic MIXED)

# The mission logic, deeply integrated (ADR 0046 S24): the Event board's portable half (the mission_logic query's
# op table; editor/documents/mission_board.h), and (a SKIP-LEG without OPENNOVA_JO_DIR) every shipped event's form;
# the logic in the views (the pick a chip asks of them; editor/documents/mission_links.h); the names and the live
# logic (a variable's words, editor/documents/mission_live.h).
opennova_add_editor_test(mission_board MIXED)

opennova_add_editor_test(mission_links)

opennova_add_editor_test(mission_names)

# The display names (ADR 0046 S15, Names: editor/documents/mission_labels.h and
# editor/graph/display_names.h) over the minted synth_logic mission: every record and every value
# that names something in a modder's words, with and without the project's names (an item by its
# catalog's name, a text key by its string), what names nothing said in words, the picker's
# choices worded, the cache, the wire's words through a session; and (a SKIP-LEG without
# OPENNOVA_JO_DIR) every record and reference of the shipped missions worded with the install's
# catalogs and tables, timed.
opennova_add_editor_test(mission_labels MIXED)

# Help in a script's text (editor/session/script_assist.h, ADR 0046 S15): the completions from the WAC
# compiler's tables and the mission of the script's name, what a word is, where it is defined, the
# mission's script, a compiler report in plain words, and the script_assist query.
opennova_add_editor_test(script_assist)

# The one table shape (editor/model/table_shape.h, ADR 0046 S13 D10) as the menu's, the def
# catalog's (items, weapons, powerups), the model's and the mission's tables fill it: the value, the
# choice, the labelled field, the record handle and the list ops over a file of each; the cost of a
# keystroke in a def catalog (one field's row resolved); and (a SKIP-LEG without OPENNOVA_JO_DIR)
# the game's powerup.def opened as the catalog's rows, generated back as the file was (its modeled layout).
opennova_add_editor_test(table_shape MIXED)

opennova_add_editor_test(model_document MIXED)

# A model's bullet-face surfaces set on its materials and its records in words (S17): each face's
# material found, a material's surface set on every face made from it in one step, mixed and unified,
# the refusals, the game's words; a SKIP-LEG counting the install's faces and mixed materials.
opennova_add_editor_test(model_surfaces MIXED)

# A model's collision shown in its viewport (S17): a box's planes make the box, every synthetic
# model's collision as shapes on the collision LOD's triangles, each record's shape and back, a
# person's hit spheres at the game's radius, a live part carrying its section, picking; a SKIP-LEG
# making every installed model's volumes into solids inside their boxes.
opennova_add_editor_test(model_collision MIXED)

# The clip and animation map documents (S10e): untouched saves, engine data edits,
# the motion refused, rows and clips, the reset row, the validator and the clip edges;
# a SKIP-LEG saving every retail clip and table untouched.
opennova_add_editor_test(animation_documents MIXED)

# The stylesheet document (S9i): rows per line, edits the game reads as shown, the
# refusals, the validator; a SKIP-LEG for OPENNOVA_JO_ASSETS' shipped menu_style.mns.
opennova_add_editor_test(styles MIXED SOURCES mns_document_test.cpp)

# The terrain viewport (the deep-integration plan's DI-30b): a terrain drawn on its own under a mission's
# environment or the engine's own, its tiles the mission's or the terrain's own, framed within the fog, DI-29's
# overlays, the ground under a point, an edit rebuilding it, a refused terrain, the wire.
opennova_add_editor_test(terrain_viewport)

opennova_add_editor_test(environment_viewport)

# Face animations (ADR 0046, round S23 lane A): a .grm as a document over the engine's reader and writer, its
# triangles' corners following their vertices, its findings, its references and its blank, over the authored
# fixture; the retail twin opens and saves the 13 faces Black Hawk Down ships (JO ships none).
opennova_add_editor_test(face_animation MIXED SOURCES face_animation_document_test.cpp)

# Fonts (ADR 0046, round S23 lane A): a .fnt as a document over the engine's reader and writer, its header and
# glyphs edited, its findings, its blank; a SKIP-LEG without OPENNOVA_JO_ASSETS (every shipped .fnt byte for byte).
opennova_add_editor_test(font_document MIXED)

# Music banks (ADR 0046, round S23 lane A): a .sbf as a document over the engine's reader and writer, its streams
# edited, its findings, a stream played through a session; a SKIP-LEG without OPENNOVA_JO_DIR (every bank the
# install streams, byte for byte).
opennova_add_editor_test(music_bank MIXED SOURCES music_bank_document_test.cpp)

# A music bank stream's audio from a wave: a stream's audio replaced and a stream added over a session (a mono 44100 Hz
# wave of the project, a stereo 22050 Hz one on disk), saved and decoded back within the codec's precision, undone.
opennova_add_editor_test(music_stream_audio)

# Waves (ADR 0046, round S23 lane A): a .wav as a document read as the game's loader reads it, its one finding, a
# trim and a normalise through a session; a SKIP-LEG without OPENNOVA_JO_ASSETS (every shipped wave byte for byte,
# DSkid.wav alone refused).
opennova_add_editor_test(wave_document MIXED)

# --- ADR 0046 S25, terrain editing: the round's rows (added with its shared seams; each lane fills its own). ---

# The terrain brush (ADR 0046 S25): its tools, a dab's weight, the dabs along a path, the brush on the wire.
opennova_add_editor_test(terrain_brush)

# The terrain tools' shared seams (S25): the step marks, the undo across documents by link, EndEdit by a gesture's
# token and its cancel, the terrain_stroke request on the wire and its refusals, the brush and the Terrain pane.
opennova_add_editor_test(terrain_stroke)

# The terrain family (S25): each plane's file as the game resolves it for a mission or a terrain.
opennova_add_editor_test(terrain_family)

# The raster core's history (S25).
opennova_add_editor_test(raster_history)

# The raster document (S25).
opennova_add_editor_test(raster_document)

# The heights document (S25); the retail twin opens and saves every JO:CA .cpt byte for byte.
opennova_add_editor_test(terrain_heights_document MIXED)

# The heights brushes (S25).
opennova_add_editor_test(heights_brushes TIMEOUT 600)

# The heights' bake rules (S25): no loop over a redo or a refused bake, Save settling first, the follow's edits.
opennova_add_editor_test(heights_bake_rules TIMEOUT 600)

# Unlocked entities following the ground at a bake's landing, locked ones staying (S25).
opennova_add_editor_test(height_lock_follow)

# The .til document (S25): a .til's tiles over formats/til, their layers kept ascending, the source notes and
# the files read only; the retail twin opens and saves all 116 shipped .til byte for byte.
opennova_add_editor_test(til_document MIXED)

# The tile tools (S25).
opennova_add_editor_test(tile_tools)

# The tile tools through the session (S25 B2): edit_record's terrain_view, the mission's .til made seeded with the
# terrain's own tiles, the view's Undo and Save, the view's tile planners, the terrain query's tiles ops.
opennova_add_editor_test(tile_edit)

# The maps and the tiles in the mission's 3D view as they are made: each paint and tile edit an Update with its device
# patch (S25).
opennova_add_editor_test(terrain_maps_view)

# Painting a texture used as a terrain map (S25); the retail twin paints every shipped map, undoes it and saves it
# identical.
opennova_add_editor_test(texture_paint MIXED)

# The paint planner (S25).
opennova_add_editor_test(terrain_map_paint)

# The 2D map's backgrounds (S25): Nile's Height and Delta maps, MED's flatness and water.
opennova_add_editor_test(mission_map_terrain_view)

# A mission entity's Height lock (S25): the field, the .mis companion's height_lock, the DI-28 words.
opennova_add_editor_test(mission_height_lock)

# Every row above, retail twins included, carries the `editor` label: CI's core leg runs as two
# shards, `editor` (these rows) and `rest` (the core rows without it), each on its own runner
# (scripts/ci/test_suites.py SHARDS). APPEND keeps a twin's `retail` label, so the suites are as
# before. Keep this at the end of the file so it sees every row.
get_property(opennova_editor_rows DIRECTORY PROPERTY TESTS)
set_property(TEST ${opennova_editor_rows} APPEND PROPERTY LABELS editor)
