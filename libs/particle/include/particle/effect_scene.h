#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "particle/emitter.h"
#include "particle/particle.h"

namespace opennova::particle {

// The portable effect-world seam. EffectScene owns catalog resolution,
// admission/ownership, emitter lifetime, deterministic catch-up, and the
// value snapshots consumed by renderer adapters. It deliberately has no
// knowledge of Godot nodes, materials, textures, or weapon action names.

struct EffectHandle {
	std::uint32_t value = 0;

	explicit operator bool() const noexcept { return value != 0; }
};

inline bool operator==(EffectHandle a, EffectHandle b) noexcept {
	return a.value == b.value;
}

inline bool operator!=(EffectHandle a, EffectHandle b) noexcept {
	return !(a == b);
}

struct EffectGroupId {
	std::uint64_t value = 0;

	explicit operator bool() const noexcept { return value != 0; }
};

inline bool operator==(EffectGroupId a, EffectGroupId b) noexcept {
	return a.value == b.value;
}

inline bool operator!=(EffectGroupId a, EffectGroupId b) noexcept {
	return !(a == b);
}

// Slot identity governs spawn admission. Owner identity governs transform
// following. Keeping the two tokens distinct prevents unrelated effects on
// one entity from suppressing or replacing each other.
struct EffectSlotToken {
	std::uint64_t value = 0;

	explicit operator bool() const noexcept { return value != 0; }
};

struct EffectOwnerToken {
	std::uint64_t value = 0;

	explicit operator bool() const noexcept { return value != 0; }
};

struct EffectPose {
	Vec3 position{};
	Vec3 right = {1.0f, 0.0f, 0.0f};
	Vec3 up = {0.0f, 1.0f, 0.0f};
	Vec3 forward = {0.0f, 0.0f, 1.0f};
};

enum class EffectAdmission : std::uint8_t {
	// Every event creates a distinct group. Direct/recoil, casing, impact,
	// explosion, and other transient presentation events use this policy.
	Always = 0,
	// Atomically replace the group currently occupying the slot.
	ReplaceOwned = 1,
	// Keep the current live group and report Suppressed. This is the witnessed
	// weapon FIRE/action-start live-handle rule, not a name-based effect rule.
	SuppressWhileOwned = 2,
};

enum class EffectBinding : std::uint8_t {
	World = 0,
	FollowOwner = 1,
};

enum class EffectRenderDomain : std::uint8_t {
	World = 0,
	FirstPerson = 1,
};

enum class EffectKillPlane : std::uint8_t {
	Disabled = 0,
	KillAbove = 1,
	KillAtOrBelow = 2,
};

struct EffectCatalogDocument {
	std::string source;
	ParticleFile file;
};

struct EffectSceneConfig {
	std::vector<EffectCatalogDocument> documents;
	// Initial-age replay uses this fixed step so a catch-up spawn follows the
	// same emission and physics cadence as ordinary world simulation.
	float simulation_tick_seconds = 1.0f / 62.5f;
	// Zero means unbounded. Non-zero limits reject explicitly rather than
	// silently dropping an effect or a subset of its emitters.
	std::size_t max_live_groups = 1024;
	std::size_t max_live_emitters = 4096;
	std::uint32_t random_seed = 1;
};

struct EffectLoadReport {
	std::size_t document_count = 0;
	std::size_t effect_count = 0;
	std::size_t particle_definition_count = 0;
	std::size_t table_definition_count = 0;
	std::size_t duplicate_effect_count = 0;
	std::size_t duplicate_particle_count = 0;
	// Counted once per failed effect, at its first missing pdefs member —
	// the resolve is all-or-nothing and stops there, matching the single
	// retail log line [orig: CEffectBank_ResolveAllEntries @ 0x5e4920].
	std::size_t unresolved_particle_reference_count = 0;
};

// Catch-up aging is presentation compensation, not an arbitrary seek loop.
// Four seconds leaves ample local/network headroom while bounding hostile or
// corrupted requests that would otherwise advance emitters billions of times.
inline constexpr std::uint32_t kEffectInitialAgeTickLimit = 256;

// A single public advance call performs at most this many fixed simulation
// ticks. Excess whole ticks are presentation backlog and are discarded; the
// sub-tick remainder is retained for deterministic accumulation.
inline constexpr std::uint32_t kEffectAdvanceTickLimit = 256;

struct EffectSpawnRequest {
	EffectHandle effect;
	EffectPose pose;
	EffectAdmission admission = EffectAdmission::Always;
	EffectBinding binding = EffectBinding::World;
	EffectRenderDomain render_domain = EffectRenderDomain::World;
	EffectSlotToken slot;
	EffectOwnerToken owner;
	// Used only for FollowOwner. pose is the initial world-space fallback
	// until an owner pose has been supplied; subsequent owner updates compose
	// this local pose with the owner's world pose.
	EffectPose owner_relative_pose;
	std::uint32_t initial_age_ticks = 0;
	std::uint64_t source_tick = 0;
	std::uint64_t source_order = 0;

	Vec3 color_tint = {1.0f, 1.0f, 1.0f};
	float spring_const = 0.0f;
	std::uint32_t lod_divisor = 1;
	EffectKillPlane kill_plane = EffectKillPlane::Disabled;
	float kill_plane_y = 0.0f;
};

enum class EffectSpawnStatus : std::uint8_t {
	Spawned = 0,
	Suppressed = 1,
	InvalidHandle = 2,
	EmptyEffect = 3,
	MissingSlot = 4,
	MissingOwner = 5,
	GroupCapacityReached = 6,
	EmitterCapacityReached = 7,
};

struct EffectSpawnReceipt {
	EffectSpawnStatus status = EffectSpawnStatus::InvalidHandle;
	EffectHandle effect;
	EffectGroupId group;
	EffectGroupId replaced_group;

	bool spawned() const noexcept {
		return status == EffectSpawnStatus::Spawned;
	}

	bool accepted() const noexcept {
		return status == EffectSpawnStatus::Spawned ||
				status == EffectSpawnStatus::Suppressed;
	}
};

struct EffectOwnerPoseUpdate {
	EffectOwnerToken owner;
	EffectPose pose;
	// present=false removes the owner and detaches all groups following it.
	bool present = true;
};

struct EffectAdvanceRequest {
	float delta_seconds = 0.0f;
};

struct EffectBounds {
	Vec3 minimum{};
	Vec3 maximum{};
	bool valid = false;
};

struct EffectLiveCounts {
	std::size_t group_count = 0;
	std::size_t emitter_count = 0;
	std::size_t particle_count = 0;
};

struct EffectGroupFrameSnapshot {
	EffectGroupId id;
	EffectHandle effect;
	std::string effect_name;
	std::string source;
	EffectPose pose;
	EffectRenderDomain render_domain = EffectRenderDomain::World;
	std::uint64_t source_tick = 0;
	std::uint64_t source_order = 0;
	std::size_t first_emitter = 0;
	std::size_t emitter_count = 0;
	bool detached = false;
};

struct EffectEmitterFrameSnapshot {
	std::uint64_t id = 0;
	std::size_t group_index = 0;
	std::size_t ordinal = 0;
	std::size_t definition_index = 0;
	std::size_t first_particle = 0;
	std::size_t particle_count = 0;
	Vec3 position{};
	Vec3 forward = {0.0f, 0.0f, 1.0f};
	Vec3 color_tint = {1.0f, 1.0f, 1.0f};
	float age = 0.0f;
	float spring_const = 0.0f;
	std::uint32_t lod_divisor = 1;
	EffectKillPlane kill_plane = EffectKillPlane::Disabled;
	float kill_plane_y = 0.0f;
};

struct ParticleFrameSnapshot {
	std::uint64_t frame_index = 0;
	double simulation_time_seconds = 0.0;
	// Definitions are immutable for the life of this snapshot, including
	// across a subsequent EffectScene::open call.
	std::shared_ptr<const std::vector<ParticleDef>> definitions;
	std::vector<EffectGroupFrameSnapshot> groups;
	std::vector<EffectEmitterFrameSnapshot> emitters;
	std::vector<Particle> particles;
};

struct EffectEmitterDebugSnapshot {
	std::uint64_t id = 0;
	std::size_t ordinal = 0;
	std::size_t definition_index = 0;
	std::string definition_name;
	std::uint32_t definition_flags = 0;
	std::size_t alive_particle_count = 0;
	bool emitting = false;
	Vec3 position{};
	Vec3 forward = {0.0f, 0.0f, 1.0f};
	float age = 0.0f;
	EffectKillPlane kill_plane = EffectKillPlane::Disabled;
	float kill_plane_y = 0.0f;
	EffectBounds bounds;
};

struct EffectGroupDebugSnapshot {
	EffectGroupId id;
	EffectHandle effect;
	std::string effect_name;
	std::string source;
	EffectAdmission admission = EffectAdmission::Always;
	EffectBinding binding = EffectBinding::World;
	EffectRenderDomain render_domain = EffectRenderDomain::World;
	EffectSlotToken slot;
	EffectOwnerToken owner;
	bool detached = false;
	EffectPose pose;
	std::uint64_t source_tick = 0;
	std::uint64_t source_order = 0;
	EffectBounds bounds;
	std::vector<EffectEmitterDebugSnapshot> emitters;
};

struct EffectDebugSnapshot {
	EffectLoadReport load;
	std::size_t interned_effect_count = 0;
	std::size_t live_group_count = 0;
	std::size_t live_emitter_count = 0;
	std::size_t live_particle_count = 0;
	std::size_t group_pool_high_water = 0;
	std::size_t emitter_pool_high_water = 0;
	std::size_t suppressed_spawn_count = 0;
	std::size_t rejected_spawn_count = 0;
	std::size_t capacity_rejection_count = 0;
	std::vector<EffectGroupDebugSnapshot> groups;
};

class EffectScene {
public:
	EffectScene();
	~EffectScene();

	EffectScene(EffectScene &&) noexcept;
	EffectScene &operator=(EffectScene &&) noexcept;

	EffectScene(const EffectScene &) = delete;
	EffectScene &operator=(const EffectScene &) = delete;

	// Replaces the complete scene and compiles a first-registration-wins,
	// globally-resolved immutable catalog.
	EffectLoadReport open(const EffectSceneConfig &config);

	// Case-insensitive, stable 1-based handles. An unknown name aliases the
	// catalog's stockeffect definition; returns 0 when stockeffect is absent.
	EffectHandle intern(std::string_view effect_name);
	std::string effect_name(EffectHandle handle) const;

	EffectSpawnReceipt spawn(const EffectSpawnRequest &request);
	void apply_owner_poses(const std::vector<EffectOwnerPoseUpdate> &updates);
	// Unique live FollowOwner tokens in deterministic group order. This narrow
	// query avoids constructing the particle-bounds debug snapshot on hot paths.
	std::vector<EffectOwnerToken> active_owner_tokens() const;
	void detach(EffectGroupId group);
	void detach_slot(EffectSlotToken slot);

	// Clears every live/runtime value while retaining the compiled catalog,
	// immutable definition identity, and stable interned handles. Mission
	// restart uses this instead of open() so load-time renderer/catalog warmup
	// remains valid for the next play session.
	void reset_runtime_state();

	// Advances all live emitters and reclaims completed groups without copying
	// render values. Embedders that batch multiple fixed ticks materialize only the
	// final frame through write_snapshot().
	void advance_simulation(const EffectAdvanceRequest &request);
	// Rewrites a retained snapshot in deterministic spawn order, reusing its
	// vector capacities across frames.
	void write_snapshot(ParticleFrameSnapshot &snapshot) const;
	// Compatibility wrapper for callers that consume every requested frame.
	ParticleFrameSnapshot advance(const EffectAdvanceRequest &request);
	// Scalar counts never materialize render values or walk individual particles.
	EffectLiveCounts live_counts() const noexcept;
	// include_bounds=false is the UI/debug hot path: it retains topology and
	// emitter metadata but avoids scanning every particle for simulation bounds.
	EffectDebugSnapshot inspect(bool p_include_bounds = true) const;

private:
	struct Impl;
	std::unique_ptr<Impl> impl_;
};

} // namespace opennova::particle
