#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/json.h>
#include <editor/preview/mission_listen.h>
#include <editor/preview/mission_options.h>
#include <editor/preview/model_preview_camera.h>
#include <editor/preview/preview_clip_sounds.h>
#include <runtime/audio/ambient_channel_pool.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/sound_selector.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

struct AssetScan;
struct SessionView;

// The weather heard where an environment viewport's camera stands (ADR 0046 S23 C; DI-19b held it out): the rain
// and the thunder of the viewport's own weather home, as the mission view's Listen hears a mission's (DI-36), through
// the runtime's own audio rules over the project's files:
// - While it rains, the rain's two loops register beside the listener as the local player's body registers them
//   (world::rain_ambient_emitters: LPNV_RAIN_L and LPNV_RAIN_R two metres either side at the rain's volume, lifetime
//   20) into the runtime's AmbientMixer, whose mix the AmbientChannelPool binds to channels the device plays [orig:
//   Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e; SoundEmitter_UpdateAndMixTop8 @ 0x5284a0].
// - Each tick's lightning raises its thunder (world::weather_thunder_sounds: A at a metre, B ten metres behind), the
//   THUNDER set found in the game's bank order and planned at its distance [orig: Sound_PlayTriggerSetScaled
//   @ 0x527b90], a one-shot the Shell's clip voices play.
// The listener is the camera's eye, standing in for the player's ears.
class EnvironmentListen {
public:
	EnvironmentListen();
	~EnvironmentListen();
	EnvironmentListen(const EnvironmentListen &) = delete;
	EnvironmentListen &operator=(const EnvironmentListen &) = delete;

	// The game's banks (the global chain over the project's expansion) and the rain's sets' layers, read again where a
	// file moved; true when what it plays from moved.
	bool refresh(const SessionView &view);
	// A tick's thunder, queued for fire_sounds.
	void thunder(int32_t tick, const world::WeatherTickEvents &events);
	// The clock's tick reached at `listener` (the camera's eye, the presentation frame) over `weather`: the rain's
	// loops registered beside it while it rains, the mix and the channels made again.
	void play_to(int32_t tick, const PreviewVec3 &listener, const world::WeatherState &weather);
	// The thunder heard since the last call, each planned at its distance with its member picked through `selector`
	// and its voices' volume scaled by `volume`, numbered from `seq`; kept for the wire (the last 16).
	std::vector<ClipSoundFired> fire_sounds(const AssetScan *scan, audio::SoundSelector &selector, uint64_t &seq,
			float volume);
	// Closed: nothing plays, the channels free, the mix forgotten.
	void close();
	bool open() const { return opened_; }

	const std::vector<MissionSoundChannel> &channels() const { return channels_; }
	const std::vector<ClipSoundFired> &sounds_fired() const { return fired_; }
	// Moves whenever what plays changes (a channel bound, released, its volume moved): the device's cue.
	uint64_t serial() const { return serial_; }
	// The body's `listen`: {on, volume, tick, channels [{channel, set, bank, wave, path, volume, pitch, distance}],
	// rain {loops, sets_found}, thunder (the queued), sounds_fired}; null while closed.
	io::JsonValue to_json(const MissionListenOptions &options) const;

private:
	struct Candidate {
		std::string set, bank, wave, path;
		int layer = 0;
		uint32_t pitch_q16 = 0x10000;
	};
	std::vector<audio::AmbientMixer::LayerDesc> describe_(const std::string &set, const SessionView &view);
	void apply_plan_(const audio::AmbientChannelPlan &plan);

	bool opened_ = false;
	ClipSoundSources banks_;
	std::unordered_map<int32_t, Candidate> candidates_;
	std::vector<audio::AmbientMixer::LayerDesc> rain_layers_[2];
	int32_t next_candidate_ = 1;
	std::unique_ptr<audio::AmbientMixer> mixer_;
	audio::AmbientChannelPool pool_;
	std::vector<MissionSoundChannel> channels_;
	struct Thunder {
		int32_t tick = 0;
		world::WeatherSoundEvent sound;
	};
	std::vector<Thunder> thunder_;
	std::vector<ClipSoundFired> fired_;
	PreviewVec3 listener_;
	int32_t tick_ = -1;
	uint64_t serial_ = 0;
	uint64_t started_ = 0;
};

} // namespace opennova::editor
