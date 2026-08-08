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

#include <algorithm>
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

// The mounted-pose resolver (S4, ADR 0028) is native-only: the engine-side
// resolver over the sim's own parse (simassets::resolve_model_mounted_pose) is
// the sole host-authority path. Its model source resolves AT QUERY TIME —
// the spec's graphic through the sim cache (production; survives asset-root
// switches because the cache re-parses under the live index), else the
// installed spec's Ref-kept model_data parse (boot order / test worlds).
bool NovaSimulation::resolve_mounted_pose(
		opennova::world::World &p_world,
		const opennova::world::Entity &p_carrier,
		const opennova::world::Seat &p_seat,
		opennova::world::MountedPose &r_out) {
	return resolve_mounted_pose_native(p_world, p_carrier, p_seat, r_out);
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
	const bool resolved = opennova::simassets::resolve_model_mounted_pose(
			model, p_carrier, p_seat, ctrl_values, time_ms, r_out);
	if (!resolved) ++mounted_native_declines_;
	return resolved;
}

bool NovaSimulation::ensure_collision_instance(
		opennova::world::World &p_world,
		opennova::world::EntityHandle p_entity) {
	if (!world_ || &p_world != world_.get() || collision_item_db_.is_null())
		return false;
	const opennova::world::Entity *entity = p_world.registry.get(p_entity);
	if (entity == nullptr) {
		collision_world_.remove_entity_instance(p_entity);
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
		collision_pose_native_.remove_entity(p_entity);
		collision_resolution_attempted_.erase(attempted);
	}

	// Re-run the idempotent attach sweep against the retained mission caches.
	// It resolves every entity that appeared since the previous sweep, including
	// a player deployed after load, without registering another graphic model.
	resolve_collision_instances(collision_item_db_, nullptr);
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
bool NovaSimulation::build_section_matrices(opennova::world::World &p_world,
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

Dictionary NovaSimulation::debug_native_pose_stats() const {
	Dictionary out;
	out["collision_queries"] = static_cast<int64_t>(collision_native_queries_);
	out["collision_declines"] = static_cast<int64_t>(collision_native_declines_);
	out["mounted_queries"] = static_cast<int64_t>(mounted_native_queries_);
	out["mounted_declines"] = static_cast<int64_t>(mounted_native_declines_);
	out["mounted_graphic_sources"] =
			static_cast<int64_t>(mounted_pose_native_graphics_.size());
	return out;
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
	if (!world_ || p_item_db.is_null()) return 0;
	(void)p_placer; // retained in the bound signature; the render placer no
	                // longer participates (S3b full: sim-cache-only sources).
	// Production installs the sim's own asset source first (ADR 0028); the
	// no-root leg below is the GUT stub seam and dies with S3.
	if (!sim_models_.has_index()) {
		godot::UtilityFunctions::print_verbose(
				"NovaSimulation: no asset root installed — collision/occlusion "
				"extraction has no model source (install set_asset_root first)");
	}
	collision_item_db_ = p_item_db;
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
						// S3 (ADR 0028): the engine-side provider poses this
						// model from the sim's own retained parse. The legacy
						// pose map stays empty in production — the fallback is
						// stub-world-only, so a shadow render-bound copy here
						// could only mask a native decline.
						if (threedi_panm_lod_has_live(*m3, 0))
							collision_pose_native_.register_generic_model(
									model_id, m3);
					}
					opennova::world::OcclusionModel occ;
					if (occlusion_model_from_3di(*m3, occ))
						occlusion_id = occlusion_world_.add_model(std::move(occ));
					bound_radius = model_bound_radius_from_3di(*m3);
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
				if (vm3 != nullptr) {
					const ThreediCollisionModel *col = vm3->collision;
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
			// S3b full: husk models resolve exclusively through the sim cache's
			// parse-once source.
			const Threedi3di3 *first_husk_m3 = nullptr;
			const Threedi3di3 *final_husk_m3 = nullptr;
			if (sim_models_.has_index()) {
				if (!first_husk_name_s.is_empty())
					first_husk_m3 = sim_models_.model_for(
							std::string(first_husk_name_s.utf8().get_data()));
				if (!final_husk_name_s.is_empty())
					final_husk_m3 = sim_models_.model_for(
							std::string(final_husk_name_s.utf8().get_data()));
			}
			if (opennova::world::ItemDeathTraits *t =
						world_->item_death_traits.get_mutable(e->item_id))
				t->husk_model_loaded =
						first_husk_m3 != nullptr || final_husk_m3 != nullptr;
						const std::string husk_key(husk_name_s.utf8().get_data());
			const Threedi3di3 *husk_m3 = first_husk_name_s.is_empty()
					? final_husk_m3
					: first_husk_m3;
			auto hit = collision_model_by_graphic_.find(husk_key);
			if (hit == collision_model_by_graphic_.end()) {
				int32_t husk_model_id = -1;
				if (husk_m3 != nullptr) {
					opennova::world::CollisionModel hmodel;
					const bool husk_spheres =
							opennova::simassets::model_has_collision(*husk_m3);
					if (collision_model_from_3di(
							husk_m3->collision, hmodel, husk_spheres)) {
						husk_model_id = collision_world_.add_model(std::move(hmodel));
						// S3b full: the provider poses husks from the sim
						// cache's lifetime-stable parse (the provider drops
						// registrations whenever the index switches).
						if (threedi_panm_lod_has_live(*husk_m3, 0))
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
				const Threedi3di3 *piece_m3 = final_husk_name_s.is_empty()
						? first_husk_m3
						: final_husk_m3;
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
	const bool preserve_authored_slot = !joiner_bridge_.wire_header_world() &&
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

// (set_ai_profile_speeds retired with S9b: the .aip resolve is native in
// mission::resolve_ai_profile_speeds, driven by boot_mission.)

void NovaSimulation::finalize_installed_seat_specs() {
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
void NovaSimulation::install_native_seat_specs(
		const Ref<NovaItemDatabase> &p_item_db,
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

bool NovaSimulation::install_seat_specs_for_type_ids(
		const Ref<NovaItemDatabase> &p_item_db,
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
