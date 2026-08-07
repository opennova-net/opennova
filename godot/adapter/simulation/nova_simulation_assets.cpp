// NovaSimulation — shell-side asset resolution: infantry anim maps (.adm),
// item traits/weapons from the item database, collision instances + section
// matrices from the .3di collision IR, and the mission item seat specs.
#include "simulation/nova_simulation_internal.h"
#include "network/item_replication_catalog_adapter.h"

#include <simassets/item_traits.h>
#include <simassets/mounted_pose.h>      // the native mounted-pose resolver (S4, ADR 0028)
#include <simassets/seat_spec_extract.h> // the native seat-spec extraction (S4, ADR 0028)
#include <threedi/threedi_ctrl_catalog.h>
#include <threedi/threedi_panm_pose.h> // the native PANM liveness gate (S3, ADR 0028)

#include <array>
#include <cmath>
#include <cstdio>

using namespace novasim;

void NovaSimulation::reset_infantry_adm_ids() {
	infantry_adm_resolved_ai_count_ = 0;
	if (!world_ || !world_->ai) return;
	AiSystem &ai = *world_->ai;
	for (int i = 0; i < ai.count(); ++i) {
		if (AiEntity *e = ai.at(i)) e->inf.adm_id = 0;
	}
}

int NovaSimulation::set_infantry_anim_map(const Ref<NovaResourceRoot> &p_resource_root, const String &p_adm_name) {
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
	if (runtime_ != nullptr) {
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
void NovaSimulation::resolve_new_infantry_adm_ids() {
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
void NovaSimulation::resolve_infantry_adm_ids(const Ref<NovaResourceRoot> &p_resource_root,
		const Ref<NovaItemDatabase> &p_item_db) {
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
void NovaSimulation::resolve_item_traits(const Ref<NovaItemDatabase> &p_item_db) {
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

void NovaSimulation::install_item_class_resolver() {
	if (!runtime_ || !item_replication_catalog_) return;
	runtime_->view().set_item_class_resolver(
			[catalog = item_replication_catalog_](uint16_t type_id) {
				return catalog->resolve_wire_entity_class(type_id);
			});
}

void NovaSimulation::install_charattr_challenge_table() {
	if (!runtime_) return;
	if (charattr_challenge_loaded_) {
		runtime_->set_charattr_challenge_table(charattr_challenge_table_);
	} else {
		runtime_->clear_charattr_challenge_table();
	}
}

void NovaSimulation::install_character_join_vars() {
	if (!runtime_ || !join_character_vars_set_) return;
	runtime_->set_character_join_vars(join_character_vars_);
}

void NovaSimulation::install_join_integrity_profile() {
	if (!runtime_) return;
	if (join_integrity_profile_id_.empty()) {
		runtime_->clear_integrity_challenge_profile();
		return;
	}
	runtime_->set_integrity_challenge_profile(join_integrity_profile_id_);
}

// The D-AI-5 host weapon seed + per-body sound-profile bind, folded into the
// engine (simassets::resolve_ai_weapons, ADR 0028) over the retained items.def
// rows — the seed semantics and [orig] witnesses live there now. Ammo names
// resolve against the mission ammo table, so call AFTER load_ammo_table.
int NovaSimulation::resolve_ai_weapons(const Ref<NovaItemDatabase> &p_item_db) {
	if (!world_ || !world_->ai || p_item_db.is_null()) return 0;
	return opennova::simassets::resolve_ai_weapons(
			*world_, p_item_db->native_items());
}

void NovaSimulation::apply_collision_to_ai() {
	collision_world_.terrain = terrain_field_.valid() ? &terrain_field_ : nullptr;
	collision_world_.set_section_matrix_provider(this);
	if (world_) {
		world_->collision = &collision_world_;
		world_->mounted_pose_provider = this;
	}
	if (ai_) ai_->collision = &collision_world_;
}

// The S4 A/B seam (ADR 0028): the legacy model-bound resolver stays
// AUTHORITATIVE while the engine-side resolver (simassets::
// resolve_model_mounted_pose over the sim's own parse) shadows it; the
// divergence counters gate the legacy delete alongside the live
// emplaced-gun recipe.
bool NovaSimulation::resolve_mounted_pose(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (mounted_pose_mode_ == CollisionPoseMode::Legacy)
		return resolve_mounted_pose_legacy(p_world, p_carrier, p_seat, r_out);
	if (mounted_pose_mode_ == CollisionPoseMode::Native)
		return resolve_mounted_pose_native(p_world, p_carrier, p_seat, r_out);
	const bool legacy_ok =
			resolve_mounted_pose_legacy(p_world, p_carrier, p_seat, r_out);
	opennova::world::MountedPose native_pose;
	const bool native_ok =
			resolve_mounted_pose_native(p_world, p_carrier, p_seat, native_pose);
	MountedPoseAbStats &ab = mounted_pose_ab_;
	++ab.queries;
	if (legacy_ok && !native_ok) {
		++ab.native_declined;
	} else if (!legacy_ok && native_ok) {
		++ab.native_posed_only;
	} else if (legacy_ok && native_ok) {
		const float dx = r_out.position.x - native_pose.position.x;
		const float dy = r_out.position.y - native_pose.position.y;
		const float dz = r_out.position.z - native_pose.position.z;
		const float pos_delta =
				std::sqrt(dx * dx + dy * dy + dz * dz);
		const auto wrap_delta = [](int16_t a, int16_t b) {
			int d = std::abs(static_cast<int>(a) - static_cast<int>(b));
			return std::min(d, 360 - d);
		};
		const int angle_delta = std::max(
				{wrap_delta(r_out.yaw, native_pose.yaw),
				 wrap_delta(r_out.pitch, native_pose.pitch),
				 wrap_delta(r_out.roll, native_pose.roll)});
		if (pos_delta > ab.max_position_delta)
			ab.max_position_delta = pos_delta;
		if (angle_delta > ab.max_angle_delta)
			ab.max_angle_delta = angle_delta;
		// 0.01 world units / 1 degree: far above float rounding between the
		// two implementations, far below any real mis-pose.
		if (pos_delta > 0.01f || angle_delta > 1) {
			++ab.divergences;
			ab.last_carrier_type = static_cast<int32_t>(p_carrier.item_id);
		}
	}
	return legacy_ok;
}

bool NovaSimulation::resolve_mounted_pose_native(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (!world_ || &p_world != world_.get() ||
			p_seat.type != opennova::world::SeatType::Gunner ||
			p_seat.bone_index == 0)
		return false;
	const auto found = mounted_pose_native_models_.find(p_carrier.item_id);
	if (found == mounted_pose_native_models_.end() || found->second == nullptr)
		return false;
	const Threedi3di3 &model = *found->second;
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr)
		return false;
	// The same three CTRL sources the legacy resolver publishes, written by
	// ordinal onto the retail bus.
	int32_t ctrl_values[THREEDI_CTRL_REGISTER_COUNT] = {};
	AiEntity *carrier_ai = ai_ ? ai_->for_handle(p_carrier.handle) : nullptr;
	const auto phase_for = [carrier_ai](int channel) -> int32_t {
		return carrier_ai != nullptr
				? carrier_ai->brain.f[AiBrain::kPartAnimPhase0 + channel]
				: 0;
	};
	if ((p_carrier.item_attrib & 0x1000u) == 0)
		ctrl_values[THREEDI_CTRL_VEHICLE_SPECIAL1] = phase_for(0);
	ctrl_values[THREEDI_CTRL_VEHICLE_SPECIAL2] = phase_for(1);
	int32_t heat_glow = 0;
	if (opennova::world::world_model_heat_glow_for(p_world, p_carrier, heat_glow))
		ctrl_values[THREEDI_CTRL_HEAT_GLOW] = heat_glow;
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(p_world, ai_.get(), p_carrier, emplaced)) {
		ctrl_values[THREEDI_CTRL_EWEAP_GUNYAW] =
				static_cast<int32_t>(emplaced.gun_yaw);
		ctrl_values[THREEDI_CTRL_EWEAP_GUNPITCH] =
				static_cast<int32_t>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: p_world.logic_tick * 16u;
	return opennova::simassets::resolve_model_mounted_pose(
			model, p_carrier, p_seat, ctrl_values, time_ms, r_out);
}

bool NovaSimulation::resolve_mounted_pose_legacy(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	if (!world_ || &p_world != world_.get() ||
			p_seat.type != opennova::world::SeatType::Gunner ||
			p_seat.bone_index == 0)
		return false;
	const auto data_found =
			mounted_pose_data_by_type_.find(p_carrier.item_id);
	if (data_found == mounted_pose_data_by_type_.end() ||
			data_found->second.is_null())
		return false;
	const Ref<NovaObjectData> &data = data_found->second;

	Dictionary controls;
	const Threedi3di3 &model = data->native_model();
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr)
		return false;
	AiEntity *carrier_ai = ai_ ? ai_->for_handle(p_carrier.handle) : nullptr;
	assign_part_anim_phases(
			controls, (p_carrier.item_attrib & 0x1000u) == 0,
			[carrier_ai](int p_channel) {
		return carrier_ai != nullptr
				? carrier_ai->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel]
				: 0;
	});
	// This is the exact witnessed publisher scope: a live UseGun child is being
	// posed from the parent carrier's PANM/bone transform, so cache the
	// carrier's inline MountSlot before evaluating that parent model.
	// [orig: Entity_AttachToBoneAndUpdateTransform @ 0x546518..0x54652B;
	//  HUD_CacheWeaponSlotInfo @ 0x44095B..0x440991]
	assign_world_model_heat_glow(controls, p_world, p_carrier);
	EmplacedWeaponControls emplaced;
	if (emplaced_weapon_controls_for(
			p_world, ai_.get(), p_carrier, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: p_world.logic_tick * 16u;
	// Keep authority and joiner attachment reconstruction on one matrix path:
	// both consume the same authored userpoint and rest/live bone transforms.
	return resolve_model_mounted_pose(
			data, p_carrier, p_seat, controls, time_ms, r_out);
}

bool NovaSimulation::ensure_collision_instance(
		opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity) {
	if (!world_ || &p_world != world_.get() ||
			collision_item_db_.is_null() || collision_placer_.is_null())
		return false;
	const opennova::world::Entity *entity = p_world.registry.get(p_entity);
	if (entity == nullptr) {
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_pose_native_.remove_entity(p_entity);
		collision_resolution_attempted_.erase(p_entity.packed);
		return false;
	}
	const auto attempted =
			collision_resolution_attempted_.find(p_entity.packed);
	if (attempted != collision_resolution_attempted_.end()) {
		if (attempted->second == entity->registry_spawn_id)
			return collision_world_.has_instance(p_world, p_entity);
		collision_world_.remove_entity_instance(p_entity);
		collision_skeletal_sources_.erase(p_entity.packed);
		collision_pose_native_.remove_entity(p_entity);
		collision_resolution_attempted_.erase(attempted);
	}

	// Re-run the idempotent attach sweep against the retained mission caches.
	// It resolves every entity that appeared since the previous sweep, including
	// a player deployed after load, without registering another graphic model.
	resolve_collision_instances(collision_item_db_, collision_placer_.ptr());
	return collision_world_.has_instance(p_world, p_entity);
}

// The S3 A/B seam (ADR 0028): the engine-side provider evaluates beside the
// legacy binding path until the live soak clears the legacy delete. Compare
// keeps LEGACY authoritative and shadows the native provider, counting
// divergent final matrices; the tolerance absorbs float-noise below anything
// collision-visible (Q22 rotation / 16.16 translation units) while a real
// mis-pose lands orders of magnitude above it.
bool NovaSimulation::build_section_matrices(opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity, int32_t p_model_id,
		const opennova::world::CollisionMatrix &p_entity_world,
		const opennova::world::CollisionModel &p_model,
		std::vector<opennova::world::CollisionMatrix> &r_out) {
	if (collision_pose_mode_ == CollisionPoseMode::Legacy)
		return build_section_matrices_legacy(p_world, p_entity, p_model_id,
				p_entity_world, p_model, r_out);
	// The provider consumes the same per-query sim state the legacy path
	// reads off this binding.
	collision_pose_native_.weapon_active = weapon_active_;
	collision_pose_native_.panm_time_override_ms = panm_time_override_ms_;
	if (collision_pose_mode_ == CollisionPoseMode::Native)
		return collision_pose_native_.build_section_matrices(p_world, p_entity,
				p_model_id, p_entity_world, p_model, r_out);

	const bool legacy_ok = build_section_matrices_legacy(p_world, p_entity,
			p_model_id, p_entity_world, p_model, r_out);
	std::vector<opennova::world::CollisionMatrix> native_mats;
	const bool native_ok = collision_pose_native_.build_section_matrices(
			p_world, p_entity, p_model_id, p_entity_world, p_model, native_mats);
	CollisionPoseAbStats &ab = collision_pose_ab_;
	++ab.queries;
	if (legacy_ok && !native_ok) {
		// A registered-but-missing native source (stub placers without an
		// asset root, unresolved rigs) is a coverage gap, not a divergence.
		++ab.native_declined;
	} else if (!legacy_ok && native_ok) {
		// The native provider posing where legacy declined (e.g. a null
		// placer with native assets) is capability, not divergence.
		++ab.native_posed_only;
	} else if (legacy_ok && native_ok) {
		if (native_mats.size() != r_out.size()) {
			++ab.result_mismatches;
		} else {
			int32_t worst = 0;
			int worst_section = -1;
			for (size_t s = 0; s < r_out.size(); ++s) {
				for (int k = 0; k < 16; ++k) {
					const int64_t d =
							static_cast<int64_t>(r_out[s].m[k]) -
							static_cast<int64_t>(native_mats[s].m[k]);
					const int32_t mag = static_cast<int32_t>(d < 0 ? -d : d);
					if (mag > worst) {
						worst = mag;
						worst_section = static_cast<int>(s);
					}
				}
			}
			if (worst > ab.max_delta) ab.max_delta = worst;
			// 1024 = 2.4e-4 units of Q22 rotation / 0.016 world units of
			// 16.16 translation — far above cross-implementation float
			// rounding, far below any real mis-pose.
			constexpr int32_t kDivergenceTolerance = 1024;
			if (worst > kDivergenceTolerance) {
				++ab.divergences;
				const bool skeletal =
						collision_pose_native_.has_skeletal_entity(p_entity);
				if (skeletal) ++ab.divergences_skeletal;
				else ++ab.divergences_generic;
				ab.last_model_id = p_model_id;
				ab.last_section = worst_section;
				ab.last_delta = worst;
				ab.last_kind = skeletal ? 1 : 0;
				if (worst_section >= 0) {
					for (int k = 0; k < 16; ++k) {
						ab.last_legacy_m[k] =
								r_out[static_cast<size_t>(worst_section)].m[k];
						ab.last_native_m[k] = native_mats[
								static_cast<size_t>(worst_section)].m[k];
					}
				}
			}
		}
	}
	return legacy_ok;
}

void NovaSimulation::debug_set_collision_pose_mode(int p_mode) {
	switch (p_mode) {
		case 0: collision_pose_mode_ = CollisionPoseMode::Legacy; break;
		case 2: collision_pose_mode_ = CollisionPoseMode::Native; break;
		default: collision_pose_mode_ = CollisionPoseMode::Compare; break;
	}
}

int NovaSimulation::debug_get_collision_pose_mode() const {
	return static_cast<int>(collision_pose_mode_);
}

Dictionary NovaSimulation::debug_collision_pose_ab_stats() const {
	Dictionary out;
	out["queries"] = static_cast<int64_t>(collision_pose_ab_.queries);
	out["divergences"] = static_cast<int64_t>(collision_pose_ab_.divergences);
	out["divergences_skeletal"] =
			static_cast<int64_t>(collision_pose_ab_.divergences_skeletal);
	out["divergences_generic"] =
			static_cast<int64_t>(collision_pose_ab_.divergences_generic);
	out["result_mismatches"] =
			static_cast<int64_t>(collision_pose_ab_.result_mismatches);
	out["native_declined"] =
			static_cast<int64_t>(collision_pose_ab_.native_declined);
	out["native_posed_only"] =
			static_cast<int64_t>(collision_pose_ab_.native_posed_only);
	out["max_delta"] = collision_pose_ab_.max_delta;
	out["last_model_id"] = collision_pose_ab_.last_model_id;
	out["last_section"] = collision_pose_ab_.last_section;
	out["last_delta"] = collision_pose_ab_.last_delta;
	out["last_kind"] = collision_pose_ab_.last_kind;
	PackedInt32Array legacy_m;
	PackedInt32Array native_m;
	legacy_m.resize(16);
	native_m.resize(16);
	for (int k = 0; k < 16; ++k) {
		legacy_m.set(k, collision_pose_ab_.last_legacy_m[k]);
		native_m.set(k, collision_pose_ab_.last_native_m[k]);
	}
	out["last_legacy_matrix"] = legacy_m;
	out["last_native_matrix"] = native_m;
	return out;
}

void NovaSimulation::debug_set_mounted_pose_mode(int p_mode) {
	switch (p_mode) {
		case 0: mounted_pose_mode_ = CollisionPoseMode::Legacy; break;
		case 2: mounted_pose_mode_ = CollisionPoseMode::Native; break;
		default: mounted_pose_mode_ = CollisionPoseMode::Compare; break;
	}
}

int NovaSimulation::debug_get_mounted_pose_mode() const {
	return static_cast<int>(mounted_pose_mode_);
}

Dictionary NovaSimulation::debug_mounted_pose_ab_stats() const {
	Dictionary out;
	out["queries"] = static_cast<int64_t>(mounted_pose_ab_.queries);
	out["divergences"] = static_cast<int64_t>(mounted_pose_ab_.divergences);
	out["native_declined"] =
			static_cast<int64_t>(mounted_pose_ab_.native_declined);
	out["native_posed_only"] =
			static_cast<int64_t>(mounted_pose_ab_.native_posed_only);
	out["max_position_delta"] = mounted_pose_ab_.max_position_delta;
	out["max_angle_delta"] = mounted_pose_ab_.max_angle_delta;
	out["last_carrier_type"] = mounted_pose_ab_.last_carrier_type;
	return out;
}

// The S4 static A/B: re-extract the installed seat-spec table's types through
// the engine-side extractor (retained def rows + the sim's parses) and diff
// the typed records. The three turret_* windows are stamped post-install from
// the weapon table and are excluded. Empty native sources report as an error
// (the GUT stub worlds); production tables compare field-for-field.
Dictionary NovaSimulation::debug_native_seat_spec_diff(
		const Ref<NovaItemDatabase> &p_item_db) {
	Dictionary out;
	out["compared"] = 0;
	out["mismatches"] = 0;
	out["native_missing"] = 0;
	out["first_mismatch"] = String();
	if (p_item_db.is_null() || !sim_models_.has_index()) {
		out["error"] = "native sources unavailable";
		return out;
	}
	std::vector<int> seeds;
	seeds.reserve(item_seat_specs_.size());
	for (const opennova::mission::ItemSeatSpec &spec : item_seat_specs_)
		seeds.push_back(spec.type_id +
				static_cast<int32_t>(opennova::mission::kItemIdOffset));
	opennova::simassets::SeatSpecExtraction native;
	opennova::simassets::extract_item_seat_specs(
			p_item_db->native_items(),
			[this](const std::string &graphic) {
				return sim_models_.model_for(graphic);
			},
			seeds, native);
	int compared = 0;
	int mismatches = 0;
	int native_missing = 0;
	String first;
	const auto note = [&](int32_t type_id, const char *what) {
		++mismatches;
		if (first.is_empty())
			first = String("type ") + String::num_int64(type_id) + ": " + what;
	};
	const auto vec_near = [](const opennova::world::Vec3 &a,
			const opennova::world::Vec3 &b) {
		return std::fabs(a.x - b.x) < 1e-3f && std::fabs(a.y - b.y) < 1e-3f &&
				std::fabs(a.z - b.z) < 1e-3f;
	};
	// yaw offsets are angles: +-180 (the atan2 branch at the exact back
	// direction) is the same seat facing.
	const auto yaw_equal = [](int16_t a, int16_t b) {
		const int d = ((static_cast<int>(a) - static_cast<int>(b)) % 360 + 360) % 360;
		return d == 0;
	};
	for (const opennova::mission::ItemSeatSpec &installed : item_seat_specs_) {
		const auto native_it = std::lower_bound(
				native.specs.begin(), native.specs.end(), installed.type_id,
				[](const opennova::mission::ItemSeatSpec &s, int32_t t) {
					return s.type_id < t;
				});
		if (native_it == native.specs.end() ||
				native_it->type_id != installed.type_id) {
			++native_missing;
			continue;
		}
		++compared;
		const opennova::mission::ItemSeatSpec &n = *native_it;
		if (n.mount_config_valid != installed.mount_config_valid ||
				n.mount_config != installed.mount_config)
			note(installed.type_id, "mount_config");
		if (n.primary_weapon != installed.primary_weapon)
			note(installed.type_id, "primary_weapon");
		if (n.seats.size() != installed.seats.size()) {
			note(installed.type_id, "seat count");
		} else {
			for (size_t s = 0; s < n.seats.size(); ++s) {
				const opennova::world::Seat &a = installed.seats[s];
				const opennova::world::Seat &b = n.seats[s];
				if (a.type != b.type || a.retail_slot != b.retail_slot ||
						a.bone_index != b.bone_index ||
						a.pose_index != b.pose_index ||
						!yaw_equal(a.yaw_offset, b.yaw_offset) ||
						a.source_name != b.source_name ||
						!vec_near(a.seat_local, b.seat_local)) {
					char detail[240];
					std::snprintf(detail, sizeof(detail),
							"seat %d inst(t=%d slot=%d bone=%d pose=%d yaw=%d src=%s l=%.4f,%.4f,%.4f) nat(t=%d slot=%d bone=%d pose=%d yaw=%d src=%s l=%.4f,%.4f,%.4f)",
							static_cast<int>(s), static_cast<int>(a.type),
							a.retail_slot, a.bone_index, a.pose_index,
							a.yaw_offset, a.source_name.c_str(),
							a.seat_local.x, a.seat_local.y, a.seat_local.z,
							static_cast<int>(b.type), b.retail_slot,
							b.bone_index, b.pose_index, b.yaw_offset,
							b.source_name.c_str(), b.seat_local.x,
							b.seat_local.y, b.seat_local.z);
					note(installed.type_id, detail);
					break;
				}
			}
		}
		if (n.armory_points.size() != installed.armory_points.size()) {
			note(installed.type_id, "armory count");
		} else {
			for (size_t s = 0; s < n.armory_points.size(); ++s) {
				if (!vec_near(installed.armory_points[s],
						n.armory_points[s])) {
					note(installed.type_id, "armory point");
					break;
				}
			}
		}
		if (n.emplacement_attachments.size() !=
				installed.emplacement_attachments.size()) {
			note(installed.type_id, "attachment count");
		} else {
			for (size_t s = 0; s < n.emplacement_attachments.size(); ++s) {
				const opennova::mission::ItemEmplacementAttachmentSpec &a =
						installed.emplacement_attachments[s];
				const opennova::mission::ItemEmplacementAttachmentSpec &b =
						n.emplacement_attachments[s];
				if (a.child_type_id != b.child_type_id || a.kind != b.kind ||
						a.stored_slot != b.stored_slot ||
						a.attachment_flags != b.attachment_flags ||
						a.angle_count != b.angle_count ||
						a.down_limit_bam != b.down_limit_bam ||
						a.up_limit_bam != b.up_limit_bam ||
						a.right_limit_bam != b.right_limit_bam ||
						a.left_limit_bam != b.left_limit_bam ||
						a.anchor_found != b.anchor_found ||
						a.anchor.bone_index != b.anchor.bone_index ||
						!yaw_equal(a.anchor.yaw_offset, b.anchor.yaw_offset) ||
						a.anchor.source_name != b.anchor.source_name ||
						!vec_near(a.anchor.seat_local, b.anchor.seat_local)) {
					note(installed.type_id, "attachment fields");
					break;
				}
			}
		}
	}
	out["compared"] = compared;
	out["mismatches"] = mismatches;
	out["native_missing"] = native_missing;
	out["native_total"] = static_cast<int>(native.specs.size());
	out["first_mismatch"] = first;
	return out;
}

bool NovaSimulation::build_section_matrices_legacy(opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity, int32_t p_model_id,
		const opennova::world::CollisionMatrix &p_entity_world,
		const opennova::world::CollisionModel &p_model,
		std::vector<opennova::world::CollisionMatrix> &r_out) {
	const auto skeletal_found =
			collision_skeletal_sources_.find(p_entity.packed);
	const opennova::world::Entity *entity =
			p_world.registry.get(p_entity);
	if (skeletal_found != collision_skeletal_sources_.end() &&
			skeletal_found->second.model_id == p_model_id &&
			entity != nullptr && skeletal_found->second.registry_spawn_id ==
					entity->registry_spawn_id) {
		const SkeletalCollisionSource &source = skeletal_found->second;
		AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
		const size_t section_count = p_model.sections.size();
		if (source.anim.is_null() || ai_entity == nullptr || entity == nullptr ||
				source.parents.size() < section_count ||
				source.rest_global.size() < section_count ||
				source.overlay_classes.size() < static_cast<int64_t>(section_count))
			return false;

		const String reset_key("anim_reset");
		auto resolve_primary_key = [&](const String &p_key) {
			if (source.anim->has_clip(p_key)) return p_key;
			return source.anim->has_clip(reset_key) ? reset_key : p_key;
		};
		const String primary_key =
				resolve_primary_key(infantry_anim_key(ai_entity->inf.anim_state));
		if (primary_key.is_empty()) return false;
		const float primary_fps = source.anim->get_clip_fps(primary_key, 0);
		const double primary_seconds = primary_fps > 0.0f
				? static_cast<double>(std::max(ai_entity->inf.clip_phase, 0)) /
						(2.0 * primary_fps)
				: 0.0;
		String source_key;
		double source_seconds = 0.0;
		const bool primary_blend = ai_entity->inf.body_blend_active();
		if (primary_blend) {
			source_key = resolve_primary_key(
					infantry_anim_key(ai_entity->inf.anim_prev));
			const float source_fps = source.anim->get_clip_fps(source_key, 0);
			if (source_fps > 0.0f)
				source_seconds =
						static_cast<double>(
								std::max(ai_entity->inf.anim_prev_clip_phase, 0)) /
						(2.0 * source_fps);
		}

		const opennova::anim::AimOverlayInputs inputs =
				aim_overlay_inputs_for(*ai_entity, *entity);
		opennova::anim::AimOverlayAngles
				angles[opennova::anim::kOverlayClassCount];
		opennova::anim::compute_aim_overlay_angles(inputs, angles);
		const Array deltas = aim_overlay_deltas_for(angles);

		String weapon_key;
		double weapon_seconds = 0.0;
		const bool collapse_right_hand =
				mount_collapses_right_hand_row(*entity);
		if (p_world.cached.local_player.valid() &&
				p_entity.packed == p_world.cached.local_player.packed &&
				opennova::world::infantry_weapon_channel_visible(
						ai_entity->inf, weapon_active_,
						mount_blocks_weapon_channel(*entity))) {
			weapon_key = infantry_anim_key(ai_entity->inf.wpn_state);
			const float weapon_fps = source.anim->get_clip_fps(weapon_key, 0);
			if (weapon_fps > 0.0f)
				weapon_seconds =
						static_cast<double>(
								std::max(ai_entity->inf.wpn_clip_phase, 0)) /
						(2.0 * weapon_fps);
		}

		const Array pose = primary_blend
				? source.anim->eval_pose_blended_overlay(
						source_key, source_seconds,
						primary_key, primary_seconds,
						ai_entity->inf.anim_blend_weight,
						source.overlay_classes, deltas,
						weapon_key, weapon_seconds, collapse_right_hand)
				: source.anim->eval_pose_overlay(
						primary_key, primary_seconds,
						source.overlay_classes, deltas,
						weapon_key, weapon_seconds, collapse_right_hand);
		if (pose.size() < static_cast<int64_t>(section_count)) return false;

		// The callback result is FINAL world-space. Build the body placement from
		// the overlay's body class (not the aim heading), then apply the skinned
		// deformation exactly once. At bind pose pose_global*rest_global^-1 is
		// identity, which guards against both double-rest and double-entity
		// translation. COBJ parent/offset/CXLT are deliberately not selectors:
		// COBJ[i] pairs strictly with this output slot i.
		const int32_t position[3] = {
				p_entity_world.m[3], p_entity_world.m[7], p_entity_world.m[11]};
		const opennova::world::CollisionMatrix body_world =
				opennova::world::collision_matrix_from_euler(
						angles[opennova::anim::kOverlayBody].yaw,
						angles[opennova::anim::kOverlayBody].pitch,
						angles[opennova::anim::kOverlayBody].roll, position);
		std::vector<Transform3D> pose_global(section_count);
		r_out.resize(section_count);
		for (size_t i = 0; i < section_count; ++i) {
			const Variant value = pose[static_cast<int64_t>(i)];
			if (value.get_type() != Variant::TRANSFORM3D) return false;
			const Transform3D local = static_cast<Transform3D>(value);
			const int32_t parent = source.parents[i];
			// eval_pose_overlay emits BN17's zero-scale local clip pose.
			// Retail zeroes the FINAL collision row after overlay/re-anchor. Preserve
			// that literal collision result for COBJ 16: composing body_world here
			// would incorrectly reintroduce the entity translation.
			// [orig: special row @0x4b1290]
			const bool collapsed_right_hand =
					collapse_right_hand && i == 16;
			if (collapsed_right_hand) {
				pose_global[i] = local;
				r_out[i] = opennova::world::CollisionMatrix{};
				continue;
			}
			pose_global[i] = parent >= 0
					? pose_global[static_cast<size_t>(parent)] * local
					: local;
			const Transform3D deformation =
					pose_global[i] * source.rest_global[i].affine_inverse();
			float render_pose[16];
			panm_render_matrix_from_godot(deformation, render_pose);
			if (!opennova::world::collision_matrix_apply_render_pose(
						body_world, render_pose, r_out[i]))
				return false;
		}
		return true;
	}

	const auto found = collision_pose_data_.find(p_model_id);
	if (found == collision_pose_data_.end() || found->second.is_null()) return false;
	const Ref<NovaObjectData> &data = found->second;
	// Retail Generic collision always transforms the canonical first RLOD. It
	// never follows the render-selected LOD or scans for another live PANM.
	constexpr int lod_index = 0;
	if (!data->has_live_panm_for_lod(lod_index)) return false;
	const PackedInt32Array targets =
			data->get_effective_panm_targets(lod_index);
	if (targets.is_empty()) return false;
	const Threedi3di3 &model = data->native_model();
	if (model.ctrl.count > 0 && model.ctrl.registers == nullptr)
		return false;

	// PLAYPARTANIM publishes its two phase accumulators to the fixed retail
	// VEHICLE_SPECIAL1/2 registers. A brainless static still evaluates
	// free-running PANM with zero phase values.
	Dictionary controls;
	AiEntity *ai_entity = ai_ ? ai_->for_handle(p_entity) : nullptr;
	assign_part_anim_phases(
			controls, entity == nullptr || (entity->item_attrib & 0x1000u) == 0,
			[ai_entity](int p_channel) {
		return ai_entity != nullptr
				? ai_entity->brain.f[
						AiBrain::kPartAnimPhase0 + p_channel]
				: 0;
	});
	// The generic collision frame receives HEAT_GLOW only when this model is the
	// carrier in that same live UseGun attachment relation. The helper omits it
	// for every other entity; a scoped cold slot still writes literal zero.
	// [orig: attachment caller @ 0x546518;
	//  HUD_CacheWeaponSlotInfo cold/hot stores @ 0x440969/@0x440991]
	if (entity != nullptr)
		assign_world_model_heat_glow(controls, p_world, *entity);
	// EWEAP yaw/pitch are independent semantic CTRL writers. B50Cal consumes
	// this pair and is unaffected by the VEHICLE_SPECIAL publication above.
	EmplacedWeaponControls emplaced;
	if (entity != nullptr &&
			emplaced_weapon_controls_for(p_world, ai_.get(), *entity, emplaced)) {
		controls[String(kEmplacedGunYawRegister)] =
				static_cast<int>(emplaced.gun_yaw);
		controls[String(kEmplacedGunPitchRegister)] =
				static_cast<int>(emplaced.gun_pitch);
	}
	const uint32_t time_ms = panm_time_override_ms_ >= 0
			? static_cast<uint32_t>(panm_time_override_ms_)
			: (world_ != nullptr ? world_->logic_tick * 16u : 0u);
	const Dictionary transforms =
			data->evaluate_panm(lod_index, time_ms, controls);
	if (transforms.is_empty()) return false;

	// Default every COBJ slot to the Simple callback. Override only PANM nodes
	// whose target part ordinal exists as a collision section. This intentionally
	// ignores COBJ parent metadata and CXLT/offset records: CVRT is model-space.
	r_out.assign(p_model.sections.size(), p_entity_world);
	bool matched_section = false;
	for (int i = 0; i < targets.size(); ++i) {
		const int section = targets[i];
		if (static_cast<size_t>(section) >= r_out.size() ||
				!transforms.has(section))
			continue;
		const Variant value = transforms[section];
		if (value.get_type() != Variant::TRANSFORM3D) return false;
		float pose[16];
		panm_render_matrix_from_godot(static_cast<Transform3D>(value), pose);
		if (!opennova::world::collision_matrix_apply_render_pose(
					p_entity_world, pose, r_out[static_cast<size_t>(section)]))
			return false;
		matched_section = true;
	}
	return matched_section;
}

void NovaSimulation::set_asset_root(const Ref<NovaResourceRoot> &p_root) {
	asset_root_ = p_root;
	sim_models_.set_index(
			p_root.is_valid() ? &p_root->native_index() : nullptr);
	collision_pose_native_.set_resource_index(
			p_root.is_valid() ? &p_root->native_index() : nullptr);
}

int NovaSimulation::resolve_collision_instances(const Ref<NovaItemDatabase> &p_item_db,
                                                Object *p_placer) {
	if (!world_ || p_item_db.is_null() || p_placer == nullptr) return 0;
	// Production installs the sim's own asset source first (ADR 0028); the
	// no-root leg below is the GUT stub seam and dies with S3.
	if (!sim_models_.has_index()) {
		godot::UtilityFunctions::print_verbose(
				"NovaSimulation: no asset root installed — collision extraction "
				"falls back to the placer (test seam; production calls "
				"set_asset_root first)");
	}
	RefCounted *placer_ref = Object::cast_to<RefCounted>(p_placer);
	if (placer_ref == nullptr) return 0;
	collision_item_db_ = p_item_db;
	collision_placer_ = placer_ref;
	apply_collision_to_ai();
	std::vector<opennova::world::EntityHandle> handles;
	world_->registry.for_each(
			[&](const opennova::world::Entity &e) { handles.push_back(e.handle); });
	int attached = 0;
	for (const opennova::world::EntityHandle h : handles) {
		opennova::world::Entity *e = world_->registry.get(h);
		if (!e || e->kind == opennova::world::EntityKind::Marker)
			continue;
		const auto previous_attempt =
				collision_resolution_attempted_.find(h.packed);
		if (previous_attempt != collision_resolution_attempted_.end() &&
				previous_attempt->second != e->registry_spawn_id) {
			collision_world_.remove_entity_instance(h);
			collision_skeletal_sources_.erase(h.packed);
			collision_pose_native_.remove_entity(h);
		}
		collision_resolution_attempted_[h.packed] = e->registry_spawn_id;
		const bool is_organic =
				e->kind == opennova::world::EntityKind::Organic;
		const int def_id = is_organic
				? visual_item_id_for_runtime_type(e->item_id, p_item_db)
				: static_cast<int>(e->item_id) +
						opennova::mission::kItemIdOffset;
		const String graphic = p_item_db->get_graphic(def_id);
		if (graphic.is_empty()) continue;
		const std::string key(graphic.utf8().get_data());
		auto it = collision_model_by_graphic_.find(key);
		if (it == collision_model_by_graphic_.end()) {
			int32_t model_id = -1;
			int32_t occlusion_id = -1;
			float bound_radius = 0.0f;
			if (sim_models_.has_index()) {
				// ADR 0028: the sim reads its own parse-once cache. The placer
				// now supplies only the render-side pose sources (live-PANM
				// object data + skeletal sets) — the S3 push-down target.
				if (const Threedi3di3 *m3 = sim_models_.model_for(key)) {
					opennova::world::CollisionModel model;
					if (collision_model_from_3di(m3->collision, model,
							opennova::simassets::model_has_collision(*m3))) {
						model_id = collision_world_.add_model(std::move(model));
						Ref<NovaObjectData> pose_data =
								p_placer->call("object_data_for", graphic);
						if (pose_data.is_valid() &&
								pose_data->has_live_panm_for_lod(0))
							collision_pose_data_[model_id] = pose_data;
						// S3 (ADR 0028): the engine-side provider poses this
						// model from the sim's own retained parse.
						if (threedi_panm_lod_has_live(*m3, 0))
							collision_pose_native_.register_generic_model(
									model_id, m3);
					}
					opennova::world::OcclusionModel occ;
					if (occlusion_model_from_3di(*m3, occ))
						occlusion_id = occlusion_world_.add_model(std::move(occ));
					bound_radius = model_bound_radius_from_3di(*m3);
				}
			} else {
				// Duck-typed MissionObjectPlacer.object_data_for(graphic) — the
				// legacy render-cache extraction. Test-only once production
				// installs the asset root; deleted with S3.
				Ref<NovaObjectData> data = p_placer->call("object_data_for", graphic);
				if (data.is_valid()) {
					opennova::world::CollisionModel model;
					if (collision_model_from_3di(
							data->native_model().collision, model, data->has_collision())) {
						model_id = collision_world_.add_model(std::move(model));
						if (data->has_live_panm_for_lod(0))
							collision_pose_data_[model_id] = data;
					}
					opennova::world::OcclusionModel occ;
					if (occlusion_model_from_3di(data->native_model(), occ))
						occlusion_id = occlusion_world_.add_model(std::move(occ));
					bound_radius = model_bound_radius_from_3di(data->native_model());
				}
			}
			it = collision_model_by_graphic_.emplace(key, model_id).first;
			collision_occlusion_by_graphic_.emplace(key, occlusion_id);
			collision_radius_by_graphic_.emplace(key, bound_radius);
		}
		// The bound-sphere radius (entity+0 boundRadius) comes from the .3di
		// MODEL header bound, not the collision block — every placed item
		// carries one, so collision-less props are still hittable by rounds and
		// reachable by blasts. Raised to the husk model's bound below, then
		// padded +0.0625 [orig: Entity_InitFromModel @ 0x40dc30 — boundRadius =
		// max(gpm[5], husk gpm[5]) + 0x1000; the authored def scale factor is
		// not yet applied (tracked, D-COL-3)].
		float entity_bound = collision_radius_by_graphic_[key];
		// Platform probe boxes (vehicle-client-movers-re.md §3 §3): the union of the
		// authored per-subobject collision AABBs — exact 16.16 values in the
		// 3di's own model space, the space the retail modelData boxes
		// [0x28..0x4C] live in. The box1-vs-footprint provenance (and the
		// axis-pair naming) is the spec's tracked unknown: both map to this
		// union here, verified against hull proportions at the solve's bench.
		if (h.pool() == 1) {
			opennova::world::VehicleTraits *vt =
					world_->vehicle_traits.get_mutable(e->item_id);
			if (vt != nullptr && vt->box_z_hi == vt->box_z_lo) {
				const Threedi3di3 *vm3 = sim_models_.has_index()
						? sim_models_.model_for(key)
						: nullptr;
				Ref<NovaObjectData> vdata;
				if (vm3 == nullptr && !sim_models_.has_index())
					vdata = p_placer->call("object_data_for", graphic);
				if (vm3 != nullptr || vdata.is_valid()) {
					const ThreediCollisionModel *col = vm3 != nullptr
							? vm3->collision
							: vdata->native_model().collision;
					if (col != nullptr && col->objects != nullptr &&
							col->object_count > 0) {
						int32_t lo[3] = {INT32_MAX, INT32_MAX, INT32_MAX};
						int32_t hi[3] = {INT32_MIN, INT32_MIN, INT32_MIN};
						for (size_t o = 0; o < col->object_count; ++o) {
							const auto &obj = col->objects[o];
							for (int a = 0; a < 3; ++a) {
								lo[a] = std::min(lo[a], obj.offset[a] + obj.min[a]);
								hi[a] = std::max(hi[a], obj.offset[a] + obj.max[a]);
							}
						}
						if (hi[0] > lo[0] && hi[1] > lo[1] && hi[2] > lo[2]) {
							vt->box_x_lo = lo[0];
							vt->box_x_hi = hi[0];
							vt->box_y_lo = lo[1];
							vt->box_y_hi = hi[1];
							vt->box_z_lo = lo[2];
							vt->box_z_hi = hi[2];
							vt->foot_x_lo = lo[0];
							vt->foot_x_hi = hi[0];
							vt->foot_y_lo = lo[1];
							vt->foot_y_hi = hi[1];
						}
					}
				}
			}
		}
		if (it->second >= 0) {
			collision_world_.assign_entity(
					h, it->second, e->registry_spawn_id);
			++attached;
			if (is_organic) {
				collision_skeletal_sources_.erase(h.packed);
				collision_pose_native_.remove_entity(h);
				// S3 (ADR 0028): the native skeletal source resolves from
				// the retained def rows + the sim's own model parse; the
				// provider validates rig/FK and declines at query time
				// exactly like the unregistered legacy leg when it cannot.
				if (sim_models_.has_index()) {
					const String anim_def = p_item_db->get_anim_def(def_id);
					if (!anim_def.is_empty()) {
						String adm = anim_def;
						if (!adm.to_lower().ends_with(".adm"))
							adm += ".adm";
						const std::string adm_name(adm.utf8().get_data());
						collision_pose_native_.register_skeletal_entity(
								h, e->registry_spawn_id, it->second,
								opennova::strutil::to_lower(adm_name) + "|" + key,
								adm_name, sim_models_.model_for(key));
					}
				}
				Ref<NovaSkeletalAnim> skeletal =
						p_placer->call("skeletal_anim_for", def_id, graphic);
				const opennova::world::CollisionModel *person_model =
						collision_world_.model(it->second);
				if (skeletal.is_valid() && skeletal->is_loaded() &&
						person_model != nullptr) {
					const Array bones = skeletal->get_skeleton_bones();
					const size_t section_count = person_model->sections.size();
					if (bones.size() >= static_cast<int64_t>(section_count)) {
						SkeletalCollisionSource source;
						source.model_id = it->second;
						source.registry_spawn_id = e->registry_spawn_id;
						source.anim = skeletal;
						source.overlay_classes =
								skeletal->get_overlay_classes();
						source.parents.resize(section_count, -1);
						source.rest_global.resize(section_count);
						bool valid_rig =
								source.overlay_classes.size() >=
								static_cast<int64_t>(section_count);
						for (size_t i = 0; valid_rig && i < section_count; ++i) {
							const Variant bone_value =
									bones[static_cast<int64_t>(i)];
							if (bone_value.get_type() != Variant::DICTIONARY) {
								valid_rig = false;
								break;
							}
							const Dictionary bone = bone_value;
							const int32_t parent =
									static_cast<int32_t>(bone.get(
											"parent_index", -1));
							const Variant rest_value =
									bone.get("rest", Transform3D());
							if (parent < -1 ||
									parent >= static_cast<int32_t>(i) ||
									rest_value.get_type() != Variant::TRANSFORM3D) {
								valid_rig = false;
								break;
							}
							const Transform3D rest =
									static_cast<Transform3D>(rest_value);
							source.parents[i] = parent;
							source.rest_global[i] = parent >= 0
									? source.rest_global[
											static_cast<size_t>(parent)] * rest
									: rest;
						}
						if (valid_rig)
							collision_skeletal_sources_[h.packed] =
									std::move(source);
					}
				}
			}
		}
		// The husk-stage collision model: attached beside the graphic instance so
		// every query swaps to the wreck once Flags & 4 sets. The collision pick
		// is the FIRST husk stage (entity+52 huskModel), not huskFinal [orig: the
		// +52 substitution @ 0x538720 / @ 0x413086; D-AI-7 residual closed].
		const String first_husk_name_s = p_item_db->get_husk(def_id);
		const String final_husk_name_s = p_item_db->get_huskfinal(def_id);
		const String husk_name_s = first_husk_name_s.is_empty()
				? final_husk_name_s
				: first_husk_name_s;
		if (!husk_name_s.is_empty()) {
			// Retail keeps live huskModel and huskFinalModel pointers on the
			// entity. A successfully opened model supplies that pointer even when
			// it has no collision block; missing/corrupt assets leave it null.
			// [orig: Entity_ProcessBuildingDeath @ 0x49442c]
			const bool native_assets = sim_models_.has_index();
			const Threedi3di3 *first_husk_m3 = nullptr;
			const Threedi3di3 *final_husk_m3 = nullptr;
			Ref<NovaObjectData> first_husk_data;
			Ref<NovaObjectData> final_husk_data;
			if (native_assets) {
				if (!first_husk_name_s.is_empty())
					first_husk_m3 = sim_models_.model_for(
							std::string(first_husk_name_s.utf8().get_data()));
				if (!final_husk_name_s.is_empty())
					final_husk_m3 = sim_models_.model_for(
							std::string(final_husk_name_s.utf8().get_data()));
			} else {
				if (!first_husk_name_s.is_empty())
					first_husk_data = p_placer->call("object_data_for", first_husk_name_s);
				if (!final_husk_name_s.is_empty())
					final_husk_data = p_placer->call("object_data_for", final_husk_name_s);
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id))
				t->husk_model_loaded = native_assets
						? (first_husk_m3 != nullptr || final_husk_m3 != nullptr)
						: (first_husk_data.is_valid() || final_husk_data.is_valid());
			const std::string husk_key(husk_name_s.utf8().get_data());
			Ref<NovaObjectData> husk_data = first_husk_name_s.is_empty()
					? final_husk_data
					: first_husk_data;
			// One model view for both modes: the sim cache's parse, or the
			// legacy render object's (the S3-retired stub seam).
			const Threedi3di3 *husk_m3 = native_assets
					? (first_husk_name_s.is_empty() ? final_husk_m3 : first_husk_m3)
					: (husk_data.is_valid() ? &husk_data->native_model() : nullptr);
			auto hit = collision_model_by_graphic_.find(husk_key);
			if (hit == collision_model_by_graphic_.end()) {
				int32_t husk_model_id = -1;
				if (husk_m3 != nullptr) {
					opennova::world::CollisionModel hmodel;
					const bool husk_spheres = native_assets
							? opennova::simassets::model_has_collision(*husk_m3)
							: husk_data->has_collision();
					if (collision_model_from_3di(
							husk_m3->collision, hmodel, husk_spheres)) {
						husk_model_id = collision_world_.add_model(std::move(hmodel));
						Ref<NovaObjectData> husk_pose = native_assets
								? Ref<NovaObjectData>(
										p_placer->call("object_data_for", husk_name_s))
								: husk_data;
						if (husk_pose.is_valid() &&
								husk_pose->has_live_panm_for_lod(0))
							collision_pose_data_[husk_model_id] = husk_pose;
						// S3 (ADR 0028): only sim-cache parses are
						// lifetime-stable enough for the native provider (the
						// stub-placer husk view dies with its Ref).
						if (native_assets &&
								threedi_panm_lod_has_live(*husk_m3, 0))
							collision_pose_native_.register_generic_model(
									husk_model_id, husk_m3);
					}
					collision_radius_by_graphic_.emplace(husk_key,
							model_bound_radius_from_3di(*husk_m3));
				}
				hit = collision_model_by_graphic_.emplace(
						husk_key, husk_model_id).first;
				collision_occlusion_by_graphic_.emplace(husk_key, -1);
			}
			if (hit->second >= 0 && it->second >= 0)
				collision_world_.assign_entity_husk(h, hit->second);
			// Retail's death-sound tail walks exact, case-insensitive "KZ"
			// user points on the active FIRST husk, not the huskFinal piece
			// model, and queues a radius-5 blast at every match. Cache this
			// metadata separately from collision registration: the same graphic
			// may already be resident as another entity's main model.
			// Unlike collision's legacy final-only fallback, the retail KZ walker
			// reads entity+52 huskModel. A def with only huskFinal has no KZ source
			// and therefore takes the entity-origin fallback blast.
			if (!first_husk_name_s.is_empty()) {
				auto kz_it = collision_husk_kz_points_by_graphic_.find(husk_key);
				if (kz_it == collision_husk_kz_points_by_graphic_.end()) {
					std::vector<opennova::world::Vec3> kz_points;
					if (husk_m3 != nullptr) {
						const Threedi3di3 &hmodel3di = *husk_m3;
						for (size_t up_index = 0;
								hmodel3di.user_points != nullptr && up_index < hmodel3di.user_point_count;
								++up_index) {
							const ThreediUserPoint &point = hmodel3di.user_points[up_index];
							if (String::utf8(point.name).nocasecmp_to("KZ") != 0)
								continue;
							// Decoded model space is (-source y, source z, source x);
							// destruction's placement math consumes mission-local (x, y, z).
							float up_pos[3];
							threedi_user_point_position(&point, up_pos);
							kz_points.push_back(opennova::world::Vec3{
									up_pos[2],
									-up_pos[0],
									up_pos[1]});
						}
					}
					kz_it = collision_husk_kz_points_by_graphic_.emplace(
							husk_key, std::move(kz_points)).first;
				}
				if (opennova::world::ItemDeathTraits *t =
							world_->item_death_traits.get_mutable(e->item_id);
						t != nullptr && t->kz_points.empty() && !kz_it->second.empty())
					t->kz_points = kz_it->second;
			}
			// The PIECE model is huskFINAL first [orig: @ 0x4934af
			// huskFinalModel ?: huskModel] — the opposite preference from the
			// collision husk pick above. Its LOD-0 part table feeds the
			// death-piece loop bound [orig: renderObj[8]+52 @ 0x49361a], the
			// per-section centers [orig: the section-row center @ 0x4938bf],
			// and section 0's z extents (the wreck ground-rest offset
			// [orig: @ 0x461e23-0x461e4b]). Its own cache, independent of the
			// collision cache: a husk graphic can double as some entity's main
			// graphic, which would leave the joint cache without an entry.
			const String piece_name_s = final_husk_name_s.is_empty()
					? first_husk_name_s
					: final_husk_name_s;
			const std::string piece_key(piece_name_s.utf8().get_data());
			auto hs = collision_husk_pieces_by_graphic_.find(piece_key);
			if (hs == collision_husk_pieces_by_graphic_.end()) {
				CollisionHuskPieceInfo info;
				Ref<NovaObjectData> hdata = final_husk_name_s.is_empty()
						? first_husk_data
						: final_husk_data;
				const Threedi3di3 *piece_m3 = native_assets
						? (final_husk_name_s.is_empty() ? first_husk_m3
						                                : final_husk_m3)
						: (hdata.is_valid() ? &hdata->native_model() : nullptr);
				if (piece_m3 != nullptr && piece_m3->lod_count > 0 &&
				    piece_m3->lods != nullptr) {
					const ThreediLod &lod = piece_m3->lods[0];
					info.sections = static_cast<int32_t>(lod.render_object_count);
					for (size_t pi = 0; lod.render_objects != nullptr && pi < lod.render_object_count;
							++pi) {
						const ThreediRenderObject &part = lod.render_objects[pi];
						info.centers.push_back(opennova::world::Vec3{
								part.abs[0] + part.bounding_center[0],
								part.abs[1] + part.bounding_center[1],
								part.abs[2] + part.bounding_center[2]});
					}
					// Section 0 owns the first opaque+alpha strip run (strips are
					// stored sequentially per render object).
					if (lod.render_objects != nullptr && lod.render_object_count > 0 &&
					    lod.strips != nullptr) {
						const ThreediRenderObject &p0 = lod.render_objects[0];
						const int32_t p0_strip_count = p0.num_strips + p0.num_alpha_strips;
						bool any = false;
						for (int32_t pr = 0; pr < p0_strip_count; ++pr) {
							const size_t idx = static_cast<size_t>(pr);
							if (idx >= lod.strip_count) break;
							const ThreediTriangleStrip &prim = lod.strips[idx];
							info.rest_min_z =
									any ? std::min(info.rest_min_z, prim.min[2])
									    : prim.min[2];
							info.rest_max_z =
									any ? std::max(info.rest_max_z, prim.max[2])
									    : prim.max[2];
							any = true;
						}
					}
				}
				hs = collision_husk_pieces_by_graphic_.emplace(
						piece_key, std::move(info)).first;
				if (piece_m3 != nullptr)
					collision_radius_by_graphic_.emplace(piece_key,
							model_bound_radius_from_3di(*piece_m3));
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id)) {
				const CollisionHuskPieceInfo &info = hs->second;
				if (t->husk_section_count == 0 && info.sections > 0)
					t->husk_section_count = info.sections;
				if (t->husk_section_centers.empty() && !info.centers.empty())
					t->husk_section_centers = info.centers;
				t->husk_rest_min_z = info.rest_min_z;
				t->husk_rest_max_z = info.rest_max_z;
			}
			// The husk model's bound joins the entity bound max [orig:
			// Entity_InitFromModel @ 0x40dc30, the huskModel[5] compare].
			entity_bound = std::max(
					entity_bound, collision_radius_by_graphic_[husk_key]);
		}
		if (entity_bound > 0.0f && e->bound_radius <= 0.0f)
			e->bound_radius = entity_bound + 0.0625f;  // the +0x1000 16.16 pad
		const int32_t occ_id = collision_occlusion_by_graphic_[key];
		// Entity_ClassifyForMinimap's ordinary-Building branch checks the
		// live graphic model's +0xE0 portal/occlusion pointer. The parsed .3di
		// and the collision/occlusion resolver are the portable ownership seam
		// for that otherwise renderer-private fact.
		e->has_minimap_model_marker = occ_id >= 0;
		if (occ_id >= 0 && e->kind == opennova::world::EntityKind::Building) {
			// The def bits the occlusion engine reads: attrib2 bit 6 "weldable"
			// [orig: itemDef+88 >> 6 @ 0x5c5cce], attrib bit 27 recurse-windows
			// [orig: itemDef+84 >> 27 @ 0x5c7456]; the destruction bone-map
			// bytes (+2193/+2194) stay 0 until the destruction system lands
			// (D-COL-2 / D-OCC-9).
			opennova::world::OcclusionWorld::EntityDefBits bits;
			bits.weldable = (p_item_db->get_attrib2(def_id) & (1u << 6)) != 0;
			bits.recurse_windows = (p_item_db->get_attrib(def_id) & (1u << 27)) != 0;
			occlusion_world_.assign_entity(h, occ_id, bits);
		}
	}
	return attached;
}

void NovaSimulation::stamp_seat_spec_turret_limits() {
	if (!world_) return;
	for (opennova::mission::ItemSeatSpec &spec : item_seat_specs_) {
		if (spec.primary_weapon.empty()) continue;
		const int index = world_->weapons.index_of(spec.primary_weapon.c_str());
		const opennova::world::WeaponTableEntry *entry =
				index >= 0 && index <= 0xFF
						? world_->weapons.by_index(static_cast<uint8_t>(index))
						: nullptr;
		if (entry == nullptr) continue;
		spec.turret_yaw_range_bam =
				turret_limit_bam(entry->turret_yaw_range_deg);
		spec.turret_pitch_max_bam =
				turret_limit_bam(entry->turret_pitch_max_deg);
		spec.turret_pitch_min_bam =
				turret_limit_bam(entry->turret_pitch_min_deg);
	}
}

void NovaSimulation::refresh_item_seat_spec(
		opennova::world::Entity &p_entity) {
	std::array<opennova::world::EntityHandle, 10> occupants{};
	for (const opennova::world::Seat &seat : p_entity.seats) {
		if (seat.retail_slot < occupants.size())
			occupants[seat.retail_slot] = seat.occupant;
	}

	// A promoted child (authority or complete-BMS joiner) already owns the exact
	// stored addeweap slot; keep that identity across definition refreshes even
	// when sibling types repeat. A stock streamed 0x0D row carries only child type
	// + parent handle, so it may recover metadata only when that type is unique in
	// the parent's definition.
	// Clear first so a later definition refresh cannot leave stale pose/capability
	// metadata on an existing row.
	const bool preserve_authored_slot = !wire_header_world_ &&
			p_entity.emplacement_pose_metadata_resolved &&
			p_entity.emplacement_slot != 0;
	const uint8_t authored_slot = p_entity.emplacement_slot;
	p_entity.emplacement_pose_metadata_resolved = false;
	p_entity.emplacement_local = {};
	p_entity.emplacement_yaw_offset = 0;
	p_entity.emplacement_bone = 0;
	p_entity.emplacement_kind = 0;
	p_entity.emplacement_slot = 0;
	p_entity.emplacement_attachment_flags = 0;
	p_entity.emplacement_angle_count = 0;
	p_entity.emplacement_down_limit_bam = 0;
	p_entity.emplacement_up_limit_bam = 0;
	p_entity.emplacement_right_limit_bam = 0;
	p_entity.emplacement_left_limit_bam = 0;
	if (world_ && p_entity.emplacement_parent.valid() &&
			p_entity.emplacement_parent_spawn_id != 0) {
		const opennova::world::Entity *parent =
				world_->registry.get(p_entity.emplacement_parent);
		if (parent != nullptr && parent->registry_spawn_id ==
					p_entity.emplacement_parent_spawn_id) {
			const opennova::mission::ItemSeatSpec *parent_spec =
					item_seat_spec_for_type(item_seat_specs_,
							static_cast<uint16_t>(parent->item_id));
			const opennova::mission::ItemEmplacementAttachmentSpec *match = nullptr;
			bool ambiguous = false;
			if (parent_spec != nullptr) {
				for (const opennova::mission::ItemEmplacementAttachmentSpec &attachment :
						parent_spec->emplacement_attachments) {
					if (attachment.child_type_id != p_entity.item_id) continue;
					if (preserve_authored_slot &&
							attachment.stored_slot != authored_slot)
						continue;
					if (match != nullptr) {
						ambiguous = true;
						break;
					}
					match = &attachment;
				}
			}
			if (match != nullptr && !ambiguous) {
				p_entity.emplacement_local = match->anchor.seat_local;
				p_entity.emplacement_yaw_offset = match->anchor.yaw_offset;
				p_entity.emplacement_bone =
						match->anchor_found ? match->anchor.bone_index : 0;
				p_entity.emplacement_kind = static_cast<uint8_t>(match->kind);
				p_entity.emplacement_slot = match->stored_slot;
				p_entity.emplacement_attachment_flags = match->attachment_flags;
				p_entity.emplacement_angle_count = match->angle_count;
				p_entity.emplacement_down_limit_bam = match->down_limit_bam;
				p_entity.emplacement_up_limit_bam = match->up_limit_bam;
				p_entity.emplacement_right_limit_bam = match->right_limit_bam;
				p_entity.emplacement_left_limit_bam = match->left_limit_bam;
				p_entity.emplacement_pose_metadata_resolved = true;
			}
		}
	}

	const opennova::mission::ItemSeatSpec *spec =
			item_seat_spec_for_type(item_seat_specs_,
					static_cast<uint16_t>(p_entity.item_id));
	// The installed table is authoritative. Clearing a type from a later table
	// must also clear stale model metadata on an already-streamed exact row.
	p_entity.emplaced_config_valid = false;
	p_entity.emplaced_config = 0;
	p_entity.armory_points.clear();
	p_entity.primary_weapon.clear();
	p_entity.seats.clear();
	if (spec == nullptr) return;

	p_entity.emplaced_config_valid = spec->mount_config_valid;
	p_entity.emplaced_config = spec->mount_config_valid
			? spec->mount_config : 0;
	p_entity.armory_points = spec->armory_points;
	p_entity.primary_weapon = spec->primary_weapon;
	// Seat specs are the def-derived trait channel: a spec that declares the
	// EWeap primary weapon carries items.def's attrib-0x20 nature. A world
	// running on installed specs without the item database (authored tool and
	// test worlds) stamps the equivalent trait here so the witnessed def gate
	// in resolve_mounted_ammo_slot [orig: @0x5460E0] holds uniformly; a real
	// items.def sweep overwrites this with the authoritative row.
	if (!p_entity.has_item_def && !spec->primary_weapon.empty()) {
		p_entity.has_item_def = true;
		p_entity.item_attrib |= opennova::world::kItemAttribEweap;
	}
	p_entity.seats = spec->seats;
	for (size_t seat_index = 0; seat_index < p_entity.seats.size();
			++seat_index) {
		opennova::world::Seat &seat = p_entity.seats[seat_index];
		seat.occupant = seat.retail_slot < occupants.size()
				? occupants[seat.retail_slot]
				: opennova::world::EntityHandle{};
		if (!seat.occupant.valid() || !world_) continue;
		opennova::world::Entity *occupant =
				world_->registry.get(seat.occupant);
		if (occupant == nullptr || !occupant->mounted ||
				occupant->mount_target != p_entity.handle)
			continue;
		// mount_seat is the dense gameplay-row index, while occupancy survives
		// table refreshes by retail's fixed mountHandles slot. Keep the occupant
		// side synchronized when a later model table changes dense ordering.
		occupant->mount_seat = static_cast<int8_t>(seat_index);
		occupant->mount_type = seat.type;
		occupant->mount_bone = seat.bone_index;
		occupant->mounted_config_valid = p_entity.emplaced_config_valid;
		occupant->mounted_config = p_entity.emplaced_config_valid
				? p_entity.emplaced_config : 0;
	}
}

// The .aip profile-speed table ({name: {"patrol": int, "combat": int}}) the
// shell resolved from the mission's ai_textfile set. Raw authored values; the
// promote-time brain seed applies the witnessed x65536/225 scale (promote.h).
void NovaSimulation::set_ai_profile_speeds(const Dictionary &p_speeds) {
	ai_profile_speeds_.clear();
	const Array keys = p_speeds.keys();
	for (int i = 0; i < keys.size(); ++i) {
		const String name = String(keys[i]).to_lower();
		if (name.is_empty()) continue;
		const Dictionary v = p_speeds[keys[i]];
		opennova::mission::PromoteOptions::AiProfileSpeeds ps;
		ps.profile = name.utf8().get_data();
		if (v.has("patrol"))
			ps.patrol_speed = static_cast<int32_t>(static_cast<int64_t>(v["patrol"]));
		if (v.has("combat"))
			ps.combat_speed = static_cast<int32_t>(static_cast<int64_t>(v["combat"]));
		ai_profile_speeds_.push_back(std::move(ps));
	}
}

void NovaSimulation::set_item_seat_specs(const Array &p_specs) {
	item_seat_specs_.clear();
	mounted_pose_data_by_type_.clear();
	mounted_pose_native_models_.clear();
	for (int64_t i = 0; i < p_specs.size(); ++i) {
		const Variant spec_v = p_specs[i];
		if (spec_v.get_type() != Variant::DICTIONARY) continue;
		const Dictionary spec_d = spec_v;

		opennova::mission::ItemSeatSpec spec;
		spec.type_id = static_cast<int32_t>(spec_d.get("type_id", 0));
		if (spec.type_id == 0) continue;
		const Variant model_data_value =
				spec_d.get("model_data", Variant());
		if (model_data_value.get_type() == Variant::OBJECT) {
			Ref<NovaObjectData> model_data(model_data_value);
			if (model_data.is_valid() && model_data->has_document())
				mounted_pose_data_by_type_[spec.type_id] = model_data;
		}
		// S4 (ADR 0028): the native mounted-pose resolver reads the sim
		// cache's parse of the same graphic (lifetime-stable, unlike the
		// render Ref). Dictionary-only test worlds carry no graphic/index
		// and simply leave the native side unregistered.
		if (sim_models_.has_index() && spec_d.has("graphic")) {
			const std::string graphic_key(
					String(spec_d.get("graphic", String())).utf8().get_data());
			if (!graphic_key.empty()) {
				if (const Threedi3di3 *m3 = sim_models_.model_for(graphic_key))
					mounted_pose_native_models_[spec.type_id] = m3;
			}
		}
		if (spec_d.has("mount_config_valid")) {
			spec.mount_config_valid = static_cast<bool>(spec_d.get("mount_config_valid", false));
			spec.mount_config = spec.mount_config_valid
			                        ? static_cast<int32_t>(spec_d.get("mount_config", 0))
			                        : 0;
		}

		const Variant seats_v = spec_d.get("seats", Array());
		if (seats_v.get_type() != Variant::ARRAY) continue;
		const Array seats_a = seats_v;
		bool retail_slots_used[10] = {};
		int inferred_passenger_slot = 0;
		for (int64_t j = 0; j < seats_a.size(); ++j) {
			const Variant seat_v = seats_a[j];
			if (seat_v.get_type() != Variant::DICTIONARY) continue;
			const Dictionary seat_d = seat_v;

			opennova::world::Seat seat;
			seat.type = seat_type_from_variant(static_cast<int>(seat_d.get("type", 0)));
			if (seat.type == opennova::world::SeatType::None) continue;
			int retail_slot = -1;
			if (seat_d.has("retail_slot")) {
				retail_slot = static_cast<int>(seat_d.get("retail_slot", -1));
			} else {
				// Compatibility for tests/tools that construct seat dictionaries
				// directly. Production extraction supplies the explicit slot.
				switch (seat.type) {
					case opennova::world::SeatType::Passenger:
						while (inferred_passenger_slot < 8 &&
						       retail_slots_used[inferred_passenger_slot])
							++inferred_passenger_slot;
						if (inferred_passenger_slot < 8)
							retail_slot = inferred_passenger_slot++;
						break;
					case opennova::world::SeatType::Controller:
					case opennova::world::SeatType::Driver:
						retail_slot = 8;
						break;
					case opennova::world::SeatType::Gunner:
						retail_slot = 9;
						break;
					default:
						break;
				}
			}
			const bool slot_matches_type =
					(retail_slot >= 0 && retail_slot < 8 &&
					 seat.type == opennova::world::SeatType::Passenger) ||
					(retail_slot == 8 &&
					 opennova::world::is_vehicle_control_seat(seat.type)) ||
					(retail_slot == 9 &&
					 seat.type == opennova::world::SeatType::Gunner);
			if (slot_matches_type && !retail_slots_used[retail_slot]) {
				seat.retail_slot = static_cast<uint8_t>(retail_slot);
				retail_slots_used[retail_slot] = true;
			}
			seat.bone_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("bone_index", 0)), 0, 255));
			seat.pose_index = static_cast<uint8_t>(
			    std::clamp(static_cast<int>(seat_d.get("pose_index", 0)), 0, 30));
			seat.source_name = String(seat_d.get("source_name", String())).utf8().get_data();
			const Vector3 pos = seat_d.get("position", Vector3());
			seat.seat_local = {static_cast<float>(pos.x), static_cast<float>(pos.y),
			                   static_cast<float>(pos.z)};
			seat.yaw_offset = static_cast<int16_t>(
			    std::clamp(static_cast<int>(seat_d.get("yaw_offset", 0)), -32768, 32767));
			spec.seats.push_back(seat);
		}
		const Variant attachments_v =
				spec_d.get("emplacement_attachments", Array());
		if (attachments_v.get_type() == Variant::ARRAY) {
			const Array attachments_a = attachments_v;
			for (int64_t j = 0; j < attachments_a.size(); ++j) {
				const Variant attachment_v = attachments_a[j];
				if (attachment_v.get_type() != Variant::DICTIONARY) continue;
				const Dictionary attachment_d = attachment_v;
				const int full_child_id =
						static_cast<int>(attachment_d.get("item_id", 0));
				const int child_type_id = full_child_id - 100000;
				if (child_type_id <= 0) continue;
				opennova::mission::ItemEmplacementAttachmentSpec attachment;
				attachment.child_type_id =
						static_cast<int32_t>(child_type_id);
				attachment.kind =
						static_cast<opennova::mission::EmplacementAttachmentKind>(
								std::clamp(static_cast<int>(
										attachment_d.get("kind", 0)), 0, 2));
				attachment.stored_slot = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("stored_slot", 0)), 0, 4));
				if (static_cast<bool>(
						attachment_d.get("designated_c", false)))
					attachment.attachment_flags |= 1;
				if (static_cast<bool>(
						attachment_d.get("designated_g", false)))
					attachment.attachment_flags |= 2;
				attachment.anchor_found = static_cast<bool>(
						attachment_d.get("anchor_found", false));
				attachment.anchor.type =
						opennova::world::SeatType::Gunner;
				attachment.anchor.attachment_frame = true;
				attachment.anchor.bone_index = static_cast<uint8_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("bone_index", 0)), 0, 255));
				attachment.anchor.source_name =
						String(attachment_d.get(
								"source_name", String())).utf8().get_data();
				const Vector3 local =
						attachment_d.get("local", Vector3());
				attachment.anchor.seat_local = {
						static_cast<float>(local.x),
						static_cast<float>(local.y),
						static_cast<float>(local.z)};
				attachment.anchor.yaw_offset = static_cast<int16_t>(
						std::clamp(static_cast<int>(
								attachment_d.get("yaw_offset", 0)),
								-32768, 32767));
				attachment.angle_count = static_cast<uint8_t>(
						static_cast<int>(
								attachment_d.get("angle_count", 0)) == 4
								? 4 : 0);
				attachment.down_limit_bam = static_cast<int32_t>(
						attachment_d.get("down_limit_bam", 0));
				attachment.up_limit_bam = static_cast<int32_t>(
						attachment_d.get("up_limit_bam", 0));
				attachment.right_limit_bam = static_cast<int32_t>(
						attachment_d.get("right_limit_bam", 0));
				attachment.left_limit_bam = static_cast<int32_t>(
						attachment_d.get("left_limit_bam", 0));
				spec.emplacement_attachments.push_back(
						std::move(attachment));
			}
		}
		// The attach-label sources: "armory*" userpoint locals (Armory-attrib items
		// only — the host gates on itemdef attrib 0x80000) + the ewep primary_weapon
		// link [orig: @0x4361ee/@0x5a36f5; ItemDef+0x54B].
		const Variant armory_v = spec_d.get("armory_points", Array());
		if (armory_v.get_type() == Variant::ARRAY) {
			const Array armory_a = armory_v;
			for (int64_t j = 0; j < armory_a.size(); ++j) {
				if (armory_a[j].get_type() != Variant::VECTOR3) continue;
				const Vector3 p = armory_a[j];
				spec.armory_points.push_back({static_cast<float>(p.x),
				                              static_cast<float>(p.y),
				                              static_cast<float>(p.z)});
			}
		}
		spec.primary_weapon =
		    String(spec_d.get("primary_weapon", String())).utf8().get_data();
		if (spec.mount_config_valid || !spec.seats.empty() || !spec.armory_points.empty() ||
		    !spec.primary_weapon.empty() || !spec.emplacement_attachments.empty())
			item_seat_specs_.push_back(std::move(spec));
	}
	// Lookup table, ordered for the binary search in item_seat_spec_for_type —
	// a joiner probes it once per present row per frame.
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
			opennova::world::Entity *entity = wire_header_world_
					? wire_world_materializer_.owned(*world_, handle)
					: world_->registry.get(handle);
			if (entity != nullptr)
				refresh_item_seat_spec(*entity);
		}
		// A header-only join may receive its model/seat table after the 0x0D
		// row. Definitions were installed above; now apply the retained fixed
		// mountHandles image without creating synthetic seats.
		if (wire_header_world_ && runtime_ != nullptr)
			(void)wire_world_materializer_.sync(runtime_->state(), *world_);
	}
}
