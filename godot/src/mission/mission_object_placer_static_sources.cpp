#include "mission/mission_object_placer.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include <runtime/simassets/model_builders.h>

#include "mission/mission_object_placer_keys.h"

// The placer's read-back seams and destruction-support registry: static
// user-point / item-effect / light-draw sources and the per-BMS static
// instance table. The placement walk itself lives in
// mission_object_placer.cpp.

namespace godot {

// --- read-back seams ---------------------------------------------------------

// Snapshot of successfully rendered static user-point sources for a world
// debug view; transform arrays are duplicated so a consumer cannot mutate
// the placer's placement record.
Array MissionObjectPlacer::get_static_user_point_sources() {
	_check_epoch();
	Array out;
	for (int i = 0; i < static_user_point_sources_.size(); ++i) {
		const Dictionary row = static_user_point_sources_[i];
		Dictionary copy;
		copy["graphic"] = row.get("graphic", String());
		copy["object_data"] = row.get("object_data", Variant());
		copy["transforms"] = Array(row.get("transforms", Array())).duplicate();
		out.push_back(copy);
	}
	return out;
}

// Snapshot of successfully rendered static entities for mission-start item
// effects. Every row is a value descriptor; no placed/render Node is
// exposed; row order is placement order and stable for the mission.
Array MissionObjectPlacer::get_static_item_effect_sources() {
	_check_epoch();
	return static_item_effect_sources_.duplicate(true);
}

uint64_t MissionObjectPlacer::get_static_light_draw_source_revision() const {
	return static_light_draw_source_revision_;
}

Array MissionObjectPlacer::get_static_light_draw_sources() {
	_check_epoch();
	Array out = static_light_draw_sources_.duplicate(true);
	for (int i = 0; i < out.size(); ++i) {
		Dictionary row = out[i];
		const int bms_id = int(row.get("bms_id", 0));
		row["active"] = bms_id == 0 ||
				!hidden_destruction_instances_.has(bms_id);
		out[i] = row;
	}
	return out;
}

void MissionObjectPlacer::_record_static_user_point_group(
		const String &p_graphic, const Array &p_transforms) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null() || data->get_user_point_count() <= 0) {
		return;
	}
	Dictionary row;
	row["graphic"] = p_graphic;
	row["object_data"] = data;
	row["transforms"] = p_transforms.duplicate();
	static_user_point_sources_.push_back(row);
}

int MissionObjectPlacer::_append_static_item_effect_source(int p_kind,
		int p_entity_index, int p_bms_id, int p_item_id,
		const String &p_graphic, const Transform3D &p_xform) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) {
		return -1;
	}
	Dictionary row;
	row["kind"] = p_kind;
	row["entity_index"] = p_entity_index;
	row["bms_id"] = p_bms_id;
	row["item_id"] = p_item_id;
	row["graphic"] = p_graphic;
	row["world_transform"] = p_xform;
	row["object_data"] = data;
	const int source_index = static_item_effect_sources_.size();
	row["source_index"] = source_index;
	static_item_effect_sources_.push_back(row);
	return source_index;
}

int MissionObjectPlacer::_append_static_light_draw_source(int p_source_index,
		int p_kind, int p_entity_index, int p_bms_id, int p_item_id,
		int p_robj_index, const AABB &p_world_bounds) {
	Dictionary row;
	const int atlas_row = static_light_draw_sources_.size();
	row["atlas_row"] = atlas_row;
	row["source_index"] = p_source_index;
	row["kind"] = p_kind;
	row["entity_index"] = p_entity_index;
	row["bms_id"] = p_bms_id;
	row["item_id"] = p_item_id;
	row["robj_index"] = p_robj_index;
	row["world_bounds"] = p_world_bounds;
	static_light_draw_sources_.push_back(row);
	++static_light_draw_source_revision_;
	return atlas_row;
}

// --- destruction support (world-wac-ai-re §24.6) -----------------------------

void MissionObjectPlacer::register_static_instance(int p_bms_id,
		const String &p_graphic, int p_index, const Transform3D &p_xform,
		bool p_casts_static_shadow, bool p_mirror_reflected) {
	DestructionInstance inst;
	inst.graphic = p_graphic;
	inst.batch_key = p_graphic;
	inst.index = p_index;
	inst.xform = p_xform;
	inst.casts_static_shadow = p_casts_static_shadow;
	inst.mirror_reflected = p_mirror_reflected;
	destruction_instances_[p_bms_id] = inst;
}

String MissionObjectPlacer::get_static_instance_batch_key(int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return String();
	}
	return rec->batch_key.is_empty() ? rec->graphic : rec->batch_key;
}

int MissionObjectPlacer::get_static_instance_binding_count(int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return 0;
	}
	return static_lod_instances_[rec->lod_instance].bindings.size();
}

bool MissionObjectPlacer::static_instance_is_mirror_reflected(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	return rec != nullptr && rec->mirror_reflected;
}

bool MissionObjectPlacer::static_instance_casts_terrain_shadow(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	return rec != nullptr && rec->casts_static_shadow;
}

// Hide a destroyed batched static: its row leaves every population it is
// live in (its level's population and shadow twin; the populations keep
// their capacity); returns the instance's placed transform for the husk
// graft.
Variant MissionObjectPlacer::hide_static_instance(int p_bms_id) {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	if (hidden_destruction_instances_.has(p_bms_id)) {
		return rec->xform;
	}
	if (rec->lod_instance >= 0 &&
			rec->lod_instance < static_lod_instances_.size()) {
		static_lod_instances_.write[rec->lod_instance].carved = true;
		HashSet<int> touched;
		_write_static_instance_slots(rec->lod_instance, -1, touched);
		_flush_static_population_changes(touched);
	}
	hidden_destruction_instances_.insert(p_bms_id);
	++static_light_draw_source_revision_;
	return rec->xform;
}

// Restore a carved static at the level last selected for it; the next
// update_static_lods re-evaluates the instance against the camera like any
// other. Repeated reset calls are safe (false when the instance was not
// hidden).
bool MissionObjectPlacer::show_static_instance(int p_bms_id) {
	if (!hidden_destruction_instances_.has(p_bms_id)) {
		return false;
	}
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec != nullptr && rec->lod_instance >= 0 &&
			rec->lod_instance < static_lod_instances_.size()) {
		StaticLodInstance &instance =
				static_lod_instances_.write[rec->lod_instance];
		instance.carved = false;
		HashSet<int> touched;
		_write_static_instance_slots(rec->lod_instance, instance.active_lod,
				touched);
		_flush_static_population_changes(touched);
	}
	hidden_destruction_instances_.erase(p_bms_id);
	++static_light_draw_source_revision_;
	return true;
}
bool MissionObjectPlacer::register_object_data(const String &p_graphic,
		const Ref<ObjectData> &p_data) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null()) {
		return false;
	}
	object_data_cache_[p_graphic] = p_data;
	return true;
}

void MissionObjectPlacer::register_occlusion_verdict(int p_item_id,
		bool p_has_occlusion) {
	_check_epoch();
	occlusion_cache_[p_item_id] = p_has_occlusion;
}

bool MissionObjectPlacer::register_resolved_static_graphic(
		const String &p_graphic, const Ref<ObjectData> &p_data,
		const Array &p_batches, const Dictionary &p_lod_profile) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null() || p_batches.is_empty()) {
		return false;
	}
	Vector<StaticBatch> retained;
	for (int i = 0; i < p_batches.size(); ++i) {
		if (p_batches[i].get_type() != Variant::DICTIONARY) {
			return false;
		}
		const Dictionary batch = p_batches[i];
		const Ref<Mesh> mesh = batch.get("mesh", Variant());
		if (mesh.is_null()) {
			return false;
		}
		StaticBatch retained_batch;
		retained_batch.mesh = mesh;
		retained_batch.material = batch.get("material", Variant());
		retained_batch.offset = batch.get("offset", Transform3D());
		retained_batch.submesh = int(batch.get("submesh", 0));
		retained_batch.robj_index = int(batch.get("robj_index", 0));
		retained_batch.lod_index = MAX(int(batch.get("lod_index", 0)), 0);
		retained_batch.blended_draw = bool(batch.get("blended_draw",
				batch.get("is_alpha", false)));
		retained.push_back(retained_batch);
	}
	StaticLodProfile profile;
	if (p_lod_profile.has("thresholds_q16")) {
		const PackedInt32Array thresholds = p_lod_profile.get("thresholds_q16",
				PackedInt32Array());
		for (int64_t i = 0; i < thresholds.size(); ++i) {
			profile.thresholds_q16.push_back(thresholds[i]);
		}
	} else if (p_data->has_document()) {
		const Threedi3di3 &native_model = p_data->native_model();
		for (std::size_t lod = 0; lod < native_model.lod_count; ++lod) {
			profile.thresholds_q16.push_back(native_model.lods[lod].lod_threshold);
		}
	}
	if (p_lod_profile.has("sphere_radius")) {
		profile.sphere_radius = float(p_lod_profile.get("sphere_radius", 0.0));
	} else if (p_data->has_document()) {
		profile.sphere_radius =
				opennova::simassets::model_bound_radius_from_3di(
						p_data->native_model());
	}
	_complete_static_lod_profile(profile, retained);
	object_data_cache_[p_graphic] = p_data;
	static_batch_cache_[p_graphic] = retained;
	static_lod_profile_cache_[p_graphic] = profile;
	return true;
}
} // namespace godot
