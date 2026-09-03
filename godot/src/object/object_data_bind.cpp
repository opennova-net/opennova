// ObjectData's runtime GDScript surface: immutable .3di loading, inspection,
// and evaluation plus the whole-content replacement signal.
#include "object/object_data_internal.h"
#include "object/model_light.h"
#include "object/model_user_point.h"

using namespace novaobj;

void ObjectData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("open_file", "path"), &ObjectData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name",
			"include_in_network_challenge"), &ObjectData::open_from_resource_root,
			DEFVAL(true));
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("mark_cached_network_challenge_foliage_model", "name"),
			&ObjectData::mark_cached_network_challenge_foliage_model);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("reset_network_challenge_model_registry"),
			&ObjectData::reset_network_challenge_model_registry);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("network_challenge_model_count"),
			&ObjectData::network_challenge_model_count);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ObjectData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ObjectData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_lod_count"), &ObjectData::get_lod_count);
	ClassDB::bind_method(D_METHOD("find_material_array_index", "material_index"),
			&ObjectData::find_material_array_index);
	ClassDB::bind_method(D_METHOD("load_material_slot_texture", "array_index", "slot"),
			&ObjectData::load_material_slot_texture);
	ClassDB::bind_method(D_METHOD("get_control_registers"), &ObjectData::get_control_registers);
	ClassDB::bind_method(D_METHOD("get_light_count"), &ObjectData::get_light_count);
	ClassDB::bind_method(D_METHOD("get_light_info", "index"), &ObjectData::get_light_info);
	ClassDB::bind_method(D_METHOD("get_user_point_count"), &ObjectData::get_user_point_count);
	ClassDB::bind_method(D_METHOD("get_user_point_info", "index"), &ObjectData::get_user_point_info);
	ClassDB::bind_method(D_METHOD("get_user_point_bone_mask", "name"),
			&ObjectData::get_user_point_bone_mask);
	ClassDB::bind_method(D_METHOD("has_collision"), &ObjectData::has_collision);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &ObjectData::has_occlusion);
	ClassDB::bind_method(D_METHOD("get_collision_volumes"), &ObjectData::get_collision_volumes);
	ClassDB::bind_method(D_METHOD("has_live_panm_for_lod", "lod_index"),
			&ObjectData::has_live_panm_for_lod);
	ClassDB::bind_method(D_METHOD("get_live_panm_lod"), &ObjectData::get_live_panm_lod);
	ClassDB::bind_method(D_METHOD("get_effective_panm_targets", "lod_index"), &ObjectData::get_effective_panm_targets);
	ClassDB::bind_method(D_METHOD("get_part_anim_count", "lod_index"), &ObjectData::get_part_anim_count);
	ClassDB::bind_method(D_METHOD("get_bone_origins", "lod_index"), &ObjectData::get_bone_origins, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_bone_parents", "lod_index"), &ObjectData::get_bone_parents, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_lod_submeshes", "lod_index", "skeletal", "bone_count", "native_frame"), &ObjectData::build_lod_submeshes, DEFVAL(false), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_skinned", "lod_index"), &ObjectData::is_skinned);
	ClassDB::bind_method(D_METHOD("eval_material_runtime", "index", "time_ms", "ctrl_values"), &ObjectData::eval_material_runtime);
	ClassDB::bind_method(D_METHOD("evaluate_panm", "lod_index", "time_ms", "ctrl_values"), &ObjectData::evaluate_panm);
	ClassDB::bind_method(D_METHOD("apply_panm_to_nodes", "lod_index", "time_ms",
			"ctrl_values", "nodes", "applied_revision"), &ObjectData::apply_panm_to_nodes);
	ClassDB::bind_method(D_METHOD("get_panm_evaluation_serial"),
			&ObjectData::get_panm_evaluation_serial);
	ClassDB::bind_method(D_METHOD("evaluate_lights", "time_ms", "ctrl_values"), &ObjectData::evaluate_lights);
	// 3DI3 flag/slot re-exports (engine threedi/threedi_3di3.h values).
	BIND_CONSTANT(MATERIAL_FLAG_ALPHA_TEST);
	BIND_CONSTANT(MATERIAL_FLAG_ALPHA_INVERT);
	BIND_CONSTANT(MATERIAL_FLAG_TWO_SIDED);
	ADD_SIGNAL(MethodInfo("object_changed"));
}
