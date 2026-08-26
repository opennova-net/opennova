#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>

#include "terrain/nova_terrain_data.h"
#include "terrain/nova_terrain.h"
#include "terrain/nova_terrain_surface_inputs.h"
#include "terrain/nova_terrain_foliage_def.h"
#include "terrain/nova_terrain_foliage_map.h"
#include "terrain/nova_foliage_dispatcher.h"
#include "terrain/nova_terrain_tile_entry.h"
#include "terrain/nova_terrain_tile_info.h"
#include "env/nova_env_keyframe.h"
#include "env/env_file.h"
#include "env/nova_color_smoother.h"
#include "env/nova_mission_environment.h"
#include "env/nova_celestial.h"
#include "env/nova_environment_cube_capture.h"
#include "mission/nova_mission_object_placer.h"
#include "env/nova_sky_dome.h"
#include "env/nova_slot_shadow.h"
#include "env/nova_sun_shadow.h"
#include "env/nova_water.h"
#include "env/nova_weather.h"
#include "env/nova_weather_core.h"
#include "env/nova_glare_occlusion.h"
#include "env/nova_star_field.h"
#include "env/nova_water_core.h"
#include "particle/nova_particle_curve_ref.h"
#include "particle/nova_particle_effect.h"
#include "particle/nova_particle_table_handles.h"
#include "particle/nova_particle_table.h"
#include "particle/nova_particle_graphic_layer.h"
#include "particle/nova_particle_def.h"
#include "particle/nova_particle_file.h"
#include "lights/nova_light_scene.h"
#include "particle/nova_effect_scene.h"
#include "particle/nova_particle_compositor.h"
#include "particle/nova_particle_renderer.h"
#include "render/nova_framefx.h"
#include "world/nova_scar_presenter.h"
#include "object/nova_entity_index.h"
#include "object/nova_object_data.h"
#include "object/nova_object_model.h"
#include "object/nova_object_shader_cache.h"
#include "object/nova_item_database.h"
#include "object/nova_weapon_database.h"
#include "object/nova_avatar_database.h"
#include "object/nova_skeletal_anim.h"
#include "hud/hud_overlay.h"
#include "hud/nova_hud_pos.h"
#include "mission/nova_mission_catalog.h"
#include "mission/nova_mission_data.h"
#include "simulation/nova_present_applier.h"
#include "simulation/nova_present_stats.h"
#include "simulation/nova_inmatch_session_values.h"
#include "simulation/nova_wire_present_pass.h"
#include "simulation/nova_simulation.h"
#include "wac/nova_wac_program.h"
#include "lwf/nova_lwf_data.h"
#include "lwf/nova_wav_loader.h"
#include "audio/nova_ambient_mixer.h"
#include "audio/nova_sound_selector.h"
#include "dbf/nova_dbf_data.h"
#include "cbin/cbin_credits_resource.h"
#include "cbin/nova_credits_player.h"
#include "fnt/nova_fnt_resource.h"
#include "rtxt/rtxt_string_file.h"
#include "network/nova_world_client.h"
#include "network/nova_world_host.h"
#include "network/nova_udp_pump.h"
#include "network/nova_lan_session.h"
#include "network/nova_net_protocol.h"
#include "network/nova_net_session_policy.h"
#include "util/nova_paths.h"
#include "util/nova_process.h"
#include "resource_index/nova_resource_root.h"
#include "audio/nova_sbf_bank.h"
#include "audio/nova_sbf_audio_stream.h"
#include "audio/nova_sbf_audio_stream_playback.h"
#include "audio/nova_music_script.h"
#include "audio/nova_music_director.h"
#include "pff/nova_pff_archive.h"
#include "mnu/nova_mnu_document.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/nova_menu_frame.h"
#include "mnu/nova_menu_audio.h"
#include "mnu/menu_video_underlay.h"
#include "mnu/nova_controls_model.h"

using namespace godot;

void initialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(TerrainData);
	GDREGISTER_CLASS(Terrain);
	GDREGISTER_CLASS(TerrainSurfaceInputs);
	GDREGISTER_CLASS(TerrainFoliageDef);
	GDREGISTER_CLASS(TerrainFoliageMap);
	GDREGISTER_CLASS(FoliageDispatcher);
	GDREGISTER_CLASS(TerrainTileEntry);
	GDREGISTER_CLASS(TerrainTileInfo);
	GDREGISTER_CLASS(EnvKeyframe);
	GDREGISTER_CLASS(EnvFile);
	GDREGISTER_CLASS(ColorSmoother);
	GDREGISTER_CLASS(MissionEnvironment);
	GDREGISTER_CLASS(SkyDome);
	GDREGISTER_CLASS(Water);
	GDREGISTER_CLASS(Celestial);
	GDREGISTER_CLASS(EnvironmentCubeCapture);
	GDREGISTER_CLASS(MissionObjectPlacer);
	GDREGISTER_CLASS(SunShadow);
	GDREGISTER_CLASS(SlotShadow);
	GDREGISTER_CLASS(Weather);
	GDREGISTER_CLASS(WeatherCore);
	GDREGISTER_CLASS(WaterCore);
	GDREGISTER_CLASS(StarField);
	GDREGISTER_CLASS(GlareOcclusion);
	GDREGISTER_CLASS(ObjectData);
	GDREGISTER_CLASS(EnvLightValues);
	GDREGISTER_CLASS(EnvLightState);
	GDREGISTER_CLASS(PanmClock);
	GDREGISTER_CLASS(ObjectModel);
	GDREGISTER_CLASS(EntityIndex);
	GDREGISTER_CLASS(ObjectShaderCache);
	GDREGISTER_CLASS(ItemDatabase);
	GDREGISTER_CLASS(WeaponDatabase);
	GDREGISTER_CLASS(AvatarDatabase);
	GDREGISTER_CLASS(SkeletalAnim);
	GDREGISTER_CLASS(HudPos);
	GDREGISTER_CLASS(HudOverlay);
	GDREGISTER_CLASS(MissionData);
	GDREGISTER_CLASS(MissionCatalogRow);
	GDREGISTER_CLASS(MissionCatalog);
	GDREGISTER_CLASS(PresentApplier);
	GDREGISTER_CLASS(MissionPresentStats);
	GDREGISTER_CLASS(MissionFrameInput);
	GDREGISTER_CLASS(MissionTickOutcome);
	GDREGISTER_CLASS(MissionFrameOutcome);
	GDREGISTER_CLASS(WirePresentStats);
	GDREGISTER_CLASS(ScarPresenterStats);
	GDREGISTER_CLASS(WirePresentPass);
	GDREGISTER_CLASS(Simulation);
	GDREGISTER_CLASS(WacProgram);
	GDREGISTER_CLASS(LwfData);
	GDREGISTER_CLASS(WavLoader);
	GDREGISTER_CLASS(AmbientMixer);
	GDREGISTER_CLASS(SoundSelector);
	GDREGISTER_CLASS(DbfData);
	GDREGISTER_ABSTRACT_CLASS(CbinEntry);
	GDREGISTER_CLASS(CbinTextEntry);
	GDREGISTER_CLASS(CbinNewlineEntry);
	GDREGISTER_CLASS(CbinImageEntry);
	GDREGISTER_CLASS(CbinCreditsResource);
	GDREGISTER_CLASS(CreditsPlayer);
	GDREGISTER_CLASS(FntResource);
	GDREGISTER_CLASS(RtxtStringFile);
	GDREGISTER_CLASS(Paths);
	GDREGISTER_CLASS(Process);
	GDREGISTER_CLASS(ParticleCurveRef);
	GDREGISTER_CLASS(ParticleEffect);
	GDREGISTER_CLASS(ParticleTableHandles);
	GDREGISTER_CLASS(ParticleTable);
	GDREGISTER_CLASS(ParticleGraphicLayer);
	GDREGISTER_CLASS(ParticleDef);
	GDREGISTER_CLASS(ParticleFile);
	GDREGISTER_CLASS(EffectScene);
	GDREGISTER_CLASS(LightScene);
	GDREGISTER_CLASS(ParticleCompositorEffect);
	GDREGISTER_CLASS(ParticleRenderer);
	GDREGISTER_CLASS(FrameFxCompositorEffect);
	GDREGISTER_CLASS(FrameFx);
	GDREGISTER_CLASS(DisplayDecode);
	GDREGISTER_CLASS(ScarPresenter);
	GDREGISTER_CLASS(ResourceRoot);
	GDREGISTER_CLASS(SbfAudioStream);
	GDREGISTER_CLASS(SbfAudioStreamPlayback);
	GDREGISTER_CLASS(SbfBank);
	GDREGISTER_CLASS(MusicScript);
	GDREGISTER_CLASS(MusicDirector);
	GDREGISTER_CLASS(PffDocument);
	GDREGISTER_CLASS(MnuDocument);
	GDREGISTER_CLASS(MnsStyleSheet);
	GDREGISTER_CLASS(MenuFrame);
	GDREGISTER_CLASS(MenuAudio);
	GDREGISTER_CLASS(MenuVideoUnderlay);
	GDREGISTER_CLASS(ControlsModel);
	GDREGISTER_CLASS(NovaWorldClient);
	GDREGISTER_CLASS(NovaWorldHost);
	GDREGISTER_CLASS(UdpPump);
	GDREGISTER_CLASS(LanSession);
	GDREGISTER_CLASS(NetProtocol);
	GDREGISTER_CLASS(NetSessionPolicy);
}

void uninitialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ObjectShaderCache::destroy_singleton();
	SlotShadow::cleanup_statics();
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
