#pragma once

// The in-game HUD's session (engine runtime/hud/hud_session.h and its role feeds,
// runtime/inmatch/hud_session_feeds.h) for the game shell's HUD presenter: the process-lifetime
// toggles and chat line, the mission effects' routing, the toggle events' device work, the
// declutter and pause words, the per-mission caches and the map queries' retry are the engine's.
// This binding runs that state's frame over the devices it is given - the world, the local
// player's presenter and the mounted HudOverlay (held by id: a freed node is no device) - and
// leaves the presenter the overlay's child controls (the SIGHTS card, the circle mask, the view
// effects, the Inset camera), which take the frame's HUD info record.

#include <godot_cpp/classes/input_event_key.hpp>
#include <godot_cpp/classes/ref.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/core/object_id.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/string.hpp>

#include <runtime/hud/hud_session.h>
#include <runtime/inmatch/hud_session_feeds.h>

#include <memory>

namespace godot {

class ControlsModel;
class FrameStats;
class GameConfigBlock;
class GameWorld;
class HudChatEntry;
class HudEntityInfo;
class HudOverlay;
class HudPos;
class HudToggles;
class LocalPlayerPresenter;
class PlayerHudWeaponDef;
class RtxtStringFile;
class Simulation;

class HudSession : public RefCounted {
	GDCLASS(HudSession, RefCounted)

protected:
	static void _bind_methods();

public:
	// The device work bits the presenter applies itself (apply_toggle_events' result).
	enum Work {
		WORK_COMMAND_MAP = 1, // the commander_menu row: open the CMAP screen
		WORK_SIGHT_STATE = 2, // the dotsize cycle stepped the sight scale: the card's rows
	};

	HudSession();

	// The boot's seed from the cfg block's persisted words (hud_color_index, hud_detail); the
	// colour cycle writes its word back into the same block.
	static Ref<HudSession> from_config(const Ref<GameConfigBlock> &p_block);

	// The toggles and the chat line, bindings over the session's own (the input router polls
	// their rows).
	Ref<HudToggles> get_toggles() const;
	Ref<HudChatEntry> get_chat() const;

	// The devices: the world (its sim, root, environment, audio), the local player's presenter
	// (the view, the FP gun bit, the view actions; may be none) and the overlay the presenter
	// built (none before the mission's overlay init).
	void set_world(GameWorld *p_world);
	void set_player_presenter(LocalPlayerPresenter *p_presenter);
	void set_overlay(HudOverlay *p_overlay);
	// The mission's loaded hudpos.def (its VEHICLE_HUD blocks for the mounted-vehicle panel).
	void set_hud_pos(const Ref<HudPos> &p_hud_pos);

	// The overlay system's init over the built overlay: the HUD's string tables registered
	// (gametext.bin, the mission's text; formats/rtxt/text_tables.h carries the witnesses), the
	// objectives header, the toggles' restamps, the status view's rule by role and the
	// grid-label origin's first query.
	void begin_mission(const String &p_mission_file);
	// The mission's teardown (`p_restart`: the SP restart's): the mission table unregistered.
	void end_mission(bool p_restart);

	// The world's drained mission effects (MissionEffect rows) routed into the session's queue,
	// the round reset and the item flashes onto the overlay at once.
	void apply_mission_effects(const Array &p_effects);

	// One frame with a local player: the overlay's whole frame state from the sim, the view and
	// the session (the clocks, the weapon slice, the tip, the key rows' events, the windows, the
	// map feeds, the rings' flush, the lanes). Returns the frame's HUD info record for the
	// overlay's child controls, null when the frame drew nothing. `p_controls` is the live
	// binding set, `p_stats` the F3 board (null: none). The result's weapon change reads through
	// is_weapon_changed / get_weapon.
	Ref<HudEntityInfo> frame(const Ref<ControlsModel> &p_controls, const Ref<FrameStats> &p_stats);
	// A dedicated host's frame: its status page, the quit dialog and the chat line over it, and
	// its HUD rows' events.
	void dedicated_host_frame();
	// The Work bits the frames since the last take named (the key rows' events).
	int take_work();
	bool is_weapon_changed() const { return weapon_changed_; }
	Ref<PlayerHudWeaponDef> get_weapon() const { return weapon_; }
	// The SIGHTS card's `slide` multiplier: the equipped weapon's at its default zero, the
	// presented view's each frame.
	int get_sight_slide_multiplier() const { return sight_slide_multiplier_; }

	// The toggle events' device work over the devices (the overlay restamps, the colour word, the
	// FP gun bit, the view actions, the toasts, the look rows, the join lock, the page resets, the
	// pause); returns the Work bits the presenter applies.
	int apply_toggle_events(int64_t p_events);
	// The escape key over live play: true when the close chain closed a window (with none open it
	// runs the respawn init and the shell opens the in-game menu).
	bool escape();
	// The SP end-of-round cine's start: its respawn init closes every window.
	void begin_end_of_round_cine();
	// One key event while the chat line is open (true: the editor took it); a flooded repeat echoes
	// into the CHAT ring in its sender's colour.
	bool chat_key(const Ref<InputEventKey> &p_key);
	bool is_chat_capturing() const;
	// The chat presets F1..F10 insert (the player profile's ten macros).
	void set_chat_presets(const PackedStringArray &p_macros);
	// The two tip options (enable_keyboardtips / enable_gameplaytips).
	void set_tip_options(bool p_keyboard_tips, bool p_gameplay_tips);
	// The shell's open-menu-screen state (the preround armory line yields to it).
	void set_menu_screen_open(bool p_open);
	// The in-game menu's share of the single-player pause word (out of a session only); its open
	// also shuts the audio channels down.
	void set_menu_pause(bool p_paused);

	// The declutter: the live level's one write seam, the mission start's re-seed, the
	// death-screen force, the render comparison's capture transaction.
	void set_hud_detail_level(int p_level);
	int get_hud_detail_level() const;
	int get_hud_detail_config() const;
	void reapply_persisted_hud_detail();
	void apply_death_screen_hud_detail();
	bool begin_hidden_capture();
	void finish_hidden_capture();
	bool is_hidden_capture_active() const;
	int get_hud_color_index() const;
	int get_friendly_tag_mode() const;

	// The read seams: the latest mission text, the end-of-round banner, the queued messages.
	String get_objective_line() const;
	String get_banner_line() const;
	int get_pending_message_count() const;
	int get_pending_chat_line_count() const;

	const opennova::hud::HudSession &native() const { return *session_; }

private:
	GameWorld *world_() const;
	LocalPlayerPresenter *presenter_() const;
	HudOverlay *overlay_() const;
	Ref<Simulation> sim_() const;
	opennova::inmatch::HudSessionRole role_(const Ref<Simulation> &p_sim) const;
	Ref<HudChatEntry> chat_ref_();
	void push_showhud_flags_();
	void restamp_detail_();
	void post_misc_toast_(const char *p_key);
	void flush_(const Ref<Simulation> &p_sim);
	void dispatch_toggle_frames_();
	void push_vehicle_panel_(const Ref<Simulation> &p_sim, int p_stance);
	void push_lanes_(const Ref<Simulation> &p_sim, const Ref<ControlsModel> &p_controls);

	std::shared_ptr<opennova::hud::HudSession> session_;
	Ref<GameConfigBlock> config_;
	Ref<HudPos> hud_pos_;
	Ref<HudChatEntry> chat_; // the binding over the session's line the overlay and router read
	ObjectID world_id_;
	ObjectID presenter_id_;
	ObjectID overlay_id_;
	bool menu_screen_open_ = false;
	bool weapon_changed_ = false;
	Ref<PlayerHudWeaponDef> weapon_;
	int sight_slide_multiplier_ = 0;
	int pending_work_ = 0;
	// The lanes' last pushes, so each clears exactly once on close.
	bool scoreboard_pushed_ = false;
	bool message_log_pushed_ = false;
	bool end_round_stats_pushed_ = false;
	bool lfp_pushed_ = false;
	bool vehicle_panel_pushed_ = false;
	bool draw_timing_armed_ = false;
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudSession::Work);
