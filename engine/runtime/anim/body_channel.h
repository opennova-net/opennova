// One clip-posed model's body channel as presentation runs it (ADR 0077): the playing clip, its
// served ring entry and its playhead; the authoritative two-channel blend; the receive-side remote
// body-state arbitration over RemoteBodyState (current, pending, the blend in, the deferred
// promotion); the upper-body weapon channel's latch; the aim overlay's class deltas; the ragdoll's
// pose override; and the pose they compose over the rig (SkeletalClips). A backend writes the
// composed pose (pose()) to its device skeleton and reads nothing back; the attach legs build their
// bone frames from the same pose (world/entity_pose.h posed_bone_frames).
//
// The playhead is pinned (an external phase) on every path the simulation drives: a clip-at, a
// blend-at and a slot-at pose the tick the channel's owner advanced, as retail's channel moves only
// in its entity update [orig: AnimChannel_AdvancePlayback @0x40B140, from AnimMap_UpdateEntity
// @0x40B5F0]. A clip played without a phase (a preview's idle, a wire row whose compact omits the
// phase, a remote request seeded at its arrival) runs free on the frame clock the owner hands
// advance(): the main loop's GetTickCount difference (world/tick_count_clock.h FrameTickElapsed),
// never a backend's own frame delta.
#pragma once

#include <runtime/anim/aim_overlay.h>
#include <runtime/anim/remote_body_state.h>
#include <runtime/anim/skeletal_clips.h>
#include <runtime/anim/skeletal_pose.h>

#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace opennova::anim {

// The upper-body weapon channel a person composes under its aim overlay: the secondary channel's
// clip at its own playhead, spliced onto the mask bones, cross-fading from its outgoing clip
// [orig: the producer Entity_UpdateInfantryPlayerBody @0x4B5DAD; the mask override in
// Entity_BuildBoneTransformMatrices @0x4B14DB/@0x4B16A7; world-wac-ai-re.md §14.8]. An empty key
// is the gate off; `parked` holds the clip's last frame on the armed end-notify's boundary tick
// [orig: AnimChannel_AdvancePlayback @0x40B19E..0x40B1B1].
struct WeaponChannel {
	std::string key;
	int32_t phase_ticks = 0;
	std::string prev_key; // empty: no cross-fade
	int32_t prev_phase_ticks = 0;
	float blend_weight = 1.0f;
	int32_t variant = 0;
	int32_t prev_variant = 0;
	bool parked = false;

	bool operator==(const WeaponChannel &o) const {
		return key == o.key && phase_ticks == o.phase_ticks && prev_key == o.prev_key &&
				prev_phase_ticks == o.prev_phase_ticks && blend_weight == o.blend_weight &&
				variant == o.variant && prev_variant == o.prev_variant && parked == o.parked;
	}
	bool operator!=(const WeaponChannel &o) const { return !(*this == o); }
};

// The verbs a present walk drives one body with. A BodyChannel takes them itself
// (BodyChannelVerbs below); a backend's model fans each out over its own channel and the
// channels of the parts linked to it.
class BodyChannelTarget {
public:
	virtual ~BodyChannelTarget() = default;
	// A clip with no phase: runs free from its start (play_free).
	virtual void play_free(const std::string &key) = 0;
	// The clip at the simulation's tick (play_at), or the outgoing clip blended under it.
	virtual void play_at(const std::string &key, int32_t phase_ticks, int32_t variant, bool parked) = 0;
	virtual void play_blend_at(const std::string &source_key, int32_t source_phase_ticks,
			const std::string &key, int32_t phase_ticks, float weight, int32_t source_variant,
			int32_t variant) = 0;
	// The coarse body-anim slot (world::BodyAnim), free-running or at a tick.
	virtual void play_slot(int32_t slot) = 0;
	virtual void play_slot_at(int32_t slot, int32_t phase_ticks, bool parked) = 0;
	// The receive-side arbitration: a raw wire request, and one simulation tick of an
	// accepted transition. Each returns whether a transition is still in flight (the
	// caller keeps ticking it), across every channel the target drives.
	virtual bool remote_request(int32_t state, const std::string &key, uint32_t flags,
			int32_t phase_ticks) = 0;
	virtual bool remote_tick(int32_t state) = 0;
};

class BodyChannel {
public:
	using Rig = std::shared_ptr<const SkeletalClips>;
	// What a verb asks of the device after it ran: nothing, the composed pose written now (the
	// channel's state changed where a caller may read the skin the same frame), or every bone
	// back at its rest (no clip resolves at all).
	enum class PoseRequest : uint8_t { kNone, kPose, kRest };

	// Bind the rig (null unbinds): every channel state resets, the pose re-composes.
	void bind(Rig rig);
	const SkeletalClips *rig() const { return rig_.get(); }
	// A clip and a rig: the channel has a pose to compose.
	bool active() const { return rig_ != nullptr && !key_.empty(); }
	// Whether the rig draws through a Person's bone builder (every clip-posed model but a
	// first-person view model), whose rows past the clip's bones take that builder's pose
	// (SkeletalClips::pose_person_rows_past_clip).
	void set_person(bool person);
	bool person() const { return person_; }

	// --- the clip selection ------------------------------------------------------------------
	// A missing semantic key resolves to the rig's RESET binding (resolve_key) wherever a verb
	// takes a state key; a served variant other than 0 plays its key as given.
	// The clip from its start, free-running (a repeat of the playing clip keeps its playhead).
	// False when the key does not resolve.
	bool play(const std::string &key, int variant);
	// The clip at a playhead in seconds, pinned.
	void play_at_time(const std::string &key, int variant, double seconds);
	// The clip at the simulation's tick, pinned; `parked` samples the armed end-notify's boundary.
	void play_at(const std::string &key, int phase_ticks, int variant = 0, bool parked = false);
	// The outgoing and incoming primary channels at one shared weight, as the channel blend runs
	// them [orig: AnimChannel_BlendTwoChannels @0x410740 -> Entity_BuildBoneTransformMatrices
	// @0x4B1290]; weight 1 (or no source) is the target's play_at, no target the source's.
	void play_blend_at(const std::string &source_key, int source_phase_ticks,
			const std::string &target_key, int target_phase_ticks, double weight,
			int source_variant = 0, int variant = 0);
	// The clip seeded at a tick, then free-running.
	void play_seeded(const std::string &key, int phase_ticks);
	// The coarse slot's key on this rig (its idle/reset fallbacks), memoised; empty when none.
	const std::string &slot_key(int slot);
	// The key a free slot play would start: false when nothing resolves or the slot's clip already
	// runs free (a repeated slot never restarts its loop).
	bool slot_play_key(int slot, std::string &r_key) const;
	// Scrub the playing clip: unpinned, posed now.
	bool set_time(double seconds);
	void stop();

	// --- the receive-side arbitration (a remote body's raw wire requests) ---------------------
	// [orig: NetPacket_SerializePlayerState @0x4C09C0, the per-record arbitration
	//  @0x4C1153..0x4C11A6; the infantry twin NetPacket_SerializeInfantryEntityState @0x4C0320
	//  @0x4C0859; AnimMap_UpdateEntity @0x40B5F0 the deferred promotion and the blend in]
	void remote_request(int state, const std::string &key, uint32_t flags, int phase_ticks);
	// One simulation tick of the transition in flight; false when none ran (no blend, no
	// pending state, or the tick names neither the current nor the pending state).
	bool remote_tick(int state);
	void reset_remote();
	bool remote_needs_tick() const { return remote_.needs_tick(); }
	bool remote_pending() const { return remote_.has_pending(); }

	// --- the layers the pose composes ---------------------------------------------------------
	// Each returns whether the layer changed (the pose then re-composes).
	bool set_weapon_channel(const WeaponChannel &channel);
	const WeaponChannel &weapon_channel() const { return weapon_; }
	bool has_weapon_channel() const { return !(weapon_.key.empty() && weapon_.phase_ticks < 0); }
	bool set_aim_overlay(const Quat deltas[kOverlayClassCount]);
	bool clear_aim_overlay();
	bool aim_overlay_valid() const { return overlay_valid_; }
	const std::array<Quat, kOverlayClassCount> &aim_overlay() const { return overlay_; }
	// The ragdoll corpse's pose over the clip (ADR 0060): weight 1 draws it alone, below 1 it
	// blends back toward the clip. clear returns false when none was held.
	void set_override(const std::vector<PoseBone> &pose, float weight);
	bool clear_override();
	bool has_override() const { return override_weight_ > 0.0f; }

	// --- the clock and the pose ---------------------------------------------------------------
	// The frame clock's elapsed seconds (0 while the owner is paused): a free-running clip moves,
	// and a deferred remote state due at the playhead promotes.
	void advance(double seconds);
	PoseRequest take_pose_request();
	// Whether the channel has frame work: a pose to write, a free-running clip on a running
	// clock, or a remote state waiting on the playhead.
	bool needs_frame(bool clock_running) const;
	bool dirty() const { return dirty_; }
	void mark_dirty() { dirty_ = true; }
	// Compose the pose from the channel's state into pose() and clear the dirt: the override, or
	// the primary sample (blended over the channel window) -> the weapon splice -> the aim overlay
	// -> a Person's rows past its clip, in the witnessed order [orig: Entity_BuildBoneTransformMatrices
	// @0x4B1290, @0x4B14A7..0x4B16A7 before the per-bone overlay loop; world-wac-ai-re.md §14.8.6].
	// False (pose() untouched) when inactive.
	bool compose();
	// The pose last composed, parent-local per bone; empty while the skeleton stands at its rest.
	const std::vector<PoseBone> &pose() const { return posed_; }
	// The playing clip's own pose (no overlay, no weapon splice) at the playhead plus an offset;
	// false when no clip plays.
	bool eval_clip_pose(double offset_seconds, std::vector<PoseBone> &r_pose) const;

	// --- reads --------------------------------------------------------------------------------
	const std::string &key() const { return key_; }
	int variant() const { return variant_; }
	// The playhead as posed: wrapped on a loop, clamped on a one-shot.
	double playhead() const;
	bool has_blend() const { return !(blend_source_key_.empty() && blend_weight_ >= 1.0f); }
	const std::string &blend_source_key() const { return blend_source_key_; }
	double blend_source_time() const { return blend_source_time_; }
	float blend_weight() const { return blend_weight_; }
	int blend_source_variant() const { return blend_source_variant_; }
	// The playhead of `key` at `phase_ticks` on its served `variant` (parked: the armed boundary).
	double clip_phase_seconds(const std::string &key, int phase_ticks, int variant = 0,
			bool parked = false) const;

private:
	const std::string &resolve_key(const std::string &key) const;
	bool looping(const std::string &key, int variant) const;
	void set_playhead(double seconds);
	void clear_blend();
	void reset_pose();
	void request_pose() { request_ = PoseRequest::kPose; }
	bool select_seeded(const std::string &key, int phase_ticks);
	void pose_blend_at_times(const std::string &source_key, double source_time,
			const std::string &target_key, double target_time, float weight,
			int source_variant, int target_variant);
	void accept_remote(const std::string &key, uint32_t flags, int phase_ticks, bool had_current);
	void queue_remote(const std::string &key);
	void clear_remote_blend();
	int phase_ticks_of(const std::string &key, double seconds) const;
	void start_remote_blend(const std::string &target_key, uint32_t flags, int target_phase);
	bool promote_remote_if_due();

	Rig rig_;
	bool person_ = true;
	std::string key_;
	int variant_ = 0;
	double time_ = 0.0;
	bool playing_ = false;
	bool external_phase_ = false;
	// The tick (and park) a play_at last posed: a repeat of it with nothing else dirty is a no-op.
	bool stamp_valid_ = false;
	int stamp_ticks_ = 0;
	bool stamp_parked_ = false;
	std::string blend_source_key_;
	double blend_source_time_ = 0.0;
	float blend_weight_ = 1.0f;
	int blend_source_variant_ = 0;
	int last_slot_ = -1;
	std::string last_slot_key_;
	RemoteBodyState remote_;
	std::string remote_pending_key_;
	std::string remote_blend_source_key_;
	WeaponChannel weapon_;
	std::array<Quat, kOverlayClassCount> overlay_{};
	bool overlay_valid_ = false;
	std::vector<PoseBone> override_;
	float override_weight_ = 0.0f;
	bool dirty_ = true;
	PoseRequest request_ = PoseRequest::kNone;
	std::vector<PoseBone> posed_;
};

// A BodyChannel driven as a BodyChannelTarget, posing nothing: what the engine's own consumers
// and the tests drive a single channel with.
class BodyChannelVerbs final : public BodyChannelTarget {
public:
	explicit BodyChannelVerbs(BodyChannel &channel) : channel_(channel) {}
	void play_free(const std::string &key) override { channel_.play(key, 0); }
	void play_at(const std::string &key, int32_t phase_ticks, int32_t variant, bool parked) override {
		channel_.play_at(key, phase_ticks, variant, parked);
	}
	void play_blend_at(const std::string &source_key, int32_t source_phase_ticks, const std::string &key,
			int32_t phase_ticks, float weight, int32_t source_variant, int32_t variant) override {
		channel_.play_blend_at(source_key, source_phase_ticks, key, phase_ticks, weight, source_variant,
				variant);
	}
	void play_slot(int32_t slot) override {
		std::string key;
		if (channel_.slot_play_key(slot, key)) channel_.play(key, 0);
	}
	void play_slot_at(int32_t slot, int32_t phase_ticks, bool parked) override {
		const std::string key = channel_.slot_key(slot);
		if (!key.empty()) channel_.play_at(key, phase_ticks, 0, parked);
	}
	bool remote_request(int32_t state, const std::string &key, uint32_t flags,
			int32_t phase_ticks) override {
		channel_.remote_request(state, key, flags, phase_ticks);
		return channel_.remote_needs_tick();
	}
	bool remote_tick(int32_t state) override {
		return channel_.remote_tick(state) && channel_.remote_needs_tick();
	}

private:
	BodyChannel &channel_;
};

} // namespace opennova::anim
