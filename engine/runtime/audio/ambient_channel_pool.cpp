// The ambient channel pool — see ambient_channel_pool.h.
// [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, channel table @ 0x24D6688]

#include <runtime/audio/ambient_channel_pool.h>

namespace opennova::audio {

int32_t AmbientChannelPool::candidate_of(int channel) const {
	return channel >= 0 && channel < channel_count() ? channels_[static_cast<size_t>(channel)] : -1;
}

int AmbientChannelPool::channel_of(int32_t candidate_id) const {
	if (candidate_id < 0) return -1;
	for (size_t c = 0; c < channels_.size(); ++c)
		if (channels_[c] == candidate_id) return static_cast<int>(c);
	return -1;
}

void AmbientChannelPool::release_all() {
	for (int32_t &candidate : channels_) candidate = -1;
}

void AmbientChannelPool::reset() {
	channels_.clear();
	failed_.clear();
}

void AmbientChannelPool::plan(const std::vector<AmbientCandidate> &rows,
		const std::function<bool(int32_t)> &resolvable, bool can_grow, AmbientChannelPlan &out) {
	out = AmbientChannelPlan();
	// Resolve streams only for new candidates that would enter the top set: a
	// failed / corrupt descriptor is cached out and the next-ranked candidate
	// gets the channel ("unresolvable = absent").
	std::vector<const AmbientCandidate *> selected;
	for (const AmbientCandidate &row : rows) {
		if (static_cast<int>(selected.size()) >= budget_) break;
		if (failed_.count(row.candidate_id) != 0) continue;
		if (channel_of(row.candidate_id) < 0 && !resolvable(row.candidate_id)) {
			failed_.insert(row.candidate_id);
			continue;
		}
		selected.push_back(&row);
	}
	std::unordered_set<int32_t> selected_ids;
	for (const AmbientCandidate *row : selected) selected_ids.insert(row->candidate_id);

	// Dropouts release their physical channel. If the same virtual candidate
	// later re-enters it is rebound and starts from the beginning, like the
	// original transient channel registration.
	for (size_t c = 0; c < channels_.size(); ++c) {
		const int32_t bound = channels_[c];
		if (bound < 0 || selected_ids.count(bound) != 0) continue;
		channels_[c] = -1;
		out.released.push_back(static_cast<int>(c));
	}

	// Incumbents refresh in place; entrants take the first free channel or a
	// new one under the budget, else stay virtual this frame.
	for (const AmbientCandidate *row : selected) {
		const int incumbent = channel_of(row->candidate_id);
		if (incumbent >= 0) {
			out.updates.push_back({ incumbent, *row });
			continue;
		}
		int channel = -1;
		for (size_t c = 0; c < channels_.size(); ++c) {
			if (channels_[c] < 0) {
				channel = static_cast<int>(c);
				break;
			}
		}
		if (channel < 0) {
			if (!can_grow || static_cast<int>(channels_.size()) >= budget_) continue;
			channels_.push_back(-1);
			channel = static_cast<int>(channels_.size()) - 1;
		}
		channels_[static_cast<size_t>(channel)] = row->candidate_id;
		out.binds.push_back({ channel, *row });
	}
	out.channel_count = channel_count();
}

} // namespace opennova::audio
