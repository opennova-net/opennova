#include <editor/preview/environment_listen.h>

#include <algorithm>
#include <cmath>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/sound_preview.h>
#include <editor/project/project_document.h>
#include <editor/session/view/session_view.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The listener's view as the player hears the game (the mission Listen's).
constexpr uint8_t kListenView = audio::kListenerViewFirstPerson;
constexpr size_t kFiredKept = 16;
// The thunder held for fire_sounds past this many is let go (a Shell held up a long while).
constexpr size_t kThunderKept = 16;

JsonValue point_json(const PreviewVec3 &at) {
	double mission[3];
	preview_to_mission(at, mission);
	JsonValue out = JsonValue::make_array();
	for (const double axis : mission) out.push(json_number(std::round(axis * 100.0) / 100.0));
	return out;
}

} // namespace

EnvironmentListen::EnvironmentListen() = default;
EnvironmentListen::~EnvironmentListen() = default;

void EnvironmentListen::close() {
	opened_ = false;
	mix_.close();
	mix_.moved();
	thunder_.clear();
	fired_.clear();
	tick_ = -1;
}

bool EnvironmentListen::refresh(const SessionView &view) {
	if (!view.findings.assets) {
		const bool was = opened_;
		close();
		return was;
	}
	const std::string expansion = view.project.document ? view.project.document->expansion.name : std::string();
	const bool moved = banks_.refresh(*view.findings.assets, expansion, PreviewRig()) || !opened_;
	opened_ = true;
	if (!moved) return false;
	// Other banks: the rain's layers read again, the channels started again.
	mix_.clear_candidates();
	mix_.describe_rain(banks_, view);
	mix_.reset_channels();
	mix_.new_mixer();
	tick_ = -1;
	mix_.moved();
	return true;
}

void EnvironmentListen::thunder(int32_t tick, const world::WeatherTickEvents &events) {
	if (!opened_) return;
	world::WeatherSoundEvent sounds[2];
	const size_t count = world::weather_thunder_sounds(events, sounds);
	for (size_t i = 0; i < count; ++i) thunder_.push_back(Thunder{ tick, sounds[i] });
	if (thunder_.size() > kThunderKept) thunder_.erase(thunder_.begin(), thunder_.end() - std::ptrdiff_t(kThunderKept));
}

void EnvironmentListen::play_to(int32_t tick, const PreviewVec3 &listener, const world::WeatherState &weather) {
	if (!opened_ || !mix_.has_mixer()) return;
	tick = std::max(tick, 0);
	// A step back of the clock: the table starts again.
	if (tick < tick_) mix_.new_mixer();
	tick_ = tick;
	mix_.mixer().advance_to_tick(tick);
	// The rain beside the listener while it rains, registered into the table at the tick.
	mix_.register_rain(tick, listener, weather);
	// The mix at the listener, then the channels the loudest take.
	mix_.mix(listener, nullptr);
}

std::vector<ClipSoundFired> EnvironmentListen::fire_sounds(const AssetScan *scan, audio::SoundSelector &selector,
		uint64_t &seq) {
	std::vector<ClipSoundFired> out;
	if (!opened_) {
		thunder_.clear();
		return out;
	}
	for (const Thunder &thunder : thunder_) {
		// At its distance from the listener along its bearing [orig: Sound_PlayTriggerSetScaled @ 0x527b90].
		audio::SetHearing heard;
		heard.at_distance = true;
		heard.distance_q16 = thunder.sound.distance_q16;
		const std::string what =
				thunder.sound.distance_q16 <= 0x10000 ? "the lightning's thunder, near" : "the lightning's thunder, far";
		ClipSoundFired fired = plan_set_heard(world::kThunderSoundSet, thunder.tick, what, banks_, heard, selector,
				kListenView);
		fired.seq = ++seq;
		fired.action = "thunder";
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	thunder_.clear();
	if (fired_.size() > kFiredKept) fired_.erase(fired_.begin(), fired_.end() - std::ptrdiff_t(kFiredKept));
	return out;
}

io::JsonValue EnvironmentListen::to_json(const MissionListenOptions &options) const {
	if (!opened_) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	out.set("on", JsonValue::make_bool(options.on));
	out.set("tick", json_number(tick_));
	JsonValue channels = mix_.channels_json(point_json);
	const size_t loops = channels.array.size();
	out.set("channels", std::move(channels));
	JsonValue rain = JsonValue::make_object();
	rain.set("loops", json_number(double(loops)));
	rain.set("sets_found", json_number(double(mix_.rain_sets_found())));
	out.set("rain", std::move(rain));
	JsonValue fired = JsonValue::make_array();
	for (const ClipSoundFired &sound : fired_) fired.push(clip_sound_fired_to_json(sound));
	out.set("sounds_fired", std::move(fired));
	return out;
}

} // namespace opennova::editor
