// NovaObjectData — the GDScript surface: method/constant bindings and the
// object_changed signal.
#include "object/nova_object_data_internal.h"

using namespace novaobj;

void NovaObjectData::_bind_methods() {
	ClassDB::bind_method(D_METHOD("snapshot_edit_state"), &NovaObjectData::snapshot_edit_state);
	ClassDB::bind_method(D_METHOD("apply_edit_state", "bytes"), &NovaObjectData::apply_edit_state);
	ClassDB::bind_method(D_METHOD("open_file", "path"), &NovaObjectData::open_file);
	ClassDB::bind_method(D_METHOD("open_from_resource_root", "resource_root", "name",
			"include_in_network_challenge"), &NovaObjectData::open_from_resource_root,
			DEFVAL(true));
	ClassDB::bind_static_method("NovaObjectData",
			D_METHOD("mark_cached_network_challenge_foliage_model", "name"),
			&NovaObjectData::mark_cached_network_challenge_foliage_model);
	ClassDB::bind_static_method("NovaObjectData",
			D_METHOD("reset_network_challenge_model_registry"),
			&NovaObjectData::reset_network_challenge_model_registry);
	ClassDB::bind_static_method("NovaObjectData",
			D_METHOD("network_challenge_model_count"),
			&NovaObjectData::network_challenge_model_count);
	ClassDB::bind_method(D_METHOD("save_project_to_dir", "dir_path"), &NovaObjectData::save_project_to_dir);
	ClassDB::bind_method(D_METHOD("export_3di_to_dir", "dir_path", "update_mask"), &NovaObjectData::export_3di_to_dir, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("reset_empty", "name"), &NovaObjectData::reset_empty, DEFVAL("untitled"));
	ClassDB::bind_method(D_METHOD("set_lod_scene", "lod_index", "path"), &NovaObjectData::set_lod_scene);
	ClassDB::bind_method(D_METHOD("has_document"), &NovaObjectData::has_document);
	ClassDB::bind_method(D_METHOD("can_save_project"), &NovaObjectData::can_save_project);
	ClassDB::bind_method(D_METHOD("can_export_3di"), &NovaObjectData::can_export_3di);
	ClassDB::bind_method(D_METHOD("get_source_path"), &NovaObjectData::get_source_path);
	ClassDB::bind_method(D_METHOD("get_source_dir"), &NovaObjectData::get_source_dir);
	ClassDB::bind_method(D_METHOD("get_object_name"), &NovaObjectData::get_object_name);
	ClassDB::bind_method(D_METHOD("get_source_kind"), &NovaObjectData::get_source_kind);
	ClassDB::bind_method(D_METHOD("get_last_error"), &NovaObjectData::get_last_error);
	ClassDB::bind_method(D_METHOD("get_oed_dirty_mask"), &NovaObjectData::get_oed_dirty_mask);
	ClassDB::bind_method(D_METHOD("get_last_oed_update_mask"), &NovaObjectData::get_last_oed_update_mask);
	ClassDB::bind_method(D_METHOD("get_summary"), &NovaObjectData::get_summary);
	ClassDB::bind_method(D_METHOD("get_project_lods"), &NovaObjectData::get_project_lods);
	ClassDB::bind_method(D_METHOD("set_lod_field", "lod_index", "key", "value"), &NovaObjectData::set_lod_field);
	ClassDB::bind_method(D_METHOD("set_project_field", "key", "value"), &NovaObjectData::set_project_field);
	ClassDB::bind_method(D_METHOD("get_material_count"), &NovaObjectData::get_material_count);
	ClassDB::bind_method(D_METHOD("get_lod_surfaces", "lod_index"), &NovaObjectData::get_lod_surfaces);
	ClassDB::bind_method(D_METHOD("get_materials"), &NovaObjectData::get_materials);
	ClassDB::bind_method(D_METHOD("get_material_info", "index"), &NovaObjectData::get_material_info);
	ClassDB::bind_method(D_METHOD("set_material_field", "index", "key", "value"), &NovaObjectData::set_material_field);
	ClassDB::bind_method(D_METHOD("get_material_shader_flags", "index"), &NovaObjectData::get_material_shader_flags);
	ClassDB::bind_method(D_METHOD("get_material_anim_frames", "index", "slot"), &NovaObjectData::get_material_anim_frames);
	ClassDB::bind_method(D_METHOD("set_material_anim_frame", "index", "slot", "frame_idx", "path"), &NovaObjectData::set_material_anim_frame);
	ClassDB::bind_method(D_METHOD("get_shader_catalog"), &NovaObjectData::get_shader_catalog);
	ClassDB::bind_static_method("NovaObjectData",
			D_METHOD("get_global_control_register_catalog"),
			&NovaObjectData::get_global_control_register_catalog);
	ClassDB::bind_static_method("NovaObjectData",
			D_METHOD("canonical_control_register_name", "name"),
			&NovaObjectData::canonical_control_register_name);
	ClassDB::bind_method(D_METHOD("get_control_registers"), &NovaObjectData::get_control_registers);
	ClassDB::bind_method(D_METHOD("set_control_register_name", "index", "name"),
			&NovaObjectData::set_control_register_name);
	ClassDB::bind_method(D_METHOD("resolve_material_texture_path", "material_index", "texture_index"), &NovaObjectData::resolve_material_texture_path);
	ClassDB::bind_method(D_METHOD("load_material_texture", "material_index", "texture_index"), &NovaObjectData::load_material_texture);
	ClassDB::bind_method(D_METHOD("resolve_texture_name", "texture_name"), &NovaObjectData::resolve_texture_name);
	ClassDB::bind_method(D_METHOD("load_texture_name", "texture_name"), &NovaObjectData::load_texture_name);
	ClassDB::bind_method(D_METHOD("get_light_count"), &NovaObjectData::get_light_count);
	ClassDB::bind_method(D_METHOD("get_lights"), &NovaObjectData::get_lights);
	ClassDB::bind_method(D_METHOD("get_light_info", "index"), &NovaObjectData::get_light_info);
	ClassDB::bind_method(D_METHOD("set_light_field", "index", "key", "value"), &NovaObjectData::set_light_field);
	ClassDB::bind_method(D_METHOD("get_user_point_count"), &NovaObjectData::get_user_point_count);
	ClassDB::bind_method(D_METHOD("get_user_point_info", "index"), &NovaObjectData::get_user_point_info);
	ClassDB::bind_method(D_METHOD("get_ground_anchor", "lod_index"), &NovaObjectData::get_ground_anchor, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("has_collision"), &NovaObjectData::has_collision);
	ClassDB::bind_method(D_METHOD("has_occlusion"), &NovaObjectData::has_occlusion);
	ClassDB::bind_method(D_METHOD("get_collision_volumes"), &NovaObjectData::get_collision_volumes);
	ClassDB::bind_method(D_METHOD("has_live_panm"), &NovaObjectData::has_live_panm);
	ClassDB::bind_method(D_METHOD("has_live_panm_for_lod", "lod_index"),
			&NovaObjectData::has_live_panm_for_lod);
	ClassDB::bind_method(D_METHOD("get_live_panm_lod"), &NovaObjectData::get_live_panm_lod);
	ClassDB::bind_method(D_METHOD("get_effective_panm_targets", "lod_index"), &NovaObjectData::get_effective_panm_targets);
	ClassDB::bind_method(D_METHOD("get_part_anim_count", "lod_index"), &NovaObjectData::get_part_anim_count);
	ClassDB::bind_method(D_METHOD("get_part_animations", "lod_index"), &NovaObjectData::get_part_animations);
	ClassDB::bind_method(D_METHOD("get_part_anim_editor_entries", "lod_index"), &NovaObjectData::get_part_anim_editor_entries);
	ClassDB::bind_method(D_METHOD("add_part_anim", "lod_index", "part_index"), &NovaObjectData::add_part_anim);
	ClassDB::bind_method(D_METHOD("duplicate_part_anim", "lod_index", "anim_index"), &NovaObjectData::duplicate_part_anim);
	ClassDB::bind_method(D_METHOD("delete_part_anim", "lod_index", "anim_index"), &NovaObjectData::delete_part_anim);
	ClassDB::bind_method(D_METHOD("set_part_anim_target", "lod_index", "anim_index", "part_index", "parent_part"), &NovaObjectData::set_part_anim_target);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_enabled", "lod_index", "anim_index", "channel", "enabled"), &NovaObjectData::set_part_anim_channel_enabled);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_mode", "lod_index", "anim_index", "channel", "axis", "mode", "control_register"), &NovaObjectData::set_part_anim_channel_mode);
	ClassDB::bind_method(D_METHOD("set_part_anim_channel_values", "lod_index", "anim_index", "channel", "axis", "from_value", "to_value", "speed"), &NovaObjectData::set_part_anim_channel_values);
	ClassDB::bind_method(D_METHOD("set_part_anim_rotation_reversed", "lod_index", "anim_index", "reversed"), &NovaObjectData::set_part_anim_rotation_reversed);
	ClassDB::bind_method(D_METHOD("get_part_anim_info", "lod_index", "anim_index"), &NovaObjectData::get_part_anim_info);
	ClassDB::bind_method(D_METHOD("set_part_anim_field", "lod_index", "anim_index", "key", "value"), &NovaObjectData::set_part_anim_field);
	ClassDB::bind_method(D_METHOD("set_part_anim_track_field", "lod_index", "anim_index", "track", "key", "value"), &NovaObjectData::set_part_anim_track_field);
	ClassDB::bind_method(D_METHOD("get_render_lod_info", "lod_index"), &NovaObjectData::get_render_lod_info);
	ClassDB::bind_method(D_METHOD("get_bone_origins", "lod_index"), &NovaObjectData::get_bone_origins, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("get_bone_parents", "lod_index"), &NovaObjectData::get_bone_parents, DEFVAL(0));
	ClassDB::bind_method(D_METHOD("build_lod_submeshes", "lod_index", "skeletal", "bone_count", "native_frame"), &NovaObjectData::build_lod_submeshes, DEFVAL(false), DEFVAL(0), DEFVAL(false));
	ClassDB::bind_method(D_METHOD("is_skinned", "lod_index"), &NovaObjectData::is_skinned);
	ClassDB::bind_method(D_METHOD("eval_material_runtime", "index", "time_ms", "ctrl_values"), &NovaObjectData::eval_material_runtime);
	ClassDB::bind_method(D_METHOD("compute_anim_frame", "index", "time_ms", "ctrl_values"), &NovaObjectData::compute_anim_frame);
	ClassDB::bind_method(D_METHOD("evaluate_panm", "lod_index", "time_ms", "ctrl_values"), &NovaObjectData::evaluate_panm);
	ClassDB::bind_method(D_METHOD("apply_panm_to_nodes", "lod_index", "time_ms",
			"ctrl_values", "nodes", "applied_revision"), &NovaObjectData::apply_panm_to_nodes);
	ClassDB::bind_method(D_METHOD("get_panm_evaluation_serial"),
			&NovaObjectData::get_panm_evaluation_serial);
	ClassDB::bind_method(D_METHOD("evaluate_lights", "time_ms", "ctrl_values"), &NovaObjectData::evaluate_lights);
	ClassDB::bind_method(D_METHOD("set_material_shader", "material_index", "shader_name"), &NovaObjectData::set_material_shader);
	ClassDB::bind_method(D_METHOD("set_material_texture", "material_index", "texture_index", "texture_name"), &NovaObjectData::set_material_texture);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot", "material_index", "slot", "texture_name"), &NovaObjectData::set_material_texture_slot);
	ClassDB::bind_method(D_METHOD("set_material_texture_slot_options", "material_index", "slot", "flags", "frame", "type"), &NovaObjectData::set_material_texture_slot_options);
	ClassDB::bind_method(D_METHOD("set_material_alpha_threshold", "material_index", "alpha_threshold"), &NovaObjectData::set_material_alpha_threshold);
	ClassDB::bind_method(D_METHOD("set_material_uv_generator", "material_index", "axis", "params"), &NovaObjectData::set_material_uv_generator);
	ClassDB::bind_method(D_METHOD("set_material_rgb_generator", "material_index", "params"), &NovaObjectData::set_material_rgb_generator);
	ClassDB::bind_method(D_METHOD("set_material_alpha_generator", "material_index", "params"), &NovaObjectData::set_material_alpha_generator);
	ClassDB::bind_method(D_METHOD("set_material_texture_animation", "material_index", "params"), &NovaObjectData::set_material_texture_animation);
	ClassDB::bind_method(D_METHOD("set_light_colors", "light_index", "start", "end"), &NovaObjectData::set_light_colors);
	ClassDB::bind_method(D_METHOD("set_part_animation_flags", "lod_index", "anim_index", "flags"), &NovaObjectData::set_part_animation_flags);

	BIND_CONSTANT(UPDATE_NONE);
	BIND_CONSTANT(UPDATE_MTRL);
	BIND_CONSTANT(UPDATE_LGHT);
	BIND_CONSTANT(UPDATE_PANM);
	BIND_CONSTANT(UPDATE_ALL);

	ADD_SIGNAL(MethodInfo("object_changed"));
}
