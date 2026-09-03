#pragma once

// Portable retail HUD spinmap compiler. It turns mission-space state and the
// TRN sector routing table into the heading-up circular map: backing disc,
// terrain mesh, ordered marker primitives, waypoint tether + distance label,
// and the counter-rotating compass ring. The embedder only binds the
// colormap/icon textures and submits these primitives.
// [orig: HUD_RenderAllOverlays @0x5A8070 -> HUD_DrawMapOverlay @0x5A5F40;
//  render_terrain_decal @0x6071C0; MapOverlay_RenderAllLayers @0x5BE840]

#include <array>
#include <cstdint>
#include <vector>

#include <runtime/terrain_query/coords.h>

namespace opennova::hud {

// Content mask handed to the map draw by the gameplay HUD pass. It selects
// which legs run; it is NOT an enable gate (the enable is the hudpos rect
// plus the compiled-in master switch dword_2723CC4 = -1).
// [orig: HUD_RenderAllOverlays @0x5a86e8 pushes 0xD07FF]
// Witnessed bits [orig: HUD_DrawMapOverlay @0x5a5f40]:
//  bit0 backing disc, bit1 marker banks, bit2 objective tethers,
//  bit5 entity labels, bit6 compass ring (draws only with bit9 too —
//   the gate is bit9 && bit6),
//  bit7 tracked-target pointer (line off, tip cell when ahead; color
//   g_hudActiveColor; no tracked-target source in this runtime yet),
//  bit8 waypoint state line (line + tip cell; altitude tricolor),
//  bit9 enables the terrain water-pass variant, the compass (with bit6),
//   and the MAPCOORDS grid label (with !bit12) — the terrain tiles
//   themselves draw UNCONDITIONALLY (render_terrain_decal is unmasked),
//  bit10 radar tick + weapon-direction + timer box,
//  bit12 the 300-wu grid leg: rules + letters/numbers + the on-map player
//   readout run BEFORE the marker walk when set (and mode != 1); it does
//   NOT suppress markers — the grid branch falls through into the bank
//   walk. The corner spinmap mask leaves it clear,
//  bit16 fullscreen centering, bit17 location labels,
//  bit18 at-tip waypoint distance label, bit19 tracked-target distance,
//  bit20 altitude nub above the rect (WPIndctr frame) — NO witnessed
//   caller mask carries bit20 (0xD07FF / 0xAF937 / 0xAE937 all lack it),
//   so the leg ships dormant in retail JO exactly as it does here.
inline constexpr uint32_t kSpinmapRetailFlags = 0x000D07FFu;

inline constexpr int32_t kSpinmapZoomDefault = 65536; // [orig: Player_InitPlayer @0x4e1763 — 65536.0 * clamp(1.0 - flt_A7640C, 0.0625, 1.0), flt_A7640C static 0]
// The big-map zoom spawn default: 8x the spinmap's world extent (the
// fullscreen M map opens ~2.6 km across on the design surface).
// [orig: Player_InitPlayer @0x4e1741..0x4e1754 — flt_7CD424 = 524288.0 *
//  the same clamp -> dword_B76490]
inline constexpr int32_t kBigMapZoomDefault = 0x80000;
inline constexpr int32_t kSpinmapZoomMin = 4096;      // [orig: zoom-in clamp @0x49bcb0 block]
inline constexpr int32_t kSpinmapZoomMax = 0x100000;  // [orig: zoom-out clamp @0x49beaf block]

// Icon byte -> draw layer. [orig: MapOverlay_RenderAllByLayer @0x5be681
// switch, duplicated @0x5be730]
inline uint8_t hud_minimap_icon_layer(uint8_t icon) {
	switch (icon) {
	case 10: case 11: case 15: case 18: case 25:
		return 1;
	case 2: case 4: case 12: case 13: case 16: case 17: case 29:
		return 2;
	case 3: case 8: case 14: case 23: case 24:
		return 3;
	default:
		return 0;
	}
}

struct HudMinimapTerrain {
	std::array<int, terrain::COORDS_SECTOR_GRID_DIM *
			terrain::COORDS_SECTOR_GRID_DIM> sector_grid{};
	int origin_x = 0;
	int origin_y = 0;
	int sector_count = 0;
	int sector_rows = 0;
	bool present = false;
	// The base pass samples the original 1024x1024 colormap atlas through its
	// four 512x512 quadrant ids. Retail binds Colormap0..3 directly; the device
	// keeps one atlas and clamps each quadrant with a half-texel inset.
	// [orig: PolyTrn_InitTextures @0x60B91D; render_terrain_decal @0x607761]
	int atlas_px = 1024;
	int cell_px = 512;
	// Retail redraws the same terrain geometry through the separate 256x256
	// `depthspin` shore texture. Its four 128x128 quadrants are selected from
	// the sector id; dry texels are transparent in the device-side equivalent.
	// [orig: depthspin build @0x60BA20; water UV transform @0x6077A2;
	//  alpha-tested redraw @0x60780A]
	bool water_present = false;
};

// Which retention bank a marker came from. Draw order within a layer is
// persistent, transient, special. [orig: MapOverlay_RenderAllByLayer @0x5be590
// walks slot_data(0x28E7F20), word_28E5620, unk_28EA820 in that order]
enum class HudMinimapBank : uint8_t {
	kPersistent = 0,
	kTransient = 1,
	kSpecial = 2,
};

struct HudMinimapMarker {
	uint8_t bank = 0; // HudMinimapBank
	uint16_t handle = 0xFFFF;
	int32_t x = 0;
	int32_t y = 0;
	int32_t z = 0; // 0x6B slots carry the ring height here (slot+28)
	int32_t heading_bam = 0;
	uint8_t icon = 0;
	uint32_t color = 0xFFFFFFFFu;
	uint8_t flags = 0;
	uint8_t source = 0;
	uint16_t remaining_ticks = 0;
	// Regular (non-special) markers render from the live entity and only when
	// it is decoded and alive. [orig: render_minimap_slot_blip @0x5be4b8
	// entity[538] gate]
	uint8_t entity_known = 0;
	// The client-side draw policy resolved from the local entity's def class
	// (witness at world::minimap_blip_draw_policy): upright classes clear
	// `rotate`, footprint classes skip the icon quad for their polygon feed,
	// and the halves/floor replace the person/generic fallbacks (0 halves =
	// unresolved -> fallback).
	uint8_t rotate = 1;
	uint8_t footprint = 0;
	int32_t half_x_q16 = 0;
	int32_t half_y_q16 = 0;
	uint8_t floor_px = 6;
	// A LOCAL-TEAM player whose class carries the charattr Medic attribute
	// draws the red-cross plate IN PLACE of its blip. The producer resolves it
	// (enemies never carry it — retail forces the bit off for the other team)
	// [orig: draw_entity_labels_and_markers @0x5a49e0 — the team gate
	//  @0x5a4ac6/@0x5a4acf, AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3,
	//  the cross @0x5a4cd6..0x5a4d48 replacing the blip].
	uint8_t medic = 0;
};

// One footprint-class entity's baked WORLD-SPACE polygon set (static
// entities; baked once per mission by the embedder's feed).
// [orig: render_collision_wireframe @0x596800 — fills in the team color and
//  builds boundary vertices; the completed pass leaves no observable stroke]
struct HudMinimapFootprint {
	uint16_t handle = 0xFFFF;
	uint32_t fill_argb = 0xFFA0A0A0u;
	std::vector<int32_t> fill_xy_q16; // x0,y0,x1,y1,x2,y2 per tri (mission)
	std::vector<int32_t> edge_xy_q16; // x0,y0,x1,y1 per boundary edge
	// World-space bounding circle over both vertex sets, stamped by
	// hud_minimap_finalize_footprint once at feed time so the per-frame
	// compile can reject off-view footprints before touching a triangle.
	int32_t bound_x_q16 = 0;
	int32_t bound_y_q16 = 0;
	int32_t bound_radius_q16 = 0;
};

// Stamp the bounding circle on a freshly parsed footprint (feed time, once).
void hud_minimap_finalize_footprint(HudMinimapFootprint &footprint);

struct HudMinimapInput {
	float rect_x1 = 0.0f; // authored HUDSPINMAP design coords (raw)
	float rect_y1 = 0.0f;
	float rect_x2 = 0.0f;
	float rect_y2 = 0.0f;
	float surface_w = 1024.0f;
	float surface_h = 768.0f;
	int32_t player_x = 0;
	int32_t player_y = 0;
	int32_t player_z = 0; // altitude, Q16 [orig: HUD_UpdateWaypointAltitudeColor @0x590970 reads Position.Z]
	int32_t player_heading_bam = 0;
	// The map-mode cycle the `map_toggle` action drives: 0 off (this input
	// describes the corner spinmap), 2 the 400x400 north-up window at
	// (20,20), 3 the fullscreen north-up map. Modes 2/3 rotate from the
	// fixed base 0x40000000 (north-up after the -90 fold), use the big-map
	// zoom, and select the big-map content mask.
	// [orig: HUD_CycleMapMode @0x520bc0 (0->2->3->0);
	//  HUD_BuildMapOverlayView @0x5a7e10 — rects 20,20..419,419 /
	//  0,0..1023,767, entity_ref 0x40000000, masks 0xAF937 (+bit16 mode 2),
	//  zoom dword_B76490]
	int map_mode = 0;
	// The big-map zoom value the radar keys adjust while a map mode is up.
	// [orig: dword_B76490; the mode-gated zoom-case reads @0x49bc98/@0x49be97]
	int32_t big_zoom_q16 = kBigMapZoomDefault;
	int32_t zoom_q16 = kSpinmapZoomDefault;
	uint32_t flags = kSpinmapRetailFlags;
	// Mission attrib bit5 rotates the map 180 degrees.
	// [orig: HUD_InitOverlaySystem @0x5a49bc — Bms_AttribFlags & 0x20 ->
	//  g_mapYaw180(0x2723EB0) = 0x80000000]
	bool flip_180 = false;
	int ticks = 0;
	bool waypoint_present = false;
	int32_t waypoint_x = 0;
	int32_t waypoint_y = 0;
	int32_t waypoint_z = 0; // Q16 [orig: dword_2723520, HUD_UpdateWaypointAltitudeColor @0x590970 altitude test]
	int waypoint_distance_m = 0;
	// The ring-edge distance label draws while this is ZERO — the retail
	// global is BSS (uninitialized .data -> 0 = LIVE by default) and an
	// authored NONZERO SPINMAPWPDISTOFF suppresses it (re-adjudicated
	// 2026-08-14 against the segment map; the "static -1" gloss read
	// undefined bytes). [orig: dword_27237C0 (.data, no file bytes);
	//  write @0x59fc1f; read @0x5a7a6a]
	int32_t waypoint_distance_offset = 0;
	// The frame overlay color (the hud_color_index scheme) colors the
	// distance and grid labels; the waypoint state line uses the altitude
	// tricolor instead. [orig: bit18 label + grid label color
	//  g_hudFrameOverlayColor @0x5a7ab5/@0x5a7a3a; bit8 line color
	//  g_waypointAltitudeColor @0x5a7878]
	uint32_t overlay_color = 0xFFFFFFFFu;
	// Player grid label: authored MAPCOORDS position (design px) and its
	// suppressor (BSS-zero -> LIVE by default; authored nonzero suppresses).
	// [orig: screenX/screenY @0x27236F4/F8, dword_27236FC (.data,
	//  uninitialized); mapcoords parse @0x5a0920]
	float map_coords_x = 0.0f;
	float map_coords_y = 0.0f;
	int32_t map_coords_off = 0;
	// Grid origin: the first pool-3 entity of type 2043, snapped to the
	// 300 wu grid by the consumer. [orig: HUD_InitOverlaySystem @0x5a4999
	//  scan (entity+80 == 2043) -> dword_2723EB4; read sub_59CB40]
	bool grid_origin_present = false;
	int32_t grid_origin_x = 0;
	int32_t grid_origin_y = 0;
	// The loaded TSDicon strip's PHYSICAL dimensions. Retail derives the
	// half-texel cell inset from the loaded tile's stored source size, so
	// the device stamps whatever asset it actually mounted — stock JO ships
	// 16x480, JOTAC's RevX02 authors 64x1920, both 30 square cells.
	// [orig: render_tiled_image_strip @0x67b540 — uv_half_texel =
	//  0.5 / (double)tile_dim]
	float icon_strip_w_px = 64.0f;
	float icon_strip_h_px = 1920.0f;
	HudMinimapTerrain terrain;
	std::vector<HudMinimapMarker> markers;
	// Static footprint polygons for footprint-class markers, joined by
	// handle at draw time. Borrowed from the frame state (the set is baked
	// once per mission and is far too heavy to copy per compile).
	const std::vector<HudMinimapFootprint> *footprints = nullptr;
};

struct HudMapVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct HudMapTri {
	HudMapVertex a;
	HudMapVertex b;
	HudMapVertex c;
	uint32_t color = 0xFFFFFFFFu;
};

struct HudMapSprite {
	float center_x = 0.0f;
	float center_y = 0.0f;
	float half_w = 0.0f;
	float half_h = 0.0f;
	float rotation_rad = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
	uint32_t color = 0xFFFFFFFFu;
	// Offset from the icon-strip slot: 0 TSDicon, 1 compass ring,
	// 2 radar slice, 3 WPIndctr strip.
	// [orig: HUD_LoadAllTextures — TSDicon.tga @0x27231AC,
	//  compring via draw_compass_indicator, WPIndctr.tga @0x27231A8]
	uint8_t texture = 0;
	uint8_t layer = 0;
};

struct HudMapLine {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	uint32_t color = 0xFFFFFFFFu;
};

struct HudMapLabel {
	float x = 0.0f;
	float y = 0.0f;
	// Preformatted text; the device leg draws it with the HUD label font.
	// [orig: "%03dm" / "%01.2fk" @0x5a7ab5 region, flt_7C69E8 = 0.001;
	//  grid "(%s,%d)" sub_59CB40]
	char text[16] = {};
	uint32_t color = 0xFFFFFFFFu;
	// 0 centered [orig: HUD_DrawTextCentered_HalfBright @0x5a7ab5],
	// 1 right-aligned [orig: HUD_DrawTextRightAligned_HalfBright @0x59cc47]
	uint8_t align = 0;
	// 0 the bold label slot (corner-map distance/MAPCOORDS labels),
	// 1 the large slot — the grid letters/numbers and the big map's player
	// readout draw with g_hudLabelFontLarge, still through the half-bright
	// drawer. [orig: HUD_DrawTextCentered_HalfBright(g_hudLabelFontLarge,...)
	//  in the @0x5a5f40 grid branch]
	uint8_t font = 0;
};

struct HudMapPass {
	bool visible = false;
	float center_x = 0.0f;
	float center_y = 0.0f;
	float radius_x = 0.0f;
	float radius_y = 0.0f;
	std::vector<HudMapTri> backing;
	std::vector<HudMapTri> terrain;
	// Same clipped sector geometry as `terrain`, with depthspin's independent
	// 2x2-quadrant UV transform. The device submits this opaque cutout after
	// both legs of Canvas's split terrain-brightness equivalent.
	std::vector<HudMapTri> terrain_water;
	// Untextured filled polygons layered with the markers (the building/zone
	// footprints — they draw first in the marker walk, so they sit under the
	// icon sprites like retail's buildings-first walk).
	std::vector<HudMapTri> overlays;
	std::vector<HudMapSprite> sprites;
	// Two line layers, matching the retail pass order: grid rules draw BEFORE
	// the marker walk (under the icon sprites); the waypoint tether and the
	// pulse-ring segments draw after. Footprint boundary vertices are retained
	// in the parsed feed but are visually inert in retail's completed pass.
	// [orig: the @0x5a5f40 grid branch precedes the bank walk; the bit8
	//  pointer leg follows it]
	std::vector<HudMapLine> lines_under;
	std::vector<HudMapLine> lines;
	std::vector<HudMapLabel> labels;
};

// One radar-zoom input step over the active zoom value. direction > 0 zooms
// OUT (x1.15 toward 0x100000), direction < 0 zooms IN (x0.85 toward 4096).
// The value is a world-extent multiplier: bigger shows more world.
// [orig: Input_HandleActionBinding @0x49AD40 — radarout row 48 = case 361
//  (dbl_7C7AA8 = 1.15 @0x49beaf), radarin row 49 = case 360
//  (dbl_7C7AB0 = 0.85 @0x49bcb0)]
// direction == 0 restores the spawn value (Player_InitPlayer semantics).
int32_t spinmap_zoom_step(int32_t zoom_q16, int direction);

// Mission-space point to the heading-up map. Returns false when outside the
// circular clip; clamp_to_edge pins the point onto its circumference.
bool project_spinmap_point(const HudMinimapInput &input, int32_t world_x,
		int32_t world_y, bool clamp_to_edge, float &out_x, float &out_y);

class HudMinimapCompiler {
public:
	// Compiles into `out`, clearing it first but keeping its vector capacity —
	// the compiler is meant to live across frames (scratch buffers and the
	// footprint index persist so the per-frame walk stays allocation-free).
	void compile(const HudMinimapInput &input, HudMapPass &out);
	// Single-shot convenience for tests.
	const HudMapPass &compile(const HudMinimapInput &input) {
		compile(input, pass_);
		return pass_;
	}

private:
	HudMapPass pass_;
	// Reused per-compile scratch (clip ping-pong buffers, footprint lookup).
	std::vector<HudMapVertex> clip_a_;
	std::vector<HudMapVertex> clip_b_;
	std::vector<const HudMinimapFootprint *> footprint_index_;
};

// The client-side map-view control state: the M-cycle mode plus the two
// radar-zoom values, with the retail globals' lifecycle. The mode clears on
// round init, respawn-state init, and while the local player is dead; the
// zooms reset to the spawn defaults with the player init.
// [orig: g_mapOverlayMode — cycle HUD_CycleMapMode @0x520bc0 (0->2->3->0),
//  clears @0x42275a (Game_InitNewRound), @0x499395 (Game_InitRespawnState),
//  @0x5cac67..0x5cac6d (dead-player gate in Render_ProcessMainSceneFrame);
//  zoom defaults Player_InitPlayer @0x4e1741..0x4e1763, big zoom
//  dword_B76490]
struct HudMapControl {
	int mode = 0; // 0 off / 2 north-up window / 3 fullscreen
	int32_t zoom_q16 = kSpinmapZoomDefault;
	int32_t big_zoom_q16 = kBigMapZoomDefault;
	// The MISSION-scaled spawn defaults: Player_InitPlayer derives both zoom
	// resets from the BMS header's map_zoom float: X = clamp(1-map_zoom,
	// 0.0625, 1.0), spin = 65536*X, big = 524288*X. The completed-pass retail
	// probe resolves the ambiguous FPU ordering directly: 00TRa authors 0.61
	// and retail submits zoom 25559 (0.39 Q16). Zero/unauthored map_zoom keeps
	// X = 1, matching the plain defaults a joiner sees.
	// [orig: Player_InitPlayer @0x4e1693..0x4e1763 — Bms_MapZoom @0xA7640C
	//  (g_BmsHeaderBlock+0x23C, bulk fread), floor flt_7C486C = 0.0625,
	//  x flt_7CD424 = 524288 -> g_bigMapZoom, x flt_7C32BC = 65536 ->
	//  g_spinmapZoom]
	int32_t spawn_zoom_q16 = kSpinmapZoomDefault;
	int32_t spawn_big_zoom_q16 = kBigMapZoomDefault;

	// Stamp the mission's authored map_zoom (BMS header float) at promotion;
	// the current values snap to the new spawn defaults like the player init.
	void set_mission_map_zoom(float map_zoom) {
		float x = map_zoom <= 0.0f ? 1.0f : 1.0f - map_zoom;
		if (x > 1.0f) x = 1.0f;
		if (x < 0.0625f) x = 0.0625f;
		spawn_zoom_q16 = static_cast<int32_t>(65536.0f * x);
		spawn_big_zoom_q16 = static_cast<int32_t>(524288.0f * x);
		zoom_q16 = spawn_zoom_q16;
		big_zoom_q16 = spawn_big_zoom_q16;
	}

	// [orig: HUD_CycleMapMode @0x520bc0]
	int cycle() {
		mode = mode == 0 ? 2 : (mode == 2 ? 3 : 0);
		return mode;
	}
	// The radar keys step the big-map zoom while a mode is up, the corner
	// spinmap's otherwise. [orig: the mode-gated zoom-case reads
	//  @0x49bc98/@0x49be97]
	int32_t zoom_step(int direction) {
		if (mode != 0) {
			big_zoom_q16 = direction == 0
					? spawn_big_zoom_q16
					: spinmap_zoom_step(big_zoom_q16, direction);
			return big_zoom_q16;
		}
		zoom_q16 = direction == 0
				? spawn_zoom_q16
				: spinmap_zoom_step(zoom_q16, direction);
		return zoom_q16;
	}
	// Spawn/respawn: the mission-scaled zoom defaults return and the mode
	// clears. [orig: Player_InitPlayer @0x4e1741..; Game_InitRespawnState
	//  @0x499395]
	void reset_spawn() {
		mode = 0;
		zoom_q16 = spawn_zoom_q16;
		big_zoom_q16 = spawn_big_zoom_q16;
	}
	// The render gate zeroes the mode whenever the local player is dead.
	// [orig: @0x5cac67..0x5cac6d — g_mapOverlayMode = 0 on Flags & 2]
	void on_local_player_dead() { mode = 0; }
};


// The map grid-label origin (the mission's first type-2043 marker) in mission
// 16.16, `present` false until the marker resolves (witness at
// HudMinimapInput::grid_origin_x). One value the embedder fills; its Godot
// record wraps it by value.
struct HudMapGridOrigin {
	bool present = false;
	int32_t x_q16 = 0;
	int32_t y_q16 = 0;
};

} // namespace opennova::hud
