// The Simulation's retained asset sources (ADR 0028, ADR 0043 d9): the
// mounted root and the item/terrain/sound/score documents the kernel legs
// resolve through, the Refs that pin the shell's sources across a reload
// (reset_world recreates the kernel), the replication catalog, the held WAC
// program and the weather render owner. Plain data with no behavior.
#pragma once

#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/core/object_id.hpp>

#include <formats/score/score.h>                          // the retained score.ini parse
#include <runtime/renderer/precipitation_frame.h>         // the precipitation drawer state + frame
#include <runtime/replication/item_replication_catalog.h> // canonical items.def replication traits
#include <runtime/terrain_query/surface_type_map.h>       // SurfaceTileEntry (the D-SND-15 placed tiles)

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
	// The sim's asset source (ADR 0028): the mounted root pinned for its index
	// lifetime; the kernel's parse-once model cache reads through it
	// (set_asset_index). Render caches stay render-only.
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
	// stamp and the decoded-client record-width resolver. The callback codec,
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
	// The placed-tile surface override (D-SND-15): the mission .til entries
	// plus the tileset's .TSD-fed tile-index -> surface table, both resolved
	// engine-side (terrain_query surface_tiles.h — the witnesses live there).
	// Owned here like the heightmap so world.tables.surface_map's raw pointers
	// survive reset_world; the zero table is retail's no-.TSD default.
	std::vector<opennova::terrain::SurfaceTileEntry> surface_tiles;
	std::array<uint8_t, 256> tile_surface_table{};
	// SndProf.def text + water plane held for (re)application on reset_world.
	std::vector<uint8_t> sndprof_text;
	int32_t env_water_z_q16 = 0;
	// The packed Avatars.def character-sex registry the portable sound-profile
	// selector reads; retained across reset_world.
	struct CharacterSexRow {
		uint16_t character_id = 0;
		bool female = false;
	};
	std::vector<CharacterSexRow> character_sex_rows;
	// Retained score.ini parse; the row is re-resolved whenever the mission's
	// attrib flags change (either load order is legal).
	opennova::score::File score_config;
	bool score_config_loaded = false;
	// The installed script program. Held as a shared_ptr so it survives
	// reset_world(); each (re)load re-applies it onto the fresh kernel
	// WacSystem when the kernel's own layered load installed none.
	std::shared_ptr<WacProgram> wac_program;
	// The Weather node bound through set_weather_render_owner; released
	// whenever the kernel (and the WeatherState it owns) is replaced or dies.
	ObjectID weather_owner_id;
	// The precipitation drawer's last-camera latch (mission-scoped) and the
	// compiled streak frame, reused across frames (its vertex capacity
	// survives clear()).
	opennova::renderer::PrecipitationDrawState precipitation_draw;
	opennova::renderer::PrecipitationDrawFrame precipitation_frame;
};

} // namespace godot
