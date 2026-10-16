#pragma once

// The ragdoll corpses' device leg (ADR 0060): once a frame it hands the
// engine's RagdollSet every skeletal body the walks presented (and the local
// avatar), steps it by the frame's logic ticks against the world as the
// session holds it, and writes each ragdoll's pose over its model's clip.
// Everything it decides is the engine's (runtime/ragdoll,
// world/corpse_ragdoll); this side only reads models and writes their poses.

#include <runtime/ragdoll/ragdoll_set.h>
#include <runtime/world/corpse_ragdoll.h>

#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/classes/ref.hpp>

#include <cstdint>
#include <memory>
#include <unordered_map>
#include <vector>

namespace opennova::anim {
class SkeletalClips;
}
namespace opennova::world {
class World;
}

namespace godot {

class EntityPresenter;
class ObjectModel;
class Simulation;
class SkeletalAnim;

class CorpseRagdollPresenter {
public:
	void set_enabled(bool p_enabled);
	bool enabled() const { return set_.enabled(); }
	// One frame: `p_ticks` logic ticks ran this frame.
	void present(
			EntityPresenter *p_entities, Simulation *p_sim, ObjectModel *p_avatar, int p_ticks);
	// Every override handed back to its clip, every ragdoll dropped.
	void clear();
	opennova::ragdoll::RagdollStats stats() const { return set_.stats(); }

private:
	struct RigEntry {
		Ref<SkeletalAnim> anim; // keeps the rig the plan was built over alive
		std::shared_ptr<const opennova::ragdoll::Rig> rig;
	};
	std::shared_ptr<const opennova::ragdoll::Rig> rig_for(ObjectModel *p_model);
	// `p_key` keys the subject in the set; `p_wire_key` is its wire handle, which
	// seeds its kick (the same as the key for a row, the local player's handle
	// for the avatar).
	void observe(const opennova::world::World &p_world, ObjectModel *p_model, uint32_t p_key,
			uint32_t p_wire_key, int32_t p_life, int32_t p_anim_state, bool p_hidden,
			bool p_eligible, const opennova::ragdoll::Collider &p_collider);

	opennova::ragdoll::RagdollSet set_;
	opennova::world::LocalAvatarLife avatar_life_;
	std::unordered_map<const opennova::anim::SkeletalClips *, RigEntry> rigs_;
	// The models this frame observed, and those carrying an override.
	std::unordered_map<uint32_t, ObjectID> seen_;
	std::unordered_map<uint32_t, ObjectID> drawn_;
};

} // namespace godot
