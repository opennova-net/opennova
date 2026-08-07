// The engine-side collision section-matrix provider (ADR 0028): poses .3di
// collision sections from the sim's OWN parsed models and clip sets — the
// PANM generic leg over threedi_panm_pose, and the person skeletal leg over
// AdmSkeletalClips — producing the same final world-space CollisionMatrix
// array the shell adapter's provider built through the render binding.
// [orig: the model+168 callback consumed in lockstep by
//  Physics_RaycastAgainstBoneCollision @ 0x4e4cb0; the person pose chain is
//  Entity_BuildBoneTransformMatrices @ 0x4b1290.]
#ifndef OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H
#define OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H

#include <simassets/adm_skeletal_clips.h>
#include <threedi/threedi_3di3.h>
#include <world/collision.h>

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace opennova {
class ResourceIndex;
}

namespace opennova::simassets {

class SimCollisionPoseProvider : public world::ICollisionSectionMatrixProvider {
public:
	// The rig source for skeletal registrations (the same mounted index the
	// SimModelCache reads). The provider never loads models itself — the
	// embedder's collision sweep passes each entity's parsed Threedi3di3.
	void set_resource_index(const ResourceIndex *index) { index_ = index; }
	void clear();

	// Register the pose model for a collision model id whose canonical first
	// RLOD carries live PANM (the generic leg's precondition; mirrors the
	// adapter's has-live-PANM registration gate). The pointer must outlive the
	// provider (SimModelCache retains its parses).
	void register_generic_model(int32_t model_id, const Threedi3di3 *model);

	// Register a person entity's skeletal source: the rig is the .adm clip
	// set sampled against the MODEL's bone table (parent-relative pivots +
	// parent indices from the canonical first RLOD's render objects). Rigs are
	// cached by rig_key (the embedder composes adm+graphic identity). Returns
	// false — and registers nothing — when the rig cannot load or its
	// FK/section preconditions fail, in which case the query path declines
	// exactly like the adapter's unregistered-source leg.
	bool register_skeletal_entity(world::EntityHandle entity,
			uint64_t registry_spawn_id, int32_t model_id,
			const std::string &rig_key, const std::string &adm_name,
			const Threedi3di3 *model);
	void remove_entity(world::EntityHandle entity);
	bool has_skeletal_entity(world::EntityHandle entity) const;
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

private:
	struct SkeletalSource {
		int32_t model_id = -1;
		uint64_t registry_spawn_id = 0;
		std::shared_ptr<const AdmSkeletalClips> rig;
	};

	bool build_skeletal(world::World &world, const SkeletalSource &source,
			world::EntityHandle entity,
			const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) const;
	bool build_generic(world::World &world, const Threedi3di3 &model3di,
			world::EntityHandle entity,
			const world::CollisionMatrix &entity_world,
			const world::CollisionModel &model,
			std::vector<world::CollisionMatrix> &out) const;

	const ResourceIndex *index_ = nullptr;
	std::unordered_map<int32_t, const Threedi3di3 *> generic_models_;
	std::unordered_map<uint64_t, SkeletalSource> skeletal_sources_;
	std::unordered_map<std::string, std::shared_ptr<const AdmSkeletalClips>>
			rig_cache_;
};

} // namespace opennova::simassets

#endif // OPENNOVA_SIMASSETS_SIM_COLLISION_POSE_H
