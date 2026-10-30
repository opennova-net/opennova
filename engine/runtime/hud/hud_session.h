#pragma once

// THE HUD SESSION: the in-game HUD's process state and the rules the embedder's HUD presenter
// ran over it each frame - the key-driven toggles (hud_toggles.h) and the captured chat line
// (hud_chat_entry.h), both process globals in retail that survive the per-mission overlay
// rebuild; the mission effects routed into the chat and system rings, the objective line and the
// persistent end-of-round banner; the device work a toggle event word names, the game.cfg colour
// word among it; the single-player pause word's menu share; the declutter level's persisted seed
// and its re-seed at every mission start; the per-mission caches (the HUDLS key labels, the F12
// legend, the equipped weapon, the tip's main-frame clock) and the map queries' retry throttle.
// The feed-reading half (the role's map feeds) is runtime/inmatch's (inmatch/hud_session_feeds.h).
// [orig: HUD_RenderAllOverlays @0x5a8070; HUD_BuildEntityInfo @0x4b8440]

#include <base/io/tick_rate.h>
#include <formats/gamecfg/game_cfg.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_chat_entry.h>
#include <runtime/hud/hud_toggles.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace opennova::hud {

class HudFrameCompiler;

// The mission effects the HUD routes, by the world EffectLog's kinds (world/world.h Effect).
enum class HudMissionEffect : uint8_t {
	None,            // not the HUD's
	LocalRoundReset, // "local_round_reset": the overlay's buffers reset (the round init)
	Text,            // "text": WAC text/ptext/text# (`text`) or BMS OutputText (`a`, the id)
	DebugText,       // "debug_text": consol/pconsol/consol#, forceanim
	Lose,            // "lose": the WAC Lose cause's gametext Misc key (`text`)
	Objective,       // "objective": a shown objective (a slot, b win, c the header id)
	HudItemFlash,    // "hud_item_flash": BMS action 28 sub 37 (a the timer, b its value)
	MissionTextChat, // "mission_text_chat": the authority's S2C 0x3F kind 1 relay (`text`)
	SubgoalWon,      // "subgoal_won": b the header id, c the round-still-running gate
	SubgoalLost,     // "subgoal_lost": the same, and the banner
};
HudMissionEffect hud_mission_effect(std::string_view effect_kind);

struct HudMissionEffectRecord {
	HudMissionEffect kind = HudMissionEffect::None;
	int32_t a = 0;
	int32_t b = 0;
	int32_t c = 0;
	std::string text;
};

// A key looked up in a table by itself, whatever its section (the mission text's relay keys:
// the table's own first match).
using TextKeyLookup = std::function<std::string(const std::string &key)>;

// One queued HUD message: literal text, or a mission-table Triggered Text id when the text is
// empty. A chat line posts into the CHAT ring, every other message into the SYSTEM ring.
struct HudPendingMessage {
	std::string text;
	int32_t text_id = 0;
	bool chat = false;
};

// What a mission effect asks of the built overlay at once: the round init's buffer reset and the
// item flash timers' stores (each a timer and its value), in the order they came. An overlay not
// yet built takes neither.
struct HudEffectOverlayWork {
	bool reset_overlay_buffers = false;
	std::vector<std::pair<int32_t, int32_t>> item_flashes;
};

// The device work one toggle event word names, applied in this order (each field's own order).
struct HudToggleWork {
	// The window actions' respawn init closes the sim's map overlay too (kOverlayWindowsCleared).
	bool close_map = false;
	bool restamp_detail = false;   // the overlay's live declutter level
	// The overlay's colour scheme and the game.cfg word (write_hud_color_word): retail's token
	// round trip, deliberately with no toast.
	bool restamp_color = false;
	bool restamp_showhud = false;  // the overlay's showhud flags and the FP gun bit (bit 0)
	bool cycle_sight_scale = false; // the dotsize cycle: the overlay's sight-scale step
	// The view actions in their catalog order (world/player_view.h kViewAction*), each as its own
	// action: the preference and the input bit.
	std::array<int, 3> view_actions{};
	int view_action_count = 0;
	// The friendly tags' cycle: the overlay's mode restamped, then its gametext Misc toast
	// (friendly_tag_toast_key) into the SYSTEM ring.
	bool friendly_tags_cycled = false;
	// The verbose toggle's Misc toast (verbose_toast_key).
	bool verbose_toggled = false;
	bool flip_mouse = false; // the session's live mouse word flips
	// The mousescale step: the typed argument rides above the event bits, the sign beside them;
	// the new word's toast (mouse_scale_toast) follows the step.
	bool step_mouse_scale = false;
	int32_t mouse_scale_param = 0;
	bool mouse_scale_negative = false;
	// The authority's join lock: the toggle runs the NovaWorld server-info update itself, then the
	// Misc toast names the new lock (lock_game_toast_key).
	bool toggle_join_lock = false;
	bool reset_briefing_pages = false;
	bool reset_scoreboard_page = false;
	bool open_command_map = false; // the commander_menu row: the shell opens the CMAP screen
	// The single-player pause word drives the session pause (the pause row flips it, the escape
	// chain clears it).
	bool apply_pause = false;
};
HudToggleWork hud_toggle_work(uint64_t events);

// The join lock's toast key for the toggle's verdict (1 locked, 0 unlocked; none below 0)
// [orig: STRMISC_LOCKGAME_ON / _OFF, Input_HandleActionBinding case 400 (lockgame)].
const char *lock_game_toast_key(int locked);

// The F12 legend's text: the gametext "hud" section's title and one label per legend icon.
struct HudMapLegendText {
	std::string title;
	std::vector<std::string> labels;
};
HudMapLegendText hud_map_legend_text(const GameTextLookup &gametext);

class HudSession {
public:
	// The HUD's message ring has 40 physical slots: no more pre-HUD messages than it can ever
	// present are kept (a net spectator may never get a local-player HUD).
	static constexpr size_t kMaxPendingMessages = 40;
	// The script chat lines post into the CHAT ring raw white: WAC text/ptext/text# through
	// Chat_AddSystemMessage, the WAC lose line and the BMS subgoal lines through
	// GameMsg_AddChatLineAndRelay, all into Chat_AddMessageChannel1 (each producer carries its
	// witness: wac/remote_command.cpp, wac/vm.cpp, hud_frame.h push_chat_line).
	static constexpr uint32_t kScriptChatArgb = 0xFFFFFFFFu;
	// The map queries' retry: about once a second (a shell throttle over a registry walk; every
	// HUD reader takes its retail clock from the frame clocks).
	static constexpr int64_t kMapQueryIntervalTicks = io::kTicksPerSecondInt;

	// The boot's seed from the persisted game.cfg words: the colour index clamps as retail's read
	// back does, the declutter level is stored verbatim (an out-of-range level blanks every gated
	// element until the huddetail cycle wraps it; docs/interface/hud-re.md "HUD declutter").
	HudSession(int32_t hud_color_word = kHudColorIndexDefault,
			int32_t hud_detail_word = kHudDetailLevelDefault);
	static HudSession from_cfg(const gamecfg::GameCfg &cfg);

	HudToggleState &toggles() { return toggles_; }
	const HudToggleState &toggles() const { return toggles_; }
	ChatEntry &chat() { return chat_; }
	const ChatEntry &chat() const { return chat_; }

	// --- The mission ---

	// The overlay system's init, every mission start and every role: the per-mission caches
	// clear (the HUDLS labels' revision, the legend, the map queries). The embedder then runs the
	// status view's rule by role once a session exists (hud_toggles_session_init: only a dedicated
	// host starts on it).
	void begin_mission();
	// The mission's teardown: the toggles take the respawn init (`restart`: the SP restart's,
	// which keeps the tip's once-counters), the chat line and the per-mission state clear, the
	// pending messages drop, a hidden capture ends, and the session's end clears the status view
	// (out of a session nothing holds it).
	void end_mission(bool restart);

	// --- The mission effects ---

	// Route one effect: the WAC text lines and the lose, objective, relay and subgoal lines into
	// the CHAT ring, the BMS triggered text and the debug lines into the SYSTEM ring (each
	// queued: PreMission effects drain before the HUD exists), the round reset and the item flash
	// onto the built overlay (`work`).
	// [orig: WacAction_Lose @0x4ed3f0 -> GameMsg_AddChatLineAndRelay @0x5ba170 (the chat line;
	//  the KEY rides the wire and each client re-resolves it) + GameMsg_SetBannerText @0x5ba200 /
	//  GameMsg_SetTeamBannerText @0x5ba1d0 (the persistent banner the MISSION FAILED screen
	//  composes, cleared at the next round start @0x5b71b0); HUD_ShowObjectiveNotification
	//  @0x5BA2E0 posts both its lines through Chat_AddMessageChannel1 @0x4985D0; the relay
	//  rides the chat ring like GameMsg_AddChatLineAndRelay @0x5BA170]
	void apply_mission_effect(const HudMissionEffectRecord &effect, const GameTextLookup &gametext,
			const GameTextLookup &mission, const TextKeyLookup &mission_keys,
			HudEffectOverlayWork &work);
	const std::deque<HudPendingMessage> &pending_messages() const { return pending_; }
	size_t pending_chat_line_count() const;
	// Post the queued messages into the overlay's rings at `main_frame` (the frame's main-frame
	// counter stamps each): a chat line raw white, a literal, or the mission's Triggered Text line
	// (hud_game_text.h triggered_text; a miss posts nothing and is reported). Returns the count
	// posted.
	size_t flush_pending_messages(HudFrameCompiler &compiler, uint32_t main_frame,
			const GameTextLookup &mission);

	// The latest player-facing mission text (the WAC text line, the triggered text).
	const std::string &objective_line() const { return objective_; }
	// The end-of-round banner line (the WAC Lose cause, a lost subgoal's line), kept until the
	// teardown so the MISSION FAILED screen can compose it.
	// [orig: g_BannerText @0x28E3DA0, written by GameMsg_SetBannerText @0x5ba200]
	const std::string &banner_line() const { return banner_; }

	// --- The toggles ---

	// The game.cfg colour word: written back on each cycle, saved with the block at its next
	// save point (the hudcolor action's token round trip).
	void write_hud_color_word(gamecfg::GameCfg &cfg) const;
	// The persisted declutter value (read at boot; the settings path owns its writes).
	int32_t hud_detail_config() const { return hud_detail_config_; }
	// The one LIVE declutter-level write: the cycle, the death-screen force and the mission
	// start's re-seed land here; the persisted value is never touched (retail's cycle and death
	// force write the layer level only).
	void set_hud_detail_level(int level) { toggles_.hud_detail_level = level; }
	// The mission start's apply: the live level re-seeded from the persisted value, so a death
	// screen's forced blank never outlives its mission.
	// [orig: the re-apply @0x59DD75 from Game_InitNewRound / HUD_InitOverlaySystem]
	void reapply_persisted_hud_detail() { set_hud_detail_level(hud_detail_config_); }
	// The in-game menu's share of the single-player pause word: its open sets bit 0 and its
	// resume clears it, out of a session only. True when the open also shuts the audio channels
	// down, as the pause row does (world WorldOutbox::audio_channels_shutdown).
	// [orig: UI_OptionsScreenInit `or 1` @0x554dd8 / UI_IngameBackResumeCommand `and ~1`
	//  @0x55549e, both behind !is_in_session]
	bool set_menu_pause(bool paused, bool in_session);

	// The render comparison's declutter: a reversible transaction over the live level, the
	// blank level for the capture and the exact level back after it. False when one is active.
	bool begin_hidden_capture();
	void finish_hidden_capture();
	bool hidden_capture_active() const { return hidden_capture_active_; }

	// --- The per-mission caches ---

	// The tip's countdown steps once a main frame: the main frames since the last frame
	// (none on the mission's first).
	int tip_frames_since(int64_t main_frame);
	// Whether the HUDLS key labels format again at the binding set's `revision` (a new mission's
	// first frame, or a binding change).
	bool slot_bar_labels_due(int64_t revision);
	int64_t slot_bar_labels_revision() const { return slot_bar_labels_revision_; }
	// Whether the equipped weapon changed (its HUD slice re-resolves only then).
	bool weapon_changed(const std::string &weapon_name);
	// The F12 legend's text, resolved once per mission at its first open.
	const HudMapLegendText &map_legend(const GameTextLookup &gametext);
	// The legend's text as it stands (empty until its first open this mission).
	const HudMapLegendText &map_legend_text() const { return map_legend_; }

	// The map queries' retry: a query is due while its feed is still missing, at most once per
	// kMapQueryIntervalTicks of `now_ticks` (a due query arms the next).
	bool map_grid_origin_due(int64_t now_ticks);
	void set_map_grid_origin_present(bool present) { map_grid_origin_present_ = present; }
	bool map_grid_origin_present() const { return map_grid_origin_present_; }
	bool map_footprints_due(int64_t now_ticks);
	void set_map_footprints_fed() { map_footprints_fed_ = true; }
	bool map_footprints_fed() const { return map_footprints_fed_; }

private:
	void queue(HudPendingMessage message);

	HudToggleState toggles_;
	ChatEntry chat_;
	int32_t hud_detail_config_ = kHudDetailLevelDefault;
	std::deque<HudPendingMessage> pending_;
	std::string objective_;
	std::string banner_;
	bool hidden_capture_active_ = false;
	int hidden_saved_detail_level_ = 0;
	int64_t tip_frames_seen_ = -1;
	int64_t slot_bar_labels_revision_ = -1;
	std::string weapon_name_;
	bool map_legend_resolved_ = false;
	HudMapLegendText map_legend_;
	bool map_grid_origin_present_ = false;
	int64_t map_grid_origin_next_ticks_ = 0;
	bool map_footprints_fed_ = false;
	int64_t map_footprints_next_ticks_ = 0;
};

} // namespace opennova::hud
