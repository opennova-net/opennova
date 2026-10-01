#pragma once

// The talk keys and the captured chat line (engine/runtime/hud/hud_chat_entry.h)
// as the process-lifetime object the game's HUD presenter holds: it samples
// the talk rows, routes every key event to the editor while the capture is
// open, and applies the device side effects the returned event bits name (the
// flood echo into the CHAT ring, the denied tone). The facts and the send are
// the Simulation's seams; the key translation (a Godot key event into the
// editor's translated character + virtual key) is this binding's device leg.

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/hud/hud_chat_entry.h>

namespace godot {

class RtxtStringFile;
class Simulation;

class HudChatEntry : public RefCounted {
	GDCLASS(HudChatEntry, RefCounted)

public:
	enum Event {
		EVENT_BEGAN = opennova::hud::chat_entry_event::kBegan,
		EVENT_DENIED_SOUND = opennova::hud::chat_entry_event::kDeniedSound,
		EVENT_SUBMITTED = opennova::hud::chat_entry_event::kSubmitted,
		EVENT_CANCELLED = opennova::hud::chat_entry_event::kCancelled,
		EVENT_EDITED = opennova::hud::chat_entry_event::kEdited,
		EVENT_FLOOD_ECHO = opennova::hud::chat_entry_event::kFloodEcho,
	};
	enum Row {
		ROW_COUNT = opennova::hud::kChatTalkRowCount,
	};

	// One frame's talk-row poll (a mask of 1 << row over row_token(row)'s
	// pressed state); returns the Event bits. `frame` is the per-main-frame
	// counter (the HUD tick).
	int poll_rows(int p_rows_down, bool p_active, bool p_chorded, const Ref<Simulation> &p_sim,
			const Ref<RtxtStringFile> &p_gametext, int64_t p_frame);
	// One key event while the capture is open: true = the editor took it
	// (every key does while capturing, releases included). Returns the Event
	// bits through last_events().
	bool key_event(const Ref<InputEventKey> &p_event, const Ref<Simulation> &p_sim,
			const Ref<RtxtStringFile> &p_gametext, int64_t p_frame);
	int last_events() const { return last_events_; }
	static String row_token(int p_row);
	void reset();
	bool is_capturing() const;
	String get_prompt() const;
	String get_text() const;
	int get_open_dispatch() const;
	// The last flood echo's line and its sender's packed ARGB.
	String get_echo_text() const;
	int64_t get_echo_argb() const;
	void set_preset(int p_index, const String &p_text);

	const opennova::hud::ChatEntry &entry() const { return entry_; }

protected:
	static void _bind_methods();

private:
	opennova::hud::ChatEntry entry_;
	int last_events_ = 0;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudChatEntry::Event);
VARIANT_ENUM_CAST(godot::HudChatEntry::Row);
