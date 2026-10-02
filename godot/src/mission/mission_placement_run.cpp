#include "mission/mission_placement_run.h"

#include <godot_cpp/classes/geometry_instance3d.hpp>
#include <godot_cpp/classes/multi_mesh.hpp>
#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/aabb.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_int64_array.hpp>

#include <algorithm>

#include <runtime/renderer/object_lod.h>
#include <runtime/renderer/render_order.h>

#include "env/water.h"
#include "mission/mission_data.h"
#include "mission/mission_object_placer_keys.h"
#include "mission/static_population_instance.h"
#include "mission/static_source_convert.h"
#include "render/frame_fx.h"
#include "render/object_lod_frame.h"
#include "util/string_convert.h"

namespace godot {

namespace {

// A static group's key: its graphic under one reflection policy. One graphic may be authored
// both with and without the BMS Reflective attribute (CP01 does exactly that); a single
// MultiMesh layer cannot express those different mirror policies.
String static_group_key(const String &p_graphic, bool p_mirror_reflected) {
	return p_graphic + String(p_mirror_reflected ? "::mirror" : "::no_mirror");
}

uint64_t now_usec() {
	return Time::get_singleton()->get_ticks_usec();
}

} // namespace

// --- MissionPlacementRun -------------------------------------------------------------------------

void MissionPlacementRun::_bind_methods() {
	ClassDB::bind_method(D_METHOD("step"), &MissionPlacementRun::step);
	ClassDB::bind_method(D_METHOD("is_done"), &MissionPlacementRun::is_done);
	ClassDB::bind_method(D_METHOD("is_cancelled"), &MissionPlacementRun::is_cancelled);
	ClassDB::bind_method(D_METHOD("get_step_count"), &MissionPlacementRun::get_step_count);
	ClassDB::bind_method(D_METHOD("get_steps_done"), &MissionPlacementRun::get_steps_done);
	ClassDB::bind_method(D_METHOD("get_step_label"), &MissionPlacementRun::get_step_label);
	ClassDB::bind_method(D_METHOD("get_stats"), &MissionPlacementRun::get_stats);
	ClassDB::bind_method(D_METHOD("get_generation"), &MissionPlacementRun::get_generation);
	BIND_ENUM_CONSTANT(STEP_MORE);
	BIND_ENUM_CONSTANT(STEP_DONE);
}

int MissionPlacementRun::bucket_units_() const {
	return int((rows.size() + size_t(kBucketRowsPerUnit) - 1) / size_t(kBucketRowsPerUnit));
}

int MissionPlacementRun::animated_units_() const {
	return int((animated.size() + kAnimatedPerUnit - 1) / kAnimatedPerUnit);
}

MissionPlacementRun::Unit MissionPlacementRun::next_unit_() const {
	if (next_row < rows.size()) return Unit::Bucket;
	if (next_group < static_order.size()) return Unit::Statics;
	if (next_animated < animated.size()) return Unit::Models;
	return Unit::Finish;
}

int MissionPlacementRun::get_step_count() const {
	if (empty_) return done_ ? 1 : 1;
	// The static and animated units are known once the bucketing ends.
	const bool bucketed = next_row >= rows.size();
	return bucket_units_() + (bucketed ? int(static_order.size()) + animated_units_() : 0) + 1;
}

String MissionPlacementRun::get_step_label() const {
	if (done_) return String();
	switch (next_unit_()) {
	case Unit::Bucket: return "bucket";
	case Unit::Statics: return "statics";
	case Unit::Models: return "models";
	case Unit::Finish: return "finish";
	}
	return String();
}

MissionPlacementRun::Step MissionPlacementRun::step() {
	if (done_) return STEP_DONE;
	// A run begun after this one on the placer cancels it: nothing more of it runs.
	if (placer_.is_null() || placer_->placement_generation_ != generation_) {
		cancelled_ = true;
		done_ = true;
		return STEP_DONE;
	}
	if (empty_) {
		placer_->_place_finish(*this);
		++done_units_;
		done_ = true;
		return STEP_DONE;
	}
	const uint64_t start = now_usec();
	switch (next_unit_()) {
	case Unit::Bucket: {
		const size_t first = next_row;
		placer_->_place_bucket(*this, first);
		bucket_usec += now_usec() - start;
		break;
	}
	case Unit::Statics:
		placer_->_place_static_group(*this, next_group++);
		static_usec += now_usec() - start;
		break;
	case Unit::Models: {
		const int first = next_animated;
		placer_->_place_animated(*this, first);
		animated_usec += now_usec() - start;
		break;
	}
	case Unit::Finish:
		placer_->_place_finish(*this);
		++done_units_;
		done_ = true;
		return STEP_DONE;
	}
	++done_units_;
	return STEP_MORE;
}

// --- the placer's units --------------------------------------------------------------------------

Ref<MissionPlacementRun> MissionObjectPlacer::begin_place_rows(const std::vector<PlacementRow> &p_rows,
		Node3D *p_parent, const Dictionary &p_options) {
	_check_epoch();
	Ref<MissionPlacementRun> run;
	run.instantiate();
	run->placer_ = Ref<MissionObjectPlacer>(this);
	run->generation_ = ++placement_generation_;
	run->stats_.instantiate();
	static_sources_.clear();
	static_source_assets_.clear();
	static_lod_profiles_.clear();
	static_lod_instances_.clear();
	static_populations_.clear();
	static_population_by_node_.clear();
	static_lod_switches_ = 0;
	placed_models_ = TypedArray<ObjectModel>();
	if (p_parent == nullptr || resource_root_.is_null()) {
		// Nothing to place: the one unit is the finish, an empty census.
		run->empty_ = true;
		return run;
	}
	_ensure_item_db();
	run->rows = p_rows;
	// Optional per-model load-progress pulse (the game shell's loading screen), mirroring the
	// original's per-model loading-screen presents (witness: placement_traits.h ledger,
	// Game_StartMission's in-loop presents).
	run->progress = p_options.get("progress", Callable());
	// Optional entity-kind exclusion: the joiner places the mission minus organics; players and
	// streamed AI render wire-direct while items/buildings/markers become ordinary placed nodes.
	run->skip_kinds = p_options.get("skip_kinds", Array());
	run->container = _ensure_container(p_parent);
	return run;
}

// Every static population goes under one StaticPopulations node, minted with the first: the
// shell's per-frame walks over the container's children (the EffectWorld light select, the
// item-effect attach) visit the entity models and one holder, never a population per graphic x
// level x bin (00TRa places 835 of them beside 77 models).
Node3D *MissionObjectPlacer::_place_populations_parent(MissionPlacementRun &p_run) {
	if (p_run.populations == nullptr) {
		p_run.populations = memnew(Node3D);
		p_run.populations->set_name(kStaticPopulationsName);
		p_run.container->add_child(p_run.populations);
	}
	return p_run.populations;
}

// Bucket entities by graphic and retail reflection population, then split static vs animated.
void MissionObjectPlacer::_place_bucket(MissionPlacementRun &p_run, size_t p_first) {
	using StaticGroup = MissionPlacementRun::StaticGroup;
	const size_t last = std::min(p_run.rows.size(), p_first + size_t(MissionPlacementRun::kBucketRowsPerUnit));
	for (size_t r = p_first; r < last; ++r) {
		const PlacementRow &entity = p_run.rows[r];
		const int kind = entity.kind;
		if (!p_run.skip_kinds.is_empty() && p_run.skip_kinds.has(kind)) {
			continue;
		}
		if (kind == MissionData::KIND_MARKER) {
			++p_run.markers;
			continue;
		}
		const int item_id = entity.item_id;
		const String graphic = _graphic_for(item_id);
		if (graphic.is_empty()) {
			++p_run.unresolved;
			continue;
		}
		const Transform3D xform = _entity_transform_for_item(
				entity.position, entity.rotation_deg, item_id);
		if (_needs_individual_node(item_id) ||
				_graphic_needs_live_panm(graphic)) {
			Dictionary a;
			a["graphic"] = graphic;
			a["item_id"] = item_id;
			a["xform"] = xform;
			a["kind"] = kind;
			a["index"] = entity.index;
			a["bms_id"] = entity.bms_id;
			a["group"] = entity.group;
			a["team"] = entity.team;
			a["ai_flags"] = static_cast<int>(entity.ai_flags);
			a["position"] = entity.position;
			p_run.animated.push_back(a);
			continue;
		}
		const bool mirror_reflected = _placement_is_mirror_reflected(
				entity.ai_flags, item_id);
		const String group_key = static_group_key(graphic, mirror_reflected);
		StaticGroup *group = p_run.static_groups.getptr(group_key);
		if (group == nullptr) {
			p_run.static_groups[group_key] = StaticGroup();
			group = p_run.static_groups.getptr(group_key);
			group->graphic = graphic;
			group->mirror_reflected = mirror_reflected;
			p_run.static_order.push_back(group_key);
		}
		group->xforms.push_back(xform);
		group->shadow_slots.push_back(item_casts_static_terrain_shadow(static_cast<MissionData::EntityKind>(kind),
				entity.ai_flags,
				item_db_->get_attrib(item_id), item_db_->get_attrib2(item_id)));
		group->bms_ids.push_back(entity.bms_id);
		group->item_ids.push_back(item_id);
		group->kinds.push_back(kind);
		group->entity_indices.push_back(entity.index);
		group->teams.push_back(entity.team);
		group->entity_attribs.push_back(entity.ai_flags);
		group->attrib2_values.push_back(item_db_->get_attrib2(item_id));
		Dictionary source;
		source["kind"] = kind;
		source["entity_index"] = entity.index;
		source["bms_id"] = entity.bms_id;
		source["item_id"] = item_id;
		source["world_transform"] = xform;
		group->effect_sources.push_back(source);
	}
	p_run.next_row = last;
}

// One static group placed: opaque and alpha-tested statics are divided into the same
// 512-world-unit cells as terrain. Retail's blended strips remain global because their
// independently chosen Q1/Q2 order must not inherit spatial batch centers. Every authored RLOD is
// emitted as its own population over the same slot list; a population carries rows only for the
// slots at its level (level 0 until the first update_static_lods evaluates the camera), so the
// coarser levels start empty and hidden.
void MissionObjectPlacer::_place_static_group(MissionPlacementRun &p_run, int p_group) {
	using StaticGroup = MissionPlacementRun::StaticGroup;
	if (p_group < 0 || p_group >= p_run.static_order.size()) {
		return;
	}
	if (p_run.progress.is_valid()) {
		p_run.progress.call();
	}
	const String group_key = p_run.static_order[p_group];
	StaticGroup &group = p_run.static_groups[group_key];
	Node3D *container = p_run.container;
	const String graphic = group.graphic;
	const int instance_count = group.xforms.size();
	const uint32_t batch_world_layer =
			group.mirror_reflected ? uint32_t(Water::VISUAL_LAYER_WORLD)
								   : uint32_t(Water::VISUAL_LAYER_WORLD_NO_MIRROR);
	const bool graphic_has_split_policy =
			p_run.static_groups.has(static_group_key(graphic, !group.mirror_reflected));
	const String policy_suffix =
			graphic_has_split_policy
			? (group.mirror_reflected ? "_Mirror" : "_NoMirror")
			: String();
	const Vector<StaticBatch> batches = _get_static_batches(graphic, container);
	if (batches.is_empty()) {
		p_run.unresolved += instance_count;
		return;
	}
	if (!p_run.resolved_graphics.has(graphic)) {
		p_run.resolved_graphics.push_back(graphic);
		++p_run.graphics;
	}
	// The graphic's authored RLOD profile rides beside its batches; one
	// row per graphic serves both reflection policy groups.
	int profile_row = -1;
	if (const int *existing = p_run.profile_rows.getptr(graphic)) {
		profile_row = *existing;
	} else {
		static_lod_profiles_.push_back(_static_lod_profile_for(graphic));
		profile_row = static_lod_profiles_.size() - 1;
		p_run.profile_rows[graphic] = profile_row;
	}
	Ref<ObjectData> shadow_data;
	if (const Ref<ObjectData> *resolved =
				object_data_cache_.getptr(graphic)) {
		shadow_data = *resolved;
	}
	for (int i = 0; i < group.xforms.size(); ++i) {
		_record_static_terrain_shadow_source(
				i < group.kinds.size() ? group.kinds[i] : -1,
				i < group.entity_indices.size()
						? group.entity_indices[i]
						: -1,
				i < group.bms_ids.size() ? group.bms_ids[i] : 0,
				i < group.teams.size() ? group.teams[i] : 0,
				i < group.entity_attribs.size()
						? group.entity_attribs[i]
						: 0,
				i < group.item_ids.size() ? group.item_ids[i] : 0,
				graphic, group.xforms[i], shadow_data);
	}
	Vector<int> effect_source_rows;
	effect_source_rows.resize(instance_count);
	for (int i = 0; i < effect_source_rows.size(); ++i) {
		effect_source_rows.write[i] = -1;
	}
	for (int i = 0; i < group.effect_sources.size(); ++i) {
		Dictionary source = group.effect_sources[i];
		effect_source_rows.write[i] = _append_static_item_effect_source(
				int(source.get("kind", -1)),
				int(source.get("entity_index", -1)),
				int(source.get("bms_id", 0)),
				int(source.get("item_id", 0)), graphic,
				source.get("world_transform", Transform3D()));
	}
	// One atlas row per retained entity/ROBJ. Multiple material surfaces
	// under that ROBJ share the row, while its query AABB is the exact merge
	// of those surfaces (every level's) in model-rest space transformed by
	// the entity.
	HashMap<int, AABB> local_robj_bounds;
	Vector<int> robj_order;
	for (const StaticBatch &batch : batches) {
		if (batch.mesh.is_null()) {
			continue;
		}
		const AABB surface_bounds = batch.offset.xform(batch.mesh->get_aabb());
		AABB *merged = local_robj_bounds.getptr(batch.robj_index);
		if (merged == nullptr) {
			local_robj_bounds[batch.robj_index] = surface_bounds;
			robj_order.push_back(batch.robj_index);
		} else {
			*merged = merged->merge(surface_bounds);
		}
	}
	HashMap<uint64_t, int> light_draw_rows;
	for (int i = 0; i < instance_count; ++i) {
		for (const int robj_index : robj_order) {
			const AABB *local_bounds = local_robj_bounds.getptr(robj_index);
			if (local_bounds == nullptr) {
				continue;
			}
			const int row = _append_static_light_draw_source(
					i < effect_source_rows.size() ? effect_source_rows[i] : -1,
					i < group.kinds.size() ? group.kinds[i] : -1,
					i < group.entity_indices.size()
							? group.entity_indices[i] : -1,
					i < group.bms_ids.size() ? group.bms_ids[i] : 0,
					i < group.item_ids.size() ? group.item_ids[i] : 0,
					robj_index, group.xforms[i].xform(*local_bounds));
			light_draw_rows[static_light_draw_key(i, robj_index)] = row;
		}
	}
	// One retained instance per entity: scale the native collision sphere
	// once, place its offset center, and retain every population it occupies.
	Vector<int> lod_rows;
	lod_rows.resize(instance_count);
	{
		const StaticLodProfile &profile = static_lod_profiles_[profile_row];
		for (int i = 0; i < instance_count; ++i) {
			StaticLodInstance retained;
			retained.profile = profile_row;
			retained.bms_id = i < group.bms_ids.size() ? group.bms_ids[i] : 0;
			retained.xform = group.xforms[i];
			const int item_id = i < group.item_ids.size() ? group.item_ids[i] : 0;
			const int32_t scale_q16 = _item_model_scale_q16(item_id);
			// An eweap powerup takes the zero-centered form its entity
			// init stamps; every other item the midpoint form.
			const bool zero_center = _item_projection_zero_center(item_id) &&
					profile.zero_center_projection_sphere.valid;
			const auto sphere = opennova::renderer::scale_object_projection_sphere_q16(
					zero_center ? profile.zero_center_projection_sphere
								: profile.projection_sphere,
					scale_q16);
			retained.origin = ObjectLodFrame::projection_center(
					group.xforms[i], sphere, scale_q16);
			retained.radius_q16 = sphere.radius_q16;
			retained.local_projection_sphere = sphere;
			retained.entity_scale_q16 = scale_q16;
			static_lod_instances_.push_back(retained);
			lod_rows.write[i] = static_lod_instances_.size() - 1;
		}
	}

	struct StaticBin {
		int x = 0;
		int z = 0;
		Vector<int> slots;
	};
	HashMap<uint64_t, int> bin_rows;
	Vector<StaticBin> bins;
	for (int i = 0; i < instance_count; ++i) {
		const int bin_x = static_batch_bin_coord(group.xforms[i].origin.x);
		const int bin_z = static_batch_bin_coord(group.xforms[i].origin.z);
		const uint64_t bin_key = static_batch_bin_key(bin_x, bin_z);
		int *bin_row = bin_rows.getptr(bin_key);
		if (bin_row == nullptr) {
			StaticBin bin;
			bin.x = bin_x;
			bin.z = bin_z;
			bins.push_back(bin);
			bin_rows[bin_key] = bins.size() - 1;
			bin_row = bin_rows.getptr(bin_key);
		}
		bins.write[*bin_row].slots.push_back(i);

		const int bms_id = i < group.bms_ids.size() ? group.bms_ids[i] : 0;
		if (bms_id != 0) {
			opennova::mission::StaticInstance inst;
			inst.bms_id = bms_id;
			inst.graphic = opennova::to_std(graphic);
			inst.batch_key = opennova::to_std(group_key);
			inst.index = i;
			inst.xform = to_static_source_transform(group.xforms[i]);
			inst.casts_static_shadow =
					i < group.shadow_slots.size() && group.shadow_slots[i];
			inst.mirror_reflected = group.mirror_reflected;
			inst.lod_instance = lod_rows[i];
			static_sources_.register_instance(std::move(inst), false);
		}
	}
	Vector<int> global_slots;
	global_slots.resize(instance_count);
	for (int i = 0; i < instance_count; ++i) {
		global_slots.write[i] = i;
	}

	const auto tag_static_shadow_source = [&](StaticPopulationInstance *p_source,
			const Vector<int> &p_slots) {
		PackedInt32Array shadow_bms_ids;
		PackedInt32Array shadow_item_ids;
		PackedInt64Array shadow_attrib2;
		PackedByteArray shadow_slots;
		for (const int slot : p_slots) {
			shadow_bms_ids.push_back(
					slot < group.bms_ids.size() ? group.bms_ids[slot] : 0);
			shadow_item_ids.push_back(
					slot < group.item_ids.size() ? group.item_ids[slot] : 0);
			shadow_attrib2.push_back(slot < group.attrib2_values.size()
							? int64_t(group.attrib2_values[slot])
							: int64_t(0));
			shadow_slots.push_back(
					slot < group.shadow_slots.size() && group.shadow_slots[slot]
							? 1
							: 0);
		}
		p_source->set_shadow_tagged(true);
		p_source->set_slot_bms_ids(shadow_bms_ids);
		p_source->set_slot_item_ids(shadow_item_ids);
		p_source->set_slot_attrib2(shadow_attrib2);
		p_source->set_slot_casts_shadow(shadow_slots);
		p_source->set_graphic(graphic);
	};
	// One population over a slot list: capacity = the slot count, one
	// binding per slot joining its retained instance (so a level switch
	// or a destruction carve moves exactly the rows it owns), and the
	// rows of the slots live at this level appended dense from row 0.
	// Returns the population index.
	const auto build_population = [&](const Ref<MultiMesh> &p_mm,
			const Vector<int> &p_slots, const StaticBatch &p_batch,
			bool p_shadow_only) -> int {
		StaticPopulation population;
		population.multimesh = p_mm;
		population.lod_index = p_batch.lod_index;
		population.shadow_only = p_shadow_only;
		population.custom_data = !p_shadow_only;
		population.row_instance.resize(p_slots.size());
		population.row_binding.resize(p_slots.size());
		population.row_slot.resize(p_slots.size());
		static_populations_.push_back(population);
		const int population_index = static_populations_.size() - 1;
		for (int local_index = 0; local_index < p_slots.size(); ++local_index) {
			const int slot = p_slots[local_index];
			if (slot < 0 || slot >= lod_rows.size()) {
				continue;
			}
			StaticLodBinding binding;
			binding.population = population_index;
			binding.slot = local_index;
			binding.lod_index = p_batch.lod_index;
			binding.offset = p_batch.offset;
			binding.live_xform = group.xforms[slot] * p_batch.offset;
			const int *row = light_draw_rows.getptr(
					static_light_draw_key(slot, p_batch.robj_index));
			binding.custom_data = Color(
					row != nullptr ? static_cast<float>(*row + 1) : 0.0f, 0.0f,
					0.0f, 0.0f);
			binding.shadow_only = p_shadow_only;
			binding.casts =
					slot < group.shadow_slots.size() && group.shadow_slots[slot];
			const int instance_row = lod_rows[slot];
			StaticLodInstance &retained = static_lod_instances_.write[instance_row];
			retained.bindings.push_back(binding);
			StaticPopulation &slots = static_populations_.write[population_index];
			slots.slot_instance.push_back(instance_row);
			slots.slot_binding.push_back(retained.bindings.size() - 1);
			if (_static_slot_live(binding, retained.active_lod, retained.inset_lod,
						retained.view_split)) {
				_static_population_append(population_index, instance_row,
						retained.bindings.size() - 1);
			}
		}
		return population_index;
	};
	// Retail draws every selected entity; hidden here means the population
	// has no live row this frame and the cull can skip it outright.
	const auto attach_population = [&](int p_population,
			StaticPopulationInstance *p_mmi, bool p_shadow_tagged) {
		StaticPopulation &population = static_populations_.write[p_population];
		population.instance_node = p_mmi->get_instance_id();
		population.shadow_tagged = p_shadow_tagged;
		static_population_by_node_[population.instance_node] = p_population;
		p_mmi->set_visible(population.live > 0);
		if (p_shadow_tagged) {
			p_mmi->set_row_slots(_static_population_row_slots(population));
		}
	};

	const auto emit_population = [&](const StaticBatch &p_batch,
			const Vector<int> &p_slots, bool p_global, int p_bin_x,
			int p_bin_z) {
		if (p_slots.is_empty() || p_batch.mesh.is_null()) {
			return;
		}
		bool has_static_shadow = false;
		bool all_static_shadow = true;
		for (const int slot : p_slots) {
			const bool casts = slot >= 0 && slot < group.shadow_slots.size() &&
					group.shadow_slots[slot];
			has_static_shadow = has_static_shadow || casts;
			all_static_shadow = all_static_shadow && casts;
		}

		Ref<MultiMesh> mm;
		mm.instantiate();
		mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		mm->set_use_custom_data(true);
		mm->set_mesh(p_batch.mesh);
		mm->set_instance_count(p_slots.size());
		mm->set_visible_instance_count(0);
		// The population's bounds cover every slot's live transform so a
		// later level switch never draws outside the advertised AABB.
		AABB population_bounds;
		bool has_population_bounds = false;
		for (const int slot : p_slots) {
			const Transform3D surface_xform = group.xforms[slot] * p_batch.offset;
			const AABB surface_bounds =
					surface_xform.xform(p_batch.mesh->get_aabb());
			population_bounds = has_population_bounds
					? population_bounds.merge(surface_bounds)
					: surface_bounds;
			has_population_bounds = true;
		}
		const int population_index =
				build_population(mm, p_slots, p_batch, false);

		StaticPopulationInstance *mmi = memnew(StaticPopulationInstance);
		mmi->set_multimesh(mm);
		if (has_population_bounds) {
			mmi->set_custom_aabb(population_bounds);
		}
		if (all_static_shadow && !p_batch.auxiliary_draw) {
			mmi->set_layer_mask(batch_world_layer |
					Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
			mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_ON);
		} else {
			mmi->set_layer_mask(batch_world_layer);
			mmi->set_cast_shadows_setting(
					GeometryInstance3D::SHADOW_CASTING_SETTING_OFF);
		}
		if (p_batch.material.is_valid()) {
			mmi->set_material_override(p_batch.material);
		}
		mmi->set_population_kind(p_global
						? StaticPopulationInstance::POPULATION_GLOBAL
						: StaticPopulationInstance::POPULATION_BIN);
		mmi->set_lod_index(p_batch.lod_index);
		if (!p_global) {
			mmi->set_bin_x(p_bin_x);
			mmi->set_bin_z(p_bin_z);
		}
		const bool legacy_name = p_global || bins.size() == 1;
		mmi->set_name(legacy_name ? vformat("Batch_%s%s_%d", graphic,
											policy_suffix, p_batch.submesh)
								  : vformat("Batch_%s%s_BinX%d_Z%d_%d", graphic,
											policy_suffix, p_bin_x, p_bin_z,
											p_batch.submesh));
		if (!p_batch.auxiliary_draw) {
			tag_static_shadow_source(mmi, p_slots);
		}
		attach_population(population_index, mmi, !p_batch.auxiliary_draw);
		_place_populations_parent(p_run)->add_child(mmi);
		if (!p_batch.auxiliary_draw) {
			FrameFx::register_q3_object_source(mmi, p_batch.material);
		}
		++p_run.batch_count;
		if (p_batch.lod_index > 0) {
			++p_run.lod_population_count;
		}
		if (p_global) {
			++p_run.global_batch_count;
		} else {
			++p_run.binned_batch_count;
			p_run.occupied_static_bins[static_batch_bin_key(p_bin_x, p_bin_z)] = true;
		}

		if (!has_static_shadow || all_static_shadow || p_batch.auxiliary_draw) {
			return;
		}
		// The filtered shadow twin: rows only for the slots that cast.
		Ref<MultiMesh> shadow_mm;
		shadow_mm.instantiate();
		shadow_mm->set_transform_format(MultiMesh::TRANSFORM_3D);
		shadow_mm->set_mesh(p_batch.mesh);
		shadow_mm->set_instance_count(p_slots.size());
		shadow_mm->set_visible_instance_count(0);
		AABB shadow_bounds;
		bool has_shadow_bounds = false;
		for (const int slot : p_slots) {
			const bool casts =
					slot < group.shadow_slots.size() && group.shadow_slots[slot];
			if (!casts) {
				continue;
			}
			const AABB surface_bounds = (group.xforms[slot] * p_batch.offset)
												.xform(p_batch.mesh->get_aabb());
			shadow_bounds = has_shadow_bounds
					? shadow_bounds.merge(surface_bounds)
					: surface_bounds;
			has_shadow_bounds = true;
		}
		const int shadow_population =
				build_population(shadow_mm, p_slots, p_batch, true);
		StaticPopulationInstance *shadow_mmi = memnew(StaticPopulationInstance);
		shadow_mmi->set_multimesh(shadow_mm);
		if (has_shadow_bounds) {
			shadow_mmi->set_custom_aabb(shadow_bounds);
		}
		shadow_mmi->set_layer_mask(Water::VISUAL_LAYER_STATIC_SHADOW_CASTER);
		shadow_mmi->set_cast_shadows_setting(
				GeometryInstance3D::SHADOW_CASTING_SETTING_SHADOWS_ONLY);
		if (p_batch.material.is_valid()) {
			shadow_mmi->set_material_override(p_batch.material);
		}
		shadow_mmi->set_population_kind(p_global
						? StaticPopulationInstance::POPULATION_GLOBAL
						: StaticPopulationInstance::POPULATION_BIN);
		shadow_mmi->set_lod_index(p_batch.lod_index);
		if (!p_global) {
			shadow_mmi->set_bin_x(p_bin_x);
			shadow_mmi->set_bin_z(p_bin_z);
		}
		shadow_mmi->set_name(legacy_name
						? vformat("StaticShadow_%s%s_%d", graphic,
								  policy_suffix, p_batch.submesh)
						: vformat("StaticShadow_%s%s_BinX%d_Z%d_%d",
								  graphic, policy_suffix, p_bin_x,
								  p_bin_z, p_batch.submesh));
		tag_static_shadow_source(shadow_mmi, p_slots);
		attach_population(shadow_population, shadow_mmi, true);
		_place_populations_parent(p_run)->add_child(shadow_mmi);
		++p_run.shadow_batch_count;
	};
	for (const StaticBatch &batch : batches) {
		if (batch.blended_draw) {
			emit_population(batch, global_slots, true, 0, 0);
			continue;
		}
		for (const StaticBin &bin : bins) {
			emit_population(batch, bin.slots, false, bin.x, bin.z);
		}
	}
	p_run.batched += instance_count;
	p_run.placed += instance_count;
}

// The animated models from `p_first`, kAnimatedPerUnit of them: an individual ObjectModel per entity.
void MissionObjectPlacer::_place_animated(MissionPlacementRun &p_run, int p_first) {
	Node3D *container = p_run.container;
	const int last = std::min(int(p_run.animated.size()), p_first + MissionPlacementRun::kAnimatedPerUnit);
	for (int a_index = p_first; a_index < last; ++a_index) {
		const Dictionary a = p_run.animated[a_index];
		if (p_run.progress.is_valid()) {
			p_run.progress.call();
		}
		const String graphic = a.get("graphic", String());
		const Ref<ObjectData> data = _load_object_data(graphic);
		if (data.is_null()) {
			++p_run.unresolved;
			continue;
		}
		const int item_id = int(a.get("item_id", 0));
		const int kind = int(a.get("kind", -1));
		ObjectModel *model = memnew(ObjectModel);
		model->set_panm_clock(panm_clock_);
		model->set_name(vformat("Anim_%s_%d", graphic, p_run.animated_count));
		model->set_graphic_name(graphic);
		model->set_mirror_reflected(_placement_is_mirror_reflected(
				uint32_t(a.get("ai_flags", 0)), item_id));
		_configure_item_scale(model, item_id);
		// Render the model origin at the entity's stored position directly:
		// the engine bakes the Ground userpoint into the stored position at
		// author-time (place / terrain-drag), not at render (witness:
		// placement_traits.h ledger, author-time Ground bake).
		model->set_transform(a.get("xform", Transform3D()));
		container->add_child(model);
		_configure_item_shadow(model, item_id);
		_configure_item_lighting(model, item_id);
		// Load the entity's body-animation set (.adm) BEFORE the data:
		// setting it first is a no-op rebuild, so set_object_data below does
		// the ONE skeletal-keyed mesh build.
		_apply_skeletal_anim(model, item_id, data->get_bone_origins(),
				data->get_bone_parents());
		// The def names the AI muzzle: items.def launchups_closeattack is
		// the launch userpoint on this item's graphic
		// (world-wac-ai-re §21.2).
		model->set_muzzle_point_name(item_db_->get_launchups_closeattack(item_id));
		// Mission-world models retain every authored RLOD and select by the
		// retail projected-radius rule. Closed building OOBJ records also become
		// Godot occluders; portal/window/open records stay with the section pass.
		model->set_authored_lod_enabled(true);
		model->set_authored_occluders_enabled(kind == MissionData::KIND_BUILDING &&
				_has_occlusion_records(item_id));
		// A building draws in the mirror's building pass, every other placed
		// entity in its first entity wave (runtime/environment/water_mirror.h);
		// a building is a Building-type def, so a pool-2 decoration is an
		// entity here (mission::placed_record_is_building).
		model->set_water_mirror_clip_wave(
				opennova::mission::placed_record_is_building(kind, item_db_->has_item(item_id),
						item_db_->get_item_type(item_id)) ?
						opennova::env::MirrorClipWave::kSectorModel :
						opennova::env::MirrorClipWave::kEntity);
		// Drive the build explicitly (not via _ready) so it is independent
		// of when place() runs relative to the main loop.
		model->set_object_data(data);
		if (model->get_authored_occluder_count() > 0) {
			++p_run.authored_occluder_models;
		}
		if (item_casts_static_terrain_shadow(static_cast<MissionData::EntityKind>(kind),
					uint32_t(a.get("ai_flags", 0)),
					item_db_->get_attrib(item_id),
					item_db_->get_attrib2(item_id))) {
			_add_individual_static_shadow_siblings(model, graphic,
					Transform3D(), vformat("live%d", p_run.animated_count));
		}
		// Tag identity on the node in BOTH runtime + editor so EntityIndex
		// can resolve SSN/group/zone event-action targets back to this live
		// model.
		Ref<EntityRef> ref;
		ref.instantiate();
		ref->set_kind(kind);
		ref->set_index(int(a.get("index", -1)));
		ref->set_bms_id(int(a.get("bms_id", 0)));
		ref->set_group(int(a.get("group", -1)));
		ref->set_team(int(a.get("team", -1)));
		ref->set_position(a.get("position", Vector3()));
		ref->set_item_id(item_id);
		model->set_thermal_entity_wave(opennova::renderer::entity_uses_thermal_wave(
				item_db_->get_item_type(item_id)));
		ref->set_graphic(graphic);
		ref->set_attrib2(int64_t(item_db_->get_attrib2(item_id)));
		model->set_entity_ref(ref);
		placed_models_.push_back(model);
		_record_static_terrain_shadow_source(kind,
				int(a.get("index", -1)), int(a.get("bms_id", 0)),
				int(a.get("team", 0)),
				uint32_t(a.get("ai_flags", 0)), item_id, graphic,
				a.get("xform", Transform3D()), data);
		++p_run.animated_count;
		++p_run.placed;
	}
	p_run.next_animated = last;
}

// The census.
void MissionObjectPlacer::_place_finish(MissionPlacementRun &p_run) {
	Ref<MissionPlacementStats> &stats = p_run.stats_;
	if (p_run.empty_) {
		return;
	}
	stats->set_markers(p_run.markers);
	stats->set_span_bucket_entities_usec(p_run.bucket_usec);
	stats->set_span_static_batches_usec(p_run.static_usec);
	stats->set_span_animated_models_usec(p_run.animated_usec);
	stats->set_placed(p_run.placed);
	stats->set_batched(p_run.batched);
	stats->set_animated(p_run.animated_count);
	stats->set_unresolved(p_run.unresolved);
	stats->set_graphics(p_run.graphics);
	stats->set_batches(p_run.batch_count);
	stats->set_static_bins(static_cast<int>(p_run.occupied_static_bins.size()));
	stats->set_static_binned_batches(p_run.binned_batch_count);
	stats->set_static_global_batches(p_run.global_batch_count);
	stats->set_static_instances_retained(static_cast<int>(static_lod_instances_.size()));
	stats->set_static_lod_populations(p_run.lod_population_count);
	stats->set_static_live_populations(get_static_live_population_count());
	// The shadow-only twins a mixed-caster population emits beside its visible
	// batch: one more geometry instance (and instance-uniform allocation) each.
	stats->set_static_shadow_batches(p_run.shadow_batch_count);
	stats->set_authored_occluder_models(p_run.authored_occluder_models);
}

} // namespace godot
