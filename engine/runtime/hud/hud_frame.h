#pragma once

// The HUD frame compiler (ADR 0033 R2): one compile turns the per-frame HUD
// state + the hudpos.def layout into a typed draw list — screen-space quads,
// lines, and game-font glyph quads — in the witnessed element order. The
// embedder keeps texture upload and rasterization only.
// [orig: HUD_BuildEntityInfo @ 0x4b8440 -> HUD_RenderAllOverlays @ 0x5a8070
//  -> HUD_RenderOverlays @ 0x5a7bb0 -> the per-element draws]
// Witness record: docs/interface/hud-re.md; per-element policy math lives in
// hud/hud_math.h and stays the single source.

#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_declutter.h>
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_scoreboard.h>
#include <runtime/hud/hud_minimap.h>

#include <array>
#include <cstdint>
#include <string>

#include <runtime/hud/hud_vehicle_panel.h> // the seat-marker policy
#include <vector>

namespace opennova::hud {

// Texture slots the embedder registers at configure time; draw-list quads
// reference these (negative = untextured fill/wireframe).
enum HudTexture : int32_t {
	kHudTexNone = -1,
	kHudTexFrame = 0,
	kHudTexCrosshair,
	kHudTexClipGfx,
	kHudTexRoundGfx,
	kHudTexStance0, // six consecutive stance frames
	kHudTexStance1,
	kHudTexStance2,
	kHudTexStance3,
	kHudTexStance4,
	kHudTexStance5,
	kHudTexMapTerrain,
	kHudTexMapWater,
	kHudTexMapIcons,
	kHudTexMapCompass,
	kHudTexMapRadar,
	kHudTexMapWpIndicator, // WPIndctr.tga [orig: HUD_LoadAllTextures @0x59e079]
	// The Tab board's stdbox: the 4x4 border stencil atlas and the tiled
	// interior brush [orig: the panel call @0x423a72 -> HUD_DrawLabelBox
	// @0x51efd0 -> render_hud_box_overlay @0x56b700, "stdbox" registered
	// @0x51effa]. Inserted BEFORE the sights
	// sentinel, which sizes the device slot array.
	kHudTexBoxBorder,
	kHudTexBoxTile,
	// The 16x16 connection-quality icon, a 4-row vertical atlas
	// [orig: the quad @0x4241fb; the atlas load CNetworkIcons_LoadTextures @0x4c2cf0].
	kHudTexNetIcon,
	// The mounted-vehicle panel silhouette (the block's `interface` texture).
	kHudTexVehiclePanel,
	// The AAS zone status panel's three team icons — 64x256 vertical 4-frame
	// atlases [orig: HUD_LoadAllTextures @0x59dda0 — JO_LFP.tga -> dword_27231A4
	//  (team 1), R_LFP.tga -> dword_27231A0 (team 2), N_LFP.tga -> dword_272319C
	//  (neutral); selected @0x598945..0x598957 in HUD_DrawZoneMarker].
	kHudTexLfpTeam1,
	kHudTexLfpTeam2,
	kHudTexLfpNeutral,
	// The marker's 36x36 team tile behind the letter: one texture for the
	// viewer's own zones, another for everyone else's [orig: the pick
	//  @0x5989b9..0x5989d1 — textureId @0x27239D4 for team == local,
	//  dword_27239C4 otherwise; the latter is lfp_alf.tga, HUD_LoadAllTextures
	//  @0x59e10e]. WITNESS PENDING: the loader of textureId @0x27239D4 (no
	//  visible writer in HUD_LoadAllTextures' xrefs) — an unresolved slot
	//  draws no tile, the same degradation as the silhouette.
	kHudTexLfpTileOwn,
	kHudTexLfpTileOther,
	kHudTexSightsBase, // authored SIGHTS rows: kHudTexSightsBase + row index
};

// THE STDBOX GEOMETRY, as raw retail numbers. The border pieces and the fill
// inset scale with the surface by s = surface_w / 1600 [orig: the scale
// 0.000625 double @0x51f02e in HUD_DrawLabelBox]; the fill's tile PERIOD does
// not scale — it is the atlas cell's own size (texW/4, 32 px for the shipped
// 128 px border.tga), because retail's fill is one wrap-addressed quad of the
// EXTRACTED cell (3,0) with UV = (screen_px + 0.5) / cell
// [orig: stdbox_draw_fill_wrap_tiled @0x56b5d0; the extraction + zeroing of
// the source cell @0x56adbd-0x56ae44]. Insets 16*s / 24*s are the ctor's
// literals [orig: the 16/24 stores @0x56b342/@0x56b351, read as rec+0x180 /
// rec+0x184 by the fill arm @0x56b7bd-0x56b80d]; the bottom-row crop is 0.9
// of BOTH dest and source [orig: the flag arm @0x56b454-0x56b470,
// flt_7C459C = 0.9].
inline constexpr float kBoxScaleRef = 1600.0f;
inline constexpr float kBoxFillInsetX = 16.0f;
inline constexpr float kBoxFillInsetY = 24.0f;
inline constexpr float kBoxBottomCrop = 0.9f;
// The titled top row's gap rule: the notch behind the title is the measured
// bold title width + 2, less 12*s once it exceeds 12*s
// [orig: HUD_DrawLabelBox @0x51f0ea-0x51f114].
inline constexpr float kBoxTitlePad = 2.0f;
inline constexpr float kBoxTitleTrim = 12.0f;

// THE hudpos.def StaticFrame BACKGROUND. Retail keeps exactly ONE, and not in
// an array: the handler copies the texture name into a single name buffer and
// the position into two words, so every dispatched StaticFrame line OVERWRITES
// the one before it and the LAST authored line is the one that draws
// [orig: HUD_ParseHudposToken @0x59F370 — the name copy @0x5a0a4e-0x5a0a62,
//  the x store @0x5a0a72, the y store @0x5a0a8a].
//
// Our def parser keeps every line so the writer round-trips, which makes the
// pick a policy the consumer has to apply: taking entry 0 draws a frame retail
// never shows when a def authors more than one. The name is only loaded when
// non-empty [orig: the strlen skip @0x59df41-0x59df5b] and the draw is gated on
// the slot having resolved [orig: the test @0x5a7c69], so an unresolvable name
// draws nothing rather than falling back.
//
// Returns the index to draw, or -1 when nothing is authored.
inline int hud_static_frame_index(int authored_count) {
	return authored_count > 0 ? authored_count - 1 : -1;
}

struct HudQuad {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
	uint32_t color = 0xFFFFFFFFu; // 0xAARRGGBB
	int32_t texture = kHudTexNone;
	bool filled = true;    // false = 1px wireframe rect
	bool additive = false; // SIGHTS add-blend rows
};

// One textured triangle (the crosshair's tapered arm strips)
// [orig: HUD_DrawCrosshairCornerQuad @ 0x590f50].
struct HudTriVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct HudTri {
	HudTriVertex a;
	HudTriVertex b;
	HudTriVertex c;
	uint32_t color = 0xFFFFFFFFu;
	int32_t texture = kHudTexNone;
};

struct HudLine {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float width = 1.0f;
	uint32_t color = 0xFFFFFFFFu;
};

// The authored 4-field hudpos position record (x, y, hidden, align).
struct HudPosRecord {
	int x = 0;
	int y = 0;
	int hidden = 0;
	int align = 0;
	bool present = false;
};

struct HudRectRecord {
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;
	bool present = false;
};

struct HudSightsRow {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	bool additive = false;
	bool texture_valid = false;
};

// The layout globals parsed once from hudpos.def (the embedder resolves
// texture names to the slots above and hands the parsed .fnt).
// [orig: the dword_27237xx.. layout global block, HUD_ParseHudposToken @0x59f370 parse]
struct HudLayout {
	HudPosRecord ammo_count;
	HudPosRecord weapon_name;
	HudPosRecord game_info;
	HudPosRecord wpd_info;
	HudPosRecord chat_text;
	// The chat box's x1/x2 columns — g_hudChatBoxCoords rows 1 and 2, the
	// source of the chat wrap width `x2 - (x1 - 4)` [orig: HUD_GetChatBoxCoord
	//  @0x5bbe90 reads dword_28E4DF8[index]; Chat_AddMessageChannel1 reads
	//  rows 2 and 1 @0x498673/@0x498688]. The table's ONLY writer is the
	// hud.def line parser: `chat_message x1 y1 x2 y2` stores rows 1/2 (and
	// the y pair to dword_28E51FC/dword_28E5200), `sys_message` rows 3/4
	// [orig: the File_ParseASCIIFile("hud.def", cb, 0x2A5A8EAD) registration
	//  @0x5be210..0x5be228; the callback @0x5bb7a0 (no function boundary in
	//  the IDB), stores @0x5bb7d1/@0x5bb7ed/@0x5bb825/@0x5bb841]. JO:CA ships
	// NO hud.def (none in resource/localres/language.pff), so on the retail
	// title the rows stay 0: the width is 4 and HUD_WordWrapText returns 1 at the
	// first character (@0x5809ea..0x5809fc, no space yet) — a retail chat
	// line NEVER wraps. These are NOT the HUDCHATTEXT pair (that is the feed
	// anchor dword_27237A8/AC): a layout builder must leave them absent
	// unless it parsed a hud.def, and absent, a line is never wrapped.
	int chat_box_x1 = 0;
	int chat_box_x2 = 0;
	bool chat_box_present = false;
	// HUDSYSTEXT — the SYSTEM feed anchor (kills, joins, system lines). The
	// def parser already produces it (def_hudpos.cpp HUDSYSTEXT -> sys_text).
	HudPosRecord sys_text;
	// The Tab board's atlases (hud_scoreboard.h). The stdbox piece size is
	// derived from the border atlas's own width (a 4x4 cell grid, so one cell
	// is a quarter of it), the same texture-derived rule the icon strips use.
	bool box_texture_valid = false;
	int box_tex_w = 0;
	bool net_icon_texture_valid = false;
	// The AAS zone status panel: the LFP_FLAGS anchor (the panel's right edge
	// and its row base) and the three team-icon atlases + the two tile slots
	// [orig: the hudpos writes g_hudZonePanelX/Y @0x5a0563/@0x5a057b; the
	//  icon loads HUD_LoadAllTextures @0x59dda0].
	int lfp_anchor_x = 0;
	int lfp_anchor_y = 0;
	bool lfp_anchor_present = false;
	std::array<bool, 3> lfp_icon_texture_valid{}; // team 1, team 2, neutral
	bool lfp_tile_own_texture_valid = false;
	bool lfp_tile_other_texture_valid = false;
	HudPosRecord clip_pos;
	HudPosRecord stance_pos;
	HudPosRecord frame_pos;
	HudRectRecord health_rect;
	HudRectRecord heat_rect;
	HudRectRecord power_rect;
	HudRectRecord spinmap_rect;
	// SPINMAPWPDISTOFF: a NONZERO authored value suppresses the waypoint
	// distance text; the retail global is BSS (no file bytes -> inits 0 =
	// label LIVE) and the sole read is ==0. The earlier "static -1" gloss
	// mis-read an undefined-bytes dump — re-adjudicated 2026-08-14 against
	// the segment map. [orig: dword_27237C0 (.data, uninitialized);
	//  parse @0x59fc1f; read @0x5a7a6a]
	int spinmap_wp_dist_off = 0;
	// MAPCOORDS: the player grid label position + its suppressor (same
	// BSS-zero live-by-default polarity). [orig:
	//  screenX/screenY/dword_27236FC (.data, uninitialized), parse @0x5a0920]
	float map_coords_x = 0.0f;
	float map_coords_y = 0.0f;
	int map_coords_off = 0;
	// The six stance frames' authored per-frame offsets + frame-0 dims.
	std::array<int, 6> stance_offset_x{};
	std::array<int, 6> stance_offset_y{};
	int stance_frame0_w = 0;
	int stance_frame0_h = 0;
	std::array<bool, 6> stance_texture_valid{};
	int frame_tex_w = 0;
	int frame_tex_h = 0;
	int crosshair_tex_w = 0;
	int crosshair_tex_h = 0;
	int clip_tex_w = 0;
	int clip_tex_h = 0;
	int round_tex_w = 0;
	int round_tex_h = 0;
	bool frame_texture_valid = false;
	bool crosshair_texture_valid = false;
	// Colors (0xAARRGGBB).
	uint32_t health_border = 0xC759C7C7u;
	uint32_t tag_good = 0xFF05FA0Du;
	uint32_t tag_middle = 0xFFFAA608u;
	uint32_t tag_bad = 0xFFB00A0Au;
	uint32_t hud_text = 0xFFFAD605u;
	uint32_t weapon_text = 0xFFFAD605u;
	uint32_t stance_tint = 0xFFFFFFFFu;
	uint32_t heat_border = 0xFFFFFFFFu;
	// The stance colour TRIPLE. Only `bad` was pulled into the layout before,
	// because the stance bar is the only element that needed it; the vehicle
	// panel bands its seats with the same three [orig: the good/middle/bad arms
	//  of the seat loop @0x5A4FD0], so all three are carried now. The def has
	// parsed all three all along.
	uint32_t stance_good = 0xFF05FA0Du;
	uint32_t stance_middle = 0xFFFAA608u;
	uint32_t stance_bad = 0xFFB00A0Au;
	// The user crosshair config pair — player options, not hudpos.def. The
	// colour is the persisted RGB forced opaque (the colour-item parse forces
	// the top byte [orig: item colour wcstoul + forced opaque @ 0x64bd10 /
	// 0x64b220]); the spread flag gates only the offset, never the draw.
	// Defaults are the retail config defaults, one home for the options model
	// and the overlay [orig: Config_SetDefaults — colour 0xFFFFFF @ 0x54d461,
	// spread on @ 0x54d472]; the persisted value is the 24-bit RGB.
	static constexpr uint32_t kCrosshairColorMask = 0xFFFFFFu;
	static constexpr uint32_t kCrosshairColorDefault = 0xFFFFFFu;
	static constexpr bool kCrosshairSpreadDefault = true;
	uint32_t crosshair_color = 0xFF000000u | kCrosshairColorDefault;
	bool crosshair_spread_enabled = kCrosshairSpreadDefault;
	// ALPHAFADE (percent, percent, seconds) [orig: parse @ 0x5a086c].
	float alpha_fade_base = 0.0f;
	float alpha_fade_max = 0.0f;
	float alpha_fade_seconds = 0.0f;
	int chat_lines = 8;
	std::vector<HudSightsRow> sights;
};

struct HudWeaponState {
	bool active = false;
	int clip = -1;
	int reserve = -1;
	int capacity = 0;
	int rounds_per_icon = 1;
	int heat = 0; // 0..0xFFFF
	std::string display_name;
	// The clip-indicator flash key: the original stamps on the (ammo class,
	// reserve, pool) triple; the single-pool port keys (round_type, reserve)
	// [orig: draw_hud_ammo_indicator restamp; D-HUD-5].
	std::string round_type;
	// The HUDCLIPGFX/HUDRNDGFX placement (per-weapon authored offsets/step)
	// and the registered textures' dims.
	int clipgfx_offset_x = 0;
	int clipgfx_offset_y = 0;
	int rndgfx_offset_x = 0;
	int rndgfx_offset_y = 0;
	int rndgfx_step_x = 0;
	int rndgfx_step_y = 0;
	int clip_tex_w = 0;
	int clip_tex_h = 0;
	int round_tex_w = 0;
	int round_tex_h = 0;
	bool clip_texture_valid = false;
	bool round_texture_valid = false;
	bool sights_card_up = false;
};

struct HudWaypointState {
	bool present = false;
	std::string name;
	int distance_m = 0;
	int32_t world_x = 0;
	int32_t world_y = 0;
	// Altitude, Q16. Drives the spinmap state-line tricolor and the
	// WPIndctr frame. [orig: dword_2723520 read by HUD_UpdateWaypointAltitudeColor @0x590970]
	int32_t world_z = 0;
};

struct HudObjectiveRow {
	std::string text;
	bool done = false;
};

struct HudAttachLabel {
	float screen_x = 0.0f;
	float screen_y = 0.0f;
	std::string text;
	bool nearest = false;
};

// One projected friendly tag (D-HUD-20) [orig: HUD_DrawEntityLabel @ 0x5a39b0
// via HUD_DrawFriendlyTagsPass @ 0x5a4480]. The presenter projects the anchor
// (entity position + the per-tick eye offset + 0.25 u [orig: @ 0x5a3a84..
// 0x5a3a98]) and supplies the view distance; the compiler owns every
// witnessed draw rule.
struct HudFriendlyTag {
	float screen_x = 0.0f;
	float screen_y = 0.0f;
	int32_t dist_q16 = 0;   // |anchor - view position|, 16.16 world units
	std::string name;       // authored/callsign; empty resolves the fallback
	uint16_t entity_id = 0; // the fallback-name index [orig: (pool<<12)|slot]
	int32_t health_ratio_fp16 = 0x10000;
	bool medic = false;     // CharAttr class flag 8 [orig: charattr.def Medic]
	bool speaking = false;  // entity == g_voicePlaybackEntity @ 0xC6EC38
	bool player = false;    // slot-walk entry (empty callsign draws the bar leg)
	// The DOWNED legs [orig: HUD_DrawEntityLabel — dead = `Flags & 2`
	// @0x5a3c1c; slot present + slot+0x10 revive seconds + slot+0x2C medic
	// request @0x5a3ddd..0x5a3df5]: the bad tier recolors light blue / gray,
	// the request pulses toward white, and the seconds are appended to the
	// text (`"%s: %ld"` @0x5a400e) or drawn bare a fontH above the tick /
	// bar forms (`"%ld"` @0x5a41f0 / @0x5a4428).
	bool dead = false;
	bool has_slot = false;
	uint8_t revive_seconds = 0;
	bool medic_request = false;
};

struct HudMessageLine {
	std::string text;
	int expire_tick = 0;
	// The stored packed ARGB. Retail stores the caller's color raw and the
	// SYSTEM ring draws THAT value; only the CHAT ring folds a computed alpha
	// over it [orig: the stored-color read @0x59ae97 vs the chat fold
	// @0x59adef]. `-1` (0xFFFFFFFF) is the triggered-text default
	// [orig: Chat_AddDebugMessage(text, -1, 930) @0x51f216].
	uint32_t color = 0xFFFFFFFFu;
};

// The Tab player list's per-frame state. The embedder resolves the header
// strings (server name, mission title, game-type label) and joins each row's
// name from the roster, exactly as it already does for the objectives header;
// the compiler owns the witnessed layout, ordering and colors.
struct HudEndRoundLine {
	std::string text;
	int y = 0; // design-space y, centred on x 512
};
struct HudEndRoundOverlayState {
	bool shown = false;
	// The overlay safe-area top/bottom in design px (dword_24C1900 /
	// dword_24C1904, stamped by Renderer_SetDisplayModeWithFallback
	// @0x587634/@0x58760b; 0 / 768 for the full frame).
	int top = 0;
	int bottom = 768;
	std::vector<HudEndRoundLine> lines;
};

// The toggled SP "Show Score" statistics panel (hud/end_round_statistics.h
// owns its policy and geometry). The shell resolves the title and the four
// Epilog label strings and formats the values, because it owns the string
// tables. [orig: HUD_DrawEndRoundStatistics @0x5b7600 while dword_24C18AC]
struct HudEndRoundStatisticsState {
	bool shown = false;
	bool raised = false; // [orig: g_spawn_success_gate && winner == 1 @0x5b763b]
	std::string title;   // gametext ("Score", "SCORE_TITLE")
	std::array<std::string, 4> labels;
	std::array<std::string, 4> values;
};

struct HudScoreboardState {
	bool shown = false;
	uint32_t game_type = 0;
	// (PgUp/PgDn paging is a recorded D-HUD-24 residual; the state grows a
	// page cursor when that leg lands.)
	std::string title;         // Overlays/STROVER_KILLLIST ("Player List")
	std::string server_name;
	std::string mission_title;
	std::string game_type_label;
	std::string players_line;  // "<Client/STRCLI04> <count>"
	std::string spectators_line; // "<Client/STRCLI23> <count>", empty when none
	std::string footer;        // Text/CHANGE_SCREEN paging hint
	std::vector<ScoreboardEntry> rows;   // wire order; the server sorts
	// The 4-team page inputs (hud_scoreboard.h scoreboard_team_page): the
	// session's side count — a joiner reads it off the 0x16 team table the
	// host serializes from its g_num_teams_config [orig: @0x50db3a ->
	// g_scoreboard_team_count @0x42fdda] — and the HUD frame counter, the
	// per-main-frame clock the LFP panel blinks on too [orig: dword_A87060,
	// Game_TickHudFrameCounters @0x434c14].
	int team_count = 0;
	int frame_counter = 0;
};

// One seat box on the mounted-vehicle panel.
struct HudVehicleSeat {
	int x = 0;          // design-space offset from the panel base
	int y = 0;
	// An OCCUPIED seat draws a filled box banded by its rider's health; an
	// empty one draws its seat-select digit instead [orig: the two arms of the
	//  slot loop @0x5A4FD0].
	bool occupied = false;
	int32_t health = 0;
	int32_t max_health = 0;
	// The seat-select key label an EMPTY seat shows. Empty string draws none.
	std::string label;
	// The local player's own seat draws an X over the box, last.
	bool own_seat = false;
	// Which retail mountHandles slot this marker stands for: passenger seats
	// 0..7 (the block's `seats` pairs), 8 the control/driver seat (the `driver`
	// pair); -1 for an EMPLACEMENT marker (the block's `emplace` pairs, one per
	// attached gun child) [orig: the three arms of HUD_DrawVehicleHealthBars
	//  @0x5a5112..0x5a5364 (seats), @0x5a53b7..0x5a5547 (emplacements),
	//  @0x5a568e..0x5a5793 (driver)].
	int retail_slot = -1;
	// An emplacement occupant bands through the clamped classifier
	// (hud_vehicle_panel.h emplacement_health_band) [orig: @0x5a54d3..0x5a54e3].
	bool is_emplacement = false;
};

struct HudVehiclePanelState {
	bool shown = false;
	// The HUDVEHSTANCEPOS anchor and the rider's stance offset, joined by
	// hud::vehicle_panel_base -- the panel rides the stance icon.
	int anchor_x = 0;
	int anchor_y = 0;
	int stance_offset_x = 0;
	int stance_offset_y = 0;
	// The silhouette behind the seats: the block's `interface` texture, drawn
	// tinted by the HULL's health band.
	bool silhouette_valid = false;
	int silhouette_w = 0;
	int silhouette_h = 0;
	int32_t hull_health = 0;
	int32_t hull_max_health = 0;
	std::vector<HudVehicleSeat> seats;
};

// One contestable AAS zone on the status panel, in the spawn-zone list's order
// (hud/hud_lfp_panel.h owns the policy). The feed (world/lfp_feed.h) resolves
// everything from the registry + the zone-timer entry; the element draws.
struct HudLfpZone {
	int letter_index = 0;   // 'A' + the spawn-zone list index
	int team = 0;           // the zone entity's team byte (+0x162 & 0x1F)
	// The zone-timer entry's fields the marker reads [orig: EntryById[1] team,
	//  [8] value, [9] control target, [10] limit, [11] rate, [12] active].
	bool timer_present = false;
	int timer_team = 0;
	int32_t control = 0;    // DWORD 9
	int32_t rate = 0;       // DWORD 11
	int32_t value = 0;      // DWORD 8
	int32_t limit = 0;      // DWORD 10
	bool active = false;    // DWORD 12
	// The two in-radius contest counts as the 0x6F message carries them:
	// +0x220 the owning side's, +0x221 the other side's.
	uint8_t count_owner = 0;
	uint8_t count_other = 0;
	// The transient minimap slot's flag byte for this zone (+4 & 0xC0 gates
	// the marker) [orig: the word_28E5620 walk @0x5a2517..0x5a256e].
	uint8_t capture_flags = 0;
	bool in_cylinder = false;
	int distance_m = 0;     // after the witnessed subtrahend, metres
};

struct HudLfpPanelState {
	bool shown = false;
	int local_team = 0;
	// The HUD frame counter the blink masks (g_hudFrameCounter & 0x18).
	int frame_counter = 0;
	// The game's AAS-vs-conquest arm: the conquest arm is unmodelled and the
	// element draws nothing under it [orig: g_GameType == 0x50010 @0x5a24a1].
	bool conquest_mode = false;
	std::string under_attack_text; // Overlays/STROVER_UNDERATTACK
	std::string ready_text;        // Overlays/STROVER_READYFORTAKEOVER
	std::vector<HudLfpZone> zones;
};

struct HudFrameState {
	int ticks = 0;
	// THE RECENT MESSAGES (J) WINDOW: the OldMessages toggle and its stdbox
	// title (Overlays/STROVER43, resolved by the embedder like the scoreboard's)
	// [orig: g_showMessageLog @0x24C18C0; the title @0x5b9e54].
	bool message_log_shown = false;
	std::string message_log_title;
	// THE AAS ZONE STATUS PANEL (hud/hud_lfp_panel.h owns its policy).
	HudLfpPanelState lfp_panel;
	float health_fraction = 1.0f;
	int stance = 0;
	bool binoculars_view_active = false;
	bool aimed_shot_available = false;
	bool keep_crosshair_while_aimed = false;
	int32_t hud_spread_fp16 = 0;
	float fov_deg = 80.0f;
	// The projected aim point in SCREEN pixels; aim_valid=false pins the 1P
	// design center [orig: @ 0x5928a0].
	bool aim_valid = false;
	float aim_screen_x = 0.0f;
	float aim_screen_y = 0.0f;
	bool windup_active = false;
	int windup_held_ticks = 0;
	std::string objective_text;
	// The objectives-panel header line, resolved by the embedder from gametext
	// (Overlays/STROVER_MISSIONOBJECTIVES); empty falls back to the literal
	// [orig: header STROVER_MISSIONOBJECTIVES @ 0x5ba986].
	std::string objectives_header;
	// The objectives-panel alpha byte, folded into every panel draw color's
	// top byte [orig: dword_24C18CC — 0/255, flipped ^= 0xFF by the co-op
	// input action @ 0x49b68b]. The presenter's show/hide toggle stands in
	// for the input binding row (identical visually: alpha 0 draws nothing).
	uint32_t objectives_alpha = 0xFF;
	HudWeaponState weapon;
	HudWaypointState waypoint;
	std::vector<HudObjectiveRow> objectives;
	// THE MOUNTED-VEHICLE PANEL (hud/hud_vehicle_panel.h owns its policy).
	// Present only while the local player rides something; the shell resolves
	// the item's VEHICLE_HUD block and the live seat occupancy, because both
	// need tables the compiler does not own.
	HudVehiclePanelState vehicle_panel;
	// The Tab board (hud/hud_scoreboard.h owns its policy).
	HudScoreboardState scoreboard;
	// THE END-OF-ROUND OVERLAY (net-re §5.68): the resolved Impact38 text
	// ladder the presenter built from hud/end_round_overlay.h, drawn inside
	// the stdbox (8, top+8, 1015, bottom-8) of the overlay safe area
	// [orig: draw_endround_stats_overlay @0x5b7cd0 — HUD_DrawLabelBox
	//  @0x5b7d3e, each line HUD_DrawTextCentered_HalfBright(Impact38, 512, y)
	//  through HUD_DrawTextCenteredScaled (ex sub_580B80) @0x580b80].
	HudEndRoundOverlayState end_round;
	// The toggled SP "Show Score" statistics panel [orig: HUD_DrawOverlayPanels @0x5c0083
	// draws HUD_DrawEndRoundStatistics @0x5b7600 while dword_24C18AC].
	HudEndRoundStatisticsState end_round_statistics;
	std::vector<HudAttachLabel> attach_labels;
	// Friendly tags (D-HUD-20). Mode default 2 = FULL [orig: Game_Run
	// @ 0x4a7fed]; fog cull against the environment's current fog distance
	// [orig: Env_FogDistCurrent @ 0x5a3b28]; one speaking level shared by the
	// (single) speaking entity [orig: g_audioOutLevelStage1].
	std::vector<HudFriendlyTag> friendly_tags;
	int friendly_tag_mode = 2;
	int32_t fog_dist_q16 = INT32_MAX;
	int speaking_level255 = 0;
	// The HUD color scheme index (0 white / 1 green / 2 hudpos hud_textcolor /
	// 3 light blue / 4 yellow / 5 salmon). Selects the master overlay color the
	// text elements draw with and the friendly-tag good-tier source [orig:
	// cfg_hud_color_index, config token "hud_color_index" default 2 @0x54d2a6;
	// applied to the live index @0x55152f; cycled 0..5 by input action
	// `hudcolor` (record row 76, dispatch code 10) @0x49afc7].
	int hud_color_index = 2;
	// The HUD declutter feed (hud_declutter.h carries the witness map): the
	// per-slot visibility table and the persisted hud_detail level, rebuilt by
	// HudDeclutter from the hudpos HUDDECLUT masks. Defaults all-visible at
	// level 0 so a declutter-less embedder (and the layout-only tests) draw
	// everything; a real hudpos feed replaces the table with the authored
	// masks (unauthored slot = hidden). The level itself rides along for the
	// two draw sites that read it directly: the level-3 whole-pass early-out
	// [orig: @ 0x5A80C4] and the chat hard cull [orig: level >= 2 @ 0x59AD43].
	std::array<bool, kDeclutterSlotCount> declutter_visible =
			declutter_all_visible();
	int hud_detail_level = 0;
	// The showhud 2-bit FP-view flags [orig: g_FpWeaponViewFlags — cycle
	// (flags + 1) & 3 @ 0x4E0561]: bit 0 gates the FP gun/viewmodel draw
	// (consumed device-side where the viewmodel submits), bit 1 gates the
	// FP-weapon sub-pass + the corner spinmap block [orig: test 2 @ 0x5A8635].
	// Default 3 = gun + spinmap (the cfg gun-visible option writes 3/2
	// [orig: @ 0x5521CB/@ 0x5521D7]).
	uint32_t showhud_flags = 3;
	HudMinimapInput minimap;
	// The mission's static footprint polygons (baked once per feed); the
	// spinmap element lends them to the compile input by pointer — the
	// per-frame input copy must never clone the triangle set.
	std::vector<HudMinimapFootprint> map_footprints;
};

struct HudDrawList {
	std::vector<HudQuad> quads;
	std::vector<HudTri> tris;
	std::vector<HudLine> lines;
	std::vector<GameFontQuad> glyphs;
	std::vector<GameFontUnderline> underlines;
	HudMapPass map;
	// The M-cycle big map (mode 2 window / mode 3 fullscreen) renders as a
	// second pass over the corner spinmap — retail draws both.
	// [orig: Render_ProcessMainSceneFrame @0x5cac50 -> HUD_BuildMapOverlayView
	//  @0x5a7e10 in addition to the HUD_RenderAllOverlays spinmap ctx]
	HudMapPass big_map;
	// Per-pass spinmap label glyphs: each pass's device leg layers its own
	// glyphs above that pass's additive terrain, after its sprites/lines —
	// not with the flat HUD text, and never across passes (the big map's
	// letters must not render under its own grid rules or over the corner
	// map from the wrong item).
	std::vector<GameFontQuad> map_glyphs;
	std::vector<GameFontQuad> big_map_glyphs;
	int64_t elements_drawn = 0;
};

// Draw-list font-page namespaces: each compiler font emits glyph pages at
// slot * FNT_MAX_PAGES, so one flat draw list mixes faces and the device leg
// indexes its page-texture table the same way.
inline constexpr int kHudFontSlotHud = 0;       // the hudpos-named HUD font
inline constexpr int kHudFontSlotLabel = 1;     // g_hudLabelFont (Arial normal)
inline constexpr int kHudFontSlotLabelBold = 2; // the bold slot (fontObj @ 0xB4C394)
inline constexpr int kHudFontSlotLabelLarge = 3; // g_hudLabelFontLarge (Impac22b)
inline constexpr int kHudFontSlotImpact38 = 4;   // g_hudLabelFontImpact38 (Impac38b)
inline constexpr int kHudFontSlotCount = 5;
// The device leg sizes its page-texture table by the count; a slot past it
// writes Ref<> handles off the end of that table (the 2026-08-24 load crash).
static_assert(kHudFontSlotImpact38 < kHudFontSlotCount,
              "every font slot must index inside kHudFontSlotCount");

// Deep in-process module: the whole witnessed element walk, stance cross-fade
// state, the clip-indicator flash state, and the triggered-text message ring
// live here; compile() emits everything for one frame in retail's order.
class HudFrameCompiler {
public:
	void configure(const HudLayout &layout, const fnt_font_t *font);

	// The overlay label fonts + their resolution scales — the Arial pair and
	// the large slot retail loads beside the hudpos HUD font
	// [orig: HUD_InitAllFonts @ 0x51ee20: g_hudLabelFont = Arial14n/16n, the
	// bold slot (fontObj @ 0xB4C394) = Arial12b/14b/16b at scale
	// (screenW<<16)/{640,800,1024}; g_hudLabelFontLarge @0xB4C3A0 =
	// Impac22b.fnt at the over-800 scale; the slot carries
	// {font, scale_x, scale_y} @ 0x580453..0x580468]. Friendly tags draw
	// with the normal face [orig: @ 0x5a3a0c], attach labels with the bold
	// face [orig: @ 0x5a3680/@ 0x5a38a1], the big-map grid labels with the
	// large face. Null fonts fall back to the hudpos font at scale 1
	// (layout-only embedders keep drawing).
	void configure_label_fonts(const fnt_font_t *normal, const fnt_font_t *bold,
			const fnt_font_t *large, float scale, float large_scale,
			const fnt_font_t *impact38 = nullptr);

	// Swap the layout WITHOUT resetting runtime state (stance fade, clip
	// flash, the message ring) — the texture-table refresh path, e.g. the
	// options crosshair-style reload [orig: HUD_LoadAllTextures @ 0x59e3d6
	// re-registers textures without touching the live HUD state].
	void update_layout(const HudLayout &layout);

	// The stance cross-fade restamp [orig: @ 0x599f8a] and the message ring
	// [orig: Chat_AddDebugMessage @ 0x4987f0] are compiler state.
	// Mission triggered text — posts into the one system ring with the default
	// white [orig: Chat_AddDebugMessage(text, -1, 930) @0x51f216].
	void push_message(const std::string &text, int now_ticks);
	// Post one line to the SYSTEM feed — the ring every Chat_AddDebugMessage
	// caller shares (kill/objective/medic lines, triggered text, and later the
	// join/system lines) [orig: the sink @0x4987f0; drawn by the second
	// HUD_DrawConsoleMessages loop @0x59ad30]. 930-tick life, >= 186-tick
	// expiry stagger; the packed ARGB is stored raw and drawn as stored (no
	// fade on this ring).
	void push_feed_line(const std::string &text, uint32_t argb, int now_ticks);
	// Post one line to the CHAT ring — the player-chat channel every S2C 0x14
	// line lands in [orig: Chat_DispatchToChannel @0x42b910 ->
	// Chat_AddMessageChannel1 @0x4985d0]. What the ring holds is the DISPLAY
	// buffer (byte_B3FDBC, slots 1..40, newest in slot 1): the line is
	// word-wrapped with the bold label font to `x2 - (x1 - 4)` of the chat
	// box [orig: @0x498673..0x4986c5, a 0 count is 1], the buffer shifts by
	// that count @0x4986db..0x498701, the new slots are zeroed @0x498718,
	// slot 1's timer is `max(930, slot2.timer + 186)` read right after the
	// zeroing @0x498722..0x498734 (so a multi-line post's own continuation
	// slot is the slot 2 it staggers against), and the segments land first
	// segment highest, last segment in slot 1, every segment after the first
	// prefixed "  " @0x498799, colour at +120 on each @0x4987c4. Continuation
	// slots keep timer 0: they never draw on the feed, and always list in the
	// Recent Messages window.
	void push_chat_line(const std::string &text, uint32_t argb, int now_ticks);
	// The two rings' live history for the message-log window: the newest
	// `max_rows` lines, oldest first, NO expiry test [orig: HUD_DrawMessageLog
	// @0x5b9d70 walks slots 16..1 of both rings @0x5b9e8a..0x5b9f1a].
	const std::vector<HudMessageLine> &chat_lines() const { return chat_lines_; }
	void reset_runtime_state();

	const HudDrawList &compile(const HudFrameState &state, float surface_w,
			float surface_h);

	const HudDrawList &last_draw_list() const { return draw_list_; }

private:
	struct StanceFade {
		int prev = 0;
		int cur = 0;
		int stamp = 0;
	};

	float sx(float design_x, float surface_w) const;
	float sy(float design_y, float surface_h) const;
	// The master overlay color for the frame: the scheme table entry at the
	// state's hud_color_index, with entry 2 sourced live from the hudpos
	// hud_textcolor (see the table in hud_frame.cpp).
	uint32_t active_color(const HudFrameState &state) const;
	void emit_rect(float x0, float y0, float x1, float y1, uint32_t color,
			bool filled, int32_t texture = kHudTexNone, bool additive = false);
	void emit_rect_uv(float x0, float y0, float x1, float y1, float u0, float v0,
			float u1, float v1, uint32_t color, int32_t texture);
	// The retail stdbox panel and the per-row connection icon. The box takes
	// the surface width because its pieces and fill inset scale with it.
	// title_gap_w > 0 draws the TITLED top row: the row-3 stub / title-bar /
	// end-cap cells around a gap of that many output pixels
	// [orig: the outTechnique arm @0x56b937, cells rec+0x108/0x120/0x138].
	void emit_stdbox(float x0, float y0, float x1, float y1, float surface_w,
			uint32_t color, float title_gap_w);
	void emit_stdbox_piece(float x0, float y0, float x1, float y1, int col,
			int row, bool crop_bottom, uint32_t color);
	void emit_net_icon(float x0, float y0, float x1, float y1, int quality);
	void emit_wire_rect(float x0, float y0, float x1, float y1, uint32_t color);
	void emit_text(const char *text, float design_x, float design_y,
			float surface_w, float surface_h, uint32_t argb, uint32_t flags);
	float measure_text_w(const char *text) const;
	float text_line_h() const;

	void element_frame(const HudFrameState &state, float w, float h);
	void element_health(const HudFrameState &state, float w, float h);
	void element_stance(const HudFrameState &state, float w, float h);
	void element_weapon_cluster(const HudFrameState &state, float w, float h);
	void element_heat(const HudFrameState &state, float w, float h);
	void element_power(const HudFrameState &state, float w, float h);
	void element_waypoint(const HudFrameState &state, float w, float h);
	void element_spinmap(const HudFrameState &state, float w, float h);
	void element_objectives(const HudFrameState &state, float w, float h);
	void element_attach_labels(const HudFrameState &state, float w, float h);
	void element_friendly_tags(const HudFrameState &state, float w, float h);
	void element_objective_line(const HudFrameState &state, float w, float h);
	void element_feed(const HudFrameState &state, float w, float h);
	void element_message_log(const HudFrameState &state, float w, float h);
	void element_lfp_panel(const HudFrameState &state, float w, float h);
	void element_scoreboard(const HudFrameState &state, float w, float h);
	void element_end_round_overlay(const HudFrameState &state, float w, float h);
	void element_end_round_statistics(const HudFrameState &state, float w,
			float h);
	void element_vehicle_panel(const HudFrameState &state, float w, float h);
	void element_sights_card(const HudFrameState &state, float w, float h);
	void element_crosshair(const HudFrameState &state, float w, float h);
	void element_clip_indicator(const HudFrameState &state, float w, float h);

	HudLayout layout_{};
	GameFont font_;
	// The Arial label pair + the large slot + scales (configure_label_fonts).
	GameFont label_font_;
	GameFont label_font_bold_;
	GameFont label_font_large_;
	GameFont label_font_impact38_; // the Impac38b slot (end-round overlay)
	float label_scale_ = 1.0f;
	float label_large_scale_ = 1.0f;
	HudDrawList draw_list_;
	// Lives across frames so the map pass vectors and clip scratch keep
	// their capacity (the per-frame spinmap compile is allocation-free).
	HudMinimapCompiler minimap_compiler_;
	// The per-frame compile input also persists: copy-assigning the frame
	// state into it reuses the markers vector's capacity instead of
	// heap-cloning it every frame.
	HudMinimapInput minimap_input_;
	StanceFade stance_;
	// The clip-indicator flash latch [orig: draw_hud_ammo_indicator flash
	// @ 0x599af9]: the round count drop stamps the flash start.
	int flash_prev_rounds_ = -1;
	int flash_stamp_ = 0;
	std::vector<HudMessageLine> feed_lines_;   // the SYSTEM ring
	std::vector<HudMessageLine> chat_lines_;   // the CHAT ring (S2C 0x14)
};

// The chat word-wrap [orig: HUD_WordWrapText @0x580980]: the whole remaining text
// measured first — it fits when `extent < current_x + max_width` @0x5809c1
// (ONE line); otherwise the characters are walked from `current_x`, each
// adding its width + 1 @0x5809e4, the last space noted @0x5809cf, and the
// first overflow @0x5809ea breaks at that space (written as a NUL in place
// @0x580a0b) — no space yet means no break @0x5809fc. The remainder recurses
// with `current_x = 2 * width(' ')` @0x580a19..0x580a3e, the two-space
// continuation prefix's width. Returns the segment count (0 without a font);
// the segments are the NUL-separated runs left in `text`.
int chat_wrap_text(const GameFont &font, float scale, std::string &text,
		int max_width, int current_x);

} // namespace opennova::hud
