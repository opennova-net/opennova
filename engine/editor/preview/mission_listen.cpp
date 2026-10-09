#include <editor/preview/mission_listen.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

#include <base/io/strutil.h>
#include <base/io/tick_rate.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/mission_document.h>
#include <editor/graph/asset_graph.h>
#include <editor/project/project_files.h>
#include <editor/preview/mission_camera.h>
#include <editor/preview/mission_overlay.h>
#include <editor/preview/mission_scene.h>
#include <editor/preview/sound_preview.h>
#include <editor/session/view/session_view.h>
#include <formats/def/def.h>
#include <formats/lwf/wav_source.h>
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/music_policy.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The listener's own source in the mixer's dynamic table (the rain beside it), standing in for the local player's
// registry serial.
constexpr uint64_t kListenerSource = 1;
// The listener's view as the player hears the game: first person [the listener's view flags: 2 first person, 4 the
// external modes; audio::layer_matches_listener_view].
constexpr uint8_t kListenView = 2;
// The sets the rain's two loops play [orig: the registry rows LPNV_RAIN_L / LPNV_RAIN_R @ 0x82F590, read
// @ 0x4b47df / @ 0x4b4894].
constexpr const char *kRainSets[2] = { "LPNV_RAIN_L", "LPNV_RAIN_R" };
// The hours' names, by the region the game's time-of-day test returns (audio::time_of_day_region), and an item's
// shot of each (items.def's dawnshot .. nightshot).
constexpr const char *kHours[4] = { "morning", "day", "evening", "night" };
constexpr const char *kShots[4] = { "dawnshot", "dayshot", "duskshot", "nightshot" };
constexpr size_t kFiredKept = 16;

// A project file by its name as the asset source serves it (a path's file name).
std::string served_name(const std::string &file) {
	const size_t slash = file.find_last_of("/\\");
	return slash == std::string::npos ? file : file.substr(slash + 1);
}

std::string metres(float value) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.0f m", double(value));
	return text;
}

float distance_between(const PreviewVec3 &a, const PreviewVec3 &b) {
	const double dx = double(a.x) - b.x, dy = double(a.y) - b.y, dz = double(a.z) - b.z;
	return float(std::sqrt(dx * dx + dy * dy + dz * dz));
}

JsonValue point_json(const PreviewVec3 &at) {
	double mission[3];
	preview_to_mission(at, mission);
	JsonValue out = JsonValue::make_array();
	for (const double axis : mission) out.push(json_number(std::round(axis * 100.0) / 100.0));
	return out;
}

} // namespace

MissionListen::MissionListen() = default;
MissionListen::~MissionListen() = default;

const MissionSoundSource *MissionListen::source(NodeId row) const {
	const auto found = source_index_.find(row);
	return found == source_index_.end() ? nullptr : &sources_[found->second];
}

const MissionListen::Catalog &MissionListen::catalog_(const SessionView &view, const std::string &file) {
	Catalog &catalog = catalogs_[file];
	const uint64_t stamp = view.findings.assets && !file.empty() ? view.findings.assets->stamp(served_name(file)) : 0;
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!view.findings.assets || !view.findings.assets->read(served_name(file), bytes) || bytes.empty()) return catalog;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0) {
		for (size_t i = 0; i < items.count; ++i) {
			const def::DefItemDef &row = items.entries[i];
			// A type id resolves to its first row [docs/world/itemdef-re.md, 2026-09-23].
			if (catalog.items.count(int64_t(row.id))) continue;
			Item item;
			item.defined = true;
			item.envs = audio::envs_slot_sets(row, item.slots);
			catalog.items.emplace(int64_t(row.id), std::move(item));
		}
	}
	def::def_free_items(&items);
	return catalog;
}

const PreviewBank *MissionListen::find_set_(const std::string &name, int32_t &index) const {
	index = -1;
	if (name.empty()) return nullptr;
	// The game's search: the global chain in its order, the first bank's first set of the name [orig:
	// SoundBank_FindSetByNameAnyBank @ 0x5274f0].
	for (const PreviewBank *bank : chain_banks(banks_.banks(), banks_.expansion()))
		for (size_t i = 0; i < bank->file.multis.size(); ++i)
			if (strutil::iequals(bank->file.multis[i].name, name)) {
				index = int32_t(i);
				return bank;
			}
	return nullptr;
}

std::vector<audio::AmbientMixer::LayerDesc> MissionListen::describe_(const std::string &set, int source,
		const char *kind, const SessionView &view) {
	std::vector<audio::AmbientMixer::LayerDesc> out;
	int32_t index = -1;
	const PreviewBank *bank = find_set_(set, index);
	if (!bank) return out;
	const lwf::File &file = bank->file;
	int layer = 0;
	for (const audio::EmitterLayer &each : audio::emitter_layers(file, file.multis[size_t(index)])) {
		const lwf::Sndparm &member = file.sndparms[each.sndparm];
		const lwf::Single *single = member.single_index < file.singles.size() ? &file.singles[member.single_index] : nullptr;
		Candidate candidate;
		candidate.source = source;
		candidate.kind = kind;
		candidate.set = file.multis[size_t(index)].name;
		candidate.bank = bank->name;
		candidate.wave = single ? single->name : std::string();
		candidate.layer = layer++;
		candidate.pitch_q16 = each.pitch_q16;
		// The wave as the game loads it, by its file's name (the scan's first file of the name, a wave): a layer the
		// project has no wave for takes no channel, as the game's candidates are only the layers whose wave its root
		// holds (MissionAudio's describe_ambient).
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

bool MissionListen::refresh(const SessionView &view, const MissionScene &scene, const MissionDocument &document,
		const std::string &basename) {
	opened_ = true;
	// The mission's start and its script, booted again where what they read moved.
	script_.follow(view.findings.assets, document, scene.header(), basename);
	// The dialog bank the mission loads and the text its subtitles read (DI-32).
	files_ = view.findings.assets;
	dialog_bank_ = mission_dialog_bank(document);
	dialog_text_ = mission::mission_base_name(basename_of(document.path())) + ".bin";
	(void)dialog_sources_now_(); // read again only where a stamp moved
	const AssetGraph *graph = view.findings.graph.get();
	const uint64_t files = view.findings.assets ? view.findings.assets->generation() : 0;
	const uint64_t generation = graph ? graph->generation() : 0;
	const std::string expansion = view.project.document ? view.project.document->expansion.name : std::string();
	const bool banks_moved = view.findings.assets && banks_.refresh(*view.findings.assets, expansion, PreviewRig());
	// The game context's pair, the expansion's when the project builds as one [orig: Expansion_LoadAssets
	// @ 0x4a4798 / @ 0x4a4906..0x4a494a].
	const audio::MusicPairNames music = audio::game_music_pair_names(expansion);
	music_bank_ = music.bank_file;
	music_script_ = music.script_file;
	music_bank_found_ = view.findings.assets && view.findings.assets->stamp(music.bank_file) != 0;
	music_script_found_ = view.findings.assets && view.findings.assets->stamp(music.script_file) != 0;
	if (followed_ && !banks_moved && scene.serial() == followed_scene_ && files == followed_files_ &&
			generation == followed_graph_)
		return false;
	followed_ = true;
	followed_scene_ = scene.serial();
	followed_files_ = files;
	followed_graph_ = generation;
	if (!graph_read_ || generation != graph_generation_) {
		resolved_.clear();
		graph_generation_ = generation;
		graph_read_ = true;
	}
	const std::unordered_map<int32_t, Candidate> before = std::move(candidates_);
	sources_.clear();
	source_index_.clear();
	candidates_.clear();
	marker_sets_.clear();
	marker_keys_.clear();
	next_candidate_ = 1;
	// Every entity whose item the game updates as an env-sound emitter, in the walk resolve_envs_markers makes:
	// the markers, the items, the buildings, the people, each pool in the file's order.
	size_t resolved_count = 0;
	for (const MissionPool pool : { MissionPool::Marker, MissionPool::Item, MissionPool::Building, MissionPool::Organic }) {
		for (const MissionEntityMark &entity : scene.entities()) {
			if (entity.pool != pool) continue;
			auto found = resolved_.find(entity.item);
			if (found == resolved_.end()) {
				const GraphSymbol *symbol =
						graph ? graph->resolve_symbol(ReferenceKind::Item, std::to_string(entity.item)) : nullptr;
				found = resolved_.emplace(entity.item, symbol ? symbol->file : std::string()).first;
			}
			if (found->second.empty()) continue;
			const Catalog &catalog = catalog_(view, found->second);
			const auto item = catalog.items.find(entity.item);
			if (item == catalog.items.end() || !item->second.envs) continue;
			MissionSoundSource source;
			source.row = entity.row;
			source.item = entity.item;
			source.pool = mission_pool_token(entity.pool);
			source.slots = item->second.slots;
			source.at = entity.at;
			const int index = int(sources_.size());
			// Its slots' sets, each once (its slots naming the same set share it: the crossfade's same-set suppress
			// compares them [orig: @ 0x4a819d]), each with its layers that take a channel.
			std::vector<std::string> names;
			std::vector<std::vector<audio::AmbientMixer::LayerDesc>> sets;
			std::array<int32_t, 4> keys = { -1, -1, -1, -1 };
			bool any_set = false;
			for (size_t slot = 0; slot < 4; ++slot) {
				int32_t at = -1;
				const PreviewBank *bank = find_set_(source.slots[slot], at);
				source.banks[slot] = bank ? bank->name : std::string();
				if (!bank) continue;
				any_set = true;
				for (const audio::EmitterLayer &layer : audio::emitter_layers(bank->file, bank->file.multis[size_t(at)])) {
					source.reach[slot] = std::max(source.reach[slot], float(layer.falloff_u));
					if (layer.min_u > 0 && (source.near[slot] <= 0.0f || float(layer.min_u) < source.near[slot]))
						source.near[slot] = float(layer.min_u);
				}
				const auto named = std::find(names.begin(), names.end(), source.slots[slot]);
				if (named != names.end()) {
					keys[slot] = int32_t(named - names.begin());
					continue;
				}
				std::vector<audio::AmbientMixer::LayerDesc> layers = describe_(source.slots[slot], index, "marker", view);
				if (layers.empty()) continue;
				keys[slot] = int32_t(names.size());
				names.push_back(source.slots[slot]);
				sets.push_back(std::move(layers));
			}
			bool any_named = false;
			for (const std::string &slot : source.slots) any_named = any_named || !slot.empty();
			// Its slots name nothing (a source of timed shots alone), a set no bank holds, or waves the project lacks.
			source.status = !any_named ? "quiet" : any_set ? "no_wave" : "no_set";
			if (!sets.empty()) {
				source.stagger = audio::envs_stagger_slot(resolved_count++);
				source.marker = int(marker_sets_.size());
				source.status = "out_of_range";
			} else {
				source.stagger = -1;
			}
			if (!sets.empty()) {
				marker_sets_.push_back(std::move(sets));
				marker_keys_.push_back(keys);
			}
			source_index_[source.row] = sources_.size();
			sources_.push_back(std::move(source));
		}
	}
	// The rain's two loops, beside the listener.
	for (int side = 0; side < 2; ++side) rain_layers_[side] = describe_(kRainSets[side], -1, "rain", view);
	// Other candidates than before (a source added, removed or given other sets, a wave found or lost): the channels
	// start again.
	bool same = before.size() == candidates_.size();
	for (const auto &[id, candidate] : candidates_) {
		if (!same) break;
		const auto was = before.find(id);
		same = was != before.end() && was->second.path == candidate.path && was->second.set == candidate.set &&
				was->second.layer == candidate.layer &&
				(candidate.source < 0 ? was->second.source < 0 : was->second.source >= 0);
	}
	if (!same) {
		pool_.reset();
		channels_.clear();
	}
	remix_();
	++serial_;
	return true;
}

void MissionListen::remix_() {
	// The channels stand while the candidates are the same (a source moved, its sets the same): an incumbent keeps its
	// channel, its wave playing on, once the new table's first walk registers it again.
	mixer_ = std::make_unique<audio::AmbientMixer>();
	mixed_tick_ = -1;
	for (const MissionSoundSource &source : sources_) {
		if (source.marker < 0) continue;
		const float at[3] = { source.at.x, source.at.y, source.at.z };
		mixer_->add_marker(at, 0, source.stagger, 0, marker_keys_[size_t(source.marker)].data(),
				marker_sets_[size_t(source.marker)]);
	}
}

void MissionListen::apply_plan_(const audio::AmbientChannelPlan &plan) {
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
		const auto found = candidates_.find(bind.row.candidate_id);
		if (found != candidates_.end()) {
			const Candidate &candidate = found->second;
			channel.source = candidate.kind;
			channel.row = candidate.source >= 0 ? sources_[size_t(candidate.source)].row : 0;
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

void MissionListen::play_to(int32_t tick, const PreviewVec3 &listener, double hours) {
	const auto start = std::chrono::steady_clock::now();
	tick = std::max(tick, 0);
	listener_ = listener;
	hours_ = hours;
	// A step back of the clock: the dialog channel starts again with the script.
	if (tick < tick_) {
		dialog_due_.clear();
		dialog_free_ = 0;
	}
	tick_ = tick;
	// The script, its weather and the items' shots to the tick (they run on over the frames after a jump), the clock
	// at the picture's hour.
	script_.set_hours(hours);
	script_.run_to(tick);
	// A step back of the clock: the sources register again from the start, as at the mission's.
	if (!mixer_ || tick < mixed_tick_) remix_();
	mixer_->set_time_of_day_hours(float(hours));
	// The cohorts the ticks since visit [orig: Entity_UpdateAllEntities @ 0x4c225a].
	mixer_->advance_to_tick(tick);
	mixed_tick_ = tick;
	// The rain beside the listener while it rains, registered into the same table at the tick [orig:
	// Entity_UpdateInfantryPlayerBody @ 0x4b4747..0x4b490e].
	if (const world::WeatherState *weather = script_.weather()) {
		double eye[3];
		preview_to_mission(listener, eye);
		world::RainAmbientBody body;
		body.pos = world::Vec3{ float(eye[0]), float(eye[1]), float(eye[2]) };
		body.source_spawn_id = kListenerSource;
		body.emitted_tick = uint32_t(tick);
		world::SoundEmitterEvent rain[2];
		const size_t count = world::rain_ambient_emitters(*weather, body, rain);
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
	// The mix at the listener: the live slots culled, their volumes, the audible loudest first [orig:
	// SoundEmitter_UpdateAndMixTop8 @ 0x5284a0]; then the channels the loudest take.
	const float ear[3] = { listener.x, listener.y, listener.z };
	const std::vector<audio::AmbientCandidate> &rows = mixer_->mix(ear);
	audio::AmbientChannelPlan plan;
	pool_.plan(rows, [&](int32_t id) {
		const auto found = candidates_.find(id);
		return found != candidates_.end() && !found->second.path.empty();
	}, true, plan);
	apply_plan_(plan);
	// What each source does now.
	std::unordered_map<int32_t, int32_t> loudest; // source index -> its loudest row
	for (const audio::AmbientCandidate &row : rows) {
		const auto found = candidates_.find(row.candidate_id);
		if (found == candidates_.end() || found->second.source < 0) continue;
		int32_t &held = loudest[found->second.source];
		held = std::max(held, row.vol);
	}
	std::unordered_map<int32_t, int> channel_of;
	for (const MissionSoundChannel &channel : channels_) {
		if (channel.candidate < 0) continue;
		const auto found = candidates_.find(channel.candidate);
		if (found != candidates_.end() && found->second.source >= 0 && !channel_of.count(found->second.source))
			channel_of[found->second.source] = channel.channel;
	}
	for (size_t i = 0; i < sources_.size(); ++i) {
		MissionSoundSource &source = sources_[i];
		source.distance = distance_between(source.at, listener);
		source.volume = 0;
		source.channel = -1;
		// The hour's region (a source with no set the mixer holds reads the clock alone, unstaggered).
		const audio::TimeOfDayRegion region =
				source.marker >= 0 ? mixer_->marker_region(source.marker) : audio::time_of_day_region(float(hours));
		source.region = region.region;
		source.blend = region.blend;
		const size_t hour = size_t(region.region);
		source.set = source.slots[hour];
		source.bank = source.banks[hour];
		source.falloff = source.reach[hour];
		source.min = source.near[hour];
		if (source.marker < 0) continue;
		const int32_t key = marker_keys_[size_t(source.marker)][hour];
		const auto heard = loudest.find(int32_t(i));
		if (heard != loudest.end()) source.volume = heard->second;
		const auto held = channel_of.find(int32_t(i));
		if (held != channel_of.end()) source.channel = held->second;
		// Silent this hour: its slot names nothing, or a set no bank holds, or one whose waves the project lacks.
		const char *silent = source.slots[hour].empty() ? "quiet" : source.banks[hour].empty() ? "no_set" : "no_wave";
		source.status = source.channel >= 0 ? "playing"
				: source.volume > 0           ? "outranked"
				: key < 0                     ? silent
											  : "out_of_range";
	}
	step_us_ = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count();
}

std::vector<ClipSoundFired> MissionListen::fire_sounds(const AssetScan *scan, audio::SoundSelector &selector,
		uint64_t &seq, float volume) {
	std::vector<ClipSoundFired> out;
	const auto scaled = [&](ClipSoundFired &fired) {
		for (ClipSoundFired::Voice &voice : fired.voices)
			voice.volume = int32_t(std::lround(double(voice.volume) * double(std::clamp(volume, 0.0f, 1.0f))));
	};
	for (const MissionScriptSound &sound : script_.take_sounds()) {
		// A Play dialog: its dialog queued on the dialog channel, its lines handed over as they come due (below).
		if (sound.kind == MissionScriptSound::Kind::Dialog) {
			queue_dialog_(sound.dialog, sound.tick);
			continue;
		}
		// A script's voice wave: at the listener (the scripted voice channel's anchor is the local player, a radio's
		// too), at the voice's volume [orig: Wac_PlayScriptedVoiceWave @ 0x4ed688: (option * 0xD2 + 0x80) >> 8].
		if (sound.kind == MissionScriptSound::Kind::Voice) {
			ClipSoundFired fired;
			fired.tick = sound.tick;
			fired.slot = -1;
			fired.seq = ++seq;
			fired.action = mission_script_sound_kind_token(sound.kind);
			fired.state = "played";
			fired.words = "Tick " + std::to_string(sound.tick) + " (the script's voice): " + sound.wave + " at volume 210.";
			fired.voices.push_back({sound.wave, sound.wave, std::string(), 0x10000u, 210});
			scaled(fired);
			if (scan) find_clip_sound_waves(fired, *scan);
			fired_.push_back(fired);
			out.push_back(std::move(fired));
			continue;
		}
		PreviewHearing heard;
		std::string what;
		if (sound.kind == MissionScriptSound::Kind::Positional) {
			// A full-volume positional one-shot at its point, heard at the listener [orig: Entity_PlaySound3D_FullVolume
			// @ 0x528e20 -> Sound_Play3DPositional @ 0x527cb0].
			const PreviewVec3 at = mission_to_preview(sound.at);
			heard.source[0] = at.x;
			heard.source[1] = at.y;
			heard.source[2] = at.z;
			heard.listener[0] = listener_.x;
			heard.listener[1] = listener_.y;
			heard.listener[2] = listener_.z;
			what = sound.shot >= 0 ? std::string("an item's ") + kShots[size_t(sound.shot)] : "the script's sound at an entity";
		} else {
			// At its distance from the listener along its bearing [orig: Sound_PlayTriggerSetScaled @ 0x527b90].
			heard.at_distance = true;
			heard.distance_q16 = sound.distance_q16;
			what = sound.kind == MissionScriptSound::Kind::Thunder
					? std::string(sound.distance_q16 <= 0x10000 ? "the lightning's thunder, near" : "the lightning's thunder, far")
					: std::string("the script's sound");
		}
		ClipSoundFired fired = plan_set_heard(sound.set, sound.tick, what, banks_, heard, selector, kListenView);
		fired.seq = ++seq;
		fired.action = sound.shot >= 0 ? std::string("shot") : std::string(mission_script_sound_kind_token(sound.kind));
		scaled(fired);
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	// The dialog lines come due on the clock: each its wave at its dialog volume, its subtitle in its words.
	while (!dialog_due_.empty() && dialog_due_.front().tick <= tick_) {
		const DialogLineDue due = std::move(dialog_due_.front());
		dialog_due_.pop_front();
		ClipSoundFired fired;
		fired.tick = due.tick;
		fired.slot = -1;
		fired.seq = ++seq;
		fired.action = "dialog";
		fired.set = due.dialog;
		fired.bank = dialog_bank_;
		const DialogPlayLine &line = due.line;
		fired.state = line.file.empty() ? "no_wave" : "played";
		fired.words = "Tick " + std::to_string(due.tick) + " (dialog " + std::to_string(due.number) + ", " + due.dialog +
		              " in " + dialog_bank_ + ", line " + std::to_string(line.index) + "): " +
		              (line.wave.empty() ? std::string("(no wave)") : line.wave) +
		              (line.file.empty() ? std::string(", which the dialog bank's sounds lack: \"EX Cannot load audio\"")
		                                 : " (" + served_name(line.file) + ")") +
		              (line.text.empty() ? std::string() : " \"" + line.text + "\"");
		if (!line.file.empty()) fired.voices.push_back({line.wave, served_name(line.file), std::string(), 0x10000u, line.volume});
		scaled(fired);
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	if (fired_.size() > kFiredKept) fired_.erase(fired_.begin(), fired_.end() - std::ptrdiff_t(kFiredKept));
	return out;
}

const DialogSources &MissionListen::dialog_sources_now_() {
	if (!files_) {
		dialog_sources_ = DialogSources();
		dialog_stamps_.clear();
		return dialog_sources_;
	}
	// The stamps of what the read takes (the bank, its sounds either way, the text either way).
	std::string stamps;
	for (const std::string &name : {dialog_bank_, mission::dialog_sounds_name(dialog_bank_), mission::dialog_sounds_name(dialog_bank_, true),
	                                dialog_text_, std::string("medmssn.bin")})
		stamps += name + ":" + std::to_string(files_->stamp(name)) + "|";
	if (stamps == dialog_stamps_) return dialog_sources_;
	dialog_stamps_ = stamps;
	std::string error;
	if (!read_dialog_sources(*files_, dialog_bank_, dialog_text_, dialog_sources_, error)) dialog_sources_.bank_name = dialog_bank_;
	return dialog_sources_;
}

double MissionListen::wave_seconds_(const std::string &file) {
	if (!files_) return 0.0;
	const uint64_t stamp = files_->stamp(file);
	const auto cached = wave_seconds_cache_.find(file);
	if (cached != wave_seconds_cache_.end() && cached->second.first == stamp) return cached->second.second;
	std::vector<uint8_t> bytes;
	const double seconds = stamp && files_->read(file, bytes) ? lwf::wave_seconds(bytes) : 0.0;
	wave_seconds_cache_[file] = {stamp, seconds};
	return seconds;
}

void MissionListen::queue_dialog_(int32_t number, int32_t tick) {
	const DialogSources &sources = dialog_sources_now_();
	const DialogPlay play = plan_dialog_play(sources, audio::dialog_name_of(number), -1,
	                                         [this](const std::string &file) { return wave_seconds_(file); });
	if (!play.found) return;
	// The dialog starts once the channel frees, each line where the plan times it from there [orig: Dialog_UpdatePlayback
	// @ 0x44e470: one dialog channel at a time].
	const int32_t start = std::max(tick, dialog_free_);
	double end = 0.0;
	for (const DialogPlayLine &line : play.lines) {
		DialogLineDue due;
		due.tick = start + int32_t(std::lround(line.start_s * io::kTickHz));
		due.number = number;
		due.dialog = play.dialog;
		due.line = line;
		dialog_due_.push_back(std::move(due));
		end = std::max(end, line.start_s + line.seconds);
	}
	dialog_free_ = start + int32_t(std::lround(end * io::kTickHz));
}

void MissionListen::close() {
	if (opened_) ++serial_;
	opened_ = false;
	script_.close();
	mixer_.reset();
	pool_.reset();
	channels_.clear();
	sources_.clear();
	source_index_.clear();
	candidates_.clear();
	marker_sets_.clear();
	marker_keys_.clear();
	rain_layers_[0].clear();
	rain_layers_[1].clear();
	resolved_.clear();
	catalogs_.clear();
	graph_read_ = false;
	followed_ = false;
	fired_.clear();
	tick_ = mixed_tick_ = -1;
	files_.reset();
	dialog_sources_ = DialogSources();
	dialog_stamps_.clear();
	wave_seconds_cache_.clear();
	dialog_due_.clear();
	dialog_free_ = 0;
}

std::vector<std::string> MissionListen::source_words(NodeId row) const {
	std::vector<std::string> out;
	const MissionSoundSource *source = this->source(row);
	if (!source) return out;
	out.push_back("Ambient sound (an env-sound " + std::string(source->pool) + ", item " + std::to_string(source->item) + ")");
	for (size_t slot = 0; slot < 4; ++slot) {
		const std::string &set = source->slots[slot];
		std::string line = std::string(kHours[slot]) + ": ";
		if (set.empty()) line += "nothing";
		else if (source->banks[slot].empty()) line += set + " (no bank the game searches holds it)";
		else line += set + " (" + source->banks[slot] + ")";
		if (int(slot) == source->region && source->marker >= 0) line += " <- now";
		out.push_back(line);
	}
	const std::string status = source->status;
	if (status == "playing")
		out.push_back("Plays " + source->set + " on channel " + std::to_string(source->channel + 1) + " at volume " +
				std::to_string(source->volume) + "/255, " + metres(source->distance) + " away (heard to " +
				metres(source->falloff) + ").");
	else if (status == "outranked")
		out.push_back("Heard at volume " + std::to_string(source->volume) + "/255 but not among the " +
				std::to_string(audio::kAmbientMixChannels) + " loudest the game's channels take.");
	else if (status == "out_of_range")
		out.push_back("Silent here: " + metres(source->distance) + " away, " + source->set + " is heard to " +
				metres(source->falloff) + ".");
	else if (status == "quiet")
		out.push_back("Silent at this hour: its " + std::string(kHours[size_t(source->region)]) + " slot is empty.");
	else if (status == "no_set")
		out.push_back(source->marker < 0 ? "Silent: none of its sets is in a bank the game searches."
		                                 : "Silent at this hour: its set is in no bank the game searches.");
	else if (status == "no_wave")
		out.push_back(source->marker < 0 ? "Silent: the project lacks the waves its sets play."
		                                 : "Silent at this hour: the project lacks the waves its set plays.");
	return out;
}

io::JsonValue MissionListen::source_json(NodeId row) const {
	const MissionSoundSource *source = this->source(row);
	if (!source) return JsonValue::make_null();
	JsonValue out = JsonValue::make_object();
	JsonValue slots = JsonValue::make_array(), banks = JsonValue::make_array();
	for (size_t slot = 0; slot < 4; ++slot) {
		slots.push(json_string(source->slots[slot]));
		banks.push(json_string(source->banks[slot]));
	}
	out.set("slots", std::move(slots));
	out.set("banks", std::move(banks));
	out.set("region", json_string(kHours[size_t(std::clamp(source->region, 0, 3))]));
	out.set("blend", json_number(std::round(double(source->blend) * 1000.0) / 1000.0));
	out.set("set", json_string(source->set));
	out.set("bank", json_string(source->bank));
	out.set("falloff", json_number(double(source->falloff)));
	out.set("min", json_number(double(source->min)));
	out.set("distance", json_number(std::round(double(source->distance) * 100.0) / 100.0));
	out.set("volume", json_number(source->volume));
	out.set("channel", json_number(source->channel));
	out.set("stagger", json_number(source->stagger));
	out.set("status", json_string(source->status));
	return out;
}

io::JsonValue MissionListen::to_json(const MissionListenOptions &options) const {
	JsonValue out = JsonValue::make_object();
	out.set("on", JsonValue::make_bool(options.on));
	out.set("volume", json_number(double(options.volume)));
	out.set("hours", json_number(std::round(hours_ * 1000.0) / 1000.0));
	out.set("tick", json_number(tick_));
	out.set("listener", point_json(listener_));
	out.set("budget", json_number(audio::kAmbientMixChannels));
	// The sources by what they do.
	JsonValue sources = JsonValue::make_object();
	size_t counts[6] = {};
	constexpr const char *kStatus[6] = { "playing", "outranked", "out_of_range", "quiet", "no_set", "no_wave" };
	for (const MissionSoundSource &source : sources_)
		for (size_t i = 0; i < 6; ++i)
			if (std::string(source.status) == kStatus[i]) ++counts[i];
	sources.set("count", json_number(double(sources_.size())));
	for (size_t i = 0; i < 6; ++i) sources.set(kStatus[i], json_number(double(counts[i])));
	out.set("sources", std::move(sources));
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
		row.set("pitch", json_number(std::round(double(channel.pitch_q16) / 65536.0 * 1000.0) / 1000.0));
		row.set("at", point_json(channel.at));
		row.set("distance", json_number(std::round(double(channel.distance) * 100.0) / 100.0));
		row.set("started", json_number(double(channel.started)));
		channels.push(std::move(row));
	}
	out.set("channels", std::move(channels));
	// The weather as the script has it now.
	JsonValue rain = JsonValue::make_object(), overcast = JsonValue::make_object();
	if (const world::WeatherState *weather = script_.weather()) {
		const auto percent = [](uint32_t q16) { return std::round(double(q16) * 100.0 / 65535.0); };
		rain.set("percent", json_number(percent(weather->rain_pct_current_q16())));
		rain.set("target", json_number(percent(weather->rain_pct_target_q16())));
		rain.set("kind", json_string(weather->precipitation_kind == uint32_t(world::PrecipitationKind::Snow) ? "snow" : "rain"));
		rain.set("sets_found", json_number(double((rain_layers_[0].empty() ? 0 : 1) + (rain_layers_[1].empty() ? 0 : 1))));
		overcast.set("percent", json_number(percent(weather->overcast_blend_q16())));
		overcast.set("target", json_number(percent(weather->overcast_target_q16())));
	}
	out.set("rain", std::move(rain));
	out.set("overcast", std::move(overcast));
	JsonValue script = JsonValue::make_object();
	script.set("booted", JsonValue::make_bool(script_.booted()));
	script.set("scripted", JsonValue::make_bool(script_.scripted()));
	script.set("runs", json_number(double(script_.script_runs())));
	script.set("tick", json_number(script_.tick()));
	script.set("boots", json_number(double(script_.boots())));
	script.set("boot_us", json_number(double(script_.boot_us())));
	script.set("error", json_string(script_.error()));
	out.set("script", std::move(script));
	// The music the game plays at a mission's start, which the Listen leaves to the game: the game context opens at
	// the start [orig: Game_StartMission @ 0x525581..0x5255a4], its Var1 seed never written so 0 [orig: @ 0x5255b3],
	// so the VM plays its Multiplayerstart section: a silent lead-in, the GAMINT sting once, then silence.
	JsonValue music = JsonValue::make_object();
	music.set("bank", json_string(music_bank_));
	music.set("script", json_string(music_script_));
	music.set("bank_found", JsonValue::make_bool(music_bank_found_));
	music.set("script_found", JsonValue::make_bool(music_script_found_));
	music.set("heard", JsonValue::make_bool(false));
	music.set("words", json_string(music_bank_found_ && music_script_found_
					? "The game plays " + music_bank_ + "'s Multiplayerstart section at a mission's start: a silent "
					  "lead-in, the GAMINT sting once, then silence. Listen does not play it."
					: "The project lacks " + (music_bank_found_ ? music_script_ : music_bank_) +
							   ": the game plays no music in a mission."));
	out.set("music", std::move(music));
	// The dialog channel (DI-32): the mission's dialog bank, whether the project has it and its sounds, the lines queued
	// and not yet due, and the tick the channel frees.
	JsonValue dialog = JsonValue::make_object();
	dialog.set("bank", json_string(dialog_bank_));
	dialog.set("bank_found", JsonValue::make_bool(dialog_sources_.bank_read));
	dialog.set("sounds", json_string(dialog_sources_.sounds_read ? dialog_sources_.sounds_name : std::string()));
	dialog.set("text", json_string(dialog_sources_.text_read ? dialog_sources_.text_name : std::string()));
	JsonValue queued = JsonValue::make_array();
	for (const DialogLineDue &due : dialog_due_) {
		JsonValue row = JsonValue::make_object();
		row.set("tick", json_number(due.tick));
		row.set("dialog", json_string(due.dialog));
		row.set("line", json_number(due.line.index));
		row.set("wave", json_string(due.line.wave));
		queued.push(std::move(row));
	}
	dialog.set("queued", std::move(queued));
	dialog.set("free_tick", json_number(dialog_free_));
	out.set("dialog", std::move(dialog));
	JsonValue fired = JsonValue::make_array();
	for (const ClipSoundFired &sound : fired_) fired.push(clip_sound_fired_to_json(sound));
	out.set("sounds_fired", std::move(fired));
	out.set("step_us", json_number(double(step_us_)));
	return out;
}

void mission_listen_shapes(const MissionListen &listen, const OrbitCamera &camera, int width, int height, NodeId hovered,
		float range, OverlayList &out) {
	const PreviewVec3 eye = camera.eye();
	// The nearest first, at most a few hundred.
	std::vector<const MissionSoundSource *> drawn;
	for (const MissionSoundSource &source : listen.sources()) {
		const float reach = std::max(source.falloff, 1.0f);
		if (range > 0.0f && distance_between(source.at, eye) - reach > range && source.row != hovered) continue;
		drawn.push_back(&source);
	}
	std::sort(drawn.begin(), drawn.end(), [&](const MissionSoundSource *a, const MissionSoundSource *b) {
		return distance_between(a->at, eye) < distance_between(b->at, eye);
	});
	if (drawn.size() > 128) drawn.resize(128);
	constexpr int kSegments = 40;
	for (const MissionSoundSource *source : drawn) {
		const std::string status = source->status;
		const uint32_t rgb = status == "playing" ? kListenPlayingRgb
				: status == "outranked"          ? kListenOutrankedRgb
				: status == "no_set" || status == "no_wave" ? kListenMissingRgb
															: kListenSilentRgb;
		const bool hot = source->row == hovered;
		const bool heard = status == "playing" || status == "outranked";
		const float thickness = hot ? 2.5f : heard ? 1.5f : 1.0f;
		const uint8_t alpha = heard || hot ? 230 : 150;
		// A ring on the ground's plane through the source at each radius (the falloff's, then the proximity's).
		for (const float radius : { source->falloff, source->min }) {
			if (!(radius > 0.0f)) continue;
			PreviewVec3 previous{};
			for (int i = 0; i <= kSegments; ++i) {
				const double angle = 2.0 * 3.14159265358979323846 * double(i) / kSegments;
				const PreviewVec3 point{ float(source->at.x + radius * std::cos(angle)), source->at.y,
					float(source->at.z + radius * std::sin(angle)) };
				CanvasPoint from, to;
				if (i > 0 && mission_project_segment(camera, width, height, previous, point, from, to)) {
					// A heard source's ring over a dark one, so it reads on any ground.
					if (heard || hot) out.line(from, to, 0x000000, thickness + 2.0f, OverlayRole::Normal, 110);
					out.line(from, to, rgb, radius == source->min ? thickness * 0.6f : thickness, OverlayRole::Normal, alpha);
				}
				previous = point;
			}
		}
		float x = 0.0f, y = 0.0f;
		if (camera.project(source->at, width, height, x, y))
			out.marker(CanvasPoint{ x, y }, OverlayGlyph::Dot, hot ? 6.0f : 4.0f, OverlayRole::Normal, rgb);
	}
}

} // namespace opennova::editor
