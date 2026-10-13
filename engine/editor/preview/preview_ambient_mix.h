#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/model/node.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/preview_clip_sounds.h>
#include <runtime/audio/ambient_channel_pool.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

struct SessionView;

// One of the channels the loudest-first mix binds (at most audio::kAmbientMixChannels [orig:
// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0, the channel table @ 0x24D6688]): what it plays (-1: free), and from what.
// `started` moves each time a candidate takes it, as a channel opened again starts its wave from its beginning
// (D-SND-6's transient channels): the device restarts its voice then.
struct MissionSoundChannel {
	int channel = 0;
	int32_t candidate = -1;
	uint64_t started = 0;
	const char *source = ""; // "marker" (an ambient source), "rain" (the rain beside the listener)
	NodeId row = 0;
	std::string set, bank, wave;
	std::string path; // the project's file of the wave ("" none: refused by the pool)
	int layer = 0;
	int32_t volume = 0;          // the mix's, 0..255
	uint32_t pitch_q16 = 0x10000; // the emitter's word (audio::emitter_play_word)
	PreviewVec3 at;
	float distance = 0.0f;
};

// The ambient mix a Listen hears (ADR 0046 DI-36, the mission view's; S23 C, the environment view's), through the
// runtime's own audio rules over the project's files: the sets' layers as candidates (each a set's layer as the
// emitter path reads it, member 0's wave, volume and clamp [orig: SoundEmitter_UpdateAndMixTop8 @ 0x528649], with the
// project's file of its wave: a layer the project has no wave for takes no channel, MissionAudio's describe), the
// runtime's AmbientMixer the sources and the rain register into, and its AmbientChannelPool binding the loudest to the
// channels the device plays, each at the emitter's pitch word alone (audio::emitter_play_word).
class PreviewAmbientMix {
public:
	// One layer that may take a channel: a source's set's layer, or a rain loop's.
	struct Candidate {
		int source = -1; // the Listen's source index (-1: the rain)
		const char *kind = "marker";
		std::string set, bank, wave, path;
		int layer = 0;
	};

	PreviewAmbientMix();
	~PreviewAmbientMix();
	PreviewAmbientMix(const PreviewAmbientMix &) = delete;
	PreviewAmbientMix &operator=(const PreviewAmbientMix &) = delete;

	// The set `name` in the game's bank order: its bank (null none) and index [orig: SoundBank_FindSetByNameAnyBank
	// @ 0x5274f0: the global chain in its order, the first bank's first set of the name].
	static const PreviewBank *find_set(const ClipSoundSources &banks, const std::string &name, int32_t &index);

	// Nothing held: no candidates, no table, the channels freed (the serial left to the caller, moved()).
	void close();
	// No candidates, ids from 1 again (the rain's layers with them).
	void clear_candidates();
	// The layers of `set` as candidates of `source` (`kind` "marker" or "rain"), ids on from the last; none for a set
	// no bank holds.
	std::vector<audio::AmbientMixer::LayerDesc> describe(const ClipSoundSources &banks, const std::string &set, int source,
			const char *kind, const SessionView &view);
	// The rain's two loops' layers (world::kRainAmbientSets), described as the rain's candidates.
	void describe_rain(const ClipSoundSources &banks, const SessionView &view);
	const std::unordered_map<int32_t, Candidate> &candidates() const { return candidates_; }
	// How many of the rain's two sets a bank holds with a wave the project has.
	int rain_sets_found() const;

	// A fresh table (its clock at the start), and the channels freed with the pool.
	void new_mixer();
	void reset_channels();
	bool has_mixer() const { return mixer_ != nullptr; }
	audio::AmbientMixer &mixer() { return *mixer_; }
	const audio::AmbientMixer &mixer() const { return *mixer_; }
	// The rain beside `listener` (the presentation frame) at `tick` while `weather` rains, registered into the table as
	// the local player's body registers it (world::rain_ambient_emitters: LPNV_RAIN_L and LPNV_RAIN_R two metres either
	// side at the rain's volume, lifetime 20) [orig: Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e].
	void register_rain(int32_t tick, const PreviewVec3 &listener, const world::WeatherState &weather);
	// The mix at `listener`: the live slots culled, their volumes, the audible loudest first [orig:
	// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]; then the channels the loudest take (the pool's plan bound), a channel's
	// `row` its source's through `row_of`. The rows, as the mix made them.
	const std::vector<audio::AmbientCandidate> &mix(const PreviewVec3 &listener, const std::function<NodeId(int)> &row_of);

	// Every channel the pool holds, free or not, by its index.
	const std::vector<MissionSoundChannel> &channels() const { return channels_; }
	// Moves whenever what plays changes (a channel bound, released, its volume moved): the device's cue.
	uint64_t serial() const { return serial_; }
	void moved() { ++serial_; }
	// The channels that play, on the wire: {channel, source, row, set, bank, layer, wave, path, volume, pitch, at,
	// distance, started}, `point` writing a place in the mission's terms.
	io::JsonValue channels_json(const std::function<io::JsonValue(const PreviewVec3 &)> &point) const;

private:
	void apply_plan_(const audio::AmbientChannelPlan &plan, const std::function<NodeId(int)> &row_of);

	std::unordered_map<int32_t, Candidate> candidates_;
	std::vector<audio::AmbientMixer::LayerDesc> rain_layers_[2];
	int32_t next_candidate_ = 1;
	std::unique_ptr<audio::AmbientMixer> mixer_;
	audio::AmbientChannelPool pool_;
	std::vector<MissionSoundChannel> channels_;
	PreviewVec3 listener_;
	uint64_t serial_ = 0;
	uint64_t started_ = 0;
};

} // namespace opennova::editor
