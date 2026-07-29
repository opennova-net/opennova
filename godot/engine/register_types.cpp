#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/editor_plugin_registration.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

#include "terrain/nova_terrain_data.h"
#include "terrain/nova_terrain.h"
#include "terrain/nova_terrain_surface_inputs.h"
#include "terrain/nova_terrain_builder.h"
#include "terrain/nova_terrain_build_job.h"
#include "terrain/nova_terrain_foliage_def.h"
#include "terrain/nova_terrain_foliage_map.h"
#include "terrain/nova_terrain_surface_map.h"
#include "terrain/nova_foliage_dispatcher.h"
#include "terrain/nova_terrain_tile_entry.h"
#include "terrain/nova_terrain_tile_info.h"
#include "terrain/nova_terrain_tile_overlay.h"
#include "terrain/trn_resource_format.h"
#include "terrain/cpt_resource_format.h"
#include "terrain/til_resource_format.h"
#include "env/nova_env_keyframe.h"
#include "env/env_file.h"
#include "env/nova_color_smoother.h"
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
#include "particle/nova_effect_scene.h"
#include "particle/nova_particle_emitter.h"
#include "particle/nova_particle_compositor.h"
#include "particle/nova_particle_renderer.h"
#include "particle/ptl_resource_format.h"
#include "object/nova_object_data.h"
#include "object/nova_object_shader_cache.h"
#include "object/nova_item_database.h"
#include "object/nova_weapon_database.h"
#include "object/nova_avatar_database.h"
#include "object/nova_skeletal_anim.h"
#include "hud/nova_hud_pos.h"
#include "mission/nova_mission_data.h"
#include "simulation/nova_present_applier.h"
#include "simulation/nova_simulation.h"
#include "wac/nova_wac_program.h"
#include "lwf/nova_lwf_data.h"
#include "lwf/nova_wav_loader.h"
#include "audio/nova_ambient_mixer.h"
#include "audio/nova_sound_selector.h"
#include "dbf/nova_dbf_data.h"
#include "cbin/cbin_credits_resource.h"
#include "cbin/kda_resource_format.h"
#include "cbin/nova_credits_player.h"
#include "fnt/nova_fnt_resource.h"
#include "fnt/fnt_resource_format.h"
#include "fnt/fnt_import_plugin.h"
#include "editor/opennova_editor_plugin.h"
#include "editor/nova_edit_history.h"
#include "rtxt/rtxt_string_file.h"
#include "rtxt/rtxt_resource_format.h"
#include "network/nova_world_client.h"
#include "network/nova_world_host.h"
#include "network/nova_net_client.h"
#include "network/nova_udp_pump.h"
#include "network/nova_lan_session.h"
#include "util/nova_data_format.h"
#include "util/nova_paths.h"
#include "util/nova_texture_format.h"
#include "resource_index/nova_resource_index.h"
#include "refs/nova_reference_index.h"
#include "resource_index/nova_resource_root.h"
#include "audio/nova_sbf_bank.h"
#include "audio/nova_sbf_audio_stream.h"
#include "audio/nova_sbf_audio_stream_playback.h"
#include "audio/sbf_resource_format.h"
#include "audio/nova_music_script.h"
#include "audio/nova_music_director.h"
#include "audio/mus_resource_format.h"
#include "pff/nova_pff_archive.h"
#include "mnu/nova_mnu_document.h"
#include "mnu/mns_stylesheet.h"
#include "mnu/mnu_resource_format.h"
#include "mnu/mns_resource_format.h"
#include "mnu/nova_mnu_screen.h"
#include "mnu/nova_mnu_label.h"
#include "mnu/nova_mnu_button.h"
#include "mnu/nova_mnu_checkbox.h"
#include "mnu/nova_mnu_combo.h"
#include "mnu/nova_mnu_edit.h"
#include "mnu/nova_mnu_multiline_edit.h"
#include "mnu/nova_mnu_goto.h"
#include "mnu/nova_mnu_list.h"
#include "mnu/nova_mnu_multi.h"
#include "mnu/nova_mnu_spinlist.h"
#include "mnu/nova_mnu_scroll.h"
#include "mnu/nova_mnu_table.h"
#include "mnu/nova_mnu_map.h"
#include "mnu/nova_mnu_globe.h"
#include "mnu/nova_mnu_marquee.h"
#include "mnu/nova_mnu_menu.h"
#include "mnu/nova_controls_model.h"

using namespace godot;

static Ref<ResourceFormatLoaderTRN> trn_loader;
static Ref<ResourceFormatSaverTRN> trn_saver;
static Ref<ResourceFormatLoaderCPT> cpt_loader;
static Ref<ResourceFormatLoaderTIL> til_loader;
static Ref<ResourceFormatSaverTIL> til_saver;
static Ref<EnvFileLoader> env_loader;
static Ref<EnvFileSaver> env_saver;
static Ref<KdaResourceFormatLoader> kda_loader;
static Ref<KdaResourceFormatSaver> kda_saver;
static Ref<ResourceFormatLoaderFNT> fnt_loader;
static Ref<ResourceFormatSaverFNT> fnt_saver;
static Ref<ResourceFormatLoaderNovaTexture> nova_tex_loader;
static Ref<ResourceFormatLoaderPTL> ptl_loader;
static Ref<ResourceFormatSaverPTL> ptl_saver;
static Ref<ResourceFormatLoaderRTXT> rtxt_loader;
static Ref<ResourceFormatSaverRTXT> rtxt_saver;
static Ref<SbfResourceFormatLoader> sbf_loader;
static Ref<SbfResourceFormatSaver> sbf_saver;
static Ref<MusResourceFormatLoader> mus_loader;
static Ref<MusResourceFormatSaver> mus_saver;
static Ref<ResourceFormatLoaderMNU> mnu_loader;
static Ref<ResourceFormatSaverMNU> mnu_saver;
static Ref<ResourceFormatLoaderMNS> mns_loader;
static Ref<ResourceFormatSaverMNS> mns_saver;

void initialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		GDREGISTER_CLASS(NovaFntImportPlugin);
		GDREGISTER_CLASS(OpenNovaEditorPlugin);
		EditorPlugins::add_by_type<OpenNovaEditorPlugin>();
		return;
	}

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(NovaTerrainData);
	GDREGISTER_CLASS(NovaTerrain);
	GDREGISTER_CLASS(NovaTerrainSurfaceInputs);
	GDREGISTER_CLASS(NovaTerrainBuilder);
	GDREGISTER_CLASS(NovaTerrainBuildJob);
	GDREGISTER_CLASS(NovaTerrainFoliageDef);
	GDREGISTER_CLASS(NovaTerrainFoliageMap);
	GDREGISTER_CLASS(NovaTerrainSurfaceMap);
	GDREGISTER_CLASS(NovaFoliageDispatcher);
	GDREGISTER_CLASS(NovaTerrainTileEntry);
	GDREGISTER_CLASS(NovaTerrainTileInfo);
	GDREGISTER_CLASS(NovaTerrainTileOverlay);
	GDREGISTER_CLASS(ResourceFormatLoaderTRN);
	GDREGISTER_CLASS(ResourceFormatSaverTRN);
	GDREGISTER_CLASS(ResourceFormatLoaderCPT);
	GDREGISTER_CLASS(ResourceFormatLoaderTIL);
	GDREGISTER_CLASS(ResourceFormatSaverTIL);
	GDREGISTER_CLASS(NovaEnvKeyframe);
	GDREGISTER_CLASS(EnvFile);
	GDREGISTER_CLASS(EnvFileLoader);
	GDREGISTER_CLASS(EnvFileSaver);
	GDREGISTER_CLASS(NovaColorSmoother);
	GDREGISTER_CLASS(NovaWeatherCore);
	GDREGISTER_CLASS(NovaWaterCore);
	GDREGISTER_CLASS(NovaStarField);
	GDREGISTER_CLASS(NovaGlareOcclusion);
	GDREGISTER_CLASS(NovaObjectData);
	GDREGISTER_CLASS(NovaObjectShaderCache);
	GDREGISTER_CLASS(NovaItemDatabase);
	GDREGISTER_CLASS(NovaWeaponDatabase);
	GDREGISTER_CLASS(NovaAvatarDatabase);
	GDREGISTER_CLASS(NovaSkeletalAnim);
	GDREGISTER_CLASS(NovaHudPos);
	GDREGISTER_CLASS(NovaMissionData);
	GDREGISTER_CLASS(NovaPresentApplier);
	GDREGISTER_CLASS(NovaSimulation);
	GDREGISTER_CLASS(NovaEditHistory);
	GDREGISTER_CLASS(NovaWacProgram);
	GDREGISTER_CLASS(NovaLwfData);
	GDREGISTER_CLASS(NovaWavLoader);
	GDREGISTER_CLASS(NovaAmbientMixer);
	GDREGISTER_CLASS(NovaSoundSelector);
	GDREGISTER_CLASS(NovaDbfData);
	GDREGISTER_ABSTRACT_CLASS(CbinEntry);
	GDREGISTER_CLASS(CbinTextEntry);
	GDREGISTER_CLASS(CbinNewlineEntry);
	GDREGISTER_CLASS(CbinImageEntry);
	GDREGISTER_CLASS(CbinCreditsResource);
	GDREGISTER_CLASS(KdaResourceFormatLoader);
	GDREGISTER_CLASS(KdaResourceFormatSaver);
	GDREGISTER_CLASS(NovaCreditsPlayer);
	GDREGISTER_CLASS(NovaFntResource);
	GDREGISTER_CLASS(ResourceFormatLoaderFNT);
	GDREGISTER_CLASS(ResourceFormatSaverFNT);
	GDREGISTER_CLASS(RtxtStringFile);
	GDREGISTER_CLASS(ResourceFormatLoaderRTXT);
	GDREGISTER_CLASS(ResourceFormatSaverRTXT);
	GDREGISTER_CLASS(NovaDataFile);
	GDREGISTER_CLASS(NovaPaths);
	GDREGISTER_CLASS(ResourceFormatLoaderNovaTexture);
	GDREGISTER_CLASS(NovaParticleCurveRef);
	GDREGISTER_CLASS(NovaParticleEffect);
	GDREGISTER_CLASS(NovaParticleTableHandles);
	GDREGISTER_CLASS(NovaParticleTable);
	GDREGISTER_CLASS(NovaParticleGraphicLayer);
	GDREGISTER_CLASS(NovaParticleDef);
	GDREGISTER_CLASS(NovaParticleFile);
	GDREGISTER_CLASS(NovaEffectScene);
	GDREGISTER_CLASS(NovaParticleEmitter);
	GDREGISTER_CLASS(NovaParticleCompositorEffect);
	GDREGISTER_CLASS(NovaParticleRenderer);
	GDREGISTER_CLASS(ResourceFormatLoaderPTL);
	GDREGISTER_CLASS(ResourceFormatSaverPTL);
	GDREGISTER_CLASS(NovaResourceIndex);
	GDREGISTER_CLASS(NovaReferenceIndex);
	GDREGISTER_CLASS(NovaResourceRoot);
	GDREGISTER_CLASS(NovaSbfAudioStream);
	GDREGISTER_CLASS(NovaSbfAudioStreamPlayback);
	GDREGISTER_CLASS(NovaSbfBank);
	GDREGISTER_CLASS(SbfResourceFormatLoader);
	GDREGISTER_CLASS(SbfResourceFormatSaver);
	GDREGISTER_CLASS(NovaMusicScript);
	GDREGISTER_CLASS(NovaMusicDirector);
	GDREGISTER_CLASS(MusResourceFormatLoader);
	GDREGISTER_CLASS(MusResourceFormatSaver);
	GDREGISTER_CLASS(NovaPffArchive);
	GDREGISTER_CLASS(NovaMnuDocument);
	GDREGISTER_CLASS(MnsStyleSheet);
	GDREGISTER_CLASS(ResourceFormatLoaderMNU);
	GDREGISTER_CLASS(ResourceFormatSaverMNU);
	GDREGISTER_CLASS(ResourceFormatLoaderMNS);
	GDREGISTER_CLASS(ResourceFormatSaverMNS);
	GDREGISTER_CLASS(NovaMnuScreen);
	GDREGISTER_CLASS(NovaMnuLabel);
	GDREGISTER_CLASS(NovaMnuButton);
	GDREGISTER_CLASS(NovaMnuCheckBox);
	GDREGISTER_CLASS(NovaMnuEdit);
	GDREGISTER_CLASS(NovaMnuMultilineEdit);
	GDREGISTER_CLASS(NovaMnuGoto);
	GDREGISTER_CLASS(NovaMnuList);
	GDREGISTER_CLASS(NovaMnuMulti);
	GDREGISTER_CLASS(NovaMnuSpinList);
	GDREGISTER_CLASS(NovaMnuScroll);
	GDREGISTER_CLASS(NovaMnuCombo);
	GDREGISTER_CLASS(NovaMnuTable);
	GDREGISTER_CLASS(NovaMnuMap);
	GDREGISTER_CLASS(NovaMnuGlobe);
	GDREGISTER_CLASS(NovaMnuMarquee);
	GDREGISTER_CLASS(NovaMnuMenu);
	GDREGISTER_CLASS(NovaControlsModel);
	GDREGISTER_CLASS(NovaWorldClient);
	GDREGISTER_CLASS(NovaWorldHost);
	GDREGISTER_CLASS(NovaNetClient);
	GDREGISTER_CLASS(NovaUdpPump);
	GDREGISTER_CLASS(NovaLanSession);

	trn_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(trn_loader);

	trn_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(trn_saver);

	cpt_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(cpt_loader);

	til_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(til_loader);

	til_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(til_saver);

	env_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(env_loader);

	env_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(env_saver);

	kda_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(kda_loader);

	kda_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(kda_saver);

	fnt_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(fnt_loader);

	fnt_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(fnt_saver);

	nova_tex_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(nova_tex_loader, true);

	ptl_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(ptl_loader);

	ptl_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(ptl_saver);

	rtxt_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(rtxt_loader);

	rtxt_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(rtxt_saver);

	sbf_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(sbf_loader);

	sbf_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(sbf_saver);

	mus_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(mus_loader);

	mus_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(mus_saver);

	mnu_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(mnu_loader);

	mnu_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(mnu_saver);

	mns_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(mns_loader);

	mns_saver.instantiate();
	ResourceSaver::get_singleton()->add_resource_format_saver(mns_saver);
}

void uninitialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level == MODULE_INITIALIZATION_LEVEL_EDITOR) {
		EditorPlugins::remove_by_type<OpenNovaEditorPlugin>();
		return;
	}

	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	ResourceLoader::get_singleton()->remove_resource_format_loader(trn_loader);
	trn_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(trn_saver);
	trn_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(cpt_loader);
	cpt_loader.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(til_loader);
	til_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(til_saver);
	til_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(env_loader);
	env_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(env_saver);
	env_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(kda_loader);
	kda_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(kda_saver);
	kda_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(fnt_loader);
	fnt_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(fnt_saver);
	fnt_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(nova_tex_loader);
	nova_tex_loader.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(ptl_loader);
	ptl_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(ptl_saver);
	ptl_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(rtxt_loader);
	rtxt_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(rtxt_saver);
	rtxt_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(sbf_loader);
	sbf_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(sbf_saver);
	sbf_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(mus_loader);
	mus_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(mus_saver);
	mus_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(mnu_loader);
	mnu_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(mnu_saver);
	mnu_saver.unref();

	ResourceLoader::get_singleton()->remove_resource_format_loader(mns_loader);
	mns_loader.unref();

	ResourceSaver::get_singleton()->remove_resource_format_saver(mns_saver);
	mns_saver.unref();

	NovaObjectShaderCache::destroy_singleton();
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
