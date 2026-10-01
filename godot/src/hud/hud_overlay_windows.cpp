// The key-toggled overlay windows' setters (engine runtime/hud/
// hud_overlay_windows.h): the F1 help page, the F12 map legend, the I
// briefing panel and its page keys. The shell resolves the text tables;
// the compiler owns the layout and the briefing's page state.
#include "hud/hud_overlay.h"
#include "mnu/controls_model.h"
#include "rtxt/rtxt_string_file.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

#include <runtime/controls/binding_set.h>
#include <runtime/hud/hud_server_status.h>
#include <runtime/hud/tip_system.h>

#include <utility>

#include <godot_cpp/classes/viewport.hpp>

namespace godot {

void HudOverlay::set_help_screen(bool p_shown, const String &p_title, const String &p_page_line,
		const String &p_footer, const PackedStringArray &p_keys, const PackedStringArray &p_texts) {
	opennova::hud::HudHelpScreenState &hs = state_.help_screen;
	hs.shown = p_shown;
	hs.title = opennova::to_std(p_title);
	hs.page_line = opennova::to_std(p_page_line);
	hs.footer = opennova::to_std(p_footer);
	hs.keys.clear();
	hs.texts.clear();
	for (int64_t i = 0; i < p_keys.size(); ++i) hs.keys.push_back(opennova::to_std(p_keys[i]));
	for (int64_t i = 0; i < p_texts.size(); ++i) hs.texts.push_back(opennova::to_std(p_texts[i]));
}

void HudOverlay::set_map_legend(bool p_shown, const String &p_title,
		const PackedStringArray &p_labels, int p_frame_counter) {
	opennova::hud::HudMapLegendState &ml = state_.map_legend;
	ml.shown = p_shown;
	ml.title = opennova::to_std(p_title);
	ml.labels.clear();
	for (int64_t i = 0; i < p_labels.size(); ++i) ml.labels.push_back(opennova::to_std(p_labels[i]));
	ml.frame_counter = p_frame_counter;
}

PackedStringArray HudOverlay::map_legend_keys() {
	PackedStringArray keys;
	for (const opennova::hud::HudMapLegendIcon &icon : opennova::hud::kHudMapLegendIcons)
		keys.push_back(String(icon.name));
	return keys;
}

void HudOverlay::set_briefing(bool p_shown, const Ref<Simulation> &p_sim) {
	state_.briefing.shown = p_shown;
	state_.briefing.text = p_shown && p_sim.is_valid() ? p_sim->mission_briefing_text() : std::string();
}

void HudOverlay::cycle_briefing_page(int p_direction, bool p_in_session) {
	compiler_.briefing_pages().cycle(p_direction, p_in_session);
}

void HudOverlay::set_tip(int p_tip, int p_countdown, bool p_local_dead,
		const Ref<RtxtStringFile> &p_gametext, const Ref<ControlsModel> &p_controls) {
	state_.tip = p_tip;
	state_.tip_countdown = p_countdown;
	state_.local_dead = p_local_dead;
	if (p_countdown <= 0) {
		state_.tip_header.clear();
		state_.tip_body.clear();
		return;
	}
	const opennova::hud::TipKeyDisplay keys = [&p_controls](const std::string &token) {
		return p_controls.is_valid()
				? opennova::controls::display_string_for_token(p_controls->native_bindings(), token)
				: std::string("???");
	};
	opennova::hud::tip_draw_text(p_tip, game_text_lookup(p_gametext), keys, state_.tip_header,
			state_.tip_body);
	queue_redraw();
}

void HudOverlay::set_quit_dialog(bool p_open, bool p_in_session, bool p_authority,
		const Ref<RtxtStringFile> &p_gametext) {
	if (!p_open && !state_.quit_dialog_open) return;
	state_.quit_dialog_open = p_open;
	state_.quit_dialog_text = p_open
			? opennova::hud::game_text(game_text_lookup(p_gametext), "Overlays",
					  opennova::hud::quit_dialog_text_key(p_in_session, p_authority), "")
			: std::string();
	queue_redraw();
}

void HudOverlay::set_server_status_page(bool p_shown, bool p_score_list_open,
		const Ref<RtxtStringFile> &p_gametext, const Ref<Simulation> &p_sim) {
	// Only an authority draws the page (hud_server_status.h: the render gate
	// reads the authority bit beside the view word); the fill is its source.
	const bool shown = p_shown && p_sim.is_valid() && p_sim->has_server_status_page();
	const bool opened = shown && !server_status_shown_;
	if (shown != server_status_shown_) {
		server_status_shown_ = shown;
		// The scene frame is not drawn while the page stands in for it: the
		// viewport's 3D world stops rendering (the device leg of retail's
		// skipped Render_ProcessMainSceneFrame), and comes back as it was.
		if (Viewport *viewport = get_viewport()) {
			if (shown) scene_3d_was_disabled_ = viewport->is_3d_disabled();
			viewport->set_disable_3d(shown || scene_3d_was_disabled_);
		}
	}
	if (!shown) return;
	// The page draws only at its throttle and the frames between present the
	// last one, so its facts are gathered for the draw that is due (and when
	// the page opens), not every frame.
	// [orig: Server_DrawStatusScreen @0x50a31a..0x50a334 — the 200 ms /
	//  inactive 10 s gates ahead of every read of the page]
	uint32_t now_ms = 0;
	bool window_active = true;
	server_status_clock_(now_ms, window_active);
	if (!opened && !compiler_.server_status_page_due_now(now_ms, window_active)) return;
	opennova::hud::ServerStatusPageState page;
	p_sim->fill_server_status_page(page);
	server_status_page_ = std::move(page);
	server_status_page_.score_list_open = p_score_list_open;
	server_status_page_.text = opennova::hud::server_status_text(
			game_text_lookup(p_gametext), server_status_page_.game_type);
	queue_redraw();
}

} // namespace godot
