// The Simulation's retained asset sources (ADR 0028, ADR 0043 d9): the
// mounted root and the item/terrain/sound/score documents the kernel legs
// resolve through, the Refs that pin the shell's sources across a reload
// (reset_world recreates the kernel), the replication catalog, the held WAC
// program and the weather render owner. Plain data with no behavior.
#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>

#include <runtime/renderer/precipitation_frame.h>         // the precipitation drawer state + frame
#include <runtime/replication/item_replication_catalog.h> // canonical items.def replication traits

#include "wac/wac_program.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace godot {

class ItemDatabase;
class ResourceRoot;
class TerrainData;

struct SimulationAssetState {
	// The sim's asset source (ADR 0028, ADR 0044): the mounted root pinned for
	// the lifetime of the shared asset store the kernel reads through
	// (set_assets). Render caches stay render-only.
	Ref<ResourceRoot> root;
	// The shell input the collision sweep reads (its retained items.def rows
	// feed the engine resolve). RefCounted, so retaining it also keeps its
	// object/ADM caches alive for a later RoundSim or F3 query.
	Ref<ItemDatabase> collision_item_db;
	// Mission-scoped source for the authoritative half of the item-trait
	// contract. World::restore rewinds registry entities to the pre-trait
	// promotion baseline, so restart reapplies this database before rebuilding
	// decoded client replicas.
	Ref<ItemDatabase> item_traits_db;
	// One immutable items.def catalog supplies both the authoritative entity
	// stamp and the decoded client's record widths and def facts
	// (ClientReplicaPipeline::set_item_catalog). The callback codec,
	// physical motion family, and allocation inputs remain independent traits.
	std::shared_ptr<const opennova::replication::ItemReplicationCatalog>
			item_replication_catalog;
	Ref<ItemDatabase> item_replication_catalog_db;
	uint64_t item_replication_catalog_revision = 0;
	// Anim-driven soldier locomotion: the kernel owns the .adm/.bad-backed
	// root-motion source (kernel_->root_motion) and the per-entity resolution
	// sweep. These pin the shell's sources (the anim root and item db a
	// joiner's decoded-row resolve reads through).
	Ref<ResourceRoot> infantry_adm_resource_root;
	Ref<ItemDatabase> infantry_adm_item_db;
	// Terrain: the kernel owns the one cpt/trn(+charmap) field store
	// (kernel_->terrain_store, ADR 0042 d4); the source TerrainData Ref is
	// retained so a reload (which recreates the kernel) can rebuild it.
	Ref<TerrainData> terrain_data;
	// SndProf.def text + water plane held for (re)application on reset_world.
	std::vector<uint8_t> sndprof_text;
    bool sound_profiles_override = false;
	int32_t env_water_z_q16 = 0;
	// The packed Avatars.def character-sex registry the portable sound-profile
	// selector reads; retained across reset_world.
	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	std::vector<CharacterSexRow> character_sex_rows;
	// The installed script program. Held as a shared_ptr so it survives
	// reset_world(); each (re)load re-applies it onto the fresh kernel
	// WacSystem when the kernel's own layered load installed none.
	std::shared_ptr<WacProgram> wac_program;
	// The Weather node bound through set_weather_render_owner; released
	// whenever the kernel (and the WeatherState it owns) is replaced or dies.
	ObjectID weather_owner_id;
	// The compiled precipitation streak frame, reused across frames (its
	// vertex capacity survives clear()); the drawer's memory is the kernel's
	// (MissionKernel::precipitation_draw, carried across loads).
	opennova::renderer::PrecipitationDrawFrame precipitation_frame;
};

} // namespace godot
