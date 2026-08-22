#include "env/nova_slot_shadow.h"

#include <godot_cpp/classes/image.hpp>
#include <godot_cpp/classes/resource_loader.hpp>
#include <godot_cpp/classes/scene_tree.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/viewport.hpp>
#include <godot_cpp/classes/viewport_texture.hpp>
#include <godot_cpp/classes/visual_instance3d.hpp>
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/utility_functions.hpp>

#include <cmath>
#include <utility>
#include <vector>

#include "env/nova_mission_environment.h"
#include "env/nova_water.h"
#include "env/nova_weather.h"
#include "lights/nova_light_scene.h"
#include "object/nova_object_model.h"
#include "resource_index/nova_resource_root.h"
#include "terrain/nova_terrain_data.h"

namespace godot {

Ref<ShaderMaterial> SlotShadow::drape_material_;
Ref<ShaderMaterial> SlotShadow::blob_material_;
Ref<ImageTexture> SlotShadow::shadowztex_;

// Free visual layers reserved as per-slot capture channels (device plumbing;
// the 12-slot budget itself is the retail RT chain — render_slot_shadow.h).
static constexpr uint32_t kCaptureLayerBits[renderer::kSlotCaptureCount] = {
	1u << 1, 1u << 2, 1u << 3, 1u << 4, 1u << 5, 1u << 6, 1u << 7, 1u << 8,
	1u << 9, 1u << 17, 1u << 18, 1u << 19
};

static_assert(((1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) | (1u << 5) |
					   (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) |
					   (1u << 17) | (1u << 18) | (1u << 19)) ==
				uint32_t(Water::VISUAL_LAYER_SLOT_CAPTURE_MASK),
		"the capture layer bits must match the Water layer table");

const StringName &SlotShadow::caster_group() {
	static const StringName group("nova_slot_shadow_casters");
	return group;
}

uint32_t SlotShadow::capture_layer_bit(int p_order) {
	if (p_order < 0 || p_order >= renderer::kSlotCaptureCount) {
		return 0;
	}
	return kCaptureLayerBits[p_order];
}

uint32_t SlotShadow::capture_layer_mask() {
	uint32_t mask = 0;
	for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
		mask |= kCaptureLayerBits[i];
	}
	return mask;
}

// The per-slot uniform names, built once: the device names up to two per
// admitted slot every frame.
struct SlotUniformNames {
	StringName mat[renderer::kSlotCaptureCount];
	StringName tex[renderer::kSlotCaptureCount];
	SlotUniformNames() {
		for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
			mat[i] = StringName(vformat("u_slot_mat_%d", i));
			tex[i] = StringName(vformat("u_slot_tex_%d", i));
		}
	}
};

static SlotUniformNames &slot_uniforms() {
	static SlotUniformNames names;
	return names;
}

static void reset_material_slots(const Ref<ShaderMaterial> &p_material) {
	for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
		p_material->set_shader_parameter(slot_uniforms().mat[i], Projection());
	}
	PackedVector4Array terms;
	terms.resize(renderer::kSlotCaptureCount);
	p_material->set_shader_parameter("u_slot_term", terms);
}

Ref<ShaderMaterial> SlotShadow::get_drape_material() {
	if (drape_material_.is_valid()) {
		return drape_material_;
	}
	const Ref<Shader> shader = ResourceLoader::get_singleton()->load(
			"res://shaders/slot_shadow_drape.gdshader", "Shader");
	drape_material_.instantiate();
	drape_material_->set_shader(shader);
	reset_material_slots(drape_material_);
	blob_material_.instantiate();
	blob_material_->set_shader(shader);
	reset_material_slots(blob_material_);
	// The authored-blob pass chains behind the silhouette pass: retail draws
	// both legs over the same patches in the same drape walk
	// (retail: RenderSlot_DrawAllDrapes @0x5d6e54..0x5d6ec4, see
	// docs/render/render-lighting-re.md).
	drape_material_->set_next_pass(blob_material_);
	// The drape distance fade thresholds — renderer::drape_fade owns them.
	const Vector2 fade_range(renderer::kDrapeFadeStartUnits,
			renderer::kDrapeFadeEndUnits - renderer::kDrapeFadeStartUnits);
	drape_material_->set_shader_parameter("u_drape_fade_range", fade_range);
	blob_material_->set_shader_parameter("u_drape_fade_range", fade_range);
	// The depth-clip stage's texture: the witnessed 32x4 ARGB step, sampled
	// CLAMP + bilinear by the shader's sampler hints (retail:
	// shadow_system_init_resources @0x5d6260..0x5d62d7 — the planner carries
	// the fill law, renderer::shadowztex_pixels).
	const auto px = renderer::shadowztex_pixels();
	PackedByteArray bytes;
	bytes.resize(static_cast<int64_t>(px.size()) * 4);
	for (size_t i = 0; i < px.size(); ++i) {
		const uint32_t argb = px[i];
		bytes[static_cast<int64_t>(i) * 4 + 0] = static_cast<uint8_t>((argb >> 16) & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 1] = static_cast<uint8_t>((argb >> 8) & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 2] = static_cast<uint8_t>(argb & 0xFF);
		bytes[static_cast<int64_t>(i) * 4 + 3] = static_cast<uint8_t>((argb >> 24) & 0xFF);
	}
	const Ref<Image> image = Image::create_from_data(renderer::kShadowZTexWidth,
			renderer::kShadowZTexHeight, false, Image::FORMAT_RGBA8, bytes);
	shadowztex_ = ImageTexture::create_from_image(image);
	drape_material_->set_shader_parameter("u_shadowztex", shadowztex_);
	return drape_material_;
}

void SlotShadow::cleanup_statics() {
	drape_material_.unref();
	blob_material_.unref();
	shadowztex_.unref();
	// Release the uniform-name table before the engine tears the StringName
	// table down (the function-local static would otherwise outlive it).
	SlotUniformNames &names = slot_uniforms();
	for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
		names.mat[i] = StringName();
		names.tex[i] = StringName();
	}
}

SlotShadow::SlotShadow() {}

void SlotShadow::_bind_methods() {
	ClassDB::bind_method(D_METHOD("set_environment_node", "environment"),
			&SlotShadow::set_environment_node);
	ClassDB::bind_method(D_METHOD("set_terrain_data", "terrain"),
			&SlotShadow::set_terrain_data);
	ClassDB::bind_method(D_METHOD("set_light_scene", "scene"),
			&SlotShadow::set_light_scene);
	ClassDB::bind_method(
			D_METHOD("set_light_context", "gain", "time_ms", "weather"),
			&SlotShadow::set_light_context);
	ClassDB::bind_method(D_METHOD("set_resource_root", "root"),
			&SlotShadow::set_resource_root);
	ClassDB::bind_method(D_METHOD("set_shadow_detail", "detail"),
			&SlotShadow::set_shadow_detail);
	ClassDB::bind_method(D_METHOD("get_shadow_detail"),
			&SlotShadow::get_shadow_detail);
	ClassDB::bind_method(D_METHOD("set_local_player_model", "model"),
			&SlotShadow::set_local_player_model);
	ClassDB::bind_method(
			D_METHOD("set_local_player_first_person", "first_person"),
			&SlotShadow::set_local_player_first_person);
	ClassDB::bind_method(D_METHOD("set_local_player_prone", "prone"),
			&SlotShadow::set_local_player_prone);
	ClassDB::bind_method(D_METHOD("advance_frame"), &SlotShadow::advance_frame);
	ClassDB::bind_method(D_METHOD("get_report"), &SlotShadow::get_report);
	ClassDB::bind_static_method("SlotShadow", D_METHOD("get_drape_material"),
			&SlotShadow::get_drape_material);
}

void SlotShadow::set_environment_node(MissionEnvironment *p_environment) {
	environment_node_id_ = p_environment != nullptr
			? ObjectID(p_environment->get_instance_id())
			: ObjectID();
}

void SlotShadow::set_terrain_data(const Ref<TerrainData> &p_terrain) {
	terrain_data_ = p_terrain;
}

void SlotShadow::set_light_scene(const Ref<LightScene> &p_scene) {
	light_scene_ = p_scene;
}

void SlotShadow::set_light_context(const Vector3 &p_gain, int p_time_ms,
		Weather *p_weather) {
	light_gain_ = p_gain;
	light_time_ms_ = p_time_ms;
	weather_id_ = p_weather != nullptr ? ObjectID(p_weather->get_instance_id())
									   : ObjectID();
}

void SlotShadow::set_resource_root(const Ref<ResourceRoot> &p_root) {
	if (resource_root_ == p_root) {
		return;
	}
	resource_root_ = p_root;
	blob_textures_.clear();
}

void SlotShadow::set_shadow_detail(int p_detail) {
	shadow_detail_ = CLAMP(p_detail, 0, 4);
}

void SlotShadow::set_local_player_model(ObjectModel *p_model) {
	local_player_id_ = p_model != nullptr ? ObjectID(p_model->get_instance_id())
										  : ObjectID();
}

void SlotShadow::set_local_player_first_person(bool p_first_person) {
	local_first_person_ = p_first_person;
}

void SlotShadow::set_local_player_prone(bool p_prone) {
	local_prone_ = p_prone;
}

void SlotShadow::_notification(int p_what) {
	if (p_what == NOTIFICATION_READY) {
		_ensure_captures();
	}
}

void SlotShadow::_ensure_captures() {
	if (viewports_[0] != nullptr) {
		return;
	}
	for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
		SubViewport *viewport = memnew(SubViewport);
		viewport->set_name(vformat("SlotCapture%d", i));
		// The retail RT chain size for this slot order
		// (render_slot_shadow.h carries the witness).
		const int size = renderer::slot_texture_size(i, shadow_detail_);
		viewport->set_size(Vector2i(size, size));
		viewport->set_transparent_background(true);
		viewport->set_update_mode(SubViewport::UPDATE_DISABLED);
		viewport->set_disable_3d(false);
		viewport->set_use_own_world_3d(false);
		viewport->set_positional_shadow_atlas_size(0);
		add_child(viewport);
		Camera3D *camera = memnew(Camera3D);
		camera->set_name("Camera");
		camera->set_projection(Camera3D::PROJECTION_ORTHOGONAL);
		camera->set_cull_mask(kCaptureLayerBits[i]);
		viewport->add_child(camera);
		viewports_[i] = viewport;
		cameras_[i] = camera;
	}
	// Bind the capture textures to the drape pass once.
	const Ref<ShaderMaterial> drape = get_drape_material();
	for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
		drape->set_shader_parameter(slot_uniforms().tex[i],
				viewports_[i]->get_texture());
	}
}

void SlotShadow::_clear_all_terms() {
	const PackedVector4Array zero = [] {
		PackedVector4Array terms;
		terms.resize(renderer::kSlotCaptureCount);
		return terms;
	}();
	get_drape_material()->set_shader_parameter("u_slot_term", zero);
	blob_material_->set_shader_parameter("u_slot_term", zero);
}

void SlotShadow::_apply_capture_layers(ObjectModel *p_model, uint32_t p_bit) {
	if (p_model == nullptr) {
		return;
	}
	// Per-instance stamp over the model subtree; ObjectModel.rebuild()
	// recreates mesh children, so admitted models re-stamp every frame (the
	// same contract as the player presenter's layer stamps).
	struct Walker {
		static void walk(Node *node, uint32_t bit, uint32_t mask) {
			VisualInstance3D *visual = Object::cast_to<VisualInstance3D>(node);
			if (visual != nullptr) {
				visual->set_layer_mask(
						(visual->get_layer_mask() & ~mask) | bit);
			}
			for (int i = 0; i < node->get_child_count(); ++i) {
				walk(node->get_child(i), bit, mask);
			}
		}
	};
	Walker::walk(p_model, p_bit, capture_layer_mask());
}

Ref<Texture2D> SlotShadow::_blob_texture(const String &p_name) {
	if (p_name.is_empty()) {
		return Ref<Texture2D>();
	}
	const Ref<Texture2D> *cached = blob_textures_.getptr(p_name);
	if (cached != nullptr) {
		return *cached;
	}
	Ref<Texture2D> texture;
	if (resource_root_.is_valid()) {
		texture = resource_root_->load_texture(p_name);
	}
	blob_textures_[p_name] = texture;
	return texture;
}

// World -> (u, v, depth01) projector for a camera-style pose (local -Z
// forward): the drape samples the capture along the same slot direction it
// was rendered from (retail: the shared unscaled direction of the capture
// and drape matrices, setup_shadow_cascade_matrices @0x58d300 /
// build_shadow_cascade_uv_matrices @0x58cf10 lookat_dir1; the person 4x
// belongs to the separate depth-clip stage — render_slot_shadow.h).
Projection SlotShadow::_drape_projection(const Transform3D &p_pose,
		float p_half_u, float p_half_v, float p_far) const {
	const Transform3D view = p_pose.affine_inverse();
	const float inv_u = 1.0f / (2.0f * MAX(p_half_u, 0.001f));
	const float inv_v = 1.0f / (2.0f * MAX(p_half_v, 0.001f));
	// Columns (godot-cpp Projection(x, y, z, w) takes column vectors):
	// u = x*inv_u + 0.5, v = 0.5 - y*inv_v (image y-down), depth01 = -z / far.
	const Projection to_uv(
			Vector4(inv_u, 0, 0, 0),
			Vector4(0, -inv_v, 0, 0),
			Vector4(0, 0, -1.0f / MAX(p_far, 0.001f), 0),
			Vector4(0.5f, 0.5f, 0, 1));
	return to_uv * Projection(view);
}

void SlotShadow::advance_frame() {
	if (!is_inside_tree()) {
		return;
	}
	_ensure_captures();
	++frame_;
	const Ref<ShaderMaterial> drape = get_drape_material();
	MissionEnvironment *env = Object::cast_to<MissionEnvironment>(
			ObjectDB::get_instance(environment_node_id_));
	Viewport *viewport = get_viewport();
	Camera3D *camera =
			viewport != nullptr ? viewport->get_camera_3d() : nullptr;
	const bool live = env != nullptr && env->is_loaded() &&
			camera != nullptr && shadow_detail_ > 0;

	// Gather the caster group.
	std::vector<CasterInfo> casters;
	HashMap<uint64_t, size_t> caster_index;
	if (live) {
		TypedArray<Node> nodes =
				get_tree()->get_nodes_in_group(caster_group());
		casters.reserve(static_cast<size_t>(nodes.size()));
		for (int64_t i = 0; i < nodes.size(); ++i) {
			ObjectModel *model = Object::cast_to<ObjectModel>(
					static_cast<Object *>(nodes[i]));
			if (model == nullptr || !model->is_inside_tree()) {
				continue;
			}
			CasterInfo info;
			info.model = model;
			casters.push_back(info);
			caster_index[uint64_t(model->get_instance_id())] =
					casters.size() - 1;
		}
	}

	// Registration diff against the plan.
	std::vector<uint64_t> stale;
	for (const KeyValue<uint64_t, uint32_t> &entry : applied_bits_) {
		if (!caster_index.has(entry.key)) {
			stale.push_back(entry.key);
		}
	}
	for (uint64_t id : stale) {
		plan_.release_entity(id);
		ObjectModel *model = Object::cast_to<ObjectModel>(
				ObjectDB::get_instance(ObjectID(id)));
		if (model != nullptr) {
			_apply_capture_layers(model, 0);
		}
		applied_bits_.erase(id);
	}
	if (!live) {
		_clear_all_terms();
		for (int i = 0; i < renderer::kSlotCaptureCount; ++i) {
			viewports_[i]->set_update_mode(SubViewport::UPDATE_DISABLED);
		}
		report_bound_ = report_captures_ = report_blobs_ = 0;
		return;
	}

	const uint64_t local_id = uint64_t(local_player_id_);
	// The frame-open slot projection direction: get_light_direction now
	// serves the Godot-axes vector (the env_axes x/z swap IS the witnessed
	// (g2, g1, g0) surface->light mapping of the raw getter tuple), so the
	// planner law only clamps and negates it (retail: Environment_GetLight-
	// DirectionFloat @0x57d870 into render_shadow_pass @0x5d7b70, see
	// docs/render/render-lighting-re.md).
	const Vector3 tuple = env->get_light_direction();
	const std::array<float, 3> sun_dir = renderer::slot_projection_direction(
			{float(tuple.x), float(tuple.y), float(tuple.z)});
	const Vector3 default_dir =
			Vector3(sun_dir[0], sun_dir[1], sun_dir[2]).normalized();
	const Vector3 sun_rgb = env->get_sun_light();
	const Vector3 sky_rgb = env->get_sky_ambient();

	// Build per-caster planner state. capture_links collects (child, parent)
	// for models linked capture-with another caster.
	std::vector<std::pair<uint64_t, uint64_t>> capture_links;
	for (CasterInfo &info : casters) {
		ObjectModel *model = info.model;
		const uint64_t id = uint64_t(model->get_instance_id());
		plan_.register_entity(id);
		if (!applied_bits_.has(id)) {
			applied_bits_[id] = 0;
		}
		const Vector3 pos = model->get_global_position();
		// The two radii the slot reads: the model sphere (gpm[5]) sizes the
		// capture extent and the depth clip; the entity bound (entity+0 —
		// the sphere raised to the husk's, + 0x1000, and 0 without a
		// collision block) sizes the lod/patch and the light query (retail:
		// RenderSlot_RenderEntityAndChildren @0x5d7835 reads the model's
		// +0x14; RenderSlot_AllocSlot @0x5d5773 and the light query read
		// entity+0, Entity_InitFromModel @0x40dc30). A model the placer did
		// not stamp falls back to half its render-bounds diagonal for both.
		float capture_radius = model->get_model_sphere_radius();
		float slot_radius = model->get_entity_bound_radius();
		if (capture_radius <= 0.0f) {
			const AABB bounds = model->get_world_bounds();
			capture_radius = MAX(0.5f, float(bounds.size.length()) * 0.5f);
			slot_radius = capture_radius;
		}
		info.capture_radius = capture_radius;
		renderer::SlotCandidateState &state = info.state;
		state.pos2d = {float(pos.x), float(pos.z)};
		state.bound_radius = slot_radius;
		state.dead = !model->is_visible_in_tree();
		// A caster parented under another caster renders with its parent in
		// retail (the seat/standing child walk of the parent's slot RT);
		// its own slot is excluded.
		ObjectModel *capture_with = model->get_slot_shadow_capture_with();
		state.seat_parented = capture_with != nullptr;
		if (capture_with != nullptr) {
			capture_links.push_back(
					{ id, uint64_t(capture_with->get_instance_id()) });
		}
		for (Node *ancestor = model->get_parent();
				!state.seat_parented && ancestor != nullptr;
				ancestor = ancestor->get_parent()) {
			ObjectModel *parent_model = Object::cast_to<ObjectModel>(ancestor);
			if (parent_model != nullptr &&
					parent_model->is_in_group(caster_group())) {
				state.seat_parented = true;
				break;
			}
		}
		state.on_vehicle = false;
		state.is_local_player_or_parent = id == local_id;
		state.interior = false;
		state.dynamic = true;
		state.is_person = model->is_slot_shadow_person();
		state.has_blob_texture =
				!model->get_slot_shadow_decal_texture().is_empty();
	}

	const Vector3 cam_pos = camera->get_global_position();
	const Vector3 cam_forward =
			-camera->get_global_transform().basis.get_column(2);
	const std::array<float, 2> cam2d{float(cam_pos.x), float(cam_pos.z)};
	const std::array<float, 2> view2d{float(cam_forward.x),
			float(cam_forward.z)};
	const auto state_for = [&](uint64_t id) -> renderer::SlotCandidateState {
		const size_t *index = caster_index.getptr(id);
		if (index == nullptr) {
			renderer::SlotCandidateState gone;
			gone.dead = true;
			return gone;
		}
		return casters[*index].state;
	};
	const std::vector<renderer::SlotAssignment> assignments =
			plan_.assign(cam2d, view2d, state_for);

	PackedVector4Array silhouette_terms;
	silhouette_terms.resize(renderer::kSlotCaptureCount);
	PackedVector4Array blob_terms;
	blob_terms.resize(renderer::kSlotCaptureCount);
	// Per-slot drape patch (world min_x, min_z, max_x, max_z) and the
	// depth-clip texgen rows for the silhouette pass; the blob pass takes the
	// same patch (retail drapes both legs over the slot's patch).
	PackedVector4Array silhouette_patches;
	silhouette_patches.resize(renderer::kSlotCaptureCount);
	PackedVector4Array blob_patches;
	blob_patches.resize(renderer::kSlotCaptureCount);
	PackedVector4Array clip_u;
	clip_u.resize(renderer::kSlotCaptureCount);
	PackedVector4Array clip_v;
	clip_v.resize(renderer::kSlotCaptureCount);
	// The slot's lod x lod patch around the marched anchor
	// (renderer::slot_patch_bounds; the march probes TerrainData when set).
	// Planar mission north is Godot -z, so the patch's north range maps to
	// z in [-max_north, -min_north].
	const auto slot_patch = [&](const Vector3 &p_pos, const Vector3 &p_dir,
									float p_radius, float p_dir_y_raw) {
		const int base_lod = renderer::slot_lod_for_radius(p_radius);
		const int lod = renderer::grazing_slot_lod(base_lod, p_dir_y_raw);
		std::array<float, 2> anchor = {float(p_pos.x), float(p_pos.z)};
		if (terrain_data_.is_valid()) {
			const TerrainData *terrain = terrain_data_.ptr();
			anchor = renderer::march_shadow_anchor(
					{float(p_pos.x), float(p_pos.y), float(p_pos.z)},
					{float(p_dir.x), float(p_dir.y), float(p_dir.z)},
					[terrain](float x, float z) {
						return terrain->get_height_world(Vector3(x, 0.0f, z));
					});
		}
		const renderer::SlotPatch patch =
				renderer::slot_patch_bounds(anchor[0], -anchor[1], lod);
		return Vector4(patch.min_x, -patch.max_north, patch.max_x,
				-patch.min_north);
	};
	int blob_cursor = 0;
	report_bound_ = report_captures_ = report_blobs_ = 0;

	// The retail child walk: models linked capture-with an admitted caster
	// (held weapons, mounted children) render into the parent's slot RT
	// (retail: RenderSlot_RenderEntityAndChildren @0x5d78ef..0x5d79d6). A
	// linked child is excluded from its own slot and its own row lands
	// before or after the parent's in registration order, so resolve every
	// claim first: child id -> the parent's capture bit this frame.
	HashMap<uint64_t, uint32_t> claimed;
	HashSet<uint64_t> rowed;
	for (const renderer::SlotAssignment &assignment : assignments) {
		rowed.insert(assignment.id);
		if (!assignment.draws_silhouette || assignment.excluded) {
			continue;
		}
		const uint32_t bit = capture_layer_bit(assignment.capture_order);
		for (const std::pair<uint64_t, uint64_t> &link : capture_links) {
			if (link.second == assignment.id) {
				claimed[link.first] = bit;
			}
		}
	}

	for (const renderer::SlotAssignment &assignment : assignments) {
		const size_t *index = caster_index.getptr(assignment.id);
		if (index == nullptr) {
			continue;
		}
		CasterInfo &info = casters[*index];
		ObjectModel *model = info.model;
		const bool captures =
				assignment.draws_silhouette && !assignment.excluded;
		// Capture-layer churn: re-stamp admitted models every frame
		// (rebuild() resets children), clear once on the way out. A claimed
		// child carries its parent's bit, never its own excluded row's zero.
		const uint32_t *claim = claimed.getptr(assignment.id);
		const uint32_t want_bit = captures
				? capture_layer_bit(assignment.capture_order)
				: (claim != nullptr ? *claim : 0u);
		uint32_t &applied = applied_bits_[assignment.id];
		if (want_bit != 0 || applied != 0) {
			_apply_capture_layers(model, want_bit);
			applied = want_bit;
		}
		if (assignment.bound && !assignment.excluded) {
			++report_bound_;
		}
		// The dominant-light pick for every bound slot (the clamped sun by
		// default; the strongest nearby point light overrides) — retail runs
		// it from the entity update for silhouette and blob slots alike, so
		// the blob leg's patch follows the same stored direction (retail:
		// RenderSlot_UpdateEntityLight @0x5d6a30 <- Entity_UpdateAllEntities).
		const Vector3 center = model->get_world_bounds().get_center();
		renderer::SlotLightPick pick;
		pick.direction = {default_dir.x, default_dir.y, default_dir.z};
		pick.attached_handle = 0;
		Vector3 attached_color;
		float attached_atten = 0.0f;
		if (light_scene_.is_valid() && assignment.bound && !assignment.excluded) {
			Weather *weather = Object::cast_to<Weather>(
					ObjectDB::get_instance(weather_id_));
			light_scene_->slot_shadow_lights(center, info.state.bound_radius,
					light_gain_, light_time_ms_, weather, slot_lights_);
			pick = renderer::pick_dominant_light(
					{float(center.x), float(center.y), float(center.z)},
					{default_dir.x, default_dir.y, default_dir.z},
					slot_lights_.data(), slot_lights_.size(),
					info.state.interior);
			if (pick.attached_handle != 0) {
				for (const renderer::SlotPointLight &point : slot_lights_) {
					if (point.handle == pick.attached_handle) {
						attached_color = Vector3(point.color[0],
								point.color[1], point.color[2]);
						const float dx = center.x - point.position[0];
						const float dy = center.y - point.position[1];
						const float dz = center.z - point.position[2];
						const float d2 = dx * dx + dy * dy + dz * dz;
						attached_atten = 1.0f /
								MAX(0.001f,
										d2 * point.attenuation[2] +
												point.attenuation[0]);
						break;
					}
				}
			}
		}
		const Vector3 dir = Vector3(pick.direction[0], pick.direction[1],
				pick.direction[2])
									.normalized();
		// The slot direction as stored: the RAW clamped-negated sun, or the
		// unit attached-light direction — what the grazing rescale's vertical
		// and the depth clip read (retail: slot+0x68..0x70, RenderSlot_Update-
		// EntityLight @0x5d6d5c; RenderSlot_DrawSilhouetteDrape @0x5d5d66).
		const float dir_y_raw = pick.attached_handle != 0 ? dir.y : sun_dir[1];
		const std::array<float, 3> stored_dir = pick.attached_handle != 0
				? std::array<float, 3>{float(dir.x), float(dir.y), float(dir.z)}
				: sun_dir;
		if (!captures) {
			if (assignment.draws_blob && !assignment.excluded &&
					blob_cursor < renderer::kSlotCaptureCount) {
				// The authored items.def blob decal for a bound slot past
				// the capture budget: top-down, heading-rotated, sized
				// w x l with the authored UV offset (retail: the blob drape
				// @0x5d59d0 — 1/w 1/l UV scale, offset + 0.5 UV center).
				const Ref<Texture2D> texture = _blob_texture(
						model->get_slot_shadow_decal_texture());
				if (texture.is_valid()) {
					const Vector4 dims = model->get_slot_shadow_decal_dims();
					const float w = MAX(dims.x, 0.25f);
					const float l = MAX(dims.y, 0.25f);
					const Vector3 pos = model->get_global_position();
					const float heading =
							float(model->get_global_rotation().y);
					const Basis yaw(Vector3(0, 1, 0), heading);
					Transform3D projector;
					// Top-down projector: local -Z maps to world -Y.
					projector.basis =
							yaw * Basis(Vector3(1, 0, 0), -Math_PI / 2.0f);
					// The authored offset is in UV fractions of the decal.
					projector.origin = pos +
							yaw.xform(Vector3(dims.z * w, 0.0f, dims.w * l)) +
							Vector3(0.0f, 100.0f, 0.0f);
					const int slot = blob_cursor++;
					blob_material_->set_shader_parameter(
							slot_uniforms().tex[slot], texture);
					blob_material_->set_shader_parameter(
							slot_uniforms().mat[slot],
							_drape_projection(projector, w * 0.5f, l * 0.5f,
									200.0f));
					blob_terms[slot] = Vector4(0, 0, 0, 2.0f);
					blob_patches[slot] = slot_patch(pos, dir,
							info.state.bound_radius, dir_y_raw);
					++report_blobs_;
				}
			}
			continue;
		}

		// Capture camera along the slot direction, sized from the MODEL
		// sphere (retail: RenderSlot_RenderEntityAndChildren @0x5d7835 —
		// float24 = min(1.25 gpm[5], gpm[5] + 0.75)).
		const int order = assignment.capture_order;
		const float radius = info.capture_radius;
		const float half_extent = renderer::silhouette_half_extent(radius);
		const float cam_dist = radius * 2.0f + 2.0f;
		Camera3D *slot_camera = cameras_[order];
		Vector3 up = Vector3(0, 1, 0);
		if (std::fabs(dir.dot(up)) > 0.99f) {
			up = Vector3(1, 0, 0);
		}
		Transform3D pose;
		pose.origin = center - dir * cam_dist;
		pose = pose.looking_at(center, up);
		slot_camera->set_global_transform(pose);
		slot_camera->set_size(2.0f * half_extent);
		slot_camera->set_near(0.05f);
		slot_camera->set_far(cam_dist * 2.0f + radius);

		// The refresh cadence (renderer::slot_refresh_mask_for carries the
		// local-player exception).
		const uint32_t effective_mask = renderer::slot_refresh_mask_for(
				shadow_detail_, assignment.id == local_id);
		SubViewport *slot_viewport = viewports_[order];
		const int want_size =
				renderer::slot_texture_size(order, shadow_detail_);
		if (slot_viewport->get_size().x != want_size) {
			slot_viewport->set_size(Vector2i(want_size, want_size));
		}
		if (renderer::slot_refresh_due(assignment.record_index, frame_,
					effective_mask, assignment.capture_dirty)) {
			slot_viewport->set_update_mode(SubViewport::UPDATE_ONCE);
		}

		// The local player's first-person drape gate
		// (renderer::local_first_person_drape_skipped carries the law).
		if (assignment.id == local_id &&
				renderer::local_first_person_drape_skipped(local_first_person_,
						local_prone_, shadow_detail_)) {
			silhouette_terms[order] = Vector4();
			continue;
		}

		// Per-slot drape term.
		Vector3 q;
		if (pick.attached_handle != 0) {
			// The attached-light darkening folds the light's attenuation at
			// the entity into the constant term (retail lights the patch
			// with the negated color — drape_attached_light_scale; the
			// per-pixel attenuation across a patch is folded to its center).
			const std::array<float, 3> scale =
					renderer::drape_attached_light_scale(
							{float(attached_color.x), float(attached_color.y),
									float(attached_color.z)},
							0.0f);
			q = Vector3(CLAMP(-scale[0] * attached_atten, 0.0f, 1.0f),
					CLAMP(-scale[1] * attached_atten, 0.0f, 1.0f),
					CLAMP(-scale[2] * attached_atten, 0.0f, 1.0f));
		} else {
			const std::array<float, 3> term = renderer::drape_shadow_term(
					{float(sun_rgb.x), float(sun_rgb.y), float(sun_rgb.z)},
					{float(sky_rgb.x), float(sky_rgb.y), float(sky_rgb.z)},
					dir.y);
			q = Vector3(term[0], term[1], term[2]);
		}
		drape->set_shader_parameter(slot_uniforms().mat[order],
				_drape_projection(pose, half_extent, half_extent,
						slot_camera->get_far()));
		silhouette_terms[order] = Vector4(q.x, q.y, q.z, 1.0f);
		// The patch around the marched anchor, from the stored direction.
		const Vector3 entity_pos = model->get_global_position();
		silhouette_patches[order] =
				slot_patch(entity_pos, dir, info.state.bound_radius, dir_y_raw);
		// The depth-clip texgen from the stored direction, the capture half
		// size and the person steepening (renderer::slot_depth_clip).
		const renderer::SlotDepthClip clip = renderer::slot_depth_clip(
				stored_dir, half_extent, info.state.is_person,
				{float(entity_pos.x), float(entity_pos.y), float(entity_pos.z)});
		clip_u[order] = Vector4(clip.u_axis[0], clip.u_axis[1], clip.u_axis[2],
				clip.u_offset);
		clip_v[order] = Vector4(clip.v_axis[0], clip.v_axis[1], clip.v_axis[2],
				clip.v_offset);
		++report_captures_;
	}

	drape->set_shader_parameter("u_slot_term", silhouette_terms);
	drape->set_shader_parameter("u_slot_patch", silhouette_patches);
	drape->set_shader_parameter("u_slot_clip_u", clip_u);
	drape->set_shader_parameter("u_slot_clip_v", clip_v);
	blob_material_->set_shader_parameter("u_slot_term", blob_terms);
	blob_material_->set_shader_parameter("u_slot_patch", blob_patches);

	// Casters the full table refused (no assignment row of their own): a
	// linked child still rides its parent's slot RT — retail's child walk
	// follows the entity hierarchy, not the slot table (the
	// RenderSlot_RenderEntityAndChildren walk the claim pass above cites) —
	// and a row-less caster whose link dropped gives its inherited channel
	// back (no row of its own ever clears it).
	for (const CasterInfo &info : casters) {
		const uint64_t id = uint64_t(info.model->get_instance_id());
		if (rowed.has(id)) {
			continue;
		}
		const uint32_t *claim = claimed.getptr(id);
		const uint32_t want_bit = claim != nullptr ? *claim : 0u;
		uint32_t &applied = applied_bits_[id];
		if (want_bit == 0 && applied == 0) {
			continue;
		}
		_apply_capture_layers(info.model, want_bit);
		applied = want_bit;
	}
}

Dictionary SlotShadow::get_report() const {
	Dictionary report;
	report["bound"] = report_bound_;
	report["captures"] = report_captures_;
	report["blobs"] = report_blobs_;
	report["registered"] = int(plan_.registered_count());
	report["detail"] = shadow_detail_;
	return report;
}

}  // namespace godot
