#include "hud/hud_session.h"

#include "audio/mission_audio.h"
#include "devtools/frame_stats.h"
#include "env/mission_environment.h"
#include "hud/hud_chat_entry.h"
#include "hud/hud_entity_info.h"
#include "hud/hud_overlay.h"
#include "hud/hud_pos.h"
#include "hud/hud_toggles.h"
#include "hud/player_hud_weapon_def.h"
#include "mnu/controls_model.h"
#include "object/item_database.h"
#include "player/game_config_block.h"
#include "player/local_player_presenter.h"
#include "resource_index/resource_root.h"
#include "rtxt/rtxt_string_file.h"
#include "rtxt/text_tables.h"
#include "simulation/hud_view_records.h"
#include "simulation/player_local_view.h"
#include "simulation/present_event_records.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"
#include "world/game_world.h"

#include <formats/rtxt/text_tables.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_config_tokens.h>
#include <runtime/hud/score_fanfare.h>
#include <runtime/world/waypoint_track.h>
#include <runtime/inmatch/hud_session_feeds.h>
#include <runtime/world/tick_accumulator.h>

#include <godot_cpp/classes/time.hpp>
#include <godot_cpp/core/object.hpp>

#include <cmath>
#include <string>

namespace godot {

namespace hud = opennova::hud;
namespace rtxt = opennova::rtxt;

namespace {

template <typename T>
T *device(ObjectID p_id) {
	return p_id.is_valid() ? Object::cast_to<T>(ObjectDB::get_instance(p_id)) : nullptr;
}

ObjectID id_of(Object *p_object) {
	return p_object != nullptr ? ObjectID(p_object->get_instance_id()) : ObjectID();
}

// The frame's string tables as registered (formats/rtxt/text_tables.h).
Ref<RtxtStringFile> gametext_table() { return TextTables::get_table(rtxt::kTableGameText); }
Ref<RtxtStringFile> mission_table() { return TextTables::get_table(rtxt::kTableMission); }

uint64_t now_usec(bool p_on) { return p_on ? Time::get_singleton()->get_ticks_usec() : 0; }

} // namespace

HudSession::HudSession() : session_(std::make_shared<hud::HudSession>()) {}

Ref<HudSession> HudSession::from_config(const Ref<GameConfigBlock> &p_block) {
	Ref<HudSession> session;
	session.instantiate();
	if (p_block.is_valid()) {
		*session->session_ = hud::HudSession::from_cfg(p_block->native().cfg());
		session->config_ = p_block;
	}
	return session;
}

Ref<HudToggles> HudSession::get_toggles() const {
	return HudToggles::over(std::shared_ptr<hud::HudToggleState>(session_, &session_->toggles()));
}

Ref<HudChatEntry> HudSession::get_chat() const {
	return HudChatEntry::over(std::shared_ptr<hud::ChatEntry>(session_, &session_->chat()));
}

void HudSession::set_world(GameWorld *p_world) { world_id_ = id_of(p_world); }
void HudSession::set_player_presenter(LocalPlayerPresenter *p_presenter) {
	presenter_id_ = id_of(p_presenter);
}
void HudSession::set_overlay(HudOverlay *p_overlay) { overlay_id_ = id_of(p_overlay); }
void HudSession::set_hud_pos(const Ref<HudPos> &p_hud_pos) { hud_pos_ = p_hud_pos; }

GameWorld *HudSession::world_() const { return device<GameWorld>(world_id_); }
LocalPlayerPresenter *HudSession::presenter_() const {
	return device<LocalPlayerPresenter>(presenter_id_);
}
HudOverlay *HudSession::overlay_() const { return device<HudOverlay>(overlay_id_); }

Ref<Simulation> HudSession::sim_() const {
	GameWorld *world = world_();
	return world != nullptr ? world->get_sim() : Ref<Simulation>();
}

opennova::inmatch::HudSessionRole HudSession::role_(const Ref<Simulation> &p_sim) const {
	return p_sim.is_valid() ? p_sim->hud_session_role() : opennova::inmatch::HudSessionRole{};
}

// --- The mission ---------------------------------------------------------------------------

void HudSession::begin_mission(const String &p_mission_file) {
	GameWorld *world = world_();
	HudOverlay *hud = overlay_();
	const Ref<ResourceRoot> root = world != nullptr ? world->get_resource_root() : Ref<ResourceRoot>();
	session_->begin_mission();
	// The HUD's string tables, refreshed from this world's root at every build so a second
	// runtime never reads the first root's strings. The gametext table IS gametext.bin (Game.bin
	// is the separate menu resource and carries no WepDes); the per-mission table is
	// <mission>.bin, else medmssn.bin (the engine's resolver, runtime_boot resolve_mission_text).
	if (root.is_valid()) {
		TextTables::register_table(rtxt::kTableGameText, TextTables::load_rtxt(root, "gametext.bin"));
		TextTables::register_table(rtxt::kTableMission,
				RtxtStringFile::load_mission_table(root, p_mission_file.get_basename()));
	}
	const Ref<Simulation> sim = sim_();
	if (hud != nullptr) {
		hud::HudFrameState &state = hud->native_state();
		// The objectives panel's header, resolved once against the fresh gametext table.
		std::string header;
		if (rtxt::table_text(rtxt::kTableGameText, rtxt::kSectionOverlays,
					"STROVER_MISSIONOBJECTIVES", header))
			state.objectives_header = header;
		// The grid-label origin, queried at once (the authority resolved it at promotion; a
		// joiner's arrives later and the frame retries).
		if (sim.is_valid()) {
			sim->hud_session_grid_origin(*session_, state);
		} else {
			state.minimap.grid_origin_present = false;
			state.minimap.grid_origin_x = 0;
			state.minimap.grid_origin_y = 0;
		}
		// The process words survive the per-mission rebuild like retail's globals: the friendly
		// tags' mode, the colour scheme, the LIVE declutter level (the round init's re-apply,
		// HudSession::reapply_persisted_hud_detail carries the witness) and the showhud flags.
		const hud::HudToggleState &t = session_->toggles();
		hud->set_friendly_tag_mode(
				static_cast<HudOverlay::FriendlyTagMode>(static_cast<int>(t.friendly_tag_mode)));
		hud->set_hud_color_index(t.hud_color_index);
		hud->set_hud_detail_level(t.hud_detail_level);
	}
	push_showhud_flags_();
	// The session's start applies the status view's rule by role.
	if (sim.is_valid()) {
		const opennova::inmatch::HudSessionRole role = role_(sim);
		hud::hud_toggles_session_init(session_->toggles(), role.in_session, role.mp_session_peer);
	}
}

void HudSession::end_mission(bool p_restart) {
	finish_hidden_capture();
	session_->end_mission(p_restart);
	TextTables::register_table(rtxt::kTableMission, Ref<RtxtStringFile>());
	weapon_ = Ref<PlayerHudWeaponDef>();
	weapon_changed_ = false;
	hud_pos_ = Ref<HudPos>();
	scoreboard_pushed_ = false;
	message_log_pushed_ = false;
	end_round_stats_pushed_ = false;
	lfp_pushed_ = false;
	vehicle_panel_pushed_ = false;
	pending_work_ = 0;
	overlay_id_ = ObjectID();
}

void HudSession::apply_mission_effects(const Array &p_effects) {
	HudOverlay *hud = overlay_();
	const hud::GameTextLookup gametext = hud::registered_text_lookup(rtxt::kTableGameText);
	const hud::GameTextLookup mission = hud::registered_text_lookup(rtxt::kTableMission);
	const std::shared_ptr<const rtxt::File> mission_file = rtxt::registered_table(rtxt::kTableMission);
	const hud::TextKeyLookup mission_keys = [mission_file](const std::string &p_key) {
		return mission_file != nullptr ? mission_file->get(p_key) : std::string();
	};
	for (int64_t i = 0; i < p_effects.size(); ++i) {
		const MissionEffect *effect = Object::cast_to<MissionEffect>(p_effects[i]);
		if (effect == nullptr) continue;
		hud::HudEffectOverlayWork work;
		session_->apply_mission_effect(opennova::inmatch::hud_mission_effect_record(effect->value()),
				gametext, mission, mission_keys, work);
		if (hud == nullptr) continue;
		if (work.reset_overlay_buffers) hud->reset_overlay_buffers();
		for (const auto &flash : work.item_flashes) hud->set_item_flash(flash.first, flash.second);
	}
}

// --- The frame -----------------------------------------------------------------------------

Ref<HudEntityInfo> HudSession::frame(const Ref<ControlsModel> &p_controls,
		const Ref<FrameStats> &p_stats) {
	weapon_changed_ = false;
	GameWorld *world = world_();
	HudOverlay *hud = overlay_();
	const Ref<Simulation> sim = sim_();
	if (world == nullptr || hud == nullptr || sim.is_null()) return Ref<HudEntityInfo>();
	LocalPlayerPresenter *presenter = presenter_();
	const bool stats_on = p_stats.is_valid() && p_stats->is_capture_active();
	// The frame's four clocks ahead of every setter and push, each HUD reader on the one its
	// original reads (engine world/world.h HudFrameCounters carries the witnesses).
	hud->set_frame_clocks(sim->get_logic_tick(), sim->get_main_frame_counter(),
			sim->get_session_frame_counter(), sim->get_hud_frame_counter());
	// Whether this frame's scene frame called its HUD pass (inmatch::FrameOutcome).
	hud->set_overlay_pass_called(world->get_last_frame_hud_pass_called());
	if (stats_on) {
		// The overlay's draw runs in the deferred flush after this frame: the previous frame's
		// compile and emit land here, armed only while the board captures.
		hud->set_draw_timing_enabled(true);
		const PackedInt64Array draw_us = hud->consume_draw_timing_us();
		if (draw_us.size() >= 2) {
			p_stats->add(FrameStats::HUD_DRAW_COMPILE, draw_us[0]);
			p_stats->add(FrameStats::HUD_DRAW_EMIT, draw_us[1]);
		}
	} else if (draw_timing_armed_) {
		hud->set_draw_timing_enabled(false);
	}
	draw_timing_armed_ = stats_on;
	const Ref<RtxtStringFile> gametext = gametext_table();
	const Ref<RtxtStringFile> mission = mission_table();

	// The equipped weapon's HUD slice, re-resolved on weapon change only
	// (HudSession::weapon_changed): the overlay takes the record's fields, the SIGHTS card (the
	// presenter's) the authored rows.
	const Ref<PlayerHudWeaponDef> weapon = world->local_player_hud_weapon_def();
	if (session_->weapon_changed(
				weapon.is_valid() ? opennova::to_std(weapon->get_weapon_name()) : std::string())) {
		hud->install_weapon(weapon, gametext);
		weapon_ = weapon;
		weapon_changed_ = true;
		sight_slide_multiplier_ = weapon.is_valid() ? weapon->get_sight_slide_multiplier() : 0;
	}

	const uint64_t t0 = now_usec(stats_on);
	// The frame the player presenter composed and stamped this display frame (a standalone HUD
	// observes the last composed view; neither read composes a camera; D-HUD-29).
	const Ref<PlayerLocalView> lv =
			presenter != nullptr ? presenter->presented_view() : world->local_player_view();
	if (lv.is_valid()) sight_slide_multiplier_ = lv->get_sight_slide_multiplier();
	// The frame's main viewport off that view's camera: every world point the HUD draws at
	// projects through it.
	hud->set_view_frame(lv);
	// The frame's HUD info record, the engine's fill from the view, the equipped weapon's FSM
	// view, the def slice and the health pair (hud/hud_entity_info.h carries the witnesses).
	const Ref<HudEntityInfo> info = HudEntityInfo::build(sim->get_local_player_health(),
			sim->get_local_player_max_health(), lv, world->local_player_weapon_view(), weapon);

	const uint64_t t1 = now_usec(stats_on);
	hud->set_attach_labels(gametext, sim);
	hud::HudToggleState &t = session_->toggles();
	// The overlay-panel pass's key-toggled state (the pause word draws STROVER7).
	hud->set_overlay_panel_windows(t.emotes_menu_open, t.radio_menu_open, t.paused);
	hud->set_menu_screen_open(menu_screen_open_);
	// The tip: the world's producer events in their order, then the countdown by the main frames
	// since the last frame, then the draw feed (hud/tip_system.h).
	const PackedByteArray tip_events = sim->take_tip_events();
	hud::hud_toggles_tip_events(t, tip_events.ptr(), static_cast<size_t>(tip_events.size()));
	// The 0x0F's death-screen HUD blank, queued by the replica fold only with the death screen up.
	if (sim->take_hud_detail_blank()) apply_death_screen_hud_detail();
	hud::hud_toggles_tip_frames(t, session_->tip_frames_since(sim->get_main_frame_counter()));
	hud->set_tip(t.tips.tip, t.tips.countdown, sim->local_player_dead(), gametext, p_controls);
	// The HUDLS key labels by weapon category: the live binding of record 200 + category,
	// formatted again only when a binding changed or the HUD was rebuilt.
	if (p_controls.is_valid() && session_->slot_bar_labels_due(p_controls->get_binding_revision())) {
		PackedStringArray labels;
		for (int category = 0; category < 12; ++category)
			labels.push_back(p_controls->display_text_for_action_code(
					ControlsModel::SLOT_BAR_KEY_RECORD_BASE + category));
		hud->set_slot_bar_key_labels(labels);
	}
	// The breath bar, the MP session lines, the HUDLS scan, the open menus, the frame-rate overlay.
	hud->set_role_facts(sim, gametext);
	// The overhead friendly tags (D-HUD-20) at the environment's live fog distance.
	const MissionEnvironment *env = world->get_environment_node();
	hud->set_friendly_tags(true, env != nullptr ? env->get_fog_distance() : 0.0f, sim);
	hud->set_radio_request_icon_viewer(sim->local_player_radio_request_icon_viewer());

	const uint64_t t2 = now_usec(stats_on);
	// The waypoint label: the current track entry's name (hud_game_text.h) and its 2D ground
	// distance (hud_math.h); the engine's gate hides it (world::waypoint_label_shown).
	const Ref<WaypointHudView> wp = sim->get_waypoint_hud_view();
	const Vector3 player = sim->get_local_player_position();
	const bool waypoint = wp.is_valid() && opennova::world::waypoint_label_shown(wp->value());
	const uint64_t t3 = now_usec(stats_on);

	hud->set_player_state(info->get_health_fraction(), info->get_stance(), info->get_fov_h_deg());
	hud->set_player_context(lv);
	// The radar contacts the session stepped behind the overlay pass's gates, then the session
	// takes this HUD's gates back (inmatch/session.h).
	hud->set_minimap_radar(sim->get_hud_radar());
	sim->set_hud_radar_gates(hud->get_radar_frame_gates());
	hud->set_minimap_state(Vector2(player.x, -player.z), player.y,
			sim->get_local_player_heading_bam(), sim->get_hud_radar_zoom_q16(),
			sim->get_hud_big_zoom_q16(), sim->get_hud_map_mode(), sim->get_hud_map_flip_180(),
			sim->get_hud_minimap_snapshot());
	hud->set_minimap_overlays(sim->get_hud_minimap_overlays(gametext));
	// The map feeds a joiner's stream can deliver after the HUD builds, retried ~1 Hz on the
	// frame clock's ticks.
	sim->hud_session_map_feeds(*session_,
			opennova::world::ticks_from_ms(GameWorld::current_frame_clock_ms()), hud->native_state());
	hud->queue_redraw();
	// The talk rows ride the router's input frames; this frame draws the line.
	if (presenter != nullptr) presenter->set_hud_chat(chat_ref_(), gametext);
	hud->set_chat_input(chat_ref_(), sim->is_mp_session());
	// The HUD binding rows' events in input-frame order (PlayerInputRouter::poll_hud_toggles).
	dispatch_toggle_frames_();
	// The quit dialog and the authority's server-status page.
	const opennova::inmatch::HudSessionRole role = role_(sim);
	hud->set_quit_dialog(t.quit_dialog_open, sim->is_mp_session(), role.authority, gametext);
	hud->set_server_status_page(t.server_status_view, t.server_status_score_list, gametext, sim);
	// The weapon cluster as the info record carries it.
	hud->set_weapon_state(info->get_weapon_drawn(), info->get_clip(), info->get_reserve(),
			info->get_heat(), info->get_spread_fp16(), info->get_aimed_shot_available(),
			info->get_keep_crosshair_while_aimed(), info->get_windup_active(),
			info->get_windup_held_ticks());
	hud->set_weapon_ammo_key(info->get_ammo_bucket(), info->get_ammo_class_id());
	hud->set_scope_state(lv, gametext);
	hud->set_combat_state(lv, gametext,
			p_controls.is_valid() ? p_controls->display_text_for_token("useitem") : String());
	hud->set_view_state(info->get_binoculars_view_active(),
			presenter != nullptr ? presenter->aim_screen_point() : Vector2(INFINITY, INFINITY));
	push_vehicle_panel_(sim, info->get_stance());
	if (waypoint) {
		const Vector3 pos = wp->get_position();
		hud->set_waypoint(HudPos::waypoint_label(mission, gametext, wp, sim->is_mp_session(),
								  sim->get_session_game_type()),
				HudPos::waypoint_distance_m(Vector2(pos.x - player.x, pos.z - player.z)),
				Vector2(pos.x, -pos.z), pos.y);
	} else {
		hud->clear_waypoint();
	}
	// The objectives panel: the sim's shown win-condition rows through the mission table.
	hud->set_objectives(t.objectives_visible, mission, sim);
	// The damage quads ride the overlay's view veil.
	hud->update_damage_feedback(info->get_flash_white(), info->get_flash_red(),
			info->get_flash_revive(), info->get_flash_revive_channel());

	const uint64_t t4 = now_usec(stats_on);
	// The effects drained during the world's tick, posted at this frame's main-frame stamp, then
	// the message feeds in wire order.
	flush_(sim);
	hud->set_kill_announcement(sim->get_kill_announcement_text(),
			sim->get_kill_announcement_tick(sim->get_logic_tick()));
	push_lanes_(sim, p_controls);
	if (stats_on) {
		const uint64_t t5 = now_usec(true);
		p_stats->add(FrameStats::HUD_SCALARS, static_cast<int64_t>(t1 - t0));
		p_stats->add(FrameStats::HUD_ATTACH, static_cast<int64_t>(t2 - t1));
		p_stats->add(FrameStats::HUD_WAYPOINT, static_cast<int64_t>(t3 - t2));
		p_stats->add(FrameStats::HUD_INFO, static_cast<int64_t>(t4 - t3));
		p_stats->add(FrameStats::HUD_FLUSH, static_cast<int64_t>(t5 - t4));
	}
	return info;
}

void HudSession::dedicated_host_frame() {
	HudOverlay *hud = overlay_();
	const Ref<Simulation> sim = sim_();
	if (hud == nullptr || sim.is_null()) return;
	hud->set_frame_clocks(sim->get_logic_tick(), sim->get_main_frame_counter(),
			sim->get_session_frame_counter(), sim->get_hud_frame_counter());
	hud->set_chat_input(chat_ref_(), sim->is_mp_session());
	// A dedicated host's HUD rows still dispatch: the router polls them with or without a player
	// and the role's head admits them (Role::action_head_admits carries the witness).
	dispatch_toggle_frames_();
	const Ref<RtxtStringFile> gametext = gametext_table();
	// The page's riders read the role facts as a listen host's do (inmatch/role_feeds.h).
	hud->set_role_facts(sim, gametext);
	const hud::HudToggleState &t = session_->toggles();
	hud->set_quit_dialog(t.quit_dialog_open, sim->is_mp_session(), role_(sim).authority, gametext);
	hud->set_server_status_page(t.server_status_view, t.server_status_score_list, gametext, sim);
}

int HudSession::take_work() {
	const int work = pending_work_;
	pending_work_ = 0;
	return work;
}

void HudSession::dispatch_toggle_frames_() {
	LocalPlayerPresenter *presenter = presenter_();
	if (presenter == nullptr) return;
	const PackedInt64Array frames = presenter->take_hud_toggle_events();
	for (int64_t i = 0; i < frames.size(); ++i) apply_toggle_events(frames[i]);
}

void HudSession::flush_(const Ref<Simulation> &p_sim) {
	HudOverlay *hud = overlay_();
	if (hud == nullptr) return;
	if (!session_->pending_messages().empty()) {
		session_->flush_pending_messages(hud->native_compiler(), hud->native_main_frame(),
				hud::registered_text_lookup(rtxt::kTableMission));
		hud->queue_redraw();
	}
	// The S2C 0x1E game events, 0x14 chat lines and 0x32 join/leave lines into their rings in
	// wire order (HudOverlay.post_feed_lines over Simulation::drain_feed_posts); the involved 0x1E
	// line feeds the banner, stamped with the logic tick (hud/feed_format.h KillAnnouncement).
	const String announcement =
			hud->post_feed_lines(p_sim, gametext_table(), session_->toggles().mp_verbose);
	if (!announcement.is_empty())
		p_sim->retain_feed_announcement(announcement, p_sim->get_logic_tick());
}

void HudSession::push_vehicle_panel_(const Ref<Simulation> &p_sim, int p_stance) {
	// While the local player rides something, the root vehicle's items.def sid joins its hudpos
	// VEHICLE_HUD block (hud/hud_vehicle_panel.h carries the witnesses); an item whose alias
	// stayed empty matches a block with no sid, as the commit's _stricmp does, an unknown item
	// none, and no block draws no panel.
	HudOverlay *hud = overlay_();
	GameWorld *world = world_();
	const Ref<VehiclePanelView> view = p_sim->get_vehicle_panel_view();
	const Ref<ItemDatabase> items = world != nullptr ? world->get_item_db() : Ref<ItemDatabase>();
	Ref<VehicleHudBlock> block;
	if (view.is_valid() && view->get_shown() && hud_pos_.is_valid() && items.is_valid() &&
			items->has_item(view->get_item_id()))
		block = hud_pos_->get_vehicle_hud(items->get_sid(view->get_item_id()));
	if (block.is_valid()) {
		hud->set_vehicle_panel(true, block, p_stance, p_sim);
		vehicle_panel_pushed_ = true;
	} else if (vehicle_panel_pushed_) {
		hud->set_vehicle_panel(false, Ref<VehicleHudBlock>(), 0, Ref<Simulation>());
		vehicle_panel_pushed_ = false;
	}
}

void HudSession::push_lanes_(const Ref<Simulation> &p_sim, const Ref<ControlsModel> &p_controls) {
	HudOverlay *hud = overlay_();
	GameWorld *world = world_();
	const hud::HudToggleState &t = session_->toggles();
	const Ref<RtxtStringFile> gametext = gametext_table();
	const auto text = [](const char *section, const char *key) {
		return RtxtStringFile::text_to_gd(
				rtxt::table_text_or(rtxt::kTableGameText, section, key, std::string()));
	};
	// The S2C 0x81 hit-confirm tone behind the cfg word (hud/score_fanfare.h carries the
	// witnesses).
	const Ref<ScoreFeedback> feedback = p_sim->take_score_feedback();
	if (feedback.is_valid() && !feedback->get_tone().is_empty() && config_.is_valid() &&
			hud::score_fanfare_enabled(config_->native().cfg()) && world != nullptr) {
		if (MissionAudio *audio = world->get_mission_audio()) audio->ui_soundset(feedback->get_tone());
	}
	// The Recent Messages (J) window and its stdbox title Overlays/STROVER43 (no literal is
	// witnessed; absent, the box draws untitled; hud/hud_toggles.h carries the flag's witness).
	if (t.message_log_open && !message_log_pushed_) {
		hud->set_message_log_title(text(rtxt::kSectionOverlays, "STROVER43"));
		hud->set_message_log_shown(true);
		message_log_pushed_ = true;
	} else if (!t.message_log_open && message_log_pushed_) {
		hud->set_message_log_shown(false);
		message_log_pushed_ = false;
	}
	// The key-toggled windows: the F1 help from the live help pages, the F12 legend's gametext
	// labels (resolved once per mission, at its first open), and the I briefing, drawn only out
	// of a session (a session's window draws the MP end-game screen instead).
	const bool legend_open = t.map_legend_open;
	if (t.help_open && !legend_open && p_controls.is_valid()) {
		hud->set_help_screen(true, p_controls->get_help_title(), p_controls->get_help_page_line(),
				ControlsModel::help_footer(), p_controls->get_help_keys(),
				p_controls->get_help_texts());
	} else {
		hud->set_help_screen(false, String(), String(), String(), PackedStringArray(),
				PackedStringArray());
	}
	const hud::HudMapLegendText &legend = legend_open
			? session_->map_legend(hud::registered_text_lookup(rtxt::kTableGameText))
			: session_->map_legend_text();
	hud::HudMapLegendState &legend_state = hud->native_state().map_legend;
	legend_state.shown = legend_open;
	legend_state.title = legend.title;
	legend_state.labels = legend.labels;
	hud->set_briefing(t.briefing_mode != 0 && !p_sim->is_mp_session(), p_sim);
	// The SP Show Score (F5) panel: the engine-composed rows, title and labels
	// (hud/end_round_statistics.h end_round_statistics_text), re-pushed every frame while open
	// (the counters are live).
	if (t.end_round_stats_open) {
		const Ref<EndRoundStatistics> stats = p_sim->get_end_round_statistics();
		if (stats.is_valid()) {
			hud->set_end_round_statistics(true, stats->get_raised(), stats->title(gametext),
					stats->labels(gametext), stats->get_values());
		}
		end_round_stats_pushed_ = true;
	} else if (end_round_stats_pushed_) {
		hud->set_end_round_statistics(false, false, String(), PackedStringArray(),
				PackedStringArray());
		end_round_stats_pushed_ = false;
	}
	// The zone status panel every HUD frame whatever the game type; the engine element picks
	// the conquest arm (inmatch/hud_frame_feeds.h hud_lfp_panel_feed), the takeover dialog rides
	// the same push.
	hud->set_lfp_panel(true, p_sim->get_session_game_type(), p_sim->get_local_player_team(),
			gametext, p_sim);
	lfp_pushed_ = true;
	// The Tab player list while its flag is up, cleared once on close.
	if (t.scoreboard_open) {
		hud->set_scoreboard(true, p_sim, gametext);
		scoreboard_pushed_ = true;
	} else if (scoreboard_pushed_) {
		hud->set_scoreboard(false, Ref<Simulation>(), Ref<RtxtStringFile>());
		scoreboard_pushed_ = false;
	}
}

// --- The toggles ---------------------------------------------------------------------------

void HudSession::push_showhud_flags_() {
	const uint32_t flags = session_->toggles().showhud_flags;
	if (HudOverlay *hud = overlay_()) hud->set_showhud_flags(static_cast<int>(flags));
	if (LocalPlayerPresenter *presenter = presenter_())
		presenter->set_fp_gun_visible((flags & hud::kShowHudFlagGun) != 0);
}

void HudSession::post_misc_toast_(const char *p_key) {
	HudOverlay *hud = overlay_();
	if (hud == nullptr || p_key == nullptr) return;
	std::string line;
	// An empty line posts nothing (hud_toggles.h mouse_scale_toast carries the witness).
	if (rtxt::table_text(rtxt::kTableGameText, rtxt::kSectionMisc, p_key, line) && !line.empty())
		hud->push_message_text(line);
}

int HudSession::apply_toggle_events(int64_t p_events) {
	const hud::HudToggleWork work = hud::hud_toggle_work(static_cast<uint64_t>(p_events));
	HudOverlay *hud = overlay_();
	GameWorld *world = world_();
	LocalPlayerPresenter *presenter = presenter_();
	const Ref<Simulation> sim = sim_();
	const hud::HudToggleState &t = session_->toggles();
	int result = 0;
	if (work.close_map && sim.is_valid()) sim->request_hud_map_close();
	if (work.restamp_detail && hud != nullptr) hud->set_hud_detail_level(t.hud_detail_level);
	if (work.restamp_color) {
		if (config_.is_valid()) session_->write_hud_color_word(config_->native().cfg());
		if (hud != nullptr) hud->set_hud_color_index(t.hud_color_index);
	}
	if (work.restamp_showhud) push_showhud_flags_();
	if (work.cycle_sight_scale && hud != nullptr) {
		hud->cycle_sight_scale();
		result |= WORK_SIGHT_STATE;
	}
	for (int i = 0; i < work.view_action_count; ++i)
		if (presenter != nullptr) presenter->apply_view_action(work.view_actions[i]);
	if (work.friendly_tags_cycled) {
		if (hud != nullptr)
			hud->set_friendly_tag_mode(
					static_cast<HudOverlay::FriendlyTagMode>(static_cast<int>(t.friendly_tag_mode)));
		post_misc_toast_(hud::friendly_tag_toast_key(t.friendly_tag_mode));
	}
	if (work.verbose_toggled) post_misc_toast_(hud::verbose_toast_key(t.mp_verbose));
	// The look rows act on the session's live words (the profile record keeps its own).
	if (work.flip_mouse && sim.is_valid()) sim->flip_local_player_mouse();
	if (work.step_mouse_scale && sim.is_valid()) {
		const int scale =
				sim->step_local_player_mouse_scale(work.mouse_scale_param, work.mouse_scale_negative);
		const std::string line = hud::mouse_scale_toast(
				hud::registered_text_lookup(rtxt::kTableGameText), scale);
		if (hud != nullptr && !line.empty()) hud->push_message_text(line);
	}
	if (work.toggle_join_lock && sim.is_valid())
		post_misc_toast_(hud::lock_game_toast_key(sim->toggle_join_lock()));
	if (work.reset_briefing_pages && hud != nullptr) hud->cycle_briefing_page(0, false);
	if (work.reset_scoreboard_page && hud != nullptr) hud->reset_scoreboard_page();
	if (work.open_command_map) result |= WORK_COMMAND_MAP;
	if (work.apply_pause && world != nullptr) world->set_shell_paused(t.paused);
	pending_work_ |= result;
	return result;
}

bool HudSession::escape() {
	const uint32_t events = opennova::inmatch::hud_session_escape(*session_, role_(sim_()));
	apply_toggle_events(events);
	return (events & hud::hud_toggle_event::kEscapeClosedWindow) != 0;
}

void HudSession::begin_end_of_round_cine() {
	const Ref<Simulation> sim = sim_();
	apply_toggle_events(hud::hud_toggles_cine_start(session_->toggles(),
			sim.is_valid() && sim->is_mp_session()));
}

bool HudSession::chat_key(const Ref<InputEventKey> &p_key) {
	const Ref<Simulation> sim = sim_();
	// The flood check reads the main-frame counter (inmatch/chat_flood.h).
	const int64_t main_frame = sim.is_valid() ? sim->get_main_frame_counter() : 0;
	const Ref<HudChatEntry> chat = chat_ref_();
	if (!chat->key_event(p_key, sim, gametext_table(), main_frame)) return false;
	if (HudOverlay *hud = overlay_()) {
		if (chat->last_events() & HudChatEntry::EVENT_FLOOD_ECHO)
			hud->push_chat_line(chat->get_echo_text(), chat->get_echo_argb());
		hud->set_chat_input(chat, sim.is_valid() && sim->is_mp_session());
	}
	return true;
}

bool HudSession::is_chat_capturing() const { return session_->chat().capturing(); }

void HudSession::set_chat_presets(const PackedStringArray &p_macros) {
	for (int64_t i = 0; i < p_macros.size(); ++i)
		session_->chat().set_preset(static_cast<int>(i), opennova::gd_to_retail_text(p_macros[i]));
}

void HudSession::set_tip_options(bool p_keyboard_tips, bool p_gameplay_tips) {
	session_->toggles().tips.options.keyboard_tips = p_keyboard_tips;
	session_->toggles().tips.options.gameplay_tips = p_gameplay_tips;
}

void HudSession::set_menu_screen_open(bool p_open) {
	menu_screen_open_ = p_open;
	// Pushed at once, so a screen that stops the HUD frame still redraws without the line.
	if (HudOverlay *hud = overlay_()) hud->set_menu_screen_open(p_open);
}

void HudSession::set_menu_pause(bool p_paused) {
	const Ref<Simulation> sim = sim_();
	if (session_->set_menu_pause(p_paused, sim.is_valid() && sim->is_mp_session())) {
		if (GameWorld *world = world_()) world->shutdown_audio_channels();
	}
}

void HudSession::restamp_detail_() {
	if (HudOverlay *hud = overlay_()) hud->set_hud_detail_level(session_->toggles().hud_detail_level);
}

void HudSession::set_hud_detail_level(int p_level) {
	session_->set_hud_detail_level(p_level);
	restamp_detail_();
}

int HudSession::get_hud_detail_level() const { return session_->toggles().hud_detail_level; }
int HudSession::get_hud_detail_config() const { return session_->hud_detail_config(); }

void HudSession::reapply_persisted_hud_detail() {
	session_->reapply_persisted_hud_detail();
	restamp_detail_();
}

void HudSession::apply_death_screen_hud_detail() {
	hud::hud_toggles_death_screen(session_->toggles());
	restamp_detail_();
}

bool HudSession::begin_hidden_capture() {
	if (!session_->begin_hidden_capture()) return false;
	restamp_detail_();
	return true;
}

void HudSession::finish_hidden_capture() {
	if (!session_->hidden_capture_active()) return;
	session_->finish_hidden_capture();
	restamp_detail_();
}

bool HudSession::is_hidden_capture_active() const { return session_->hidden_capture_active(); }
int HudSession::get_hud_color_index() const { return session_->toggles().hud_color_index; }
int HudSession::get_friendly_tag_mode() const {
	return static_cast<int>(session_->toggles().friendly_tag_mode);
}

String HudSession::get_objective_line() const {
	return RtxtStringFile::text_to_gd(session_->objective_line());
}
String HudSession::get_banner_line() const {
	return RtxtStringFile::text_to_gd(session_->banner_line());
}
int HudSession::get_pending_message_count() const {
	return static_cast<int>(session_->pending_messages().size());
}
int HudSession::get_pending_chat_line_count() const {
	return static_cast<int>(session_->pending_chat_line_count());
}

Ref<HudChatEntry> HudSession::chat_ref_() {
	if (chat_.is_null()) chat_ = get_chat();
	return chat_;
}

void HudSession::_bind_methods() {
	ClassDB::bind_static_method("HudSession", D_METHOD("from_config", "block"),
			&HudSession::from_config);
	ClassDB::bind_method(D_METHOD("get_toggles"), &HudSession::get_toggles);
	ClassDB::bind_method(D_METHOD("get_chat"), &HudSession::get_chat);
	ClassDB::bind_method(D_METHOD("set_world", "world"), &HudSession::set_world);
	ClassDB::bind_method(D_METHOD("set_player_presenter", "presenter"),
			&HudSession::set_player_presenter);
	ClassDB::bind_method(D_METHOD("set_overlay", "overlay"), &HudSession::set_overlay);
	ClassDB::bind_method(D_METHOD("set_hud_pos", "hud_pos"), &HudSession::set_hud_pos);
	ClassDB::bind_method(D_METHOD("begin_mission", "mission_file"), &HudSession::begin_mission);
	ClassDB::bind_method(D_METHOD("end_mission", "restart"), &HudSession::end_mission);
	ClassDB::bind_method(D_METHOD("apply_mission_effects", "effects"),
			&HudSession::apply_mission_effects);
	ClassDB::bind_method(D_METHOD("frame", "controls", "stats"), &HudSession::frame);
	ClassDB::bind_method(D_METHOD("dedicated_host_frame"), &HudSession::dedicated_host_frame);
	ClassDB::bind_method(D_METHOD("take_work"), &HudSession::take_work);
	ClassDB::bind_method(D_METHOD("is_weapon_changed"), &HudSession::is_weapon_changed);
	ClassDB::bind_method(D_METHOD("get_weapon"), &HudSession::get_weapon);
	ClassDB::bind_method(D_METHOD("get_sight_slide_multiplier"),
			&HudSession::get_sight_slide_multiplier);
	ClassDB::bind_method(D_METHOD("apply_toggle_events", "events"),
			&HudSession::apply_toggle_events);
	ClassDB::bind_method(D_METHOD("escape"), &HudSession::escape);
	ClassDB::bind_method(D_METHOD("begin_end_of_round_cine"), &HudSession::begin_end_of_round_cine);
	ClassDB::bind_method(D_METHOD("chat_key", "key"), &HudSession::chat_key);
	ClassDB::bind_method(D_METHOD("is_chat_capturing"), &HudSession::is_chat_capturing);
	ClassDB::bind_method(D_METHOD("set_chat_presets", "macros"), &HudSession::set_chat_presets);
	ClassDB::bind_method(D_METHOD("set_tip_options", "keyboard_tips", "gameplay_tips"),
			&HudSession::set_tip_options);
	ClassDB::bind_method(D_METHOD("set_menu_screen_open", "open"),
			&HudSession::set_menu_screen_open);
	ClassDB::bind_method(D_METHOD("set_menu_pause", "paused"), &HudSession::set_menu_pause);
	ClassDB::bind_method(D_METHOD("set_hud_detail_level", "level"),
			&HudSession::set_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_hud_detail_level"), &HudSession::get_hud_detail_level);
	ClassDB::bind_method(D_METHOD("get_hud_detail_config"), &HudSession::get_hud_detail_config);
	ClassDB::bind_method(D_METHOD("reapply_persisted_hud_detail"),
			&HudSession::reapply_persisted_hud_detail);
	ClassDB::bind_method(D_METHOD("apply_death_screen_hud_detail"),
			&HudSession::apply_death_screen_hud_detail);
	ClassDB::bind_method(D_METHOD("begin_hidden_capture"), &HudSession::begin_hidden_capture);
	ClassDB::bind_method(D_METHOD("finish_hidden_capture"), &HudSession::finish_hidden_capture);
	ClassDB::bind_method(D_METHOD("is_hidden_capture_active"),
			&HudSession::is_hidden_capture_active);
	ClassDB::bind_method(D_METHOD("get_hud_color_index"), &HudSession::get_hud_color_index);
	ClassDB::bind_method(D_METHOD("get_friendly_tag_mode"), &HudSession::get_friendly_tag_mode);
	ClassDB::bind_method(D_METHOD("get_objective_line"), &HudSession::get_objective_line);
	ClassDB::bind_method(D_METHOD("get_banner_line"), &HudSession::get_banner_line);
	ClassDB::bind_method(D_METHOD("get_pending_message_count"),
			&HudSession::get_pending_message_count);
	ClassDB::bind_method(D_METHOD("get_pending_chat_line_count"),
			&HudSession::get_pending_chat_line_count);
	BIND_ENUM_CONSTANT(WORK_COMMAND_MAP);
	BIND_ENUM_CONSTANT(WORK_SIGHT_STATE);
}

} // namespace godot
