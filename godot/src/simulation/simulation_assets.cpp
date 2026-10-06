// Simulation — shell-side asset resolution: infantry anim maps (.adm),
// item traits/weapons from the item database, collision instances + section
// matrices from the .3di collision IR, and the mission item seat specs.
#include "simulation/simulation_internal.h"
#include "util/string_convert.h"

#include <runtime/mission/item_traits.h>
#include <runtime/world/mounted_pose.h>      // the native mounted-pose resolver (S4, ADR 0028)
#include <runtime/mission/seat_spec_extract.h> // the native seat-spec extraction (S4, ADR 0028)
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <memory>

using namespace sim_internal;

int Simulation::set_infantry_anim_map(const Ref<ResourceRoot> &p_resource_root, const String &p_adm_name) {
	assets_.infantry_adm_resource_root = p_resource_root;
	// The kernel owns default/model map resolution. The joiner observes its
	// animation revision and re-arms decoded rows when the registry changes.
	return kernel_->install_infantry_anim(
			opennova::to_std(p_adm_name),
			p_resource_root.is_valid() ? &p_resource_root->native_assets() : nullptr);
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip,
// not the shared default set — the kernel's sweep (D-INF-6), (re)armed here
// with the shell's sources. The Refs are retained because multiplayer players
// are spawned after this mission-load sweep and the joiner's decoded-row
// resolve reads the same inputs.
void Simulation::resolve_infantry_adm_ids(const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db) {
	if (p_resource_root.is_null() || p_item_db.is_null()) return;
	assets_.infantry_adm_resource_root = p_resource_root;
	assets_.infantry_adm_item_db = p_item_db;
	kernel_->set_items_table(&p_item_db->native_items());
	kernel_->rearm_infantry_adm(&p_resource_root->native_assets());
}

// The items.def trait sweep: the engine-side fold (mission::resolve_item_traits,
// ADR 0028) reads the database's retained DefItemsFile rows directly — the trait
// semantics, ID-space offset, and [orig] witnesses live there now. This binding
// contributes the ONE wire-class source — the replication ItemReplicationCatalog
// (ADR 0026) — as an injected supplier so mission code stays net-free. Idempotent;
// called after load and again after spawning the local player.
void Simulation::resolve_item_traits(const Ref<ItemDatabase> &p_item_db) {
	if (p_item_db.is_null()) return;
	assets_.item_traits_db = p_item_db;
	if (!world_installed_) pending_item_traits_db_ = p_item_db;
	// The kernel's item legs (the collision demand sweep, the adm resolve)
	// read the same rows; the Ref above pins their lifetime.
	kernel_->set_items_table(&p_item_db->native_items());
	ensure_item_replication_catalog(p_item_db);
	// The kernel runs the sweep over the table installed above and re-runs it
	// inside every baseline restore (the baseline predates these traits).
	kernel_->resolve_item_traits(
			[catalog = assets_.item_replication_catalog](int def_id) {
				// The same immutable profile supplies the host stamp and the
				// client decode width. A missing or unresolved definition
				// fails closed as Unknown.
				const opennova::replication::ItemReplicationProfile *replication =
						catalog->by_definition_id(def_id);
				return static_cast<uint8_t>(replication != nullptr
						? replication->wire_entity_class()
						: opennova::EntityClass::Unknown);
			});

	install_item_catalog();
}

// The replication catalog over the database's retained parse, rebuilt when
// the database or its revision changed.
void Simulation::ensure_item_replication_catalog(const Ref<ItemDatabase> &p_item_db) {
	if (p_item_db.is_null()) return;
	if (assets_.item_replication_catalog &&
			assets_.item_replication_catalog_db.ptr() == p_item_db.ptr() &&
			assets_.item_replication_catalog_revision == p_item_db->get_revision())
		return;
	// Built straight off the retained parse: the per-row walk sees every row,
	// so the catalog resolves a duplicated id to its first row and lists the
	// repeats.
	assets_.item_replication_catalog =
			std::make_shared<const opennova::replication::ItemReplicationCatalog>(
					opennova::replication::ItemReplicationCatalog::from_items_def(
							p_item_db->native_items()));
	assets_.item_replication_catalog_db = p_item_db;
	assets_.item_replication_catalog_revision = p_item_db->get_revision();
}

void Simulation::install_item_catalog() {
	if (!assets_.item_replication_catalog) return;
	// A host role is constructed with the catalog of its time and retains it
	// for every HostClient view it rebuilds (the baseline restore); the catalog
	// built after a role installed lands here. The live runtime takes it now.
	if (host_role_ != nullptr) host_role_->set_item_catalog(assets_.item_replication_catalog);
	if (runtime_) runtime_->view().set_item_catalog(assets_.item_replication_catalog);
}

void Simulation::install_charattr_challenge_table() {
	if (runtime_) {
		if (net_.charattr_challenge_loaded) {
			runtime_->set_charattr_challenge_table(net_.charattr_challenge_table);
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
	const opennova::inmatch::CharAttrChallengeTable *live =
			runtime_ ? runtime_->charattr_challenge_table() : nullptr;
	const opennova::inmatch::CharAttrChallengeTable &table =
			live != nullptr ? *live : net_.charattr_challenge_table;
	kernel_->world.tables.class_attribute_flags =
			opennova::inmatch::charattr_class_attribute_rows(table);
}

void Simulation::install_character_join_vars() {
	if (!runtime_ || !net_.join_character_vars_set) return;
	runtime_->set_character_join_vars(net_.join_character_vars);
}

void Simulation::install_join_integrity_profile() {
	if (!runtime_) return;
	if (net_.join_integrity_profile_id.empty()) {
		runtime_->clear_integrity_challenge_profile();
		return;
	}
	runtime_->set_integrity_challenge_profile(net_.join_integrity_profile_id);
}

void Simulation::install_expansion_version_root() {
	if (!runtime_) return;
	runtime_->set_expansion_version_root(net_.join_expansion_version_root);
}

void Simulation::install_app_id() {
	if (!runtime_) return;
	runtime_->set_app_id(net_.app_id);
}

void Simulation::install_join_cd_cookie() {
	if (!runtime_) return;
	runtime_->set_join_cd_cookie(net_.join_cd_cookie);
}

int Simulation::set_character_avatar_database(
		const Ref<AvatarDatabase> &p_avatar_db) {
	assets_.character_sex_rows.clear();
	if (p_avatar_db.is_valid()) {
		for (const AvatarDatabase::CharacterSexRow &row :
				p_avatar_db->character_sex_rows())
			assets_.character_sex_rows.push_back(
					CharacterSexRow{row.character_id, row.female});
	}
	apply_character_traits_to_world();
	return static_cast<int>(assets_.character_sex_rows.size());
}

int Simulation::get_mounted_graphic_source_count() const {
	return kernel_ != nullptr ? static_cast<int>(kernel_->mounted_graphics.size()) : 0;
}


void Simulation::set_asset_root(const Ref<ResourceRoot> &p_root) {
	assets_.root = p_root;
	kernel_->set_assets(
			p_root.is_valid() ? &p_root->native_assets() : nullptr);
}

int Simulation::resolve_collision_instances(
		const Ref<ItemDatabase> &p_item_db) {
	if (p_item_db.is_null()) return 0;
	assets_.collision_item_db = p_item_db;
	// The kernel's demand sweep (ensure_collision_instance) reads the same
	// rows; the Ref above pins their lifetime.
	kernel_->set_items_table(&p_item_db->native_items());
	// Production installs the shared native asset source first (ADR 0044).
	if (!kernel_->assets().has_source()) {
		godot::UtilityFunctions::print_verbose(
				"Simulation: no asset root installed — collision/occlusion "
				"extraction has no model source (install set_asset_root first)");
	}
	// The sweep itself is the kernel's (mission::resolve_collision_instances
	// over its retained items.def rows and its own systems, ADR 0031 re-opening
	// the S7b asset-resolution leg); this binding supplies the rows, nothing else.
	return kernel_->resolve_collision_instances();
}

void Simulation::stamp_seat_spec_turret_limits() {
	if (!kernel_) return;
	opennova::mission::stamp_seat_spec_turret_limits(kernel_->world, kernel_->seat_specs);
}

void Simulation::refresh_item_seat_spec(
		opennova::world::Entity &p_entity) {
	if (!kernel_) return;
	opennova::mission::refresh_item_seat_spec(kernel_->world, kernel_->seat_specs,
			p_entity, kernel_->wire_header_world);
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
	// can already exist. Refresh the live item-pool rows (vehicles and buildings,
	// mission::pool_takes_item_seat_spec) immediately and preserve any occupant
	// by retail's fixed mountHandles slot, never by dense vector index.
	if (kernel_) {
		std::vector<opennova::world::EntityHandle> items;
		kernel_->world.registry.for_each([&](const opennova::world::Entity &entity) {
			if (opennova::mission::pool_takes_item_seat_spec(entity.handle.pool()))
				items.push_back(entity.handle);
		});
		for (const opennova::world::EntityHandle handle : items) {
			opennova::world::Entity *entity = kernel_->wire_header_world && joiner_role_ != nullptr
					? joiner_role_->materializer().owned(kernel_->world, handle)
					: kernel_->world.registry.get(handle);
			if (entity != nullptr)
				refresh_item_seat_spec(*entity);
		}
		// A header-only join may receive its model/seat table after the 0x0D
		// row. Definitions were installed above; now apply the retained fixed
		// mountHandles image without creating synthetic seats.
		if (kernel_->wire_header_world && joiner_role_ != nullptr && runtime_ != nullptr)
			(void)joiner_role_->materializer().sync(runtime_->state(), kernel_->world);
	}
}

// S16 (ADR 0028): the production seat/mount install IS the native extraction —
// mission::extract_item_seat_specs over the retained items.def rows and the
// shared native models. The shell GDScript extractor and its Dictionary
// install seam are gone; before this cutover the two extractions were diffed
// live on retail 00TRg (29/29 specs identical, 0 mismatches, 0 native-missing,
// 2026-08-07). Model userpoints resolve through the shared asset store at install, so
// boot wires the asset root before the steps run.
void Simulation::install_native_seat_specs(
		const Ref<ItemDatabase> &p_item_db,
		const std::vector<int> &p_seed_item_ids) {
	kernel_->seat_specs.clear();
	kernel_->mounted_graphics.clear();
	if (p_item_db.is_valid() && !p_seed_item_ids.empty()) {
		opennova::mission::SeatSpecExtraction native;
		opennova::mission::extract_item_seat_specs(
				p_item_db->native_items(),
				[this](const std::string &graphic) {
					return kernel_->assets().model(graphic).get();
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
	if (p_item_db.is_null() || !kernel_->assets().has_source()) return false;
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
