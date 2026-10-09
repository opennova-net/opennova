#include <runtime/menu/menu_sound.h>

#include <base/io/strutil.h>
#include <formats/mnu/mnu.h>

namespace opennova::menu {

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
