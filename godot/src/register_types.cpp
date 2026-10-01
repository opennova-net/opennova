#include "./register_types.h"
#include "util/texture_path_resolver.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/io/log.h>
#include <base/io/log_ring.h>

#include "terrain/terrain_data.h"
#include "terrain/terrain.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_foliage_def.h"
#include "terrain/terrain_foliage_map.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/foliage_frame_stats.h"
#include "terrain/foliage_mask_pass.h"
#include "terrain/terrain_tile_entry.h"
#include "terrain/terrain_tile_info.h"
#include "env/env_keyframe.h"
#include "env/env_file.h"
#include "env/env_records.h"
#include "env/mission_environment.h"
#include "env/celestial.h"
#include "env/environment_cube_capture.h"
#include "mission/mission_object_placer.h"
#include "mission/mission_placement_stats.h"
#include "mission/static_population_instance.h"
#include "env/sky_dome.h"
#include "env/slot_shadow.h"
#include "env/sun_shadow.h"
#include "env/water.h"
#include "env/weather.h"
#include "env/precipitation.h"
#include "particle/particle_effect.h"
#include "particle/particle_file.h"
#include "lights/effect_light_director.h"
#include "lights/effect_light_report.h"
#include "lights/light_scene.h"
#include "lights/light_spawn.h"
#include "particle/effect_group_report.h"
#include "particle/effect_load_report.h"
#include "particle/effect_scene.h"
#include "particle/effect_spawn_records.h"
#include "particle/effect_world.h"
#include "particle/particle_compositor.h"
#include "particle/particle_renderer.h"
#include "render/frame_fx.h"
#include "render/q3_source_registry.h"
#include "render/retained_array_mesh.h"
#include "render/scene_overlay_compositor.h"
#include "render/target_projection_xr_interface.h"
#include "world/item_effect_director.h"
#include "world/occlusion_frame.h"
#include "world/game_world.h"
#include "world/load_timeline.h"
#include "world/loading_screen_info.h"
#include "world/post_mission_route.h"
#include "world/resource_root_resolver.h"
#include "world/runtime_perf_counters.h"
#include "world/world_view.h"
#include "world/scar_draw_list.h"
#include "world/scar_presenter.h"
#include "object/entity_index.h"
#include "object/entity_ref.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/post_multiply_draw.h"
#include "object/object_shader_cache.h"
#include "object/item_database.h"
#include "object/weapon_database.h"
#include "object/ammo_database.h"
#include "object/weapon_def.h"
#include "object/avatar_database.h"
#include "object/model_light.h"
#include "object/model_user_point.h"
#include "object/avatar_records.h"
#include "object/character_join_profile.h"
#include "mission/player_visual_spec.h"
#include "mission/mission_info.h"
#include "env/mission_environment_overrides.h"
#include "simulation/fp_viewmodel_spec.h"
#include "simulation/person_overlay_models.h"
#include "simulation/player_aim_overlay.h"
#include "simulation/player_local_view.h"
#include "simulation/player_weapon_event.h"
#include "simulation/destruction_events.h"
#include "simulation/destruction_presenter.h"
#include "simulation/throwable_presenter.h"
#include "simulation/vehicle_trail_presenter.h"
#include "simulation/debug_pick_card.h"
#include "simulation/hitbox_debug_report.h"
#include "simulation/hud_view_records.h"
#include "simulation/deploy_rows.h"
#include "simulation/player_weapon_view.h"
#include "simulation/player_inventory.h"
#include "simulation/weapon_kit_entry.h"
#include "simulation/weapon_profile_summary.h"
#include "simulation/environment_snapshot.h"
#include "simulation/present_event_records.h"
#include "object/skeletal_anim.h"
#include "hud/player_hud_weapon_def.h"
#include "hud/hud_draw_list_stats.h"
#include "hud/hud_overlay.h"
#include "hud/map_view_state.h"
#include "hud/map_view_window.h"
#include "hud/command_map_screen.h"
#include "hud/hud_toggles.h"
#include "hud/hud_chat_entry.h"
#include "hud/end_round_transition.h"
#include "hud/hud_inset_scope.h"
#include "devtools/dev_tools.h"
#include "devtools/imgui_pass_node.h"
#if OPENNOVA_EDITOR
#include "authoring/editor_app.h"
#include "authoring/script_edit.h"
#include "authoring/script_highlighter.h"
#endif
#include "devtools/debug_arg_spec.h"
#include "devtools/debug_control_records.h"
#include "devtools/debug_control_table.h"
#include "devtools/debug_shell_host.h"
#include "devtools/frame_stats.h"
#include "hud/hud_pos.h"
#include "hud/vehicle_hud_block.h"
#include "mission/mission_catalog.h"
#include "mission/mission_data.h"
#include "mission/mission_perf_counters.h"
#include "mission/mission_root.h"
#include "mission/mission_setup_options.h"
#include "network/join_target.h"
#include "simulation/entity_card.h"
#include "simulation/end_round_state.h"
#include "simulation/entity_row.h"
#include "simulation/entity_presenter.h"
#include "simulation/present_stats.h"
#include "simulation/inmatch_session_values.h"
#include "simulation/simulation.h"
#include "lwf/lwf_data.h"
#include "lwf/wav_loader.h"
#include "audio/ambient_mixer.h"
#include "audio/sound_selector.h"
#include "audio/ambient_layer.h"
#include "audio/sound_bank.h"
#include "audio/mission_audio_records.h"
#include "audio/mission_audio.h"
#include "dbf/dbf_data.h"
#include "cbin/cbin_credits_resource.h"
#include "cbin/credits_player.h"
#include "fnt/fnt_resource.h"
#include "rtxt/rtxt_string_file.h"
#include "network/novaworld_client.h"
#include "network/novaworld_server_browser.h"
#include "network/host_session_options.h"
#include "network/novaworld_server_row.h"
#include "network/novaworld_gate_info.h"
#include "network/udp_datagram.h"
#include "network/udp_pump.h"
#include "network/lan_session.h"
#include "network/lan_server_row.h"
#include "network/net_protocol.h"
#include "network/net_session_policy.h"
#include "util/paths.h"
#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "audio/sbf_bank.h"
#include "audio/sbf_audio_stream.h"
#include "audio/sbf_audio_stream_playback.h"
#include "audio/music_script.h"
#include "audio/music_director.h"
#include "audio/music_pair_names.h"
#include "mnu/mnu_document.h"
#include "mnu/mnu_rows.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/menu_draw_list_stats.h"
#include "mnu/menu_frame.h"
#include "mnu/menu_audio.h"
#include "mnu/menu_driver.h"
#include "mnu/menu_video_underlay.h"
#include "mnu/controls_model.h"
#include "player/first_person_arms_witness.h"
#include "player/gameplay_camera.h"
#include "player/local_player_presenter.h"
#include "player/local_player_visuals.h"
#include "player/player_move_intent.h"
#include "player/player_spawn_loadout.h"
#include "player/player_viewmodel_def.h"
#include "player/player_viewmodel_rig.h"
#include "player/player_weapon_effects.h"

using namespace godot;

void initialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	// Engine diagnostics (io/log.h — the mission kernel's boot/tick warnings)
	// surface on Godot's warning channel; the sink stays unset elsewhere so
	// ctest binaries keep their own stdout sink.
	if (opennova::io::log_sink_slot() == nullptr) {
		opennova::io::set_log_sink([](opennova::io::LogLevel level, const char *message) {
			if (level >= opennova::io::LogLevel::kWarn)
				UtilityFunctions::push_warning(String::utf8(message));
		});
	}
	// The bounded ring MCP's game_logs drains (DevTools.engine_log_after;
	// ADR 0042 d5) records every level, then chains to the push_warning
	// forwarder above.
	opennova::io::LogRing::install();

	GDREGISTER_CLASS(TerrainData);
	GDREGISTER_CLASS(Terrain);
	GDREGISTER_CLASS(TerrainSurfaceInputs);
	GDREGISTER_CLASS(TerrainFoliageDef);
	GDREGISTER_CLASS(TerrainFoliageMap);
	GDREGISTER_CLASS(FoliageFrameStats);
	GDREGISTER_CLASS(VegGraphicRow);
	GDREGISTER_CLASS(FoliageDispatcher);
	GDREGISTER_CLASS(FoliageMaskCompositorEffect);
	GDREGISTER_CLASS(TerrainTileEntry);
	GDREGISTER_CLASS(TerrainTileInfo);
	GDREGISTER_CLASS(EnvKeyframe);
	GDREGISTER_CLASS(MissionEnvironmentOverrides);
	GDREGISTER_CLASS(EnvFile);
	GDREGISTER_CLASS(MissionEnvironment);
	GDREGISTER_CLASS(SkyDome);
	GDREGISTER_CLASS(Water);
	GDREGISTER_CLASS(Celestial);
	GDREGISTER_CLASS(EnvironmentCubeCapture);
	GDREGISTER_CLASS(MissionPlacementStats);
	GDREGISTER_CLASS(StaticPopulationInstance);
	GDREGISTER_CLASS(MissionObjectPlacer);
	GDREGISTER_CLASS(SunShadow);
	GDREGISTER_CLASS(SlotCaptureCompositorEffect);
	GDREGISTER_CLASS(SlotShadow);
	GDREGISTER_CLASS(Weather);
	GDREGISTER_CLASS(Precipitation);
	GDREGISTER_CLASS(ObjectData);
	GDREGISTER_CLASS(EnvLightValues);
	GDREGISTER_CLASS(EnvDayPhase);
	GDREGISTER_CLASS(EnvSunGlare);
	GDREGISTER_CLASS(EnvironmentSnapshot);
	GDREGISTER_CLASS(EnvLightState);
	GDREGISTER_CLASS(PanmClock);
	GDREGISTER_CLASS(EntityRef);
	GDREGISTER_CLASS(PostMultiplyDraw);
	GDREGISTER_CLASS(ObjectModel);
	GDREGISTER_CLASS(EntityIndex);
	GDREGISTER_CLASS(ObjectShaderCache);
	GDREGISTER_CLASS(ModelLight);
	GDREGISTER_CLASS(ModelUserPoint);
	GDREGISTER_CLASS(ItemDatabase);
	GDREGISTER_CLASS(WeaponSightRow);
	GDREGISTER_CLASS(WeaponActionRow);
	GDREGISTER_CLASS(WeaponDef);
	GDREGISTER_CLASS(ArmoryClassRow);
	GDREGISTER_CLASS(WeaponDatabase);
	GDREGISTER_CLASS(AmmoDatabase);
	GDREGISTER_CLASS(AvatarPartRow);
	GDREGISTER_CLASS(AvatarComboRow);
	GDREGISTER_CLASS(AvatarNationalityRow);
	GDREGISTER_CLASS(AvatarDivisionRow);
	GDREGISTER_CLASS(AvatarDiagnosticRow);
	GDREGISTER_CLASS(CharacterJoinProfile);
	GDREGISTER_CLASS(PlayerVisualSpec);
	GDREGISTER_CLASS(FpViewmodelSpec);
	GDREGISTER_CLASS(PlayerLocalView);
	GDREGISTER_CLASS(PlayerAimOverlay);
	GDREGISTER_CLASS(PlayerWeaponView);
	GDREGISTER_CLASS(WaypointHudView);
	GDREGISTER_CLASS(HudMapGridOrigin);
	GDREGISTER_CLASS(HudMapOverlays);
	GDREGISTER_CLASS(VehiclePanelView);
	GDREGISTER_CLASS(ScoreFeedback);
	GDREGISTER_CLASS(ScoreboardHeader);
	GDREGISTER_CLASS(EndRoundOverlay);
	GDREGISTER_CLASS(EndRoundStatistics);
	GDREGISTER_CLASS(EndRoundColumn);
	GDREGISTER_CLASS(EndRoundRow);
	GDREGISTER_CLASS(RoundOutcome);
	GDREGISTER_CLASS(DeployStatus);
	GDREGISTER_CLASS(DeployZoneRow);
	GDREGISTER_CLASS(DeployListRow);
	GDREGISTER_CLASS(DestructionEffectEvent);
	GDREGISTER_CLASS(HuskSwapEvent);
	GDREGISTER_CLASS(DestructionDrain);
	GDREGISTER_CLASS(HitboxDebugEntity);
	GDREGISTER_CLASS(HitboxDebugOrganic);
	GDREGISTER_CLASS(HitboxDebugReport);
	GDREGISTER_CLASS(DebugPickCard);
	GDREGISTER_CLASS(PlayerWeaponEvent);
	GDREGISTER_CLASS(WeaponKitEntry);
	GDREGISTER_CLASS(PlayerInventorySlot);
	GDREGISTER_CLASS(PlayerInventory);
	GDREGISTER_CLASS(WeaponProfileSide);
	GDREGISTER_CLASS(WeaponProfileSummary);
	GDREGISTER_CLASS(ThrowableVisualRow);
	GDREGISTER_CLASS(VehicleTrailVisualRow);
	GDREGISTER_CLASS(FirePresentationEvent);
	GDREGISTER_CLASS(FireSoundRow);
	GDREGISTER_CLASS(SlotSoundRow);
	GDREGISTER_CLASS(SoundEmitterRow);
	GDREGISTER_CLASS(RoundImpactRow);
	GDREGISTER_CLASS(MissionEffect);
	GDREGISTER_CLASS(DeathPieceRow);
	GDREGISTER_CLASS(DeathPieceDraw);
	GDREGISTER_CLASS(RoundGlowRow);
	GDREGISTER_CLASS(AvatarDatabase);
	GDREGISTER_CLASS(SkeletalAnim);
	GDREGISTER_CLASS(HudPos);
	GDREGISTER_CLASS(VehicleHudBlock);
	GDREGISTER_CLASS(HudDrawListStats);
	GDREGISTER_CLASS(HudOverlay);
	GDREGISTER_CLASS(MapViewState);
	GDREGISTER_CLASS(MapViewWindow);
	GDREGISTER_CLASS(CommandMapScreen);
	GDREGISTER_CLASS(HudToggles);
	GDREGISTER_CLASS(HudChatEntry);
	GDREGISTER_CLASS(EndRoundTransition);
	GDREGISTER_CLASS(HudInsetScope);
	GDREGISTER_CLASS(PlayerHudWeaponDef);
	GDREGISTER_CLASS(MissionInfo);
	GDREGISTER_CLASS(MissionData);
	GDREGISTER_CLASS(MissionCatalogRow);
	GDREGISTER_CLASS(MissionCatalog);
	// The present passes EntityPresenter owns (ADR 0043 d9): two are
	// RefCounted only so their anchor Callables have an Object target;
	// registered internally, never script-visible.
	GDREGISTER_INTERNAL_CLASS(RetainedArrayMesh);
	GDREGISTER_INTERNAL_CLASS(DestructionPresenter);
	GDREGISTER_INTERNAL_CLASS(ThrowablePresenter);
	GDREGISTER_INTERNAL_CLASS(VehicleTrailPresenter);
	GDREGISTER_CLASS(EntityPresenter);
	GDREGISTER_CLASS(PersonOverlayModels);
	GDREGISTER_CLASS(MissionPresentStats);
	GDREGISTER_CLASS(EntityRow);
	GDREGISTER_CLASS(EndRoundState);
	GDREGISTER_CLASS(EntityCardSeat);
	GDREGISTER_CLASS(EntityCard);
	GDREGISTER_CLASS(MissionFrameInput);
	GDREGISTER_CLASS(MissionFrameOutcome);
	GDREGISTER_CLASS(MissionSetupOptions);
	GDREGISTER_CLASS(MissionPerfCounters);
	GDREGISTER_CLASS(MissionRoot);
	GDREGISTER_CLASS(WirePresentStats);
	GDREGISTER_CLASS(ScarDrawList);
	GDREGISTER_CLASS(ScarPresenterStats);
	GDREGISTER_CLASS(FirePresentStats);
	GDREGISTER_CLASS(DestructionPresentStats);
	GDREGISTER_CLASS(ThrowablePresentStats);
	GDREGISTER_CLASS(ScarPresentStats);
	GDREGISTER_CLASS(Simulation);
	GDREGISTER_CLASS(LwfData);
	GDREGISTER_CLASS(WavLoader);
	GDREGISTER_CLASS(AmbientMixer);
	GDREGISTER_CLASS(SoundSelector);
	GDREGISTER_CLASS(DbfData);
	GDREGISTER_CLASS(AmbientLayer);
	GDREGISTER_CLASS(SoundBank);
	GDREGISTER_CLASS(MissionAudioMarker);
	GDREGISTER_CLASS(MissionAudioChannel);
	GDREGISTER_CLASS(MissionAudioCandidateBinding);
	GDREGISTER_CLASS(MissionAudioDynamicEmitter);
	GDREGISTER_CLASS(MissionAudioStats);
	GDREGISTER_CLASS(MissionAudioPerf);
	GDREGISTER_CLASS(FiredSoundset);
	GDREGISTER_CLASS(MissionAudio);
	GDREGISTER_ABSTRACT_CLASS(CbinEntry);
	GDREGISTER_CLASS(CbinTextEntry);
	GDREGISTER_CLASS(CbinNewlineEntry);
	GDREGISTER_CLASS(CbinImageEntry);
	GDREGISTER_CLASS(CbinCreditsResource);
	GDREGISTER_CLASS(CreditsPlayer);
	GDREGISTER_CLASS(FntResource);
	GDREGISTER_CLASS(RtxtStringFile);
	GDREGISTER_CLASS(Paths);
	GDREGISTER_CLASS(ParticleEffect);
	GDREGISTER_CLASS(ParticleFile);
	GDREGISTER_CLASS(EffectLoadReport);
	GDREGISTER_CLASS(EffectSpawnRequest);
	GDREGISTER_CLASS(EffectSpawnReceipt);
	GDREGISTER_CLASS(EffectSpawnOptions);
	GDREGISTER_CLASS(EffectScene);
	GDREGISTER_CLASS(EffectEmitterReport);
	GDREGISTER_CLASS(EffectGroupReport);
	GDREGISTER_CLASS(EffectLightRow);
	GDREGISTER_CLASS(EffectLightReport);
	GDREGISTER_CLASS(LightScene);
	GDREGISTER_CLASS(GlowSpawn);
	GDREGISTER_CLASS(ModelLightSpawn);
	GDREGISTER_CLASS(EffectLightDirector);
	GDREGISTER_CLASS(ParticleCompositorEffect);
	GDREGISTER_CLASS(SceneOverlayCompositorEffect);
	GDREGISTER_CLASS(TargetProjectionXrInterface);
	GDREGISTER_CLASS(ParticleRenderer);
	GDREGISTER_CLASS(EffectWorld);
	GDREGISTER_CLASS(ItemEffectDirectorStats);
	GDREGISTER_CLASS(ItemEffectDirector);
	GDREGISTER_CLASS(OcclusionFrame);
	// The world (ADR 0043 slice G10): the world node, its two view interfaces
	// (the live implementations are internal), the resource-root resolver
	// hook, the load timeline, the loading-screen record and the typed perf
	// counters.
	GDREGISTER_CLASS(ResourceRootResolver);
	GDREGISTER_CLASS(LoadTimelineSpan);
	GDREGISTER_CLASS(LoadTimeline);
	GDREGISTER_CLASS(LoadingScreenInfo);
	GDREGISTER_CLASS(PostMissionRoute);
	GDREGISTER_CLASS(RuntimePerfCounters);
	GDREGISTER_CLASS(WorldView);
	GDREGISTER_CLASS(ArmoryWorldView);
	GDREGISTER_INTERNAL_CLASS(LiveWorldView);
	GDREGISTER_INTERNAL_CLASS(LiveArmoryWorldView);
	GDREGISTER_CLASS(GameWorld);
	GDREGISTER_CLASS(FrameFxCompositorEffect);
	GDREGISTER_CLASS(FrameFx);
	GDREGISTER_CLASS(DisplayDecode);
	GDREGISTER_CLASS(ScarPresenter);
	GDREGISTER_CLASS(ResourceRoot);
	GDREGISTER_CLASS(LaunchFlags);
	GDREGISTER_CLASS(SbfAudioStream);
	GDREGISTER_CLASS(SbfAudioStreamPlayback);
	GDREGISTER_CLASS(SbfBank);
	GDREGISTER_CLASS(MusicScript);
	GDREGISTER_CLASS(MusicPairNames);
	GDREGISTER_CLASS(MusicDirector);
	GDREGISTER_CLASS(MnuDocument);
	GDREGISTER_CLASS(MnuSoundRow);
	GDREGISTER_CLASS(MnuActionRow);
	GDREGISTER_CLASS(MnsStyleSheet);
	GDREGISTER_CLASS(MenuDrawListStats);
	GDREGISTER_CLASS(MenuFrame);
	GDREGISTER_CLASS(MenuAudio);
	GDREGISTER_CLASS(MenuScrollRange);
	GDREGISTER_CLASS(MenuDriver);
	GDREGISTER_CLASS(MenuVideoUnderlay);
	GDREGISTER_CLASS(ControlsModel);
	// The local player's presentation (ADR 0043 slice G8): the presenter node,
	// its registered viewmodel rig, the world's local-player visuals, the
	// gameplay-camera seam and the four typed records; the weapon-effects
	// object is RefCounted only so its anchor Callables have an Object target.
	GDREGISTER_CLASS(PlayerMoveIntent);
	GDREGISTER_CLASS(PlayerSpawnLoadout);
	GDREGISTER_CLASS(PlayerViewmodelDef);
	GDREGISTER_CLASS(FirstPersonArmsWitness);
	GDREGISTER_CLASS(GameplayCamera);
	GDREGISTER_CLASS(PlayerViewmodelRig);
	GDREGISTER_INTERNAL_CLASS(PlayerWeaponEffects);
	GDREGISTER_CLASS(LocalPlayerVisuals);
	GDREGISTER_CLASS(LocalPlayerPresenter);
	GDREGISTER_CLASS(NovaWorldClient);
	GDREGISTER_CLASS(HostSessionOptions);
	GDREGISTER_CLASS(JoinTarget);
	GDREGISTER_CLASS(NovaWorldServerRow);
	GDREGISTER_CLASS(NovaWorldGateInfo);
	GDREGISTER_CLASS(NovaWorldServerBrowser);
	GDREGISTER_CLASS(UdpDatagram);
	GDREGISTER_CLASS(UdpPump);
	GDREGISTER_CLASS(LanSession);
	GDREGISTER_CLASS(LanServerRow);
	GDREGISTER_CLASS(NetProtocol);
	GDREGISTER_CLASS(NetSessionPolicy);
	// The ImGui pass seams (ADR 0039): registered in every flavour so scripts
	// parse; the release DLL's DevTools is inert.
	GDREGISTER_CLASS(FrameStatsWindow);
	GDREGISTER_CLASS(FrameStats);
	GDREGISTER_CLASS(ImGuiPassNode);
	GDREGISTER_CLASS(DevTools);
#if OPENNOVA_EDITOR
	// The OpenNova Editor's shell (ADR 0046 d4): the editor-enabled variant only, so
	// nothing the game ships depends on the editor. The script device's control and its
	// colours (S13 V10) with it.
	GDREGISTER_CLASS(EditorApp);
	GDREGISTER_CLASS(ScriptEdit);
	GDREGISTER_CLASS(ScriptHighlighter);
#endif
	// The debug-control table F3 and MCP share (ADR 0043 d12), in every
	// flavour: only the ImGui windows are debug-only.
	GDREGISTER_CLASS(DebugArgSpec);
	GDREGISTER_CLASS(DebugMarshalResult);
	GDREGISTER_CLASS(DebugControlRow);
	GDREGISTER_CLASS(DebugControlState);
	GDREGISTER_CLASS(DebugInvokeResult);
	GDREGISTER_CLASS(DebugShellHost);
	GDREGISTER_CLASS(DebugControlTable);
}

void uninitialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ObjectShaderCache::destroy_singleton();
	TargetProjectionXrInterface::cleanup_statics();
	SlotShadow::cleanup_statics();
	Q3SourceRegistry::cleanup_statics();
	ObjectData::clear_static_caches();
	opennova::clear_texture_resolver_caches();
}

extern "C" {
GDExtensionBool GDE_EXPORT opennova_library_init(
		GDExtensionInterfaceGetProcAddress p_get_proc_address,
		GDExtensionClassLibraryPtr p_library,
		GDExtensionInitialization *r_initialization) {
	godot::GDExtensionBinding::InitObject init_obj(p_get_proc_address, p_library, r_initialization);

	init_obj.register_initializer(initialize_opennova_module);
	init_obj.register_terminator(uninitialize_opennova_module);
	init_obj.set_minimum_library_initialization_level(MODULE_INITIALIZATION_LEVEL_SCENE);

	return init_obj.init();
}
}
