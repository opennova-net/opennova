#pragma once

// THE PRESENT-EFFECT POSE INDEX (ADR 0040 ladder E8): the pose an attached
// effect follows, by the identity its attachment names — wire handle, SSN,
// authored BMS id or spawn origin — resolved lazily against the role's view
// and cached for one decoded-client epoch (the kernel's logic tick and the
// replica's applied frame), misses included, so stale attachments never turn
// a lookup into a full entity scan per fixed tick. A joiner's decoded handles
// belong to the host, so only wire identity is meaningful there; host / listen
// views resolve every alias from the registry entity.

#include <runtime/inmatch/role_feeds.h>
#include <runtime/world/bms_handle_index.h>

#include <cstdint>
#include <unordered_map>
#include <unordered_set>

namespace opennova::replication {
struct ClientEntityState;
}

namespace opennova::inmatch {

// A pose in MISSION units and degrees (the embedder maps axes).
struct EffectPose {
	float x = 0.0f, y = 0.0f, z = 0.0f;
	float pitch_deg = 0.0f, yaw_deg = 0.0f, roll_deg = 0.0f;
};

inline uint64_t effect_origin_key(int kind, int index) {
	return (static_cast<uint64_t>(static_cast<uint32_t>(kind)) << 32) |
			static_cast<uint32_t>(index);
}

class EffectPoseIndex {
public:
	void invalidate();
	// Revalidate for this frame: a cold cache, another runtime, a new logic
	// tick or a newly applied replica frame rebuilds; no kernel or runtime
	// empties it.
	void ensure(const RoleView &view);
	// Null when the identity has no presented pose this epoch.
	const EffectPose *for_handle(const RoleView &view, uint16_t handle);
	const EffectPose *for_ssn(const RoleView &view, int ssn);
	const EffectPose *for_bms_id(const RoleView &view, int bms_id, world::BmsHandleIndex &bms_handles);
	const EffectPose *for_origin(const RoleView &view, int kind, int index);

private:
	const EffectPose *cached_(uint16_t handle) const;
	bool cache_(const RoleView &view, const replication::ClientEntityState &state);
	bool cache_(const RoleView &view, const world::Entity &entity);
	void alias_(const world::Entity &entity, uint16_t handle);

	bool valid_ = false;
	uint32_t logic_tick_ = 0;
	uint32_t client_frame_ = 0;
	const ClientRuntime *runtime_ = nullptr;
	std::unordered_map<uint16_t, EffectPose> poses_by_handle_;
	std::unordered_map<int, uint16_t> handles_by_bms_id_;
	std::unordered_map<int, uint16_t> handles_by_ssn_;
	std::unordered_map<uint64_t, uint16_t> handles_by_origin_;
	// A missing owner is also stable for one epoch; positive caches remain
	// authoritative when another alias materializes the same row.
	std::unordered_set<uint16_t> missing_handles_;
	std::unordered_set<int> missing_bms_ids_;
	std::unordered_set<int> missing_ssns_;
	std::unordered_set<uint64_t> missing_origins_;
};

} // namespace opennova::inmatch
