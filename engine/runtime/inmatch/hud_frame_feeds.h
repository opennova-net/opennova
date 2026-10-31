#pragma once

// THE HUD'S FRAME FEEDS: what the in-game HUD reads of the running role each display frame, put
// straight into its frame state (hud::HudFrameState) for the overlay pass to draw. Each reads the
// role's facts (role_feeds.h, takeover_status_feed.h) and composes them with the gametext
// strings retail's drawers read; the embedder only hands its frame state over, then loads the
// textures the feed names. They live in runtime/inmatch because the facts read the role's
// replica view; the HUD-side half (the projection, the strings) is runtime/hud's.

#include <array>
#include <cstdint>
#include <string>

#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_viewport.h>
#include <runtime/inmatch/role_feeds.h>

namespace opennova::inmatch {

// What the feeds read of the running session: the role view (a joiner's seat scan reads its
// occupancy, RoleView::occupancy) and the mission's spawn-zone registry (the zone letters and
// pick indices).
struct HudFeedSource {
	RoleView view;
	const world::SpawnZoneRegistry *zones = nullptr;
};

// THE AAS ZONE STATUS PANEL and THE TAKEOVER DIALOG off one zone push: the panel's game type
// (its conquest arm), the viewer's team, its two status strings and, while `shown` with a local
// player, the zone rows (collect_lfp_zones); the dialog's nearest running entry
// (collect_takeover_status) composed with the gametext and the team. The marker's blink reads
// the frame's HUD frame counter, stepped once per logic tick outside the pause and the epilog
// screen [orig: Game_ProcessMainFrame @0x5265d5 -> Game_TickHudFrameCounters @0x434c23].
void hud_lfp_panel_feed(const HudFeedSource &source, bool shown, uint32_t game_type,
		int local_team, const hud::GameTextLookup &gametext, hud::HudFrameState &state);

// THE OVERHEAD FRIENDLY TAGS (D-HUD-20): the fog cull distance (16.16; none past a zero or
// negative distance) and the speaking pulse's level (the mixer meter's first stage) every frame,
// then, `shown` over a valid viewport with a mode other than OFF, the role's gather
// (collect_friendly_tags) projected through `viewport` (hud::project_friendly_tags). Otherwise
// the tags are cleared.
// [orig: HUD_DrawFriendlyTagsPass @0x5A4480 -> HUD_DrawEntityLabel @0x5A39B0 - the fog
//  g_EnvFogDistCurrent @0x5A3B28; the speaking pulse reads the mixer meter's first stage]
void hud_friendly_tags_feed(const HudFeedSource &source, bool shown,
		const hud::HudViewport &viewport, float fog_distance_units, hud::HudFrameState &state);

// What the role-facts feed hands back besides the frame state: the weapon inset's friendly name
// and the title's Inset reticle quad (the combat feed's target name and reticle, set into the
// combat state each frame) and, while the layout's HUDLS_SYSTEM draws the slot bar, the icon each
// category's slot loads (its first def's hud_loadout_select texture, in the HUD loader's alpha
// mode; "" for an empty category).
struct HudRoleFeed {
	std::string inset_target_name;
	bool inset_reticle_quad = false; // HudRoleFacts::inset_reticle_quad (DFX2-45)
	bool slot_bar_loads = false;
	std::array<std::string, 10> slot_bar_icons;
};

// THE HUD'S ROLE FACTS (hud_role_facts) into the frame state: the open F9 / F10 menus' rows,
// the breath bar with its Overlays/STROVER91 label, the pause text (STROVER7), the MP session
// lines with their strings (hud::hud_session_text, by the session's game type), the squad
// orders, the connection indicators, the frame-rate overlay, the connection status line
// (STROVER54) and the start timer (STROVER59 / STROVER63), the EXPPOINTS element with its strings
// (hud::hud_exp_points_text), the session's title and the lone-gunner fact, the vehicle panel's
// rider lines with their weapon texts (hud::hud_vehicle_rider_text), and the HUDLS slot bar's
// scan with each slot's key label (`slot_bar_key_labels`, binding record 200 + the def's
// category) while `hudls_system` draws the bar.
void hud_role_facts_feed(const HudFeedSource &source, uint32_t voice_menus,
		const hud::GameTextLookup &gametext, uint32_t hudls_system,
		const std::array<std::string, 12> &slot_bar_key_labels, hud::HudFrameState &state,
		HudRoleFeed &out);

} // namespace opennova::inmatch
