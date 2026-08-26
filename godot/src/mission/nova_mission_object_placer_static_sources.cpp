#include "mission/nova_mission_object_placer.h"

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/variant/typed_array.hpp>

#include "mission/nova_mission_object_placer_keys.h"

// The placer's read-back seams and destruction-support registry: static
// user-point / item-effect / light-draw sources, static terrain-shadow
// sources, and the per-BMS static instance table. The placement walk itself
// lives in nova_mission_object_placer.cpp.

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

void MissionObjectPlacer::_record_static_terrain_shadow_source(int p_kind,
		int p_index, int p_bms_id, int p_team, uint32_t p_entity_attrib,
		int p_item_id, const String &p_graphic,
		const Transform3D &p_xform, const Ref<ObjectData> &p_data) {
	// The retail collector walks only pool 2 then pool 1. Keeping rejected
	// records from those pools preserves the exact policy inputs for the
	// portable admission predicate and its diagnostics.
	if (p_kind != opennova::mission::kEntityKindBuilding &&
			p_kind != opennova::mission::kEntityKindItem) {
		return;
	}
	StaticTerrainShadowSource source;
	source.bms_id = p_bms_id;
	source.item_id = p_item_id;
	source.entity_kind = p_kind;
	source.entity_index = p_index;
	source.team = p_team;
	source.entity_attrib = p_entity_attrib;
	if (item_db_.is_valid()) {
		source.item_attrib = item_db_->get_attrib(p_item_id);
		source.item_attrib2 = item_db_->get_attrib2(p_item_id);
	}
	source.graphic = p_graphic;
	source.world_transform = p_xform;
	source.object_data = p_data;
	const int row = static_terrain_shadow_sources_.size();
	static_terrain_shadow_sources_.push_back(source);
	static_terrain_shadow_source_rows_[entity_identity_key(p_kind, p_index)]
			.push_back(row);
	if (source.bms_id != 0) {
		static_terrain_shadow_rows_by_bms_[source.bms_id].push_back(row);
	}
	_bump_static_terrain_shadow_source_revision();
}

void MissionObjectPlacer::_bump_static_terrain_shadow_source_revision() {
	++static_terrain_shadow_source_revision_;
	if (static_terrain_shadow_source_revision_ == 0) {
		++static_terrain_shadow_source_revision_;
	}
}

uint64_t MissionObjectPlacer::get_static_terrain_shadow_source_revision() {
	_check_epoch();
	return static_terrain_shadow_source_revision_;
}

Vector<MissionObjectPlacer::StaticTerrainShadowSource>
MissionObjectPlacer::get_static_terrain_shadow_sources() {
	_check_epoch();
	Vector<StaticTerrainShadowSource> out = static_terrain_shadow_sources_;
	for (int i = 0; i < out.size(); ++i) {
		StaticTerrainShadowSource &source = out.write[i];
		if (source.object_data.is_null() && !source.graphic.is_empty()) {
			source.object_data = _load_object_data(source.graphic);
		}
		if (source.bms_id == 0) continue;
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(source.bms_id)) {
			source.world_transform = instance->xform;
		}
		source.active = !hidden_destruction_instances_.has(source.bms_id);
		if (const StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(source.bms_id)) {
			source.graphic = replacement->graphic;
			source.world_transform = replacement->world_transform;
			source.object_data = replacement->object_data.is_valid()
					? replacement->object_data
					: _load_object_data(replacement->graphic);
			source.active = replacement->active;
		}
	}

	// `register_static_instance` is the deterministic construction seam for
	// already-resolved renderers and asset-free tests. Merge any record not
	// already published by place() as a building-policy source;
	// casts=false maps to the same authored NoShadow veto the collector owns.
	for (const KeyValue<int64_t, DestructionInstance> &kv :
			destruction_instances_) {
		bool represented = false;
		for (const StaticTerrainShadowSource &source : out) {
			if (source.bms_id == static_cast<int>(kv.key)) {
				represented = true;
				break;
			}
		}
		if (represented) continue;
		StaticTerrainShadowSource source;
		source.bms_id = static_cast<int>(kv.key);
		source.entity_kind = opennova::mission::kEntityKindBuilding;
		source.entity_index = kv.value.index;
		source.entity_attrib = kv.value.casts_static_shadow
				? 0u
				: opennova::mission::kEntityAttribNoShadow;
		source.graphic = kv.value.graphic;
		source.world_transform = kv.value.xform;
		source.object_data = _load_object_data(source.graphic);
		source.active = !hidden_destruction_instances_.has(kv.key);
		if (const StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(kv.key)) {
			source.graphic = replacement->graphic;
			source.world_transform = replacement->world_transform;
			source.object_data = replacement->object_data.is_valid()
					? replacement->object_data
					: _load_object_data(replacement->graphic);
			source.active = replacement->active;
		}
		out.push_back(source);
	}
	return out;
}

Array MissionObjectPlacer::get_static_terrain_shadow_source_diagnostics() {
	Array out;
	const Vector<StaticTerrainShadowSource> sources =
			get_static_terrain_shadow_sources();
	for (const StaticTerrainShadowSource &source : sources) {
		Dictionary row;
		row["bms_id"] = source.bms_id;
		row["item_id"] = source.item_id;
		row["entity_kind"] = source.entity_kind;
		row["entity_index"] = source.entity_index;
		row["team"] = source.team;
		row["entity_attrib"] = static_cast<int64_t>(source.entity_attrib);
		row["item_attrib"] = static_cast<int64_t>(source.item_attrib);
		row["item_attrib2"] = static_cast<int64_t>(source.item_attrib2);
		row["graphic"] = source.graphic;
		row["world_transform"] = source.world_transform;
		row["object_data"] = source.object_data;
		row["active"] = source.active;
		out.push_back(row);
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

void MissionObjectPlacer::_append_static_user_point_source(
		const String &p_graphic, const Transform3D &p_xform) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null() || data->get_user_point_count() <= 0) {
		return;
	}
	for (int i = 0; i < static_user_point_sources_.size(); ++i) {
		Dictionary row = static_user_point_sources_[i];
		if (String(row.get("graphic", String())) != p_graphic) {
			continue;
		}
		Array transforms = Array(row.get("transforms", Array())).duplicate();
		transforms.push_back(p_xform);
		row["transforms"] = transforms;
		static_user_point_sources_[i] = row;
		return;
	}
	Dictionary row;
	row["graphic"] = p_graphic;
	row["object_data"] = data;
	Array transforms;
	transforms.push_back(p_xform);
	row["transforms"] = transforms;
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
	_bump_static_terrain_shadow_source_revision();
}

Variant MissionObjectPlacer::get_static_instance_transform(
		int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	return rec->xform;
}

String MissionObjectPlacer::get_static_instance_batch_key(int p_bms_id) const {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return String();
	}
	return rec->batch_key.is_empty() ? rec->graphic : rec->batch_key;
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

// Hide a destroyed batched static in every batch of its graphic/reflection
// population (zero-scale at its own origin — the batch keeps its instance
// count); returns the instance's placed transform for the husk graft.
Variant MissionObjectPlacer::hide_static_instance(int p_bms_id) {
	const DestructionInstance *rec = destruction_instances_.getptr(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	if (hidden_destruction_instances_.has(p_bms_id)) {
		return rec->xform;
	}
	const Transform3D carved(Basis().scaled(Vector3()), rec->xform.origin);
	Array originals;
	const String batch_key = rec->batch_key.is_empty()
			? rec->graphic
			: rec->batch_key;
	const Vector<Ref<MultiMesh>> *batches =
			destruction_batches_.getptr(batch_key);
	if (batches != nullptr) {
		for (const Ref<MultiMesh> &mm : *batches) {
			if (mm.is_valid() && rec->index >= 0 &&
					rec->index < mm->get_instance_count()) {
				Dictionary saved;
				saved["multimesh"] = mm;
				saved["transform"] = mm->get_instance_transform(rec->index);
				saved["index"] = rec->index;
				originals.push_back(saved);
				mm->set_instance_transform(rec->index, carved);
			}
		}
	}
	hidden_destruction_instances_[p_bms_id] = originals;
	++static_light_draw_source_revision_;
	_bump_static_terrain_shadow_source_revision();
	return rec->xform;
}

// Restore a carved static; repeated reset calls are safe (false when the
// instance was not hidden).
bool MissionObjectPlacer::show_static_instance(int p_bms_id) {
	const Array *originals = hidden_destruction_instances_.getptr(p_bms_id);
	if (originals == nullptr) {
		return false;
	}
	for (int i = 0; i < originals->size(); ++i) {
		const Dictionary saved = (*originals)[i];
		const Ref<MultiMesh> mm = saved.get("multimesh", Variant());
		const int index = int(saved.get("index", -1));
		if (mm.is_valid() && index >= 0 && index < mm->get_instance_count()) {
			mm->set_instance_transform(index,
					saved.get("transform", Transform3D()));
		}
	}
	hidden_destruction_instances_.erase(p_bms_id);
	++static_light_draw_source_revision_;
	_bump_static_terrain_shadow_source_revision();
	return true;
}

bool MissionObjectPlacer::update_static_terrain_shadow_source_transform(
		int p_kind, int p_index, const Transform3D &p_xform) {
	_check_epoch();
	bool matched = false;
	bool saw_source_match = false;
	bool observable_changed = false;
	const Vector<int> *source_rows = static_terrain_shadow_source_rows_.getptr(
			entity_identity_key(p_kind, p_index));
	if (source_rows != nullptr) {
		for (const int i : *source_rows) {
			if (i < 0 || i >= static_terrain_shadow_sources_.size()) continue;
			StaticTerrainShadowSource &source =
					static_terrain_shadow_sources_.write[i];
			if (source.entity_kind != p_kind || source.entity_index != p_index) {
				continue;
			}
			saw_source_match = true;
			matched = true;
			const bool policy_admitted =
					opennova::mission::item_casts_static_terrain_shadow(
					source.entity_kind, source.entity_attrib,
					source.item_attrib, source.item_attrib2);
			bool effective_active = source.active &&
					(source.bms_id == 0 ||
							!hidden_destruction_instances_.has(source.bms_id));
			StaticTerrainShadowSource *replacement = source.bms_id != 0
					? static_terrain_shadow_replacements_.getptr(source.bms_id)
					: nullptr;
			if (replacement != nullptr) effective_active = replacement->active;
			bool value_changed = false;
			if (source.world_transform != p_xform) {
				source.world_transform = p_xform;
				value_changed = true;
			}
			if (source.bms_id != 0) {
				if (DestructionInstance *instance =
							destruction_instances_.getptr(source.bms_id)) {
					if (instance->xform != p_xform) {
						instance->xform = p_xform;
						value_changed = true;
					}
				}
				if (replacement != nullptr) {
					if (replacement->world_transform != p_xform) {
						replacement->world_transform = p_xform;
						value_changed = true;
					}
				}
			}
			observable_changed |=
					policy_admitted && effective_active && value_changed;
		}
	}
	if (!saw_source_match &&
			p_kind == opennova::mission::kEntityKindBuilding) {
		for (KeyValue<int64_t, DestructionInstance> &kv :
				destruction_instances_) {
			if (kv.value.index != p_index) continue;
			matched = true;
			StaticTerrainShadowSource *replacement =
					static_terrain_shadow_replacements_.getptr(kv.key);
			const bool effective_active = kv.value.casts_static_shadow &&
					(replacement != nullptr
							? replacement->active
							: !hidden_destruction_instances_.has(kv.key));
			bool value_changed = false;
			if (kv.value.xform != p_xform) {
				kv.value.xform = p_xform;
				value_changed = true;
			}
			if (replacement != nullptr) {
				if (replacement->world_transform != p_xform) {
					replacement->world_transform = p_xform;
					value_changed = true;
				}
			}
			observable_changed |= effective_active && value_changed;
		}
	}
	if (observable_changed) _bump_static_terrain_shadow_source_revision();
	return matched;
}

void MissionObjectPlacer::_static_shadow_bms_policy(int p_bms_id,
		bool &r_represented, bool &r_policy_admitted,
		bool &r_base_active) const {
	r_represented = false;
	r_policy_admitted = false;
	r_base_active = false;
	const Vector<int> *rows =
			static_terrain_shadow_rows_by_bms_.getptr(p_bms_id);
	if (rows == nullptr) return;
	for (const int i : *rows) {
		if (i < 0 || i >= static_terrain_shadow_sources_.size()) continue;
		const StaticTerrainShadowSource &source =
				static_terrain_shadow_sources_[i];
		if (source.bms_id != p_bms_id) continue;
		r_represented = true;
		const bool admitted =
				opennova::mission::item_casts_static_terrain_shadow(
					source.entity_kind, source.entity_attrib,
					source.item_attrib, source.item_attrib2);
		r_policy_admitted |= admitted;
		r_base_active |= admitted && source.active &&
				!hidden_destruction_instances_.has(p_bms_id);
	}
}

bool MissionObjectPlacer::set_static_terrain_shadow_replacement(
		int p_bms_id, const String &p_graphic, const Transform3D &p_xform,
		bool p_active) {
	_check_epoch();
	bool policy_admitted = false;
	bool base_active = false;
	bool represented = false;
	_static_shadow_bms_policy(p_bms_id, represented, policy_admitted,
			base_active);
	bool source_exists = represented;
	if (!represented) {
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(p_bms_id)) {
			source_exists = true;
			policy_admitted = instance->casts_static_shadow;
			base_active = policy_admitted &&
					!hidden_destruction_instances_.has(p_bms_id);
		}
	}
	if (p_bms_id == 0 || p_graphic.is_empty() || !source_exists) {
		return false;
	}
	StaticTerrainShadowSource replacement;
	replacement.bms_id = p_bms_id;
	replacement.graphic = p_graphic;
	replacement.world_transform = p_xform;
	replacement.object_data = _load_object_data(p_graphic);
	replacement.active = p_active && replacement.object_data.is_valid();
	const StaticTerrainShadowSource *current =
			static_terrain_shadow_replacements_.getptr(p_bms_id);
	if (current != nullptr) {
		if (current->graphic == replacement.graphic &&
				current->world_transform == replacement.world_transform &&
				current->object_data.ptr() == replacement.object_data.ptr() &&
				current->active == replacement.active) {
			return replacement.object_data.is_valid();
		}
	}
	const bool was_effective = policy_admitted &&
			(current != nullptr ? current->active : base_active);
	const bool becomes_effective = policy_admitted && replacement.active;
	static_terrain_shadow_replacements_[p_bms_id] = replacement;
	if (was_effective || becomes_effective) {
		_bump_static_terrain_shadow_source_revision();
	}
	return replacement.object_data.is_valid();
}

bool MissionObjectPlacer::clear_static_terrain_shadow_replacement(
		int p_bms_id) {
	_check_epoch();
	const StaticTerrainShadowSource *replacement =
			static_terrain_shadow_replacements_.getptr(p_bms_id);
	if (replacement == nullptr) return false;
	bool policy_admitted = false;
	bool base_active = false;
	bool represented = false;
	_static_shadow_bms_policy(p_bms_id, represented, policy_admitted,
			base_active);
	if (!represented) {
		if (const DestructionInstance *instance =
					destruction_instances_.getptr(p_bms_id)) {
			policy_admitted = instance->casts_static_shadow;
			base_active = policy_admitted &&
					!hidden_destruction_instances_.has(p_bms_id);
		}
	}
	const bool was_effective = policy_admitted && replacement->active;
	const bool becomes_effective = policy_admitted && base_active;
	static_terrain_shadow_replacements_.erase(p_bms_id);
	if (was_effective || becomes_effective) {
		_bump_static_terrain_shadow_source_revision();
	}
	return true;
}

bool MissionObjectPlacer::register_object_data(const String &p_graphic,
		const Ref<ObjectData> &p_data) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null()) {
		return false;
	}
	object_data_cache_[p_graphic] = p_data;
	_bump_static_terrain_shadow_source_revision();
	return true;
}

bool MissionObjectPlacer::register_static_batches(const String &p_graphic,
		const Array &p_batches) {
	_check_epoch();
	if (p_graphic.is_empty() || p_batches.is_empty()) {
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
		retained.push_back(retained_batch);
		if (retained_batch.material.is_valid() &&
				batch_materials_.find(retained_batch.material) < 0) {
			batch_materials_.push_back(retained_batch.material);
		}
	}
	static_batch_cache_[p_graphic] = retained;
	return true;
}

void MissionObjectPlacer::register_occlusion_verdict(int p_item_id,
		bool p_has_occlusion) {
	_check_epoch();
	occlusion_cache_[p_item_id] = p_has_occlusion;
}

bool MissionObjectPlacer::register_resolved_static_graphic(
		const String &p_graphic, const Ref<ObjectData> &p_data,
		const Array &p_batches) {
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
		retained.push_back(retained_batch);
		if (retained_batch.material.is_valid() &&
				batch_materials_.find(retained_batch.material) < 0) {
			batch_materials_.push_back(retained_batch.material);
		}
	}
	object_data_cache_[p_graphic] = p_data;
	static_batch_cache_[p_graphic] = retained;
	_bump_static_terrain_shadow_source_revision();
	return true;
}

} // namespace godot
