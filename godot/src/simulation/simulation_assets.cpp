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

void Simulation::reset_infantry_adm_ids() {
	infantry_adm_resolved_ai_count_ = 0;
	if (!world_ || !world_->ai) return;
	AiSystem &ai = *world_->ai;
	for (int i = 0; i < ai.count(); ++i) {
		if (AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	}
}

int Simulation::set_infantry_anim_map(const Ref<ResourceRoot> &p_resource_root, const String &p_adm_name) {
	// The default clip set (adm_id 0): every infantry entity grounds off this until its own
	// model's .adm is registered (register_infantry_adm + set_infantry_adm_id). Clearing here
	// resets the whole registry on each (re)load.
	infantry_anim_.clear();
	reset_infantry_adm_ids();
	const int default_adm_id = infantry_anim_.register_adm(
			p_resource_root.is_valid() ? &p_resource_root->native_index() : nullptr,
			std::string(p_adm_name.utf8().get_data()));
	const int default_clip_count = default_adm_id == 0 ? infantry_anim_.clip_count(0) : 0;
	// Every stored per-entity id indexes this registry; rebuilding it invalidates
	// all prior assignments. Only repopulate once slot 0 is the successfully loaded
	// default map; otherwise a model-specific ADM could usurp the default slot and
	// turn a failed load into false success.
	if (default_adm_id == 0) resolve_new_infantry_adm_ids();
	// The registry ids just changed meaning: stale row stamps and the
	// per-type cache would index the rebuilt registry with old ids. Re-arm
	// every decoded organic row for a fresh resolve + channel.
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
	apply_root_motion_to_ai();
	return default_clip_count;
}

// Assign only newly attached AI entries. AiSystem::attach is append-only, including when
// an entity handle is reused, so the count is a generation-safe high-water mark. This is
// the spawn-time half of AnimMap_RegisterEntity: late joiner-local/remote players must not
// retain the default E_STAND map or configured emplacements fall back to anim_emplaced.
// [orig: AnimMap_RegisterEntity @0x40bb60; AnimMap_UpdateEntity @0x40b5f0.]
void Simulation::resolve_new_infantry_adm_ids() {
	if (!world_ || !world_->ai || infantry_adm_resource_root_.is_null() ||
			infantry_adm_item_db_.is_null() || infantry_anim_.empty())
		return;
	AiSystem &ai = *world_->ai;
	const int count = ai.count();
	if (infantry_adm_resolved_ai_count_ < 0 ||
			infantry_adm_resolved_ai_count_ > count)
		infantry_adm_resolved_ai_count_ = 0;
	for (int i = infantry_adm_resolved_ai_count_; i < count; ++i) {
		AiEntity *e = ai.at(i);
		if (!e) continue;
		e->inf.adm_id = 0;
		if (!e->inf.active) continue;
		const opennova::world::Entity *ent = world_->registry.get(e->handle);
		if (!ent) continue;
		const int visual_item_id =
				visual_item_id_for_runtime_type(ent->item_id, infantry_adm_item_db_);
		String adm = infantry_adm_item_db_->get_anim_def(visual_item_id);
		if (adm.is_empty()) continue;
		if (!adm.to_lower().ends_with(".adm")) adm += ".adm";
		const int adm_id = infantry_anim_.register_adm(
				&infantry_adm_resource_root_->native_index(),
				std::string(adm.utf8().get_data()));
		if (adm_id >= 0) e->inf.adm_id = adm_id;
	}
	infantry_adm_resolved_ai_count_ = count;
}

// Per-entity .adm resolution: ground each soldier off its OWN model's clip, not the shared
// default set (adm_id 0). Retain the shell inputs because multiplayer players are spawned
// after this mission-load sweep; the step/spawn hooks above the world layer resolve each
// later AiSystem entry exactly once.
void Simulation::resolve_infantry_adm_ids(const Ref<ResourceRoot> &p_resource_root,
		const Ref<ItemDatabase> &p_item_db) {
	if (p_resource_root.is_null() || p_item_db.is_null()) return;
	infantry_adm_resource_root_ = p_resource_root;
	infantry_adm_item_db_ = p_item_db;
	reset_infantry_adm_ids();
	resolve_new_infantry_adm_ids();
}

// The items.def trait sweep: the engine-side fold (simassets::resolve_item_traits,
// ADR 0028) reads the database's retained DefItemsFile rows directly — the trait
// semantics, ID-space offset, and [orig] witnesses live there now. This binding
// contributes the ONE wire-class source — the netsim ItemReplicationCatalog
// (ADR 0026) — as an injected supplier so simassets stays net-free. Idempotent;
// called after load and again after spawning the local player.
void Simulation::resolve_item_traits(const Ref<ItemDatabase> &p_item_db) {
	if (!world_ || p_item_db.is_null()) return;
	item_traits_db_ = p_item_db;
	if (!item_replication_catalog_ ||
			item_replication_catalog_db_.ptr() != p_item_db.ptr() ||
			item_replication_catalog_revision_ != p_item_db->get_revision()) {
		item_replication_catalog_ = build_item_replication_catalog(p_item_db);
		item_replication_catalog_db_ = p_item_db;
		item_replication_catalog_revision_ = p_item_db->get_revision();
	}
	opennova::simassets::resolve_item_traits(
			*world_, p_item_db->native_items(),
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
	if (!world_) return;
	// The joiner's live copy carries every S2C 0x41 clear applied so far; a
	// HostClient keeps the boot copy, and a failed/missing charattr.def leaves
	// the all-zero table -- no class carries an attribute, retail's failed-load
	// state [orig: CharAttr_LoadFromDef @0x412140 memsets 0x7C0 bytes first;
	// AnimMap_IsSlotActive @0x4125e0; see docs/interface/hud-re.md].
	const opennova::np::CharAttrChallengeTable *live =
			runtime_ ? runtime_->charattr_challenge_table() : nullptr;
	const opennova::np::CharAttrChallengeTable &table =
			live != nullptr ? *live : charattr_challenge_table_;
	world_->class_attribute_flags =
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

// The D-AI-5 host weapon seed + per-body sound-profile bind, folded into the
// engine (simassets::resolve_ai_weapons, ADR 0028) over the retained items.def
// rows — the seed semantics and [orig] witnesses live there now. Ammo names
// resolve against the mission ammo table, so call AFTER load_ammo_table.
int Simulation::resolve_ai_weapons(const Ref<ItemDatabase> &p_item_db) {
	if (!world_ || !world_->ai || p_item_db.is_null()) return 0;
	return opennova::simassets::resolve_ai_weapons(
			*world_, p_item_db->native_items());
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
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	collision_world_.set_section_matrix_provider(this);
	if (world_) {
		world_->collision = &collision_world_;
		world_->mounted_pose_provider = this;
		world_->muzzle_pose_provider = &collision_pose_native_;
	}
	if (ai_) ai_->collision = &collision_world_;
}

// The mounted-pose resolver (S4, ADR 0028) is native-only: the engine-side
// resolver over the sim's own parse (simassets::resolve_model_mounted_pose) is
// the sole host-authority path. Its model source resolves AT QUERY TIME —
// the spec's graphic through the sim cache (production; survives asset-root
// switches because the cache re-parses under the live index), else the
// installed spec's Ref-kept model_data parse (boot order / test worlds).
bool Simulation::resolve_mounted_pose(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	return resolve_mounted_pose_native(p_world, p_carrier, p_seat, r_out);
}

bool Simulation::resolve_mounted_pose_native(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (!world_ || &p_world != world_.get() ||
			p_seat.type != opennova::world::SeatType::Gunner ||
			p_seat.bone_index == 0)
		return false;
	++mounted_native_queries_;
	const Threedi3di3 *model_ptr = nullptr;
	const auto found_graphic =
			mounted_pose_native_graphics_.find(p_carrier.item_id);
	if (found_graphic != mounted_pose_native_graphics_.end() &&
			sim_models_.has_index())
		model_ptr = sim_models_.model_for(found_graphic->second);
	if (model_ptr == nullptr) {
		++mounted_native_declines_;
		return false;
	}
	const Threedi3di3 &model = *model_ptr;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr) {
		++mounted_native_declines_;
		return false;
	}
	// Gather the three CTRL sources; the ordinal writes onto the retail bus
	// and the PANM clock are simassets' (compose_mounted_pose_controls /
	// mounted_pose_time_ms).
	opennova::simassets::MountedPoseControlSources sources;
	AiEntity *carrier_ai = ai_ ? ai_->for_handle(p_carrier.handle) : nullptr;
	if (carrier_ai != nullptr) {
		sources.part_anim_phase0 =
				carrier_ai->brain.f[AiBrain::kPartAnimPhase0];
		sources.part_anim_phase1 =
				carrier_ai->brain.f[AiBrain::kPartAnimPhase0 + 1];
	}
	sources.has_heat_glow = opennova::world::world_model_heat_glow_for(
			p_world, p_carrier, sources.heat_glow);
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(p_world, ai_.get(), p_carrier, emplaced)) {
		sources.has_emplaced = true;
		sources.emplaced_gun_yaw = emplaced.gun_yaw;
		sources.emplaced_gun_pitch = emplaced.gun_pitch;
	}
	int32_t ctrl_bus[THREEDI_CTRL_REGISTER_COUNT] = {};
	opennova::simassets::compose_mounted_pose_controls(
			p_carrier.item_attrib, sources, ctrl_bus);
	std::array<int32_t, THREEDI_CTRL_REGISTER_COUNT> ctrl_values{};
	std::copy(std::begin(ctrl_bus), std::end(ctrl_bus), ctrl_values.begin());
	const uint32_t time_ms = opennova::simassets::mounted_pose_time_ms(
			p_world.logic_tick, panm_time_override_ms_);
	if (mounted_pose_cache_logic_tick_ != p_world.logic_tick) {
		mounted_pose_live_cache_.clear();
		mounted_pose_cache_logic_tick_ = p_world.logic_tick;
	}
	auto rest_found = mounted_pose_rest_cache_.find(model_ptr);
	if (rest_found == mounted_pose_rest_cache_.end()) {
		MountedPoseRestCache rest;
		if (!opennova::simassets::evaluate_model_mounted_pose_parts(
				model, 0u, nullptr, rest.parts)) {
			++mounted_native_declines_;
			return false;
		}
		rest_found = mounted_pose_rest_cache_.emplace(
				model_ptr, std::move(rest)).first;
	}
	std::vector<MountedPoseLiveCache> &model_live =
			mounted_pose_live_cache_[model_ptr];
	auto live_found = std::find_if(model_live.begin(), model_live.end(),
			[&](const MountedPoseLiveCache &candidate) {
				return candidate.time_ms == time_ms &&
						candidate.controls == ctrl_values;
			});
	if (live_found == model_live.end()) {
		MountedPoseLiveCache live;
		live.time_ms = time_ms;
		live.controls = ctrl_values;
		live.valid = opennova::simassets::evaluate_model_mounted_pose_parts(
				model, time_ms, ctrl_values.data(), live.parts);
		model_live.push_back(std::move(live));
		live_found = model_live.end() - 1;
		++mounted_native_evaluations_;
	} else {
		++mounted_native_cache_hits_;
	}
	const bool resolved = live_found->valid &&
			opennova::simassets::resolve_model_mounted_pose_from_parts(
					model, p_carrier, p_seat, rest_found->second.parts,
					live_found->parts, r_out);
	if (!resolved) ++mounted_native_declines_;
	return resolved;
}

bool Simulation::ensure_collision_instance(
		opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity) {
	if (!world_ || &p_world != world_.get() || collision_item_db_.is_null())
		return false;
	const opennova::world::Entity *entity = p_world.registry.get(p_entity);
	if (entity == nullptr) {
		collision_world_.remove_entity_instance(p_entity);
		collision_pose_native_.remove_entity(p_entity);
		collision_resolve_.resolution_attempted.erase(p_entity.packed);
		return false;
	}
	const auto attempted =
			collision_resolve_.resolution_attempted.find(p_entity.packed);
	if (attempted != collision_resolve_.resolution_attempted.end()) {
		if (attempted->second == entity->registry_spawn_id)
			return collision_world_.has_instance(p_world, p_entity);
		collision_world_.remove_entity_instance(p_entity);
		collision_pose_native_.remove_entity(p_entity);
		collision_resolve_.resolution_attempted.erase(attempted);
	}

	// Re-run the idempotent attach sweep against the retained mission caches.
	// It resolves every entity that appeared since the previous sweep, including
	// a player deployed after load, without registering another graphic model.
	resolve_collision_instances(collision_item_db_);
	return collision_world_.has_instance(p_world, p_entity);
}

// The collision section-matrix provider (S3, ADR 0028) is native-AUTHORITATIVE.
// The engine-side provider poses from the sim's own parsed models and clip sets;
// production always installs the sim's asset root, so every collision entity is
// registered and served here. The legacy render-bound builder is reached ONLY
// when the native provider has no source for this entity — the GUT stub worlds
// that resolve collision through a duck-typed placer with no asset root (the
// native provider loads its skeletal rigs and retains PANM parses through a
// resource index those worlds never install). The two paths were proven
// byte-identical by the live A/B soak this cutover retires.
bool Simulation::build_section_matrices(opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity, int32_t p_model_id,
		const opennova::world::CollisionMatrix &p_entity_world,
		const opennova::world::CollisionModel &p_model,
		std::vector<opennova::world::CollisionMatrix> &r_out) {
	// The provider consumes the same per-query sim state this binding reads.
	collision_pose_native_.weapon_active = local_weapon_.active;
	collision_pose_native_.panm_time_override_ms = panm_time_override_ms_;
	++collision_native_queries_;
	if (collision_pose_native_.build_section_matrices(p_world, p_entity,
			p_model_id, p_entity_world, p_model, r_out))
		return true;
	// Native-only (S3b full). False WITHOUT a registered source is the
	// normal rigid path — most placed items carry no live PANM and no rig,
	// and CollisionWorld poses them as identity sections. False WITH a
	// source is a real decline (the masked-failure signal the soak/probes
	// gate on); the legacy render-bound builder that once silently absorbed
	// these is gone.
	if (collision_pose_native_.has_skeletal_entity(p_entity) ||
			collision_pose_native_.has_generic_model(p_model_id))
		++collision_native_declines_;
	return false;
}

Dictionary Simulation::debug_native_pose_stats() const {
	Dictionary out;
	out["collision_queries"] = static_cast<int64_t>(collision_native_queries_);
	out["collision_declines"] = static_cast<int64_t>(collision_native_declines_);
	out["muzzle_queries"] = static_cast<int64_t>(
			collision_pose_native_.muzzle_query_count());
	out["muzzle_resolves"] = static_cast<int64_t>(
			collision_pose_native_.muzzle_resolve_count());
	out["mounted_queries"] = static_cast<int64_t>(mounted_native_queries_);
	out["mounted_declines"] = static_cast<int64_t>(mounted_native_declines_);
	out["mounted_evaluations"] = static_cast<int64_t>(mounted_native_evaluations_);
	out["mounted_cache_hits"] = static_cast<int64_t>(mounted_native_cache_hits_);
	out["mounted_rest_cache_entries"] =
			static_cast<int64_t>(mounted_pose_rest_cache_.size());
	out["mounted_graphic_sources"] =
			static_cast<int64_t>(mounted_pose_native_graphics_.size());
	return out;
}


void Simulation::set_asset_root(const Ref<ResourceRoot> &p_root) {
	asset_root_ = p_root;
	mounted_pose_rest_cache_.clear();
	mounted_pose_live_cache_.clear();
	mounted_pose_cache_logic_tick_ = 0xFFFFFFFFu;
	sim_models_.set_index(
			p_root.is_valid() ? &p_root->native_index() : nullptr);
	collision_pose_native_.set_resource_index(
			p_root.is_valid() ? &p_root->native_index() : nullptr);
}

int Simulation::resolve_collision_instances(
		const Ref<ItemDatabase> &p_item_db) {
	if (!world_ || p_item_db.is_null()) return 0;
	// Production installs the sim's own asset source first (ADR 0028).
	if (!sim_models_.has_index()) {
		godot::UtilityFunctions::print_verbose(
				"Simulation: no asset root installed — collision/occlusion "
				"extraction has no model source (install set_asset_root first)");
	}
	collision_item_db_ = p_item_db;
	apply_collision_to_ai();
	// The sweep itself is engine code (simassets::resolve_collision_instances,
	// ADR 0031 re-opening the S7b asset-resolution leg): this binding supplies
	// the retained items.def rows and the engine systems, nothing else.
	const opennova::simassets::CollisionResolveDeps deps{
			collision_world_, occlusion_world_, collision_pose_native_,
			sim_models_};
	return opennova::simassets::resolve_collision_instances(
			*world_, p_item_db->native_items(), collision_resolve_, deps);
}

void Simulation::stamp_seat_spec_turret_limits() {
	if (!world_) return;
	opennova::simassets::stamp_seat_spec_turret_limits(*world_, item_seat_specs_);
}

void Simulation::refresh_item_seat_spec(
		opennova::world::Entity &p_entity) {
	if (!world_) return;
	opennova::simassets::refresh_item_seat_spec(*world_, item_seat_specs_,
			p_entity, joiner_bridge_.wire_header_world());
}

// (set_ai_profile_speeds retired with S9b: the .aip resolve is native in
// mission::resolve_ai_profiles, driven by boot_mission.)

void Simulation::finalize_installed_seat_specs() {
	// Lookup table, ordered for the binary search in item_seat_spec_for_type —
	// a joiner probes it once per present row per frame. (The native
	// extraction emits sorted specs already; kept for the wire-type installs.)
	std::sort(item_seat_specs_.begin(), item_seat_specs_.end(),
			[](const opennova::mission::ItemSeatSpec &a,
					const opennova::mission::ItemSeatSpec &b) {
				return a.type_id < b.type_id;
			});
	stamp_seat_spec_turret_limits();

	// The production header-only join resolves model metadata after network rows
	// can already exist. Refresh live pool-1 rows immediately and preserve any
	// occupant by retail's fixed mountHandles slot, never by dense vector index.
	if (world_) {
		std::vector<opennova::world::EntityHandle> items;
		world_->registry.for_each([&](const opennova::world::Entity &entity) {
			if (entity.handle.pool() == 1) items.push_back(entity.handle);
		});
		for (const opennova::world::EntityHandle handle : items) {
			opennova::world::Entity *entity = joiner_bridge_.wire_header_world()
					? joiner_bridge_.materializer().owned(*world_, handle)
					: world_->registry.get(handle);
			if (entity != nullptr)
				refresh_item_seat_spec(*entity);
		}
		// A header-only join may receive its model/seat table after the 0x0D
		// row. Definitions were installed above; now apply the retained fixed
		// mountHandles image without creating synthetic seats.
		if (joiner_bridge_.wire_header_world() && runtime_ != nullptr)
			(void)joiner_bridge_.materializer().sync(runtime_->state(), *world_);
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
	item_seat_specs_.clear();
	mounted_pose_native_graphics_.clear();
	if (p_item_db.is_valid() && !p_seed_item_ids.empty()) {
		opennova::simassets::SeatSpecExtraction native;
		opennova::simassets::extract_item_seat_specs(
				p_item_db->native_items(),
				[this](const std::string &graphic) {
					return sim_models_.model_for(graphic);
				},
				p_seed_item_ids, native);
		item_seat_specs_ = std::move(native.specs);
		mounted_pose_native_graphics_ = std::move(native.graphic_by_type);
	}
	finalize_installed_seat_specs();
}

bool Simulation::install_seat_specs_for_type_ids(
		const Ref<ItemDatabase> &p_item_db,
		const PackedInt32Array &p_type_ids) {
	if (p_item_db.is_null() || !sim_models_.has_index()) return false;
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
