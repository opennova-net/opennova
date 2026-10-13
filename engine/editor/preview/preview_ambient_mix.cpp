#include <editor/preview/preview_ambient_mix.h>

#include <cmath>

#include <base/io/fixed.h>
#include <base/io/os_path.h>
#include <editor/assets/asset_registry.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/sound_preview.h>
#include <editor/session/view/session_view.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The listener's own source in the mixer's dynamic table (the rain beside it), standing in for the local player's
// registry serial.
constexpr uint64_t kListenerSource = 1;

float distance_between(const PreviewVec3 &a, const PreviewVec3 &b) {
	const double dx = double(a.x) - b.x, dy = double(a.y) - b.y, dz = double(a.z) - b.z;
	return float(std::sqrt(dx * dx + dy * dy + dz * dz));
}

} // namespace

PreviewAmbientMix::PreviewAmbientMix() = default;
PreviewAmbientMix::~PreviewAmbientMix() = default;

const PreviewBank *PreviewAmbientMix::find_set(const ClipSoundSources &banks, const std::string &name, int32_t &index) {
	index = -1;
	if (name.empty()) return nullptr;
	for (const PreviewBank *bank : chain_banks(banks.banks(), banks.expansion()))
		if ((index = audio::find_bank_set(bank->file, name)) >= 0) return bank;
	index = -1;
	return nullptr;
}

void PreviewAmbientMix::close() {
	clear_candidates();
	mixer_.reset();
	reset_channels();
}

void PreviewAmbientMix::clear_candidates() {
	candidates_.clear();
	rain_layers_[0].clear();
	rain_layers_[1].clear();
	next_candidate_ = 1;
}

std::vector<audio::AmbientMixer::LayerDesc> PreviewAmbientMix::describe(const ClipSoundSources &banks,
		const std::string &set, int source, const char *kind, const SessionView &view) {
	std::vector<audio::AmbientMixer::LayerDesc> out;
	int32_t index = -1;
	const PreviewBank *bank = find_set(banks, set, index);
	if (!bank) return out;
	const lwf::File &file = bank->file;
	const lwf::Multi &multi = file.multis[size_t(index)];
	// The wave as the game loads it, by its file's name (the scan's first file of the name, a wave): a layer the
	// project has no wave for takes no channel, as the game's candidates are only the layers whose wave its root
	// holds (audio::ambient_layer_rows, MissionAudio's describe_ambient alike).
	const auto wave_entry = [&](const std::string &wave) -> const AssetEntry * {
		const AssetEntry *entry = view.project.scan ? view.project.scan->find(io::utf8_file_name(wave)) : nullptr;
		return entry && entry->kind == AssetKind::Wave ? entry : nullptr;
	};
	// Each layer by its place among the set's emitter layers.
	const std::vector<audio::EmitterLayer> layers = audio::emitter_layers(file, multi);
	for (const audio::AmbientLayerRow &row :
			audio::ambient_layer_rows(file, multi, [&](const std::string &wave) { return wave_entry(wave) != nullptr; })) {
		const audio::EmitterLayer &each = row.layer;
		const lwf::Sndparm &member = file.sndparms[each.sndparm];
		Candidate candidate;
		candidate.source = source;
		candidate.kind = kind;
		candidate.set = multi.name;
		candidate.bank = bank->name;
		candidate.wave = file.singles[member.single_index].name;
		for (size_t i = 0; i < layers.size(); ++i)
			if (layers[i].sndparm == each.sndparm && layers[i].playlist == each.playlist) candidate.layer = int(i);
		candidate.path = wave_entry(row.wave)->relative_path;
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

void PreviewAmbientMix::describe_rain(const ClipSoundSources &banks, const SessionView &view) {
	for (int side = 0; side < 2; ++side) rain_layers_[side] = describe(banks, world::kRainAmbientSets[side], -1, "rain", view);
}

int PreviewAmbientMix::rain_sets_found() const {
	return (rain_layers_[0].empty() ? 0 : 1) + (rain_layers_[1].empty() ? 0 : 1);
}

void PreviewAmbientMix::new_mixer() {
	mixer_ = std::make_unique<audio::AmbientMixer>();
}

void PreviewAmbientMix::reset_channels() {
	pool_.reset();
	channels_.clear();
}

void PreviewAmbientMix::register_rain(int32_t tick, const PreviewVec3 &listener, const world::WeatherState &weather) {
	if (!mixer_) return;
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
}

const std::vector<audio::AmbientCandidate> &PreviewAmbientMix::mix(const PreviewVec3 &listener,
		const std::function<NodeId(int)> &row_of) {
	listener_ = listener;
	const float ear[3] = { listener.x, listener.y, listener.z };
	const std::vector<audio::AmbientCandidate> &rows = mixer_->mix(ear);
	audio::AmbientChannelPlan plan;
	pool_.plan(rows, [&](int32_t id) {
		const auto found = candidates_.find(id);
		return found != candidates_.end() && !found->second.path.empty();
	}, true, plan);
	apply_plan_(plan, row_of);
	return rows;
}

void PreviewAmbientMix::apply_plan_(const audio::AmbientChannelPlan &plan, const std::function<NodeId(int)> &row_of) {
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
		// The emitter's word alone, as the game's channel plays it (audio::emitter_play_word; D-SND-56).
		const uint32_t pitch = audio::emitter_play_word(row.pitch_q16);
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
		const auto found = candidates_.find(bind.row.candidate_id);
		if (found != candidates_.end()) {
			const Candidate &candidate = found->second;
			channel.source = candidate.kind;
			channel.row = candidate.source >= 0 && row_of ? row_of(candidate.source) : 0;
			channel.set = candidate.set;
			channel.bank = candidate.bank;
			channel.wave = candidate.wave;
			channel.path = candidate.path;
			channel.layer = candidate.layer;
		}
		fill(channel, bind.row);
		++serial_;
	}
	for (const audio::AmbientChannelPlan::Bind &update : plan.updates) fill(channels_[size_t(update.channel)], update.row);
}

io::JsonValue PreviewAmbientMix::channels_json(const std::function<io::JsonValue(const PreviewVec3 &)> &point) const {
	JsonValue channels = JsonValue::make_array();
	for (const MissionSoundChannel &channel : channels_) {
		if (channel.candidate < 0) continue;
		JsonValue row = JsonValue::make_object();
		row.set("channel", json_number(channel.channel));
		row.set("source", json_string(channel.source));
		row.set("row", json_number(double(channel.row)));
		row.set("set", json_string(channel.set));
		row.set("bank", json_string(channel.bank));
		row.set("layer", json_number(channel.layer));
		row.set("wave", json_string(channel.wave));
		row.set("path", json_string(channel.path));
		row.set("volume", json_number(channel.volume));
		row.set("pitch", json_number(std::round(double(channel.pitch_q16) / io::kFp16OneD * 1000.0) / 1000.0));
		row.set("at", point(channel.at));
		row.set("distance", json_number(std::round(double(channel.distance) * 100.0) / 100.0));
		row.set("started", json_number(double(channel.started)));
		channels.push(std::move(row));
	}
	return channels;
}

} // namespace opennova::editor
