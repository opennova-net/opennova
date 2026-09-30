// The key-toggled overlay windows' setters (engine runtime/hud/
// hud_overlay_windows.h): the F1 help page, the F12 map legend, the I
// briefing panel and its page keys. The shell resolves the text tables;
// the compiler owns the layout and the briefing's page state.
#include "hud/hud_overlay.h"
#include "simulation/simulation.h"
#include "util/string_convert.h"

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

} // namespace godot
