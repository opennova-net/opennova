#pragma once

// The HUD frame compiler (ADR 0033 R2): one compile turns the per-frame HUD
// state + the hudpos.def layout into a typed draw list — screen-space quads,
// lines, and game-font glyph quads — in the witnessed element order. The
// embedder keeps texture upload and rasterization only.
// [orig: HUD_BuildEntityInfo @ 0x4b8440 -> HUD_RenderAllOverlays @ 0x5a8070
//  -> HUD_RenderOverlays @ 0x5a7bb0 -> the per-element draws]
// Witness record: docs/interface/hud-re.md; per-element policy math lives in
// hud/hud_math.h and stays the single source.

#include <runtime/hud/feed_format.h>
#include <runtime/hud/hud_combat.h>
#include <runtime/hud/game_font.h>
#include <runtime/hud/hud_declutter.h>
#include <runtime/hud/hud_elements.h> // which element of the walk drew each run
#include <runtime/hud/hud_math.h>
#include <runtime/hud/hud_scoreboard.h>
#include <runtime/hud/hud_minimap.h>
#include <runtime/hud/hud_map_view.h> // the DEATH window pass
#include <runtime/hud/hud_overlay_windows.h>
#include <runtime/hud/hud_server_status.h> // the server-status page
#include <runtime/hud/net_quality_indicators.h> // the connection indicators' state
#include <runtime/hud/sight_overlay.h> // the SIGHTS row modes + sight-scale default

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
	kHudTexMapRadar,       // dmgslice.tga, the 12-ring marks
	kHudTexMapWpIndicator, // WPIndctr.tga [orig: HUD_LoadAllTextures @0x59e079]
	// dmgslc_n.tga, the 24-ring marks: map sprite texture offset 4
	// (kHudMapSpriteRadarNarrow) [orig: rec @0x2723930 loaded
	// @0x59de25..0x59de42].
	kHudTexMapRadarNarrow,
	// The Tab board's stdbox: the 4x4 border stencil atlas and the tiled
	// interior brush [orig: the panel call @0x423a72 -> HUD_DrawLabelBox
	// @0x51efd0 -> Render_HUDBoxOverlay @0x56b700, "stdbox" registered
	// @0x51effa]. Inserted BEFORE the sights
	// sentinel, which sizes the device slot array.
	kHudTexBoxBorder,
	kHudTexBoxTile,
	// The 16x16 connection-quality icon, a 4-row vertical atlas
	// [orig: the quad @0x4241fb; the atlas load CNetworkIcons_LoadTextures @0x4c2cf0].
	// neticon2.tga, iconPtrs[1] = g_NetConnectionIcon @0xB5E03C; the HUD's
	// top-left quality icon draws the same atlas.
	kHudTexNetIcon,
	// The other two connection indicators [orig: CNetworkIcons_LoadTextures
	// @0x4c2cf0 — neticon1.tga, 4 rows (tga height / 4 @0x4c2d58) ->
	// iconPtrs[0] = g_NetLatencyIcon @0xB5E038, the T/R link-error pair;
	// neticon3.tga, 2 rows (tga height / 2 @0x4c2e46) -> iconPtrs[2] =
	// g_NetNovaWorldIcon @0xB5E040, the NovaWorld N].
	kHudTexNetLinkIcon,
	kHudTexNetNovaWorldIcon,
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
	//  @0x5989b9..0x5989d1 — g_HUDZoneTileOwnTexture @0x27239D4 for team == local,
	//  g_HUDZoneTileOtherTexture @0x27239C4 otherwise; HUD_LoadAllTextures loads
	//  lfp_alf.tga into the record @0x27239C0 @0x59e10e and lfp_dlf.tga into
	//  @0x27239D0 @0x59e11f, alpha mode 0, the shader at +4 being what the
	//  drawers bind]. The capture-point labels' marker types 9 / 8 bind the
	//  same pair [orig: HUD_DrawEntityMarker @0x593704 / @0x593724].
	kHudTexLfpTileOwn,
	kHudTexLfpTileOther,
	kHudTexTarget,
	kHudTexTargetFriendly,
	kHudTexCustomAim,
	kHudTexCommander,
	kHudTexWeaponSilhouette,
	kHudTexVehicleStatus,
	kHudTexCargo,
	kHudTexParachute,
	kHudTexArmor,
	kHudTexDriverCrosshair,
	kHudTexVehicleFixed,
	kHudTexVehicleLag,
	// The vehicle-bay logos HUD_DrawEntityMarker types 5/6/7 bind
	// [orig: dword_2723A84 / dword_2723A94 / dword_2723AA4 @0x593642 /
	//  @0x593662 / @0x593686; loaded HUD_LoadAllTextures @0x59de7a..0x59deab].
	kHudTexLogoHelo,
	kHudTexLogoHumm,
	kHudTexLogoBoat,
	// The HUDLS weapon slot bar: the bracket and the more-available marker
	// (hudpos-named, loaded alpha mode 0) and one icon per weapon category
	// 0..9 — the category's first def's hud_loadout_select texture
	// [orig: HUD_LoadAllTextures @0x59DF03 / @0x59DF2D -> dword_2723788 /
	//  dword_2723798 (handle +4, w +8, h +12); the def's texture block
	//  WeaponDefs_ParseLineCallback @0x544A44 -> def+0x1B8, read as
	//  def+0x1BC/0x1C0/0x1C4 by HUD_DrawWeaponSlotBar @0x599E5A..0x599E73].
	kHudTexSlotBarBracket,
	kHudTexSlotBarMoreAv,
	// The tip panel ("MrClippy"): its box atlas border3.tga (no second stage)
	// and the two icons, keyboard k_tip.tga and gameplay g_tip.tga
	// [orig: CTipSystem_Init @0x5b6970 — the panel slot @0x5b6978, border3
	//  @0x5b698c, k_tip @0x5b69ae, g_tip @0x5b69b6].
	kHudTexTipBox,
	kHudTexTipKeyboard,
	kHudTexTipGameplay,
	kHudTexSlotBarIcon0, // + weapon category 0..9
	kHudTexSightsBase = kHudTexSlotBarIcon0 + 10, // authored SIGHTS rows: + row index
};
static_assert(kHudTexMapIcons + kHudMapSpriteRadar == kHudTexMapRadar &&
		kHudTexMapIcons + kHudMapSpriteRadarNarrow == kHudTexMapRadarNarrow,
		"map sprite texture offsets index from the icon-strip slot");

// THE STDBOX GEOMETRY, as raw retail numbers. The border pieces and the fill
// inset scale with the surface by s = surface_w / 1600 [orig: the scale
// 0.000625 double @0x51f02e in HUD_DrawLabelBox]; the fill's tile PERIOD does
// not scale — it is the atlas cell's own size (texW/4, 32 px for the shipped
// 128 px border.tga), because retail's fill is one wrap-addressed quad of the
// EXTRACTED cell (3,0) with UV = (screen_px + 0.5) / cell
// [orig: HUD_StdboxDrawFillWrapTiled @0x56b5d0; the extraction + zeroing of
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

// The connection indicators' corners as the CNetQuality reset leaves them:
// quality (4, 4), link error (20, 4), NovaWorld (52, 4) — also the corners the
// server-status page always uses [orig: CNetQuality_Reset @0x4c5908..0x4c591e;
// the forceDefaultPos arms @0x4c325d / @0x4c332e / @0x4c345c].
inline constexpr std::array<int, 6> kNetIndicatorResetPos = {4, 4, 20, 4, 52, 4};

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
	// A second texture stage (kHudTexNone = none): the quad's colour is
	// saturate(2 * stage0 * texture2) and its alpha stage0.a * texture2.a --
	// MODULATE2X(CURRENT, TEXTURE) colour, MODULATE(CURRENT, TEXTURE) alpha --
	// with texture2 sampled SCREEN-ANCHORED and wrap-addressed: UV1 =
	// (screen_px + 0.5) / stage2 at each corner, i.e. surface pixel i shows
	// texel i mod stage2 (the half pixel is D3D9's pixel-centre rule; a
	// raster whose pixel centres sit at i + 0.5 samples the same texel at
	// px / stage2). Only the stdbox border pieces carry one.
	// [orig: HUD_DrawTexturedQuad_0 @0x56b3e0 -- UV1 @0x56b560..0x56b5a7 over
	//  the stage dims passed in; the 0x651 two-texture material's preferred
	//  permutation RenderState_FindBestTextureFormatPermutation @0x6820c0 --
	//  stage 1 colour RenderState_DecodeModeColorStage(0xF00) @0x682b5a,
	//  alpha RenderState_DecodeModeAlphaStage(0xF0) @0x682b2b]
	int32_t texture2 = kHudTexNone;
	float stage2_w = 0.0f;
	float stage2_h = 0.0f;
};

// One textured triangle (the crosshair's tapered arm strips)
// [orig: HUD_DrawCrosshairCornerQuad @ 0x590f50].
struct HudTriVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	uint32_t color = 0xFFFFFFFFu;
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
	// The row's authored draw mode (sight_overlay.h SightRowSpec carries the
	// witness): `scale` rides the sight-scale index, `slide` the scope-zero
	// multiplier times these frames.
	bool scale = false;
	bool slide = false;
	int32_t slide_frames = 0;
};

// The HUDLS weapon slot bar's layout globals (def.h DefHudPosDef carries the
// parse witnesses). `system` 0 never draws — and neither shipped hudpos.def
// authors HUDLS_SYSTEM, so on retail the bar never shows. The texture fields
// are the device's (the two hudpos-named textures' pixel sizes).
// [orig: dword_2723700 (system), dword_2723718/1C (key offset), the signed
//  bytes byte_2723733/34 (MOREAV offset), dword_2723738 + 8(n-1) (slot n)]
struct HudSlotBarLayout {
	int system = 0;
	int key_ofst_x = 0;
	int key_ofst_y = 0;
	int moreav_dx = 0;
	int moreav_dy = 0;
	std::array<int, 10> slot_x{};
	std::array<int, 10> slot_y{};
	bool bracket_texture_valid = false;
	int bracket_tex_w = 0;
	int bracket_tex_h = 0;
	bool moreav_texture_valid = false;
	int moreav_tex_w = 0;
	int moreav_tex_h = 0;
};

// The layout globals parsed once from hudpos.def (the embedder resolves
// texture names to the slots above and hands the parsed .fnt).
// [orig: the dword_27237xx.. layout global block, HUD_ParseHudposToken @0x59f370 parse]
struct HudLayout {
	HudCombatLayout combat;
	HudPosRecord scope_range;
	HudPosRecord scope_zero;
	HudPosRecord scope_mag;
	HudPosRecord ammo_count;
	HudPosRecord weapon_name;
	HudPosRecord game_info;
	HudPosRecord wpd_info;
	// The session text anchors. HUDTIMECLOCK keeps two fields — an authored
	// 3rd/4th are ignored [orig: HUD_ParseHudposToken @0x59FDB8 / @0x59FDD0 ->
	// dword_2723638/3C]; HUDPLAYERCOUNT and HUDTEAMXY are the 4-field form
	// [orig: @0x59FDFC..0x59FE35 -> dword_2723664..70; @0x5A1324..0x5A135D ->
	// dword_272382C..g_HUDTeamIdAlign @0x2723838]; ZONEINFO is x, y and the
	// alignment in .align, no hidden dword [orig: @0x5A0650..0x5A0676 ->
	// g_HUDZoneInfoX @0x2723DA4, dword_2723DA8/AC]. Unauthored they keep the
	// BSS zero record the drawers still read.
	HudPosRecord time_clock;
	HudPosRecord player_count;
	HudPosRecord team_xy;
	HudPosRecord zone_info;
	HudSlotBarLayout hudls;
	HudPosRecord chat_text;
	// The chat box's x1/x2 columns — g_HUDChatBoxCoords rows 1 and 2, the
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
	// HUDORDERS — the right edge of the two squad order lines; -1 / -1 when
	// unauthored [orig: dword_2723D84 / dword_2723D88].
	HudPosRecord squad_orders{-1, -1};
	// BREATHTIME — the breath bar's anchor: x, y and the alignment word, three
	// fields with no hidden dword. Unauthored it keeps the BSS zero (0, 0,
	// left): the bar has no presence gate [orig: HUD_ParseHudposToken
	// @0x59FB3B..0x59FB84 -> dword_2723810/14/18 (atof, atof,
	// HUD_ParseTextAlignment)].
	HudPosRecord breath_time;
	// The Tab board's atlases (hud_scoreboard.h). The stdbox piece size is
	// derived from the border atlas's own width (a 4x4 cell grid, so one cell
	// is a quarter of it), the same texture-derived rule the icon strips use.
	bool box_texture_valid = false;
	int box_tex_w = 0;
	// The boxtile camo's own dims, the border pieces' second-stage UV
	// divisors [orig: stored into the style as floats @0x56b357 / @0x56b361,
	// passed to HUD_DrawTexturedQuad_0 for every piece @0x56b97a..0x56bcbb].
	int box_tile_w = 0;
	int box_tile_h = 0;
	bool net_icon_texture_valid = false;
	// The connection indicators' other two atlases (neticon1 / neticon3) and
	// their three design-space corners, quality / link-error / NovaWorld, as
	// x, y pairs. The hudpos NETWORKINDICATOR line authors all six; unauthored,
	// they keep the reset's (4, 4), (20, 4), (52, 4) [orig: CNetQuality_Reset
	// @0x4c5908..0x4c591e -> g_NetQuality +0x40..+0x54; HUD_ParseHudposToken
	// @0x59f981..0x59fa0c overwrites them after the mission-start reset].
	bool net_link_icon_texture_valid = false;
	bool net_novaworld_icon_texture_valid = false;
	std::array<int, 6> net_indicator_pos = kNetIndicatorResetPos;
	// The AAS zone status panel: the LFP_FLAGS anchor (the panel's right edge
	// and its row base) and the three team-icon atlases + the two tile slots
	// [orig: the hudpos writes g_HUDZonePanelX/Y @0x5a0563/@0x5a057b; the
	//  icon loads HUD_LoadAllTextures @0x59dda0].
	int lfp_anchor_x = 0;
	int lfp_anchor_y = 0;
	bool lfp_anchor_present = false;
	std::array<bool, 3> lfp_icon_texture_valid{}; // team 1, team 2, neutral
	bool lfp_tile_own_texture_valid = false;
	bool lfp_tile_other_texture_valid = false;
	HudPosRecord clip_pos;
	HudPosRecord stance_pos;
	// HUDVEHSTANCEPOS — the vehicle panel's base before the rider's stance
	// offset (hud_vehicle_panel.h reads it as the panel anchor).
	HudPosRecord veh_stance_pos;
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
	// PAUSEDPOS: the SP pause text's anchor, BSS zero unauthored (the drawer
	// then uses (1000, 4)) [orig: dword_272360C / dword_2723610, parse
	// @0x59FCB0 / @0x59FCC8].
	int paused_x = 0;
	int paused_y = 0;
	// MRCLIPPYNORMAL / MRCLIPPYALTERNATE: the tip panel's x, y and its two
	// pads (the width and the height added past the measured text), BSS zero
	// unauthored [orig: HUD_ParseHudposToken @0x59fa15..0x59fae0 ->
	// g_TipSystem +0x40..+0x4C / +0x50..+0x5C (dword_28E1B10..28E1B2C)].
	std::array<int, 4> tip_normal{};
	std::array<int, 4> tip_alternate{};
	// The tip panel's atlas (border3.tga, its piece size a quarter of its own
	// width) and the two icons.
	bool tip_box_texture_valid = false;
	int tip_box_tex_w = 0;
	bool tip_keyboard_texture_valid = false;
	bool tip_gameplay_texture_valid = false;
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
	// ALPHAFADE as the original stores it: the base and max alphas
	// (percent x kPercentToAlpha) and the ramp in ticks (seconds x 62), each
	// _ftol2_sse of the double product [orig: parse @ 0x5a086c..0x5a08c2,
	// dword_2723614 / dword_2723618 / dword_272361C].
	int alpha_fade_base_alpha = 0;
	int alpha_fade_max_alpha = 0;
	int alpha_fade_ramp_ticks = 0;
	int chat_lines = 8;
	std::vector<HudSightsRow> sights;
};

// [orig: HUD_DrawScopeOverlayDetails @ 0x59e420]. The embedder supplies
// localized templates; the compiler owns selection, values, color and layout.
struct HudScopeState {
	bool active = false;
	bool scoped = false;
	bool rangefinder = false;
	bool zeroable = false;
	int32_t range_q16 = 0;
	int32_t max_range_q16 = 0;
	int zero_word = 0;
	int zero_step_metres = 0;
	int magnification = 1;
	std::string range_format;
	std::string range_over_1km;
	std::string zero_format;
	std::string zero_auto;
	std::string zero_none;
	std::string magnification_format;
};

struct HudWeaponState {
	bool active = false;
	int clip = -1;
	int reserve = -1; // HUD total: capacity-one chamber already folded by the feed
	int capacity = 0;
	int rounds_per_icon = 1;
	int heat = 0; // 0..0xFFFF
	std::string display_name;
	// The clip-indicator flash key's two def halves (the third is `reserve`):
	// the def's ammo bucket (def+0xDC, WeaponTableEntry::ammo_bucket) and the
	// LOW BYTE of its ammo-class id (def+0xD8) [orig: HUD_DrawAmmoIndicator
	// @0x599A90..0x599AB2 — `mov edi,[ecx+0DCh]`, `cmp dl,[ecx+0D8h]`; D-HUD-5].
	int32_t ammo_bucket = 0;
	uint8_t ammo_class_id = 0;
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
	// The COMPOSED label (hud_game_text.h waypoint_label_text: "m to" + the
	// resolved name, the CTF colour runs, the LFP override); empty stands for
	// retail's NULL name, which draws the distance alone [orig: the null test
	// @0x5948bc].
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
	bool speaking = false;  // entity == g_VoicePlaybackEntity @ 0xC6EC38
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
	// The entity's S2C 0x6D radio-request latch (entity+885, event 6 -> 1)
	// folded with the carrier test: `+885 != 0 && !Entity_FindChildByDefType
	// (entity, 1, 1)` -- an entity aboard a def-type-1 carrier shows no icon
	// [orig: HUD_DrawEntityLabel `cmp byte ptr [ebx+375h], 0` @0x5a3bfe,
	//  the child walk @0x5a3c0e, `xor ebp, ebp` @0x5a3c1a; the writer
	//  NapiNPClientMsg_HandleEntityDeath @0x430C50]. Drawn as the
	//  kFriendlyTagRadioRequestIcon cell only under
	//  HudFrameState::radio_request_icon_viewer.
	bool radio_request = false;
	// The player slot's squad colour index (slot+0x33) into kHudSquadColors;
	// 0 = no override. Client-local: its only writer is the CMAP entity
	// click [orig: CMap_EntityWidgetHandler @0x5485E4 — (idx + 1) % 14], so
	// the seam defaults 0 until that producer lands (D-HUD-20).
	uint8_t squad_color_index = 0;
};

// THE SQUAD COLOUR TABLE [orig: g_SquadColors @0x83B450, 14 dwords]: entry 0
// is never drawn (the drawer's `> 0` test), entry 1 the violet, then the
// twelve pastels.
inline constexpr std::array<uint32_t, 14> kHudSquadColors = {
		0xFF000000u, 0xFFA000F0u, 0xFFFF9999u, 0xFFFFCC99u, 0xFFFFFF99u, 0xFFCCFF99u,
		0xFF99FF99u, 0xFF99FFCCu, 0xFF99FFFFu, 0xFF99CCFFu, 0xFF9999FFu, 0xFFCC99FFu,
		0xFFFF99FFu, 0xFFFF99CCu};

// The friendly tag's tier colour after the squad override: the GOOD tier takes
// the squad colour whole, the MIDDLE tier each RGB channel of it times 0.7
// truncated (alpha kept), the BAD tier never overrides; a missing slot or
// index 0 keeps `tier_rgb` [orig: HUD_DrawEntityLabel — good @0x5A3CBB..0x5A3CD5,
// middle @0x5A3CE9..0x5A3DBC (dbl_7D9DE8 = 0.7 under the chop control word
// 0xC00), bad falls to @0x5A3DC9 without a read].
uint32_t friendly_tag_squad_color(int band, bool has_slot, uint8_t squad_index,
		uint32_t tier_rgb);

struct HudMessageLine {
	std::string text;
	int expire_tick = 0;
	// The stored packed ARGB. Retail stores the caller's color raw and the
	// SYSTEM ring draws THAT value; only the CHAT ring folds a computed alpha
	// over it [orig: the stored-color read @0x59ae97 vs the chat fold
	// @0x59adef]. `-1` (0xFFFFFFFF) is the triggered-text default
	// [orig: Chat_AddMessageChannel2(text, -1, 930) @0x51f216].
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
	bool raised = false; // [orig: g_SpawnSuccessGate && winner == 1 @0x5b763b]
	std::string title;   // gametext ("Score", "SCORE_TITLE")
	std::array<std::string, 4> labels;
	std::array<std::string, 4> values;
};

struct HudScoreboardState {
	bool shown = false;
	uint32_t game_type = 0;
	// (The PgUp/PgDn page is the compiler's own cursor — HudFrameCompiler::
	// scoreboard_page_step — because the drawer folds and writes it back
	// every draw [orig: g_ScoreboardPanelPage @0xA823C0, the fold @0x423c0f].)
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
	// host serializes from its g_NumTeamsConfig [orig: @0x50db3a ->
	// g_ScoreboardTeamCount @0x42fdda] — and the HUD frame counter, the
	// per-main-frame clock the LFP panel blinks on too [orig: dword_A87060,
	// Game_TickHudFrameCounters @0x434c14].
	int team_count = 0;
	int frame_counter = 0;
	// The row composer's session facts (hud_scoreboard.h ScoreboardRowContext):
	// the SU gate, solo KOTH's timed flag, the countdown minutes and the local
	// player's team byte (-1 without a local entity).
	bool status_suffix = false;
	bool timed = false;
	int time_limit = 0;
	int local_team = -1;
	// The class names and the team-score block's labels the embedder resolved
	// from gametext (hud_scoreboard.h scoreboard_class_name_key, the STRCLI
	// team/"(of" rows).
	ScoreboardClassNames class_names;
	ScoreboardHeaderText header_text;
	// The 0x16 team table, indexed by team 0..4 [orig: @0xA85AEC + 16t].
	std::array<ScoreboardTeamScore, 5> teams{};
	// The flag carrier line's facts: whether a carrier is latched
	// [orig: dword_A860C4], its entity name and team byte, and the
	// Overlays label.
	bool flag_carrier = false;
	std::string flag_carrier_name;
	uint8_t flag_carrier_team = 0;
	std::string flag_carrier_label;
};

// THE CHAT INPUT LINE (hud/hud_chat_entry.h owns the capture and its
// editing): the open capture's prompt, typed text, the dispatch's color
// (chat_input_line_color) and the per-main-frame counter the cursor blinks on
// [orig: StdCtype_Destructor (IDB misnomer) @0x5b8f30, called from
// HUD_DrawOverlayPanels @0x5c014e while g_InputCaptureMode is set and != 3].
struct HudChatInputState {
	bool shown = false;
	std::string prompt;
	std::string text;
	uint32_t color = 0xFFFFFFFFu;
	uint32_t frame = 0; // [orig: dword_A8705C]
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
	// How many times the label is drawn over itself: an emplacement's digit
	// repeats once per slot-list position its running emplacement count
	// matches (hud_vehicle_panel.h emplace_label_draws); every other label once.
	int label_draws = 1;
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
	// The HUD frame counter the blink masks (g_HUDFrameCounter & 0x18).
	int frame_counter = 0;
	// The game's AAS-vs-conquest arm: the conquest arm is unmodelled and the
	// element draws nothing under it [orig: g_GameType == 0x50010 @0x5a24a1].
	bool conquest_mode = false;
	std::string under_attack_text; // Overlays/STROVER_UNDERATTACK
	std::string ready_text;        // Overlays/STROVER_READYFORTAKEOVER
	std::vector<HudLfpZone> zones;
};

// The gametext strings the session lines draw, resolved once per frame by
// hud_game_text.h hud_session_text (the keys and sections are retail's own).
struct HudSessionText {
	std::string timer;             // Overlays/STROVER50
	std::string players_remaining; // Client/STRCLI25
	std::string players;           // Client/STRCLI04
	std::string spectators;        // Client/STRCLI23
	std::string in_the_zone;       // Overlays/STROVER53
	// The team names by team byte 0..4 then the unknown arm — STRCLI19, 05,
	// 06, 17, 18, 01 (05/06 are also the TKOTH timer labels).
	std::array<std::string, 6> team_names;
	std::string attacking;         // client/strcli20
	std::string defending;         // client/strcli21
};

// THE MP SESSION LINES' per-frame facts: the CLOCK timer and player count,
// the GAMEINFO/ZONEINFO overlay, the TEAMID line. The role feed
// (inmatch/role_feeds.h hud_role_facts) fills them from the replica and the
// authority's own state; each field names the retail global it stands for.
struct HudSessionState {
	bool in_session = false;             // g_NapiNPCtx.is_in_session
	uint32_t game_type = 0;              // g_GameType
	int32_t round_time_remaining = -1;   // g_RoundTimeRemaining (-1 = untimed)
	bool permanent_death = false;        // byte_A821EF
	int remaining_count = 0;             // g_ScoreboardDeadRowCount (the live count)
	int row_count = 0;                   // g_ScoreboardRowCount
	int spectator_count = 0;             // g_ScoreboardSpectatorCount
	// T: g_TimeLimitMinutes on the authority, the S2C 0x64 copy dword_A821C0
	// on a joiner [orig: HUD_DrawGameTimerOverlay @0x59CCDB..0x59CCEB].
	int32_t time_limit_minutes = 0;
	// Teams 1 and 2 of the 0x16 team table: score1 (movsx) and the KOTH hold
	// byte [orig: dword_A85AFC / dword_A85B08, dword_A85B0C / dword_A85B18].
	std::array<int32_t, 2> team_score1{};
	std::array<int32_t, 2> team_koth{};
	// CaptureZone_FindMaxProximityCoverage(local) [orig: @0x5BF4D0].
	int32_t zone_coverage = 0;
	int team = 0;                        // byte_27234FE (hudInfo+0x176)
	uint32_t attack_defend = 0;          // dword_B78FE8: 1 defending, 2 attacking
	// The death screen's spectate arm (dword_A860F4 && dword_A860F0): the HUD
	// info rebuilt for the target feeds the health bar and the TEAMID line —
	// `team` above carries the target's team byte then — plus the target's
	// name (+0xF4) and its health ratio (hudInfo+92, Health << 16 over the
	// max, capped at 1). Producer: inmatch::hud_role_facts.
	// [orig: HUD_RenderOverlays @0x5a7bc5..0x5a7c25; HUD_BuildEntityInfo
	//  @0x4b87a2..0x4b87d3]
	bool spectating = false;
	std::string spectated_name;
	float spectated_health_fraction = 0.0f;
	HudSessionText text;
};

// One weapon category's slot-bar scan result (world/hud_slot_bar_feed.h owns
// the witnessed scan). The icon is the device's texture at
// kHudTexSlotBarIcon0 + category.
struct HudSlotBarSlot {
	bool present = false; // the category holds a def-bearing slot
	int count = 0;        // def-bearing slots counted (the MOREAV gate is > 1)
	bool icon_valid = false;
	int icon_w = 0;       // def+0x1C0
	int icon_h = 0;       // def+0x1C4
	// KeyBinding_FormatDisplayString of binding record 200 + the def's
	// category (0x81AE08 + 108*def[0]): after the start-up re-lay that is the
	// catalog row dispatching 200 + category (controls action_for_code), the
	// zero record's "" where none does; the embedder resolves it from the live
	// bindings [orig: HUD_DrawWeaponSlotBar @0x599e8c..0x599e9f;
	// KeyBinding_SortBySequentialId @0x498260].
	std::string key_label;
};

// The /NOHUD overlay master word: the .data init 3, cleared to 0 by the
// launch flag [orig: dword_840B18 (static 3); Game_ParseCommandLineAndInit
// @0x4a7310 -> sub_58FF20(0) @0x4A7A09, its only writer].
inline constexpr uint32_t kHudOverlayMasterDefault = 3;
inline uint32_t hud_overlay_master(bool no_hud) {
	return no_hud ? 0u : kHudOverlayMasterDefault;
}

struct HudFrameState {
	HudCombatState combat;
	int ticks = 0;
	// The /NOHUD master word (hud_overlay_master): bit 1 gates the gameplay
	// overlay pass after its palette/radar work [orig: HUD_RenderAllOverlays
	// `test dword_840B18, 2` @0x5A81CE]; the whole word gates
	// HUD_DrawGameplayOverlays [orig: `cmp dword_840B18, 0` @0x5BDE9B].
	uint32_t overlay_master = kHudOverlayMasterDefault;
	HudSessionState session;
	// The two squad order lines S2C 0x72 wrote (replication ClientState
	// squad_orders) [orig: byte_2721DB8, two 128-byte lines].
	std::array<std::string, 2> squad_orders;
	std::array<HudSlotBarSlot, 10> slot_bar;
	// THE RECENT MESSAGES (J) WINDOW: the OldMessages toggle and its stdbox
	// title (Overlays/STROVER43, resolved by the embedder like the scoreboard's)
	// [orig: g_ShowMessageLog @0x24C18C0; the title @0x5b9e54].
	bool message_log_shown = false;
	std::string message_log_title;
	KillAnnouncement kill_announcement;
	// THE AAS ZONE STATUS PANEL (hud/hud_lfp_panel.h owns its policy).
	HudLfpPanelState lfp_panel;
	float health_fraction = 1.0f;
	int stance = 0;
    int mount_slot = 0, weapon_category = 0; // HUD info +560 and WeaponDef+0
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
	// The objectives-panel header line, resolved by the embedder from gametext
	// (Overlays/STROVER_MISSIONOBJECTIVES); a miss is GameText_GetString's ""
	// and draws nothing [orig: header STROVER_MISSIONOBJECTIVES @ 0x5ba986;
	// the miss @0x51ec00].
	std::string objectives_header;
	// The objectives-panel alpha byte, folded into every panel draw color's
	// top byte [orig: dword_24C18CC — 0/255, flipped ^= 0xFF by the co-op
	// input action @ 0x49b68b]. The presenter's show/hide toggle stands in
	// for the input binding row (identical visually: alpha 0 draws nothing).
	uint32_t objectives_alpha = 0xFF;
	HudWeaponState weapon;
	HudScopeState scope;
	// The SIGHTS card's per-PLAYER inputs (they outlive the weapon record,
	// which clear_weapon resets): the sight-scale index retail keeps in
	// dword_B76780 (default at player init, cycled by the dotsize action —
	// sight_overlay.h next_sight_scale_index) and the scope-zero slide
	// multiplier (sight_overlay.h sight_slide_multiplier; 0 until the
	// scope-zero state has a port).
	int sight_scale_index = kSightScaleIndexDefault;
    int aspect_mode = -1;
	int32_t sight_slide_multiplier = 0;
	HudWaypointState waypoint;
	std::vector<HudObjectiveRow> objectives;
	// THE MOUNTED-VEHICLE PANEL (hud/hud_vehicle_panel.h owns its policy).
	// Present only while the local player rides something; the shell resolves
	// the item's VEHICLE_HUD block and the live seat occupancy, because both
	// need tables the compiler does not own.
	HudVehiclePanelState vehicle_panel;
	// The Tab board (hud/hud_scoreboard.h owns its policy).
	HudScoreboardState scoreboard;
	// The open chat capture's input line.
	HudChatInputState chat_input;
	// The key-toggled windows HUD_DrawGameplayOverlays draws after the
	// objectives (hud/hud_overlay_windows.h): the F1 key-binding help, the
	// F12 map legend (it wins over the help list), the I briefing panel.
	HudHelpScreenState help_screen;
	HudMapLegendState map_legend;
	HudBriefingState briefing;
	// The overlay-panel pass's voice-macro menus (hud_overlay_windows.h).
	HudVoiceMacroMenuState emotes_menu;
	HudVoiceMacroMenuState radio_menu;
	// THE SP PAUSE TEXT: while the pause word is set the overlay-panel pass
	// draws gametext Overlays/STROVER7 (resolved by the embedder) right-aligned
	// in the Impact38 slot at PAUSEDPOS, or (1000, 4) when either field is
	// zero, in g_HUDColors.active [orig: HUD_DrawOverlayPanels @0x5c0120 ->
	// HUD_DrawPausedText @0x59d650 (ex sub_59D650)].
	bool paused = false;
	std::string paused_text;
	// THE QUIT DIALOG (hud_toggles.h HudToggleState::quit_dialog_open): a
	// stdbox (280, 340)-(744, 428) with gametext Overlays/STROVER_QUITSERVER on
	// an authority in a session, _QUITCLIENT for a joiner, STROVER5 out of a
	// session (the embedder resolves it), centred at (512, 364) in the Impact38
	// slot in g_HUDColors.active. The scene frame's gameplay overlays draw it
	// below the blank declutter level; the status page draws it unconditionally.
	// [orig: UI_DrawDisconnectReasonDialog @0x5b8eb0 — from
	//  HUD_DrawGameplayOverlays @0x5be1ce (the level-3 skip @0x5be185) and
	//  Server_DrawStatusScreen @0x50b270]
	bool quit_dialog_open = false;
	std::string quit_dialog_text;
	// THE TIP ("MrClippy", tip_system.h): the showing tip and its countdown,
	// the Tips header and the expanded body the embedder resolved, and the
	// local player's dead bit. The scene frame draws it after the HUD pass:
	// at MRCLIPPYNORMAL with the M-cycle map closed, else at MRCLIPPYALTERNATE
	// right after the big map, and not at all on the frame a dead player's
	// map mode is cleared [orig: Render_ProcessMainSceneFrame
	// @0x5cac50..0x5cad28 -> CTipSystem_Draw @0x5b6d60].
	int32_t tip = 0;
	int32_t tip_countdown = 0;
	std::string tip_header;
	std::string tip_body;
	bool local_dead = false;
	// THE END-OF-ROUND OVERLAY (net-re §5.68): the resolved Impact38 text
	// ladder the presenter built from hud/end_round_overlay.h, drawn inside
	// the stdbox (8, top+8, 1015, bottom-8) of the overlay safe area
	// [orig: HUD_DrawEndRoundStatsOverlay @0x5b7cd0 — HUD_DrawLabelBox
	//  @0x5b7d3e, each line HUD_DrawTextCentered_HalfBright(Impact38, 512, y)
	//  through HUD_DrawTextCenteredScaled (ex sub_580B80) @0x580b80].
	HudEndRoundOverlayState end_round;
	// The toggled SP "Show Score" statistics panel [orig: HUD_DrawOverlayPanels @0x5c0083
	// draws HUD_DrawEndRoundStatistics @0x5b7600 while dword_24C18AC].
	HudEndRoundStatisticsState end_round_statistics;
	std::vector<HudAttachLabel> attach_labels;
	// Friendly tags (D-HUD-20). Mode default 2 = FULL [orig: Game_Run
	// @ 0x4a7fed]; fog cull against the environment's current fog distance
	// [orig: g_EnvFogDistCurrent @ 0x5a3b28]; one speaking level shared by the
	// (single) speaking entity [orig: g_AudioOutLevelStage1].
	std::vector<HudFriendlyTag> friendly_tags;
	int friendly_tag_mode = 2;
	int32_t fog_dist_q16 = INT32_MAX;
	int speaking_level255 = 0;
	// The radio-request icon's VIEWER gate: the local player's mount state
	// (+0x168) is a Controller (2) or Driver (5) seat, or the local player
	// carries its own +885 latch [orig: HUD_DrawEntityLabel @0x5a3bba..
	// 0x5a3be8 -- `mov ecx, [eax+168h]; cmp ecx, 2; ...; cmp ecx, 5`
	// @0x5a3bcb..0x5a3bd9, `cmp byte ptr [eax+375h], 0` @0x5a3bdf].
	bool radio_request_icon_viewer = false;
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
	// The HUD item flash timers (hud_declutter.h HudItemFlash) the blinking
	// items read this frame.
	std::array<int32_t, HudItemFlash::kCount> item_flash{};
	// The showhud 2-bit FP-view flags [orig: g_FpWeaponViewFlags — cycle
	// (flags + 1) & 3 @ 0x4E0561]: bit 0 gates the FP gun/viewmodel draw
	// (consumed device-side where the viewmodel submits), bit 1 gates the
	// FP-weapon sub-pass + the corner spinmap block [orig: test 2 @ 0x5A8635].
	// Default 3 = gun + spinmap (the cfg gun-visible option writes 3/2
	// [orig: @ 0x5521CB/@ 0x5521D7]).
	uint32_t showhud_flags = 3;
	// THE BREATH BAR (element_breath_bar): the underwater breath samples the
	// host counts four a second and ships in the S2C 0x0A player state
	// [orig: word_A85B7C, written only by NapiNPClientMsg_0x00A @0x430104], the
	// drown limit's seconds [orig: g_WacVarBreathTime, from the same message
	// @0x4301A1; 20 from WacScript_FreeAll @0x4f6381], the round-over latch the
	// caller skips the bar under [orig: g_SpawnSuccessGate, tested
	// @0x5BDECA..0x5BDED1], and the label the embedder resolves from gametext
	// (Overlays/STROVER91).
	int breath_samples = 0;
	int breath_time = 20;
	bool spawn_success_gate = false;
	std::string breath_label;
	HudMinimapInput minimap;
	// The mission's static footprint polygons (baked once per feed); the
	// spinmap element lends them to the compile input by pointer — the
	// per-frame input copy must never clone the triangle set.
	std::vector<HudMinimapFootprint> map_footprints;
	// The non-bank map legs' feed (zone labels, pool-3 rings, player slots,
	// the tracked callout), lent to the compile input by pointer.
	HudMinimapOverlays map_overlays;
	// The connection indicators (net_quality_indicators.h): the session gate,
	// the NovaWorld icon's gate and the g_NetQuality display state the role's
	// replica runtime keeps.
	HudNetQualityState net_quality;
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
	// The flat entries from these indices on belong to the gameplay-overlay
	// windows HUD_DrawGameplayOverlays draws AFTER the big map (the device
	// leg layers them above it) [orig: Render_ProcessMainSceneFrame — the big
	// map @0x5cad15, HUD_DrawGameplayOverlays @0x5cae0b].
	struct TopBegin {
		size_t quads = 0;
		size_t tris = 0;
		size_t lines = 0;
		size_t glyphs = 0;
		size_t underlines = 0;
	} top_begin;
	// Kind-grouping restarts in the flat HUD below top_begin: the device leg
	// submits each run between consecutive breaks as its own quads / tris /
	// lines / glyphs group, so an element that must layer in the walk's
	// order (the vehicle-bay logos over what came before and under what
	// comes after) is not lifted above the later quads. Each break is the
	// kind cursors at the point it was marked.
	std::vector<TopBegin> order_breaks;
	int64_t elements_drawn = 0;
	// Which element of the walk emitted each run of the flat lists (and drew a map pass), in the
	// walk's order: hud_elements.h, a tool's record of the walk (what lies under a point of the HUD),
	// never read by a draw.
	std::vector<HudElementSpan> element_spans;
};

// Draw-list font-page namespaces: each compiler font emits glyph pages at
// slot * FNT_MAX_PAGES, so one flat draw list mixes faces and the device leg
// indexes its page-texture table the same way.
inline constexpr int kHudFontSlotHud = 0;       // the hudpos-named HUD font
inline constexpr int kHudFontSlotLabel = 1;     // g_HUDLabelFont (Arial normal)
inline constexpr int kHudFontSlotLabelBold = 2; // the bold slot (fontObj @ 0xB4C394)
inline constexpr int kHudFontSlotLabelLarge = 3; // g_HUDLabelFontLarge (Impac22b)
inline constexpr int kHudFontSlotImpact38 = 4;   // g_HUDLabelFontImpact38 (Impac38b)
inline constexpr int kHudFontSlotCount = 5;
// The device leg sizes its page-texture table by the count; a slot past it
// writes Ref<> handles off the end of that table (the 2026-08-24 load crash).
static_assert(kHudFontSlotImpact38 < kHudFontSlotCount,
              "every font slot must index inside kHudFontSlotCount");

// Deep in-process module: the whole witnessed element walk, stance cross-fade
// state, the clip-indicator flash state, and the triggered-text message ring
// live here; compile() emits everything for one frame in retail's order.
bool hud_weapon_group_visible(const HudFrameState &state);
bool hud_stance_group_visible(const HudFrameState &state);

class HudFrameCompiler {
public:
	// `font` is the hudpos font loaded for the current width (null when hudpos
	// names none or the load failed); see set_hudpos_font for the HUD slot rule.
	void configure(const HudLayout &layout, const opennova::fnt::fnt_font_t *font);

	// The HUD font slot: the hudpos font at scale 1.0 when one loaded, else a
	// copy of the bold label slot, font and width scale alike
	// [orig: HUD_SelectHudposFont @0x591890: HUD_LoadFontIntoSlot(name, slot,
	// 0x10000) when the name is set @0x5918B4; a slot still empty takes
	// g_HUDLabelFontBold @0x5918C3..0x5918D6]. The overlay reloads it with the
	// label fonts on a width-tier change, as HUD_InitAllFonts @0x51EE20 does.
	void set_hudpos_font(const opennova::fnt::fnt_font_t *font);

	// The overlay label fonts + their resolution scales — the Arial pair and
	// the large slot retail loads beside the hudpos HUD font
	// [orig: HUD_InitAllFonts @ 0x51ee20: g_HUDLabelFont = Arial14n/16n, the
	// bold slot (fontObj @ 0xB4C394) = Arial12b/14b/16b at scale
	// (screenW<<16)/{640,800,1024}; g_HUDLabelFontLarge @0xB4C3A0 =
	// Impac22b.fnt at the over-800 scale; the slot carries
	// {font, scale_x, scale_y} @ 0x580453..0x580468]. Friendly tags draw
	// with the normal face [orig: @ 0x5a3a0c], attach labels with the bold
	// face [orig: @ 0x5a3680/@ 0x5a38a1], the big-map grid labels with the
	// large face. Null fonts fall back to the HUD slot (the hudpos font at
	// scale 1, or the bold copy at its scale) so layout-only embedders keep drawing.
	void configure_label_fonts(const opennova::fnt::fnt_font_t *normal, const opennova::fnt::fnt_font_t *bold,
			const opennova::fnt::fnt_font_t *large, float scale, float large_scale,
			const opennova::fnt::fnt_font_t *impact38 = nullptr);

	// Swap the layout WITHOUT resetting runtime state (stance fade, clip
	// flash, the message ring) — the texture-table refresh path, e.g. the
	// options crosshair-style reload [orig: HUD_LoadAllTextures @ 0x59e3d6
	// re-registers textures without touching the live HUD state].
	void update_layout(const HudLayout &layout);

	// The stance cross-fade restamp [orig: @ 0x599f8a] and the message ring
	// [orig: Chat_AddMessageChannel2 @ 0x4987f0] are compiler state.
	// Mission triggered text — posts into the one system ring with the default
	// white [orig: Chat_AddMessageChannel2(text, -1, 930) @0x51f216].
	void push_message(const std::string &text, int now_ticks);
	// Post one line to the SYSTEM feed — the ring every Chat_AddMessageChannel2
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
	// The two rings' live history for the message-log window: every held
	// line, oldest first, NO expiry test [orig: HUD_DrawMessageLog
	// @0x5b9d70 walks slots 16..1 of both rings @0x5b9e8a..0x5b9f1a].
	const std::vector<HudMessageLine> &chat_lines() const { return chat_lines_; }
	void reset_runtime_state();
	// Respawn clears the overlay clocks, retaining the chat/system rings and
	// the previous stance/ammo values [orig: HUD_ResetAllOverlayBuffers
	// @0x59dd40]. Of that routine the compiler holds state for exactly two
	// words: stance_.stamp is dword_2723D38 and flash_stamp_ is dword_2723D48
	// (cleared @0x59dd8f / @0x59dd89), the stamp stores of
	// HUD_DrawStanceIndicator @0x599f98 and HUD_DrawAmmoIndicator @0x599aca;
	// their prev/cur bytes (byte_2723D3C/D3D @0x599f8c/@0x599f92,
	// byte_2723D4C @0x599ac5) are retained as in retail. The rest of the
	// routine has no compiler-side state: the missile list unk_2722B40
	// (memset @0x59dd4e), the radar blip table unk_2721F40 + the edge timers
	// dword_2721EEC/EF0 (@0x59dd5e..0x59dd6a) and the radar tick
	// dword_2723EAC (@0x59dd83) are the local player's radar state, reset by
	// the round init (world::radar_reset); the layer visibility call
	// @0x59dd75, dword_2723EA8 (@0x59dd7d) and the target-overlay stamp
	// dword_2723D58 (@0x59dd95) have none here.
	void reset_overlay_buffers();

	// The HUD-owned gates the radar legs ride, as bits the embedder hands the
	// session's per-frame radar step (inmatch/session.h; world::radar_hud_frame):
	// bit 0 the overlay pass runs past its hud_detail-3 early-out (the
	// spawn-success early-out the session reads live), bit 1 the corner
	// spinmap also draws with mask bits 9, 6 and 10, so its own update site
	// runs [orig: HUD_RenderAllOverlays @0x5a80c4; the spinmap gates
	// @0x5a8635 / @0x5a86e8; HUD_DrawMapOverlay @0x5a78ff..0x5a790f].
	static constexpr uint32_t kRadarGatePass = 1u;
	static constexpr uint32_t kRadarGateMapSite = 2u;
	uint32_t radar_frame_gates(const HudFrameState &state) const;

	// THE CONNECTION INDICATORS' DRAWER, shared by both of retail's callers
	// [orig: CNetQuality_DrawIndicators @0x4c3200]: the HUD compile's last
	// element (force_default_pos false: the hudpos corners) and the listen
	// host's server-status page (force_default_pos true: the reset corners
	// (4, 4) / (20, 4) / (52, 4)) [orig: Server_DrawStatusScreen
	// @0x50b2a8..0x50b2af]. It appends to the list being built (after a
	// compile, last_draw_list()): the server-status page's compile calls it
	// with the frame's HudNetQualityState and its own /NOHUD word.
	void emit_net_quality_indicators(const HudNetQualityState &net, uint32_t overlay_master,
			bool force_default_pos, float w, float h);

	const HudDrawList &compile(const HudFrameState &state, float surface_w,
			float surface_h);

	const HudDrawList &last_draw_list() const { return draw_list_; }

	// The menu map windows (hud_map_view.h), compiled on the menu's
	// custom-draw call rather than in the HUD walk, over this frame state's
	// terrain / markers / footprints, their labels laid out in the label fonts
	// like the other map passes. The glyphs ride their own list so the host's
	// pass sandwich layers them.
	struct MapWindowDraw {
		HudMapWindowPass pass;
		std::vector<GameFontQuad> glyphs;
		// The zone walk's letters, laid out per segment: zone_glyph_ends[i] is
		// the end of segment i's glyphs (hud_map_view.h HudMapWindowSegment).
		std::vector<GameFontQuad> zone_glyphs;
		std::vector<size_t> zone_glyph_ends;
		// The CMAP's placed waypoints as this render projected them, each
		// name measured in the bold label slot (hud_map_view.h
		// command_map_waypoint_hover / command_map_close_button_position).
		std::array<CommandMapWaypointAnchor, kCommandMapWaypointSlots> waypoint_anchors;
	};
	// The DEATH deploy screen's MAP window [orig: CMap_OverlayInputHandler
	// event 1 @0x554310 -> MapOverlay_DrawView @0x5a58e0].
	const MapWindowDraw &compile_death_map(const HudFrameState &state,
			const DeathMapFrame &frame, const DeathMapFacts &facts, float surface_w,
			float surface_h);
	// THE SERVER-STATUS PAGE (hud_server_status.h), drawn INSTEAD of the scene
	// frame while the authority's status view is up. The page's throttle
	// decides whether it redraws: false leaves the previous page list in
	// place (retail presents nothing that frame). `now_ms` is the wall clock
	// in milliseconds, `window_active` the window's focus.
	// [orig: Server_DrawStatusScreen @0x50a2d0]
	bool compile_server_status_page(const HudFrameState &state, const ServerStatusPageState &page,
			uint32_t now_ms, bool window_active, float surface_w, float surface_h);
	const HudDrawList &server_status_page_list() const { return status_page_list_; }
	// Whether compile_server_status_page would draw now (the stamp untouched).
	bool server_status_page_due_now(uint32_t now_ms, bool window_active) const {
		return server_status_page_due_at(status_last_draw_ms_, now_ms, window_active);
	}
	// The CMAP MAP / ORDERS_MAP window [orig: CMapWindow_HandleEvent event 1
	// @0x5497f0 -> HUD_BuildMapOverlayView mode 4 @0x5a7e10].
	const MapWindowDraw &compile_command_map(const HudFrameState &state,
			CommandMapView &cmap, const MapViewRect &rect, int32_t scaled_800,
			const DeathMapFacts &facts, float surface_w, float surface_h);
	// The console messages alone (the chat and system rings at their HUD
	// anchors, the HUD call's gates), for CMAP's CHAT_MSGS slot; their own
	// list, so last_draw_list() is untouched [orig: CMap_OnChatMsgsCustomDraw
	// @0x5482d0].
	const HudDrawList &compile_console_messages(const HudFrameState &state, float surface_w,
			float surface_h);
	// The briefing panel's page state (the page keys walk it; the drawer
	// writes each overflowing page's successor start).
	HudBriefingPages &briefing_pages() { return briefing_pages_; }
	// THE TAB BOARD'S PAGE [orig: g_ScoreboardPanelPage @0xA823C0]: the page
	// keys step it (hud_scoreboard.h scoreboard_takes_page_keys gates them
	// [orig: `sub/add g_ScoreboardPanelPage, 1` @0x49c917/@0x49c93e]), the
	// open edge zeroes it [orig: Scoreboard_TogglePlayerList @0x4244e4], and
	// the drawer folds it into range and writes it back [orig: @0x423c0f].
	void scoreboard_page_step(int delta) { scoreboard_page_ += delta; }
	void reset_scoreboard_page() { scoreboard_page_ = 0; }
	int scoreboard_page() const { return scoreboard_page_; }

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
	// One g_HUDColors.palette entry 0..5 as the per-frame refresh leaves it:
	// entry 2 is the hudpos hud_textcolor [orig: HUD_RenderAllOverlays
	// @0x5A810C / HUD_DrawGameTimerOverlay @0x59CCB8].
	uint32_t hud_palette(int index) const;
	void emit_rect(float x0, float y0, float x1, float y1, uint32_t color,
			bool filled, int32_t texture = kHudTexNone, bool additive = false);
	void emit_rect_uv(float x0, float y0, float x1, float y1, float u0, float v0,
			float u1, float v1, uint32_t color, int32_t texture);
	// The retail stdbox panel and the per-row connection icon. The box takes
	// the surface width because its pieces and fill inset scale with it, and
	// the box's alpha (0..255), which its diffuse alpha<<24 | 0x7F7F7F carries.
	// title_gap_w > 0 draws the TITLED top row: the row-3 stub / title-bar /
	// end-cap cells around a gap of that many output pixels
	// [orig: the outTechnique arm @0x56b937, cells rec+0x108/0x120/0x138].
	void emit_stdbox(float x0, float y0, float x1, float y1, float surface_w,
			uint32_t alpha, float title_gap_w);
	void emit_stdbox_piece(float x0, float y0, float x1, float y1, int col,
			int row, bool crop_bottom, uint32_t color, int32_t texture = kHudTexBoxBorder);
	// The eight border pieces of a box style over its atlas `texture`, each
	// piece cw x ch output pixels (title_gap_w as in emit_stdbox).
	void emit_box_pieces(float x0, float y0, float x1, float y1, float cw, float ch,
			uint32_t color, float title_gap_w, int32_t texture);
	void emit_net_icon(float x0, float y0, float x1, float y1, int quality);
	void emit_wire_rect(float x0, float y0, float x1, float y1, uint32_t color);
	// The three-quad progress bar in surface pixels: the border rect, the
	// opaque black rect one pixel in, then the fill two pixels in, `fraction`
	// of the inner width, centred on the bar's integer midpoint or anchored at
	// its left [orig: HUD_DrawProgressBar @0x59B340].
	void emit_progress_bar(int xl, int yt, int xr, int yb, uint32_t fill, uint32_t border,
			float fraction, bool centered);
	void emit_text(const char *text, float design_x, float design_y,
			float surface_w, float surface_h, uint32_t argb, uint32_t flags);
	// One run in an overlay font SLOT at a surface anchor: the slot's scale
	// pair rides into the draw. A slot whose file is absent falls back to the
	// HUD slot (font_) at its own scale, like every other label element.
	// [orig: HUD_DrawTextLeft_HalfBright @0x5804C0 -- slot scales @0x58052B /
	//  @0x580539; HUD_DrawTextCentered_HalfBright @0x580680]
	void emit_slot_text(const GameFont &slot, float slot_scale, const char *text,
			float surface_x, float surface_y, uint32_t argb, uint32_t flags);
	// A draw through HUD_DrawTextAtVirtualPos: the design point scaled with
	// the integer (x*W + 512)/1024, (y*H + 384)/768 rounding, then the
	// half-bright drawer the mode picks — 0 left, 1 right, 2 centred, any
	// other mode draws nothing [orig: HUD_DrawTextAtVirtualPos @0x5d3ec0 ->
	// sub_5D2EA0 @0x5d2ea0].
	void emit_text_at_virtual_pos(const GameFont &slot, float slot_scale, const char *text,
			int design_x, int design_y, float w, float h, uint32_t argb, int mode);
	// A draw through HUD_DrawTextAligned: the same integer scaling, then the
	// half-bright drawer by mode with the caller's flag word (0x100 turns the
	// inline tags off) [orig: HUD_DrawTextAligned @0x5d3f30 ->
	// HUD_DrawTextAligned_HalfBright @0x5d2f20].
	void emit_text_aligned(const GameFont &slot, float slot_scale, const char *text,
			int design_x, int design_y, float w, float h, uint32_t argb, int mode,
			uint32_t flags);
	// The same half-bright drawer pick at a SCREEN point [orig: sub_5D2EA0].
	void emit_half_bright_text(const GameFont &slot, float slot_scale, const char *text,
			float screen_x, float screen_y, uint32_t argb, int mode);
	float measure_text_w(const char *text) const;
	float text_line_h() const;

	void element_scope_details(const HudFrameState &state, float w, float h);
	void element_frame(float w, float h);
	void element_health(const HudFrameState &state, float w, float h);
	void element_stance(const HudFrameState &state, float w, float h);
	void element_weapon_cluster(const HudFrameState &state, float w, float h);
	void element_heat(const HudFrameState &state, float w, float h);
	void element_power(const HudFrameState &state, float w, float h);
	void element_breath_bar(const HudFrameState &state, float w, float h);
	void element_waypoint(const HudFrameState &state, float w, float h);
	void element_spinmap(const HudFrameState &state, float w, float h);
	// A map pass's labels through the CPU half-bright drawer, bold, large or
	// regular slot per label; the page's MODULATE2X doubles them on the device.
	void layout_map_labels(const HudMapPass &pass, std::vector<GameFontQuad> &out) const;
	// The same over pass.labels[begin, end) (the DEATH zone walk's segments).
	void layout_map_labels(const HudMapPass &pass, size_t begin, size_t end,
			std::vector<GameFontQuad> &out) const;
	bool corner_spinmap_visible(const HudFrameState &state) const;
	void element_objectives(const HudFrameState &state, float w, float h);
	void element_attach_labels(const HudFrameState &state);
	void element_friendly_tags(const HudFrameState &state);
	// The MP session lines (hud_frame_session_lines.cpp) and the HUDLS bar
	// (hud_frame_slot_bar.cpp).
	void element_game_info(const HudFrameState &state, float w, float h);
	void element_clock(const HudFrameState &state, float w, float h);
	void element_team_id_line(const HudFrameState &state, float w, float h);
	void element_weapon_slot_bar(const HudFrameState &state, float w, float h);
	void element_feed(const HudFrameState &state, float w, float h);
	HudDrawList console_draw_;
	void element_squad_orders(const HudFrameState &state, float w, float h);
	void element_message_log(const HudFrameState &state, float w, float h);
	// The overlay-panel pass's voice-macro menus and the pause text
	// (hud_frame_overlay_windows.cpp).
	void element_voice_macro_menu(const HudFrameState &state, const HudVoiceMacroMenuState &menu,
			int context_row, float w, float h);
	void element_paused_text(const HudFrameState &state, float w, float h);
	// The tip panel (hud_frame_tip.cpp); `alternate` picks MRCLIPPYALTERNATE.
	void element_tip(const HudFrameState &state, bool alternate, float w, float h);
	void compile_overlay_panel_menus(const HudFrameState &state, float w, float h);
	void element_briefing(const HudFrameState &state, float w, float h);
	void element_help_screen(const HudFrameState &state, float w, float h);
	void element_map_legend(const HudFrameState &state, float w, float h);
	// HUD_DrawLabelBox over design corners: the stdbox, and a non-empty title
	// just inside its corner in the bold slot, half-bright [orig:
	// HUD_DrawLabelBox @0x51efd0 — the title gap @0x51f0ea..0x51f114, the
	// title HUD_DrawTextLeft_HalfBright at (x + 15, y + 2)].
	void emit_label_box(float x1, float y1, float x2, float y2, const std::string &title,
			uint32_t title_color, float w, float h);
	void mark_top_layer();
	void compile_overlay_pass(const HudFrameState &state, float w, float h);
	void compile_gameplay_overlay_windows(const HudFrameState &state, float w, float h);
	void element_lfp_panel(const HudFrameState &state, float w, float h);
	void element_scoreboard(const HudFrameState &state, float w, float h);
	// The chat input line at design (50, y): the scene frame's panels pass
	// y 608, the status page 480.
	void element_chat_input(const HudFrameState &state, float w, float h, float y = 608.0f);
	void element_quit_dialog(const HudFrameState &state, float w, float h);
	void element_server_console_lines(const ServerStatusPageState &page, float w, float h);
	void element_player_score_list(const ServerStatusPageState &page, float w, float h);
	void element_end_round_overlay(const HudFrameState &state, float w, float h);
	void element_kill_announcement(const HudFrameState &state, float w, float h);
	void element_end_round_statistics(const HudFrameState &state, float w,
			float h);
	void element_vehicle_panel(const HudFrameState &state, float w, float h);
	void element_sights_card(const HudFrameState &state, float w, float h);
	void element_service_prompt(const HudFrameState &state, float w, float h);
	void element_inset_cues(const HudFrameState &state, float w, float h);
	void element_optical_cues(const HudFrameState &state, float w, float h);
	// The in-world vehicle-bay logos (hud_bay_logos.h) through the marker
	// drawer's types 5/6/7 [orig: HUD_DrawVehicleBayLogos @0x5a2c00 ->
	// HUD_DrawEntityMarker @0x593140].
	void element_vehicle_bay_logos(const HudFrameState &state, float w, float h);
	// The capture-point labels (hud_capture_labels.h) through the marker
	// drawer's types 8/9 [orig: Render_CapturePointLabels @0x5a2840 ->
	// HUD_DrawEntityMarker @0x593140, HUD_DrawProgressBar @0x59B340].
	void element_capture_point_labels(const HudFrameState &state, float w, float h);
	// HUD_DrawTexturedQuadCentered over an alpha-mode-0 shader: design centre
	// and extent, two textured triangles (hud_optical_cues.cpp carries the
	// witness) [orig: HUD_DrawTexturedQuadCentered @0x5909E0].
	void emit_textured_quad_centered(int32_t cx, int32_t cy, int32_t qw, int32_t qh,
			int32_t texture, uint32_t diffuse, float w, float h);
	// The connection indicators (hud_frame_net_quality.cpp): the HUD's call,
	// last in the frame and only at hud_detail level 0 [orig:
	// Render_ProcessMainSceneFrame @0x5cae4d..0x5cae5d]; the drawer is public
	// (emit_net_quality_indicators).
	void element_net_quality_indicators(const HudFrameState &state, float w, float h);
	void mark_order_break();
	// One element of the walk, `draw` its call: what it emitted recorded as its span
	// (HudDrawList::element_spans) when it drew anything.
	template <typename Draw>
	void element_(HudElement element, Draw &&draw) {
		const HudDrawCursor from = draw_cursor_();
		const bool map = draw_list_.map.visible, big_map = draw_list_.big_map.visible;
		draw();
		note_element_(element, from, !map && draw_list_.map.visible, !big_map && draw_list_.big_map.visible);
	}
	HudDrawCursor draw_cursor_() const;
	void note_element_(HudElement element, const HudDrawCursor &from, bool map, bool big_map);
	void element_targeting(const HudFrameState &state, float w, float h);
	void element_instruments(const HudFrameState &state, float w, float h);
	void element_crosshair(const HudFrameState &state, float w, float h);
	void element_clip_indicator(const HudFrameState &state, float w, float h);

	void resolve_hud_font_();

	HudLayout layout_{};
	// The HUD slot (resolve_hud_font_): the hudpos font, or the bold label font.
	GameFont font_;
	float hud_font_scale_ = 1.0f;
	const opennova::fnt::fnt_font_t *hudpos_font_ = nullptr;
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
	// The menu map window passes (compile_death_map / compile_command_map)
	// and their persistent scratch.
	DeathMapCompiler death_map_compiler_;
	CommandMapCompiler command_map_compiler_;
	HudMinimapInput map_window_input_;
	MapWindowDraw death_map_draw_;
	MapWindowDraw command_map_draw_;
	StanceFade stance_;
	// The clip-indicator flash latch: the (bucket, reserve, class byte) key
	// and the stamp, BSS-zero like retail's [orig: dword_2723D40 / dword_2723D44
	// / byte_2723D4C, stamp dword_2723D48; HUD_DrawAmmoIndicator @0x599A96..0x599ACA].
	int32_t flash_key_bucket_ = 0;
	int32_t flash_key_reserve_ = 0;
	uint8_t flash_key_class_ = 0;
	int flash_stamp_ = 0;
	// THE SNAPSHOT TWIN g_HUDColors.active @0x24C1868: seeded table[index] |
	// 0xFF000000 at HUD init [orig: HUD_InitTeamColorTable @0x51F240], rewritten
	// table[index] by the hudcolor cycle [orig: @0x49AFC7] and restamped
	// palette[index] (no alpha OR) every frame by the GAMEINFO drawer
	// [orig: HUD_DrawGameTimerOverlay @0x59CCD0] — so under /NOHUD or the
	// blank level the last restamp persists.
	uint32_t hud_colors_active_ = 0;
	int hud_colors_active_index_ = -1; // -1 = not seeded yet
	int silhouette_stamp_ = 0;
	uint64_t silhouette_vehicle_ = 0;
	std::string silhouette_weapon_;
	std::vector<HudMessageLine> feed_lines_;   // the SYSTEM ring
	std::vector<HudMessageLine> chat_lines_;   // the CHAT ring (S2C 0x14)
	// The CHAT channel's RAW ring beside its display buffer: every posted
	// line unwrapped and cut at 119 characters, newest last [orig:
	// Chat_AddMessageChannel1 @0x4985d0 — the shift into byte_B3EA38's slots
	// and the copy @0x498621]. Only a dedicated host's status page reads it
	// (HUD_DrawServerConsoleLines).
	std::vector<HudMessageLine> chat_raw_lines_;
	HudBriefingPages briefing_pages_;
	int scoreboard_page_ = 0;
	// The status page's own state [orig: byte_24C10A0+0x2C, the last draw
	// stamp; +0x24, the ticker counter 0..31] and its last compiled list.
	uint32_t status_last_draw_ms_ = 0;
	int status_ticker_ = 0;
	HudDrawList status_page_list_;
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

// The block wrapper's layout [orig: HUD_DrawWrappedText @0x580C00], in screen
// pixels: the width budget is (x_end - x_start) / scale_x font units
// @0x580c19..0x580c40, the line step the scaled height of 'I', truncated
// @0x580c68..0x580c8f (an empty line steps half of it @0x580c93 /
// @0x580d6a..0x580d76). Each character extends the measured run
// @0x580cff..0x580d0e; past the budget the line breaks at the last space,
// else at the overflowing character, which is dropped @0x580d2b..0x580d41;
// a CR (and a following LF) ends a line @0x580d43..0x580d4f /
// @0x580e62..0x580e6c. Lines before `skip_lines` count but neither draw nor
// advance @0x580d59..0x580d64. Returns 0 at the text's end @0x580e44 /
// @0x580e84, else the index of the first line that does not fit above
// `y_limit` (unless y_start == y_limit) @0x580e4a..0x580e60 / @0x580e9c.
int wrapped_text_layout(const GameFont &font, float scale_x, float scale_y,
		const std::string &text, int x_start, int y_start, int x_end, int y_limit,
		int skip_lines, std::vector<HudWrappedLine> &out);

} // namespace opennova::hud
