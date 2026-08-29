#include "./register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <base/io/log.h>

#include "terrain/terrain_data.h"
#include "terrain/terrain.h"
#include "terrain/terrain_surface_inputs.h"
#include "terrain/terrain_foliage_def.h"
#include "terrain/terrain_foliage_map.h"
#include "terrain/foliage_dispatcher.h"
#include "terrain/terrain_tile_entry.h"
#include "terrain/terrain_tile_info.h"
#include "env/env_keyframe.h"
#include "env/env_file.h"
#include "env/color_smoother.h"
#include "env/mission_environment.h"
#include "env/celestial.h"
#include "env/environment_cube_capture.h"
#include "mission/mission_object_placer.h"
#include "env/sky_dome.h"
#include "env/slot_shadow.h"
#include "env/sun_shadow.h"
#include "env/water.h"
#include "env/weather.h"
#include "env/weather_core.h"
#include "env/glare_occlusion.h"
#include "env/star_field.h"
#include "env/water_core.h"
#include "particle/particle_curve_ref.h"
#include "particle/particle_effect.h"
#include "particle/particle_table_handles.h"
#include "particle/particle_table.h"
#include "particle/particle_graphic_layer.h"
#include "particle/particle_def.h"
#include "particle/particle_file.h"
#include "lights/light_scene.h"
#include "particle/effect_scene.h"
#include "particle/particle_compositor.h"
#include "particle/particle_renderer.h"
#include "render/frame_fx.h"
#include "world/scar_presenter.h"
#include "object/entity_index.h"
#include "object/object_data.h"
#include "object/object_model.h"
#include "object/object_shader_cache.h"
#include "object/item_database.h"
#include "object/weapon_database.h"
#include "object/avatar_database.h"
#include "object/skeletal_anim.h"
#include "hud/hud_overlay.h"
#include "devtools/imgui_pass_node.h"
#include "devtools/dev_tools.h"
#include "devtools/frame_stats.h"
#include "devtools/oned_ui.h"
#include "hud/hud_pos.h"
#include "mission/mission_catalog.h"
#include "mission/mission_data.h"
#include "simulation/entity_card.h"
#include "simulation/entity_row.h"
#include "simulation/present_applier.h"
#include "simulation/present_stats.h"
#include "simulation/inmatch_session_values.h"
#include "simulation/wire_present_pass.h"
#include "simulation/simulation.h"
#include "wac/wac_program.h"
#include "lwf/lwf_data.h"
#include "lwf/wav_loader.h"
#include "audio/ambient_mixer.h"
#include "audio/sound_selector.h"
#include "dbf/dbf_data.h"
#include "cbin/cbin_credits_resource.h"
#include "cbin/credits_player.h"
#include "fnt/fnt_resource.h"
#include "rtxt/rtxt_string_file.h"
#include "network/novaworld_client.h"
#include "network/novaworld_host.h"
#include "network/udp_pump.h"
#include "network/lan_session.h"
#include "network/net_protocol.h"
#include "network/net_session_policy.h"
#include "util/paths.h"
#include "util/process.h"
#include "resource_index/launch_flags.h"
#include "resource_index/resource_root.h"
#include "audio/sbf_bank.h"
#include "audio/sbf_audio_stream.h"
#include "audio/sbf_audio_stream_playback.h"
#include "audio/music_script.h"
#include "audio/music_director.h"
#include "pff/pff_document.h"
#include "mnu/mnu_document.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/menu_frame.h"
#include "mnu/menu_audio.h"
#include "mnu/menu_video_underlay.h"
#include "mnu/controls_model.h"

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
	GDREGISTER_CLASS(EntityRow);
	GDREGISTER_CLASS(EntityCardSeat);
	GDREGISTER_CLASS(EntityCard);
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
	GDREGISTER_CLASS(LaunchFlags);
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
	// The ImGui pass seams (ADR 0039): registered in every flavour so scripts
	// parse; the release DLL's DevTools is inert, OnedUi runs everywhere.
	GDREGISTER_CLASS(FrameStatsWindow);
	GDREGISTER_CLASS(FrameStats);
	GDREGISTER_ABSTRACT_CLASS(ImGuiPassNode);
	GDREGISTER_CLASS(DevTools);
	GDREGISTER_CLASS(OnedUiRequest);
	GDREGISTER_CLASS(OnedUi);
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
