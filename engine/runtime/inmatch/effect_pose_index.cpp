// The present-effect pose index — see effect_pose_index.h.

#include <runtime/inmatch/effect_pose_index.h>

#include <base/io/fixed.h>
#include <runtime/inmatch/client_runtime.h>
#include <runtime/inmatch/present_rows.h>
#include <runtime/mission/mission_kernel.h>
#include <runtime/replication/client_state.h>
#include <runtime/replication/entity_wire_bridge.h>
#include <runtime/world/angle.h>

namespace opennova::inmatch {

void EffectPoseIndex::invalidate() {
	valid_ = false;
	runtime_ = nullptr;
	poses_by_handle_.clear();
	handles_by_bms_id_.clear();
	handles_by_ssn_.clear();
	handles_by_origin_.clear();
	missing_handles_.clear();
	missing_bms_ids_.clear();
	missing_ssns_.clear();
	missing_origins_.clear();
}

void EffectPoseIndex::ensure(const RoleView &view) {
	if (view.kernel == nullptr || view.runtime == nullptr) {
		if (valid_) invalidate();
		return;
	}
	const replication::ClientState &client = view.runtime->state();
	const uint32_t logic_tick = view.kernel->world.logic_tick;
	if (valid_ && runtime_ == view.runtime && logic_tick_ == logic_tick &&
			client_frame_ == client.frames_applied)
		return;
	invalidate();
	logic_tick_ = logic_tick;
	client_frame_ = client.frames_applied;
	runtime_ = view.runtime;
	valid_ = true;
}

const EffectPose *EffectPoseIndex::cached_(uint16_t handle) const {
	const auto found = poses_by_handle_.find(handle);
	return found != poses_by_handle_.end() ? &found->second : nullptr;
}

void EffectPoseIndex::alias_(const world::Entity &entity, uint16_t handle) {
	if (entity.bms_id > 0) {
		const int bms_id = static_cast<int>(entity.bms_id);
		handles_by_bms_id_[bms_id] = handle;
		missing_bms_ids_.erase(bms_id);
	}
	if (entity.net_id > 0) {
		const int ssn = static_cast<int>(entity.net_id);
		handles_by_ssn_[ssn] = handle;
		missing_ssns_.erase(ssn);
	}
	const int kind = world::spawn_origin_kind(entity.spawn_origin);
	const int index = static_cast<int>(world::spawn_origin_index(entity.spawn_origin));
	const uint64_t origin = effect_origin_key(kind, index);
	handles_by_origin_[origin] = handle;
	missing_origins_.erase(origin);
}

bool EffectPoseIndex::cache_(const RoleView &view, const replication::ClientEntityState &state) {
	// Match the replica present rows' joiner self-filter: the host's wire echo
	// H is not drawn and therefore cannot own a presented effect. Packed handle
	// zero is a valid pool-0 identity, so presence rides the runtime's explicit
	// validity seam, never a zero sentinel.
	if (view.joiner && view.runtime != nullptr && view.runtime->has_self_handle() &&
			state.handle == view.runtime->self_handle())
		return false;
	if (poses_by_handle_.find(state.handle) != poses_by_handle_.end()) return true;
	// Host/listen presentation can recover the authored pitch and roll from the
	// authoritative registry. The compact peer row only carries yaw; joiners
	// therefore retain the wire-only zeroes here.
	const world::Entity *entity = view.joiner
			? nullptr
			: view.kernel->world.registry.get(world::EntityHandle{ state.handle });
	EffectPose pose;
	pose.x = static_cast<float>(state.x / io::kFp16OneD);
	pose.y = static_cast<float>(state.y / io::kFp16OneD);
	pose.z = static_cast<float>(state.z / io::kFp16OneD);
	pose.pitch_deg = entity != nullptr ? static_cast<float>(entity->pitch) : 0.0f;
	pose.yaw_deg = static_cast<float>(world::mission_yaw_deg_from_bam_heading(state.heading_bam));
	pose.roll_deg = entity != nullptr ? static_cast<float>(entity->roll) : 0.0f;
	poses_by_handle_[state.handle] = pose;
	missing_handles_.erase(state.handle);
	// A joiner's decoded handles belong to the host, so only wire identity is
	// meaningful there; host/listen views resolve every alias from the same
	// registry entity the present snapshot uses.
	if (view.joiner || entity == nullptr) return true;
	alias_(*entity, state.handle);
	return true;
}

bool EffectPoseIndex::cache_(const RoleView &view, const world::Entity &entity) {
	const uint16_t handle = entity.handle.packed;
	if (poses_by_handle_.find(handle) != poses_by_handle_.end()) return true;
	const world::AiEntity *ae = view.kernel->world.ai.for_handle(entity.handle);
	EffectPose pose;
	pose.x = entity.position.x;
	pose.y = entity.position.y;
	pose.z = entity.position.z;
	pose.pitch_deg = static_cast<float>(pool_present_pitch_deg(entity));
	pose.yaw_deg = static_cast<float>(
			pool_present_yaw_deg(entity, ae, replication::entity_class_of(entity)));
	pose.roll_deg = static_cast<float>(pool_present_roll_deg(entity));
	poses_by_handle_[handle] = pose;
	missing_handles_.erase(handle);
	alias_(entity, handle);
	return true;
}

const EffectPose *EffectPoseIndex::for_handle(const RoleView &view, uint16_t handle) {
	ensure(view);
	if (const EffectPose *cached = cached_(handle)) return cached;
	if (view.runtime == nullptr) return nullptr;
	if (missing_handles_.count(handle) != 0) return nullptr;
	if (!view.joiner) {
		const world::Entity *entity = view.kernel->world.registry.get(world::EntityHandle{ handle });
		if (entity != nullptr && cache_(view, *entity)) return cached_(handle);
		missing_handles_.insert(handle);
		return nullptr;
	}
	for (const replication::ClientEntityState &state : view.runtime->state().entities) {
		if (state.handle != handle) continue;
		if (cache_(view, state)) return cached_(handle);
		break;
	}
	missing_handles_.insert(handle);
	return nullptr;
}

const EffectPose *EffectPoseIndex::for_ssn(const RoleView &view, int ssn) {
	if (ssn <= 0) return nullptr;
	ensure(view);
	const auto found = handles_by_ssn_.find(ssn);
	if (found != handles_by_ssn_.end()) return cached_(found->second);
	if (view.runtime == nullptr || view.joiner) return nullptr;
	if (missing_ssns_.count(ssn) != 0) return nullptr;
	const world::Entity *match = nullptr;
	view.kernel->world.registry.for_each([&](const world::Entity &e) {
		if (match == nullptr && static_cast<int>(e.net_id) == ssn) match = &e;
	});
	if (match != nullptr && cache_(view, *match)) return cached_(match->handle.packed);
	missing_ssns_.insert(ssn);
	return nullptr;
}

const EffectPose *EffectPoseIndex::for_bms_id(const RoleView &view, int bms_id,
		world::BmsHandleIndex &bms_handles) {
	if (bms_id <= 0) return nullptr;
	ensure(view);
	const auto found = handles_by_bms_id_.find(bms_id);
	if (found != handles_by_bms_id_.end()) return cached_(found->second);
	if (view.runtime == nullptr || view.joiner) return nullptr;
	if (missing_bms_ids_.count(bms_id) != 0) return nullptr;
	const world::Entity *entity =
			view.kernel->world.registry.get(bms_handles.resolve(view.kernel->world, bms_id));
	if (entity != nullptr && cache_(view, *entity)) return cached_(entity->handle.packed);
	missing_bms_ids_.insert(bms_id);
	return nullptr;
}

const EffectPose *EffectPoseIndex::for_origin(const RoleView &view, int kind, int index) {
	if (kind < 0 || index < 0) return nullptr;
	ensure(view);
	const uint64_t requested = effect_origin_key(kind, index);
	const auto found = handles_by_origin_.find(requested);
	if (found != handles_by_origin_.end()) return cached_(found->second);
	if (view.runtime == nullptr || view.joiner) return nullptr;
	if (missing_origins_.count(requested) != 0) return nullptr;
	const world::Entity *match = nullptr;
	view.kernel->world.registry.for_each([&](const world::Entity &e) {
		if (match != nullptr) return;
		const int k = world::spawn_origin_kind(e.spawn_origin);
		const int i = static_cast<int>(world::spawn_origin_index(e.spawn_origin));
		if (effect_origin_key(k, i) == requested) match = &e;
	});
	if (match != nullptr && cache_(view, *match)) return cached_(match->handle.packed);
	missing_origins_.insert(requested);
	return nullptr;
}

} // namespace opennova::inmatch
