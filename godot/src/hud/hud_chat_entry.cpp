#include "hud/hud_chat_entry.h"

#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <runtime/hud/feed_format.h>

using namespace godot;

namespace {

// The device leg: a Godot key into the editor's pair — the translated
// character (0 when the key has none) and the Windows virtual key the
// editor's non-character arm switches on (hud::kChatVkUp / kChatVkDown /
// kChatVkF1..).
void translate_key(const InputEventKey &event, int &key_char, int &vk) {
	key_char = 0;
	vk = 0;
	const Key code = event.get_keycode();
	switch (code) {
		case KEY_ENTER:
		case KEY_KP_ENTER: key_char = 13; return;
		case KEY_ESCAPE: key_char = 27; return;
		case KEY_BACKSPACE: key_char = 8; return;
		case KEY_TAB: key_char = 9; return;
		case KEY_UP: vk = opennova::hud::kChatVkUp; return;
		case KEY_DOWN: vk = opennova::hud::kChatVkDown; return;
		default: break;
	}
	if (code >= KEY_F1 && code < KEY_F1 + opennova::hud::kChatPresetCount) {
		vk = opennova::hud::kChatVkF1 + static_cast<int>(code - KEY_F1);
		return;
	}
	// The game font is 8-bit: Latin-1 characters type, the rest do not.
	const char32_t unicode = event.get_unicode();
	if (unicode > 0 && unicode < 256) key_char = static_cast<int>(unicode);
}

} // namespace

int HudChatEntry::poll_rows(int p_rows_down, bool p_active, bool p_chorded,
		const Ref<Simulation> &p_sim, const Ref<RtxtStringFile> &p_gametext, int64_t p_frame) {
	const uint32_t frame = static_cast<uint32_t>(p_frame);
	const opennova::hud::ChatEntryFacts facts =
			p_sim.is_valid() ? p_sim->chat_entry_facts(frame) : opennova::hud::ChatEntryFacts{};
	last_events_ = static_cast<int>(entry_.poll_rows(static_cast<uint32_t>(p_rows_down),
			p_active, p_chorded, facts, game_text_lookup(p_gametext)));
	if ((last_events_ & EVENT_DENIED_SOUND) != 0 && p_sim.is_valid())
		p_sim->raise_chat_denied_sound();
	return last_events_;
}

bool HudChatEntry::key_event(const Ref<InputEventKey> &p_event, const Ref<Simulation> &p_sim,
		const Ref<RtxtStringFile> &p_gametext, int64_t p_frame) {
	last_events_ = 0;
	if (!entry_.capturing() || p_event.is_null()) return false;
	// Releases are the editor's too: nothing else sees a key while the line
	// is open.
	if (!p_event->is_pressed()) return true;
	int key_char = 0;
	int vk = 0;
	translate_key(**p_event, key_char, vk);
	const uint32_t frame = static_cast<uint32_t>(p_frame);
	const opennova::hud::ChatEntryFacts facts =
			p_sim.is_valid() ? p_sim->chat_entry_facts(frame) : opennova::hud::ChatEntryFacts{};
	Simulation *sim = p_sim.is_valid() ? p_sim.ptr() : nullptr;
	const opennova::hud::ChatSender send = [sim, frame](int dispatch, std::string &text) {
		return sim != nullptr ? sim->send_chat_line(dispatch, text, frame)
				: opennova::hud::ChatSendResult::Refused;
	};
	last_events_ = static_cast<int>(
			entry_.key(key_char, vk, facts, game_text_lookup(p_gametext), send));
	if ((last_events_ & EVENT_DENIED_SOUND) != 0 && sim != nullptr) sim->raise_chat_denied_sound();
	return true;
}

String HudChatEntry::row_token(int p_row) {
	return String(opennova::hud::chat_talk_row_token(p_row));
}

void HudChatEntry::reset() { entry_.reset(); }
bool HudChatEntry::is_capturing() const { return entry_.capturing(); }
String HudChatEntry::get_prompt() const { return opennova::to_gd(entry_.prompt()); }
String HudChatEntry::get_text() const { return opennova::to_gd(entry_.text()); }
int HudChatEntry::get_open_dispatch() const { return entry_.open_dispatch(); }
String HudChatEntry::get_echo_text() const { return opennova::to_gd(entry_.echo_text()); }
int64_t HudChatEntry::get_echo_argb() const {
	return static_cast<int64_t>(opennova::hud::chat_dispatch_flood_color(entry_.echo_dispatch()));
}
void HudChatEntry::set_preset(int p_index, const String &p_text) {
	entry_.set_preset(p_index, opennova::to_std(p_text));
}

void HudChatEntry::_bind_methods() {
	ClassDB::bind_method(D_METHOD("poll_rows", "rows_down", "active", "chorded", "sim",
								 "gametext", "frame"),
			&HudChatEntry::poll_rows);
	ClassDB::bind_method(D_METHOD("key_event", "event", "sim", "gametext", "frame"),
			&HudChatEntry::key_event);
	ClassDB::bind_method(D_METHOD("last_events"), &HudChatEntry::last_events);
	ClassDB::bind_static_method("HudChatEntry", D_METHOD("row_token", "row"),
			&HudChatEntry::row_token);
	ClassDB::bind_method(D_METHOD("reset"), &HudChatEntry::reset);
	ClassDB::bind_method(D_METHOD("is_capturing"), &HudChatEntry::is_capturing);
	ClassDB::bind_method(D_METHOD("get_prompt"), &HudChatEntry::get_prompt);
	ClassDB::bind_method(D_METHOD("get_text"), &HudChatEntry::get_text);
	ClassDB::bind_method(D_METHOD("get_open_dispatch"), &HudChatEntry::get_open_dispatch);
	ClassDB::bind_method(D_METHOD("get_echo_text"), &HudChatEntry::get_echo_text);
	ClassDB::bind_method(D_METHOD("get_echo_argb"), &HudChatEntry::get_echo_argb);
	ClassDB::bind_method(D_METHOD("set_preset", "index", "text"), &HudChatEntry::set_preset);
	BIND_ENUM_CONSTANT(EVENT_BEGAN);
	BIND_ENUM_CONSTANT(EVENT_DENIED_SOUND);
	BIND_ENUM_CONSTANT(EVENT_SUBMITTED);
	BIND_ENUM_CONSTANT(EVENT_CANCELLED);
	BIND_ENUM_CONSTANT(EVENT_EDITED);
	BIND_ENUM_CONSTANT(EVENT_FLOOD_ECHO);
	BIND_ENUM_CONSTANT(ROW_COUNT);
}
