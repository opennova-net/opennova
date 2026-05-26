#include "register_types.h"

#include <gdextension_interface.h>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/defs.hpp>
#include <godot_cpp/godot.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/resource_saver.hpp>

#include "terrain/nova_terrain_data.h"
#include "terrain/nova_terrain.h"
#include "terrain/nova_terrain_builder.h"
#include "terrain/nova_terrain_build_job.h"
#include "terrain/nova_terrain_foliage_def.h"
#include "terrain/nova_terrain_foliage_map.h"
#include "terrain/nova_foliage_dispatcher.h"
#include "terrain/nova_terrain_tile_entry.h"
#include "terrain/nova_terrain_tile_info.h"
#include "terrain/nova_terrain_tile_overlay.h"
#include "terrain/trn_resource_format.h"
#include "terrain/cpt_resource_format.h"
#include "terrain/til_resource_format.h"
#include "env/nova_env_keyframe.h"
#include "env/env_file.h"
#include "object/nova_object_data.h"
#include "object/nova_object_shader_cache.h"
#include "util/nova_data_format.h"
#include "util/nova_texture_format.h"
#include "resource_index/nova_resource_index.h"

using namespace godot;

static Ref<ResourceFormatLoaderTRN> trn_loader;
static Ref<ResourceFormatSaverTRN> trn_saver;
static Ref<ResourceFormatLoaderCPT> cpt_loader;
static Ref<ResourceFormatLoaderTIL> til_loader;
static Ref<ResourceFormatSaverTIL> til_saver;
static Ref<EnvFileLoader> env_loader;
static Ref<EnvFileSaver> env_saver;
static Ref<ResourceFormatLoaderNovaTexture> nova_tex_loader;

void initialize_opennova_module(ModuleInitializationLevel p_level) {
	if (p_level != MODULE_INITIALIZATION_LEVEL_SCENE) {
		return;
	}

	GDREGISTER_CLASS(NovaTerrainData);
	GDREGISTER_CLASS(NovaTerrain);
	GDREGISTER_CLASS(NovaTerrainBuilder);
	GDREGISTER_CLASS(NovaTerrainBuildJob);
	GDREGISTER_CLASS(NovaTerrainFoliageDef);
	GDREGISTER_CLASS(NovaTerrainFoliageMap);
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
	GDREGISTER_CLASS(NovaObjectData);
	GDREGISTER_CLASS(NovaObjectShaderCache);
	GDREGISTER_CLASS(NovaDataFile);
	GDREGISTER_CLASS(ResourceFormatLoaderNovaTexture);
	GDREGISTER_CLASS(NovaResourceIndex);

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

	nova_tex_loader.instantiate();
	ResourceLoader::get_singleton()->add_resource_format_loader(nova_tex_loader, true);
}

void uninitialize_opennova_module(ModuleInitializationLevel p_level) {
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

	ResourceLoader::get_singleton()->remove_resource_format_loader(nova_tex_loader);
	nova_tex_loader.unref();
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
