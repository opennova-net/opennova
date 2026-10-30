#pragma once

// THE MISSION'S AMBIENT EMITTERS: the bookkeeping between the emitter table
// (AmbientMixer) and the eight channels the device plays (AmbientChannelPool),
// which the Godot mission audio kept until ADR 0077 R2. It holds the placed envs
// markers described as candidates (each layer of each set a marker registers,
// with a stable candidate id), replays the entity-attached registrations in
// producer-tick order into the table [orig: SoundEmitter_Register @0x529270 ->
// SoundEmitter_RegisterSetLayers @ 0x528340, before the render-frame
// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0], keys each (source, lane) to its own
// candidate ids while it lives, drives the table's clock from the world's
// logic tick (or a wall clock for an owner with none), and each render frame
// ranks the live slots and plans which candidate rides which channel. The device
// resolves an entrant's wave to a stream and plays the plan: players, streams,
// bus writes. Positions here are the presentation frame the listener is in
// (mission (x, y, z) -> (x, z, -y), mission::bms_to_presentation_position).
//
// Candidate ids are the port's (retail's slot table has none): a marker's are
// fixed for the mission, a lane's for as long as it stays registered with the
// same set, so an incumbent keeps its channel across refreshes; a retired id is
// reissued only once no channel carries it.

#include <runtime/audio/ambient_channel_pool.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/world/sound_emitter_mailbox.h>

#include <array>
#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opennova::audio {

// The table a candidate registered from: a placed marker's layers play on the
// device's ambient output, an entity-attached lane's on its effects output.
enum class AmbientSource : uint8_t { Marker, Emitter };

// One layer the mix may bind to a channel: its member 0's wave as the bank
// names it (the device resolves the stream from it) and the table it came from.
struct AmbientCandidateRef {
	std::string wave;
	AmbientSource source = AmbientSource::Marker;
};

// What the emitters ask the loaded bank chain (the device's banks): whether it
// holds a set [orig: SoundBank_FindSetByNameAnyBank @ 0x5274f0], and a set's
// layers as the emitter mix takes them (ambient_layer_rows: member 0 of each
// layer whose wave the device's files hold), empty for a set it lacks.
struct AmbientSetSource {
	std::function<bool(const std::string &set_name)> has_set;
	std::function<std::vector<AmbientLayerRow>(const std::string &set_name)> layers;
};

// The counts the device reports: the envs rows offered and the markers that
// registered a set, the marker layers described as candidates, the candidates
// the channel plan asked the device to resolve (once each) and the resolves
// that failed (a failed candidate is cached out of the mix).
struct AmbientEmitterStats {
	int markers_total = 0;
	int markers_resolved = 0;
	int candidates = 0;
	int candidates_validated = 0;
	int decode_failures = 0;
};

class AmbientEmitters {
public:
	// Asked once per entrant the pool would bind: true when the device has a
	// stream for the candidate's wave. A false answer caches the candidate out.
	using Resolve =
			std::function<bool(int32_t candidate_id, const AmbientCandidateRef &candidate)>;

	explicit AmbientEmitters(int channel_budget = kAmbientMixChannels);

	// The bank chain the markers and the lanes describe their sets through.
	void set_sets(AmbientSetSource sets) { sets_ = std::move(sets); }

	// Back to cold (a mission's setup or teardown): no markers, lanes, queued
	// registrations or candidates, the pool's channels gone, ids from 1, not live.
	// The time of day stays.
	void clear();

	// Describe envs rows (resolve_envs_markers) as markers: each authored slot
	// whose set the chain holds is playable, an empty or unknown one silent in its
	// hours; each distinct playable set's layers become candidates. A row with no
	// described set registers nothing. The table takes them at restart().
	void add_envs_markers(const std::vector<EnvsMarker> &rows);

	// (Re)start the emitter table over every marker held: a fresh table at tick 0,
	// the lanes and the queued registrations forgotten, the world clock unlatched.
	// The channels keep their candidates (a marker's id survives a restart).
	void restart();
	bool live() const { return live_; }

	// The mission clock, HHMM (MissionEnvironment.time_of_day), as the markers'
	// time-of-day regions read it.
	void set_time_of_day_hhmm(double hhmm);

	// Entity-attached registrations (the world's SoundEmitterMailbox drain), held
	// until the clock replays them at their producer ticks. Ignored while not live.
	void queue_emitters(const std::vector<world::SoundEmitterEvent> &events);

	// The world's logic tick after a session tick batch: the first one latches the
	// table onto the world's clock (an editor session that free-ran before Play
	// keeps its slot lifetimes through the offset), the queued registrations
	// replay each at its producer tick, then the table runs to the tick.
	void advance_to_logic_tick(int64_t logic_tick);

	// The render frame's mix at `listener`: the late registrations land at the
	// current tick, an owner with no world clock free-runs `delta_seconds`, the
	// live slots rank loudest first through the occlusion seam, and the pool binds
	// the loudest the device can play (`can_grow` false pins the channel count:
	// the device has no home for a new player). Expired lanes and channel-free
	// retired ids are then settled. The plan stays valid until the next frame.
	const AmbientChannelPlan &frame(const float listener[3], float delta_seconds, bool can_grow,
			const Resolve &resolve, OcclusionFn occl = nullptr, void *occl_ctx = nullptr);

	// The audio channel shutdown: every channel free (the candidates stay; the
	// next frame binds anew).
	void release_channels() { pool_.release_all(); }

	// A live candidate's wave and table; null for an id nothing holds.
	const AmbientCandidateRef *candidate(int32_t candidate_id) const;
	int channel_count() const { return pool_.channel_count(); }
	int32_t candidate_of(int channel) const { return pool_.candidate_of(channel); }
	int channel_of(int32_t candidate_id) const { return pool_.channel_of(candidate_id); }
	int failed_count() const { return pool_.failed_count(); }
	int marker_count() const { return static_cast<int>(markers_.size()); }
	const AmbientEmitterStats &stats() const { return stats_; }
	int64_t clock_tick() const { return mixer_.clock_tick(); }

private:
	struct MarkerLayer {
		int32_t candidate_id = 0;
		AmbientLayerRow row;
	};
	struct MarkerSet {
		std::string name;
		std::vector<MarkerLayer> layers;
	};
	struct Marker {
		float pos[3] = { 0.0f, 0.0f, 0.0f };
		int32_t source_bms_id = 0;
		int32_t stagger_slot = 0;
		std::array<std::string, 4> slot_sets; // the playable slots, "" silent
		std::vector<MarkerSet> sets; // distinct, in slot order
	};
	// One entity-attached (source, lane) registration while it lives: the set it
	// plays, its candidate ids (one per layer) and the tick its keep-alive ends.
	struct Lane {
		uint64_t source_spawn_id = 0;
		int32_t lane = 0;
		std::string set_name;
		std::vector<int32_t> candidate_ids;
		int64_t expires_tick = 0;
	};

	void flush_emitters(int64_t final_tick);
	int find_lane(uint64_t source_spawn_id, int32_t lane) const;
	void forget_lane(int index);
	int32_t allocate_candidate_id();
	void prune_lanes();
	void release_retired_ids();

	AmbientSetSource sets_;
	AmbientMixer mixer_;
	AmbientChannelPool pool_;
	bool live_ = false;
	float tod_hours_ = 12.0f;
	std::vector<Marker> markers_;
	std::unordered_map<int32_t, AmbientCandidateRef> candidates_;
	std::unordered_set<int32_t> validated_;
	std::vector<Lane> lanes_; // in registration order
	std::vector<world::SoundEmitterEvent> queued_;
	std::vector<world::SoundEmitterEvent> flushing_; // flush_emitters' batch, its storage kept
	int32_t next_candidate_id_ = 1;
	std::vector<int32_t> free_ids_;
	std::vector<int32_t> retired_ids_; // forgotten while a channel may still carry them
	// Latched by the first world tick: the table runs on the world's clock from then.
	bool world_driven_ = false;
	int64_t world_tick_offset_ = 0;
	AmbientEmitterStats stats_;
	std::vector<AmbientCandidate> describable_; // frame()'s rows, their storage kept
	AmbientChannelPlan plan_;
};

} // namespace opennova::audio
