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
		EVENT_FRIENDLY_TAGS_CYCLED = opennova::hud::hud_toggle_event::kFriendlyTagsCycled,
		EVENT_HELP_TOGGLED = opennova::hud::hud_toggle_event::kHelpToggled,
		EVENT_MAP_LEGEND_TOGGLED = opennova::hud::hud_toggle_event::kMapLegendToggled,
		EVENT_BRIEFING_TOGGLED = opennova::hud::hud_toggle_event::kBriefingToggled,
		EVENT_BRIEFING_PAGES_RESET = opennova::hud::hud_toggle_event::kBriefingPagesReset,
		EVENT_VERBOSE_TOGGLED = opennova::hud::hud_toggle_event::kVerboseToggled,
		EVENT_ESCAPE_CLOSED_WINDOW = opennova::hud::hud_toggle_event::kEscapeClosedWindow,
		EVENT_ESCAPE_OPEN_MENU = opennova::hud::hud_toggle_event::kEscapeOpenMenu,
		EVENT_COMMAND_MAP_OPENED = opennova::hud::hud_toggle_event::kCommandMapOpened,
		EVENT_SCOREBOARD_PAGE_RESET = opennova::hud::hud_toggle_event::kScoreboardPageReset,
	};
	// The polled catalog rows, one bit each (engine HudToggleRow).
	enum Row {
		ROW_HUD_DETAIL = opennova::hud::kRowHudDetail,
		ROW_HUD_COLOR = opennova::hud::kRowHudColor,
		ROW_SHOWHUD = opennova::hud::kRowShowHud,
		ROW_DOTSIZE = opennova::hud::kRowDotsize,
		ROW_GOALS = opennova::hud::kRowGoals,
		ROW_VIEW1ST = opennova::hud::kRowView1st,
		ROW_VIEW_WITH_GUN = opennova::hud::kRowViewWithGun,
		ROW_VIEW_CHASE = opennova::hud::kRowViewChase,
		ROW_PLAYER_LIST = opennova::hud::kRowPlayerList,
		ROW_OLD_MESSAGES = opennova::hud::kRowOldMessages,
		ROW_SHOW_SCORE = opennova::hud::kRowShowScore,
		ROW_FRIENDLY_TAGS = opennova::hud::kRowFriendlyTags,
		ROW_HELP = opennova::hud::kRowHelp,
		ROW_HELP_MAP = opennova::hud::kRowHelpMap,
		ROW_BRIEFING = opennova::hud::kRowBriefing,
		ROW_VERBOSE = opennova::hud::kRowVerbose,
		ROW_COMMANDER_MENU = opennova::hud::kRowCommanderMenu,
		ROW_COUNT = opennova::hud::kHudToggleRowCount,
	};

	// One frame's poll over the sampled rows (a mask of 1 << Row); returns the
	// Event bits.
	// `local_alive` feeds the commander_menu row's dead-player gate.
	int poll(int p_rows_down, bool p_rows_share_key, bool p_chorded, bool p_active,
			bool p_in_session, bool p_objective_game, bool p_local_alive = true);
	// The escape action's HUD-window close chain; returns the Event bits.
	int escape(bool p_in_session, bool p_spawn_gate);
	// The catalog config token behind a Row.
	static String row_token(int p_row);
	// The respawn / mission init: the overlay windows and the latches clear.
	void reset_mission();
	// The death-screen force of the live declutter level.
	void force_death_screen_hud_detail();
	// The friendly-tags cycle; returns the gametext Misc toast key for the new mode.
	String cycle_friendly_tags();
	// The Misc toast key for the current friendly-tag mode / verbose flag.
	String friendly_tag_toast_key() const;
	String verbose_toast_key() const;
	// The direct cycles (the action dispatch without a key: tests, the F3 seams).
	void cycle_hud_color();
	void cycle_hud_detail();
	void cycle_showhud();
	void toggle_objectives();

	int get_hud_color_index() const;
	void set_hud_color_index(int p_index);
	int get_hud_detail_level() const;
	void set_hud_detail_level(int p_level);
	int get_showhud_flags() const;
	void set_showhud_flags(int p_flags);
	int get_friendly_tag_mode() const;
	bool is_objectives_visible() const;
	bool is_scoreboard_open() const;
	bool is_message_log_open() const;
	bool is_end_round_stats_open() const;
	bool is_help_open() const;
	bool is_map_legend_open() const;
	int get_briefing_mode() const;
	bool is_mp_verbose() const;

	const opennova::hud::HudToggleState &state() const { return state_; }

protected:
	static void _bind_methods();

private:
	opennova::hud::HudToggleState state_;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudToggles::Event);
VARIANT_ENUM_CAST(godot::HudToggles::Row);
