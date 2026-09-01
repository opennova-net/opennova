// Simulation — shell-side asset resolution: infantry anim maps (.adm),
// item traits/weapons from the item database, collision instances + section
// matrices from the .3di collision IR, and the mission item seat specs.
#include "simulation/simulation_internal.h"
#include "network/item_replication_catalog_adapter.h"

#include <runtime/simassets/item_traits.h>
#include <runtime/simassets/mounted_pose.h>      // the native mounted-pose resolver (S4, ADR 0028)
#include <runtime/simassets/seat_spec_extract.h> // the native seat-spec extraction (S4, ADR 0028)
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>

using namespace sim_internal;

int Simulation::set_infantry_anim_map(const Ref<ResourceRoot> &p_resource_root, const String &p_adm_name) {
	// The default clip set (adm_id 0) and the per-entity re-resolve are the
	// kernel's ONE install (mission_kernel.cpp install_infantry_anim); this
	// binding hands the mounted index over and re-arms the joiner's decoded
	// rows, whose stamps index the rebuilt registry with old ids otherwise.
	const int default_clip_count = kernel_->install_infantry_anim(
			std::string(p_adm_name.utf8().get_data()),
			p_resource_root.is_valid() ? &p_resource_root->native_index() : nullptr);
	client_row_adm_by_type_.clear();
	if (runtime_ != nullptr && joiner_) {
		for (opennova::netsim::ClientEntityState &es :
				runtime_->state().entities) {
			if (es.rm_adm_id != -2) {
				es.rm_adm_id = -2;
				es.rm_state = -1;
				es.rm_leg_seeded = false;
			}
		}
	}
	return default_clip_count;
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip,
// not the shared default set — the kernel's sweep (D-INF-6), (re)armed here
// with the shell's sources. The Refs are retained because multiplayer players
// are spawned after this mission-load sweep and the joiner's decoded-row
// resolve reads the same inputs.
void Simulation::resolve_infantry_adm_ids(const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db) {
	if (p_resource_root.is_null() || p_item_db.is_null()) return;
	infantry_adm_resource_root_ = p_resource_root;
	infantry_adm_item_db_ = p_item_db;
	kernel_->set_items_table(&p_item_db->native_items());
	kernel_->rearm_infantry_adm(&p_resource_root->native_index());
}

// The items.def trait sweep: the engine-side fold (simassets::resolve_item_traits,
// ADR 0028) reads the database's retained DefItemsFile rows directly — the trait
// semantics, ID-space offset, and [orig] witnesses live there now. This binding
// contributes the ONE wire-class source — the netsim ItemReplicationCatalog
// (ADR 0026) — as an injected supplier so simassets stays net-free. Idempotent;
// called after load and again after spawning the local player.
void Simulation::resolve_item_traits(const Ref<ItemDatabase> &p_item_db) {
	if (p_item_db.is_null()) return;
	item_traits_db_ = p_item_db;
	// The kernel's item legs (the collision demand sweep, the adm resolve)
	// read the same rows; the Ref above pins their lifetime.
	kernel_->set_items_table(&p_item_db->native_items());
	if (!item_replication_catalog_ ||
			item_replication_catalog_db_.ptr() != p_item_db.ptr() ||
			item_replication_catalog_revision_ != p_item_db->get_revision()) {
		item_replication_catalog_ = build_item_replication_catalog(p_item_db);
		item_replication_catalog_db_ = p_item_db;
		item_replication_catalog_revision_ = p_item_db->get_revision();
	}
	opennova::simassets::resolve_item_traits(
			kernel_->world, p_item_db->native_items(),
			[catalog = item_replication_catalog_](int def_id) {
				// The same immutable profile supplies the host stamp and the
				// client decode width. Missing/ambiguous definitions fail
				// closed as Unknown.
				const opennova::netsim::ItemReplicationProfile *replication =
						catalog->by_definition_id(def_id);
				return static_cast<uint8_t>(replication != nullptr
						? replication->wire_entity_class()
						: opennova::EntityClass::Unknown);
			});

	install_item_class_resolver();
}

void Simulation::install_item_class_resolver() {
	if (!runtime_ || !item_replication_catalog_) return;
	runtime_->view().set_item_class_resolver(
			[catalog = item_replication_catalog_](uint16_t type_id) {
				return catalog->resolve_wire_entity_class(type_id);
			});
}

void Simulation::install_charattr_challenge_table() {
	if (runtime_) {
		if (charattr_challenge_loaded_) {
			runtime_->set_charattr_challenge_table(charattr_challenge_table_);
		} else {
			runtime_->clear_charattr_challenge_table();
		}
	}
	sync_class_attribute_flags();
}

void Simulation::sync_class_attribute_flags() {
	if (!kernel_) return;
	// The joiner's live copy carries every S2C 0x41 clear applied so far; a
	// HostClient keeps the boot copy, and a failed/missing charattr.def leaves
	// the all-zero table -- no class carries an attribute, retail's failed-load
	// state [orig: CharAttr_LoadFromDef @0x412140 memsets 0x7C0 bytes first;
	// AnimMap_IsSlotActive @0x4125e0; see docs/interface/hud-re.md].
	const opennova::np::CharAttrChallengeTable *live =
			runtime_ ? runtime_->charattr_challenge_table() : nullptr;
	const opennova::np::CharAttrChallengeTable &table =
			live != nullptr ? *live : charattr_challenge_table_;
	kernel_->world.class_attribute_flags =
			opennova::np::charattr_class_attribute_rows(table);
}

void Simulation::install_character_join_vars() {
	if (!runtime_ || !join_character_vars_set_) return;
	runtime_->set_character_join_vars(join_character_vars_);
}

void Simulation::install_join_integrity_profile() {
	if (!runtime_) return;
	if (join_integrity_profile_id_.empty()) {
		runtime_->clear_integrity_challenge_profile();
		return;
	}
	runtime_->set_integrity_challenge_profile(join_integrity_profile_id_);
}

void Simulation::install_expansion_version_root() {
	if (!runtime_) return;
	runtime_->set_expansion_version_root(join_expansion_version_root_);
}

void Simulation::install_app_id() {
	if (!runtime_) return;
	runtime_->set_app_id(app_id_);
}

void Simulation::install_join_cd_cookie() {
	if (!runtime_) return;
	runtime_->set_join_cd_cookie(join_cd_cookie_);
}

// The D-AI-5 host weapon seed + per-body sound-profile bind, folded into the
// engine (simassets::resolve_ai_weapons, ADR 0028) over the retained items.def
// rows — the seed semantics and [orig] witnesses live there now. Ammo names
// resolve against the mission ammo table, so call AFTER load_ammo_table.
int Simulation::resolve_ai_weapons(const Ref<ItemDatabase> &p_item_db) {
	if (!kernel_->world.ai || p_item_db.is_null()) return 0;
	return opennova::simassets::resolve_ai_weapons(
			kernel_->world, p_item_db->native_items());
}

int Simulation::set_character_avatar_database(
		const Ref<AvatarDatabase> &p_avatar_db) {
	character_sex_rows_.clear();
	if (p_avatar_db.is_valid()) {
		for (const AvatarDatabase::CharacterSexRow &row :
				p_avatar_db->character_sex_rows())
			character_sex_rows_.push_back(
					CharacterSexRow{row.character_id, row.female});
	}
	apply_character_traits_to_world();
	return static_cast<int>(character_sex_rows_.size());
}

void Simulation::apply_collision_to_ai() {
	// The kernel is the ONE registered section-matrix/mounted-pose provider;
	// wire_collision points the world/AI systems at its collision world.
	kernel_->wire_collision();
}

Dictionary Simulation::debug_native_pose_stats() const {
	Dictionary out;
	out["collision_queries"] = static_cast<int64_t>(kernel_->collision_queries);
	out["collision_declines"] = static_cast<int64_t>(kernel_->collision_declines);
	out["muzzle_queries"] = static_cast<int64_t>(
			kernel_->collision_pose.muzzle_query_count());
	out["muzzle_resolves"] = static_cast<int64_t>(
			kernel_->collision_pose.muzzle_resolve_count());
	out["mounted_queries"] = static_cast<int64_t>(kernel_->mounted_queries);
	out["mounted_declines"] = static_cast<int64_t>(kernel_->mounted_declines);
	out["mounted_evaluations"] = static_cast<int64_t>(kernel_->mounted_evaluations);
	out["mounted_cache_hits"] = static_cast<int64_t>(kernel_->mounted_cache_hits);
	out["mounted_rest_cache_entries"] =
			static_cast<int64_t>(kernel_->mounted_rest_cache_size());
	out["mounted_graphic_sources"] =
			static_cast<int64_t>(kernel_->mounted_graphics.size());
	return out;
}


void Simulation::set_asset_root(const Ref<ResourceRoot> &p_root) {
	asset_root_ = p_root;
	kernel_->set_asset_index(
			p_root.is_valid() ? &p_root->native_index() : nullptr);
}

int Simulation::resolve_collision_instances(
		const Ref<ItemDatabase> &p_item_db) {
	if (p_item_db.is_null()) return 0;
	collision_item_db_ = p_item_db;
	// The kernel's demand sweep (ensure_collision_instance) reads the same
	// rows; the Ref above pins their lifetime.
	kernel_->set_items_table(&p_item_db->native_items());
	// Production installs the sim's own asset source first (ADR 0028).
	if (!kernel_->models.has_index()) {
		godot::UtilityFunctions::print_verbose(
				"Simulation: no asset root installed — collision/occlusion "
				"extraction has no model source (install set_asset_root first)");
	}
	apply_collision_to_ai();
	// The sweep itself is engine code (simassets::resolve_collision_instances,
	// ADR 0031 re-opening the S7b asset-resolution leg): this binding supplies
	// the retained items.def rows and the engine systems, nothing else.
	const opennova::simassets::CollisionResolveDeps deps{
			kernel_->collision, kernel_->occlusion, kernel_->collision_pose,
			kernel_->models};
	return opennova::simassets::resolve_collision_instances(
			kernel_->world, p_item_db->native_items(), kernel_->collision_state, deps);
}

void Simulation::stamp_seat_spec_turret_limits() {
	if (!kernel_) return;
	opennova::simassets::stamp_seat_spec_turret_limits(kernel_->world, kernel_->seat_specs);
}

void Simulation::refresh_item_seat_spec(
		opennova::world::Entity &p_entity) {
	if (!kernel_) return;
	opennova::simassets::refresh_item_seat_spec(kernel_->world, kernel_->seat_specs,
			p_entity, joiner_bridge_.wire_header_world());
}

// (set_ai_profile_speeds retired with S9b: the .aip resolve is native in
// mission::resolve_ai_profiles, driven by boot_mission.)

void Simulation::finalize_installed_seat_specs() {
	// Lookup table, ordered for the binary search in item_seat_spec_for_type —
	// a joiner probes it once per present row per frame. (The native
	// extraction emits sorted specs already; kept for the wire-type installs.)
	std::sort(kernel_->seat_specs.begin(), kernel_->seat_specs.end(),
			[](const opennova::mission::ItemSeatSpec &a,
					const opennova::mission::ItemSeatSpec &b) {
				return a.type_id < b.type_id;
			});
	stamp_seat_spec_turret_limits();

	// The production header-only join resolves model metadata after network rows
	// can already exist. Refresh live pool-1 rows immediately and preserve any
	// occupant by retail's fixed mountHandles slot, never by dense vector index.
	if (kernel_) {
		std::vector<opennova::world::EntityHandle> items;
		kernel_->world.registry.for_each([&](const opennova::world::Entity &entity) {
			if (entity.handle.pool() == 1) items.push_back(entity.handle);
		});
		for (const opennova::world::EntityHandle handle : items) {
			opennova::world::Entity *entity = joiner_bridge_.wire_header_world()
					? joiner_bridge_.materializer().owned(kernel_->world, handle)
					: kernel_->world.registry.get(handle);
			if (entity != nullptr)
				refresh_item_seat_spec(*entity);
		}
		// A header-only join may receive its model/seat table after the 0x0D
		// row. Definitions were installed above; now apply the retained fixed
		// mountHandles image without creating synthetic seats.
		if (joiner_bridge_.wire_header_world() && runtime_ != nullptr)
			(void)joiner_bridge_.materializer().sync(runtime_->state(), kernel_->world);
	}
}

// S16 (ADR 0028): the production seat/mount install IS the native extraction —
// simassets::extract_item_seat_specs over the retained items.def rows and the
// sim's own parse-once models. The shell GDScript extractor and its Dictionary
// install seam are gone; before this cutover the two extractions were diffed
// live on retail 00TRg (29/29 specs identical, 0 mismatches, 0 native-missing,
// 2026-08-07). Model userpoints resolve through the sim cache at install, so
// boot wires the asset root before the steps run.
void Simulation::install_native_seat_specs(
		const Ref<ItemDatabase> &p_item_db,
		const std::vector<int> &p_seed_item_ids) {
	kernel_->seat_specs.clear();
	kernel_->mounted_graphics.clear();
	if (p_item_db.is_valid() && !p_seed_item_ids.empty()) {
		opennova::simassets::SeatSpecExtraction native;
		opennova::simassets::extract_item_seat_specs(
				p_item_db->native_items(),
				[this](const std::string &graphic) {
					return kernel_->models.model_for(graphic);
				},
				p_seed_item_ids, native);
		kernel_->seat_specs = std::move(native.specs);
		kernel_->mounted_graphics = std::move(native.graphic_by_type);
	}
	finalize_installed_seat_specs();
}

bool Simulation::install_seat_specs_for_type_ids(
		const Ref<ItemDatabase> &p_item_db,
		const PackedInt32Array &p_type_ids) {
	if (p_item_db.is_null() || !kernel_->models.has_index()) return false;
	std::vector<int> seeds;
	seeds.reserve(static_cast<size_t>(p_type_ids.size()));
	for (int64_t i = 0; i < p_type_ids.size(); ++i) {
		const int32_t type_id = p_type_ids[i];
		if (type_id > 0)
			seeds.push_back(static_cast<int>(type_id) +
					static_cast<int>(opennova::mission::kItemIdOffset));
	}
	install_native_seat_specs(p_item_db, seeds);
	return true;
}
