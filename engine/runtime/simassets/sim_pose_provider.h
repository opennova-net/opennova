// The engine-side collision section-matrix provider (ADR 0028): poses .3di
// collision sections from the sim's OWN parsed models and clip sets — the
// PANM generic leg over threedi_panm_pose, and the person skeletal leg over
// AdmSkeletalClips — producing the same final world-space CollisionMatrix
// array the shell binding's provider built through the render binding.
// [orig: the model+168 callback consumed in lockstep by
//  Physics_RaycastAgainstBoneCollision @ 0x4e4cb0; the person pose chain is
//  Entity_BuildBoneTransformMatrices @ 0x4b1290.]
#ifndef OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H
#define OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H

#include <runtime/anim/aim_overlay.h>
#include <runtime/simassets/adm_skeletal_clips.h>
#include <formats/threedi/threedi_3di3.h>
#include <runtime/world/collision.h>
#include <runtime/world/pose_provider.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {
class ResourceIndex;
}

namespace opennova::world {
struct AiEntity;
}

namespace opennova::simassets {

// The third-person held-weapon attach calibration and derivation — the ONE
// home, single-sourced for the render applier (PresentApplier's
// held_weapon_* statics) and the sim-side muzzle resolution alike.
//
// The original draws the held gun RIGID: one matrix is qmemcpy'd into EVERY
// bone slot of the weapon model, so it carries no skeleton, clip, or pose of
// its own — the whole appearance is the transform built from this
// calibration. Position is bone 16's own pivot nudged and carried through
// that bone's posed matrix — `M16 · (pivot16 + nudge)`; since `M16 · pivot16`
// IS the joint world position, that reduces to joint + M16_rotation · nudge.
// The nudge X term is authored in the x-negated render frame, so it is
// NEGATED here rather than copied.
//
// Orientation is one of TWO frames, chosen on a single bit:
//  * The ENTITY frame (default): the weapon's own attach triple through the
//    generic placed-object basis builder — a gfx3 poses exactly like a
//    placed .3di (weapon models are authored muzzle +Z / up +Y).
//  * The HAND frame: bone 16's own matrix with a fixed calibration,
//    `Ry_e · Rz_e · M16` — selected when the WEAPON-channel hold state
//    carries g_animStateFlagsTable bit 0x80 (knife/grenade/designator holds,
//    melee, binoculars, BOTH reloads, the death family). The calibration
//    angles survive to Godot unchanged in SIGN: the rotation builder stores
//    -sin θ (each block rotates by -θ) and conjugating through the loader's
//    X-negation flips it back — two inversions, so the authored constants
//    are used as-is (it looks like a missing negation and is not one).
//
// [orig: the weapon rides model bone 16 ".bad row BN17 R Hand"
//  (BoneCallback_org0_World draw 5 @ 0x4e3c87..0x4e3d99, all-bones fill
//  @ 0x4e3d71); matrix build @ 0x4b2180..0x4b22f8, translation-only
//  overwrite @ 0x4b22cf..0x4b22f8; entity attach basis @ 0x4b1bdc..0x4b1bf8;
//  pivot nudge flt_7C68E8 = 0.05 +X/-Y, flt_7C9BA8 = 0.051 +Z @ 0x4b2186;
//  hand-frame gate @ 0x4b21b6, branch @ 0x4b220f, Rz dbl_7C9BA0
//  @ 0x4b2215..0x4b2251, Ry dbl_7C9B98 @ 0x4b2256..0x4b22c2 via
//  Math_BuildRotationMatrix4x4_ByAxis @ 0x611db0 (sin negated via
//  dbl_7C57B0); the state's writer Entity_UpdateInfantryPlayerBody
//  @ 0x4b5dad..0x4b5ea9; the shared placement builder
//  Math_BuildFixedPointToFloatMatrix4x4 @ 0x612200.
//  world-wac-ai-re §14.2/§14.4/§14.4a/§14.4b]
inline constexpr int kHeldWeaponBoneIndex = 16;
inline constexpr float kHeldWeaponAttachNudgeX = -0.05f;
inline constexpr float kHeldWeaponAttachNudgeY = -0.05f;
inline constexpr float kHeldWeaponAttachNudgeZ = 0.051f;
inline constexpr double kHeldWeaponHandFrameZRad = 0.5759761961496483;
inline constexpr double kHeldWeaponHandFrameYRad = -1.3613982818082597;

// The model's bone table for a skeletal rig: parent-relative pivots and parent
// indices from the canonical first RLOD's render objects (raw, engine frame).
// The same rows AdmSkeletalClips::load_from_adm consumes; public so a test can
// stand a rig up from a mounted model without the collision resolve.
bool model_bone_table(const opennova::threedi::Threedi3di3 &model,
		std::vector<anim::Vec3> &r_origins, std::vector<int> &r_parents);

class SimPoseProvider : public world::IPoseProvider {
public:
	// The rig source for skeletal registrations (the same mounted index the
	// SimModelCache reads). The provider never loads models itself — the
	// embedder's collision sweep passes each entity's parsed Threedi3di3.
	// Switching the index drops every registration and cached rig: the
	// registered model pointers belong to the embedder's parse cache, whose
	// parses die with the old index (SimModelCache::set_index resets), and
	// the rigs were loaded from the old index's bytes.
	void set_resource_index(const ResourceIndex *index) {
		if (index_ != index) clear();
		index_ = index;
	}
	void clear();

	// Register the pose model for a collision model id whose canonical first
	// RLOD carries live PANM (the generic leg's precondition; mirrors the
	// binding's has-live-PANM registration gate). The pointer must outlive the
	// provider (SimModelCache retains its parses).
	void register_generic_model(int32_t model_id, const opennova::threedi::Threedi3di3 *model);
	// Register the parsed model behind a collision model id for the userpoint
	// leg (every model with a userpoint table, PANM or not). The pointer must
	// outlive the provider.
	void register_userpoint_model(int32_t model_id, const opennova::threedi::Threedi3di3 *model);

	// Register a person entity's skeletal source: the rig is the .adm clip
	// set sampled against the MODEL's bone table (parent-relative pivots +
	// parent indices from the canonical first RLOD's render objects). Rigs are
	// cached by rig_key (the embedder composes adm+graphic identity). Returns
	// false — and registers nothing — when the rig cannot load or its
	// FK/section preconditions fail, in which case the query path declines
	// exactly like the binding's unregistered-source leg.
	bool register_skeletal_entity(world::EntityHandle entity,
			uint64_t registry_spawn_id, int32_t model_id,
			const std::string &rig_key, const std::string &adm_name,
			const opennova::threedi::Threedi3di3 *model,
			const std::string &muzzle_userpoint = std::string());
	void remove_entity(world::EntityHandle entity);
	bool has_skeletal_entity(world::EntityHandle entity) const;
	// Whether a query for this model id has a registered pose source at all.
	// A build_section_matrices false WITHOUT a source is the normal rigid
	// path (CollisionWorld's identity sections); false WITH one is a real
	// decline — the embedder's masked-failure counter keys on this.
	bool has_generic_model(int32_t model_id) const {
		return generic_models_.find(model_id) != generic_models_.end();
	}
	// Diagnostics: the registered rig for an entity (null when none).
	const AdmSkeletalClips *skeletal_rig(world::EntityHandle entity) const {
		const auto it = skeletal_sources_.find(entity.packed);
		return it == skeletal_sources_.end() ? nullptr : it->second.rig.get();
	}

	// Per-query sim state the embedder syncs before dispatch: the local
	// player's weapon-in-hands flag (the weapon-channel visibility gate) and
	// the debug PANM clock override (-1 = the logic-tick clock).
	bool weapon_active = false;
	int64_t panm_time_override_ms = -1;

	// Late attachment stays with the embedder's collision sweep.
	bool ensure_collision_instance(world::World &, world::EntityHandle) override {
		return false;
	}
	bool build_section_matrices(world::World &world, world::EntityHandle entity,
			int32_t model_id, const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) override;
	bool resolve_muzzle_pose(world::World &world, world::EntityHandle entity,
			int32_t out[3]) override;
	bool resolve_userpoint_rigid(world::World &world, world::EntityHandle entity,
			int userpoint_index, int32_t out[3]) override;
	bool resolve_userpoint_transform(world::World &world,
			world::EntityHandle entity, int userpoint_index,
			int32_t out[6]) override;
	uint64_t muzzle_query_count() const { return muzzle_queries_; }
	uint64_t muzzle_resolve_count() const { return muzzle_resolves_; }

private:
	struct SkeletalSource {
		int32_t model_id = -1;
		uint64_t registry_spawn_id = 0;
		std::shared_ptr<const AdmSkeletalClips> rig;
		int32_t muzzle_bone = -1;
		int32_t muzzle_model_position[3] = {};
	};

	// The shared clip/blend/overlay/weapon-channel pose evaluation the
	// collision build runs (the @0x4b1290 chain up to the per-section
	// composition). r_pose holds parent-relative locals.
	bool eval_entity_pose(world::World &world, const SkeletalSource &source,
			world::EntityHandle entity, std::vector<anim::PoseBone> &r_pose,
			anim::AimOverlayAngles *r_angles, anim::AimOverlayInputs &r_inputs,
			const world::Entity *&r_entity, world::AiEntity *&r_ai) const;
	bool build_skeletal(world::World &world, const SkeletalSource &source,
			world::EntityHandle entity,
			const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) const;
	bool build_generic(world::World &world, const opennova::threedi::Threedi3di3 &model3di,
			world::EntityHandle entity,
			const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) const;
	// The canonical first RLOD's PANM part pose at the sim clock over the
	// retail CTRL bus (PLAYPARTANIM phases, HEAT_GLOW, EWEAP yaw/pitch).
	// False when the LOD has no live PANM (every part is the entity frame).
	bool panm_part_matrices(world::World &world, const opennova::threedi::Threedi3di3 &model3di,
			world::EntityHandle entity,
			std::vector<opennova::threedi::ThreediMatrix4x4> &r_part_matrices) const;

	const ResourceIndex *index_ = nullptr;
	std::unordered_map<int32_t, const opennova::threedi::Threedi3di3 *> generic_models_;
	std::unordered_map<int32_t, const opennova::threedi::Threedi3di3 *> userpoint_models_;
	std::unordered_map<uint64_t, SkeletalSource> skeletal_sources_;
	std::unordered_map<std::string, std::shared_ptr<const AdmSkeletalClips>>
			rig_cache_;
	uint64_t muzzle_queries_ = 0;
	uint64_t muzzle_resolves_ = 0;
};

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H
