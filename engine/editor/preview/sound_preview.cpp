#include <editor/preview/sound_preview.h>

#include <cstdio>

#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/menu/menu_sound.h>

namespace opennova::editor {

namespace {

std::string pitch_words(uint32_t q16) {
	char text[32];
	std::snprintf(text, sizeof(text), "%.2f", double(q16) / double(lwf::kPitchUnityQ16));
	return text;
}

} // namespace

std::vector<const PreviewBank *> chain_banks(const std::vector<PreviewBank> &banks, const std::string &expansion) {
	std::vector<const PreviewBank *> out;
	for (const std::string &name : audio::global_bank_chain(expansion))
		for (const PreviewBank &bank : banks)
			if (strutil::iequals(bank.name, name)) {
				out.push_back(&bank);
				break;
			}
	return out;
}

PreviewPlay plan_set_play(const std::vector<PreviewBank> &banks, const std::string &expansion, const std::string &set,
                          const std::string &only, audio::SoundSelector &selector, uint8_t view_flags,
                          const audio::SetHearing *heard, int menu_master) {
	PreviewPlay play;
	play.set = set;
	if (set.empty()) {
		play.words = "No set is named: the game plays nothing.";
		return play;
	}
	// Where the game looks: the one bank asked, else the chain in its order.
	std::vector<const PreviewBank *> search;
	if (!only.empty()) {
		for (const PreviewBank &bank : banks)
			if (strutil::iequals(bank.name, only) || strutil::iequals(bank.path, only)) search.push_back(&bank);
		if (search.empty()) {
			play.words = "The project has no sound bank " + only + ".";
			return play;
		}
	} else {
		search = chain_banks(banks, expansion);
	}
	const PreviewBank *bank = nullptr;
	int32_t index = -1;
	for (const PreviewBank *candidate : search)
		if ((index = audio::find_bank_set(candidate->file, set)) >= 0) {
			bank = candidate;
			break;
		}
	if (!bank) {
		if (only.empty())
			play.words = "No sound bank the game searches (gamelocl.lwf, game.lwf, game3.lwf, game2.lwf, an expansion's own) "
			             "has a set named " + set + ": the game plays nothing.";
		else
			play.words = only + " has no set named " + set + ": the game plays nothing.";
		return play;
	}
	play.found = true;
	play.set = bank->file.multis[size_t(index)].name;
	play.bank = bank->name;
	play.bank_path = bank->path;
	// The selector's key is the bank's place among every bank the preview knows, stable while the
	// project's banks are, so a sequential layer steps on between plays as one bank's does in the game.
	audio::SetLocation location;
	location.bank = int32_t(bank - banks.data());
	location.set = index;
	location.cull_range = int32_t(bank->file.multis[size_t(index)].target_id);
	const audio::SetFire plan = audio::plan_set_fire(bank->file, location, selector, view_flags, heard);
	if (!plan.in_range) {
		play.in_range = false;
		play.words = play.set + " in " + play.bank + " is past its range of " +
		             std::to_string(bank->file.multis[size_t(index)].target_id) + " m from the camera: the game plays nothing.";
		return play;
	}
	std::string words;
	for (const audio::SetFireVoice &fired : plan.voices) {
		const audio::OneshotVoice &voice = fired.voice;
		const lwf::Sndparm &member = bank->file.sndparms[voice.sndparm];
		PreviewVoice out;
		out.layer = voice.layer;
		out.wave = fired.wave;
		out.file = io::utf8_file_name(fired.path);
		out.pitch_q16 = voice.pitch_q16;
		out.volume = voice.vol255;
		if (menu_master >= 0)
			out.volume = menu::menu_channel_volume(menu_master, int(member.volume), int(member.clamp_volume),
			                                       int(bank->file.playlists[voice.playlist].falloff_radius));
		words += (words.empty() ? "" : "; ") + (out.file.empty() ? out.wave + " (no file)" : out.file) + " at pitch " +
		         pitch_words(out.pitch_q16) + ", volume " + std::to_string(out.volume);
		play.voices.push_back(std::move(out));
	}
	play.words = play.set + " in " + play.bank + (words.empty() ? std::string(": no layer has a member to play.") : ": " + words + ".");
	return play;
}

PreviewPlay plan_slot_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile, int slot,
                           const std::vector<PreviewBank> &banks, const std::string &expansion,
                           audio::SoundSelector &selector, uint8_t view_flags, const audio::SetHearing *heard) {
	PreviewPlay play;
	const char *keyword = audio::sound_profile_slot_keyword(slot);
	if (!keyword) {
		play.words = "No profile slot " + std::to_string(slot) + ": the slots run 0 to 50.";
		return play;
	}
	const audio::SoundProfile *bound = audio::find_sound_profile(profiles, profile.c_str());
	if (!bound) {
		play.words = "SndProf.def has no profile: the game plays nothing.";
		return play;
	}
	const std::string &set = bound->set_names[size_t(slot)];
	if (set.empty()) {
		play.words = bound->name + "'s " + keyword + " is empty: the game plays nothing.";
		return play;
	}
	play = plan_set_play(banks, expansion, set, std::string(), selector, view_flags, heard);
	play.words = bound->name + "'s " + keyword + (play.in_range ? " plays " : ": ") + play.words;
	return play;
}

bool foot_surface_of(const std::string &word, audio::FootSurface &out) {
	if (strutil::iequals(word, "ground")) return out = audio::FootSurface::Ground, true;
	if (strutil::iequals(word, "snow")) return out = audio::FootSurface::Snow, true;
	if (strutil::iequals(word, "object")) return out = audio::FootSurface::Object, true;
	if (strutil::iequals(word, "water")) return out = audio::FootSurface::Water, true;
	return false;
}

const char *foot_surface_word(audio::FootSurface surface) {
	switch (surface) {
	case audio::FootSurface::Snow: return "snow";
	case audio::FootSurface::Object: return "object";
	case audio::FootSurface::Water: return "water";
	case audio::FootSurface::Ground: break;
	}
	return "ground";
}

PreviewPlay plan_footstep_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile,
                               audio::FootSurface surface, int foot, const std::vector<PreviewBank> &banks,
                               const std::string &expansion, audio::SoundSelector &selector) {
	return plan_slot_play(profiles, profile, audio::footstep_slot_on(surface, foot), banks, expansion, selector);
}

} // namespace opennova::editor
