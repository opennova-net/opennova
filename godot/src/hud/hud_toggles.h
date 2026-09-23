#pragma once

// The HUD's key-driven toggles (engine/runtime/hud/hud_toggles.h) as the
// process-lifetime object the game's HUD presenter holds across mission
// rebuilds: it samples the bound keys, hands them to poll(), and applies the
// device side effects the returned event bits name (the overlay restamps,
// the config token write, the toast, the FP gun bit, the camera preference).

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/hud/hud_toggles.h>

namespace godot {

class HudToggles : public RefCounted {
	GDCLASS(HudToggles, RefCounted)

public:
	enum Event {
		EVENT_HUD_DETAIL_CYCLED = opennova::hud::hud_toggle_event::kHudDetailCycled,
		EVENT_HUD_COLOR_CYCLED = opennova::hud::hud_toggle_event::kHudColorCycled,
		EVENT_SHOWHUD_CYCLED = opennova::hud::hud_toggle_event::kShowHudCycled,
		EVENT_DOTSIZE_CYCLED = opennova::hud::hud_toggle_event::kDotsizeCycled,
		EVENT_OBJECTIVES_TOGGLED = opennova::hud::hud_toggle_event::kObjectivesToggled,
		EVENT_GUN_BIT_CHANGED = opennova::hud::hud_toggle_event::kGunBitChanged,
		EVENT_FIRST_PERSON_SELECTED = opennova::hud::hud_toggle_event::kFirstPersonSelected,
		EVENT_THIRD_PERSON_SELECTED = opennova::hud::hud_toggle_event::kThirdPersonSelected,
		EVENT_SCOREBOARD_TOGGLED = opennova::hud::hud_toggle_event::kScoreboardToggled,
		EVENT_MESSAGE_LOG_TOGGLED = opennova::hud::hud_toggle_event::kMessageLogToggled,
		EVENT_SHOW_SCORE_TOGGLED = opennova::hud::hud_toggle_event::kShowScoreToggled,
		EVENT_OVERLAY_WINDOWS_CLEARED = opennova::hud::hud_toggle_event::kOverlayWindowsCleared,
		EVENT_GUN_VIEW_SELECTED = opennova::hud::hud_toggle_event::kGunViewSelected,
	};

	// One frame's poll over the sampled key states; returns the Event bits.
	int poll(bool p_huddetail, bool p_hudcolor, bool p_rows_share_key, bool p_showhud,
			bool p_dotsize, bool p_goals, bool p_view1st, bool p_viewwithgun, bool p_viewchase,
			bool p_playerlist, bool p_old_messages, bool p_show_score, bool p_chorded,
			bool p_active, bool p_in_session);
	// The respawn / mission init: the three overlay windows and the latches clear.
	void reset_mission();
	// The death-screen force of the live declutter level.
	void force_death_screen_hud_detail();
	// The friendly-tags cycle; returns the gametext Misc toast key for the new mode.
	String cycle_friendly_tags();
	// The direct cycles (the action dispatch without a key: tests, the F3 seams).
	void cycle_hud_color();
	void cycle_hud_detail();
	void cycle_showhud();
	void toggle_objectives();
	void close_message_log();

	int get_hud_color_index() const;
	void set_hud_color_index(int p_index);
	int get_hud_detail_level() const;
	void set_hud_detail_level(int p_level);
	int get_showhud_flags() const;
	void set_showhud_flags(int p_flags);
	int get_friendly_tag_mode() const;
	bool is_objectives_visible() const;
	void set_objectives_visible(bool p_visible);
	bool is_scoreboard_open() const;
	bool is_message_log_open() const;
	bool is_end_round_stats_open() const;

	const opennova::hud::HudToggleState &state() const { return state_; }

protected:
	static void _bind_methods();

private:
	opennova::hud::HudToggleState state_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudToggles::Event);
