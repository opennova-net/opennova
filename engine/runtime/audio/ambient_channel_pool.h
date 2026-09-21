#pragma once

// THE AMBIENT CHANNEL POOL: the eight physical voices the loudest-first mix
// rows bind to. Retail sorts every in-range emitter voice by computed volume
// each frame and keeps the loudest 8 on real channels [orig:
// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, channel table @ 0x24D6688]; a
// selected incumbent keeps its channel while its rank moves, a dropout
// releases its channel, an entrant takes a free channel (a new one under the
// budget) and starts from the beginning like the original transient
// registration, and a candidate whose stream cannot be resolved is cached out
// so the next-ranked row takes the channel. The embedder resolves streams and
// drives the players; this decides.

#include <runtime/audio/ambient_mixer.h>

#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

namespace opennova::audio {

inline constexpr int kAmbientMixChannels = 8;

struct AmbientChannelPlan {
	struct Bind {
		int channel = -1;
		AmbientCandidate row;
	};
	std::vector<int> released; // channels whose voice drops this frame
	std::vector<Bind> binds; // fresh starts; a channel at or past the previous count is new
	std::vector<Bind> updates; // incumbents: position / volume / pitch refresh
	int channel_count = 0; // channels after this plan (the embedder grows its players to it)
};

class AmbientChannelPool {
public:
	explicit AmbientChannelPool(int budget = kAmbientMixChannels) : budget_(budget) {}

	// `rows` ranked loudest-first (AmbientMixer::mix_rows), already narrowed to
	// the candidates the embedder can describe. `resolvable` is asked once per
	// entrant that is not bound yet; a false answer is remembered for the
	// candidate's life. `can_grow` false pins the channel count (the players'
	// root is not attached).
	void plan(const std::vector<AmbientCandidate> &rows,
			const std::function<bool(int32_t)> &resolvable, bool can_grow, AmbientChannelPlan &out);

	int channel_count() const { return static_cast<int>(channels_.size()); }
	int32_t candidate_of(int channel) const;
	int channel_of(int32_t candidate_id) const;
	bool has_failed(int32_t candidate_id) const { return failed_.count(candidate_id) != 0; }
	int failed_count() const { return static_cast<int>(failed_.size()); }
	// Every channel free; the failure cache and the channel count stay.
	void release_all();
	// A new marker set: its candidate ids start over, so nothing stays failed.
	void forget_failures() { failed_.clear(); }
	// A retired dynamic candidate: its id may be reissued.
	void forget_candidate(int32_t candidate_id) { failed_.erase(candidate_id); }
	// Back to cold: no channels, nothing failed.
	void reset();

private:
	int budget_;
	std::vector<int32_t> channels_; // the candidate per channel, -1 = free
	std::unordered_set<int32_t> failed_;
};

} // namespace opennova::audio
