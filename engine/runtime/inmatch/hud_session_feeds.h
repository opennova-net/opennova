#pragma once

// THE HUD SESSION'S ROLE FEEDS: what the HUD session (hud/hud_session.h) reads of the running
// role - the connection mode's gates its key rows and windows take, the world's mission effects
// it routes, and the map feeds a joiner's world stream can deliver after the HUD builds (the
// grid-label origin, the static footprints), retried until they appear. They live in
// runtime/inmatch because they read the role's view; runtime/hud never includes inmatch.

#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_session.h>
#include <runtime/inmatch/role_feeds.h>
#include <runtime/inmatch/session.h>

#include <cstdint>

namespace opennova::world {
struct Effect;
}

namespace opennova::inmatch {

// The HUD's gates by role: whether a session is live (g_NapiNPCtx.is_in_session), the
// connection mode's two bits (single player and a listen host carry both, a joiner only the peer
// bit, a dedicated host only the authority bit; HudKeyPoll carries the witness) and the
// authority's round-over latch.
struct HudSessionRole {
	bool in_session = false;
	bool authority = false;
	bool mp_session_peer = false;
	bool round_over = false;
};
HudSessionRole hud_session_role(RoleKind kind, const RoleView &view);

// One world EffectLog entry as the HUD routes it (hud_mission_effect over its kind).
hud::HudMissionEffectRecord hud_mission_effect_record(const world::Effect &effect);

// The escape key over live play: the HUD-window close chain over the role's gates (the round-over
// latch makes it a no-op out of a session; hud_toggles_escape carries the witness). Returns the
// event bits.
uint32_t hud_session_escape(hud::HudSession &session, const HudSessionRole &role);

// THE GRID-LABEL ORIGIN (the mission's type-2043 marker entity): the authority resolves it at
// promotion, a joiner's decodes from the world stream after the HUD builds, so the query repeats
// until it appears, throttled (HudSession::map_grid_origin_due) unless `force` (the overlay
// system's init queries at once). The origin lands in the frame state's minimap block.
// [orig: HUD_InitOverlaySystem @0x5a4999 pool scan]
void hud_session_grid_origin_feed(hud::HudSession &session, const RoleView &view,
		int64_t now_ticks, bool force, hud::HudFrameState &state);

// THE STATIC FOOTPRINTS: the building and zone outlines baked once per mission
// (build_minimap_footprints), latched on the first non-empty gather and retried at most once per
// throttle interval until then (a joiner's statics can decode after the HUD builds; a mission
// with no footprint statics must not walk the registry every frame).
void hud_session_footprints_feed(hud::HudSession &session, const RoleView &view,
		int64_t now_ticks, hud::HudFrameState &state);

} // namespace opennova::inmatch
