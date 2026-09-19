// The engine-side collision section-matrix provider — the binding's
// build_section_matrices legs moved verbatim onto the sim's own parsed
// models/clips (ADR 0028). The Godot binding's Transform3D round trip was the
// identity on every matrix entry the collision applier reads, so the PANM leg
// hands the evaluator's matrices straight through; the skeletal leg composes
// the same FK/deformation in native rows.
#include <runtime/world/entity_pose.h>

#include <runtime/world/pose_inputs.h>

#include <runtime/anim/aim_overlay.h>
#include <base/io/fixed.h>
#include <base/io/strutil.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm_pose.h>
#include <runtime/world/ai.h>
#include <runtime/world/angle.h>
#include <runtime/world/infantry.h>
#include <runtime/world/mount_controls.h>
#include <runtime/world/world.h>

#include <algorithm>
#include <cmath>
#include <limits>
#include <cstring>
#include <string>
#include <utility>

using namespace opennova::threedi;

namespace opennova::world {

namespace {

// Retail's final special row clips BN17 R Hand (model bone 16) after channel
// composition, overlay, and parent-pivot re-anchor.
// [orig: Entity_BuildBoneTransformMatrices @0x4b1290 special row;
//  world-wac-ai-re.md §14.1.5]
constexpr int kRightHandBoneIndex = 16;

constexpr double kBamToRadians = 6.28318530717958647692 / 4294967296.0;
constexpr double kHalfPi = 1.57079632679489661923;

anim::Quat quat_axis_x(double angle) {
	const double h = angle * 0.5;
	return anim::Quat{static_cast<float>(std::cos(h)),
			static_cast<float>(std::sin(h)), 0.0f, 0.0f};
}

anim::Quat quat_axis_y(double angle) {
	const double h = angle * 0.5;
	return anim::Quat{static_cast<float>(std::cos(h)), 0.0f,
			static_cast<float>(std::sin(h)), 0.0f};
}

anim::Quat quat_axis_z(double angle) {
	const double h = angle * 0.5;
	return anim::Quat{static_cast<float>(std::cos(h)), 0.0f, 0.0f,
			static_cast<float>(std::sin(h))};
}

// The model-frame orientation of one overlay class — the native twin of the
// binding's bms_to_godot_basis (mission_object_placer): Ry(yaw) * Rz(pitch) * Rx(roll)
// with the trailing +90° model-forward correction. The correction does NOT
// cancel in the body-relative delta below — it conjugates the delta into the
// node frame apply_aim_overlay expects.
anim::Quat overlay_model_quat(const anim::AimOverlayAngles &angles) {
	const anim::Quat yaw =
			quat_axis_y(static_cast<double>(angles.yaw) * kBamToRadians);
	const anim::Quat pitch =
			quat_axis_z(static_cast<double>(angles.pitch) * kBamToRadians);
	const anim::Quat roll =
			quat_axis_x(static_cast<double>(angles.roll) * kBamToRadians);
	const anim::Quat fwd = quat_axis_y(kHalfPi);
	return anim::quat_mul(anim::quat_mul(anim::quat_mul(yaw, pitch), roll), fwd);
}

// Row-major render float[16] from a composed deformation — the exact formula
// the binding's panm_render_matrix_from_godot applied to the same rows
// (the (-x, y, z) handedness flip + the row-vector transpose).
void render_matrix_from_deformation(
		const anim::SkeletalClips::RestTransform &deformation, float out[16]) {
	std::memset(out, 0, sizeof(float) * 16);
	const float *r = deformation.rows;
	out[0] = r[0];
	out[4] = -r[1];
	out[8] = -r[2];
	out[1] = -r[3];
	out[5] = r[4];
	out[9] = r[5];
	out[2] = -r[6];
	out[6] = r[7];
	out[10] = r[8];
	out[12] = -deformation.origin.x;
	out[13] = deformation.origin.y;
	out[14] = deformation.origin.z;
	out[15] = 1.0f;
}

} // namespace

// The model's bone table for the rig: parent-relative pivots + parent indices
// from the canonical first RLOD's render objects (raw, engine frame — the
// same rows the binding's get_bone_origins/get_bone_parents exposed).
bool model_bone_table(const Threedi3di3 &model,
		std::vector<anim::Vec3> &r_origins, std::vector<int> &r_parents) {
	r_origins.clear();
	r_parents.clear();
	if (model.lod_count == 0 || model.lods == nullptr) return false;
	const ThreediLod &lod = model.lods[0];
	if (lod.render_object_count == 0 || lod.render_objects == nullptr)
		return false;
	r_origins.reserve(lod.render_object_count);
	r_parents.reserve(lod.render_object_count);
	for (size_t i = 0; i < lod.render_object_count; ++i) {
		const ThreediRenderObject &part = lod.render_objects[i];
		r_origins.push_back(anim::Vec3{part.rel[0], part.rel[1], part.rel[2]});
		r_parents.push_back(part.parent_index);
	}
	return true;
}

void EntityPoseProvider::clear() {
	generic_models_.clear();
	userpoint_models_.clear();
	skeletal_sources_.clear();
	muzzle_queries_ = 0;
	muzzle_resolves_ = 0;
}

void EntityPoseProvider::register_generic_model(int32_t model_id,
		const assets::Model &model) {
	if (model == nullptr) return;
	generic_models_[model_id] = model;
}

bool EntityPoseProvider::register_skeletal_entity(
		world::EntityHandle entity, uint64_t registry_spawn_id,
		int32_t model_id,
		const std::string &adm_name, const assets::Model &model,
		const std::string &muzzle_userpoint) {
	skeletal_sources_.erase(entity.packed);
	if (assets_ == nullptr || !model || adm_name.empty()) return false;
	std::vector<anim::Vec3> origins;
	std::vector<int> parents;
	if (!model_bone_table(*model, origins, parents)) return false;
	auto rig = assets_->skeletal_rig(adm_name, origins, parents);
	if (rig == nullptr || !rig->loaded() || !rig->fk_valid()) return false;
	SkeletalSource source;
	source.model_id = model_id;
	source.registry_spawn_id = registry_spawn_id;
	source.rig = std::move(rig);
	source.model = model;
	// The gun-flash userpoint resolved on its posed bone [orig: Entity_GetAttachmentWorldPosition
	//  @0x4b2670; docs/world/world-wac-ai-re.md §21 / D-AI-6].
	if (!muzzle_userpoint.empty() && model->user_points != nullptr) {
		for (size_t i = 0; i < model->user_point_count; ++i) {
			const ThreediUserPoint &point = model->user_points[i];
			if (!strutil::iequals(point.name, muzzle_userpoint) ||
					point.subobject_index < 0 ||
					static_cast<size_t>(point.subobject_index) >=
							source.rig->bone_count()) {
				continue;
			}
			// The RAW authored record position: the fixed matrix chain works in
			// the native model frame (the render-frame swizzle lives inside the
			// pose sandwich), exactly as retail transforms record[+0..+8].
			// [orig: @0x4b272e..0x4b2743]
			source.muzzle_bone = point.subobject_index;
			source.muzzle_model_position[0] = point.x;
			source.muzzle_model_position[1] = point.y;
			source.muzzle_model_position[2] = point.z;
			break;
		}
	}
	skeletal_sources_[entity.packed] = std::move(source);
	return true;
}

void EntityPoseProvider::remove_entity(world::EntityHandle entity) {
	skeletal_sources_.erase(entity.packed);
}

void EntityPoseProvider::register_userpoint_model(int32_t model_id,
		const assets::Model &model) {
	if (model == nullptr || model_id < 0) return;
	userpoint_models_[model_id] = model;
}

// The vehicle/eweap userpoint through the current part pose: the entity
// placement matrix (with its Q16 scale when set), the userpoint's part matrix
// from the sim-clock PANM evaluation, then the record position and the posed
// bone's euler. A model without live PANM poses every part in the entity
// frame (Model_TransformBoneMatrices leaves unanimated parts at their parent).
// [orig: Entity_ComputeUserpointWorldTransform @0x545c60 (placement
//  @0x545d91..0x545dec) -> Userpoint_ComputeWorldTransform @0x56c420 (part
//  pose @0x56c4dd, point @0x56c4f2..0x56c513, euler @0x56c604)]
bool EntityPoseProvider::resolve_userpoint_transform(world::World &world,
		world::EntityHandle entity, int userpoint_index, int32_t out[6]) {
	if (out == nullptr || userpoint_index <= 0 || world.collision == nullptr)
		return false;
	const world::Entity *e = world.registry.get(entity);
	if (e == nullptr) return false;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr) return false;
	const Threedi3di3 &model3di = *found->second;
	if (model3di.user_points == nullptr ||
			static_cast<size_t>(userpoint_index) > model3di.user_point_count)
		return false;
	const ThreediUserPoint &point = model3di.user_points[userpoint_index - 1];

	const world::CollisionMatrix entity_world = world::entity_placement_matrix(*e);
	world::CollisionMatrix bone_world = entity_world;
	std::vector<ThreediMatrix4x4> part_matrices;
	if (point.subobject_index >= 0 &&
			panm_part_matrices(world, model3di, entity, part_matrices) &&
			static_cast<size_t>(point.subobject_index) < part_matrices.size()) {
		if (!world::collision_matrix_apply_render_pose(entity_world,
					part_matrices[static_cast<size_t>(point.subobject_index)].m,
					bone_world))
			return false;
	}
	// The raw authored record position in the native model frame
	// [orig: @0x56c4f2..0x56c513 transforms record[+0..+8] as stored].
	const int32_t local_q16[3] = {point.x, point.y, point.z};
	bone_world.transform_point(local_q16, out);
	world::collision_matrix_to_euler(bone_world, out + 3);
	return true;
}

// [orig: Entity_GetBoneTransformAndOrientation @0x4B0C50]
bool EntityPoseProvider::resolve_named_transform(
		world::World &world, world::EntityHandle entity, const char *name, int32_t out[6]) {
	if (name == nullptr || world.collision == nullptr)
		return false;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr)
		return false;
	const Threedi3di3 &model = *found->second;
	for (size_t i = 0; model.user_points && i < model.user_point_count; ++i)
		if (strutil::iequals(model.user_points[i].name, name))
			return resolve_userpoint_transform(world, entity, static_cast<int>(i + 1), out);
	return false;
}

// [orig: Entity_FindAttachBone @0x4B9580, last case-insensitive match]
int EntityPoseProvider::last_named_userpoint(
		world::World &world, world::EntityHandle entity, const char *name) {
	if (name == nullptr || world.collision == nullptr) return 0;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr) return 0;
	const Threedi3di3 &model = *found->second;
	int result = 0;
	for (size_t i = 0; model.user_points && i < model.user_point_count; ++i)
		if (strutil::iequals(model.user_points[i].name, name)) result = static_cast<int>(i + 1);
	return result;
}

bool EntityPoseProvider::resolve_userpoint_rigid(world::World &world,
		world::EntityHandle entity, int userpoint_index, int32_t out[3]) {
	if (out == nullptr || userpoint_index <= 0 || world.collision == nullptr)
		return false;
	const world::Entity *e = world.registry.get(entity);
	if (e == nullptr) return false;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr) return false;
	const Threedi3di3 &model3di = *found->second;
	if (model3di.user_points == nullptr ||
			static_cast<size_t>(userpoint_index) > model3di.user_point_count)
		return false;
	const ThreediUserPoint &point = model3di.user_points[userpoint_index - 1];
	// The raw record position through the placement matrix alone
	// [orig: Entity_ComputeWeaponFireOrigin @0x43b5f6].
	const int32_t local_q16[3] = {point.x, point.y, point.z};
	world::entity_placement_matrix(*e).transform_point(local_q16, out);
	return true;
}

// [orig: Entity_ComputeWeaponFireTransform_0 @0x456980, COBJ pivot branch]
bool EntityPoseProvider::resolve_userpoint_pivot(
		world::World &world, world::EntityHandle entity, int userpoint_index, int32_t out[3]) {
	if (out == nullptr || userpoint_index <= 0 || world.collision == nullptr)
		return false;
	const world::Entity *e = world.registry.get(entity);
	if (e == nullptr)
		return false;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr)
		return false;
	const Threedi3di3 &model = *found->second;
	if (model.user_points == nullptr || size_t(userpoint_index) > model.user_point_count ||
			model.collision == nullptr || model.collision->objects == nullptr)
		return false;
	const int part = model.user_points[userpoint_index - 1].subobject_index;
	if (part < 0 || size_t(part) >= model.collision->object_count)
		return false;
	world::entity_placement_matrix(*e).transform_point(model.collision->objects[part].offset, out);
	return true;
}

// [orig: Entity_ProcessSectionDamageTransition @0x43F496..0x43F501]
bool EntityPoseProvider::resolve_section_pivot(world::World &world,
		world::EntityHandle entity, int part, int32_t out[3]) {
	if (out == nullptr || part < 0 || world.collision == nullptr) return false;
	const world::Entity *e = world.registry.get(entity);
	if (e == nullptr) return false;
	const auto found = userpoint_models_.find(world.collision->entity_model_id(entity));
	if (found == userpoint_models_.end() || found->second == nullptr) return false;
	const Threedi3di3 &model = *found->second;
	if (model.collision == nullptr || model.collision->objects == nullptr ||
			size_t(part) >= model.collision->object_count) return false;
	world::entity_placement_matrix(*e).transform_point(model.collision->objects[part].offset, out);
	return true;
}

bool EntityPoseProvider::has_skeletal_entity(
		world::EntityHandle entity) const {
	return skeletal_sources_.find(entity.packed) != skeletal_sources_.end();
}

bool EntityPoseProvider::build_section_matrices(world::World &world,
		world::EntityHandle entity, int32_t model_id,
		const world::CollisionMatrix &entity_world,
		const world::CollisionModel &model,
		std::vector<world::CollisionMatrix> &out) {
	const auto skeletal_found = skeletal_sources_.find(entity.packed);
	const world::Entity *e = world.registry.get(entity);
	if (skeletal_found != skeletal_sources_.end() &&
			skeletal_found->second.model_id == model_id && e != nullptr &&
			skeletal_found->second.registry_spawn_id == e->registry_spawn_id) {
		return build_skeletal(world, skeletal_found->second, entity,
				entity_world, model, out);
	}
	const auto generic_found = generic_models_.find(model_id);
	if (generic_found == generic_models_.end() ||
			generic_found->second == nullptr)
		return false;
	return build_generic(world, *generic_found->second, entity, entity_world,
			model, out);
}

bool EntityPoseProvider::resolve_muzzle_pose(world::World &world,
		world::EntityHandle entity, int32_t out[3]) {
	++muzzle_queries_;
	if (out == nullptr) return false;
	const auto found = skeletal_sources_.find(entity.packed);
	const world::Entity *registered = world.registry.get(entity);
	if (found == skeletal_sources_.end() || registered == nullptr ||
			found->second.registry_spawn_id !=
					registered->registry_spawn_id) {
		return false;
	}
	const SkeletalSource &source = found->second;
	world::CollisionMatrix muzzle_world;
	if (!build_skeletal_bone_matrix(world, source, entity, source.muzzle_bone, muzzle_world))
		return false;
	muzzle_world.transform_point(source.muzzle_model_position, out);
	++muzzle_resolves_;
	return true;
}

bool EntityPoseProvider::resolve_organic_attachment(world::World &world,
		world::EntityHandle entity, uint8_t userpoint, int32_t out[3]) {
	++muzzle_queries_;
	const auto found = skeletal_sources_.find(entity.packed);
	const world::Entity *registered = world.registry.get(entity);
	if (out == nullptr || userpoint == 0 || found == skeletal_sources_.end() ||
			registered == nullptr || found->second.registry_spawn_id != registered->registry_spawn_id)
		return false;
	const auto &source = found->second;
	if (!source.model || !source.model->user_points ||
			size_t(userpoint) > source.model->user_point_count) return false;
	const auto &point = source.model->user_points[userpoint - 1];
	world::CollisionMatrix matrix;
	if (!build_skeletal_bone_matrix(world, source, entity, point.subobject_index, matrix))
		return false;
	const int32_t local[3] = {point.x, point.y, point.z};
	matrix.transform_point(local, out);
	++muzzle_resolves_;
	return true;
}

bool EntityPoseProvider::build_skeletal_bone_matrix(world::World &world,
		const SkeletalSource &source, world::EntityHandle entity, int bone_index,
		world::CollisionMatrix &out_matrix) const {
	const anim::SkeletalClips *rig = source.rig.get();
	if (bone_index < 0 || rig == nullptr || !rig->loaded() ||
			!rig->fk_valid() ||
			static_cast<size_t>(bone_index) >= rig->parents().size() ||
			static_cast<size_t>(bone_index) >=
					rig->rest_global_inverse().size()) {
		return false;
	}

	std::vector<anim::PoseBone> pose;
	anim::AimOverlayAngles angles[anim::kOverlayClassCount];
	anim::AimOverlayInputs inputs;
	const world::Entity *posed_entity = nullptr;
	world::AiEntity *ai_entity = nullptr;
	if (!eval_entity_pose(world, source, entity, pose, angles, inputs,
			posed_entity, ai_entity) ||
			static_cast<size_t>(bone_index) >= pose.size()) {
		return false;
	}

	// Accumulate only the muzzle bone's ancestor prefix. This is the same
	// parent-local FK and bind-rest division build_skeletal() uses for every
	// collision section, without constructing or publishing an entire matrix
	// array for a single attachment-point query.
	std::vector<anim::SkeletalClips::RestTransform> pose_global(
			static_cast<size_t>(bone_index) + 1);
	const bool collapse_right_hand =
			mount_collapses_right_hand_row(*posed_entity);
	for (int32_t i = 0; i <= bone_index; ++i) {
		anim::SkeletalClips::RestTransform local;
		anim::quat_to_mat3_rows(pose[static_cast<size_t>(i)].rotation,
				local.rows);
		local.origin = pose[static_cast<size_t>(i)].origin;
		// The same collapsed row build_skeletal() keeps for COBJ 16: the
		// zero-scale local (origin kept) is the row itself, not composed onto
		// its parent, so a muzzle below it lands where the collision pose does.
		// [orig: special row @0x4b1290]
		if (collapse_right_hand && i == kRightHandBoneIndex) {
			std::memset(local.rows, 0, sizeof(local.rows));
			pose_global[static_cast<size_t>(i)] = local;
			continue;
		}
		const int parent = rig->parents()[static_cast<size_t>(i)];
		pose_global[static_cast<size_t>(i)] = parent >= 0
				? anim::rest_mul(pose_global[static_cast<size_t>(parent)], local)
				: local;
	}
	const anim::SkeletalClips::RestTransform deformation = anim::rest_mul(
			pose_global[static_cast<size_t>(bone_index)],
			rig->rest_global_inverse()[static_cast<size_t>(bone_index)]);
	float render_pose[16];
	render_matrix_from_deformation(deformation, render_pose);
	const int32_t position[3] = {
			ai_entity != nullptr ? ai_entity->pos[0] : static_cast<int32_t>(posed_entity->position.x * io::kFp16One),
			ai_entity != nullptr ? ai_entity->pos[1] : static_cast<int32_t>(posed_entity->position.y * io::kFp16One),
			ai_entity != nullptr ? ai_entity->pos[2] : static_cast<int32_t>(posed_entity->position.z * io::kFp16One),
	};
	const world::CollisionMatrix body_world =
			world::collision_matrix_from_euler(
					angles[anim::kOverlayBody].yaw,
					angles[anim::kOverlayBody].pitch,
					angles[anim::kOverlayBody].roll, position);
	if (!world::collision_matrix_apply_render_pose(
			body_world, render_pose, out_matrix)) {
		return false;
	}
	return true;
}


bool EntityPoseProvider::resolve_skeletal_anchor(world::World &world,
		world::EntityHandle entity, world::SkeletalAnchor anchor, int32_t out[3]) {
	const auto found = skeletal_sources_.find(entity.packed);
	const world::Entity *registered = world.registry.get(entity);
	if (out == nullptr || found == skeletal_sources_.end() || registered == nullptr ||
			found->second.registry_spawn_id != registered->registry_spawn_id) return false;
	const SkeletalSource &source = found->second;
	const int bone = anchor == world::SkeletalAnchor::Head ? 14 : kHeldWeaponBoneIndex;
	world::CollisionMatrix matrix;
	if (!build_skeletal_bone_matrix(world, source, entity, bone, matrix)) return false;
	const anim::Vec3 pivot = source.rig->rest_global()[static_cast<size_t>(bone)].origin;
	// Rest pivots are in the X-negated render frame. Convert the nudged point
	// back to native model axes before applying the same posed matrix as collision.
	// [orig: held pivot+nudge @0x4B2180; head pivot+(0,.15,.10) @0x4B1290 tail]
	const bool held = anchor == world::SkeletalAnchor::HeldWeapon;
	const float point[3] = {
		pivot.z + (held ? kHeldWeaponAttachNudgeZ : 0.10f),
		pivot.x + (held ? kHeldWeaponAttachNudgeX : 0.0f),
		pivot.y + (held ? kHeldWeaponAttachNudgeY : 0.15f)
	};
	// Keep the authored binary32 point's fractional Q16 bits until the final
	// world-coordinate store. These anchors are float-matrix translations.
	for (int axis = 0; axis < 3; ++axis) {
		const int32_t *row = matrix.m + 4 * axis;
		const double value = double(row[3]) +
				(double(row[0]) * point[0] + double(row[1]) * point[1] +
				 double(row[2]) * point[2]) / 64.0;
		out[axis] = static_cast<int32_t>(std::clamp(value,
				static_cast<double>(std::numeric_limits<int32_t>::min()),
				static_cast<double>(std::numeric_limits<int32_t>::max())));
	}
	return true;
}

bool EntityPoseProvider::eval_entity_pose(world::World &world,
		const SkeletalSource &source, world::EntityHandle entity,
		std::vector<anim::PoseBone> &r_pose, anim::AimOverlayAngles *r_angles,
		anim::AimOverlayInputs &r_inputs, const world::Entity *&r_entity,
		world::AiEntity *&r_ai) const {
	const anim::SkeletalClips *rig = source.rig.get();
	r_ai = world.ai.for_handle(entity);
	r_entity = world.registry.get(entity);
	if (rig == nullptr || !rig->loaded() || !rig->fk_valid() ||
			r_ai == nullptr || r_entity == nullptr)
		return false;

	const std::string reset_key("anim_reset");
	const auto resolve_primary_key = [&](const std::string &key) {
		if (rig->has_clip(key)) return key;
		return rig->has_clip(reset_key) ? reset_key : key;
	};
	const std::string primary_key =
			resolve_primary_key(opennova::world::infantry_anim_key(r_ai->inf.body_clip_state()));
	if (primary_key.empty()) return false;
	const double primary_seconds =
			rig->clip_seconds_at_tick(primary_key, r_ai->inf.clip_phase);
	std::string source_key;
	double source_seconds = 0.0;
	const bool primary_blend = r_ai->inf.body_blend_active();
	if (primary_blend) {
		source_key = resolve_primary_key(
				opennova::world::infantry_anim_key(r_ai->inf.anim_prev));
		source_seconds = rig->clip_seconds_at_tick(source_key, r_ai->inf.anim_prev_clip_phase);
	}

	r_inputs = aim_overlay_inputs_for(*r_ai, *r_entity);
	anim::compute_aim_overlay_angles(r_inputs, r_angles);
	anim::Quat deltas[anim::kOverlayClassCount];
	const anim::Quat body_inv =
			anim::quat_inv(overlay_model_quat(r_angles[anim::kOverlayBody]));
	for (int c = 0; c < static_cast<int>(anim::kOverlayClassCount); ++c) {
		deltas[c] = anim::quat_mul(body_inv, overlay_model_quat(r_angles[c]));
	}

	std::string weapon_key;
	double weapon_seconds = 0.0;
	std::string weapon_prev_key;
	double weapon_prev_seconds = 0.0;
	float weapon_blend = 1.0f;
	int weapon_variant = 0;
	int weapon_prev_variant = 0;
	if (world.cached.local_player.valid() &&
			entity.packed == world.cached.local_player.packed &&
			world::infantry_weapon_channel_visible(
					r_ai->inf, weapon_active,
					mount_blocks_weapon_channel(*r_entity))) {
		weapon_key = opennova::world::infantry_anim_key(r_ai->inf.weapon_clip_state());
		weapon_variant = r_ai->inf.wpn_variant;
		weapon_seconds = rig->clip_seconds_at_tick(
				weapon_key, r_ai->inf.wpn_clip_phase, weapon_variant);
		// The secondary channel's own cross-fade rides into the authoritative pose
		// exactly as it does into presentation, so hitboxes and the drawn body
		// agree through the window [orig: the shared AnimMap_UpdateEntity re-init].
		if (r_ai->inf.weapon_blend_active()) {
			weapon_prev_key = opennova::world::infantry_anim_key(r_ai->inf.wpn_prev);
			weapon_prev_variant = r_ai->inf.wpn_prev_variant;
			weapon_blend = r_ai->inf.wpn_blend_weight;
			weapon_prev_seconds = rig->clip_seconds_at_tick(
					weapon_prev_key, r_ai->inf.wpn_prev_clip_phase, weapon_prev_variant);
		}
	}

	rig->eval_composed_pose(primary_key, primary_seconds, primary_blend,
			source_key, source_seconds, r_ai->inf.anim_blend_weight,
			deltas, weapon_key, weapon_seconds, r_pose,
			weapon_prev_key, weapon_prev_seconds, weapon_blend,
			weapon_variant, weapon_prev_variant);
	return true;
}

bool EntityPoseProvider::build_skeletal(world::World &world,
		const SkeletalSource &source, world::EntityHandle entity,
		const world::CollisionMatrix &entity_world,
		const world::CollisionModel &model,
		std::vector<world::CollisionMatrix> &out) const {
	const anim::SkeletalClips *rig = source.rig.get();
	const size_t section_count = model.sections.size();
	if (rig == nullptr || !rig->loaded() || !rig->fk_valid() ||
			rig->parents().size() < section_count ||
			rig->rest_global().size() < section_count ||
			rig->overlay_classes().size() < section_count)
		return false;

	std::vector<anim::PoseBone> pose;
	anim::AimOverlayAngles angles[anim::kOverlayClassCount];
	anim::AimOverlayInputs inputs;
	const world::Entity *e = nullptr;
	world::AiEntity *ai_entity = nullptr;
	if (!eval_entity_pose(world, source, entity, pose, angles, inputs, e,
				ai_entity))
		return false;
	const bool collapse_right_hand = mount_collapses_right_hand_row(*e);
	if (pose.size() < section_count) return false;

	// The callback result is FINAL world-space. Build the body placement from
	// the overlay's body class (not the aim heading), then apply the skinned
	// deformation exactly once. At bind pose pose_global*rest_global^-1 is
	// identity, which guards against both double-rest and double-entity
	// translation. COBJ parent/offset/CXLT are deliberately not selectors:
	// COBJ[i] pairs strictly with this output slot i.
	const int32_t position[3] = {
			entity_world.m[3], entity_world.m[7], entity_world.m[11]};
	const world::CollisionMatrix body_world = world::collision_matrix_from_euler(
			angles[anim::kOverlayBody].yaw, angles[anim::kOverlayBody].pitch,
			angles[anim::kOverlayBody].roll, position);
	std::vector<anim::SkeletalClips::RestTransform> pose_global(section_count);
	out.resize(section_count);
	for (size_t i = 0; i < section_count; ++i) {
		anim::SkeletalClips::RestTransform local;
		anim::quat_to_mat3_rows(pose[i].rotation, local.rows);
		local.origin = pose[i].origin;
		// Retail zeroes the FINAL collision row after overlay/re-anchor.
		// Preserve that literal collision result for COBJ 16: composing
		// body_world here would incorrectly reintroduce the entity
		// translation; the zero-scale local (origin kept) still FK-chains any
		// children exactly like the binding's collapsed pose.
		// [orig: special row @0x4b1290]
		if (collapse_right_hand && i == kRightHandBoneIndex) {
			std::memset(local.rows, 0, sizeof(local.rows));
			pose_global[i] = local;
			out[i] = world::CollisionMatrix{};
			continue;
		}
		const int parent = rig->parents()[i];
		pose_global[i] = parent >= 0
				? anim::rest_mul(pose_global[static_cast<size_t>(parent)], local)
				: local;
		const anim::SkeletalClips::RestTransform deformation =
				anim::rest_mul(pose_global[i], rig->rest_global_inverse()[i]);
		float render_pose[16];
		render_matrix_from_deformation(deformation, render_pose);
		if (!world::collision_matrix_apply_render_pose(
					body_world, render_pose, out[i]))
			return false;
	}
	return true;
}

bool EntityPoseProvider::panm_part_matrices(world::World &world,
		const Threedi3di3 &model3di, world::EntityHandle entity,
		std::vector<ThreediMatrix4x4> &r_part_matrices) const {
	// Retail Generic collision always transforms the canonical first RLOD. It
	// never follows the render-selected LOD or scans for another live PANM.
	constexpr int lod_index = 0;
	if (!threedi_panm_lod_has_live(model3di, lod_index)) return false;
	if (model3di.ctrl.count > 0 && model3di.ctrl.registers == nullptr)
		return false;

	// The retail global CTRL bus, written by ordinal — absent keys and zero
	// values are indistinguishable on the bus, exactly like the binding's
	// Dictionary translation.
	int32_t ctrl_values[THREEDI_CTRL_REGISTER_COUNT] = {};
	const world::Entity *e = world.registry.get(entity);
	if (e != nullptr)
		for (int phase = 0; phase < 6; ++phase)
			ctrl_values[THREEDI_CTRL_OBJECT_DESTROY + phase] = e->destroy_phases_q16[phase];
	world::AiEntity *ai_entity =
			world.ai.for_handle(entity);
	// PLAYPARTANIM publishes its two phase accumulators to the fixed retail
	// VEHICLE_SPECIAL1/2 registers. A brainless static still evaluates
	// free-running PANM with zero phase values. SPECIAL1 alone is suppressed
	// by ItemDefAttrib 0x1000 (FastRope); SPECIAL2 is unconditional.
	// [orig: Entity_ApplyCommand case 0x22 @ 0x43B192; integrator @ 0x456710;
	//  HUD_CacheEntityDisplayInfo @ 0x4A3E18..0x4A3E38]
	const auto phase_for = [ai_entity](int channel) -> int32_t {
		return ai_entity != nullptr
				? ai_entity->brain.f[world::AiBrain::kPartAnimPhase0 + channel]
				: 0;
	};
	if (e == nullptr || (e->item_attrib & 0x1000u) == 0) {
		ctrl_values[THREEDI_CTRL_VEHICLE_SPECIAL1] = phase_for(0);
	}
	ctrl_values[THREEDI_CTRL_VEHICLE_SPECIAL2] = phase_for(1);
	// The generic collision frame receives HEAT_GLOW only when this model is
	// the carrier in a live UseGun attachment relation; a scoped cold slot
	// still reads literal zero. [orig: attachment caller @ 0x546518;
	//  HUD_CacheWeaponSlotInfo cold/hot stores @ 0x440969/@ 0x440991]
	if (e != nullptr) {
		int32_t heat_glow = 0;
		if (world::world_model_heat_glow_for(world, *e, heat_glow))
			ctrl_values[THREEDI_CTRL_HEAT_GLOW] = heat_glow;
	}
	// EWEAP yaw/pitch are independent semantic CTRL writers (B50Cal consumes
	// the pair).
	if (e != nullptr) {
		world::EmplacedWeaponControls emplaced;
		if (world::emplaced_weapon_controls_for(world, *e, emplaced)) {
			ctrl_values[THREEDI_CTRL_EWEAP_GUNYAW] =
					static_cast<int32_t>(emplaced.gun_yaw);
			ctrl_values[THREEDI_CTRL_EWEAP_GUNPITCH] =
					static_cast<int32_t>(emplaced.gun_pitch);
		}
	}
	if (e != nullptr)
		world.doors.write_phases(*e, ctrl_values + THREEDI_CTRL_DOOR_00,
				world::DoorSystem::kMaxDoors);
	const uint32_t time_ms = panm_time_override_ms >= 0
			? static_cast<uint32_t>(panm_time_override_ms)
			: world.logic_tick * 16u;
	return threedi_panm_pose_parts(model3di, lod_index, time_ms, ctrl_values,
			r_part_matrices, nullptr);
}

bool EntityPoseProvider::build_generic(world::World &world,
		const Threedi3di3 &model3di, world::EntityHandle entity,
		const world::CollisionMatrix &entity_world,
		const world::CollisionModel &model,
		std::vector<world::CollisionMatrix> &out) const {
	constexpr int lod_index = 0;
	std::vector<ThreediPartAnimation> effective;
	threedi_panm_effective_for_lod(model3di, lod_index, effective);
	if (effective.empty()) return false;
	std::vector<ThreediMatrix4x4> part_matrices;
	if (!panm_part_matrices(world, model3di, entity, part_matrices)) return false;

	// Default every COBJ slot to the Simple callback. Override only PANM nodes
	// whose target part ordinal exists as a collision section. This
	// intentionally ignores COBJ parent metadata and CXLT/offset records:
	// CVRT is model-space. The evaluator's row-major matrices are exactly the
	// render matrices the collision applier consumes — the binding's
	// Transform3D round trip was the identity on every entry it reads.
	out.assign(model.sections.size(), entity_world);
	bool matched_section = false;
	for (const ThreediPartAnimation &node : effective) {
		const size_t section = node.subobject_index;
		if (section >= out.size() || section >= part_matrices.size()) continue;
		if (!world::collision_matrix_apply_render_pose(
					entity_world, part_matrices[section].m, out[section]))
			return false;
		matched_section = true;
	}
	return matched_section;
}

} // namespace opennova::world
