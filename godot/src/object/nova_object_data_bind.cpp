// ObjectData — the GDScript surface: method/constant bindings and the
// object_changed signal.
#include "object/nova_object_data_internal.h"

using namespace novaobj;

void ObjectData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snapshot_edit_state"), &ObjectData::snapshot_edit_state);
	ClassDB::bind_method(D_METHOD("apply_edit_state", "bytes"), &ObjectData::apply_edit_state);
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
	ClassDB::bind_method(D_METHOD("save_project_to_dir", "dir_path"), &ObjectData::save_project_to_dir);
	ClassDB::bind_method(D_METHOD("export_3di_to_dir", "dir_path", "update_mask"), &ObjectData::export_3di_to_dir, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("reset_empty", "name"), &ObjectData::reset_empty, DEFVAL("untitled"));
	ClassDB::bind_method(D_METHOD("set_lod_scene", "lod_index", "path"), &ObjectData::set_lod_scene);
	ClassDB::bind_method(D_METHOD("has_document"), &ObjectData::has_document);
	ClassDB::bind_method(D_METHOD("can_save_project"), &ObjectData::can_save_project);
	ClassDB::bind_method(D_METHOD("can_export_3di"), &ObjectData::can_export_3di);
	ClassDB::bind_method(D_METHOD("get_source_path"), &ObjectData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_source_dir"), &ObjectData::get_source_dir);
	ClassDB::bind_method(D_METHOD("get_object_name"), &ObjectData::get_object_name);
	ClassDB::bind_method(D_METHOD("get_source_kind"), &ObjectData::get_source_kind);
	ClassDB::bind_method(D_METHOD("get_last_error"), &ObjectData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_oed_dirty_mask"), &ObjectData::get_oed_dirty_mask);
	ClassDB::bind_method(D_METHOD("get_last_oed_update_mask"), &ObjectData::get_last_oed_update_mask);
	ClassDB::bind_method(D_METHOD("get_summary"), &ObjectData::get_summary);
	ClassDB::bind_method(D_METHOD("get_project_lods"), &ObjectData::get_project_lods);
	ClassDB::bind_method(D_METHOD("set_lod_field", "lod_index", "key", "value"), &ObjectData::set_lod_field);
	ClassDB::bind_method(D_METHOD("set_project_field", "key", "value"), &ObjectData::set_project_field);
	ClassDB::bind_method(D_METHOD("get_material_count"), &ObjectData::get_material_count);
	ClassDB::bind_method(D_METHOD("get_lod_surfaces", "lod_index"), &ObjectData::get_lod_surfaces);
	ClassDB::bind_method(D_METHOD("get_materials"), &ObjectData::get_materials);
	ClassDB::bind_method(D_METHOD("get_material_info", "index"), &ObjectData::get_material_info);
	ClassDB::bind_method(D_METHOD("set_material_field", "index", "key", "value"), &ObjectData::set_material_field);
	ClassDB::bind_method(D_METHOD("get_material_shader_flags", "index"), &ObjectData::get_material_shader_flags);
	ClassDB::bind_method(D_METHOD("get_material_anim_frames", "index", "slot"), &ObjectData::get_material_anim_frames);
	ClassDB::bind_method(D_METHOD("set_material_anim_frame", "index", "slot", "frame_idx", "path"), &ObjectData::set_material_anim_frame);
	ClassDB::bind_method(D_METHOD("get_shader_catalog"), &ObjectData::get_shader_catalog);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("get_global_control_register_catalog"),
			&ObjectData::get_global_control_register_catalog);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("canonical_control_register_name", "name"),
			&ObjectData::canonical_control_register_name);
	ClassDB::bind_method(D_METHOD("get_control_registers"), &ObjectData::get_control_registers);
	ClassDB::bind_method(D_METHOD("set_control_register_name", "index", "name"),
			&ObjectData::set_control_register_name);
	ClassDB::bind_method(D_METHOD("resolve_material_texture_path", "material_index", "texture_index"), &ObjectData::resolve_material_texture_path);
	ClassDB::bind_method(D_METHOD("load_material_texture", "material_index", "texture_index"), &ObjectData::load_material_texture);
	ClassDB::bind_method(D_METHOD("resolve_texture_name", "texture_name"), &ObjectData::resolve_texture_name);
	ClassDB::bind_method(D_METHOD("load_texture_name", "texture_name"), &ObjectData::load_texture_name);
	ClassDB::bind_method(D_METHOD("get_light_count"), &ObjectData::get_light_count);
	ClassDB::bind_method(D_METHOD("get_lights"), &ObjectData::get_lights);
	ClassDB::bind_method(D_METHOD("get_light_info", "index"), &ObjectData::get_light_info);
	ClassDB::bind_method(D_METHOD("set_light_field", "index", "key", "value"), &ObjectData::set_light_field);
	ClassDB::bind_method(D_METHOD("get_user_point_count"), &ObjectData::get_user_point_count);
	ClassDB::bind_method(D_METHOD("get_user_point_info", "index"), &ObjectData::get_user_point_info);
	ClassDB::bind_method(D_METHOD("get_user_point_bone_mask", "name"),
			&ObjectData::get_user_point_bone_mask);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("part_anim_rate_for_seconds", "seconds"),
			&ObjectData::part_anim_rate_for_seconds);
	ClassDB::bind_static_method("ObjectData",
			D_METHOD("part_anim_step", "phase", "dir", "rate"),
			&ObjectData::part_anim_step);
	ClassDB::bind_method(D_METHOD("get_ground_anchor", "lod_index"), &ObjectData::get_ground_anchor, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("has_collision"), &ObjectData::has_collision);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &ObjectData::has_occlusion);
	ClassDB::bind_method(D_METHOD("get_collision_volumes"), &ObjectData::get_collision_volumes);
	ClassDB::bind_method(D_METHOD("has_live_panm"), &ObjectData::has_live_panm);
	ClassDB::bind_method(D_METHOD("has_live_panm_for_lod", "lod_index"),
			&ObjectData::has_live_panm_for_lod);
	ClassDB::bind_method(D_METHOD("get_live_panm_lod"), &ObjectData::get_live_panm_lod);
	ClassDB::bind_method(D_METHOD("get_effective_panm_targets", "lod_index"), &ObjectData::get_effective_panm_targets);
	ClassDB::bind_method(D_METHOD("get_part_anim_count", "lod_index"), &ObjectData::get_part_anim_count);
	ClassDB::bind_method(D_METHOD("get_part_animations", "lod_index"), &ObjectData::get_part_animations);
	ClassDB::bind_method(D_METHOD("get_part_anim_editor_entries", "lod_index"), &ObjectData::get_part_anim_editor_entries);
	ClassDB::bind_method(D_METHOD("add_part_anim", "lod_index", "part_index"), &ObjectData::add_part_anim);
	ClassDB::bind_method(D_METHOD("duplicate_part_anim", "lod_index", "anim_index"), &ObjectData::duplicate_part_anim);
	ClassDB::bind_method(D_METHOD("delete_part_anim", "lod_index", "anim_index"), &ObjectData::delete_part_anim);
	ClassDB::bind_method(D_METHOD("set_part_anim_target", "lod_index", "anim_index", "part_index", "parent_part"), &ObjectData::set_part_anim_target);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_enabled", "lod_index", "anim_index", "channel", "enabled"), &ObjectData::set_part_anim_channel_enabled);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_mode", "lod_index", "anim_index", "channel", "axis", "mode", "control_register"), &ObjectData::set_part_anim_channel_mode);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_values", "lod_index", "anim_index", "channel", "axis", "from_value", "to_value", "speed"), &ObjectData::set_part_anim_channel_values);
	ClassDB::bind_method(D_METHOD("set_part_anim_rotation_reversed", "lod_index", "anim_index", "reversed"), &ObjectData::set_part_anim_rotation_reversed);
	ClassDB::bind_method(D_METHOD("get_part_anim_info", "lod_index", "anim_index"), &ObjectData::get_part_anim_info);
	ClassDB::bind_method(D_METHOD("set_part_anim_field", "lod_index", "anim_index", "key", "value"), &ObjectData::set_part_anim_field);
	ClassDB::bind_method(D_METHOD("set_part_anim_track_field", "lod_index", "anim_index", "track", "key", "value"), &ObjectData::set_part_anim_track_field);
	ClassDB::bind_method(D_METHOD("get_render_lod_info", "lod_index"), &ObjectData::get_render_lod_info);
	ClassDB::bind_method(D_METHOD("get_bone_origins", "lod_index"), &ObjectData::get_bone_origins, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_bone_parents", "lod_index"), &ObjectData::get_bone_parents, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_lod_submeshes", "lod_index", "skeletal", "bone_count", "native_frame"), &ObjectData::build_lod_submeshes, DEFVAL(false), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_skinned", "lod_index"), &ObjectData::is_skinned);
	ClassDB::bind_method(D_METHOD("eval_material_runtime", "index", "time_ms", "ctrl_values"), &ObjectData::eval_material_runtime);
	ClassDB::bind_method(D_METHOD("compute_anim_frame", "index", "time_ms", "ctrl_values"), &ObjectData::compute_anim_frame);
	ClassDB::bind_method(D_METHOD("evaluate_panm", "lod_index", "time_ms", "ctrl_values"), &ObjectData::evaluate_panm);
	ClassDB::bind_method(D_METHOD("apply_panm_to_nodes", "lod_index", "time_ms",
			"ctrl_values", "nodes", "applied_revision"), &ObjectData::apply_panm_to_nodes);
	ClassDB::bind_method(D_METHOD("get_panm_evaluation_serial"),
			&ObjectData::get_panm_evaluation_serial);
	ClassDB::bind_method(D_METHOD("evaluate_lights", "time_ms", "ctrl_values"), &ObjectData::evaluate_lights);
	ClassDB::bind_method(D_METHOD("set_material_shader", "material_index", "shader_name"), &ObjectData::set_material_shader);
	ClassDB::bind_method(D_METHOD("set_material_texture", "material_index", "texture_index", "texture_name"), &ObjectData::set_material_texture);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot", "material_index", "slot", "texture_name"), &ObjectData::set_material_texture_slot);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot_options", "material_index", "slot", "flags", "frame", "type"), &ObjectData::set_material_texture_slot_options);
	ClassDB::bind_method(D_METHOD("set_material_alpha_threshold", "material_index", "alpha_threshold"), &ObjectData::set_material_alpha_threshold);
	ClassDB::bind_method(D_METHOD("set_material_uv_generator", "material_index", "axis", "params"), &ObjectData::set_material_uv_generator);
	ClassDB::bind_method(D_METHOD("set_material_rgb_generator", "material_index", "params"), &ObjectData::set_material_rgb_generator);
	ClassDB::bind_method(D_METHOD("set_material_alpha_generator", "material_index", "params"), &ObjectData::set_material_alpha_generator);
	ClassDB::bind_method(D_METHOD("set_material_texture_animation", "material_index", "params"), &ObjectData::set_material_texture_animation);
	ClassDB::bind_method(D_METHOD("set_light_colors", "light_index", "start", "end"), &ObjectData::set_light_colors);
	ClassDB::bind_method(D_METHOD("set_part_animation_flags", "lod_index", "anim_index", "flags"), &ObjectData::set_part_animation_flags);

	BIND_CONSTANT(UPDATE_NONE);
	BIND_CONSTANT(UPDATE_MTRL);
	BIND_CONSTANT(UPDATE_LGHT);
	BIND_CONSTANT(UPDATE_PANM);
	BIND_CONSTANT(UPDATE_ALL);

	ADD_SIGNAL(MethodInfo("object_changed"));
}
