#include "mission/mission_object_placer.h"
#include "mission/static_population_instance.h"
#include "mission/static_source_convert.h"
#include "util/string_convert.h"

#include <godot_cpp/core/object.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <runtime/world/model_geometry.h>
#include <base/io/fixed.h>

using namespace opennova::threedi;

namespace godot {

uint64_t MissionObjectPlacer::_retain_static_source_asset(const Ref<ObjectData> &p_data) {
	if (p_data.is_null()) return 0;
	const uint64_t id = static_cast<uint64_t>(p_data->get_instance_id());
	static_source_assets_.insert(id, p_data);
	return id;
}

Ref<ObjectData> MissionObjectPlacer::static_source_object_data(uint64_t asset_id) const {
	const auto *data = static_source_assets_.getptr(asset_id);
	return data != nullptr ? *data : Ref<ObjectData>();
}

std::vector<opennova::mission::StaticEffectSource>
MissionObjectPlacer::static_item_effect_sources() {
	_check_epoch();
	return static_sources_.effect_sources();
}

std::vector<opennova::mission::StaticLightDrawSource>
MissionObjectPlacer::static_light_draw_sources() {
	_check_epoch();
	return static_sources_.light_draw_sources();
}

uint64_t MissionObjectPlacer::static_light_draw_source_revision() {
	return static_sources_.light_revision();
}

uint64_t MissionObjectPlacer::get_static_terrain_shadow_source_revision() {
	_check_epoch();
	return static_sources_.shadow_revision();
}

std::vector<opennova::mission::StaticTerrainShadowSource>
MissionObjectPlacer::get_static_terrain_shadow_sources() {
	_check_epoch();
	return static_sources_.shadow_sources([this](const std::string &graphic) {
		return _retain_static_source_asset(_load_object_data(opennova::to_gd(graphic)));
	});
}

void MissionObjectPlacer::_record_static_terrain_shadow_source(int p_kind,
		int p_index, int p_bms_id, int p_team, uint32_t p_entity_attrib,
		int p_item_id, const String &p_graphic,
		const Transform3D &p_xform, const Ref<ObjectData> &p_data) {
	opennova::mission::StaticTerrainShadowSource source;
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
	source.graphic = opennova::to_std(p_graphic);
	source.world_transform = to_static_source_transform(p_xform);
	source.asset_id = p_data.is_valid() ? static_cast<uint64_t>(p_data->get_instance_id()) : 0;
	if (static_sources_.record_shadow(std::move(source))) _retain_static_source_asset(p_data);
}

int MissionObjectPlacer::_append_static_item_effect_source(int p_kind,
		int p_entity_index, int p_bms_id, int p_item_id,
		const String &p_graphic, const Transform3D &p_xform) {
	const Ref<ObjectData> data = _load_object_data(p_graphic);
	if (data.is_null()) return -1;
	opennova::mission::StaticEffectSource source;
	source.kind = p_kind;
	source.entity_index = p_entity_index;
	source.bms_id = p_bms_id;
	source.item_id = p_item_id;
	source.graphic = opennova::to_std(p_graphic);
	source.world_transform = to_static_source_transform(p_xform);
	source.asset_id = _retain_static_source_asset(data);
	source.entity_bound_radius_q16 = _item_entity_bound_radius_q16(p_item_id, data);
	source.model_floor_q16 = opennova::world::model_bound_floor_q16(data->native_model());
	return static_sources_.append_effect(std::move(source));
}

int MissionObjectPlacer::_append_static_light_draw_source(int p_source_index,
		int p_kind, int p_entity_index, int p_bms_id, int p_item_id,
		int p_robj_index, const AABB &p_world_bounds) {
	opennova::mission::StaticLightDrawSource source;
	source.source_index = p_source_index;
	source.kind = p_kind;
	source.entity_index = p_entity_index;
	source.bms_id = p_bms_id;
	source.item_id = p_item_id;
	source.robj_index = p_robj_index;
	source.light_transfer = item_db_.is_valid() ? item_db_->get_light_transfer(p_item_id) : 0.0f;
	for (int i = 0; i < 3; ++i) {
		source.bounds_position[i] = p_world_bounds.position[i];
		source.bounds_size[i] = p_world_bounds.size[i];
	}
	return static_sources_.append_light_draw(std::move(source));
}

// --- destruction support (world-wac-ai-re §24.6) -----------------------------

void MissionObjectPlacer::register_static_instance(int p_bms_id,
		const String &p_graphic, int p_index, const Transform3D &p_xform,
		bool p_casts_static_shadow, bool p_mirror_reflected) {
	opennova::mission::StaticInstance inst;
	inst.bms_id = p_bms_id;
	inst.graphic = opennova::to_std(p_graphic);
	inst.batch_key = inst.graphic;
	inst.index = p_index;
	inst.xform = to_static_source_transform(p_xform);
	inst.casts_static_shadow = p_casts_static_shadow;
	inst.mirror_reflected = p_mirror_reflected;
	static_sources_.register_instance(std::move(inst), true);
}

String MissionObjectPlacer::get_static_instance_batch_key(int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr) {
		return String();
	}
	return opennova::to_gd(rec->batch_key.empty() ? rec->graphic : rec->batch_key);
}

int MissionObjectPlacer::get_static_instance_binding_count(int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return 0;
	}
	return static_lod_instances_[rec->lod_instance].bindings.size();
}

bool MissionObjectPlacer::static_instance_is_mirror_reflected(
		int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	return rec != nullptr && rec->mirror_reflected;
}

bool MissionObjectPlacer::static_instance_casts_terrain_shadow(
		int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	return rec != nullptr && rec->casts_static_shadow;
}

// Hide a destroyed batched static: its row leaves every population it is
// live in (its level's population and shadow twin; the populations keep
// their capacity); returns the instance's placed transform for the husk
// graft.
Variant MissionObjectPlacer::hide_static_instance(int p_bms_id) {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr) {
		return Variant();
	}
	if (static_sources_.is_hidden(p_bms_id)) {
		return from_static_source_transform(rec->xform);
	}
	if (rec->lod_instance >= 0 &&
			rec->lod_instance < static_lod_instances_.size()) {
		static_lod_instances_.write[rec->lod_instance].carved = true;
		HashSet<int> touched;
		_write_static_instance_slots(rec->lod_instance, -1, touched);
		_flush_static_population_changes(touched);
	}
	static_sources_.hide_instance(p_bms_id);
	return from_static_source_transform(rec->xform);
}

bool MissionObjectPlacer::inherit_static_entity_projection(int p_bms_id,
		ObjectModel *p_model) const {
	const auto *record = static_sources_.instance(p_bms_id);
	if (p_model == nullptr || record == nullptr || record->lod_instance < 0 ||
			record->lod_instance >= static_lod_instances_.size()) return false;
	const StaticLodInstance &instance = static_lod_instances_[record->lod_instance];
	p_model->set_entity_projection_override(
			instance.local_projection_sphere, instance.entity_scale_q16);
	return true;
}

// Restore a carved static at the level last selected for it; the next
// update_static_lods re-evaluates the instance against the camera like any
// other. Repeated reset calls are safe (false when the instance was not
// hidden).
bool MissionObjectPlacer::show_static_instance(int p_bms_id) {
	if (!static_sources_.is_hidden(p_bms_id)) {
		return false;
	}
	const auto *rec = static_sources_.instance(p_bms_id);
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
	static_sources_.show_instance(p_bms_id);
	return true;
}

// --- the editor's moves (ADR 0046 S14) --------------------------------------

bool MissionObjectPlacer::move_static_instance(int p_bms_id, const Transform3D &p_xform) {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return false;
	}
	StaticLodInstance &instance = static_lod_instances_.write[rec->lod_instance];
	instance.xform = p_xform;
	instance.origin = ObjectLodFrame::projection_center(p_xform,
			instance.local_projection_sphere, instance.entity_scale_q16);
	HashSet<int> touched;
	for (int binding_index = 0; binding_index < instance.bindings.size(); ++binding_index) {
		StaticLodBinding &binding = instance.bindings.write[binding_index];
		binding.live_xform = p_xform * binding.offset;
		if (binding.row < 0 || binding.population < 0 ||
				binding.population >= static_populations_.size()) {
			continue;
		}
		StaticPopulation &population = static_populations_.write[binding.population];
		if (population.multimesh.is_null()) {
			continue;
		}
		population.multimesh->set_instance_transform(binding.row, binding.live_xform);
		touched.insert(binding.population);
		// The population's advertised bounds grown to hold the row where it is now.
		StaticPopulationInstance *node = Object::cast_to<StaticPopulationInstance>(
				ObjectDB::get_instance(population.instance_node));
		const Ref<Mesh> mesh = population.multimesh->get_mesh();
		if (node != nullptr && mesh.is_valid()) {
			const AABB bounds = binding.live_xform.xform(mesh->get_aabb());
			const AABB held = node->get_custom_aabb();
			node->set_custom_aabb(held.size == Vector3() ? bounds : held.merge(bounds));
		}
	}
	// The rows the Q3 pass read are stale: the touched populations publish again (their
	// visibility and shadow row maps stand).
	_flush_static_population_changes(touched);
	return true;
}

Variant MissionObjectPlacer::get_static_instance_transform(int p_bms_id) const {
	const auto *rec = static_sources_.instance(p_bms_id);
	if (rec == nullptr || rec->lod_instance < 0 ||
			rec->lod_instance >= static_lod_instances_.size()) {
		return Variant();
	}
	return static_lod_instances_[rec->lod_instance].xform;
}

bool MissionObjectPlacer::warm_static_graphic(const String &p_graphic, Node *p_tree_parent) {
	_check_epoch();
	if (p_graphic.is_empty()) {
		return false;
	}
	return !_get_static_batches(p_graphic, p_tree_parent).is_empty();
}

bool MissionObjectPlacer::update_static_terrain_shadow_source_transform(
		MissionData::EntityKind p_kind, int p_index, const Transform3D &p_xform) {
	_check_epoch();
	return static_sources_.update_shadow_transform(p_kind, p_index,
			to_static_source_transform(p_xform));
}

bool MissionObjectPlacer::set_static_terrain_shadow_replacement(
		int p_bms_id, const String &p_graphic, const Transform3D &p_xform,
		bool p_active) {
	_check_epoch();
	const std::string graphic = opennova::to_std(p_graphic);
	if (!static_sources_.accepts_replacement(p_bms_id, graphic)) return false;
	return static_sources_.set_shadow_replacement(p_bms_id, graphic,
			to_static_source_transform(p_xform), p_active,
			_retain_static_source_asset(_load_object_data(p_graphic)));
}

bool MissionObjectPlacer::clear_static_terrain_shadow_replacement(int p_bms_id) {
	_check_epoch();
	return static_sources_.clear_shadow_replacement(p_bms_id);
}

bool MissionObjectPlacer::register_object_data(const String &p_graphic,
		const Ref<ObjectData> &p_data) {
	_check_epoch();
	if (p_graphic.is_empty() || p_data.is_null()) {
		return false;
	}
	object_data_cache_[p_graphic] = p_data;
	static_sources_.bump_shadow_revision();
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
			profile.thresholds_q16.push_back(opennova::renderer::rlod_threshold_q16_from_rmdl(
					native_model.lods[lod].lod_threshold));
		}
	}
	if (p_lod_profile.has("sphere_radius")) {
		const auto q16 = opennova::io::float_to_fp16_16_round_sat;
		const Vector3 center = p_lod_profile.get("sphere_center", Vector3());
		profile.projection_sphere.center_q16 = {q16(center.z), q16(center.x), q16(center.y)};
		profile.projection_sphere.radius_q16 = q16(double(p_lod_profile.get("sphere_radius", 0.0)));
		profile.projection_sphere.valid = true;
	} else if (p_data->has_document()) {
		profile.projection_sphere =
				opennova::world::collision_projection_sphere_from_3di(p_data->native_model());
		profile.zero_center_projection_sphere =
				opennova::world::collision_projection_sphere_from_3di(
						p_data->native_model(), 0, 0, true);
	}
	_complete_static_lod_profile(profile, retained);
	object_data_cache_[p_graphic] = p_data;
	static_batch_cache_[p_graphic] = retained;
	static_lod_profile_cache_[p_graphic] = profile;
	static_sources_.bump_shadow_revision();
	return true;
}
} // namespace godot
