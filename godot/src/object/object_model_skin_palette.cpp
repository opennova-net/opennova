// ObjectModel: the skinned effects' bone palette. A strip whose material runs
// one of retail's skinned vertex programs is posed by the object shaders
// (godot/shaders/object/skin.gdshaderinc) from this model's palette instead of
// Godot's skinning: the lit programs light the vertex's FIRST bone frame while
// they blend its position over four, which Godot's skinning (one blended
// matrix for position, normal and tangent alike) cannot express
// (renderer::ObjectSkinNormal carries the witness). The palette is the
// skeleton's settled pose: one texture row per bone, republished whenever the
// Skeleton3D settles a pose, and the matrices the render-slot capture skins
// its silhouettes with.

#include "object/object_model.h"

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>

#include <algorithm>
#include <cstring>

namespace godot {

namespace {

// Three RGBA32F texels per bone: the rows of the bind-to-skeleton 3x4 matrix.
constexpr int kPaletteTexelsPerBone = 3;
constexpr int kPaletteFloatsPerBone = kPaletteTexelsPerBone * 4;
constexpr int kPaletteBytesPerBone = kPaletteFloatsPerBone * static_cast<int>(sizeof(float));

void write_palette_row(uint8_t *p_row, const Transform3D &p_matrix) {
	float values[kPaletteFloatsPerBone];
	for (int row = 0; row < 3; ++row) {
		values[row * 4 + 0] = static_cast<float>(p_matrix.basis.rows[row].x);
		values[row * 4 + 1] = static_cast<float>(p_matrix.basis.rows[row].y);
		values[row * 4 + 2] = static_cast<float>(p_matrix.basis.rows[row].z);
		values[row * 4 + 3] = static_cast<float>(p_matrix.origin[row]);
	}
	std::memcpy(p_row, values, sizeof(values));
}

} // namespace

bool ObjectModel::material_runs_skin_program(int p_material_index) const {
	const bool *runs = material_skin_programs_.getptr(int64_t(p_material_index));
	return runs != nullptr && *runs;
}

// Retail's palette entry k is the matrix of the strip's bone-table entry k
// (the witness sits on renderer::prepare_model_mesh, which already gives each
// index byte its part), so one row per skeleton bone serves every strip. The
// skin binds each bone's rest inverse
// (Skeleton3D::create_skin_from_rest_transforms, bind i = bone i).
void ObjectModel::compute_skin_palette(std::vector<Transform3D> &r_palette) const {
	r_palette.clear();
	if (skeleton_ == nullptr || skin_bind_poses_.empty()) {
		return;
	}
	const int bone_count = static_cast<int>(skin_bind_poses_.size());
	if (skeleton_->get_bone_count() < bone_count) {
		return;
	}
	r_palette.resize(static_cast<std::size_t>(bone_count));
	for (int bone = 0; bone < bone_count; ++bone) {
		r_palette[static_cast<std::size_t>(bone)] = skeleton_->get_bone_global_pose(bone) *
				skin_bind_poses_[static_cast<std::size_t>(bone)];
	}
}

void ObjectModel::clear_skin_palette() {
	skin_bind_poses_.clear();
	skin_bone_bounds_.clear();
	skin_bone_has_bounds_.clear();
	skin_posed_bounds_ = AABB();
	skin_palette_.clear();
	skin_palette_image_.unref();
	skin_palette_texture_.unref();
}

// After the level surfaces are classified: the per-bone bind boxes of every
// palette strip, the palette texture, and its binding on their materials.
void ObjectModel::build_skin_palette() {
	clear_skin_palette();
	if (skeleton_ == nullptr || skeleton_skin_.is_null()) {
		return;
	}
	const int bone_count = skeleton_->get_bone_count();
	if (bone_count <= 0) {
		return;
	}
	bool any = false;
	skin_bone_bounds_.assign(static_cast<std::size_t>(bone_count), AABB());
	skin_bone_has_bounds_.assign(static_cast<std::size_t>(bone_count), false);
	for (const std::vector<LevelSurface> &level : level_surfaces_) {
		for (const LevelSurface &surface : level) {
			if (!surface.skin_palette) {
				continue;
			}
			any = true;
			const Array bones = surface.bone_bounds.keys();
			for (int64_t i = 0; i < bones.size(); ++i) {
				const int bone = bones[i];
				if (bone < 0) {
					continue;
				}
				const AABB box = surface.bone_bounds[bones[i]];
				// A key past the skeleton draws through its last bone, where
				// the palette fetch clamps its row (skin.gdshaderinc
				// obj_skin_rows).
				const std::size_t row = static_cast<std::size_t>(std::min(bone, bone_count - 1));
				skin_bone_bounds_[row] = skin_bone_has_bounds_[row]
						? skin_bone_bounds_[row].merge(box)
						: box;
				skin_bone_has_bounds_[row] = true;
			}
		}
	}
	if (!any) {
		skin_bone_bounds_.clear();
		skin_bone_has_bounds_.clear();
		return;
	}
	skin_bind_poses_.reserve(static_cast<std::size_t>(bone_count));
	for (int bone = 0; bone < bone_count; ++bone) {
		skin_bind_poses_.push_back(bone < skeleton_skin_->get_bind_count()
						? skeleton_skin_->get_bind_pose(bone)
						: Transform3D());
	}
	skin_palette_image_ = Image::create_empty(kPaletteTexelsPerBone, bone_count, false,
			Image::FORMAT_RGBAF);
	skin_palette_texture_ = ImageTexture::create_from_image(skin_palette_image_);
	for (const std::vector<LevelSurface> &level : level_surfaces_) {
		for (const LevelSurface &surface : level) {
			if (!surface.skin_palette || surface.material.is_null()) {
				continue;
			}
			surface.material->set_shader_parameter("u_skin_palette", skin_palette_texture_);
			surface.material->set_shader_parameter("u_skin_palette_bound", true);
		}
	}
	publish_skin_palette();
}

// The Skeleton3D settled a pose (its deferred update, the moment Godot uploads
// the skeleton's own skins, which every pose writer ends in): republish the
// palette, and the palette strips' culling box as the union of their per-bone
// bind boxes carried through the posed palette (the bounds Godot derives for
// the strips it skins itself).
void ObjectModel::publish_skin_palette() {
	if (skin_palette_texture_.is_null()) {
		return;
	}
	compute_skin_palette(skin_palette_);
	if (skin_palette_.empty()) {
		return;
	}
	// The image's own buffer, written in place: it copies only while an
	// upload still shares it.
	uint8_t *bytes = skin_palette_image_->ptrw();
	AABB bounds;
	bool has_bounds = false;
	for (std::size_t bone = 0; bone < skin_palette_.size(); ++bone) {
		write_palette_row(bytes + bone * static_cast<std::size_t>(kPaletteBytesPerBone),
				skin_palette_[bone]);
		if (skin_bone_has_bounds_[bone]) {
			const AABB box = skin_palette_[bone].xform(skin_bone_bounds_[bone]);
			bounds = has_bounds ? bounds.merge(box) : box;
			has_bounds = true;
		}
	}
	skin_palette_texture_->update(skin_palette_image_);
	skin_posed_bounds_ = has_bounds ? bounds : AABB();
	apply_skin_palette_bounds();
}

// A palette strip carries the posed box as its custom AABB (the instance hangs
// under the skeleton with an identity transform, so the box is in skeleton
// space); any other slot keeps Godot's own.
void ObjectModel::apply_skin_palette_bounds() {
	const std::vector<LevelSurface> *level =
			active_lod_ >= 0 &&
					static_cast<std::size_t>(active_lod_) < level_surfaces_.size()
			? &level_surfaces_[static_cast<std::size_t>(active_lod_)]
			: nullptr;
	for (std::size_t slot = 0; slot < surface_slots_.size(); ++slot) {
		MeshInstance3D *instance = surface_slots_[slot].instance;
		if (instance == nullptr) {
			continue;
		}
		const bool palette = level != nullptr && slot < level->size() &&
				(*level)[slot].skin_palette;
		const AABB box = palette ? skin_posed_bounds_ : AABB();
		if (instance->get_custom_aabb() != box) {
			instance->set_custom_aabb(box);
		}
	}
	if (view_twins_.empty() || view_twin_lod_ < 0 ||
			static_cast<std::size_t>(view_twin_lod_) >= level_surfaces_.size()) {
		return;
	}
	RenderingServer *rs = RenderingServer::get_singleton();
	if (rs == nullptr) {
		return;
	}
	const std::vector<LevelSurface> &twin_level =
			level_surfaces_[static_cast<std::size_t>(view_twin_lod_)];
	for (const ViewTwin &twin : view_twins_) {
		if (twin.surface >= 0 && static_cast<std::size_t>(twin.surface) < twin_level.size() &&
				twin_level[static_cast<std::size_t>(twin.surface)].skin_palette) {
			rs->instance_set_custom_aabb(twin.instance, skin_posed_bounds_);
		}
	}
}

Array ObjectModel::get_skin_palette() const {
	Array out;
	if (skin_palette_texture_.is_null()) {
		return out;
	}
	std::vector<Transform3D> palette;
	compute_skin_palette(palette);
	for (const Transform3D &matrix : palette) {
		out.push_back(matrix);
	}
	return out;
}

bool ObjectModel::skin_palette_of(const MeshInstance3D *p_instance,
		std::vector<Transform3D> &r_palette) {
	r_palette.clear();
	if (p_instance == nullptr) {
		return false;
	}
	const Skeleton3D *skeleton = Object::cast_to<Skeleton3D>(p_instance->get_parent());
	if (skeleton == nullptr) {
		return false;
	}
	const ObjectModel *model = Object::cast_to<ObjectModel>(skeleton->get_parent());
	if (model == nullptr || model->skeleton_ != skeleton ||
			model->skin_palette_texture_.is_null()) {
		return false;
	}
	const std::vector<LevelSurface> *level =
			model->active_lod_ >= 0 &&
					static_cast<std::size_t>(model->active_lod_) < model->level_surfaces_.size()
			? &model->level_surfaces_[static_cast<std::size_t>(model->active_lod_)]
			: nullptr;
	if (level == nullptr) {
		return false;
	}
	for (std::size_t slot = 0; slot < model->surface_slots_.size() && slot < level->size();
			++slot) {
		if (model->surface_slots_[slot].instance == p_instance) {
			if (!(*level)[slot].skin_palette) {
				return false;
			}
			model->compute_skin_palette(r_palette);
			return !r_palette.empty();
		}
	}
	return false;
}

} // namespace godot
