#include <editor/preview/preview_clip_sounds.h>

#include <algorithm>

#include <base/io/strutil.h>
#include <editor/assets/project_asset_source.h>
#include <formats/def/def.h>
#include <formats/lwf/lwf.h>
#include <runtime/anim/anim_event_bits.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/world/infantry_sound.h>

namespace opennova::editor {

namespace {

using io::JsonValue;
using io::json_number;
using io::json_string;

std::string file_of(const std::string &path) { return path.substr(path.find_last_of("/\\") + 1); }

// What one sound of an event is in words: "left footstep", "right footstep", "sound 5".
std::string sound_words(const world::AnimEventSound &sound) {
	if (sound.foot == 0) return "left footstep";
	if (sound.foot == 1) return "right footstep";
	return "sound " + std::to_string(sound.slot - audio::kSlotAudio1 + 1);
}

std::string slot_keyword(int slot) {
	if (slot < 0) return std::string(); // a set played by name (DI-10's death sound), no profile's slot
	const char *keyword = audio::sound_profile_slot_keyword(slot);
	return keyword ? keyword : "slot " + std::to_string(slot);
}

// The bank of the game's search the set is found in, without case (the first bank's first set of the
// name [orig: SoundBank_FindSetByNameAnyBank @ 0x5274f0]); null for none.
const PreviewBank *bank_holding(const ClipSoundSources &sources, const std::string &set) {
	for (const PreviewBank *bank : chain_banks(sources.banks(), sources.expansion()))
		if (audio::find_bank_set(bank->file, set) >= 0) return bank;
	return nullptr;
}

} // namespace

const char *clip_sound_body_token(ClipSoundBody body) {
	switch (body) {
	case ClipSoundBody::Npc: return "npc";
	case ClipSoundBody::Player: return "player";
	case ClipSoundBody::Auto: break;
	}
	return "auto";
}

io::JsonValue clip_sound_options_to_json(const ClipSoundOptions &options) {
	JsonValue out = JsonValue::make_object();
	out.set("mute", JsonValue::make_bool(options.mute));
	out.set("surface", json_string(foot_surface_word(options.surface)));
	out.set("body", json_string(clip_sound_body_token(options.body)));
	out.set("female", JsonValue::make_bool(options.female));
	out.set("profile", json_string(options.profile));
	return out;
}

bool read_clip_sound_options(const io::JsonValue &json, ClipSoundOptions &held, std::string &error) {
	if (!json.is_object()) {
		error = "options.sound is an object {mute, surface, body, female, profile}.";
		return false;
	}
	ClipSoundOptions options = held;
	for (const io::JsonMember &member : json.object) {
		const std::string &key = member.key;
		const JsonValue &value = member.value;
		if (key == "mute" || key == "female") {
			if (!value.is_bool()) {
				error = "options.sound." + key + " is true or false.";
				return false;
			}
			(key == "mute" ? options.mute : options.female) = value.boolean;
		} else if (key == "surface") {
			if (!value.is_string() || !foot_surface_of(value.string, options.surface)) {
				error = "options.sound.surface is ground, snow, object or water.";
				return false;
			}
		} else if (key == "body") {
			if (value.is_string() && value.string == "auto") options.body = ClipSoundBody::Auto;
			else if (value.is_string() && value.string == "npc") options.body = ClipSoundBody::Npc;
			else if (value.is_string() && value.string == "player") options.body = ClipSoundBody::Player;
			else {
				error = "options.sound.body is auto (the paired item's), npc or player.";
				return false;
			}
		} else if (key == "profile") {
			if (!value.is_string()) {
				error = "options.sound.profile is a SndProf.def profile's name (\"\" the paired item's).";
				return false;
			}
			options.profile = value.string;
		} else {
			error = "Unknown sound option \"" + key + "\" (it takes mute, surface, body, female, profile).";
			return false;
		}
	}
	held = options;
	return true;
}

// --- ClipSoundSources ------------------------------------------------------------------------------

bool ClipSoundSources::refresh(const ProjectAssetSource &files, const std::string &expansion, const PreviewRig &rig) {
	bool moved = false;
	std::vector<Stamp> now;
	now.push_back({"SndProf.def", files.stamp("SndProf.def")});
	for (const std::string &name : audio::global_bank_chain(expansion)) now.push_back({name, files.stamp(name)});
	const bool same = now.size() == stamps_.size() &&
	                  std::equal(now.begin(), now.end(), stamps_.begin(), [](const Stamp &a, const Stamp &b) {
		                  return a.name == b.name && a.stamp == b.stamp;
	                  });
	if (!same || expansion != expansion_ || stamps_.empty()) {
		moved = true;
		stamps_ = now;
		expansion_ = expansion;
		std::vector<uint8_t> bytes;
		// SndProf.def through the game's walk [orig: SoundProfile_LoadAll @ 0x527490].
		profiles_.clear();
		profile_file_ = files.read("SndProf.def", bytes) && !bytes.empty();
		if (profile_file_) {
			audio::SoundProfileTable table;
			table.parse(reinterpret_cast<const char *>(bytes.data()), bytes.size());
			profiles_ = table.entries();
		}
		// The banks of the search, each as the game opens it by its name.
		banks_.clear();
		for (size_t i = 1; i < stamps_.size(); ++i) {
			bytes.clear();
			if (!files.read(stamps_[i].name, bytes) || bytes.empty()) continue;
			PreviewBank bank;
			bank.name = stamps_[i].name;
			bank.path = files.path_of(stamps_[i].name);
			std::string error;
			if (lwf::parse_lwf_buffer(bytes.data(), bytes.size(), bank.file, error)) banks_.push_back(std::move(bank));
		}
	}
	// The item pairing the clip: an item's anim_def (a weapon's animadm names no profile).
	const bool item_pairs = !rig.record_file.empty() && rig.record_field == "anim_def";
	const std::string catalog = item_pairs ? file_of(rig.record_file) : std::string();
	const uint64_t stamp = item_pairs ? files.stamp(catalog) : 0;
	if (catalog != item_file_ || rig.record != item_record_ || stamp != item_stamp_) {
		moved = true;
		item_file_ = catalog;
		item_record_ = rig.record;
		item_stamp_ = stamp;
		item_ = ClipSoundItem();
		std::vector<uint8_t> bytes;
		if (item_pairs && files.read(catalog, bytes) && !bytes.empty()) {
			def::DefItemsFile items{};
			if (def::def_parse_items_memory(bytes.data(), bytes.size(), &items) == 0)
				for (size_t i = 0; i < items.count; ++i) {
					const def::DefItemDef &def = items.entries[i];
					if (!strutil::iequals(def.display_name, rig.record)) continue;
					item_.found = true;
					item_.name = def.display_name;
					item_.sound_profile = def.sound_profile;
					item_.sound_profile_female = def.sound_profile_female;
					item_.move_function = def.move_function;
					item_.ai_function = def.ai_function;
					const char *ammo[] = {def.ammo_closeattack, def.ammo_easyrocket, def.ammo_advancedrocket,
					                      def.ammo_marker3};
					const char *launch[] = {def.launchups_closeattack, def.launchups_rocket, def.launchups_marker3};
					for (size_t slot = 0; slot < 4; ++slot) item_.ammo[slot] = ammo[slot];
					for (size_t slot = 0; slot < 3; ++slot) item_.launch[slot] = launch[slot];
					break;
				}
			def::def_free_items(&items);
		}
	}
	return moved;
}

// --- the binding -------------------------------------------------------------------------------------

ClipSoundBinding clip_sound_binding(const std::vector<audio::SoundProfile> &profiles, const ClipSoundOptions &options,
                                    const ClipSoundItem &item, const PreviewRig &rig) {
	ClipSoundBinding binding;
	// The body: the class table's callback for the item's move_function, or the one chosen.
	if (options.body == ClipSoundBody::Player) {
		binding.player = true;
		binding.body_words = "A player's body (org2), chosen: it reads the clip's events on even ticks.";
	} else if (options.body == ClipSoundBody::Npc) {
		binding.body_words = "An NPC's body (org1), chosen: it reads the clip's events on odd ticks.";
	} else if (!item.found) {
		binding.body_words = std::string(rig.source == "chosen" ? "The model was chosen, so no item says the body"
		                                                        : "No item pairs the clip, so none says the body") +
		                     ": an NPC's (org1), which reads the clip's events on odd ticks.";
	} else if (strutil::iequals(item.move_function, "org2")) {
		binding.player = true;
		binding.body_words = item.name + " runs move_function org2, a player's body: it reads the clip's events on even ticks.";
	} else if (strutil::iequals(item.move_function, "org1")) {
		binding.body_words = item.name + " runs move_function org1, an NPC's body: it reads the clip's events on odd ticks.";
	} else {
		binding.body_words = item.name + " runs move_function " +
		                     (item.move_function.empty() ? std::string("none") : item.move_function) +
		                     ", no person's body: the preview plays it as an NPC's (org1), on odd ticks.";
	}
	if (profiles.empty()) {
		binding.profile_words = "SndProf.def holds no profile: nothing plays.";
		return binding;
	}
	const auto bound = [&](const std::string &name) { return audio::item_sound_profile(profiles, name.c_str())->name; };
	if (!options.profile.empty()) {
		binding.profile = bound(options.profile);
		binding.profile_words = strutil::iequals(binding.profile, options.profile)
		                                ? binding.profile + ", picked."
		                                : "No profile is named " + options.profile + ": the game takes the first, " +
		                                          binding.profile + ".";
		return binding;
	}
	if (!item.found) {
		binding.profile = bound(std::string());
		binding.profile_words = std::string(rig.source == "chosen" ? "The model was chosen" : "No item pairs the clip") +
		                        ": it plays " + binding.profile +
		                        "'s slots, as an item naming no sound_profile binds default.";
		return binding;
	}
	// A female avatar's profile is a player's alone (the character entity's byte).
	const bool female = options.female && binding.player;
	const std::string &authored = female ? item.sound_profile_female : item.sound_profile;
	const char *field = female ? "sound_profileFemale" : "sound_profile";
	binding.profile = bound(authored);
	if (authored.empty())
		binding.profile_words = item.name + " names no " + field + ": it binds " + binding.profile + ".";
	else if (strutil::iequals(binding.profile, authored))
		binding.profile_words = binding.profile + ", " + item.name + "'s " + field + ".";
	else
		binding.profile_words = item.name + "'s " + field + " " + authored +
		                        " names no profile of SndProf.def: the game binds its first, " + binding.profile + ".";
	if (options.female && !binding.player)
		binding.profile_words += " (A female avatar's profile is a player's: an NPC's body plays the sound_profile.)";
	return binding;
}

// --- the schedule --------------------------------------------------------------------------------------

std::vector<ClipEventDue> clip_events_due(const anim::ClipTimeline &clock, const ClipSoundTrack &track, int32_t period,
                                          int32_t from, int32_t to, bool player_body, bool catch_up) {
	std::vector<ClipEventDue> out;
	if (track.triggers.empty() || to <= from) return out;
	if (catch_up && to - from > kClipSoundCatchUpTicks) from = to - kClipSoundCatchUpTicks;
	for (int32_t tick = from + 1; tick <= to; ++tick) {
		if (!world::anim_sound_tick(uint32_t(tick), player_body)) continue;
		const int32_t clip_tick = period > 0 ? tick % period : tick;
		if (clip_tick <= 0) continue;
		const uint32_t word = anim::clip_trigger_at(clock, track.triggers, clip_tick);
		if (word == 0) continue;
		ClipEventDue due;
		due.tick = tick;
		due.clip_tick = clip_tick;
		due.frame = clock.frame_index_at(clip_tick);
		due.word = word;
		// The frame's capsule bottom, lerped between its records as the channel samples it.
		if (track.bottoms.size() >= 2) {
			const double position = clock.frame_at(clip_tick);
			const size_t frame = std::min(size_t(position), track.bottoms.size() - 2);
			const double fraction = std::clamp(position - double(frame), 0.0, 1.0);
			due.bottom = float(track.bottoms[frame] * (1.0 - fraction) + track.bottoms[frame + 1] * fraction);
		}
		out.push_back(due);
	}
	return out;
}

// --- what fires -------------------------------------------------------------------------------------------

io::JsonValue clip_sound_fired_to_json(const ClipSoundFired &fired) {
	JsonValue row = JsonValue::make_object();
	row.set("seq", json_number(double(fired.seq)));
	row.set("tick", json_number(fired.tick));
	row.set("frame", json_number(fired.frame));
	row.set("bits", json_number(double(fired.bits)));
	row.set("slot", json_number(fired.slot));
	row.set("keyword", json_string(slot_keyword(fired.slot)));
	row.set("foot", json_string(fired.foot == 0 ? "left" : fired.foot == 1 ? "right" : ""));
	row.set("action", json_string(fired.action));
	row.set("leg", json_string(fired.leg));
	row.set("profile", json_string(fired.profile));
	row.set("set", json_string(fired.set));
	row.set("bank", json_string(fired.bank));
	row.set("state", json_string(fired.state));
	row.set("words", json_string(fired.words));
	JsonValue voices = JsonValue::make_array();
	for (const ClipSoundFired::Voice &voice : fired.voices) {
		JsonValue one = JsonValue::make_object();
		one.set("wave", json_string(voice.wave));
		one.set("path", json_string(voice.path.empty() ? voice.file : voice.path));
		one.set("pitch", json_number(double(voice.pitch_q16) / double(lwf::kPitchUnityQ16)));
		one.set("volume", json_number(voice.volume));
		voices.push(std::move(one));
	}
	row.set("voices", std::move(voices));
	return row;
}

std::vector<ClipSoundFired> plan_clip_event(const ClipEventDue &due, const ClipSoundOptions &options,
                                            const ClipSoundBinding &binding, const ClipSoundSources &sources,
                                            const PreviewVec3 &listener, audio::SoundSelector &selector) {
	std::vector<ClipSoundFired> out;
	const audio::FootState under = audio::foot_state_on(options.surface);
	world::AnimEventSound sounds[world::kAnimEventSoundMax];
	const int count = world::anim_event_sounds(due.word, under.feet_z, under.water_z, under.on_entity,
	                                           under.surface_type, sounds);
	const audio::SoundProfile *profile =
			binding.profile.empty() ? nullptr : audio::find_sound_profile(sources.profiles(), binding.profile.c_str());
	for (int i = 0; i < count; ++i) {
		ClipSoundFired fired;
		fired.tick = due.tick;
		fired.frame = due.frame;
		fired.bits = due.word;
		fired.slot = sounds[i].slot;
		fired.foot = sounds[i].foot;
		const std::string what = "Frame " + std::to_string(due.frame) + " (" + sound_words(sounds[i]) + "): ";
		if (!profile) {
			fired.state = "no_profile";
			fired.words = what + binding.profile_words;
			out.push_back(std::move(fired));
			continue;
		}
		fired.profile = profile->name;
		fired.set = profile->set_names[size_t(fired.slot)];
		// A foley sound plays at the body's origin, a footstep at its feet, the frame's capsule bottom
		// below it [orig: the dip @0x4b77d3]; the preview's body stands at the origin, y up.
		audio::SetHearing heard;
		heard.source[1] = sounds[i].foot < 0 ? 0.0f : -due.bottom;
		heard.listener[0] = listener.x;
		heard.listener[1] = listener.y;
		heard.listener[2] = listener.z;
		const PreviewPlay play = plan_slot_play(sources.profiles(), profile->name, fired.slot, sources.banks(),
		                                        sources.expansion(), selector, kClipSoundListenerView, &heard);
		fired.bank = play.bank;
		fired.words = what + play.words;
		fired.state = fired.set.empty()     ? "empty"
		              : !play.found         ? "missing"
		              : !play.in_range      ? "out_of_range"
		              : play.voices.empty() ? "silent"
		              : options.mute        ? "muted"
		                                    : "played";
		for (const PreviewVoice &voice : play.voices)
			fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
		out.push_back(std::move(fired));
	}
	return out;
}

void find_clip_sound_waves(ClipSoundFired &fired, const AssetScan &scan) {
	std::string lacks;
	size_t found = 0;
	for (ClipSoundFired::Voice &voice : fired.voices) {
		const AssetEntry *entry = voice.file.empty() ? nullptr : scan.find(voice.file);
		if (entry && entry->kind == AssetKind::Wave) {
			voice.path = entry->relative_path;
			++found;
		} else {
			lacks += (lacks.empty() ? "" : ", ") + (voice.file.empty() ? voice.wave : voice.file);
		}
	}
	if (lacks.empty()) return;
	fired.words += " The project lacks " + lacks + ".";
	if (found == 0 && (fired.state == "played" || fired.state == "muted")) fired.state = "no_wave";
}

ClipSoundFired plan_set_heard(const std::string &set, int32_t tick, const std::string &what,
                              const ClipSoundSources &sources, const audio::SetHearing &heard,
                              audio::SoundSelector &selector, uint8_t view_flags) {
	ClipSoundFired fired;
	fired.tick = tick;
	fired.slot = -1;
	fired.set = set;
	const PreviewPlay play =
			plan_set_play(sources.banks(), sources.expansion(), set, std::string(), selector, view_flags, &heard);
	fired.bank = play.bank;
	fired.words = "Tick " + std::to_string(tick) + " (" + what + "): " + play.words;
	fired.state = !play.found ? "missing" : !play.in_range ? "out_of_range" : play.voices.empty() ? "silent" : "played";
	for (const PreviewVoice &voice : play.voices)
		fired.voices.push_back({voice.wave, voice.file, std::string(), voice.pitch_q16, voice.volume});
	return fired;
}

ClipSoundFired plan_set_at_origin(const std::string &set, int32_t tick, const std::string &what,
                                  const ClipSoundSources &sources, const PreviewVec3 &listener,
                                  audio::SoundSelector &selector) {
	// Played at the item, the preview's origin, as the camera hears it.
	audio::SetHearing heard;
	heard.listener[0] = listener.x;
	heard.listener[1] = listener.y;
	heard.listener[2] = listener.z;
	return plan_set_heard(set, tick, what, sources, heard, selector, kClipSoundListenerView);
}

std::vector<std::string> clip_event_sound_words(uint32_t word, const ClipSoundOptions &options,
                                                const ClipSoundBinding &binding, const ClipSoundSources &sources) {
	std::vector<std::string> out;
	const audio::FootState under = audio::foot_state_on(options.surface);
	world::AnimEventSound sounds[world::kAnimEventSoundMax];
	const int count = world::anim_event_sounds(word, under.feet_z, under.water_z, under.on_entity,
	                                           under.surface_type, sounds);
	const audio::SoundProfile *profile =
			binding.profile.empty() ? nullptr : audio::find_sound_profile(sources.profiles(), binding.profile.c_str());
	for (int i = 0; i < count; ++i) {
		const std::string keyword = slot_keyword(sounds[i].slot);
		const std::string what = sound_words(sounds[i]) + " plays ";
		if (!profile) {
			out.push_back(what + "nothing: " + binding.profile_words);
			continue;
		}
		const std::string &set = profile->set_names[size_t(sounds[i].slot)];
		if (set.empty()) {
			out.push_back(what + "nothing: " + profile->name + "'s " + keyword + " is empty.");
			continue;
		}
		const PreviewBank *bank = bank_holding(sources, set);
		out.push_back(bank ? what + keyword + ": " + set + " (" + bank->name + ")"
		                   : what + "nothing: " + keyword + "'s " + set + " is in no bank the game searches.");
	}
	return out;
}

} // namespace opennova::editor
