#include "hud_session.h"

#include "hud_config_tokens.h"
#include "hud_frame.h"
#include "hud_game_text.h"
#include "hud_overlay_windows.h"

#include <base/io/log.h>
#include <runtime/world/player_view.h>

#include <algorithm>

namespace opennova::hud {

namespace {

namespace ev = hud_toggle_event;

struct EffectKindName {
	const char *name;
	HudMissionEffect kind;
};
constexpr EffectKindName kEffectKinds[] = {
	{"local_round_reset", HudMissionEffect::LocalRoundReset},
	{"text", HudMissionEffect::Text},
	{"debug_text", HudMissionEffect::DebugText},
	{"lose", HudMissionEffect::Lose},
	{"objective", HudMissionEffect::Objective},
	{"hud_item_flash", HudMissionEffect::HudItemFlash},
	{"mission_text_chat", HudMissionEffect::MissionTextChat},
	{"subgoal_won", HudMissionEffect::SubgoalWon},
	{"subgoal_lost", HudMissionEffect::SubgoalLost},
};

} // namespace

HudMissionEffect hud_mission_effect(std::string_view effect_kind) {
	for (const EffectKindName &row : kEffectKinds)
		if (effect_kind == row.name) return row.kind;
	return HudMissionEffect::None;
}

HudToggleWork hud_toggle_work(uint64_t events) {
	const uint32_t bits = static_cast<uint32_t>(events);
	HudToggleWork work;
	work.close_map = (bits & ev::kOverlayWindowsCleared) != 0;
	work.restamp_detail = (bits & ev::kHudDetailCycled) != 0;
	work.restamp_color = (bits & ev::kHudColorCycled) != 0;
	work.restamp_showhud = (bits & (ev::kShowHudCycled | ev::kGunBitChanged)) != 0;
	work.cycle_sight_scale = (bits & ev::kDotsizeCycled) != 0;
	// The view rows in their catalog order, each as its own action (world/player_view.h
	// player_view_apply_view_action: the preference and the input bit).
	if (bits & ev::kFirstPersonSelected)
		work.view_actions[work.view_action_count++] = world::kViewActionFirstPerson;
	if (bits & ev::kGunViewSelected)
		work.view_actions[work.view_action_count++] = world::kViewActionWithGun;
	if (bits & ev::kThirdPersonSelected)
		work.view_actions[work.view_action_count++] = world::kViewActionChase;
	work.friendly_tags_cycled = (bits & ev::kFriendlyTagsCycled) != 0;
	work.verbose_toggled = (bits & ev::kVerboseToggled) != 0;
	work.flip_mouse = (bits & ev::kMouseFlipped) != 0;
	work.step_mouse_scale = (bits & ev::kMouseScaleStepped) != 0;
	work.mouse_scale_param = static_cast<int32_t>(events >> ev::kMouseScaleParamShift);
	work.mouse_scale_negative = (bits & ev::kMouseScaleNegative) != 0;
	work.toggle_join_lock = (bits & ev::kJoinLockToggled) != 0;
	work.reset_briefing_pages = (bits & ev::kBriefingPagesReset) != 0;
	work.reset_scoreboard_page = (bits & ev::kScoreboardPageReset) != 0;
	work.open_command_map = (bits & ev::kCommandMapOpened) != 0;
	work.apply_pause = (bits & (ev::kPauseToggled | ev::kPauseCleared)) != 0;
	return work;
}

const char *lock_game_toast_key(int locked) {
	if (locked < 0) return nullptr;
	return locked == 1 ? "STRMISC_LOCKGAME_ON" : "STRMISC_LOCKGAME_OFF";
}

HudMapLegendText hud_map_legend_text(const GameTextLookup &gametext) {
	// [orig: GameText_GetString("Hud", "hud_map_legend") @0x49749e; GameText_GetString("Hud",
	//  name) @0x4977a7, one per legend icon]
	HudMapLegendText text;
	text.title = game_text(gametext, kGameTextHud, "hud_map_legend", "");
	text.labels.reserve(static_cast<size_t>(kHudMapLegendIconCount));
	for (const HudMapLegendIcon &icon : kHudMapLegendIcons)
		text.labels.push_back(game_text(gametext, kGameTextHud, icon.name, ""));
	return text;
}

HudSession::HudSession(int32_t hud_color_word, int32_t hud_detail_word)
	: hud_detail_config_(hud_detail_word) {
	toggles_.hud_color_index = clamp_hud_color_index(hud_color_word);
	toggles_.hud_detail_level = hud_detail_word;
}

HudSession HudSession::from_cfg(const gamecfg::GameCfg &cfg) {
	return HudSession(cfg.hud_color_index, cfg.hud_detail);
}

void HudSession::begin_mission() {
	slot_bar_labels_revision_ = -1;
	map_legend_resolved_ = false;
	map_legend_ = HudMapLegendText{};
	map_grid_origin_present_ = false;
	map_grid_origin_next_ticks_ = 0;
	map_footprints_fed_ = false;
	map_footprints_next_ticks_ = 0;
}

void HudSession::end_mission(bool restart) {
	finish_hidden_capture();
	weapon_name_.clear();
	objective_.clear();
	banner_.clear();
	if (restart)
		hud_toggles_restart_round(toggles_);
	else
		hud_toggles_reset_mission(toggles_);
	tip_frames_seen_ = -1;
	slot_bar_labels_revision_ = -1;
	chat_.reset();
	pending_.clear();
	hud_toggles_session_init(toggles_, false, true);
}

void HudSession::queue(HudPendingMessage message) {
	pending_.push_back(std::move(message));
	while (pending_.size() > kMaxPendingMessages) pending_.pop_front();
}

void HudSession::apply_mission_effect(const HudMissionEffectRecord &e,
		const GameTextLookup &gametext, const GameTextLookup &mission,
		const TextKeyLookup &mission_keys, HudEffectOverlayWork &work) {
	switch (e.kind) {
		case HudMissionEffect::None:
			return;
		case HudMissionEffect::LocalRoundReset:
			work.reset_overlay_buffers = true;
			return;
		case HudMissionEffect::Text:
			// WAC text/ptext/text# carries a literal and posts into the CHAT ring; BMS OutputText
			// carries a nonzero Triggered Text id and posts into the SYSTEM ring.
			if (!e.text.empty()) {
				objective_ = e.text;
				queue({e.text, 0, true});
			} else if (e.a != 0) {
				queue({std::string(), e.a, false});
			}
			return;
		case HudMissionEffect::DebugText:
			// The SYSTEM ring, the objective line untouched.
			if (!e.text.empty()) queue({e.text, 0, false});
			return;
		case HudMissionEffect::Lose: {
			// The gametext Misc line as GameText_GetString answers it: the entry's text as the
			// table holds it, empty on a miss; the banner takes it either way, the chat ring only
			// a line with text [orig: GameText_GetString @0x51ebd0; GameMsg_AddChatLineAndRelay
			// @0x5ba170 posts only `if (*message)`].
			if (e.text.empty()) return;
			const std::string line = game_text(gametext, kGameTextMisc, e.text.c_str(), "");
			banner_ = line;
			if (!line.empty()) queue({line, 0, true});
			return;
		}
		case HudMissionEffect::Objective: {
			// The gametext header, then the mission directive (hud_game_text.h carries the keys,
			// the length rule and the witness).
			const std::string header = objective_header(gametext);
			if (!header.empty()) queue({header, 0, true});
			const std::string directive = objective_directive(e.b != 0, e.c, mission);
			if (!directive.empty()) queue({directive, 0, true});
			return;
		}
		case HudMissionEffect::HudItemFlash:
			work.item_flashes.emplace_back(e.a, e.b);
			return;
		case HudMissionEffect::MissionTextChat: {
			// The key resolves in this peer's mission text; an empty line posts nothing.
			const std::string line = mission_keys ? mission_keys(e.text) : std::string();
			if (!line.empty()) queue({line, 0, true});
			return;
		}
		case HudMissionEffect::SubgoalWon:
		case HudMissionEffect::SubgoalLost: {
			if (e.c == 0) return;
			const bool lost = e.kind == HudMissionEffect::SubgoalLost;
			const std::string line = subgoal_message(lost, e.b, mission);
			if (line.empty()) return;
			if (lost) banner_ = line;
			queue({line, 0, true});
			return;
		}
	}
}

size_t HudSession::pending_chat_line_count() const {
	return static_cast<size_t>(std::count_if(pending_.begin(), pending_.end(),
			[](const HudPendingMessage &m) { return m.chat; }));
}

size_t HudSession::flush_pending_messages(HudFrameCompiler &compiler, uint32_t main_frame,
		const GameTextLookup &mission) {
	size_t posted = 0;
	const int now = static_cast<int>(main_frame);
	for (const HudPendingMessage &message : pending_) {
		if (message.chat) {
			compiler.push_chat_line(message.text, kScriptChatArgb, now);
		} else if (!message.text.empty()) {
			compiler.push_message(message.text, now);
		} else {
			// The mission's Triggered Text line; a miss shows nothing.
			const std::string text = triggered_text(message.text_id, mission);
			if (text.empty()) {
				io::logf(io::LogLevel::kWarn,
						"HUD: mission text ID%03d not found in the mission string table.",
						message.text_id);
				continue;
			}
			objective_ = text;
			compiler.push_message(text, now);
		}
		++posted;
	}
	pending_.clear();
	return posted;
}

void HudSession::write_hud_color_word(gamecfg::GameCfg &cfg) const {
	cfg.hud_color_index = toggles_.hud_color_index;
}

bool HudSession::set_menu_pause(bool paused, bool in_session) {
	if (in_session) return false;
	toggles_.paused = paused;
	return paused;
}

bool HudSession::begin_hidden_capture() {
	if (hidden_capture_active_) return false;
	hidden_saved_detail_level_ = toggles_.hud_detail_level;
	hidden_capture_active_ = true;
	toggles_.hud_detail_level = kHudDetailLevelBlank;
	return true;
}

void HudSession::finish_hidden_capture() {
	if (!hidden_capture_active_) return;
	hidden_capture_active_ = false;
	toggles_.hud_detail_level = hidden_saved_detail_level_;
}

int HudSession::tip_frames_since(int64_t main_frame) {
	int frames = 0;
	if (tip_frames_seen_ >= 0)
		frames = static_cast<int>(std::max<int64_t>(main_frame - tip_frames_seen_, 0));
	tip_frames_seen_ = main_frame;
	return frames;
}

bool HudSession::slot_bar_labels_due(int64_t revision) {
	if (revision == slot_bar_labels_revision_) return false;
	slot_bar_labels_revision_ = revision;
	return true;
}

bool HudSession::weapon_changed(const std::string &weapon_name) {
	if (weapon_name == weapon_name_) return false;
	weapon_name_ = weapon_name;
	return true;
}

const HudMapLegendText &HudSession::map_legend(const GameTextLookup &gametext) {
	if (!map_legend_resolved_) {
		map_legend_ = hud_map_legend_text(gametext);
		map_legend_resolved_ = true;
	}
	return map_legend_;
}

bool HudSession::map_grid_origin_due(int64_t now_ticks) {
	if (map_grid_origin_present_ || now_ticks < map_grid_origin_next_ticks_) return false;
	map_grid_origin_next_ticks_ = now_ticks + kMapQueryIntervalTicks;
	return true;
}

bool HudSession::map_footprints_due(int64_t now_ticks) {
	if (map_footprints_fed_ || now_ticks < map_footprints_next_ticks_) return false;
	map_footprints_next_ticks_ = now_ticks + kMapQueryIntervalTicks;
	return true;
}

} // namespace opennova::hud
