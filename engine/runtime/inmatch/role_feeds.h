#pragma once

// THE ROLE FEEDS (ADR 0040 ladder E8): the client-side presentation facts that
// read a running role's state — the end-of-round session state, overlay input
// and stat rows, the friendly-tags gather, and the DEATH screen's status.
// Each reads the ONE replica view every role folds (the listen host's own
// loopback included) plus the authority's session context where retail reads
// its server globals; the embedder only marshals the result into its records.
// They live in runtime/inmatch because runtime/world is net-agnostic and may
// not read ClientState.

#include <runtime/hud/end_round_overlay.h>
#include <runtime/hud/game_text_lookup.h>
#include <runtime/hud/hud_chat_entry.h> // ChatEntryFacts
#include <runtime/hud/hud_frame.h> // HudSessionState
#include <runtime/hud/hud_minimap.h> // HudMapGridOrigin
#include <runtime/hud/hud_map_view.h> // DeathMapFacts
#include <runtime/inmatch/stat_screen_feed.h>
#include <runtime/world/deploy_screen_feed.h>
#include <runtime/world/friendly_tags.h>
#include <runtime/world/lfp_feed.h>
#include <runtime/world/spawn_select.h>
#include <runtime/world/weapon_inventory.h> // WeaponSlotBarCategory

#include <array>

#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace opennova::mission {
class MissionKernel;
}
namespace opennova::hud {
struct HudScoreboardState;
}

namespace opennova::inmatch {

class ClientRuntime;
struct NapiNPServerCtx;

// What a feed reads of the running role.
struct RoleView {
	// Non-const: the friendly-tags world walk stamps its per-entity scratch.
	mission::MissionKernel *kernel = nullptr;
	// The role's replica runtime: the HostClient's on the authority, the
	// joiner's own; null on the bare local role and a dedicated host.
	const ClientRuntime *runtime = nullptr;
	// The authority's session context; null on a joiner and the bare local role.
	const NapiNPServerCtx *host = nullptr;
	bool joiner = false;
	// The host option word the embedder staged (GameConfig::mp_attributes) —
	// the session rules word of every role that is not a joiner.
	uint32_t staged_mp_attributes = 0;
};

// The client-local death-screen latch: the 0x0A flags1 bit-0 edges every
// role's view folds (the listen host's own loopback included)
// [orig: g_DeathScreenActive, NapiNPClientMsg_0x00A @0x42ff88..0x43002b].
bool local_death_screen_active(const RoleView &view);

// The one role-agnostic read of the local player's dead bit: the joiner's
// replica, the authority's entity flags (the kernel).
bool local_player_dead(const RoleView &view);

// [orig: the S2C 0x1D landing NapiNPClientMsg_0x01D @0x430840 —
//  g_SpawnSuccessGate, g_EndRoundWinnerTeam, g_ScoreTeamScore0/1,
//  g_EndRoundDrawFlag, dword_A81B2C = GetTickCount; the 0x56 board
//  completion.] Role-agnostic: every role's view folds both lanes.
EndRoundSessionState end_round_session_state(const RoleView &view);

// The overlay ladder's input off the folded 0x1D header and the round clock.
hud::EndRoundOverlayInput end_round_overlay_input(const RoleView &view);

// THE TAB BOARD'S FACTS for this client: the projected rows (their live
// entity team and class answered by the role's own entity model — the
// authority's pools, a joiner's decoded rows), the 0x16 team table and side
// count, the SU gate and the timed flag off the replica, the KOTH minutes
// (the authority's own g_TimeLimitMinutes, a joiner's session-config copy),
// the local player's team byte, and the latched flag carrier's name and team.
// Strings (title, labels, class names) stay the embedder's.
// [orig: HUD_DrawKillList @0x423a30 — T @0x423a4b..0x423a5e, the local team
//  @0x423d64; HUD_DrawGameScoreOverlay @0x423060 — the team table
//  @0x4232c5.., the carrier dword_A860C4 @0x423944..0x4239ee]
void scoreboard_feed(const RoleView &view, hud::HudScoreboardState &out);

// THE TALK KEYS' FACTS for this client (hud::ChatEntryFacts): the death
// screen and round-over latches off the replica, the session (a joiner's
// connection, the authority's is_in_session), the replica's reset hold, the
// game type's team bit, the authority bit, the local player and whether it
// rides a def-type-1 carrier, the NovaWorld network type (the embedder's —
// a joiner's runtime carries none) and the per-main-frame counter. Every
// process with a HUD in a session is a session peer.
// [orig: Input_HandleActionBinding @0x49b989..0x49badd; the vehicle walk
//  Entity_FindChildByDefType(local, 1, 0) @0x49ba38]
hud::ChatEntryFacts chat_entry_facts(const RoleView &view, bool novaworld, uint32_t frame);

// The breath bar's facts for THIS client (hud::HudFrameState breath_samples /
// breath_time / spawn_success_gate; the label is the embedder's gametext).
// The samples ride every role's replica (the listen host's loopback included);
// the breath seconds are the authority's own WAC named value on a host and the
// 0x0A sub-block-1 copy on a joiner; the round-over latch is the folded 0x1D
// header. [orig: HUD_DrawBreathBar @0x59D6F0 reads word_A85B7C and
// g_WacVarBreathTime; HUD_DrawGameplayOverlays skips it while
// g_SpawnSuccessGate @0x5BDECA..0x5BDED1; the 0x1D latch
// NapiNPClientMsg_0x01D @0x430840]
struct BreathBarFacts {
	int samples = 0;
	int breath_time = 20;
	bool spawn_success_gate = false;
};
BreathBarFacts breath_bar_facts(const RoleView &view);

// THE HUD'S ROLE FACTS, one read per display frame: the breath bar's, the MP
// session lines' (hud::HudSessionState — its gametext strings are the
// embedder's hud_session_text) and the HUDLS slot bar's per-category scan with
// each category's first def's hud_loadout_select texture name (the device
// loads it). Every field cites its retail global at the fill.
struct HudRoleFacts {
	BreathBarFacts breath;
	hud::HudSessionState session;
	std::array<world::WeaponSlotBarCategory, 10> slot_bar{};
	std::array<std::string, 10> slot_bar_icons;
	// The F9 / F10 voice-macro menus the caller asked for (their open words;
	// the frame compiler applies the death-screen gate): each row's key is
	// the local player's voice-macro name (flags 0xC for the emotes, 6 for the
	// radio) and its text the vmacros macrotext entry, the key itself on a
	// miss; the titles are macrotext EMOTES_TITLE / RADIO_TITLE, else the
	// literal fallbacks. No local player resolves nothing.
	// [orig: HUD_DrawEmotesMenu @0x5bff00 (sub_5BFB00(buf, i, 0xC, local)
	//  @0x5bff9e, the title @0x5bff25..0x5bff57); HUD_DrawRadioTitleMenu
	//  @0x5bfb90 (sub_5BFB00(buf, i, 6, local) @0x5bfc2e, the title
	//  @0x5bfbb5..0x5bfbe7); the dword_24C1930 & 0x10000 arms are dead: no
	//  writer sets that bit]
	hud::HudVoiceMacroMenuState emotes_menu;
	hud::HudVoiceMacroMenuState radio_menu;
};
// `voice_menus`: bit 0 asks for the emotes menu, bit 1 for the radio menu.
inline constexpr uint32_t kHudVoiceMenuEmotes = 1u;
inline constexpr uint32_t kHudVoiceMenuRadio = 2u;
HudRoleFacts hud_role_facts(const RoleView &view, uint32_t voice_menus = 0);

// The stat.mnu RESULTLIST rows the tab filter admits (0 all, 1 team 2, 2 team
// 1): the roster joined to the frozen board, the local row resolved from the
// header's board index [orig: StatScreen_PopulateStatResultsList @0x562240 — row slot
// store @0x562576, board join @0x5624F3, selection compare @0x56272E].
std::vector<StatScreenRow> end_round_rows(const RoleView &view, int tab);

// The friendly-tags gather (D-HUD-20): the world walk with the role's pass
// facts, plus a joiner's roster walk. False when there is no local player
// (nothing gathered). The witnessed pass is cited at world/friendly_tags.
bool collect_friendly_tags(const RoleView &view, std::vector<world::FriendlyTagSource> &out);

// The DEATH screen's STATIC facts for THIS client [orig: the client globals
// UI_UpdateDeathScreenContent @0x5536a0 reads — dword_A85B5C / A85B60 / A85B68
// from the 0x0A sub-block 0, word_A85BC0 + entity+538/548 from the 0x6E fold].
// `zones` is the mission's spawn-zone list, `medic_key_label` the MedicReq
// binding's display string the embedder resolves.
world::DeployScreenStatus deploy_screen_status(const RoleView &view,
		const world::SpawnZoneRegistry &zones, const std::string &medic_key_label,
		const hud::GameTextLookup &gametext);

// The map grid-label origin: the mission's first type-2043 marker. The
// authority stashes it at promotion from the mission doc; a JOINER promotes
// a marker-less wire-header BMS (D-NET-194), so its origin resolves from the
// replicated pool-3 entity in the decoded view instead — the same
// client-side pool scan retail's HUD init runs (witness at
// World::map_grid_origin_x / replication::client_minimap_grid_origin).
hud::HudMapGridOrigin hud_map_grid_origin(const RoleView &view);

// THE JOINER'S READINESS the parity harness classifies (the WITNESSED
// deploy-hold split, docs/net/novaworld-net-re.md §5.61: the client enters its
// protocol InMatch phase and owns a live motor after the first loadout grant
// so its pre-pick C2S 0x0C flows, while the player-paced DEATH screen holds
// presentation). Both are false on every role but a live joiner.
// The deploy hold: the pick is the player's (AwaitDeployPick) on a bound self
// handle, no join error, the session alive — the embedder ANDs its DEATH
// screen's presented state.
bool joiner_deploy_hold_ready(const RoleView &view);
// In match: a local player in the protocol's InMatch phase; an auto-deploy run
// also waits for the pick to clear.
bool joiner_in_match_ready(const RoleView &view, bool auto_deploy);

// The DEATH screen's zone rows [orig: UI_UpdateDeathScreenContent @0x5536a0 —
// def present, team match, SECURED (a numbered zone lists only at full control:
// the zone-timer EntryById[9] >= [10] gate), attrib 0x40000; letter = 'A' +
// registry index, name = WPNames/STRWPNAME%03d(index+1)]. The local BMS owns
// membership/letter identity; live S2C 0x6F/0x53 owns team + control. Every
// TEAM zone is emitted with its `secured` verdict: the second (occupant) loop
// of the populate has no secured gate, so the list builder
// (world/deploy_screen_feed.h) decides which rows list and where the
// occupants land. A joiner's feed: false (nothing emitted) on every other role.
bool deploy_zone_rows(const RoleView &view, const world::SpawnZoneRegistry &zones,
		std::vector<world::DeployZoneRow> &out);

// The AAS zone status panel's zone rows: the world's zone walk
// (world/lfp_feed.h) with the role's zone-timer image — the client runtime's
// 13-DWORD image of the retail shared timer list, present once a 0x6F value
// has arrived, which is when retail's CProximityList_FindEntryById @0x537f50
// (called from HUD_DrawZoneMarker @0x598719) finds one — and the transient / special minimap slot's flag byte (+4 &
// 0xC0 gates the marker; retail walks the 1160-slot transient bank
// @0x5a2517..0x5a256e; the 0x6B ring slots land in the special bank here).
// False without a kernel, a replica runtime or a local player.
bool collect_lfp_zones(const RoleView &view, const world::SpawnZoneRegistry &zones,
		int local_team, std::vector<hud::HudLfpZone> &out);

// The DEATH MAP window's world facts (hud_map_view.h DeathMapFacts): the
// local player (position, team +354), the deploy-overlay latch, the
// spawn-target hold, the game type, the local player's wave zone, the
// spawn-zone AABB, and one row per registered spawn zone — its team (the
// zone-timer image's owner once a 0x6F value has landed, else the entity's),
// the ready verdict of the timer entry the letter gate reads (no entry, or
// DWORD 9 >= DWORD 10 — the same image collect_lfp_zones reads), the label
// anchor and the 0x6E queued count / countdown. False without a kernel.
// [orig: MapOverlay_DrawView @0x5a58e0 — CProximityList_FindEntryById
//  @0x5a5b6e, EntryById[9] >= [10] @0x5a5b77; the anchor sub_59C300
//  @0x59c300; entity+550/+548 from the 0x6E fold @0x429880]
bool death_map_facts(const RoleView &view, const world::SpawnZoneRegistry &zones,
		hud::DeathMapFacts &out);

// THE PER-DRAWN-ENTITY LIGHTING FEED (D-RLIT-3 plus the interior lerp).
// Retail pushes a render-state stack level around every drawn entity's
// submits [orig: Terrain_RenderSectorEntities @0x5c7bb6..0x5c7c14,
// Terrain_RenderSectorEntitiesBySide @0x5c7f3c..0x5c7fc3]:
//  - effectScale, the sun-visibility factor [orig: Terrain_SetupEffectForEntity
//    @0x5c74a0 -> Entity_ComputeSunVisibility @0x5c6800, stack write @0x5c7bff];
//    a CONTAINED entity (+0x1D0, the first blink hit, nonzero) skips the rays,
//    keeps 1.0 and takes the containing building's interior light group
//    [orig: @0x5c74ae..0x5c74f3];
//  - the submit flag 0x80 for a contained entity (`neg/sbb/and 80h` @0x5c7c05..
//    0x5c7c14 and @0x5c7fb6..0x5c7fc3), which the batch collectors turn into
//    entry bit 1, the interior lerp [orig: @0x5d962f..0x5d963b skinned,
//    @0x5d9167..0x5d916e rigid];
//  - the aux daylight t = the containing building's ItemDef+0x218
//    (light_transfer / 100), loaded ONLY by the person wave for a contained
//    person [orig: @0x5c7f83..0x5c7f93]. The non-person wave never writes it,
//    so a contained vehicle or prop lerps with the stack base 0, which
//    RenderBatchCtx_BeginFrame zeroes [orig: @0x5d89b6..0x5d89b8]: floor and
//    ceiling only, no sun.
// The blocked-ray count and eligibility gate are the collision world's; this
// feed mirrors both drawn identity domains (placed rows by BMS id,
// wire-rendered rows by handle) and diffs the context per identity so a
// steady frame emits nothing. Unlisted entities are outdoor quality 4.
struct EntityLighting {
	uint8_t quality = 4;          // 1..4 sun quality; 4 when contained
	bool interior = false;        // contained: the submit flag 0x80
	float light_transfer = 0.0f;  // the aux daylight t (persons only)
	int32_t interior_bms = 0;     // the containing building's BMS id (0 = none)
	int32_t interior_section = 0; // the containing blink volume's section
	bool operator==(const EntityLighting &o) const {
		return quality == o.quality && interior == o.interior &&
				light_transfer == o.light_transfer && interior_bms == o.interior_bms &&
				interior_section == o.interior_section;
	}
	bool operator!=(const EntityLighting &o) const { return !(*this == o); }
};
struct EntityLightingChange {
	bool wire = false; // false: `bms_id` names a placed row; true: `handle` a wire row
	int32_t bms_id = 0;
	uint16_t handle = 0;
	EntityLighting lighting;
};
struct EntityLightingFeed {
	// Per-entity context last emitted, split by identity domain: wire handle
	// zero and a placed BMS id zero are both valid sentinels in their own
	// schemas, so they never share one integer map.
	std::unordered_map<int32_t, EntityLighting> last_by_bms;
	std::unordered_map<uint16_t, EntityLighting> last_by_wire;
	int64_t layout_revision_seen = -1;
	// The local player's own sun quality (a spawned entity outside the placed
	// walk): the presenter seam's only — the FP parts keep the witnessed
	// effectScale=1 exemption while the third-person body dims.
	uint8_t local_quality = 4;

	// One display frame. `sun_step_q16` is light_dir * 200 u in mission fixed
	// [orig: end = start + 200 * lightdir @0x5c6858..0x5c6876]; `culled_bms` /
	// `culled_wire` are the occlusion pass's culled identities (retail only rays
	// a drawn entity: a culled one keeps its last factor until it renders
	// again); a new `layout_revision` forgets the wire-domain cache.
	void collect(const RoleView &view, const int32_t sun_step_q16[3],
			const std::unordered_set<int32_t> &culled_bms,
			const std::unordered_set<int32_t> &culled_wire, int64_t layout_revision,
			std::vector<EntityLightingChange> &out);
};

} // namespace opennova::inmatch
