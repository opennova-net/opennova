#pragma once

// Portable retail HUD spinmap compiler. It turns mission-space state and the
// TRN sector routing table into the heading-up circular map: backing disc,
// terrain mesh, ordered marker primitives, waypoint tether + distance label,
// and the counter-rotating compass ring. The embedder only binds the
// colormap/icon textures and submits these primitives.
// [orig: HUD_RenderAllOverlays @0x5A8070 -> HUD_DrawMapOverlay @0x5A5F40;
//  Render_TerrainDecal @0x6071C0; MapOverlay_RenderAllLayers @0x5BE840]

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include <runtime/terrain_query/coords.h>

namespace opennova::hud {

// Content mask handed to the map draw by the gameplay HUD pass. It selects
// which legs run; it is NOT an enable gate (the enable is the hudpos rect
// plus the compiled-in master switch dword_2723CC4 = -1).
// [orig: HUD_RenderAllOverlays @0x5a86e8 pushes 0xD07FF]
// Witnessed bits [orig: HUD_DrawMapOverlay @0x5a5f40], in draw order:
//  bit0 the depth MASK: an invisible rect quad at z 0.1 then the 33-vertex
//   disc fan at z 0.999996, both diffuse 0 through stock shader #2 — no
//   colour reaches the screen; later z-tested draws crop to the disc
//   [orig: @0x5a6527..0x5a6667],
//  bit9 selects the terrain pass: set = zwrite off / ztest on (cropped to
//   the disc), clear = ztest always + zwrite on (the tiles overwrite the
//   mask, so only the rect viewport crops) [orig: Render_TerrainDecal
//   use_alt_blend @0x6071F7]; bit9 also gates the compass (with bit6), the
//   MAPCOORDS label (with !bit12) and the 2-design-px disc inset,
//  bit12 the 300-wu grid leg (rules + letters/numbers + the on-map player
//   readout) BEFORE the marker walk (and mode != 1); it falls through,
//  bit1 marker banks, bit2 objective tethers (any drawn tether suppresses
//   the bit8 pointer), bit15 route lines (g_GameType 0x10010),
//   bit14 zone waypoint labels, bit3 zone letters, bit4|bit11 the pool-3
//   walk (bit4 the 6006 KOTH ring, bit11 the 2044 location labels, the
//   6027/6028 capture rings always), bit17 player waypoints, bit5 entity
//   labels and slot markers (bit13 adds the names), bit19 the tracked
//   callout, bit7 the nearest-FARP chevron, bit8 the waypoint pointer,
//  then after the flush: bit10 (with bits 9 and 6: the radar contact
//   update, the dmgslice sector marks and the threat ring,
//   hud_minimap_radar.cpp) + bit6 compass, bit20 altitude nub,
//   the MAPCOORDS label, bit18 the pointer-race distance label, bit19's
//   label. bit16 squares the rect on its half-height about its centre.
//  bit20 — NO witnessed caller mask carries it (0xD07FF / 0xAF937 /
//   0xAE937 all lack it), so the nub ships dormant in retail JO.
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
	// [orig: PolyTrn_InitTextures @0x60B91D; Render_TerrainDecal @0x607761]
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
	// Regular (non-special) markers render from the live pool entity; the
	// blip drawer returns for a def-less one (the port: not decoded / not
	// alive). The entity[538] test at @0x5be4b8 gates only the zone ring and
	// palette swap. [orig: Minimap_DrawBlip @0x5978a8; Render_MinimapSlotBlip
	// @0x5be4b8]
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
	// [orig: HUD_DrawEntityLabelsAndMarkers @0x5a49e0 — the team gate
	//  @0x5a4ac6/@0x5a4acf, AnimMap_IsSlotActive(playerClass, 8) @0x5a4ab3,
	//  the cross @0x5a4cd6..0x5a4d48 replacing the blip].
	uint8_t medic = 0;
	// --- v5: the slot's LIVE pool-entity facts the drawer and the later map
	// legs read at draw time (the 1160-slot walks and Minimap_DrawBlip read
	// the pool entity behind the slot handle, never the slot copy).
	// [orig: HUD_DrawMapOverlay @0x5a6d33..0x5a74fa; Minimap_DrawBlip
	//  @0x597890; Render_MinimapSlotBlip @0x5be490..0x5be57b]
	uint8_t team = 0;         // entity+354
	uint8_t zone_number = 0;  // entity+538 (live)
	uint8_t def_type = 0;     // ItemDef+0x5C (0 with no def)
	uint8_t entity_bits = 0;  // kMarkerEntity* below
	int16_t zone_index = -1;  // SpawnZoneList_IndexOf(entity), -1 = not listed
	uint16_t zone_radius = 0; // entity+350, whole world units
	int32_t entity_x = 0;     // entity+4/+8, the live position
	int32_t entity_y = 0;
	// The blip/label anchor: the placement matrix of entity+4 applied to the
	// collision-bbox centre entity+0x1FC [orig: Math_BuildFixedPointMatrix-
	//  FromEulerAngles + Math_FixedPointTransformPoint22 @0x5978B2..0x5978DC
	//  (blip) / @0x5a7284..0x5a7299 (zone labels)].
	int32_t anchor_x = 0;
	int32_t anchor_y = 0;
	int32_t bound_radius_q16 = 0; // entity+0, the early-cull margin source
	// --- v6: the facts the vehicle-bay logo walk reads behind the handle
	// [orig: HUD_DrawVehicleBayLogos (ex Radar_DrawBlips) @0x5a2c00 --
	//  entity+0xC @0x5a2d56, the def's +0xAD8 @0x5a2d6f].
	int32_t entity_z = 0;   // entity+0xC, the live altitude
	uint8_t bay_groups = 0; // ItemDef+0xAD8: 1 land, 2 air, 4 water families
};

// HudMinimapMarker::entity_bits.
inline constexpr uint8_t kMarkerEntityHasModel = 0x01;  // entity+0x30 != 0
inline constexpr uint8_t kMarkerEntityOcclusion = 0x02; // model+0xE0 != 0
inline constexpr uint8_t kMarkerEntityZoneDef = 0x04;   // ItemDef attrib & 0x40000
inline constexpr uint8_t kMarkerEntityDead = 0x08;      // entity+0x24 & 2
inline constexpr uint8_t kMarkerEntityHud = 0x10;       // the HUD entity itself
inline constexpr uint8_t kMarkerEntityFarp = 0x20;      // ItemDef attrib2 & 0x2000
inline constexpr uint8_t kMarkerEntityVehicleBay = 0x40; // ItemDef attrib2 (+0x58) & 1

// The retail HUD colour table entries the map legs read [orig:
// g_MinimapOverlayColorTable @0x840A10 — [9] 0xFF802020, [10]
// g_MapOverlayTeamColor 0xFF304080, [11] dword_840A3C 0xFF208020; the
// g_HUDColors palette immediates HUD_InitTeamColorTable @0x51f245..0x51f2b3 —
// [1] 0xFF00FF00, [3] 0xFF80A0FF, [5] 0xFFFF5050].
inline constexpr uint32_t kMapOverlayTeam1 = 0xFF304080u;
inline constexpr uint32_t kMapOverlayTeam2 = 0xFF802020u;
inline constexpr uint32_t kMapOverlayNeutral = 0xFF208020u;
inline constexpr uint32_t kHudPaletteGreen = 0xFF00FF00u;
inline constexpr uint32_t kHudPaletteLightBlue = 0xFF80A0FFu;
inline constexpr uint32_t kHudPaletteSalmon = 0xFFFF5050u;

// One pool-3 entity the mask&0x810 walk reads [orig: HUD_DrawMapOverlay
// @0x5a7504..0x5a76f5 — def id +0x50: 6006 KOTH zone, 2044 location marker,
// 6027/6028 capture zones].
struct HudMinimapPoolEntity {
	int32_t def_id = 0;
	int32_t x = 0;
	int32_t y = 0;
	int32_t radius_q16 = 0; // entity+0
	int16_t location_index = 0; // (int16)entity+0x280 [orig: @0x5a75d1]
};

// One pool-4 player waypoint (def 6089) the bit17 leg labels
// [orig: @0x5a76fe..0x5a77ac; Waypoint_CreateForPlayer @0x4DFCE0].
struct HudMinimapPlayerWaypoint {
	int32_t x = 0;
	int32_t y = 0;
	std::string name; // entity+0xF4
};

// One g_PlayerSlotPtrTable row the bit5 loop 1 walks [orig:
// HUD_DrawEntityLabelsAndMarkers @0x5a4a54..0x5a4eb4].
struct HudMinimapPlayerSlot {
	bool active = false;        // slot+0x0D
	bool own_slot = false;      // the table entry's [0] is the HUD entity
	bool revivable = false;     // slot+0x10
	bool medic_request = false; // slot+0x2C
	bool radio_request = false; // entity+0x375 == 1
	bool aboard_vehicle = false; // Entity_FindChildByDefType(entity, 1, 1)
	bool medic = false;         // AnimMap_IsSlotActive(entity+0x294, 8)
	uint8_t squad = 0;          // slot+0x33
	bool name_slot = false;     // PlayerSlot_FindByEntityPtr found a slot
	std::string name;           // slot+0x14 (or entity+0xF4 without a slot)
	std::string clan;           // slot+0x20
	HudMinimapMarker blip;      // the slot entity's pose, class and policy
};

// The HUD_SetTrackedEntityTarget state the bit19 leg draws (the retail
// globals dword_2721EBC..2721ECC) plus the live facts of its entity.
// [orig: HUD_SetTrackedEntityTarget @0x59D050; Render_LaserSightEffect
//  @0x59D110]
struct HudMinimapTracked {
	uint32_t serial = 0;       // bumps on every set (ED0 restamp)
	int32_t ticks = 0;         // dword_2721EBC, per-tick decremented
	int32_t snap_x = 0;        // dword_2721EC0/EC4, the position at set
	int32_t snap_y = 0;
	bool friendly = false;     // byte_2721ECC
	uint32_t set_color = 0xFFFFFFFFu; // ED0 at set: palette[3] / white
	bool live_known = false;
	int32_t live_x = 0;        // entity+4/+8
	int32_t live_y = 0;
	bool radio_request = false; // entity+0x375 == 1
	bool hidden_bit = false;    // entity+0x24 & 1
	bool aboard_vehicle = false; // Entity_FindChildByDefType(entity, 1, 1)
	bool viewer_mount_ok = false; // HUD entity +0x168 is 2 or 5
	HudMinimapMarker blip;      // the entity's class/policy for the enemy blip
};

// Entity_GetDisplayName's non-null result for a transient-bank person (bit5
// loop 2); a handle absent from the list is the null return.
struct HudMinimapName {
	uint16_t handle = 0xFFFF;
	std::string text;
};

// Everything the later map legs read beyond the marker banks. One value the
// embedder fills per frame; the compile input borrows it by pointer.
struct HudMinimapOverlays {
	int32_t game_type = 0;          // g_GameType
	int32_t zone_score_delta = 0;   // dword_A85AFC - dword_A85B0C (KOTH)
	int32_t zone_timer = 0;         // dword_A85B68 (the spawn-target hold)
	uint32_t owned_zone_mask = 0;   // dword_A85BBC
	uint8_t hud_team = 0;           // the HUD entity's team byte
	uint16_t hud_handle = 0xFFFF;
	bool hud_present = false;       // g_HUDInfoCurrentEntity || g_LocalPlayerEntity
	bool enemy_tags_visible = false; // g_EnemyTagsVisible
	// The player profile's +0x67C dword the own-slot revive leg reads
	// [orig: *((_DWORD *)g_CurPlayerProfile + 415) @0x5a4b40].
	bool own_revive_profile = false;
	std::vector<HudMinimapPoolEntity> pool3;
	std::vector<std::string> location_names; // g_LocationNames
	std::vector<HudMinimapPlayerWaypoint> player_waypoints;
	std::vector<HudMinimapPlayerSlot> player_slots;
	HudMinimapTracked tracked;
	std::vector<HudMinimapName> names;
	// WPNames STRWPNAME%03d (zone index + 1), indexed by zone index, and the
	// two Overlays formats the bit14 labels wrap them in.
	std::vector<std::string> zone_wp_names;
	std::string objective_point_format; // STROVER_OBJECTIVEPOINT_SHORT
	std::string defensive_position_format; // STROVER_DEFENSIVEPOSITION
};

// The HUD_BuildMapOverlayView parameter block's mask bytes (the M cycle
// passes none, which is every flag on) [orig: HUD_BuildMapOverlayView
// @0x5a7e8a..0x5a7eb2 — BYTE1(params+4) glow selects 0xAF937 / 0xAE937,
// params+7 == 0 clears bits 11/14/17, params+6 == 0 clears bit17].
struct HudBigMapParams {
	bool glow = true;
	bool labels = true;           // params+7
	bool player_waypoints = true; // params+6
};

// One footprint-class entity's baked WORLD-SPACE polygon set (static
// entities; baked once per mission by the embedder's feed).
// [orig: Render_CollisionWireframe @0x596800 — fills in the team color and
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

// The radar-contact state the bit-10 legs draw, snapshotted once per HUD
// frame from the local player's contact table (world/radar_contacts.h,
// radar_hud_frame): the four sector rings, the incoming-missile list with
// each row's live position, the local player's position the threat ring
// measures from, and the NoTracers rules bit.
// [orig: dword_2721F30 / dword_2721F24 / dword_2721F0C / dword_2721EF4;
//  unk_2722B40 [0, dword_2721F3C); g_LocalPlayerEntity->Position read by
//  HUD_DrawDirectionalIndicatorRing @0x5981e3; g_RulesFlags & 1]
struct HudMinimapRadar {
	std::array<uint8_t, 12> red12{};
	std::array<uint8_t, 12> olive12{};
	std::array<uint8_t, 24> red24{};
	std::array<uint8_t, 24> olive24{};
	bool rules_no_tracers = false;
	int32_t local_x = 0;
	int32_t local_y = 0;
	// One row per missile-list entry in [0, count) — row 0 included, the null
	// rows kept (`present` = the entry's pointer is set).
	struct Threat {
		uint8_t present = 0;
		int32_t x = 0;
		int32_t y = 0;
	};
	std::vector<Threat> threats;
};

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
	// Mode 4 is the WINDOWED view (hud_map_view.h): the caller's virtual
	// rect, north-up, the caller's content mask and scale — the CMAP map
	// (HUD_BuildMapOverlayView mode 4) and the DEATH MAP window
	// (MapOverlay_DrawView).
	int map_mode = 0;
	// Mode 4 only: world units per pixel (the caller computes it: CMAP from
	// HUD_DrawMapOverlay's zoom / (width x 200), DEATH from its handler's
	// scale argument), and the marker walk's angle offset from the view angle
	// (DEATH walks at 0x3FFFFFC0 under a 0x40000000 view).
	// [orig: HUD_DrawMapOverlay @0x5a6501..0x5a650b; MapOverlay_DrawView
	//  @0x5a59ae / @0x5a5a0b]
	float window_scale = 0.0f;
	int32_t window_marker_angle_bias_bam = 0;
	// The big-map zoom value the radar keys adjust while a map mode is up.
	// [orig: dword_B76490; the mode-gated zoom-case reads @0x49bc98/@0x49be97]
	int32_t big_zoom_q16 = kBigMapZoomDefault;
	int32_t zoom_q16 = kSpinmapZoomDefault;
	uint32_t flags = kSpinmapRetailFlags;
	// Mission attrib bit5 rotates the map 180 degrees.
	// [orig: HUD_InitOverlaySystem @0x5a49bc — g_BmsAttribFlags & 0x20 ->
	//  g_MapYaw180(0x2723EB0) = 0x80000000]
	bool flip_180 = false;
	int ticks = 0;
	// The sixteen HUD item flash timers (hud_declutter.h HudItemFlash): [5]
	// blinks the waypoint pointer, [14] the FARP chevron, [10]..[13]/[15]
	// hide blips by cell/colour on their lit phase [orig: HUD_DrawMapOverlay
	// @0x5a785b / @0x5a77fe; Minimap_DrawBlip @0x597DFD..0x597E92].
	std::array<int32_t, 16> item_flash{};
	bool waypoint_present = false;
	int32_t waypoint_x = 0;
	int32_t waypoint_y = 0;
	int32_t waypoint_z = 0; // Q16 [orig: dword_2723520, HUD_UpdateWaypointAltitudeColor @0x590970 altitude test]
	// The nearest FARP HUD_BuildEntityInfo resolved this frame (hudInfo+0x10
	// and g_TrackedTargetPos) [orig: @0x4b891a..0x4b89e3].
	bool farp_present = false;
	int32_t farp_x = 0;
	int32_t farp_y = 0;
	// g_HudposTextColor, the tether's neutral-team colour [orig: @0x5a6dc2].
	uint32_t hudpos_text_color = 0xFFFFFFFFu;
	HudBigMapParams big_map_params;
	// The non-bank legs' feed (borrowed; null = none of them draw).
	const HudMinimapOverlays *overlays = nullptr;
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
	//  g_HUDFrameOverlayColor @0x5a7ab5/@0x5a7a3a; bit8 line color
	//  g_WaypointAltitudeColor @0x5a7878]
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
	// [orig: Render_TiledImageStrip @0x67b540 — uv_half_texel =
	//  0.5 / (double)tile_dim]
	float icon_strip_w_px = 64.0f;
	float icon_strip_h_px = 1920.0f;
	HudMinimapTerrain terrain;
	std::vector<HudMinimapMarker> markers;
	// Static footprint polygons for footprint-class markers, joined by
	// handle at draw time. Borrowed from the frame state (the set is baked
	// once per mission and is far too heavy to copy per compile).
	const std::vector<HudMinimapFootprint> *footprints = nullptr;
	// The radar-contact snapshot the bit-10 legs draw (HudMinimapRadar above).
	HudMinimapRadar radar;
	// Both slice textures loaded — the device stamps it; the marks draw only
	// then [orig: dword_2723924 && dword_2723934 @0x59c359..0x59c36b].
	bool radar_slices_loaded = false;
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
	// 2 radar slice (dmgslice.tga, the 12-ring), 3 WPIndctr strip,
	// 4 narrow radar slice (dmgslc_n.tga, the 24-ring); kHudMapTextureNone =
	// untextured (the ring bands' per-vertex diffuse).
	// [orig: HUD_LoadAllTextures — TSDicon.tga @0x27231AC,
	//  compring via HUD_DrawCompassIndicator, WPIndctr.tga @0x27231A8,
	//  dmgslice.tga -> rec @0x2723920 / dmgslc_n.tga -> rec @0x2723930
	//  @0x59de25..0x59de42]
	uint8_t texture = 0;
	uint8_t layer = 0;
	// geom_count > 0: draw HudMapPass::geom[geom_first .. +geom_count) as a
	// triangle list (per-vertex colour and UV) INSTEAD of the quad — the
	// disc/rect crop of a straddling quad, or a ring band.
	uint32_t geom_first = 0;
	uint32_t geom_count = 0;
	// The device applies the pass's own MODULATE2X(TEXTURE, DIFFUSE) colour
	// stage (saturate(2 * texel * diffuse), alpha MODULATE) to this sprite,
	// so `color` carries the raw diffuse. Set by the radar marks, whose
	// material is mode word 0x651 (blend 1, alpha 0x50, colour 0x600); the
	// other sprites fold the stage into the diffuse at compile.
	// [orig: HUD_LoadAllTextures @0x59de25..0x59de42 (flags 1617);
	//  RenderState_DecodeModeColorStage @0x681080 (0x600)]
	bool modulate2x = false;
};

inline constexpr uint8_t kHudMapTextureNone = 0xFF;

struct HudMapGeomVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
	uint32_t color = 0xFFFFFFFFu;
};

inline constexpr uint8_t kHudMapSpriteRadar = 2;
inline constexpr uint8_t kHudMapSpriteRadarNarrow = 4;

// An untextured vertex-coloured triangle (the threat ring's strips).
struct HudMapColorVertex {
	float x = 0.0f;
	float y = 0.0f;
	uint32_t color = 0u;
};

struct HudMapColorTri {
	HudMapColorVertex a;
	HudMapColorVertex b;
	HudMapColorVertex c;
};

struct HudMapLine {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	uint32_t color = 0xFFFFFFFFu;
	// The second vertex's diffuse when it differs (the gouraud route lines);
	// 0 = the same as `color`.
	uint32_t color_end = 0;
};

struct HudMapLabel {
	float x = 0.0f;
	float y = 0.0f;
	// Preformatted text; the device leg draws it with the HUD label font.
	// Sized for the widest map label (the 128-byte bit14 Buffer and the
	// 64-byte display names) [orig: Buffer[32] @0x5a5f40 frame].
	char text[128] = {};
	uint32_t color = 0xFFFFFFFFu;
	// 0 centered [orig: HUD_DrawTextCentered_HalfBright @0x580680],
	// 1 right-aligned [orig: HUD_DrawTextRightAligned_HalfBright @0x59cc47],
	// 2 left [orig: HUD_DrawTextLeft_HalfBright @0x5804C0]
	uint8_t align = 0;
	// The g_HUDLabelFont slot: 0 the bold [1] slot (distance, MAPCOORDS,
	// zone letters/labels, location labels, player waypoints), 1 the large
	// [2] slot (the grid letters/numbers and the big map's player readout),
	// 2 the regular [0] slot (the bit5 names). [orig: the font args of the
	// @0x5a5f40 label draws; HUD_DrawEntityLabelsAndMarkers @0x5a4ea6]
	uint8_t font = 0;
	// Every map drawer forces the alpha opaque except HUD_DrawTextHalfBrightF,
	// which keeps it (the bit19 label) [orig: @0x580688 / @0x5804c6 vs
	//  @0x580726].
	uint8_t keep_alpha = 0;
	// Drawn INSIDE the map pass, i.e. under the rect viewport: the glyphs crop
	// to HudMapPass::clip_* per pixel [orig: SetViewport(rect) @0x5a64a8,
	//  restored @0x5a78e3 before the tail labels].
	uint8_t clip = 0;
};

struct HudMapPass {
	bool visible = false;
	float center_x = 0.0f;
	float center_y = 0.0f;
	float radius_x = 0.0f;
	float radius_y = 0.0f;
	// The rect viewport the pass draws under (in-pass labels crop to it).
	// The bit0 depth mask is not geometry: its crop is baked into every
	// z-tested leg by the compiler (the 32-gon clips below).
	float clip_x1 = 0.0f;
	float clip_y1 = 0.0f;
	float clip_x2 = 0.0f;
	float clip_y2 = 0.0f;
	// A render pass's own opaque rect clear, drawn first (the CMAP window's
	// CGfxDevice_Clear, hud_map_view.h CommandMapCompiler). The spinmap never
	// fills one: its bit0 disc is the invisible mask above.
	std::vector<HudMapTri> clear;
	std::vector<HudMapTri> terrain;
	// Same clipped sector geometry as `terrain`, with depthspin's independent
	// 2x2-quadrant UV transform. The device submits this opaque cutout after
	// both legs of Canvas's split terrain-brightness equivalent.
	std::vector<HudMapTri> terrain_water;
	// Untextured filled polygons layered with the markers (the building/zone
	// footprints — they draw first in the marker walk, so they sit under the
	// icon sprites like retail's buildings-first walk).
	std::vector<HudMapTri> overlays;
	// The ordered marker/pointer/ring stream (insertion order is draw order).
	std::vector<HudMapSprite> sprites;
	std::vector<HudMapGeomVertex> geom;
	// Two line layers, matching the retail pass order: grid rules draw BEFORE
	// the marker walk (under the icon sprites); the pointer tethers and the
	// route lines draw after. Footprint boundary vertices are retained in the
	// parsed feed but are visually inert in retail's completed pass.
	// [orig: the @0x5a5f40 grid branch precedes the bank walk; the bit8
	//  pointer leg follows it]
	std::vector<HudMapLine> lines_under;
	std::vector<HudMapLine> lines;
	std::vector<HudMapLabel> labels;
	// The threat ring's untextured vertex-coloured strips (bit 10), drawn
	// unclipped right BEFORE sprite `ring_tris_before_sprite` — after the
	// dmgslice marks, ahead of the compass ring.
	// [orig: HUD_DrawMapOverlay @0x5a7921..0x5a7931 — the marks, the ring,
	//  then HUD_DrawCompassIndicator]
	std::vector<HudMapColorTri> ring_tris;
	size_t ring_tris_before_sprite = 0;
};

// One radar-zoom input step over the active zoom value. direction > 0 zooms
// OUT (x1.15 toward 0x100000), direction < 0 zooms IN (x0.85 toward 4096).
// The value is a world-extent multiplier: bigger shows more world.
// [orig: Input_HandleActionBinding @0x49AD40 — radarout row 48 = case 361
//  (dbl_7C7AA8 = 1.15 @0x49beaf), radarin row 49 = case 360
//  (dbl_7C7AB0 = 0.85 @0x49bcb0)]
// direction == 0 restores the spawn value (Player_InitPlayer semantics).
int32_t spinmap_zoom_step(int32_t zoom_q16, int direction);

// One TSDicon strip cell's UV rect (30 square cells, half-texel insets from
// the loaded strip's physical size) and the MODULATE2X diffuse fold the strip
// renderer's texture stage implies -- shared by the map blips and the
// friendly-tag radio-request icon, which both submit through
// Render_DrawIconStripCell_Debug [orig: Render_TiledImageStrip @0x67b540;
// Render_DrawIconStripCell_Debug @0x67bae0].
void hud_icon_strip_cell_uv(const HudMinimapInput &input, uint8_t icon,
		float &u0, float &v0, float &u1, float &v1);
uint32_t hud_icon_strip_modulate2x_color(uint32_t argb);

// The bearing every radar leg quantises: atan2(dx, dy) times `scale`, chopped
// to 64 bits under the x87 control word with the low word kept. The add uses
// +2^31/pi (dbl_7C19D8), the contact update and the threat ring its negation
// (dbl_7C57B8).
// [orig: Radar_AddBlip @0x59b2b4..0x59b2d1; Radar_UpdateContacts
//  @0x59a8ab..0x59a8ca; HUD_DrawDirectionalIndicatorRing @0x5982b2..0x5982be]
inline constexpr double kRadarBearingScale = 683565275.5764316;
uint32_t radar_bearing_raw(int32_t dx, int32_t dy, double scale);
// The viewer-relative sector of an update/ring bearing: A = 0xF5555800 - raw -
// yaw, sector = ((A >> 16) * n) >> 16 for the 12- and 24-sector rings.
// [orig: Radar_UpdateContacts @0x59a8bb..0x59a8e8 / @0x59a922..0x59a930;
//  HUD_DrawDirectionalIndicatorRing @0x5982ad..0x5982d4]
int radar_sector(uint32_t raw, uint32_t yaw, int sectors);

// The spinmap's content-mask bit-10 legs, run by the compiler under
// (flags & 0x200) && (flags & 0x40) && (flags & 0x400) just before the compass
// ring (hud_minimap_radar.cpp): the dmgslice sector marks (12-ring then
// 24-ring sprites, appended to out.sprites), then the threat ring's strips
// (out.ring_tris, split before the next sprite — the compass).
// [orig: HUD_DrawMapOverlay @0x5a78ff..0x5a7931 ->
//  HUD_DrawWeaponDirectionIndicators @0x59c350, HUD_DrawTimerOverlayBox
//  @0x59c7b0 -> HUD_DrawDirectionalIndicatorRing @0x598180]
void hud_minimap_emit_radar(const HudMinimapInput &input, uint32_t flags, HudMapPass &out);

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
	std::vector<HudMapGeomVertex> geom_a_;
	std::vector<HudMapGeomVertex> geom_b_;
	std::vector<const HudMinimapFootprint *> footprint_index_;
	// The tracked-callout globals persist ACROSS passes and frames: the laser
	// draw writes them and the bit5 loop (which runs before it) reads the
	// previous write [orig: dword_2721ED0 / byte_2721ED4 — written by
	//  HUD_SetTrackedEntityTarget @0x59D0EF..0x59D0FF and
	//  Render_LaserSightEffect @0x59D1A3/@0x59D1BD/@0x59D356/@0x59D532, read
	//  by HUD_DrawEntityLabelsAndMarkers @0x5a4c8b..0x5a4dad].
	uint32_t tracked_serial_ = 0;
	uint32_t tracked_color_ = 0;
	uint8_t tracked_alpha_ = 0;
};

// The client-side map-view control state: the M-cycle mode plus the two
// radar-zoom values, with the retail globals' lifecycle. The mode clears on
// round init, respawn-state init, and while the local player is dead; the
// zooms reset to the spawn defaults with the player init.
// [orig: g_MapOverlayMode — cycle HUD_CycleMapMode @0x520bc0 (0->2->3->0),
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
	// [orig: Player_InitPlayer @0x4e1693..0x4e1763 — g_BmsMapZoom @0xA7640C
	//  (g_BmsHeaderBlock+0x23C, bulk fread), floor flt_7C486C = 0.0625,
	//  x flt_7CD424 = 524288 -> g_BigMapZoom, x flt_7C32BC = 65536 ->
	//  g_SpinmapZoom]
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

    // Mortar scope callbacks preserve an already-open fullscreen map.
	// [orig: @0x54330C/@0x54339C/@0x5434E7]
	void weapon_command(int command) {
		if (command > 0 && mode == 0)
			mode = 2;
		else if (command < 0 && mode == 2)
			mode = 0;
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
	// [orig: @0x5cac67..0x5cac6d — g_MapOverlayMode = 0 on Flags & 2]
	void on_local_player_dead() { mode = 0; }
	// The respawn init every overlay-window action runs zeroes the mode
	// (hud_toggles.h kOverlayWindowsCleared) [orig: Game_InitRespawnState
	// @0x499360, g_MapOverlayMode = 0 @0x499395].
	void on_respawn_init() { mode = 0; }
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
