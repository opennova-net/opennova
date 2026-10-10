#include <editor/preview/environment_listen.h>

#include <algorithm>
#include <cmath>

#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/sound_preview.h>
#include <editor/project/project_document.h>
#include <editor/session/view/session_view.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The listener's own source in the mixer's dynamic table (the rain beside it), standing in for the local player's
// registry serial; the listener's view as the player hears the game (the mission Listen's).
constexpr uint64_t kListenerSource = 1;
constexpr uint8_t kListenView = audio::kListenerViewFirstPerson;
constexpr size_t kFiredKept = 16;
// The thunder held for fire_sounds past this many is let go (a Shell held up a long while).
constexpr size_t kThunderKept = 16;

std::string served_name(const std::string &file) {
	const size_t slash = file.find_last_of("/\\");
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

float distance_between(const PreviewVec3 &a, const PreviewVec3 &b) {
	const double dx = double(a.x) - b.x, dy = double(a.y) - b.y, dz = double(a.z) - b.z;
	return float(std::sqrt(dx * dx + dy * dy + dz * dz));
}

} // namespace

EnvironmentListen::EnvironmentListen() = default;
EnvironmentListen::~EnvironmentListen() = default;

void EnvironmentListen::close() {
	opened_ = false;
	candidates_.clear();
	rain_layers_[0].clear();
	rain_layers_[1].clear();
	mixer_.reset();
	pool_.reset();
	channels_.clear();
	thunder_.clear();
	fired_.clear();
	tick_ = -1;
	++serial_;
}

std::vector<audio::AmbientMixer::LayerDesc> EnvironmentListen::describe_(const std::string &set,
		const SessionView &view) {
	std::vector<audio::AmbientMixer::LayerDesc> out;
	// The game's search: the global chain in its order, the first bank's first set of the name [orig:
	// SoundBank_FindSetByNameAnyBank @ 0x5274f0].
	const PreviewBank *bank = nullptr;
	int32_t index = -1;
	for (const PreviewBank *each : chain_banks(banks_.banks(), banks_.expansion()))
		if ((index = audio::find_bank_set(each->file, set)) >= 0) {
			bank = each;
			break;
		}
	if (!bank) return out;
	const lwf::File &file = bank->file;
	int layer = 0;
	for (const audio::EmitterLayer &each : audio::emitter_layers(file, file.multis[size_t(index)])) {
		const lwf::Sndparm &member = file.sndparms[each.sndparm];
		const lwf::Single *single = member.single_index < file.singles.size() ? &file.singles[member.single_index] : nullptr;
		Candidate candidate;
		candidate.set = file.multis[size_t(index)].name;
		candidate.bank = bank->name;
		candidate.wave = single ? single->name : std::string();
		candidate.layer = layer++;
		candidate.pitch_q16 = each.pitch_q16;
		// A layer the project has no wave for takes no channel (the mission Listen's rule, MissionAudio's describe).
		const AssetEntry *entry = single && view.project.scan ? view.project.scan->find(served_name(single->path)) : nullptr;
		if (!entry || entry->kind != AssetKind::Wave) continue;
		candidate.path = entry->relative_path;
		audio::AmbientMixer::LayerDesc desc;
		desc.candidate_id = next_candidate_++;
		desc.falloff_u = each.falloff_u;
		desc.min_u = each.min_u;
		desc.member_vol = each.volume;
		desc.clamp_vol = each.clamp;
		candidates_[desc.candidate_id] = std::move(candidate);
		out.push_back(desc);
	}
	return out;
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
	candidates_.clear();
	for (int side = 0; side < 2; ++side) rain_layers_[side] = describe_(world::kRainAmbientSets[side], view);
	pool_.reset();
	channels_.clear();
	mixer_ = std::make_unique<audio::AmbientMixer>();
	tick_ = -1;
	++serial_;
	return true;
}

void EnvironmentListen::thunder(int32_t tick, const world::WeatherTickEvents &events) {
	if (!opened_) return;
	world::WeatherSoundEvent sounds[2];
	const size_t count = world::weather_thunder_sounds(events, sounds);
	for (size_t i = 0; i < count; ++i) thunder_.push_back(Thunder{ tick, sounds[i] });
	if (thunder_.size() > kThunderKept) thunder_.erase(thunder_.begin(), thunder_.end() - std::ptrdiff_t(kThunderKept));
}

void EnvironmentListen::apply_plan_(const audio::AmbientChannelPlan &plan) {
	while (int(channels_.size()) < plan.channel_count) {
		MissionSoundChannel channel;
		channel.channel = int(channels_.size());
		channels_.push_back(channel);
	}
	for (const int released : plan.released) {
		MissionSoundChannel free_channel;
		free_channel.channel = released;
		channels_[size_t(released)] = free_channel;
		++serial_;
	}
	const auto fill = [&](MissionSoundChannel &channel, const audio::AmbientCandidate &row) {
		const PreviewVec3 at{ row.pos[0], row.pos[1], row.pos[2] };
		const auto found = candidates_.find(row.candidate_id);
		const uint32_t member = found != candidates_.end() ? found->second.pitch_q16 : 0x10000u;
		const uint32_t pitch = uint32_t((uint64_t(member) * uint32_t(row.pitch_q16)) >> 16);
		if (channel.volume != row.vol || channel.pitch_q16 != pitch || channel.at.x != at.x || channel.at.y != at.y ||
				channel.at.z != at.z)
			++serial_;
		channel.volume = row.vol;
		channel.pitch_q16 = pitch;
		channel.at = at;
		channel.distance = distance_between(at, listener_);
	};
	for (const audio::AmbientChannelPlan::Bind &bind : plan.binds) {
		MissionSoundChannel &channel = channels_[size_t(bind.channel)];
		channel = MissionSoundChannel();
		channel.channel = bind.channel;
		channel.candidate = bind.row.candidate_id;
		channel.started = ++started_;
		channel.source = "rain";
		const auto found = candidates_.find(bind.row.candidate_id);
		if (found != candidates_.end()) {
			channel.set = found->second.set;
			channel.bank = found->second.bank;
			channel.wave = found->second.wave;
			channel.path = found->second.path;
			channel.layer = found->second.layer;
		}
		fill(channel, bind.row);
		++serial_;
	}
	for (const audio::AmbientChannelPlan::Bind &update : plan.updates) fill(channels_[size_t(update.channel)], update.row);
}

void EnvironmentListen::play_to(int32_t tick, const PreviewVec3 &listener, const world::WeatherState &weather) {
	if (!opened_ || !mixer_) return;
	tick = std::max(tick, 0);
	listener_ = listener;
	// A step back of the clock: the table starts again.
	if (tick < tick_) mixer_ = std::make_unique<audio::AmbientMixer>();
	tick_ = tick;
	mixer_->advance_to_tick(tick);
	// The rain beside the listener while it rains, registered into the table at the tick [orig:
	// Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e].
	double eye[3];
	preview_to_mission(listener, eye);
	world::RainAmbientBody body;
	body.pos = world::Vec3{ float(eye[0]), float(eye[1]), float(eye[2]) };
	body.source_spawn_id = kListenerSource;
	body.emitted_tick = uint32_t(tick);
	world::SoundEmitterEvent rain[2];
	const size_t count = world::rain_ambient_emitters(weather, body, rain);
	for (size_t i = 0; i < count; ++i) {
		const int side = rain[i].lane == 1 ? 0 : 1;
		if (rain_layers_[side].empty()) continue;
		const double at[3] = { rain[i].pos.x, rain[i].pos.y, rain[i].pos.z };
		const PreviewVec3 placed = mission_to_preview(at);
		const float pos[3] = { placed.x, placed.y, placed.z };
		mixer_->register_emitter(kListenerSource, rain[i].lane, pos, 0, rain[i].lifetime_ticks, rain[i].pitch_q16,
				rain[i].volume_q8_8, rain_layers_[side]);
	}
	// The mix at the listener, then the channels the loudest take [orig: SoundEmitter_UpdateAndMixTop8 @ 0x5284a0].
	const float ear[3] = { listener.x, listener.y, listener.z };
	const std::vector<audio::AmbientCandidate> &rows = mixer_->mix(ear);
	audio::AmbientChannelPlan plan;
	pool_.plan(rows, [&](int32_t id) {
		const auto found = candidates_.find(id);
		return found != candidates_.end() && !found->second.path.empty();
	}, true, plan);
	apply_plan_(plan);
}

std::vector<ClipSoundFired> EnvironmentListen::fire_sounds(const AssetScan *scan, audio::SoundSelector &selector,
		uint64_t &seq, float volume) {
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
		for (ClipSoundFired::Voice &voice : fired.voices)
			voice.volume = int32_t(std::lround(double(voice.volume) * double(std::clamp(volume, 0.0f, 1.0f))));
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
	out.set("volume", json_number(options.volume));
	out.set("tick", json_number(tick_));
	JsonValue channels = JsonValue::make_array();
	size_t loops = 0;
	for (const MissionSoundChannel &channel : channels_) {
		if (channel.candidate < 0) continue;
		++loops;
		JsonValue row = JsonValue::make_object();
		row.set("channel", json_number(channel.channel));
		row.set("set", json_string(channel.set));
		row.set("bank", json_string(channel.bank));
		row.set("wave", json_string(channel.wave));
		row.set("path", json_string(channel.path));
		row.set("volume", json_number(channel.volume));
		row.set("pitch", json_number(std::round(double(channel.pitch_q16) / 65536.0 * 1000.0) / 1000.0));
		row.set("distance", json_number(std::round(double(channel.distance) * 100.0) / 100.0));
		channels.push(std::move(row));
	}
	out.set("channels", std::move(channels));
	JsonValue rain = JsonValue::make_object();
	rain.set("loops", json_number(double(loops)));
	rain.set("sets_found", json_number(double((rain_layers_[0].empty() ? 0 : 1) + (rain_layers_[1].empty() ? 0 : 1))));
	out.set("rain", std::move(rain));
	JsonValue fired = JsonValue::make_array();
	for (const ClipSoundFired &sound : fired_) fired.push(clip_sound_fired_to_json(sound));
	out.set("sounds_fired", std::move(fired));
	return out;
}

} // namespace opennova::editor
