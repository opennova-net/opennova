// ObjectModel: the skinned effects' bone palette. A strip whose material runs
// one of retail's skinned vertex programs is posed by the object shaders
// (godot/shaders/object/skin.gdshaderinc) from this model's palette instead of
// Godot's skinning: the lit programs light the vertex's FIRST bone frame while
// they blend its position over four, which Godot's skinning (one blended
// matrix for position, normal and tangent alike) cannot express
// (renderer::ObjectSkinNormal carries the witness). The palette is the
// skeleton's settled pose: one texture row per bone, republished whenever the
// Skeleton3D settles a pose, and the matrices the render-slot capture skins
// its silhouettes with. The skin's binds also carry the clip-posed parts'
// PANM layer (apply_skeletal_panm below).

#include "object/object_model.h"

#include <formats/threedi/threedi_panm_pose.h>

#include <godot_cpp/classes/mesh_instance3d.hpp>
#include <godot_cpp/classes/rendering_server.hpp>
#include <godot_cpp/core/math.hpp>

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
			for (const opennova::renderer::BoneBindBox &bind : surface.bone_bounds) {
				if (bind.bone < 0) {
					continue;
				}
				const Vector3 min(bind.min[0], bind.min[1], bind.min[2]);
				const AABB box(min, Vector3(bind.max[0], bind.max[1], bind.max[2]) - min);
				// A bone past the skeleton draws through its last bone, where
				// the palette fetch clamps its row (skin.gdshaderinc
				// obj_skin_rows).
				const std::size_t row = static_cast<std::size_t>(
						std::min(static_cast<int>(bind.bone), bone_count - 1));
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

// A clip-posed model's part tracks, composed over the bone matrices its clip
// built exactly as retail's submit composes a static model's over its entity
// matrix (threedi_panm_pose_parts_over carries the witness: the first-person
// gun and its arms, rigid and per-vertex skinned alike). Godot skins bone i as
// its global pose times the skin's bind i, so the composed part matrix lands
// as that bind, G_i^-1 x out_i: a bind takes any affine matrix (a part scaled
// to nothing included) where a bone pose keeps only position, rotation and
// scale, and the bones keep the clip's pose for everything else that reads
// them. Bone i is part i, as the fake skinning binds a rigid part
// (renderer::prepare_model_mesh). The skin palette follows the same binds.
void ObjectModel::apply_skeletal_panm() {
	if (!skeletal_scene_ || skeleton_ == nullptr || skeleton_skin_.is_null() ||
			object_data_.is_null() || !object_data_->has_document()) {
		return;
	}
	const opennova::threedi::Threedi3di3 &model = object_data_->native_model();
	if (active_lod_ < 0 || static_cast<std::size_t>(active_lod_) >= model.lod_count ||
			model.lods == nullptr) {
		return;
	}
	const std::size_t part_count = model.lods[active_lod_].render_object_count;
	const int bone_count = std::min({skeleton_->get_bone_count(),
			static_cast<int>(skeleton_skin_->get_bind_count()),
			static_cast<int>(skin_rest_binds_.size())});
	if (part_count == 0 || bone_count <= 0) {
		return;
	}
	// Each part's posed frame: its bone's global pose times the rest bind (a
	// part past the rig rides the last bone, like its fake-skinned vertices).
	std::vector<Transform3D> globals(static_cast<std::size_t>(bone_count));
	for (int bone = 0; bone < bone_count; ++bone) {
		globals[static_cast<std::size_t>(bone)] = skeleton_->get_bone_global_pose(bone);
	}
	std::vector<opennova::threedi::ThreediMatrix4x4> inputs(part_count);
	for (std::size_t part = 0; part < part_count; ++part) {
		const std::size_t bone = std::min(part, static_cast<std::size_t>(bone_count - 1));
		inputs[part] = ObjectData::panm_matrix(globals[bone] * skin_rest_binds_[bone]);
	}
	std::vector<opennova::threedi::ThreediMatrix4x4> posed;
	std::vector<uint8_t> driven;
	const bool layered = opennova::threedi::threedi_panm_pose_parts_over(model, active_lod_,
			opennova::threedi::threedi_panm_runtime_time_ms(anim_time_ms_),
			runtime_ctrl_values().data(), inputs, posed, &driven);
	if (!layered && !skin_binds_layered_) {
		return;
	}
	bool changed = false;
	const std::size_t binds = std::min(part_count, static_cast<std::size_t>(bone_count));
	for (std::size_t bone = 0; bone < binds; ++bone) {
		Transform3D bind = skin_rest_binds_[bone];
		// A bone posed to nothing (the collapsed right hand) has no inverse;
		// its part keeps the clip's collapse.
		if (layered && driven[bone] != 0 &&
				!Math::is_zero_approx(globals[bone].basis.determinant())) {
			const Transform3D composed =
					globals[bone].affine_inverse() * ObjectData::panm_transform(posed[bone]);
			if (!composed.is_equal_approx(bind)) {
				bind = composed;
			}
		}
		const int index = static_cast<int>(bone);
		if (skeleton_skin_->get_bind_pose(index) != bind) {
			skeleton_skin_->set_bind_pose(index, bind);
			changed = true;
		}
		if (bone < skin_bind_poses_.size()) {
			skin_bind_poses_[bone] = bind;
		}
	}
	skin_binds_layered_ = layered;
	if (changed) {
		publish_skin_palette();
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
