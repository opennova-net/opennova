#include "world/game_world.h"

#include "render/render_view.h"
#include "object/material_info.h"
#include "mission/mission_info.h"

using namespace godot;

Ref<Environment> GameWorld::frame_clear_environment() const {
	if (preview_environment_.is_valid()) return preview_environment_;
	return clear_color_ != nullptr ? clear_color_->get_environment() : Ref<Environment>();
}

void GameWorld::set_preview_configuration(bool enabled) {
	if (enabled) preview_properties_.begin(this, { "mission_file", "terrain_file", "env_file" });
	else preview_properties_.end(this);
	if (terrain_ != nullptr) terrain_->set_preview_configuration(enabled);
	if (dispatcher_ != nullptr) dispatcher_->set_preview_configuration(enabled);
	if (env_ != nullptr) env_->set_preview_configuration(enabled);
	if (water_ != nullptr) water_->set_preview_configuration(enabled);
	if (celestial_ != nullptr) celestial_->set_preview_configuration(enabled);
	if (environment_cube_ != nullptr) environment_cube_->set_preview_configuration(enabled);
	if (weather_ != nullptr) weather_->set_preview_configuration(enabled);
	if (sun_shadow_ != nullptr) sun_shadow_->set_preview_configuration(enabled);
}

Error GameWorld::load_preview(const String &p_local_directory) {
	// Never convert a live runtime into an editor preview.
	if (!is_node_ready() || (world_ready_ && !preview_active_)) return ERR_BUSY;
	unload_preview();
	preview_diagnostics_.clear();
	preview_status_ = "failed";
	auto fail = [&](Error error, const String &message) {
		unload_preview();
		preview_status_ = "failed";
		preview_diagnostics_.append(message);
		return error;
	};
	if (world_source_.is_null()) return fail(ERR_UNCONFIGURED, "Assign a WorldSource in the Inspector.");
	const String name = world_source_->get_mission_name().strip_edges();
	Ref<ResourceRoot> root = world_source_->open_root(p_local_directory);
	if (root.is_null()) return fail(world_source_->get_last_error_code(), world_source_->get_last_error());
	Ref<MissionData> mission = world_source_->open_mission(root);
	if (mission.is_null()) return fail(world_source_->get_last_error_code(), world_source_->get_last_error());
	Ref<TerrainData> terrain;
	terrain.instantiate();
	const String terrain_name = mission->get_terrain_ref() + String(".trn");
	const String environment_name = mission->get_environment_ref() + String(".env");
	for (const String &dependency : { terrain_name, environment_name }) {
		if (!root->has_file(dependency))
			return fail(ERR_FILE_NOT_FOUND, name + String(" requires ") + dependency);
	}
	if (terrain->load_from_resource_root(root, terrain_name) != OK)
		return fail(ERR_CANT_OPEN, "Could not load " + terrain_name);
	Ref<EnvFile> environment;
	environment.instantiate();
	if (environment->load_from_resource_root(root, environment_name) != OK)
		return fail(ERR_CANT_OPEN, "Could not load " + environment_name);
	return load_preview_documents(root, mission, terrain, environment);
}

Error GameWorld::load_preview_documents(const Ref<ResourceRoot> &p_root,
		const Ref<MissionData> &p_mission, const Ref<TerrainData> &p_terrain,
		const Ref<EnvFile> &p_environment) {
	if (!is_node_ready() || (world_ready_ && !preview_active_)) return ERR_BUSY;
	unload_preview();
	preview_diagnostics_.clear();
	preview_status_ = "failed";
	auto fail = [&](Error error, const String &message) {
		unload_preview();
		preview_status_ = "failed";
		preview_diagnostics_.append(message);
		return error;
	};
	if (world_source_.is_null() || p_root.is_null() || p_mission.is_null() || !p_mission->is_loaded() ||
			p_terrain.is_null() || !p_terrain->is_loaded() || p_environment.is_null() ||
			!p_environment->is_loaded() || p_environment->has_mission_overrides())
		return fail(ERR_INVALID_PARAMETER, "Preview requires an open source and native base documents.");
	const String name = world_source_->get_mission_name().strip_edges();
	const String terrain_name = p_mission->get_terrain_ref() + String(".trn");
	const String environment_name = p_mission->get_environment_ref() + String(".env");
	for (const String &dependency : { terrain_name, environment_name }) {
		if (!p_root->has_file(dependency))
			return fail(ERR_FILE_NOT_FOUND, name + String(" requires ") + dependency);
	}
	if (terrain_ == nullptr || env_ == nullptr)
		return fail(ERR_UNCONFIGURED, "Use the GameWorld scene with Terrain and MissionEnvironment children.");

	// These are the same document and device operations the runtime load uses.
	// The editor never calls start_runtime(), audio/effect startup, or play().
	set_preview_configuration(true);
	preview_active_ = true;
	if (clear_color_ != nullptr && clear_color_->get_environment().is_valid())
		preview_environment_ = clear_color_->get_environment()->duplicate(true);
	mission_file_ = name;
	terrain_file_ = terrain_name;
	env_file_ = environment_name;
	resource_root_ = p_root;
	load_mission_tile_info(name, p_root, PackedByteArray(), false);
	if (!load_environment(environment_name))
		return fail(ERR_CANT_OPEN, "Could not load " + environment_name);
	if (!env_->get_environment_data()->load_bytes(p_environment->to_bytes()))
		return fail(ERR_PARSE_ERROR, "Could not apply the environment document.");
	apply_mission_environment_overrides(p_mission);
	Ref<MissionInfo> info = p_mission->get_info();
	env_->configure_mission_clock(info->get_start_time(), info->get_minutes_per_day());
	prepare_autonomous_weather();
	set_weather_world_tick_driven(true);
	if (!bind_terrain_data(p_terrain))
		return fail(ERR_CANT_OPEN, "Could not load " + terrain_name);
	if (sky_dome_ != nullptr && !sky_dome_->is_built()) sky_dome_->build();
	if (water_ != nullptr && !water_->is_built()) water_->build();
	loaded_mission_ = p_mission;
	loaded_mission_file_ = name;
	start_mission_root();
	place_mission_objects(p_mission);
	// Placement samples the runtime clock. Reset its shared sample before
	// the first editor render and never sample the wall clock in preview.
	panm_clock_->sample(0, -1);
	world_ready_ = true;
	set_water_world_rendering_enabled(true);
	collect_preview_diagnostics();
	preview_status_ = preview_diagnostics_.is_empty() ? "ready" : "partial";
	return OK;
}

Error GameWorld::update_preview_settings(const Ref<EnvFile> &p_environment) {
	if (!preview_active_ || !world_ready_ || env_ == nullptr) return ERR_UNAVAILABLE;
	if (p_environment.is_null() || !p_environment->is_loaded() || p_environment->has_mission_overrides())
		return ERR_INVALID_PARAMETER;
	Ref<EnvFile> effective = env_->get_environment_data();
	if (effective.is_null() || !effective->load_bytes(p_environment->to_bytes())) return ERR_PARSE_ERROR;
	apply_mission_environment_overrides(loaded_mission_);
	Ref<MissionInfo> info = loaded_mission_->get_info();
	env_->configure_mission_clock(info->get_start_time(), info->get_minutes_per_day());
	prepare_autonomous_weather();
	set_weather_world_tick_driven(true);
	if (weather_ != nullptr) weather_->resync_colors();
	configure_foliage();
	if (environment_cube_ != nullptr) environment_cube_->force_capture();
	preview_diagnostics_.clear();
	collect_preview_diagnostics();
	preview_status_ = preview_diagnostics_.is_empty() ? "ready" : "partial";
	return OK;
}

void GameWorld::collect_preview_diagnostics() {
	const auto &trn = terrain_data_->get_trn();
	const String trn_name = loaded_mission_->get_terrain_ref() + String(".trn");
	auto missing = [&](const String &owner, const String &name) {
		if (!name.is_empty() && !resource_root_->has_file(name))
			preview_diagnostics_.append(owner + String(" requires ") + name);
	};
	if (terrain_data_->get_cpt().tiles.empty())
		preview_diagnostics_.append(trn_name + String(": no renderable baked terrain (") + String(trn.polydata.c_str()) + String(")."));
	for (const std::string *texture : { &trn.colormap, &trn.detailmap, &trn.detailmap_c1,
			&trn.detailmap_c2, &trn.detailmap_c3, &trn.detailblendmap, &trn.detailmap2,
			&trn.detailmapdist, &trn.detailmapdist2, &trn.charmap, &trn.foliagemap, &trn.tilestrip })
		missing(trn_name, String(texture->c_str()));
	const String env_name = loaded_mission_->get_environment_ref() + String(".env");
	const Ref<EnvFile> environment = env_->get_environment_data();
	for (const String &dependency : { environment->get_sky_map1(), environment->get_sky_map2(),
			environment->get_sun_3di(), environment->get_moon_3di(),
			environment->get_glare_3di(), environment->get_star_3di() })
		missing(env_name, dependency);
	missing(loaded_mission_file_, "items.def");
	Ref<ItemDatabase> items = placer_->get_item_db();
	HashSet<String> checked;
	if (items.is_valid()) {
		for (auto kind : { MissionData::KIND_MARKER, MissionData::KIND_ITEM,
				MissionData::KIND_BUILDING, MissionData::KIND_ORGANIC }) {
			TypedArray<MissionEntityRecord> entities = loaded_mission_->get_entities(kind);
			for (int i = 0; i < entities.size(); ++i) {
				Ref<MissionEntityRecord> entity = entities[i];
				const int id = entity->get_item_id();
				if (!items->has_item(id)) {
					const String key = vformat("item %d", id);
					if (!checked.has(key)) {
						checked.insert(key);
						preview_diagnostics_.append(loaded_mission_file_ + String(": items.def has no ") + key);
					}
					continue;
				}
				String graphic = items->get_graphic(id);
				if (graphic.is_empty() || checked.has(graphic)) continue;
				checked.insert(graphic);
				Ref<ObjectData> data = placer_->object_data_for(graphic);
				if (graphic.get_extension().is_empty()) graphic += ".3di";
				missing(vformat("items.def item %d", id), graphic);
				if (data.is_valid()) {
					for (int material = 0; material < data->get_material_count(); ++material) {
						MaterialInfo info;
						if (!data->get_material_info(material, info)) continue;
						for (const auto &slot : { std::pair<int, String>(ObjectData::TEX_SLOT_DIFFUSE, info.diffuse_a),
								std::pair<int, String>(ObjectData::TEX_SLOT_DETAIL, info.detail_a),
								std::pair<int, String>(ObjectData::TEX_SLOT_NORMAL, info.normal_a) }) {
							if (!slot.second.is_empty() && data->load_material_slot_texture(material, slot.first).is_null())
								preview_diagnostics_.append(graphic + String(" requires texture ") + slot.second);
						}
					}
				}
			}
		}
	}
	if (mission_stats_.is_valid() && mission_stats_->get_unresolved() > 0)
		preview_diagnostics_.append(vformat("%d placed models could not be resolved; see Output for decoder details.",
				mission_stats_->get_unresolved()));
	if (dispatcher_ != nullptr) {
		const Array slots = dispatcher_->get_slot_diagnostics();
		for (int i = 0; i < slots.size(); ++i) {
			const Dictionary slot = slots[i];
			const String status = slot.get("status", "");
			if (status == "missing_mesh" || status == "invalid_mesh")
				preview_diagnostics_.append(trn_name + String(": ") + String(slot.get("graphic", "")) + String(" (") + status + String(")"));
		}
	}
}

Error GameWorld::refresh_preview(Camera3D *p_camera) {
	if (!preview_active_ || !world_ready_ || !is_inside_tree()) return ERR_UNAVAILABLE;
	if (p_camera == nullptr || !p_camera->is_inside_tree()) return ERR_INVALID_PARAMETER;
	RenderView view(this, p_camera);
	frame_delta_ = 0.0;
	frame_camera_pos_ = p_camera->get_camera_transform().origin;
	frame_camera_xform_ = p_camera->get_camera_transform();
	frame_stats_on_ = false;
	frame_timing_ = false;
	// Shared device legs, all at the fixed authored time. No session, wall
	// clock, script, audio, or effect simulation enters this table.
	static const FrameLeg legs[] = {
		{ "scene_environment", -1, &GameWorld::leg_scene_environment, kLegNone },
		{ "environment", -1, &GameWorld::leg_environment_nodes, kLegNone },
		{ "terrain", -1, &GameWorld::leg_terrain, kLegNone },
		{ "water", -1, &GameWorld::leg_water, kLegNone },
		{ "foliage", -1, &GameWorld::leg_foliage, kLegNone },
		{ "materials", -1, &GameWorld::leg_materials, kLegNone },
		{ "clear", -1, &GameWorld::leg_clear, kLegNone },
	};
	FrameContext ctx;
	run_leg_table(legs, sizeof(legs) / sizeof(legs[0]), ctx, false);
	return OK;
}

void GameWorld::unload_preview() {
	if (!preview_active_) return;
	unload();
	// Runtime unload keeps the last terrain for its loading transition.
	// Preview unload must remove it before another scene becomes visible.
	if (terrain_ != nullptr) terrain_->clear();
	if (dispatcher_ != nullptr) {
		dispatcher_->set_terrain_data(Ref<TerrainData>());
		dispatcher_->configure_slots(Array(), Array(), Array());
		dispatcher_->clear_asset_cache();
	}
	if (sky_dome_ != nullptr) sky_dome_->clear();
	if (water_ != nullptr) {
		water_->release_runtime_renderer_resources();
		water_->set_terrain_data(Ref<TerrainData>());
	}
	if (celestial_ != nullptr) {
		celestial_->set_terrain_data(Ref<TerrainData>());
		celestial_->set_resource_root(Ref<ResourceRoot>());
		celestial_->clear();
	}
	if (precipitation_ != nullptr) precipitation_->set_resource_root(Ref<ResourceRoot>());
	if (environment_cube_ != nullptr) environment_cube_->set_terrain_data(Ref<TerrainData>());
	if (env_ != nullptr) {
		env_->set_environment_data(Ref<EnvFile>());
		env_->set_overcast_data(Ref<EnvFile>());
	}
	if (slot_shadow_ != nullptr) {
		slot_shadow_->set_terrain_data(Ref<TerrainData>());
		slot_shadow_->set_resource_root(Ref<ResourceRoot>());
	}
	set_preview_configuration(false);
	preview_environment_.unref();
	preview_active_ = false;
	terrain_data_.unref();
	resource_root_.unref();
	preview_status_ = "idle";
	preview_diagnostics_.clear();
}
