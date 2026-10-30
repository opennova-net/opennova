// The HUD's frame feeds (inmatch/hud_frame_feeds.h) over a session with no role (the bare
// feed source: no kernel, no replica, no zones): the zone panel's strings, game type and team
// with no rows, the takeover dialog composed from no entry; the friendly tags' fog and
// speaking words with no tags; the role facts' Overlays strings and the slot bar's loads by
// the HUDLS system word.
// [orig: HUD_DrawZoneStatusPanel @0x5A2480; HUD_DrawFriendlyTagsPass @0x5A4480;
//  HUD_DrawWeaponSlotBar @0x599E5A]
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_viewport.h>
#include <runtime/inmatch/hud_frame_feeds.h>

using namespace opennova;
using namespace opennova::hud;
using namespace opennova::inmatch;

static int failures = 0;
#define CHECK(x)                                                                                   \
	do {                                                                                           \
		if (!(x)) {                                                                                \
			std::fprintf(stderr, "FAIL line %d: %s\n", __LINE__, #x);                              \
			++failures;                                                                            \
		}                                                                                          \
	} while (0)

namespace {

std::string overlays(const char *section, const char *key, const char *fallback) {
	if (std::strcmp(section, "Overlays") == 0) return std::string("[") + key + "]";
	return fallback;
}

void lfp_panel() {
	HudFrameState state;
	state.lfp_panel.zones.resize(2);
	const GameTextLookup none = [](const char *, const char *, const char *fallback) {
		return std::string(fallback);
	};
	hud_lfp_panel_feed(HudFeedSource{}, true, 0x50010u, 2, none, state);
	CHECK(!state.lfp_panel.shown);
	CHECK(state.lfp_panel.zones.empty());
	CHECK(state.lfp_panel.game_type == 0x50010u);
	CHECK(state.lfp_panel.local_team == 2);
	CHECK(state.lfp_panel.under_attack_text == "!Under\nAttack!!");
	hud_lfp_panel_feed(HudFeedSource{}, false, 0u, 1, GameTextLookup(overlays), state);
	CHECK(state.lfp_panel.under_attack_text == "[STROVER_UNDERATTACK]");
	CHECK(state.lfp_panel.ready_text == "[STROVER_READYFORTAKEOVER]");
}

void friendly_tags() {
	HudFrameState state;
	state.friendly_tags.resize(3);
	HudViewBlock block;
	block.fov_q16 = 90 * 65536;
	block.right = 1023;
	block.bottom = 767;
	const HudViewport viewport = hud_viewport_build(block, 1.0f, 1024, 768);
	hud_friendly_tags_feed(HudFeedSource{}, true, viewport, 100.0f, state);
	CHECK(state.friendly_tags.empty());
	CHECK(state.fog_dist_q16 == 100 * 65536);
	CHECK(state.speaking_level == 0);
	hud_friendly_tags_feed(HudFeedSource{}, true, viewport, 0.0f, state);
	CHECK(state.fog_dist_q16 == INT32_MAX); // no fog distance, no fog cull
}

void role_facts() {
	HudFrameState state;
	std::array<std::string, 12> keys;
	keys.fill("K");
	HudRoleFeed feed;
	hud_role_facts_feed(HudFeedSource{}, 0, GameTextLookup(overlays), 0, keys, state, feed);
	CHECK(state.paused_text == "[STROVER7]");
	CHECK(state.breath_label == "[STROVER91]");
	CHECK(state.connect_status.label == "[STROVER54]");
	CHECK(state.start_timer.label == "[STROVER59]" && state.start_timer.footer == "[STROVER63]");
	CHECK(!feed.slot_bar_loads);
	hud_role_facts_feed(HudFeedSource{}, 0, GameTextLookup(overlays), 1, keys, state, feed);
	CHECK(feed.slot_bar_loads);
	// No role scans no slot: every category is empty and names no icon.
	for (size_t c = 0; c < state.slot_bar.size(); ++c) {
		CHECK(!state.slot_bar[c].present);
		CHECK(feed.slot_bar_icons[c].empty());
	}
}

} // namespace

int main() {
	lfp_panel();
	friendly_tags();
	role_facts();
	if (failures != 0) {
		std::fprintf(stderr, "%d failure(s)\n", failures);
		return EXIT_FAILURE;
	}
	std::puts("hud_frame_feeds_test: ok");
	return EXIT_SUCCESS;
}
