// ObjectModel: the views drawing the world — the per-view RLOD walk that feeds
// the engine's projector, sub-pixel floor and level selector once per view
// (the frame's image, and while it renders the weapon Inset pass), the Inset
// view's own verdicts, and the twin RenderingServer instances that draw them
// where they differ from the main view's (docs/render/render-occlusion-re.md
// §8a; the engine's world/occlusion.h OcclusionView carries the witness that
// retail runs the whole scene pass, collector to RLOD walk, per view). Split
// out of object_model.cpp: one responsibility, the views.

#include "object/object_model.h"

#include "object/object_shader_cache.h"
#include "object/post_multiply_draw.h"
#include "render/object_lod_frame.h"

#include <godot_cpp/classes/camera3d.hpp>
#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/classes/world3d.hpp>
#include <godot_cpp/templates/local_vector.hpp>

#include <runtime/renderer/object_lod.h>
#include <runtime/renderer/render_order.h>

#include <algorithm>

namespace godot {

HashSet<ObjectModel *> ObjectModel::inset_view_models_;
bool ObjectModel::inset_view_open_ = false;

namespace {

constexpr int kMainView = 0;
constexpr int kInsetView = 1;
constexpr int kViewCount = 2;

// The instance uniforms the object shader family declares
// (shaders/object/shared.gdshaderinc, foliage_mask.gdshaderinc,
// viewmodel_pass.gdshaderinc): what a twin mirrors from the node instance it
// stands in for. Built once and never destroyed (a static with a Godot
// destructor would run after the extension's hooks are gone).
constexpr int kTwinUniformCount = 16;
const StringName *twin_uniform_names() {
	static StringName *names = nullptr;
	if (names == nullptr) {
		names = memnew_arr(StringName, kTwinUniformCount);
		const char *const raw[kTwinUniformCount] = {
			"u_entity_light", "u_point_light_count",
			"u_point_light_posr_0", "u_point_light_posr_1", "u_point_light_posr_2",
			"u_point_light_posr_3", "u_point_light_color_0", "u_point_light_color_1",
			"u_point_light_color_2", "u_point_light_color_3", "u_match_terrain_enabled",
			"u_match_terrain_page_ready", "u_match_terrain_page_layer",
			"u_match_terrain_page_projection", "u_viewmodel_pass", "u_foliage_mask_side",
		};
		for (int i = 0; i < kTwinUniformCount; ++i) {
			names[i] = StringName(raw[i]);
		}
	}
	return names;
}

} // namespace

int ObjectModel::update_authored_lods(const Transform3D &p_camera_transform,
		float p_vertical_fov_degrees,
		float p_viewport_width,
		float p_viewport_height) {
	// The frame scale, the projected radius and the selector are engine facts
	// (runtime/renderer/object_lod.h); the frame struct converts the camera.
	const ObjectLodFrame frame = ObjectLodFrame::make(p_camera_transform,
			p_vertical_fov_degrees, p_viewport_width, p_viewport_height);
	return update_authored_lod_views(&frame, 1);
}

int ObjectModel::update_authored_lods_for_camera(Camera3D *p_camera, float p_viewport_width) {
	const ObjectLodFrame frame = ObjectLodFrame::from_camera(p_camera, p_viewport_width);
	return update_authored_lod_views(&frame, 1);
}

int ObjectModel::update_authored_lods_for_views(Camera3D *p_main, float p_main_width,
		Camera3D *p_inset, float p_inset_width) {
	ObjectLodFrame frames[kViewCount];
	frames[kMainView] = ObjectLodFrame::from_camera(p_main, p_main_width);
	int count = 1;
	if (p_inset != nullptr) {
		frames[kInsetView] = ObjectLodFrame::from_camera(p_inset, p_inset_width);
		count = kViewCount;
	}
	const int applied = update_authored_lod_views(frames, count);
	sync_view_twins();
	return applied;
}

// [engine: renderer::select_object_lod, object_subpixel_culled and
//  project_bound_sphere_radius_q16 own the witnessed rules — the sector-entity
//  draw returns before the RLOD walk below 0.75 px (retail render_sector_entity
//  @ 0x5c42d8..0x5c42de); this walk feeds them each registered model per
//  view, on that view's own frame scale]
int ObjectModel::update_authored_lod_views(const ObjectLodFrame *p_frames,
		int p_frame_count) {
	const int view_count =
			p_frames == nullptr ? 0 : std::clamp(p_frame_count, 0, kViewCount);
	const bool inset = view_count > kInsetView && p_frames[kInsetView].valid;
	if (inset != inset_view_open_) {
		set_inset_view_open(inset);
	}
	if ((authored_lod_models_.is_empty() && pixel_cull_models_.is_empty()) ||
			view_count == 0) {
		refresh_inset_views();
		return 0;
	}
	bool any_valid = false;
	for (int v = 0; v < view_count; ++v) {
		any_valid = any_valid || p_frames[v].valid;
	}
	if (!any_valid) {
		refresh_inset_views();
		return 0;
	}
	// The held weapon's own 2 px gate, per view: its model sphere projected
	// at its attach point, the raw radius against 0x20000. The node leaves the
	// main camera when the main view culls it; the Inset view keeps its own
	// verdict. A sphere a view does not see is not drawn by that camera either
	// way. [retail BoneCallback_org0_World @ 0x4e3d07..0x4e3d4b]
	for (ObjectModel *model : pixel_cull_models_) {
		bool seen[kViewCount] = {};
		bool drawn[kViewCount] = {};
		if (model->is_inside_tree()) {
			const Vector3 origin = model->get_global_transform().origin;
			for (int v = 0; v < view_count; ++v) {
				int32_t projected_q16 = 0;
				if (!p_frames[v].valid || !p_frames[v].project_q16(origin,
								model->attachment_pixel_cull_radius_q16_, projected_q16)) {
					continue;
				}
				seen[v] = true;
				drawn[v] = !opennova::renderer::held_weapon_projection_culled(projected_q16);
			}
		}
		model->set_camera_pixel_culled(seen[kMainView] && !drawn[kMainView]);
		if (inset && seen[kInsetView]) {
			model->inset_camera_pixel_culled_ = !drawn[kInsetView];
			model->inset_own_ |= kInsetOwnPixel;
			model->mark_inset_view();
		}
	}
	if (authored_lod_models_.is_empty()) {
		refresh_inset_views();
		return 0;
	}
	// The cheap math runs over the registered set in place; visibility and
	// level changes are applied after the walk so a visibility notification or
	// set_active_lod's runtime-state refresh never runs against the set being
	// iterated. Nothing allocates while no model crosses a threshold.
	struct LodSwitch {
		ObjectModel *model = nullptr;
		int lod_index = 0;
	};
	struct SubpixelChange {
		ObjectModel *model = nullptr;
		bool hidden = false;
	};
	// Frame scratch that keeps its capacity across calls (deliberately never
	// freed: a static with a Godot allocator destructor would run after the
	// extension's allocator hooks are gone), so a frame with attachments or
	// crossings allocates nothing once warm.
	static LocalVector<LodSwitch> &switches = *memnew(LocalVector<LodSwitch>);
	static LocalVector<SubpixelChange> &subpixel_changes =
			*memnew(LocalVector<SubpixelChange>);
	switches.clear();
	subpixel_changes.clear();
	static uint64_t projection_frame = 0;
	++projection_frame;
	// Attachments take their owner's level (and sub-pixel verdict) after the
	// owners' own selections have been applied (renderer::attachment_lod_index).
	static LocalVector<ObjectModel *> &attachments =
			*memnew(LocalVector<ObjectModel *>);
	attachments.clear();
	for (ObjectModel *model : authored_lod_models_) {
		if (!model->is_inside_tree()) {
			continue;
		}
		if (!model->authored_lod_owner_.is_null()) {
			attachments.push_back(model);
			continue;
		}
		ObjectModel *source = model->get_authored_lod_projection_owner();
		if (source == nullptr) source = model;
		if (source->lod_projection_frame_ != projection_frame) {
			source->lod_projection_frame_ = projection_frame;
			const Transform3D world = source->get_global_transform();
			const auto &sphere = source->entity_projection_sphere_;
			for (int v = 0; v < view_count; ++v) {
				const ObjectLodFrame &frame = p_frames[v];
				if (!frame.valid) {
					source->lod_projection_visible_[v] = false;
					continue;
				}
				if (sphere.valid) {
					const Vector3 center = ObjectLodFrame::projection_center(
							world, sphere, source->entity_projection_scale_q16_);
					source->lod_projection_visible_[v] = frame.project_q16(center,
							sphere.radius_q16, source->lod_projected_radius_q16_[v]);
				} else {
					// Document-less previews have no entity collision-bound producer.
					const float radius = (source->model_sphere_radius_ > 0.0f
							? source->model_sphere_radius_
							: source->model_bounds_.get_longest_axis_size() * 0.5f) *
							ObjectLodFrame::uniform_scale(world.basis);
					source->lod_projection_visible_[v] = frame.project(
							world.origin, radius, source->lod_projected_radius_q16_[v]);
				}
			}
		}
		// Each view selects on its own frame scale. A view that rejected the
		// entity never reaches its selector, and an entity a view does not see
		// keeps that view's level and sub-pixel verdict. View 0 decides the
		// node; view 1 is the Inset state a twin draws where it differs.
		for (int v = 0; v < view_count; ++v) {
			if (!source->lod_projection_visible_[v]) {
				continue;
			}
			const int32_t projected_q16 = source->lod_projected_radius_q16_[v];
			const bool above_floor = !opennova::renderer::object_subpixel_culled(projected_q16);
			int lod_index = -1;
			if (above_floor) {
				lod_index = opennova::renderer::select_object_lod(
						model->authored_lod_thresholds_q16_, projected_q16,
						p_frames[v].projection_scale, model->authored_lod_available_)
									.lod_index;
			}
			if (v == kMainView) {
				if (above_floor == model->subpixel_hidden_) {
					subpixel_changes.push_back(SubpixelChange{ model, !above_floor });
				}
				if (lod_index >= 0 && lod_index != model->active_lod_) {
					switches.push_back(LodSwitch{ model, lod_index });
				}
				continue;
			}
			if (lod_index >= 0) {
				model->inset_lod_ = lod_index;
			} else if ((model->inset_own_ & kInsetOwnLod) == 0) {
				model->inset_lod_ = model->active_lod_;
			}
			model->inset_subpixel_hidden_ = !above_floor;
			model->inset_own_ |= kInsetOwnLod;
			model->mark_inset_view();
		}
	}
	for (const SubpixelChange &change : subpixel_changes) {
		if (authored_lod_models_.has(change.model)) {
			change.model->set_subpixel_hidden(change.hidden);
		}
	}
	int applied = 0;
	for (const LodSwitch &change : switches) {
		// A switch applied earlier in this loop can unregister or free another
		// queued model (set_active_lod's runtime-state refresh reaches child
		// nodes); only a still-registered model is dereferenced. The
		// tree-visibility walk only for the models that actually cross: a
		// hidden model re-selects on the frame it becomes visible.
		if (!authored_lod_models_.has(change.model) ||
				!change.model->is_visible_in_tree()) {
			continue;
		}
		change.model->set_active_lod(change.lod_index);
		++applied;
	}
	for (ObjectModel *attachment : attachments) {
		if (!authored_lod_models_.has(attachment)) {
			continue;
		}
		const ObjectModel *owner = attachment->get_authored_lod_owner();
		const int threshold_count =
				static_cast<int>(attachment->authored_lod_thresholds_q16_.size());
		// Retail draws an attachment inside its owner's bone callback, so the
		// owner's sub-pixel return drops it too.
		attachment->set_subpixel_hidden(owner != nullptr && owner->subpixel_hidden_);
		const int level = attachment->exact_owner_lod_ && owner != nullptr
				? owner->active_lod_
				: opennova::renderer::attachment_lod_index(
						  owner != nullptr ? owner->active_lod_ : 0, threshold_count);
		if (level >= 0 && level != attachment->active_lod_) {
			attachment->set_active_lod(level);
			++applied;
		}
		if (!inset) {
			continue;
		}
		// The owner's Inset verdicts, as its main ones above.
		const bool owner_own = owner != nullptr && (owner->inset_own_ & kInsetOwnLod) != 0;
		const int owner_level = owner == nullptr ? 0
				: owner_own ? owner->inset_lod_
							: owner->active_lod_;
		const int inset_level = attachment->exact_owner_lod_ && owner != nullptr
				? owner_level
				: opennova::renderer::attachment_lod_index(owner_level, threshold_count);
		attachment->inset_lod_ = inset_level >= 0 ? inset_level : attachment->active_lod_;
		attachment->inset_subpixel_hidden_ = owner != nullptr &&
				(owner_own ? owner->inset_subpixel_hidden_ : owner->subpixel_hidden_);
		attachment->inset_own_ |= kInsetOwnLod;
		attachment->mark_inset_view();
	}
	refresh_inset_views();
	return applied;
}

void ObjectModel::mark_inset_view() {
	inset_view_models_.insert(this);
}

void ObjectModel::set_inset_view_open(bool p_open) {
	inset_view_open_ = p_open;
	if (p_open) {
		return;
	}
	// The Inset stopped rendering: every Inset verdict releases and every
	// split model converges onto its one node.
	LocalVector<ObjectModel *> models;
	models.reserve(inset_view_models_.size());
	for (ObjectModel *model : inset_view_models_) {
		models.push_back(model);
	}
	for (ObjectModel *model : models) {
		model->inset_own_ = 0;
		model->refresh_view_split();
	}
	inset_view_models_.clear();
}

void ObjectModel::refresh_inset_views() {
	if (inset_view_models_.is_empty()) {
		return;
	}
	static LocalVector<ObjectModel *> &models = *memnew(LocalVector<ObjectModel *>);
	models.clear();
	for (ObjectModel *model : inset_view_models_) {
		models.push_back(model);
	}
	for (ObjectModel *model : models) {
		if (inset_view_models_.has(model)) {
			model->refresh_view_split();
		}
	}
}

void ObjectModel::set_inset_occlusion_hidden(bool p_hidden) {
	inset_occlusion_hidden_ = p_hidden;
	inset_own_ |= kInsetOwnOcclusion;
	mark_inset_view();
}

void ObjectModel::set_inset_occlusion_section_mask(int64_t p_raw_mask,
		int64_t /*p_forced_mask: the def's, shared with the main view*/) {
	inset_section_mask_ = p_raw_mask;
	inset_own_ |= kInsetOwnSections;
	mark_inset_view();
}

void ObjectModel::set_inset_view_lod(int p_lod_index) {
	inset_lod_ = exact_owner_lod_ ? p_lod_index : clamp_lod_index(p_lod_index);
	inset_subpixel_hidden_ = false;
	inset_own_ |= kInsetOwnLod;
	mark_inset_view();
}

void ObjectModel::clear_inset_occlusion() {
	inset_own_ &= static_cast<uint8_t>(~(kInsetOwnOcclusion | kInsetOwnSections));
	if (inset_view_models_.has(this)) {
		refresh_view_split();
	}
}

int ObjectModel::get_inset_view_lod() const {
	return (inset_own_ & kInsetOwnLod) != 0 ? inset_lod_ : active_lod_;
}

bool ObjectModel::is_inset_view_subpixel_hidden() const {
	return (inset_own_ & kInsetOwnLod) != 0 ? inset_subpixel_hidden_ : subpixel_hidden_;
}

bool ObjectModel::inset_section_part_visible(int64_t p_mask, int p_section) const {
	const uint32_t bit = 1u << (static_cast<uint32_t>(p_section) & 31u);
	if ((destroyed_section_mask_ & bit) != 0) {
		return false;
	}
	if (p_mask == -1) {
		return true;
	}
	return ((static_cast<uint32_t>(p_mask) | forced_section_mask_) & bit) != 0;
}

// Reconcile the two views' verdicts. Equal: one node on the world bits, no
// twin. Different: the node keeps the main view's on the main-view bits, and
// while the Inset draws the model a twin per drawn surface of the Inset's
// level draws its level and sections on the Inset bit. A world model only
// (the viewmodel and the hidden first-person body never reach the Inset).
void ObjectModel::refresh_view_split() {
	bool split = false;
	bool inset_draws = false;
	int inset_level = active_lod_;
	int64_t inset_mask = occlusion_section_mask_;
	if (inset_view_open_ && is_inside_tree() &&
			(presentation_layer_ == PRESENTATION_LAYER_WORLD ||
					presentation_layer_ == PRESENTATION_LAYER_LOCAL_BODY)) {
		const Node3D *parent = get_parent_node_3d();
		const bool ancestors = parent == nullptr || parent->is_visible_in_tree();
		inset_level = (inset_own_ & kInsetOwnLod) != 0 ? inset_lod_ : active_lod_;
		inset_mask = get_inset_view_section_mask();
		const bool main_draws = ancestors && is_visible() && !camera_pixel_culled_;
		inset_draws = inset_view_draws();
		split = main_draws != inset_draws ||
				(inset_draws && (inset_level != active_lod_ ||
										inset_mask != occlusion_section_mask_));
	}
	if (split != view_split_) {
		view_split_ = split;
		apply_view_split_layers();
	}
	if (!split || !inset_draws || !geometry_visible_) {
		free_view_twins();
	} else if (view_twins_.empty() || view_twin_lod_ != inset_level ||
			view_twin_mask_ != inset_mask || view_twin_serial_ != scene_build_serial_ ||
			view_twin_destroyed_ != destroyed_section_mask_) {
		build_view_twins(inset_level, inset_mask);
	}
	if (!view_split_ && view_twins_.empty() && (!inset_view_open_ || inset_own_ == 0)) {
		inset_view_models_.erase(this);
	}
}

// The Inset pass's verdicts for the node, under ancestors that draw there: a
// part model under a body the main view hid (the avatar head) follows the
// body's Inset verdicts, as retail draws it inside the body's own submit;
// any other hidden ancestor hides it.
bool ObjectModel::inset_view_draws() const {
	if (!inset_view_open_ || !is_inside_tree() ||
			(presentation_layer_ != PRESENTATION_LAYER_WORLD &&
					presentation_layer_ != PRESENTATION_LAYER_LOCAL_BODY)) {
		return false;
	}
	const Node3D *parent = get_parent_node_3d();
	bool ancestors = parent == nullptr || parent->is_visible_in_tree();
	if (!ancestors) {
		const ObjectModel *owner = Object::cast_to<ObjectModel>(parent);
		ancestors = owner != nullptr && owner->inset_view_draws();
	}
	// A node another owner hid directly (its own flag down while the main
	// view's verdicts would draw it) draws in neither view.
	const bool main_verdicts = present_visible_ && !occlusion_hidden_ && !subpixel_hidden_;
	const bool external_hidden = !is_visible() && main_verdicts;
	const bool inset_hidden = (inset_own_ & kInsetOwnOcclusion) != 0
			? inset_occlusion_hidden_ : occlusion_hidden_;
	const bool inset_subpixel = (inset_own_ & kInsetOwnLod) != 0
			? inset_subpixel_hidden_ : subpixel_hidden_;
	const bool inset_pixel = (inset_own_ & kInsetOwnPixel) != 0
			? inset_camera_pixel_culled_ : camera_pixel_culled_;
	return ancestors && !external_hidden && present_visible_ && !inset_hidden &&
			!inset_subpixel && !inset_pixel;
}

Variant ObjectModel::get_view_twin_shader_parameter(int p_index, const StringName &p_name) const {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || p_index < 0 || static_cast<std::size_t>(p_index) >= view_twins_.size()) {
		return Variant();
	}
	return rs->instance_geometry_get_shader_parameter(
			view_twins_[static_cast<std::size_t>(p_index)].instance, p_name);
}

bool ObjectModel::is_inset_view_drawn() const {
	if (!inset_view_open_ || !is_inside_tree()) {
		return false;
	}
	return view_split_ ? !view_twins_.empty() : is_visible_in_tree() && !camera_pixel_culled_;
}

int64_t ObjectModel::get_inset_view_section_mask() const {
	return (inset_own_ & kInsetOwnSections) != 0 ? inset_section_mask_ : occlusion_section_mask_;
}

bool ObjectModel::is_inset_view_level_skinned() const {
	const int level = view_twin_lod_ >= 0 ? view_twin_lod_ : active_lod_;
	return object_data_.is_valid() && object_data_->is_skinned(level);
}

void ObjectModel::collect_inset_point_light_draw_parts(std::vector<int32_t> &r_robjs) const {
	r_robjs.clear();
	if (view_twin_lod_ < 0 || static_cast<std::size_t>(view_twin_lod_) >= level_surfaces_.size()) {
		return;
	}
	const std::vector<LevelSurface> &level = level_surfaces_[static_cast<std::size_t>(view_twin_lod_)];
	for (const ViewTwin &twin : view_twins_) {
		if (twin.surface < 0 || static_cast<std::size_t>(twin.surface) >= level.size()) {
			continue;
		}
		const LevelSurface &surface = level[static_cast<std::size_t>(twin.surface)];
		if (!surface.is_skinned &&
				std::find(r_robjs.begin(), r_robjs.end(), surface.robj_index) == r_robjs.end()) {
			r_robjs.push_back(surface.robj_index);
		}
	}
	std::sort(r_robjs.begin(), r_robjs.end());
}

void ObjectModel::apply_inset_point_light_selection(int p_count, const Vector4 *p_posr,
		const Vector4 *p_color) {
	write_twin_point_lights(true, 0, p_count, p_posr, p_color);
}

void ObjectModel::apply_inset_point_light_selection_to_robj(int p_robj_index, int p_count,
		const Vector4 *p_posr, const Vector4 *p_color) {
	write_twin_point_lights(false, p_robj_index, p_count, p_posr, p_color);
}

void ObjectModel::write_twin_point_lights(bool p_all, int p_robj_index, int p_count,
		const Vector4 *p_posr, const Vector4 *p_color) {
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr || view_twin_lod_ < 0 ||
			static_cast<std::size_t>(view_twin_lod_) >= level_surfaces_.size()) {
		return;
	}
	// The node's packed form (apply_point_light_selection_to_robj): the
	// count, then four (posr, colour) pairs, zero past the count.
	const int count = CLAMP(p_count, 0, 4);
	uint64_t hash = 0xcbf29ce484222325ull;
	const auto mix = [&hash](const void *data, size_t size) {
		const uint8_t *bytes = static_cast<const uint8_t *>(data);
		for (size_t i = 0; i < size; ++i) {
			hash = (hash ^ bytes[i]) * 0x100000001b3ull;
		}
	};
	mix(&count, sizeof(count));
	for (int i = 0; i < count; ++i) {
		mix(&p_posr[i], sizeof(Vector4));
		mix(&p_color[i], sizeof(Vector4));
	}
	// twin_uniform_names(): 1 the count, 2..5 the posr, 6..9 the colours.
	const StringName *names = twin_uniform_names();
	const std::vector<LevelSurface> &level = level_surfaces_[static_cast<std::size_t>(view_twin_lod_)];
	for (ViewTwin &twin : view_twins_) {
		if (twin.surface < 0 || static_cast<std::size_t>(twin.surface) >= level.size()) {
			continue;
		}
		const LevelSurface &surface = level[static_cast<std::size_t>(twin.surface)];
		if (!p_all && (surface.is_skinned ? p_robj_index != -1
										  : surface.robj_index != p_robj_index)) {
			continue;
		}
		if (twin.own_lights && twin.lights_hash == hash) {
			continue;
		}
		twin.own_lights = true;
		twin.lights_hash = hash;
		rs->instance_geometry_set_shader_parameter(twin.instance, names[1],
				static_cast<float>(count));
		for (int i = 0; i < 4; ++i) {
			rs->instance_geometry_set_shader_parameter(twin.instance, names[2 + i],
					i < count ? p_posr[i] : Vector4());
			rs->instance_geometry_set_shader_parameter(twin.instance, names[6 + i],
					i < count ? p_color[i] : Vector4());
		}
	}
}

// The retained surface slots take the split decision (presentation_layer_mask
// swaps the world bits for the main-view bits); nothing else under the model
// is touched (the placer's level-bound shadow siblings keep their own layer).
void ObjectModel::apply_view_split_layers() {
	for (const SurfaceSlot &slot : surface_slots_) {
		if (slot.instance != nullptr) {
			slot.instance->set_layer_mask(presentation_layer_mask(false));
		}
		if (slot.auxiliary != nullptr) {
			slot.auxiliary->set_layer_mask(presentation_layer_mask(true));
		}
	}
}

void ObjectModel::build_view_twins(int p_level, int64_t p_mask) {
	free_view_twins();
	if (p_level < 0 || static_cast<std::size_t>(p_level) >= level_surfaces_.size() ||
			!is_inside_tree()) {
		return;
	}
	const Ref<World3D> world = get_world_3d();
	RenderingServer *rs = RenderingServer::get_singleton();
	if (world.is_null() || rs == nullptr) {
		return;
	}
	const RID scenario = world->get_scenario();
	const std::vector<LevelSurface> &level = level_surfaces_[static_cast<std::size_t>(p_level)];
	bool rigid = false;
	for (std::size_t k = 0; k < level.size(); ++k) {
		const LevelSurface &surface = level[k];
		if (surface.mesh.is_null()) {
			continue;
		}
		// A rigid strip draws only while its part's section is drawn in that
		// view; a skinned strip hangs under the skeleton, which no section
		// hides (apply_section_visibility).
		if (!surface.is_skinned && !inset_section_part_visible(p_mask, surface.robj_index)) {
			continue;
		}
		if (surface.is_skinned && skeleton_ != nullptr && view_twin_skin_.is_null() &&
				skeleton_skin_.is_valid()) {
			view_twin_skin_ = skeleton_->register_skin(skeleton_skin_);
		}
		for (int pass = 0; pass < 2; ++pass) {
			const Ref<ShaderMaterial> &material =
					pass == 0 ? surface.material : surface.auxiliary_material;
			if (pass == 1 && material.is_null()) {
				continue;
			}
			ViewTwin twin;
			twin.surface = static_cast<int>(k);
			twin.auxiliary = pass == 1;
			twin.instance = rs->instance_create();
			rs->instance_set_base(twin.instance, surface.mesh->get_rid());
			rs->instance_set_scenario(twin.instance, scenario);
			rs->instance_set_layer_mask(twin.instance, LAYER_INSET_VIEW);
			rs->instance_geometry_set_cast_shadows_setting(
					twin.instance, RenderingServer::SHADOW_CASTING_SETTING_OFF);
			if (material.is_valid()) {
				rs->instance_geometry_set_material_override(twin.instance, material->get_rid());
			}
			if (surface.is_skinned && view_twin_skin_.is_valid()) {
				rs->instance_attach_skeleton(twin.instance, view_twin_skin_->get_skeleton());
			}
			view_twins_.push_back(twin);
			++live_geometry_instance_count_;
		}
		rigid = rigid || !surface.is_skinned;
	}
	view_twin_lod_ = p_level;
	view_twin_mask_ = p_mask;
	view_twin_serial_ = scene_build_serial_;
	view_twin_destroyed_ = destroyed_section_mask_;
	// The twin level's dynamic materials the node's level does not push (its
	// alpha-strip duplicates, rows only that level uses): classified like the
	// node's own (classify_materials) and pushed by sync_view_twin.
	for (const ViewTwin &twin : view_twins_) {
		if (twin.auxiliary) {
			continue;
		}
		const LevelSurface &surface = level[static_cast<std::size_t>(twin.surface)];
		if (surface.material.is_null()) {
			continue;
		}
		bool node_pushes = false;
		for (int i = 0; i < surface_materials_.size() && !node_pushes; ++i) {
			node_pushes = surface_materials_[i] == surface.material;
		}
		bool listed = false;
		for (const TwinDynamicMaterial &row : view_twin_dynamic_) {
			listed = listed || row.material == surface.material;
		}
		if (node_pushes || listed) {
			continue;
		}
		const bool needs_eval = material_runtime_is_dynamic(surface.material_index);
		const Array *frames = anim_frames_by_mat_.getptr(surface.material_index);
		if (!needs_eval && (frames == nullptr || frames->size() <= 1)) {
			continue;
		}
		TwinDynamicMaterial row;
		row.material = surface.material;
		row.material_index = surface.material_index;
		row.needs_eval = needs_eval;
		view_twin_dynamic_.push_back(row);
	}
	// A level the node does not show poses its parts on nodes of its own
	// (outside the tree), written through the same per-graphic PANM cache
	// the node's own parts read.
	if (rigid && p_level != active_lod_ && object_data_.is_valid() &&
			object_data_->has_document()) {
		const opennova::threedi::Threedi3di3 &model = object_data_->native_model();
		const std::size_t parts = static_cast<std::size_t>(p_level) < model.lod_count &&
						model.lods != nullptr
				? model.lods[p_level].render_object_count
				: 0;
		for (std::size_t i = 0; i < parts; ++i) {
			view_twin_pose_nodes_.push_back(memnew(Node3D));
		}
		view_twin_pose_revision_ = 0;
	}
	sync_view_twin();
	sync_inset_section_twins(!view_twins_.empty());
}

void ObjectModel::attach_inset_section_twin(const RID &p_instance, int p_section) {
	if (!p_instance.is_valid()) {
		return;
	}
	bool listed = false;
	for (InsetSectionTwin &twin : inset_section_twins_) {
		if (twin.instance == p_instance) {
			twin.section = p_section;
			listed = true;
		}
	}
	if (!listed) {
		InsetSectionTwin twin;
		twin.instance = p_instance;
		twin.section = p_section;
		inset_section_twins_.push_back(twin);
	}
	sync_inset_section_twins(!view_twins_.empty());
}

void ObjectModel::detach_inset_section_twin(const RID &p_instance) {
	for (std::size_t i = 0; i < inset_section_twins_.size(); ++i) {
		if (inset_section_twins_[i].instance == p_instance) {
			inset_section_twins_.erase(inset_section_twins_.begin() +
					static_cast<std::ptrdiff_t>(i));
			return;
		}
	}
}

// The section-bound instances ride this model's Inset part poses: the part
// node's pose where the twin shows the node's level, else the twin level's
// own pose nodes; a section the Inset does not draw hides them.
void ObjectModel::sync_inset_section_twins(bool p_drawn) {
	if (inset_section_twins_.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	const bool node_pose = view_twin_lod_ == active_lod_ || rigid_parts_;
	const Transform3D model_xform = is_inside_tree() ? get_global_transform() : Transform3D();
	for (const InsetSectionTwin &twin : inset_section_twins_) {
		const bool drawn = p_drawn && is_inside_tree() &&
				inset_section_part_visible(view_twin_mask_, twin.section);
		rs->instance_set_visible(twin.instance, drawn);
		if (!drawn) {
			continue;
		}
		Transform3D xform = model_xform;
		if (node_pose) {
			if (Node3D *const *part = robj_nodes_.getptr(twin.section)) {
				if (*part != nullptr) {
					xform = (*part)->get_global_transform();
				}
			}
		} else if (twin.section >= 0 && twin.section < view_twin_pose_nodes_.size()) {
			if (const Node3D *pose = Object::cast_to<Node3D>(
						static_cast<Object *>(view_twin_pose_nodes_[twin.section]))) {
				xform = model_xform * pose->get_transform();
			}
		}
		rs->instance_set_transform(twin.instance, xform);
	}
}

void ObjectModel::free_view_twins() {
	sync_inset_section_twins(false);
	if (view_twins_.empty() && view_twin_pose_nodes_.is_empty() && view_twin_skin_.is_null() &&
			view_twin_dynamic_.empty()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	for (const ViewTwin &twin : view_twins_) {
		if (rs != nullptr && twin.instance.is_valid()) {
			rs->free_rid(twin.instance);
		}
	}
	live_geometry_instance_count_ -= static_cast<int64_t>(view_twins_.size());
	view_twins_.clear();
	for (int64_t i = 0; i < view_twin_pose_nodes_.size(); ++i) {
		Node3D *pose = Object::cast_to<Node3D>(static_cast<Object *>(view_twin_pose_nodes_[i]));
		if (pose != nullptr) {
			memdelete(pose);
		}
	}
	view_twin_pose_nodes_ = Array();
	view_twin_pose_revision_ = 0;
	view_twin_skin_.unref();
	view_twin_dynamic_.clear();
	view_twin_lod_ = -1;
}

// One model's twins follow its final pose: the model transform through the
// part pose of the twin's level (the node's own part nodes when that level is
// the node's), the instance uniforms of the node instance drawing the same
// part, and the blended strips' water-side rung (renderer/render_order), which
// the node's refresh_render_order sets only for the level it shows.
void ObjectModel::sync_view_twin() {
	if (view_twins_.empty() || !is_inside_tree() || view_twin_lod_ < 0 ||
			static_cast<std::size_t>(view_twin_lod_) >= level_surfaces_.size()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	const Transform3D model_xform = get_global_transform();
	const bool node_pose = view_twin_lod_ == active_lod_;
	if (!node_pose && !rigid_parts_ && !view_twin_pose_nodes_.is_empty() &&
			object_data_.is_valid()) {
		view_twin_pose_revision_ = object_data_->apply_panm_to_nodes_table(view_twin_lod_,
				anim_time_ms_, runtime_ctrl_values(), view_twin_pose_nodes_,
				view_twin_pose_revision_);
	}
	if (!view_twin_dynamic_.empty() && object_data_.is_valid()) {
		const opennova::renderer::ControlRegisterValues ctrl = runtime_ctrl_values();
		for (TwinDynamicMaterial &row : view_twin_dynamic_) {
			apply_dynamic_material(row.material, row.material_index, row.needs_eval, row.stamp,
					ctrl);
		}
	}
	const StringName *names = twin_uniform_names();
	ObjectShaderCache *shader_cache = ObjectShaderCache::get_singleton();
	const std::vector<LevelSurface> &level =
			level_surfaces_[static_cast<std::size_t>(view_twin_lod_)];
	for (const ViewTwin &twin : view_twins_) {
		if (twin.surface < 0 || static_cast<std::size_t>(twin.surface) >= level.size()) {
			continue;
		}
		const LevelSurface &surface = level[static_cast<std::size_t>(twin.surface)];
		Node3D *parent = nullptr;
		Transform3D xform = model_xform;
		if (surface.is_skinned && skeleton_ != nullptr) {
			parent = skeleton_;
			xform = skeleton_->get_global_transform();
		} else {
			Node3D *const *part = robj_nodes_.getptr(surface.robj_index);
			parent = part != nullptr ? *part : nullptr;
			if (node_pose || rigid_parts_) {
				if (parent != nullptr) {
					xform = parent->get_global_transform();
				}
			} else if (surface.robj_index >= 0 &&
					surface.robj_index < view_twin_pose_nodes_.size()) {
				const Node3D *pose = Object::cast_to<Node3D>(
						static_cast<Object *>(view_twin_pose_nodes_[surface.robj_index]));
				if (pose != nullptr) {
					xform = model_xform * pose->get_transform();
				}
			}
		}
		rs->instance_set_transform(twin.instance, xform);
		// The node instance standing for the same part (else any slot).
		GeometryInstance3D *donor = nullptr;
		if (parent != nullptr) {
			for (int i = 0; i < parent->get_child_count() && donor == nullptr; ++i) {
				Node *child = parent->get_child(i);
				const bool auxiliary = Object::cast_to<PostMultiplyDraw>(child) != nullptr;
				if (auxiliary == twin.auxiliary) {
					donor = Object::cast_to<GeometryInstance3D>(child);
				}
			}
		}
		if (donor == nullptr && !surface_slots_.empty()) {
			donor = twin.auxiliary && surface_slots_[0].auxiliary != nullptr
					? static_cast<GeometryInstance3D *>(surface_slots_[0].auxiliary)
					: static_cast<GeometryInstance3D *>(surface_slots_[0].instance);
		}
		if (donor != nullptr) {
			for (int i = 0; i < kTwinUniformCount; ++i) {
				// A twin the Inset pass lit keeps its own point lights (1..9).
				if (twin.own_lights && i >= 1 && i <= 9) {
					continue;
				}
				const Variant value = donor->get_instance_shader_parameter(names[i]);
				if (value.get_type() != Variant::NIL) {
					rs->instance_geometry_set_shader_parameter(twin.instance, names[i], value);
				}
			}
		}
		if (!node_pose && !twin.auxiliary && surface.is_alpha && surface.material.is_valid() &&
				shader_cache != nullptr) {
			const float height = surface.is_skinned
					? static_cast<float>(get_global_position().y)
					: static_cast<float>(xform.xform(surface.local_center).y);
			const int32_t rung = render_rung_override_ != kRenderRungFromWaterSide
					? render_rung_override_
					: shader_cache->alpha_rung_for_height(height);
			if (surface.material->get_render_priority() != rung) {
				surface.material->set_render_priority(rung);
			}
		}
	}
}

void ObjectModel::sync_view_twins() {
	for (ObjectModel *model : inset_view_models_) {
		if (!model->view_twins_.empty()) {
			model->sync_view_twin();
			model->sync_inset_section_twins(true);
		}
	}
}

} // namespace godot
