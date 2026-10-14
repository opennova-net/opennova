#include <editor/preview/mission_listen.h>

#include <algorithm>
#include <chrono>
#include <string_view>
#include <cmath>
#include <cstdio>

#include <base/io/fixed.h>
#include <base/io/os_path.h>
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
#include <runtime/audio/envs_markers.h>
#include <runtime/audio/music_policy.h>
#include <runtime/audio/ambient_mixer.h>
#include <runtime/audio/dialog_queue.h>
#include <runtime/audio/oneshot_play.h>
#include <runtime/mission/collision_resolve.h>
#include <runtime/mission/mission_sidecars.h>
#include <runtime/world/script_voice.h>
#include <runtime/world/weather_state.h>

namespace opennova::editor {

namespace {

using io::json_number;
using io::json_string;
using io::JsonValue;

// The listener's view as the player hears the game: first person (audio::kListenerViewFirstPerson).
constexpr uint8_t kListenView = audio::kListenerViewFirstPerson;
// The hours' names, by the region the game's time-of-day test returns (audio::time_of_day_region), and an item's
// shot of each (items.def's dawnshot .. nightshot).
constexpr const char *kHours[4] = { "morning", "day", "evening", "night" };
constexpr const char *kShots[4] = { "dawnshot", "dayshot", "duskshot", "nightshot" };
constexpr size_t kFiredKept = 16;
// The scripted voice's volume at the listener, as the game's voice channel plays it: its gain byte, (210 * the
// dialogue volume + 128) >> 8 at full, through the distance curve at the local anchor's 100-unit radius, the
// distance 0 [orig: Wac_PlayScriptedVoiceWave @ 0x4ed688; Audio_Play3DPositionalSound @ 0x4ecc90..0x4ecc96;
// Audio_UpdateAmbientStream @ 0x4edb22] (world::ScriptVoiceChannel::frame's, the runtime's own).
int32_t script_voice_volume_at_listener() {
	return audio::calc_distance_volume(0, int64_t(100) * io::kFp16OneInt, (world::kScriptVoiceVolume * 255 + 128) >> 8, 255);
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
	const uint64_t stamp = view.findings.assets && !file.empty() ? view.findings.assets->stamp(io::utf8_file_name(file)) : 0;
	if (catalog.stamp == stamp && stamp != 0) return catalog;
	catalog = Catalog();
	catalog.stamp = stamp;
	std::vector<uint8_t> bytes;
	if (!view.findings.assets || !view.findings.assets->read(io::utf8_file_name(file), bytes) || bytes.empty()) return catalog;
	def::DefItemsFile items{};
	if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0) {
		// A type id resolves to its first row (mission::item_defs_by_id).
		for (const auto &entry : mission::item_defs_by_id(items)) {
			const def::DefItemDef &row = *entry.second;
			Item item;
			item.defined = true;
			item.envs = audio::envs_slot_sets(row, item.slots);
			catalog.items.emplace(int64_t(row.id), std::move(item));
		}
	}
	def::def_free_items(&items);
	return catalog;
}

bool MissionListen::refresh(const SessionView &view, const MissionScene &scene, const MissionDocument &document,
		const std::string &basename) {
	opened_ = true;
	// The mission's start and its script, booted again where what they read moved.
	script_.follow(view.findings.assets, document, scene.header(), basename);
	// The dialog bank the mission loads and the text its subtitles read (DI-32).
	files_ = view.findings.assets;
	dialog_bank_ = mission_dialog_bank(document);
	dialog_mission_base_ = mission::mission_base_name(basename_of(document.path()));
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
	const std::unordered_map<int32_t, PreviewAmbientMix::Candidate> before = mix_.candidates();
	sources_.clear();
	source_index_.clear();
	mix_.clear_candidates();
	marker_sets_.clear();
	marker_keys_.clear();
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
			// Its slots' sets a bank holds, each once (audio::envs_distinct_sets: its slots naming the same set share
			// it, the crossfade's same-set suppress), each with its layers that take a channel, and its slots keyed into
			// those (audio::envs_slot_keys), as MissionAudio registers a marker.
			std::array<std::string, 4> held;
			bool any_set = false;
			for (size_t slot = 0; slot < 4; ++slot) {
				int32_t at = -1;
				const PreviewBank *bank = PreviewAmbientMix::find_set(banks_, source.slots[slot], at);
				source.banks[slot] = bank ? bank->name : std::string();
				if (!bank) continue;
				any_set = true;
				held[slot] = source.slots[slot];
				for (const audio::EmitterLayer &layer : audio::emitter_layers(bank->file, bank->file.multis[size_t(at)])) {
					source.reach[slot] = std::max(source.reach[slot], float(layer.falloff_u));
					if (layer.min_u > 0 && (source.near[slot] <= 0.0f || float(layer.min_u) < source.near[slot]))
						source.near[slot] = float(layer.min_u);
				}
			}
			std::vector<std::string> names;
			std::vector<std::vector<audio::AmbientMixer::LayerDesc>> sets;
			for (const std::string &name : audio::envs_distinct_sets(held)) {
				std::vector<audio::AmbientMixer::LayerDesc> layers = mix_.describe(banks_, name, index, "marker", view);
				if (layers.empty()) continue;
				names.push_back(name);
				sets.push_back(std::move(layers));
			}
			const std::array<int32_t, 4> keys = audio::envs_slot_keys(held, names);
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
	// The rain's two loops, beside the listener; the sets the items loop.
	mix_.describe_rain(banks_, view);
	item_sets_.clear();
	for (const std::string &name : script_.item_loop_sets()) {
		std::vector<audio::AmbientMixer::LayerDesc> layers = mix_.describe(banks_, name, -1, "item", view);
		if (!layers.empty()) item_sets_[name] = std::move(layers);
	}
	// Other candidates than before (a source added, removed or given other sets, a wave found or lost): the channels
	// start again. An item's own candidates of its set (made as it registers) are not among them.
	size_t kept = 0;
	for (const auto &[id, candidate] : before)
		if (std::string_view(candidate.kind) != "item" || mix_.candidates().count(id)) ++kept;
	bool same = kept == mix_.candidates().size();
	for (const auto &[id, candidate] : mix_.candidates()) {
		if (!same) break;
		const auto was = before.find(id);
		same = was != before.end() && was->second.path == candidate.path && was->second.set == candidate.set &&
				was->second.layer == candidate.layer &&
				(candidate.source < 0 ? was->second.source < 0 : was->second.source >= 0);
	}
	if (!same) mix_.reset_channels();
	remix_(true);
	mix_.moved();
	return true;
}

void MissionListen::remix_(bool keep_loops) {
	// The channels stand while the candidates are the same (a source moved, its sets the same): an incumbent keeps its
	// channel, its wave playing on, once the new table's first walk registers it again.
	mix_.new_mixer();
	mixed_tick_ = -1;
	std::map<std::pair<uint64_t, int>, ItemLoop> held;
	held.swap(item_loops_);
	for (const MissionSoundSource &source : sources_) {
		if (source.marker < 0) continue;
		const float at[3] = { source.at.x, source.at.y, source.at.z };
		mix_.mixer().add_marker(at, 0, source.stagger, 0, marker_keys_[size_t(source.marker)].data(),
				marker_sets_[size_t(source.marker)]);
	}
	if (!keep_loops) return;
	// Each item's loop registered again as its entity update last registered it, in the order of their ticks, so a
	// held clock keeps it heard (the update registers it again only on a later tick), its lifetime from that tick.
	std::vector<const MissionScriptEmitter *> again;
	for (const auto &entry : held) again.push_back(&entry.second.last);
	std::stable_sort(again.begin(), again.end(),
			[](const MissionScriptEmitter *a, const MissionScriptEmitter *b) { return a->tick < b->tick; });
	for (const MissionScriptEmitter *emitter : again) register_item_(*emitter);
}

void MissionListen::register_item_(const MissionScriptEmitter &emitter) {
	audio::AmbientMixer &mixer = mix_.mixer();
	const world::SoundEmitterEvent &event = emitter.event;
	if (emitter.tick > mixer.clock_tick()) mixer.advance_to_tick(emitter.tick);
	const float at[3] = { event.pos.x, event.pos.y, event.pos.z };
	const int32_t lifetime = audio::emitter_lifetime_word(event.lifetime_ticks);
	const std::pair<uint64_t, int> key{ event.source_spawn_id, int(event.lane) };
	if (event.source_only) {
		mixer.update_emitter_source(event.source_spawn_id, at, event.source_bms_id);
		return;
	}
	if (event.pitch_q16 == 0 || event.volume_q8_8 == 0) {
		mixer.register_emitter(event.source_spawn_id, event.lane, at, event.source_bms_id, lifetime, event.pitch_q16,
				event.volume_q8_8, {});
		item_loops_.erase(key);
		return;
	}
	const auto set = item_sets_.find(event.set_name);
	if (set == item_sets_.end()) return;
	ItemLoop &loop = item_loops_[key];
	loop.last = emitter;
	if (loop.set != event.set_name) {
		if (!loop.set.empty())
			mixer.register_emitter(event.source_spawn_id, event.lane, at, event.source_bms_id, lifetime, 0, 0, {});
		loop.set = event.set_name;
		loop.layers = mix_.renumber(set->second);
	}
	mixer.register_emitter(event.source_spawn_id, event.lane, at, event.source_bms_id, lifetime, event.pitch_q16,
			event.volume_q8_8, loop.layers);
}

void MissionListen::play_to(int32_t tick, const PreviewVec3 &listener, double hours) {
	const auto start = std::chrono::steady_clock::now();
	tick = std::max(tick, 0);
	listener_ = listener;
	hours_ = hours;
	// A step back of the clock: the dialog channel starts again with the script.
	if (tick < tick_) {
		dialog_.clear();
		dialog_tick_ = 0;
	}
	tick_ = tick;
	// The script, its weather and the items' shots to the tick (they run on over the frames after a jump), the clock
	// at the picture's hour.
	script_.set_hours(hours);
	script_.run_to(tick);
	// A step back of the clock: the sources register again from the start, as at the mission's.
	if (!mix_.has_mixer() || tick < mixed_tick_) remix_(false);
	mix_.mixer().set_time_of_day_hours(float(hours));
	// The items' loops the script's entity update registered, each at its tick; then the cohorts the ticks since visit
	// [orig: Entity_UpdateAllEntities @ 0x4c225a].
	for (const MissionScriptEmitter &emitter : script_.take_emitters()) register_item_(emitter);
	mix_.mixer().advance_to_tick(tick);
	mixed_tick_ = tick;
	// The rain beside the listener while it rains, registered into the same table at the tick.
	if (const world::WeatherState *weather = script_.weather()) mix_.register_rain(tick, listener, *weather);
	// The mix at the listener, then the channels the loudest take, a source's its row.
	const std::vector<audio::AmbientCandidate> &rows =
			mix_.mix(listener, [this](int source) { return sources_[size_t(source)].row; });
	const std::unordered_map<int32_t, PreviewAmbientMix::Candidate> &candidates = mix_.candidates();
	// What each source does now.
	std::unordered_map<int32_t, int32_t> loudest; // source index -> its loudest row
	for (const audio::AmbientCandidate &row : rows) {
		const auto found = candidates.find(row.candidate_id);
		if (found == candidates.end() || found->second.source < 0) continue;
		int32_t &held = loudest[found->second.source];
		held = std::max(held, row.vol);
	}
	std::unordered_map<int32_t, int> channel_of;
	for (const MissionSoundChannel &channel : mix_.channels()) {
		if (channel.candidate < 0) continue;
		const auto found = candidates.find(channel.candidate);
		if (found != candidates.end() && found->second.source >= 0 && !channel_of.count(found->second.source))
			channel_of[found->second.source] = channel.channel;
	}
	for (size_t i = 0; i < sources_.size(); ++i) {
		MissionSoundSource &source = sources_[i];
		source.distance = distance_between(source.at, listener);
		source.volume = 0;
		source.channel = -1;
		// The hour's region (a source with no set the mixer holds reads the clock alone, unstaggered).
		const audio::TimeOfDayRegion region =
				source.marker >= 0 ? mix_.mixer().marker_region(source.marker) : audio::time_of_day_region(float(hours));
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
		uint64_t &seq) {
	std::vector<ClipSoundFired> out;
	std::vector<DialogChannel::Loaded> loaded;
	for (const MissionScriptSound &sound : script_.take_sounds()) {
		// A Play dialog: the channel run through the ticks before its own, then its dialog registered, so its tick's
		// playback, after the tick's events, sees it [orig: Game_ProcessMainFrame @ 0x5265af runs Dialog_UpdatePlayback
		// once a tick]; its lines handed over as the channel loads them (below).
		if (sound.kind == MissionScriptSound::Kind::Dialog) {
			run_dialog_to_(sound.tick, loaded);
			register_dialog_(sound.dialog);
			continue;
		}
		// A script's voice wave: at the listener (the scripted voice channel's anchor is the local player, a radio's
		// too), at the voice channel's volume there (script_voice_volume_at_listener).
		if (sound.kind == MissionScriptSound::Kind::Voice) {
			const int32_t voice_volume = script_voice_volume_at_listener();
			ClipSoundFired fired;
			fired.tick = sound.tick;
			fired.slot = -1;
			fired.seq = ++seq;
			fired.action = mission_script_sound_kind_token(sound.kind);
			fired.state = "played";
			fired.words = "Tick " + std::to_string(sound.tick) + " (the script's voice): " + sound.wave + " at volume " +
			              std::to_string(voice_volume) + ".";
			fired.voices.push_back({sound.wave, sound.wave, std::string(), 0x10000u, voice_volume});
			if (scan) find_clip_sound_waves(fired, *scan);
			fired_.push_back(fired);
			out.push_back(std::move(fired));
			continue;
		}
		audio::SetHearing heard;
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
		if (scan) find_clip_sound_waves(fired, *scan);
		fired_.push_back(fired);
		out.push_back(std::move(fired));
	}
	// The dialog channel run through the clock's tick: each line it loads its wave at its dialog volume, its subtitle in
	// its words.
	run_dialog_to_(tick_ + 1, loaded);
	const DialogSources &sources = dialog_sources_now_();
	for (const DialogChannel::Loaded &each : loaded) {
		const audio::DialogLineRef &line = each.line;
		const dbf::Group *group = sources.bank_read ? audio::find_dialog(sources.bank, line.dialog_name) : nullptr;
		const std::string text = group && line.line >= 0 && size_t(line.line) < group->lines.size()
				? audio::dialog_line_text(sources.text_read ? &sources.text : nullptr, group->lines[size_t(line.line)])
				: std::string();
		ClipSoundFired fired;
		fired.tick = each.tick;
		fired.slot = -1;
		fired.seq = ++seq;
		fired.action = "dialog";
		fired.set = line.dialog_name;
		fired.bank = dialog_bank_;
		fired.state = line.file.empty() ? "no_wave" : "played";
		fired.words = "Tick " + std::to_string(each.tick) + " (dialog " +
		              std::to_string(audio::dialog_index_of(line.dialog_name)) + ", " + line.dialog_name + " in " +
		              dialog_bank_ + ", line " + std::to_string(line.line) + "): " +
		              (line.wave.empty() ? std::string("(no wave)") : line.wave) +
		              (line.file.empty() ? std::string(", which the dialog bank's sounds lack: \"EX Cannot load audio\"")
		                                 : " (" + io::utf8_file_name(line.file) + ")") +
		              (text.empty() ? std::string() : " \"" + text + "\"");
		if (!line.file.empty()) fired.voices.push_back({line.wave, io::utf8_file_name(line.file), std::string(), 0x10000u, line.volume});
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
	const mission::Sidecar &text = *mission::sidecar_for_role("text");
	for (const std::string &name : {dialog_bank_, mission::dialog_sounds_name(dialog_bank_), mission::dialog_sounds_name(dialog_bank_, true),
	                                dialog_mission_base_ + text.extension, std::string(text.fallback)})
		stamps += name + ":" + std::to_string(files_->stamp(name)) + "|";
	if (stamps == dialog_stamps_) return dialog_sources_;
	dialog_stamps_ = stamps;
	std::string error;
	if (!read_dialog_sources(*files_, dialog_bank_, dialog_mission_base_, dialog_sources_, error)) dialog_sources_.bank_name = dialog_bank_;
	return dialog_sources_;
}

DialogWave MissionListen::dialog_wave_(const std::string &file) {
	if (!files_) return DialogWave();
	const uint64_t stamp = files_->stamp(file);
	const auto cached = dialog_waves_.find(file);
	if (cached != dialog_waves_.end() && cached->second.first == stamp) return cached->second.second;
	std::vector<uint8_t> bytes;
	const DialogWave wave = stamp && files_->read(file, bytes) ? dialog_wave(bytes) : DialogWave();
	dialog_waves_[file] = {stamp, wave};
	return wave;
}

void MissionListen::register_dialog_(int32_t number) {
	const DialogSources &sources = dialog_sources_now_();
	const std::string name = audio::dialog_name_of(number);
	const dbf::Group *group = sources.bank_read && !name.empty() ? audio::find_dialog(sources.bank, name) : nullptr;
	if (!group) return;
	dialog_.enqueue(group->group_name,
	                audio::resolve_dialog_lines(&sources.bank, sources.sounds_read ? &sources.sounds : nullptr, number));
}

void MissionListen::run_dialog_to_(int32_t end, std::vector<DialogChannel::Loaded> &out) {
	const DialogChannel::WaveOf wave_of = [this](const std::string &file) { return dialog_wave_(file); };
	for (; dialog_tick_ < end; ++dialog_tick_) dialog_.tick(dialog_tick_, wave_of, out);
}

void MissionListen::close() {
	if (opened_) mix_.moved();
	opened_ = false;
	script_.close();
	mix_.close();
	sources_.clear();
	source_index_.clear();
	marker_sets_.clear();
	marker_keys_.clear();
	resolved_.clear();
	catalogs_.clear();
	graph_read_ = false;
	followed_ = false;
	fired_.clear();
	tick_ = mixed_tick_ = -1;
	files_.reset();
	dialog_sources_ = DialogSources();
	dialog_stamps_.clear();
	dialog_waves_.clear();
	dialog_.clear();
	dialog_tick_ = 0;
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
	out.set("channels", mix_.channels_json(point_json));
	// The weather as the script has it now.
	JsonValue rain = JsonValue::make_object(), overcast = JsonValue::make_object();
	if (const world::WeatherState *weather = script_.weather()) {
		const auto percent = [](uint32_t q16) { return std::round(double(q16) * 100.0 / 65535.0); };
		rain.set("percent", json_number(percent(weather->rain_pct_current_q16())));
		rain.set("target", json_number(percent(weather->rain_pct_target_q16())));
		rain.set("kind", json_string(weather->precipitation_kind == uint32_t(world::PrecipitationKind::Snow) ? "snow" : "rain"));
		rain.set("sets_found", json_number(double(mix_.rain_sets_found())));
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
	// The dialog channel (DI-32): the mission's dialog bank, whether the project has it and its sounds, and the game's
	// queue: each slot's dialog, the next line it loads of its lines, and its timer (a line's hold, or its delay).
	JsonValue dialog = JsonValue::make_object();
	dialog.set("bank", json_string(dialog_bank_));
	dialog.set("bank_found", JsonValue::make_bool(dialog_sources_.bank_read));
	dialog.set("sounds", json_string(dialog_sources_.sounds_read ? dialog_sources_.sounds_name : std::string()));
	dialog.set("text", json_string(dialog_sources_.text_read ? dialog_sources_.text_name : std::string()));
	JsonValue slots = JsonValue::make_array();
	for (const audio::DialogQueue::Slot &slot : dialog_.queue().slots()) {
		JsonValue row = JsonValue::make_object();
		row.set("dialog", json_string(slot.dialog));
		row.set("line", json_number(double(slot.index)));
		row.set("lines", json_number(double(slot.lines.size())));
		row.set("timer", json_number(double(slot.timer & 0x7FFFFFFFu)));
		row.set("waiting", json_string((slot.timer & 0x80000000u) != 0 ? "delay" : "hold"));
		slots.push(std::move(row));
	}
	dialog.set("slots", std::move(slots));
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

void MissionListen::keep(const ClipSoundFired &fired) {
	fired_.push_back(fired);
	if (fired_.size() > kFiredKept) fired_.erase(fired_.begin(), fired_.end() - std::ptrdiff_t(kFiredKept));
}

} // namespace opennova::editor
