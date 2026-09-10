#include "world/game_world.h"

#include "render/render_view.h"
#include "object/material_info.h"
#include "mission/mission_info.h"
#include "mission/mission_records.h"
#include "mission/mission_root.h"
#include "object/entity_ref.h"

#include <godot_cpp/core/object.hpp>

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
	// The placed models' authored light records into the point-light pool:
	// the director walk mission start runs beside the effect catalog warm-up.
	// The preview has no EffectWorld, so only this half of that pair applies.
	if (light_director_.is_valid()) light_director_->reattach();
	// Placement samples the runtime clock. Reset its shared sample before
	// the first editor render and never sample the wall clock in preview.
	panm_clock_->sample(0, -1);
	build_preview_entities();
	world_ready_ = true;
	set_water_world_rendering_enabled(true);
	collect_preview_diagnostics();
	preview_status_ = preview_diagnostics_.is_empty() ? "ready" : "partial";
	return OK;
}

// --- the editor's entity projections (ADR 0044) ------------------------------

namespace {
const char *kPreviewEntitiesName = "PreviewEntities";

String preview_entity_name(int p_kind, int p_index) {
	return vformat("Entity_%d_%d", p_kind, p_index);
}
} // namespace

Node3D *GameWorld::preview_entities() const {
	return Object::cast_to<Node3D>(ObjectDB::get_instance(preview_entities_id_));
}

void GameWorld::free_preview_entities() {
	Node3D *container = preview_entities();
	preview_entities_id_ = ObjectID();
	if (container == nullptr) return;
	if (Node *parent = container->get_parent()) parent->remove_child(container);
	memdelete(container);
}

ObjectModel *GameWorld::placed_model_for(int p_kind, int p_index) const {
	if (placer_.is_null()) return nullptr;
	const TypedArray<ObjectModel> models = placer_->get_placed_models();
	for (int i = 0; i < models.size(); ++i) {
		ObjectModel *model = Object::cast_to<ObjectModel>(models[i]);
		if (model == nullptr) continue;
		const Ref<EntityRef> ref = model->get_entity_ref();
		if (ref.is_valid() && ref->get_kind() == p_kind && ref->get_index() == p_index) return model;
	}
	return nullptr;
}

void GameWorld::sync_preview_entity_proxy(WorldEntityProxy *p_proxy,
		const Ref<MissionEntityRecord> &p_record) {
	p_proxy->set_kind(p_record->get_kind());
	p_proxy->set_index(p_record->get_index());
	p_proxy->set_bms_id(p_record->get_bms_id());
	p_proxy->set_item_id(p_record->get_item_id());
	p_proxy->set_graphic(placer_.is_valid() ? placer_->graphic_for(p_record->get_item_id()) : String());
	const int bms_id = p_record->get_bms_id();
	if (placer_.is_valid() && bms_id != 0 && placer_->has_static_instance(bms_id)) {
		p_proxy->set_representation(WorldEntityProxy::STATIC_INSTANCE);
		p_proxy->set_transform(placer_->get_static_instance_transform(bms_id));
		p_proxy->set_local_bounds(placer_->get_static_instance_local_bounds(bms_id));
		return;
	}
	if (ObjectModel *model = placed_model_for(p_record->get_kind(), p_record->get_index())) {
		p_proxy->set_representation(WorldEntityProxy::MODEL);
		p_proxy->set_transform(model->get_transform());
		p_proxy->set_local_bounds(model->get_model_bounds());
		return;
	}
	p_proxy->set_representation(WorldEntityProxy::UNPLACED);
	p_proxy->set_transform(MissionObjectPlacer::entity_transform(p_record->get_position(),
			p_record->get_rotation_deg()));
	p_proxy->set_local_bounds(AABB());
}

void GameWorld::build_preview_entities() {
	free_preview_entities();
	MissionRoot *runtime = get_runtime();
	if (runtime == nullptr || loaded_mission_.is_null()) return;
	Node3D *container = memnew(Node3D);
	container->set_name(kPreviewEntitiesName);
	runtime->add_child(container);
	preview_entities_id_ = container->get_instance_id();
	const TypedArray<MissionEntityRecord> records = loaded_mission_->get_all_entities();
	for (int i = 0; i < records.size(); ++i) {
		const Ref<MissionEntityRecord> record = records[i];
		if (record.is_null()) continue;
		WorldEntityProxy *proxy = memnew(WorldEntityProxy);
		proxy->set_name(preview_entity_name(record->get_kind(), record->get_index()));
		container->add_child(proxy);
		sync_preview_entity_proxy(proxy, record);
	}
}

TypedArray<WorldEntityProxy> GameWorld::get_preview_entity_proxies() const {
	TypedArray<WorldEntityProxy> out;
	Node3D *container = preview_entities();
	if (container == nullptr) return out;
	for (int i = 0; i < container->get_child_count(); ++i) {
		WorldEntityProxy *proxy = Object::cast_to<WorldEntityProxy>(container->get_child(i));
		if (proxy != nullptr) out.append(proxy);
	}
	return out;
}

WorldEntityProxy *GameWorld::get_preview_entity_proxy(int p_kind, int p_index) const {
	Node3D *container = preview_entities();
	if (container == nullptr) return nullptr;
	return Object::cast_to<WorldEntityProxy>(
			container->get_node_or_null(NodePath(preview_entity_name(p_kind, p_index))));
}

Error GameWorld::update_preview_entity(int p_kind, int p_index) {
	if (!preview_active_ || !world_ready_ || loaded_mission_.is_null() || placer_.is_null())
		return ERR_UNAVAILABLE;
	const Ref<MissionEntityRecord> record =
			loaded_mission_->get_entity(static_cast<MissionData::EntityKind>(p_kind), p_index);
	if (record.is_null()) return ERR_DOES_NOT_EXIST;
	WorldEntityProxy *proxy = get_preview_entity_proxy(p_kind, p_index);
	if (proxy == nullptr) return ERR_DOES_NOT_EXIST;
	// A different item is a different model: only a re-place projects it.
	if (proxy->get_item_id() != record->get_item_id()) return reload_preview_entities();
	const Transform3D xform = placer_->placement_transform(record->get_position(),
			record->get_rotation_deg(), record->get_item_id());
	const int bms_id = record->get_bms_id();
	if (bms_id != 0 && placer_->has_static_instance(bms_id)) {
		placer_->set_static_instance_transform(bms_id, xform);
	} else if (ObjectModel *model = placed_model_for(p_kind, p_index)) {
		// The individual model carries its own shadow casters as children;
		// only its terrain shadow source is a placer row.
		model->set_transform(xform);
		const Ref<EntityRef> ref = model->get_entity_ref();
		if (ref.is_valid()) ref->set_position(record->get_position());
		placer_->update_static_terrain_shadow_source_transform(
				static_cast<MissionData::EntityKind>(p_kind), p_index, xform);
	}
	sync_preview_entity_proxy(proxy, record);
	return OK;
}

Error GameWorld::reload_preview_entities() {
	if (!preview_active_ || !world_ready_ || loaded_mission_.is_null()) return ERR_UNAVAILABLE;
	// The same placement the load ran: place() clears the previous
	// MissionObjects, the director re-walks the placed lights, and the
	// clock sample stays at the authored time.
	place_mission_objects(loaded_mission_);
	if (light_director_.is_valid()) light_director_->reattach();
	panm_clock_->sample(0, -1);
	build_preview_entities();
	preview_diagnostics_.clear();
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
	// The preview table lives with the other two in game_world_frame.cpp
	// (ADR 0043 d9: one frame-leg home); its rows and omissions are
	// annotated there.
	FrameContext ctx;
	run_leg_table(kPreviewRefresh, kPreviewRefreshCount, ctx, false);
	return OK;
}

void GameWorld::unload_preview() {
	if (!preview_active_) return;
	free_preview_entities();
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
