cmake_minimum_required(VERSION 3.20)

# The web build (emcmake): Emscripten refuses SHARED libraries, and godot-cpp's
# hack that re-enables them for the side module must run right after the
# toolchain, i.e. before project(). It does nothing on any other platform.
set(CMAKE_PROJECT_INCLUDE "${CMAKE_CURRENT_LIST_DIR}/../../third_party/godot-cpp/cmake/emsdkHack.cmake")

project(opennova_godot LANGUAGES C CXX)

# FetchContent populates in the configure step itself (CMake 3.30+) instead of
# configuring and building a sub-project per dependency on every configure.
if(POLICY CMP0168)
    cmake_policy(SET CMP0168 NEW)
endif()

if(EMSCRIPTEN)
    # Every object in the side module shares the main module's threaded
    # memory, so the engine and vendored libraries compile with -pthread too
    # (godot-cpp applies -sUSE_PTHREADS only to itself and its consumers).
    add_compile_options(-pthread)
endif()

# Generate and compile only the engine classes this extension reaches (~190 of
# ~1020): godot_cpp_profile.json is derived from godot/src by
# scripts/lint/godot_cpp_profile.py (regenerate with --write when a binding
# starts using a class the list lacks; CI's lint step fails on a stale list).
# godot-cpp trims its API at configure time, so the profile is a configure
# dependency.
set(GODOTCPP_BUILD_PROFILE "${CMAKE_CURRENT_SOURCE_DIR}/godot_cpp_profile.json"
    CACHE PATH "godot-cpp build profile (generated)" FORCE)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GODOTCPP_BUILD_PROFILE}")
add_subdirectory(../../third_party/godot-cpp godot-cpp)
# sqlite is only needed to DEFINE the novaworld service target inside engine/;
# the extension links opennova_runtime (which PUBLIC-links the wire-only
# opennova_net: codec + session/gate legs; the in-match session and
# replication live in runtime/, ADR 0043 d4), NEVER opennova_novaworld_service,
# so no sqlite or service code ends up in the binary.
add_subdirectory(../../third_party/sqlite opennova_sqlite)
# Vendored miniz: the BFC1 container decoder's inflate (engine/formats/bfc1).
add_subdirectory(../../third_party/miniz opennova_miniz)
option(OPENNOVA_DEVTOOLS "Build the game's in-engine dev-tool windows (Dear ImGui)" ON)
# The imgui-godot addon ships no web binaries, so the web build has no host
# for the dev-tool windows either.
if(GODOTCPP_TARGET STREQUAL "template_release" OR EMSCRIPTEN)
    set(OPENNOVA_DEVTOOLS OFF)
endif()
# Two GDExtension variants from one source tree (ADR 0046 d4): the runtime-only
# `libopennova.*` the game and the Play child ship, and the editor-enabled
# `libopennova_editor.*` the OpenNova Editor ships (and every source run loads, as
# the superset). OFF builds the runtime variant alone.
option(OPENNOVA_EDITOR "Build the editor-enabled GDExtension variant beside the runtime one" ON)
# The web build (ADR 0049) is the game's runtime variant alone: the editor's windows
# ride the same imgui-godot addon (no web binaries), its Play child spawns a desktop
# process, and leaving the option on would turn OPENNOVA_IMGUI on below, compiling
# Dear ImGui and the pass into the web side module.
if(EMSCRIPTEN)
    set(OPENNOVA_EDITOR OFF)
endif()
# Dear ImGui builds when either product wants its pass: the game's dev tools or
# the editor's windows (OPENNOVA_IMGUI is read by engine/).
if(OPENNOVA_DEVTOOLS OR OPENNOVA_EDITOR)
    set(OPENNOVA_IMGUI ON)
    add_subdirectory(../../third_party/imgui opennova_imgui)
else()
    set(OPENNOVA_IMGUI OFF)
endif()
# The engine (Shape A group targets — see engine/CMakeLists.txt).
add_subdirectory(../../engine opennova_engine)

# The bindings, compiled once and shared by both variants: everything but the
# entry point (register_types.cpp, which differs by what it registers), the
# ImGui pass node (whose attach leg is per variant) and the editor's authoring
# layer (editor variant only).
add_library(opennova_bindings OBJECT
    network/novaworld_client.cpp
    network/host_session_options.cpp
    network/novaworld_server_row.cpp
    network/novaworld_gate_info.cpp
    network/novaworld_server_browser.cpp
    network/ping_sweep_worker.cpp
    network/novaworld_identity.cpp
    network/udp_datagram.cpp
    network/udp_pump.cpp
    network/lan_session.cpp
    network/lan_server_row.cpp
    network/net_protocol.cpp
    network/net_session_policy.cpp
    network/connection_error.cpp
    network/join_screen_status.cpp
    network/join_target.cpp
    terrain/terrain.cpp
    terrain/terrain_surface_inputs.cpp
    terrain/terrain_static_shadow_rasterizer.cpp
    terrain/terrain_tile_cache_device.cpp
    terrain/terrain_data.cpp
    terrain/terrain_foliage_def.cpp
    terrain/terrain_foliage_map.cpp
    terrain/foliage_dispatcher.cpp
    terrain/foliage_frame_stats.cpp
    terrain/foliage_mask_pass.cpp
    terrain/terrain_tile_entry.cpp
    terrain/terrain_tile_info.cpp
    env/env_keyframe.cpp
    env/env_file.cpp
    env/env_records.cpp
    env/mission_environment_overrides.cpp
    env/mission_environment.cpp
    mission/mission_catalog.cpp
    mission/mission_object_placer.cpp
    mission/mission_placement_stats.cpp
    mission/mission_placement_run.cpp
    mission/static_population_instance.cpp
    mission/mission_object_placer_static_sources.cpp
    env/celestial.cpp
    env/environment_cube_capture.cpp
    env/sky_dome.cpp
    env/slot_shadow.cpp
    env/sun_shadow.cpp
    env/water.cpp
    env/weather.cpp
    env/precipitation.cpp
    env/water_core.cpp
    env/glare_occlusion.cpp
    particle/particle_effect.cpp
    particle/particle_file.cpp
    lights/effect_light_director.cpp
    lights/effect_light_report.cpp
    lights/light_scene.cpp
    lights/light_spawn.cpp
    particle/effect_distortion_drawer.cpp
    particle/effect_group_report.cpp
    particle/effect_load_report.cpp
    particle/effect_scene.cpp
    particle/effect_spawn_records.cpp
    particle/effect_world.cpp
    particle/particle_compositor.cpp
    particle/particle_far_pass.cpp
    particle/particle_renderer.cpp
    render/environment_cube_blit.cpp
    render/frame_fx.cpp
    render/nvg_view_device.cpp
    render/object_lod_frame.cpp
    render/q3_frame_adapter.cpp
    render/q3_geometry_cache.cpp
    render/q3_source_registry.cpp
    render/retained_array_mesh.cpp
    render/scene_overlay_compositor.cpp
    render/slot_capture_adapter.cpp
    render/target_projection_xr_interface.cpp
    object/entity_index.cpp
    object/entity_ref.cpp
    object/object_model.cpp
    object/object_model_anim.cpp
    object/object_model_materials.cpp
    object/object_model_scene.cpp
    object/object_model_skin_palette.cpp
    object/object_model_views.cpp
    object/object_data.cpp
    object/object_data_bind.cpp
    object/object_data_state.cpp
    object/object_data_materials.cpp
    object/object_data_geometry.cpp
    object/object_data_runtime_eval.cpp
    object/object_shader_cache.cpp
    object/item_database.cpp
    object/model_light.cpp
    object/model_user_point.cpp
    object/weapon_database.cpp
    object/ammo_database.cpp
    object/weapon_def.cpp
    object/avatar_database.cpp
    object/avatar_records.cpp
    object/character_join_profile.cpp
    mission/player_visual_spec.cpp
    simulation/fp_viewmodel_spec.cpp
    simulation/player_aim_overlay.cpp
    simulation/player_local_view.cpp
    simulation/player_weapon_event.cpp
    simulation/destruction_events.cpp
    simulation/debug_pick_card.cpp
    simulation/hitbox_debug_report.cpp
    simulation/hud_view_records.cpp
    simulation/epilog_cine_record.cpp
    simulation/deploy_rows.cpp
    simulation/player_weapon_view.cpp
    simulation/player_inventory.cpp
    simulation/weapon_profile_summary.cpp
    simulation/present_event_records.cpp
    simulation/environment_snapshot.cpp
    simulation/weapon_kit_entry.cpp
    object/skeletal_anim.cpp
    hud/hud_draw_list_stats.cpp
    hud/hud_overlay.cpp
    hud/hud_overlay_combat.cpp
    hud/hud_overlay_flat.cpp
    hud/hud_overlay_windows.cpp
    hud/hud_map_pass_renderer.cpp
    hud/map_view_window.cpp
    hud/command_map_screen.cpp
    hud/hud_toggles.cpp
    hud/hud_chat_entry.cpp
    hud/end_round_transition.cpp
    hud/hud_inset_scope.cpp
    hud/player_hud_weapon_def.cpp
    devtools/frame_stats.cpp
    devtools/dev_tools.cpp
    devtools/dev_tools_overlay.cpp
    devtools/dev_tools_windows.cpp
    devtools/debug_arg_spec.cpp
    devtools/debug_control_records.cpp
    devtools/debug_shell_host.cpp
    devtools/debug_control_table.cpp
    devtools/scripted_input.cpp
    hud/hud_pos.cpp
    hud/vehicle_hud_block.cpp
    mission/mission_data.cpp
    mission/mission_info.cpp
    mission/mission_perf_counters.cpp
    mission/mission_root.cpp
    mission/mission_setup_options.cpp
    simulation/entity_presenter.cpp
    simulation/entity_presenter_wire.cpp
    simulation/person_overlay_models.cpp
    simulation/fire_presenter.cpp
    simulation/destruction_presenter.cpp
    simulation/throwable_presenter.cpp
    simulation/vehicle_trail_presenter.cpp
    simulation/vehicle_water_wake_presenter.cpp
    simulation/present_stats.cpp
    simulation/entity_card.cpp
    simulation/entity_row.cpp
    simulation/end_round_state.cpp
    simulation/inmatch_session_values.cpp
    simulation/simulation.cpp
    simulation/simulation_assets.cpp
    simulation/simulation_bind.cpp
    simulation/simulation_frame.cpp
    simulation/simulation_frame_stats.cpp
    simulation/simulation_devtools.cpp
    simulation/simulation_net.cpp
    simulation/simulation_occlusion.cpp
    simulation/simulation_player.cpp
    simulation/simulation_player_loadout.cpp
    simulation/simulation_deploy_screen.cpp
    simulation/simulation_end_round.cpp
    simulation/simulation_player_view.cpp
    simulation/simulation_player_weapon.cpp
    simulation/effect_section_source.cpp
    simulation/simulation_present.cpp
    simulation/simulation_script_voice.cpp
    simulation/simulation_scars.cpp
    simulation/simulation_hud_feeds.cpp
    world/item_effect_director.cpp
    world/occlusion_frame.cpp
    world/game_world.cpp
    world/game_world_frame.cpp
    world/game_world_load.cpp
    world/game_world_effects.cpp
    world/session_drive.cpp
    world/world_view.cpp
    world/load_timeline.cpp
    world/loading_screen_info.cpp
    world/post_mission_route.cpp
    world/runtime_perf_counters.cpp
    world/resource_root_resolver.cpp
    world/scar_draw_list.cpp
    world/scar_presenter.cpp
    wac/wac_program.cpp
    lwf/lwf_data.cpp
    lwf/wav_loader.cpp
    audio/ambient_mixer.cpp
    audio/sound_selector.cpp
    audio/ambient_layer.cpp
    audio/sound_bank.cpp
    audio/mission_audio_records.cpp
    audio/mission_audio.cpp
    audio/mission_audio_voice.cpp
    dbf/dbf_data.cpp
    rtxt/rtxt_string_file.cpp
    util/data_format.cpp
    util/paths.cpp
    util/pcx_texture_bridge.cpp
    util/texture_files.cpp
    util/texture_path_resolver.cpp
    resource_index/resource_root.cpp
    resource_index/resource_root_file_source.cpp
    resource_index/launch_flags.cpp
    fnt/fnt_resource.cpp
    audio/sbf_bank.cpp
    audio/sbf_audio_stream.cpp
    audio/sbf_audio_stream_playback.cpp
    audio/music_script.cpp
    audio/music_director.cpp
    audio/music_pair_names.cpp
    mnu/mnu_document.cpp
    mnu/mnu_rows.cpp
    mnu/mns_stylesheet.cpp
    mnu/menu_draw_list_stats.cpp
    mnu/menu_frame.cpp
    mnu/menu_audio.cpp
    mnu/menu_driver.cpp
    mnu/menu_video_underlay.cpp
    mnu/controls_model.cpp
    player/first_person_arms_witness.cpp
    player/gameplay_camera.cpp
    player/local_player_presenter.cpp
    player/local_player_visuals.cpp
    player/player_input_router.cpp
    player/player_move_intent.cpp
    player/player_spawn_loadout.cpp
    player/player_viewmodel_def.cpp
    player/player_viewmodel_rig.cpp
    player/player_weapon_effects.cpp
)

target_include_directories(opennova_bindings PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# opennova_runtime carries the sim chain (runtime -> formats -> base -> io) the
# bindings consume directly; opennova_net is the wire only and never links
# runtime (ADR 0043 d4). The service target is deliberately absent (see the sqlite note
# above); scripts/lint/link_graph_check.py enforces it.
target_link_libraries(opennova_bindings PUBLIC
    godot-cpp
    opennova_runtime
    opennova_net
)

get_target_property(GODOTCPP_SUFFIX godot-cpp GODOTCPP_SUFFIX)

set(OUTPUT_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../bin/")

# gersemi: off
set_target_properties(
    opennova_bindings
    PROPERTIES
        CXX_STANDARD 17
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}
        POSITION_INDEPENDENT_CODE ON
        FOLDER "opennova"
)
# gersemi: on

# The runtime-only variant: the game and the Play child. It never links
# opennova_editor (link_graph_check.py), and its ImGui pass node attaches only when
# the game's dev tools are compiled in.
add_library(opennova SHARED
    register_types.cpp
    devtools/imgui_pass_node.cpp
)
target_include_directories(opennova PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(opennova PRIVATE opennova_bindings)
if(OPENNOVA_DEVTOOLS)
    target_compile_definitions(opennova PRIVATE OPENNOVA_EDITOR=0 OPENNOVA_IMGUI_NODE=1)
else()
    target_compile_definitions(opennova PRIVATE OPENNOVA_EDITOR=0 OPENNOVA_IMGUI_NODE=0)
endif()

# gersemi: off
set_target_properties(
    opennova
    PROPERTIES
        CXX_STANDARD 17
        CXX_EXTENSIONS OFF
        CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}

        POSITION_INDEPENDENT_CODE ON

        LIBRARY_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
        RUNTIME_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
        PDB_OUTPUT_DIRECTORY     "$<1:${OUTPUT_DIR}>"

        PREFIX "lib"
        OUTPUT_NAME "opennova${GODOTCPP_SUFFIX}"

        FOLDER "opennova"
)
# gersemi: on

# The editor-enabled variant (ADR 0046 d4): the same bindings plus the authoring
# layer (godot/src/authoring) and the editor core, with the ImGui pass always
# attachable. `libopennova_editor.*`; selected by the `opennova_editor` feature tag
# and by the plain rows of opennova.gdextension (source runs load the superset).
if(OPENNOVA_EDITOR)
    add_library(opennova_editor_gdext SHARED
        register_types.cpp
        devtools/imgui_pass_node.cpp
        authoring/child_process.cpp
        authoring/editor_app.cpp
        authoring/menu_viewport_applier.cpp
        authoring/mission_viewport_applier.cpp
        authoring/model_viewport_applier.cpp
        authoring/preview_sound_player.cpp
        authoring/script_device.cpp
        authoring/script_edit.cpp
        authoring/script_highlighter.cpp
        authoring/texture_viewport_applier.cpp
        authoring/effect_viewport_applier.cpp
        authoring/preview_effects.cpp
        authoring/thumbnail_images.cpp
        authoring/viewport_device.cpp
        authoring/viewport_devices.cpp
    )
    target_include_directories(opennova_editor_gdext PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    target_link_libraries(opennova_editor_gdext PRIVATE opennova_bindings opennova_editor)
    target_compile_definitions(opennova_editor_gdext PRIVATE OPENNOVA_EDITOR=1 OPENNOVA_IMGUI_NODE=1)

    # gersemi: off
    set_target_properties(
        opennova_editor_gdext
        PROPERTIES
            CXX_STANDARD 17
            CXX_EXTENSIONS OFF
            CXX_VISIBILITY_PRESET ${GODOTCPP_SYMBOL_VISIBILITY}

            POSITION_INDEPENDENT_CODE ON

            LIBRARY_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
            RUNTIME_OUTPUT_DIRECTORY "$<1:${OUTPUT_DIR}>"
            PDB_OUTPUT_DIRECTORY     "$<1:${OUTPUT_DIR}>"

            PREFIX "lib"
            OUTPUT_NAME "opennova_editor${GODOTCPP_SUFFIX}"

            FOLDER "opennova"
    )
    # gersemi: on
endif()

# emsdkHack clears the shared-library suffix; Godot looks the web side module
# up by the .wasm name opennova.gdextension lists. Every object is already
# LLVM-optimized; binaryen's -O3 over the whole ~16 MB module ran past an hour,
# so its link-time pass runs at -O1 (the last -O on the link line wins). The web
# build is the runtime variant alone (OPENNOVA_EDITOR is off there, above).
if(EMSCRIPTEN)
    set_target_properties(opennova PROPERTIES SUFFIX ".wasm")
    target_link_options(opennova PRIVATE -O1)
endif()
