#include <runtime/menu/menu_sound.h>

#include <base/io/strutil.h>
#include <formats/mnu/mnu.h>
#include <runtime/audio/oneshot_play.h>

namespace opennova::menu {

const lwf::File *MenuBankCollection::bank(const std::string &name, const Open &open, int32_t *key) {
	if (key) *key = 0;
	if (name.empty()) return nullptr;
	for (const Entry &entry : entries_) {
		if (!strutil::iequals(entry.name, name)) continue;
		if (key) *key = entry.key;
		return entry.opened ? &entry.file : nullptr;
	}
	entries_.emplace_back();
	Entry &entry = entries_.back();
	entry.name = name;
	entry.opened = open && open(name, entry.file);
	if (!entry.opened) {
		entry.file = lwf::File();
		return nullptr;
	}
	entry.key = next_key_++;
	if (key) *key = entry.key;
	return &entry.file;
}

void MenuBankCollection::clear() {
	entries_.clear();
	next_key_ = 1;
}

std::vector<MenuSoundVoice> plan_menu_sound(const lwf::File &bank, int32_t bank_key,
		const std::string &trigger, int master_volume, audio::SoundSelector &selector) {
	std::vector<MenuSoundVoice> out;
	const int32_t si = audio::find_bank_set(bank, trigger);
	if (si < 0) return out;
	const lwf::Multi &set = bank.multis[static_cast<size_t>(si)];
	audio::SetLocation loc;
	loc.bank = bank_key;
	loc.set = si;
	const std::vector<uint32_t> layers = audio::set_layers(bank, set);
	for (size_t li = 0; li < layers.size(); ++li) {
		const lwf::Playlist &layer = bank.playlists[layers[li]];
		const int32_t pick = audio::pick_layer_member(bank, loc, static_cast<int32_t>(li), layers[li],
				selector);
		if (pick < 0) continue;
		const std::vector<uint32_t> members = audio::layer_members(bank, layer);
		const lwf::Sndparm &member = bank.sndparms[members[static_cast<size_t>(pick)]];
		MenuSoundVoice voice;
		if (member.single_index < bank.singles.size()) voice.path = bank.singles[member.single_index].path;
		voice.volume = menu_channel_volume(master_volume, static_cast<int>(member.volume),
				static_cast<int>(member.clamp_volume), static_cast<int>(layer.falloff_radius));
		voice.pitch = menu_effective_pitch(member.pitch_scaled, set.pitch_base);
		out.push_back(std::move(voice));
	}
	return out;
}

int menu_sound_state_of(const std::string &token) {
	// [orig: CUIElement_ParseXMLDefinition @ 0x648120, the SOUND arm's STATE compares]: the parse's
	// tokens in order, MOUSEIN 1, MOUSEOUT 2, SELECTED 3.
	for (int i = 0; mnu::kSoundStates[i]; ++i)
		if (strutil::iequals(token, mnu::kSoundStates[i])) return i + 1;
	return kSoundNone;
}

const char *menu_sound_state_token(int state) {
	return state >= kSoundMouseIn && state <= kSoundSelected ? mnu::kSoundStates[state - 1] : "";
}

const mnu::Sound *menu_window_sound(const mnu::Window &window, int state) {
	if (state == kSoundNone) return nullptr;
	// Each row of the state with a TRIGGER writes the state's slot [orig: @ 0x648a68 tests the
	// state and the trigger, @ 0x648ac0 writes the slot]: the last one stands.
	const mnu::Sound *slot = nullptr;
	for (const mnu::Sound &sound : window.sounds)
		if (menu_sound_state_of(sound.state) == state && !sound.trigger.empty()) slot = &sound;
	return slot;
}

MenuSoundPump::Edge MenuSoundPump::step_(uint64_t key, Latch &latch, int to) {
	Edge edge;
	edge.key = key;
	// A state the window does not hold plays (its row, where it has one) and is held; MOUSEOUT and
	// SELECTED let go at once [orig: @ 0x647b9c..0x647c6e].
	if (to == latch.sound) return edge;
	edge.state = to;
	latch.sound = (to == kSoundMouseOut || to == kSoundSelected) ? kSoundNone : to;
	return edge;
}

MenuSoundPump::Edge MenuSoundPump::click(uint64_t key) {
	Latch &latch = latches_[key];
	// The click's arm: SELECTED, the verdict "under the mouse, button up" [orig: @ 0x647b28..0x647b32].
	const Edge edge = step_(key, latch, kSoundSelected);
	latch.verdict = 2;
	latch.clicked = true;
	return edge;
}

void MenuSoundPump::sample(bool claimed, uint64_t claim, bool live, bool button_down,
		const std::function<bool(uint64_t)> &reached, std::vector<Edge> &out) {
	const bool hit = claimed && live;
	if (hit) latches_[claim]; // the window under the mouse is one the pump has met from now on
	Edge entered;
	for (auto &[key, latch] : latches_) {
		if (!reached(key)) continue;
		if (latch.clicked) {
			// Its click this sample was its arm of the pump: nothing more of it now.
			latch.clicked = false;
			continue;
		}
		if (hit && key == claim) {
			// Under the mouse: held while the button is down, else MOUSEIN [orig: @ 0x647b3c..0x647b51].
			if (button_down) {
				latch.verdict = 3;
			} else {
				latch.verdict = 2;
				entered = step_(key, latch, kSoundMouseIn);
			}
			continue;
		}
		// Not under the mouse: MOUSEOUT where it was with the button up [orig: @ 0x647b8e..0x647b97].
		const int to = latch.verdict == 2 ? int(kSoundMouseOut) : int(latch.sound);
		latch.verdict = 0;
		const Edge left = step_(key, latch, to);
		if (left.state != kSoundNone) out.push_back(left);
	}
	if (entered.state != kSoundNone) out.push_back(entered);
}

void MenuSoundPump::reset() {
	latches_.clear();
}

int MenuSoundPump::sound_state(uint64_t key) const {
	const auto it = latches_.find(key);
	return it == latches_.end() ? int(kSoundNone) : int(it->second.sound);
}

int MenuSoundPump::verdict(uint64_t key) const {
	const auto it = latches_.find(key);
	return it == latches_.end() ? 0 : int(it->second.verdict);
}

} // namespace opennova::menu
