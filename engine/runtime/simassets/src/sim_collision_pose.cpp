// The engine-side collision section-matrix provider — the adapter's
// build_section_matrices legs moved verbatim onto the sim's own parsed
// models/clips (ADR 0028). The Godot binding's Transform3D round trip was the
// identity on every matrix entry the collision applier reads, so the PANM leg
// hands the evaluator's matrices straight through; the skeletal leg composes
// the same FK/deformation in native rows.
#include "simassets/sim_collision_pose.h"

#include "simassets/pose_inputs.h"

#include <anim/aim_overlay.h>
#include <io/strutil.h>
#include <threedi/threedi_ctrl_catalog.h>
#include <threedi/threedi_panm_pose.h>
#include <world/ai.h>
#include <world/angle.h>
#include <world/infantry.h>
#include <world/mount_controls.h>
#include <world/world.h>

#include <cmath>
#include <cstring>
#include <string>
#include <utility>

namespace opennova::simassets {

namespace {

// Retail's final special row clips BN17 R Hand (model bone 16) after channel
// composition, overlay, and parent-pivot re-anchor.
// [orig: Entity_BuildBoneTransformMatrices @0x4b1290 special row;
//  world-wac-ai-re.md §14.1.5]
constexpr int kRightHandBoneIndex = 16;

constexpr double kBamToRadians = 6.28318530717958647692 / 4294967296.0;
constexpr double kHalfPi = 1.57079632679489661923;

// anim-state id -> the .adm clip key ("anim_" + the retail state name).
// [orig: g_animStateNameTable @0x8135F0]
std::string infantry_anim_key(int state) {
	if (state < 0 || state >= opennova::world::kInfantryAnimStateCount)
		return std::string();
	const char *name = opennova::world::kInfantryAnimNames[state];
	if (name == nullptr || name[0] == '\0') return std::string();
	return std::string("anim_") + name;
}

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
// adapter's godot_model_basis_from_overlay: Ry(yaw) * Rz(pitch) * Rx(roll)
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
// the adapter's panm_render_matrix_from_godot applied to the same rows
// (the (-x, y, z) handedness flip + the row-vector transpose).
void render_matrix_from_deformation(
		const AdmSkeletalClips::RestTransform &deformation, float out[16]) {
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

// The model's bone table for the rig: parent-relative pivots + parent indices
// from the canonical first RLOD's render objects (raw, engine frame — the
// same rows the adapter's get_bone_origins/get_bone_parents exposed).
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

} // namespace

void SimCollisionPoseProvider::clear() {
	generic_models_.clear();
	skeletal_sources_.clear();
	rig_cache_.clear();
}

void SimCollisionPoseProvider::register_generic_model(int32_t model_id,
		const Threedi3di3 *model) {
	if (model == nullptr) return;
	generic_models_[model_id] = model;
}

bool SimCollisionPoseProvider::register_skeletal_entity(
		world::EntityHandle entity, uint64_t registry_spawn_id,
		int32_t model_id, const std::string &rig_key,
		const std::string &adm_name, const Threedi3di3 *model) {
	skeletal_sources_.erase(entity.packed);
	if (index_ == nullptr || model == nullptr || adm_name.empty()) return false;
	std::shared_ptr<const AdmSkeletalClips> rig;
	const auto cached = rig_cache_.find(rig_key);
	if (cached != rig_cache_.end()) {
		rig = cached->second;
	} else {
		std::vector<anim::Vec3> origins;
		std::vector<int> parents;
		if (!model_bone_table(*model, origins, parents)) return false;
		auto loaded = std::make_shared<AdmSkeletalClips>();
		if (!loaded->load_from_adm(index_, adm_name, origins, parents)) {
			// Negative results are not cached: a later mission mount can make
			// the .adm resolvable (mirrors the adapter's per-sweep resolve).
			return false;
		}
		rig = std::move(loaded);
		rig_cache_.emplace(rig_key, rig);
	}
	if (rig == nullptr || !rig->loaded() || !rig->fk_valid()) return false;
	SkeletalSource source;
	source.model_id = model_id;
	source.registry_spawn_id = registry_spawn_id;
	source.rig = std::move(rig);
	skeletal_sources_[entity.packed] = std::move(source);
	return true;
}

void SimCollisionPoseProvider::remove_entity(world::EntityHandle entity) {
	skeletal_sources_.erase(entity.packed);
}

bool SimCollisionPoseProvider::has_skeletal_entity(
		world::EntityHandle entity) const {
	return skeletal_sources_.find(entity.packed) != skeletal_sources_.end();
}

bool SimCollisionPoseProvider::build_section_matrices(world::World &world,
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

bool SimCollisionPoseProvider::eval_entity_pose(world::World &world,
		const SkeletalSource &source, world::EntityHandle entity,
		std::vector<anim::PoseBone> &r_pose, anim::AimOverlayAngles *r_angles,
		anim::AimOverlayInputs &r_inputs, const world::Entity *&r_entity,
		world::AiEntity *&r_ai) const {
	const AdmSkeletalClips *rig = source.rig.get();
	r_ai = world.ai != nullptr ? world.ai->for_handle(entity) : nullptr;
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
			resolve_primary_key(infantry_anim_key(r_ai->inf.anim_state));
	if (primary_key.empty()) return false;
	const float primary_fps = rig->clip_fps(primary_key, 0);
	const double primary_seconds = primary_fps > 0.0f
			? static_cast<double>(std::max(r_ai->inf.clip_phase, 0)) /
					(2.0 * primary_fps)
			: 0.0;
	std::string source_key;
	double source_seconds = 0.0;
	const bool primary_blend = r_ai->inf.body_blend_active();
	if (primary_blend) {
		source_key = resolve_primary_key(
				infantry_anim_key(r_ai->inf.anim_prev));
		const float source_fps = rig->clip_fps(source_key, 0);
		if (source_fps > 0.0f)
			source_seconds =
					static_cast<double>(
							std::max(r_ai->inf.anim_prev_clip_phase, 0)) /
					(2.0 * source_fps);
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
	if (world.cached.local_player.valid() &&
			entity.packed == world.cached.local_player.packed &&
			world::infantry_weapon_channel_visible(
					r_ai->inf, weapon_active,
					mount_blocks_weapon_channel(*r_entity))) {
		weapon_key = infantry_anim_key(r_ai->inf.wpn_state);
		const float weapon_fps = rig->clip_fps(weapon_key, 0);
		if (weapon_fps > 0.0f)
			weapon_seconds =
					static_cast<double>(
							std::max(r_ai->inf.wpn_clip_phase, 0)) /
					(2.0 * weapon_fps);
	}

	rig->eval_composed_pose(primary_key, primary_seconds, primary_blend,
			source_key, source_seconds, r_ai->inf.anim_blend_weight,
			deltas, weapon_key, weapon_seconds, r_pose);
	return true;
}

bool SimCollisionPoseProvider::build_skeletal(world::World &world,
		const SkeletalSource &source, world::EntityHandle entity,
		const world::CollisionMatrix &entity_world,
		const world::CollisionModel &model,
		std::vector<world::CollisionMatrix> &out) const {
	const AdmSkeletalClips *rig = source.rig.get();
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
	std::vector<AdmSkeletalClips::RestTransform> pose_global(section_count);
	out.resize(section_count);
	for (size_t i = 0; i < section_count; ++i) {
		AdmSkeletalClips::RestTransform local;
		anim::quat_to_mat3_rows(pose[i].rotation, local.rows);
		local.origin = pose[i].origin;
		// Retail zeroes the FINAL collision row after overlay/re-anchor.
		// Preserve that literal collision result for COBJ 16: composing
		// body_world here would incorrectly reintroduce the entity
		// translation; the zero-scale local (origin kept) still FK-chains any
		// children exactly like the adapter's collapsed pose.
		// [orig: special row @0x4b1290]
		if (collapse_right_hand && i == kRightHandBoneIndex) {
			std::memset(local.rows, 0, sizeof(local.rows));
			pose_global[i] = local;
			out[i] = world::CollisionMatrix{};
			continue;
		}
		const int parent = rig->parents()[i];
		pose_global[i] = parent >= 0
				? rest_mul(pose_global[static_cast<size_t>(parent)], local)
				: local;
		const AdmSkeletalClips::RestTransform deformation =
				rest_mul(pose_global[i], rig->rest_global_inverse()[i]);
		float render_pose[16];
		render_matrix_from_deformation(deformation, render_pose);
		if (!world::collision_matrix_apply_render_pose(
					body_world, render_pose, out[i]))
			return false;
	}
	return true;
}

bool SimCollisionPoseProvider::build_generic(world::World &world,
		const Threedi3di3 &model3di, world::EntityHandle entity,
		const world::CollisionMatrix &entity_world,
		const world::CollisionModel &model,
		std::vector<world::CollisionMatrix> &out) const {
	// Retail Generic collision always transforms the canonical first RLOD. It
	// never follows the render-selected LOD or scans for another live PANM.
	constexpr int lod_index = 0;
	if (!threedi_panm_lod_has_live(model3di, lod_index)) return false;
	std::vector<ThreediPartAnimation> effective;
	threedi_panm_effective_for_lod(model3di, lod_index, effective);
	if (effective.empty()) return false;
	if (model3di.ctrl.count > 0 && model3di.ctrl.registers == nullptr)
		return false;

	// The retail global CTRL bus, written by ordinal — absent keys and zero
	// values are indistinguishable on the bus, exactly like the adapter's
	// Dictionary translation.
	int32_t ctrl_values[THREEDI_CTRL_REGISTER_COUNT] = {};
	const world::Entity *e = world.registry.get(entity);
	world::AiEntity *ai_entity =
			world.ai != nullptr ? world.ai->for_handle(entity) : nullptr;
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
		if (world::emplaced_weapon_controls_for(world, world.ai, *e, emplaced)) {
			ctrl_values[THREEDI_CTRL_EWEAP_GUNYAW] =
					static_cast<int32_t>(emplaced.gun_yaw);
			ctrl_values[THREEDI_CTRL_EWEAP_GUNPITCH] =
					static_cast<int32_t>(emplaced.gun_pitch);
		}
	}
	const uint32_t time_ms = panm_time_override_ms >= 0
			? static_cast<uint32_t>(panm_time_override_ms)
			: world.logic_tick * 16u;

	std::vector<ThreediMatrix4x4> part_matrices;
	if (!threedi_panm_pose_parts(model3di, lod_index, time_ms, ctrl_values,
				part_matrices, nullptr))
		return false;

	// Default every COBJ slot to the Simple callback. Override only PANM nodes
	// whose target part ordinal exists as a collision section. This
	// intentionally ignores COBJ parent metadata and CXLT/offset records:
	// CVRT is model-space. The evaluator's row-major matrices are exactly the
	// render matrices the collision applier consumes — the adapter's
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

} // namespace opennova::simassets
