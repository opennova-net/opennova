#include <editor/preview/sound_preview.h>

#include <cstdio>

#include <base/io/strutil.h>
#include <runtime/audio/bank_chain.h>
#include <runtime/audio/footstep_slot.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::editor {

namespace {

std::string file_name_of(const std::string &path) {
	const size_t slash = path.find_last_of("/\\");
	return slash == std::string::npos ? path : path.substr(slash + 1);
}

// The first set of the name in a bank, without case [orig: SoundBank_FindTriggerByName @ 0x75be90]; -1 none.
int32_t set_in(const lwf::File &bank, const std::string &name) {
	for (size_t i = 0; i < bank.multis.size(); ++i)
		if (strutil::iequals(bank.multis[i].name, name)) return int32_t(i);
	return -1;
}

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
                          const std::string &only, audio::SoundSelector &selector, uint8_t view_flags) {
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
		if ((index = set_in(candidate->file, set)) >= 0) {
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
	const audio::OneshotPlan plan = audio::plan_oneshot_at_distance(bank->file, location, 0, selector, view_flags, false);
	std::string words;
	for (const audio::OneshotVoice &voice : plan.voices) {
		const lwf::Sndparm &member = bank->file.sndparms[voice.sndparm];
		if (member.single_index >= bank->file.singles.size()) continue;
		const lwf::Single &single = bank->file.singles[member.single_index];
		PreviewVoice out;
		out.layer = voice.layer;
		out.wave = single.name;
		out.file = file_name_of(single.path);
		out.pitch_q16 = voice.pitch_q16;
		out.volume = voice.vol255;
		words += (words.empty() ? "" : "; ") + (out.file.empty() ? out.wave + " (no file)" : out.file) + " at pitch " +
		         pitch_words(out.pitch_q16) + ", volume " + std::to_string(out.volume);
		play.voices.push_back(std::move(out));
	}
	play.words = play.set + " in " + play.bank + (words.empty() ? std::string(": no layer has a member to play.") : ": " + words + ".");
	return play;
}

const audio::SoundProfile *preview_profile(const std::vector<audio::SoundProfile> &profiles, const std::string &name) {
	if (profiles.empty()) return nullptr;
	for (const audio::SoundProfile &profile : profiles)
		if (strutil::iequals(profile.name, name)) return &profile;
	return &profiles.front();
}

PreviewPlay plan_slot_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile, int slot,
                           const std::vector<PreviewBank> &banks, const std::string &expansion,
                           audio::SoundSelector &selector, uint8_t view_flags) {
	PreviewPlay play;
	const char *keyword = audio::sound_profile_slot_keyword(slot);
	if (!keyword) {
		play.words = "No profile slot " + std::to_string(slot) + ": the slots run 0 to 50.";
		return play;
	}
	const audio::SoundProfile *bound = preview_profile(profiles, profile);
	if (!bound) {
		play.words = "SndProf.def has no profile: the game plays nothing.";
		return play;
	}
	const std::string &set = bound->set_names[size_t(slot)];
	if (set.empty()) {
		play.words = bound->name + "'s " + keyword + " is empty: the game plays nothing.";
		return play;
	}
	play = plan_set_play(banks, expansion, set, std::string(), selector, view_flags);
	play.words = bound->name + "'s " + keyword + " plays " + play.words;
	return play;
}

bool foot_surface_of(const std::string &word, FootSurface &out) {
	if (strutil::iequals(word, "ground")) return out = FootSurface::Ground, true;
	if (strutil::iequals(word, "snow")) return out = FootSurface::Snow, true;
	if (strutil::iequals(word, "object")) return out = FootSurface::Object, true;
	if (strutil::iequals(word, "water")) return out = FootSurface::Water, true;
	return false;
}

const char *foot_surface_word(FootSurface surface) {
	switch (surface) {
	case FootSurface::Snow: return "snow";
	case FootSurface::Object: return "object";
	case FootSurface::Water: return "water";
	case FootSurface::Ground: break;
	}
	return "ground";
}

int footstep_slot_on(FootSurface surface, int foot) {
	// The game's own test over the state that picks the surface: feet under a water plane, a ground
	// entity, the charmap's surface 3 [orig: org2 @0x4b77c6-0x4b78a8].
	const bool water = surface == FootSurface::Water;
	return audio::footstep_slot(water ? -1 : 0, water ? 1 : 0, surface == FootSurface::Object,
	                            surface == FootSurface::Snow ? 3 : 0, foot);
}

PreviewPlay plan_footstep_play(const std::vector<audio::SoundProfile> &profiles, const std::string &profile,
                               FootSurface surface, int foot, const std::vector<PreviewBank> &banks,
                               const std::string &expansion, audio::SoundSelector &selector) {
	return plan_slot_play(profiles, profile, footstep_slot_on(surface, foot), banks, expansion, selector);
}

} // namespace opennova::editor
