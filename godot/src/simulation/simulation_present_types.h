// Private data types of the Simulation binding's present/perf legs, moved out
// of simulation.h (the 2500-line header ratchet): the capture-window session
// phase attribution, the present-row identity, the FollowOwner effect-pose
// cache entry, and the per-pool respawn-lifecycle mirror. Consumed only by the
// Simulation TUs; nothing here is bound.
#pragma once

#include <cstdint>

#include <godot_cpp/variant/vector3.hpp>

#include <net/npruntime/client_runtime.h>
#include <net/npruntime/host_session.h>

namespace godot {

// Capture-window-only attribution summed across every fixed tick consumed by
// one render frame: the engine's own per-pump records accumulate as-is
// (np::HostSessionPerf carries the ServerTickPerf world/replication split,
// np::ClientFramePerf the local ClientRuntime frame) plus the shell-side
// legs measured in the Simulation TUs. get_session_perf flattens them onto
// the F3 keys. Ordinary play leaves runtime_profiling_enabled_ false, so
// producers neither read clocks nor write these fields.
struct SessionPhasePerf {
	opennova::np::HostSessionPerf host_session;
	opennova::np::ClientFramePerf client;
	int64_t host_prep_us = 0; // viewport/input/request setup before host_session_pump
	int64_t host_player_us = 0; // the host's local view/weapon/medic pumps
	int64_t client_decode_us = 0; // the local ClientState fold (host) / joiner wire leg
	int64_t adm_resolve_us = 0;
};

// Exact identity/order of the most recently returned PF_* buffer. Dynamic
// values (pose, animation, visibility) deliberately do not participate:
// GDScript row plans may keep their offsets while reading fresh values.
struct PresentRowIdentity {
	int32_t wire_handle = 0;
	int32_t type_id = 0;
	int32_t bms_id = 0;
	int32_t kind = -1;
	int32_t index = -1;

	bool operator==(const PresentRowIdentity &p_other) const {
		return wire_handle == p_other.wire_handle &&
		       type_id == p_other.type_id &&
		       bms_id == p_other.bms_id &&
		       kind == p_other.kind &&
		       index == p_other.index;
	}
};

// FollowOwner consumes the same wire-decoded pose as the present pass, but it
// does so once per fixed tick inside a catch-up batch; this is the cached
// per-handle entry (see the cache block in simulation.h).
struct PresentEffectPose {
	Vector3 position;
	Vector3 rotation_deg;
};

// The decoded fold's dead->alive respawn revision, mirrored per pool row so
// WirePresentPass sees the same PF_RESPAWN_REVISION edges on every role.
struct PoolPresentLifecycle {
	uint64_t registry_spawn_id = 0;
	uint32_t respawn_revision = 0;
	bool dead_known = false;
	bool dead = false;
};

} // namespace godot
