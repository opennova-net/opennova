#pragma once

// The WINDOWED map views: the DEATH deploy screen's MAP window
// (MapOverlay_DrawView, fed by CMap_OverlayInputHandler) and the CMAP
// command map's MAP / ORDERS_MAP controls (CMapWindow_HandleEvent, drawing
// through HUD_BuildMapOverlayView mode 4). Both are menu custom-draw
// callbacks: event 1 draws into the widget's device rect, the mouse-class
// events pan (left drag) and zoom (right drag, wheel). This header owns their
// pan/zoom state machines and the DEATH pass's own legs; the terrain and the
// marker walk ride the spinmap compiler (hud_minimap.h, map mode 4).
// Witness record: docs/interface/hud-re.md (D-HUD-19).
// [orig: CMap_OverlayInputHandler @0x554310 -> MapOverlay_DrawView @0x5a58e0;
//  CMapWindow_HandleEvent @0x5497f0 -> HUD_BuildMapOverlayView @0x5a7e10]

#include <runtime/hud/hud_minimap.h>

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// The mouse-class menu events both map callbacks switch on (the payload
// carries the menu DESIGN mouse x/y at +8/+12 — the device point divided by
// the menu scale, map_view_device_to_design — and the button mask at +16).
// [orig: CMap_OverlayInputHandler @0x554310 — the jump table @0x55455a and
//  the move leg @0x5543c4; CMapWindow_HandleEvent @0x5497f0 — @0x549e9c]
enum class MapViewEvent : uint32_t {
	kMove = 0x1000001u,
	kLeftDown = 0x1000002u,
	kLeftUp = 0x1000003u,
	kRightDown = 0x1000005u,
	kRightUp = 0x1000006u,
	kWheel = 0x100000Bu,
};
// The move payload's button mask (+16): bit 0 the left button, bit 1 the
// right [orig: `test byte ptr [eax+10h], 1` @0x5543d5, `, 2` @0x554473].
inline constexpr uint32_t kMapViewButtonLeft = 1u;
inline constexpr uint32_t kMapViewButtonRight = 2u;

// The shared zoom policy: a multiplier on the 65536-unit world extent,
// clamped to [0.1, 10], opening at 4.0; a zoom-in step is x0.85 and a
// zoom-out step x(1/0.85); the right-drag zoom is 1.005^(-dx).
// [orig: flt_7C69F4 = 0.1, flt_7C44B4 = 10.0, flt_7C44B8 = 4.0,
//  flt_7D2214 = 0.85, flt_7D2210 = 1.1764705, dbl_7D2288 = 1.005]
inline constexpr float kMapViewZoomMin = 0.1f;
inline constexpr float kMapViewZoomMax = 10.0f;
inline constexpr float kMapViewZoomInit = 4.0f;
inline constexpr float kMapViewZoomInStep = 0.85f;
inline constexpr float kMapViewZoomOutStep = 1.1764705f;
inline constexpr double kMapViewDragZoomBase = 1.005;

// base^exponent by binary exponentiation, the reciprocal for a negative
// exponent. The game runs with the x87 precision control at 24 bits (the
// device is created without D3DCREATE_FPU_PRESERVE), so every product and
// the reciprocal round to single precision.
// [orig: Math_IntegerPow @0x54b500]
double map_view_integer_pow(double base, int exponent);

// A custom-draw payload's device rect (+8..+20: left, top, right, bottom).
struct MapViewRect {
	int32_t left = 0;
	int32_t top = 0;
	int32_t right = 0;
	int32_t bottom = 0;
};

// One menu design coordinate onto the device: v x the scene's scale factor,
// truncated [orig: CUIScene_ScaleRectDesignToDevice @0x63b210 — (int)(v *
// scale) per edge]. The menu scale is surface / 800 (x) or / 600 (y).
int32_t map_view_design_to_device(int32_t design, float scale);
// One device mouse coordinate onto the menu design space the mouse-class
// events carry: v / the scene's scale factor, truncated [orig:
// UI_DispatchMouseEvent @0x63ab00 — `fild g_MouseState.x; fdiv [ebx+4];
// _ftol2_sse`, the same for y over [ebx+8]].
int32_t map_view_device_to_design(int32_t device, float scale);

// The pan/zoom view both map callbacks keep: the zoom multiplier, the pan
// offset from the local player (16.16 mission units), the drag latches and
// the last mouse point, and the window width + pixel ratio the render pass
// stamps for the move leg's pixel-to-world factor.
struct MapViewPan {
	float zoom = 0.0f;
	int32_t pan_x = 0;
	int32_t pan_y = 0;
	bool left_drag = false;
	bool right_drag = false;
	int32_t last_x = 0;
	int32_t last_y = 0;
	int32_t window_w = 0;
	int32_t window_h = 0;
	float pixel_ratio = 0.0f;

	// The render-pass stamps: the window size and the menu's device ratio,
	// scaled(800) x 0.00125 [orig: @0x554336..0x55437f; flt_7D2294 = 0.00125].
	void stamp_window(const MapViewRect &rect, int32_t scaled_800);
	// The draw scale, world units per pixel: zoom x 65536 / (200 x width),
	// rounded to single precision [orig: @0x554385..0x5543b1].
	float draw_scale() const;
	// A left-drag move: K = zoom x 65536 / ((W / S) x 200), the x step
	// -dx x K x 65536 and the y step dy x K x 65536, each truncated.
	// [orig: @0x5543df..0x554444 (DEATH); @0x549c64..0x549cce (CMAP, the
	//  x product through flt_7C32B8 = -65536)]
	void drag_pan(int32_t x, int32_t y, int32_t &out_dx, int32_t &out_dy);
	// A right-drag move: zoom x= 1.005^(-dx), stored before the clamp to
	// [0.1, 10] [orig: @0x55447d..0x554504 (DEATH); @0x549cfe..0x549d72 (CMAP)].
	void drag_zoom(int32_t x, int32_t y);
	// One zoom step: direction > 0 zooms in (x0.85, floor 0.1), < 0 out
	// (x1.1764705, ceiling 10), 0 nothing [orig: sub_5535D0 @0x5535d0 (the
	// DEATH wheel); loc_548230 @0x548230 (the CMAP buttons and wheel)].
	void zoom_step(int direction);
};

// The DEATH screen map's frame parameters for one render pass: the payload
// rect, the view centre (the local player + the pan) and the draw scale.
// [orig: CMap_OverlayInputHandler event 1 @0x554332..0x5543b7 ->
//  MapOverlay_DrawView(rect, centerX, centerY, scale)]
struct DeathMapFrame {
	MapViewRect rect;
	int32_t center_x = 0;
	int32_t center_y = 0;
	float scale = 1.0f;
};

// DeathScreen_UpdateUI's zoom fit: the spawn-zone AABB (retail's swapped-name
// globals, the true min/max here), the local player's position, and the
// DEATH_SHROUD window's authored rect size (800x600 design units).
struct DeathMapFitInput {
	bool shroud_present = false;
	int32_t shroud_w = 0;
	int32_t shroud_h = 0;
	int32_t bounds_min_x = 0;
	int32_t bounds_min_y = 0;
	int32_t bounds_max_x = 0;
	int32_t bounds_max_y = 0;
	int32_t player_x = 0;
	int32_t player_y = 0;
};

// The DEATH map's state machine (the 0x25A39xx globals).
// [orig: flt_25A39B8 zoom; dword_25A39B0/B4 pan; dword_25A3990/398C the pan
//  target; byte_25A39A4/A5 the drag latches; dword_25A39A8/AC the last mouse
//  point; dword_25A3998/399C the window size; flt_25A3994 the pixel ratio;
//  dword_25A39BC the init latch]
struct DeathMapView {
	MapViewPan view;
	int32_t target_x = 0;
	int32_t target_y = 0;
	bool initialized = false;

	// The screen's load event: once per load, the first-ever load seeds zoom
	// 4.0 and a zero pan (a later load keeps the zoom). The three bytes the
	// seed also sets (byte_25A39A0..A2) have no reader.
	// [orig: HUD_InitDeathScreen @0x5546d0, from UI_DispatchScreenEvent event
	//  3 @0x54eb94; the unload clears the latch, sub_553120 @0x553120 from
	//  event 4 @0x54ee21]
	void on_load();
	void on_unload() { initialized = false; }
	// The screen's show event: fit the zoom to the zone AABB widened to hold
	// the player (+-20 wu) against the shroud's larger side, and centre the
	// pan on the AABB; an empty AABB (all four zero) keeps the unwidened
	// zeros. A zero extent resets zoom 4.0 / pan 0 and skips the 10.0 test;
	// otherwise a zoom that lands on exactly 10.0 zeroes the pan and target.
	// Without the shroud window only the 10.0 test runs on the held zoom.
	// [orig: DeathScreen_UpdateUI @0x553150 — the fit @0x553256..0x5533d7,
	//  the 10.0 test @0x5533e5..0x55340c, from UI_DispatchScreenEvent event 5
	//  @0x54efd8]
	void fit(const DeathMapFitInput &in);
	// The per-main-frame ease: the pan moves one eighth of the way to the
	// target (arithmetic shift) every frame the menu scene runs. The zoom
	// ease beside it keys on dword_25A3988, which every writer sets to 0 —
	// dead code, not ported.
	// [orig: DeathScreen_UpdateShroudReveal @0x554730 — @0x554784..0x5547b4,
	//  the dead zoom ease @0x5547ad..0x5547d8; per frame from
	//  UI_TeardownScene @0x54e67c]
	void ease_frame();
	// Event 1: stamp the window and hand back the view the pass draws.
	// [orig: CMap_OverlayInputHandler @0x554332..0x5543b7]
	DeathMapFrame render(const MapViewRect &rect, int32_t scaled_800, int32_t player_x,
			int32_t player_y);
	// The mouse-class events. Returns true when the event is a move — the
	// handler then looks up DEATH/SPAWNPOINTS_TABLE and deselects every row
	// (CTableWnd_SetRowSelected(-1, 0)); the shipped death.mnu authors no
	// SPAWNPOINTS_TABLE (its list is SPAWNPOINTS_LIST), so the lookup fails.
	// A move with neither drag armed by its button clears both latches.
	// `wheel` is the wheel remainder for kWheel (> 0 forward).
	// [orig: CMap_OverlayInputHandler @0x554310 — the down/up cases
	//  @0x554561..0x5545bb, the wheel @0x5545bc, the move @0x5543c4..0x554541]
	bool on_event(MapViewEvent event, int32_t x, int32_t y, uint32_t buttons, int32_t wheel);
};

// The CMAP screen's toggle bytes: GRID (the grid leg, bit 12), TEXT (bits
// 11/14/17), WAYPOINTS (bit 17), and CREATE_WAYPOINTS (the left click places
// a waypoint instead of panning).
// [orig: byte_252DD60 GRID sub_547FF0 @0x547ff0; byte_252DD61 TEXT
//  sub_5481A0 @0x5481a0; byte_252DD62 WAYPOINTS sub_548080 @0x548080;
//  byte_252DD63 CREATE_WAYPOINTS Settings_BuildDefaultProfile @0x548180]
struct CommandMapToggles {
	bool grid = true;
	bool text = true;
	bool waypoints = true;
	bool create_waypoints = false;
};
// The toggle controls by index (each draw toggle has an ORDERS_ twin).
enum CommandMapToggle : int {
	kCommandToggleGrid = 0,
	kCommandToggleText = 1,
	kCommandToggleWaypoints = 2,
	kCommandToggleCreateWaypoints = 3,
};

// The CMAP map's state machine (the 0x252DDxx globals).
// [orig: flt_252DD78 zoom; dword_252DD70/74 pan; byte_252DD64/65 the drag
//  latches; g_CMapClickPoint / dword_252DD6C the last mouse point;
//  dword_252DD58/5C the window size; flt_252DD54 the pixel ratio;
//  dword_252DD7C the init latch]
struct CommandMapView {
	MapViewPan view;
	CommandMapToggles toggles;
	bool initialized = false;

	// The screen's first open: zoom 4.0, every draw toggle on, a zero pan.
	// The same init clears the waypoint table (world/user_waypoints.h, held
	// per mission) and allocates the orders list (the presenter's).
	// [orig: sub_54B280 @0x54b280]
	void on_load();
	// The mouse-class events. A left press with CREATE_WAYPOINTS set asks for
	// the waypoint-name dialog instead of panning (kPlaceWaypoint: the
	// embedder runs the dialog leg and stores the click point); the wheel is
	// ignored while the right drag runs. A move with neither drag armed clears
	// both latches and asks for the waypoint hover test (kHoverTest).
	// [orig: CMapWindow_HandleEvent @0x5497f0 — @0x549e9c..0x54a087, the move
	//  @0x549c47..0x549e7b]
	enum class EventResult { kNone, kPlaceWaypoint, kHoverTest };
	EventResult on_event(MapViewEvent event, int32_t x, int32_t y, uint32_t buttons,
			int32_t wheel);
	// The ZOOMIN / ZOOMOUT button pair (user data 1 / -1).
	// [orig: loc_548230 registered by CCommandMap_RegisterAllControls]
	void zoom_button(int direction) { view.zoom_step(direction); }
	// A toggle control's state-change event (0x3000001): GRID / TEXT /
	// WAYPOINTS store the control's checked state (the embedder then checks
	// both twins from the byte); CREATE_WAYPOINTS flips its byte.
	// [orig: sub_547FF0 @0x548000..0x548011; sub_5481A0 @0x5481b0..0x5481c1;
	//  sub_548080 @0x548090..0x5480a1; Settings_BuildDefaultProfile @0x548180
	//  (`byte_252DD63 = byte_252DD63 == 0`)]
	void toggle_changed(int which, bool checked);
	bool toggle(int which) const;
	// Event 1: stamp the window, then turn the params block (mode 4, the three
	// toggles, the player position + pan, the float zoom, the payload rect)
	// into the compile input HUD_BuildMapOverlayView builds — the rect to the
	// 1024x768 virtual space by Viewport_ScreenToVirtual, the mode-4 mask, the
	// north-up angle the windowed compile fixes, and HUD_DrawMapOverlay's
	// scale (Z x 65536 over the scaled width x 200). `base` arrives carrying
	// the terrain / markers / surface / flip the compiler reads; its view
	// fields are overwritten. The render's depth/colour clear of the rect,
	// the player crosshair and the waypoint hover labels beside the call are
	// the unported screen's.
	// [orig: CMapWindow_HandleEvent render @0x549861..0x54998f;
	//  HUD_BuildMapOverlayView @0x5a7e10 — mode 4 @0x5a7f1a..0x5a7faf, the
	//  zoom @0x5a803c]
	void render(const MapViewRect &rect, int32_t scaled_800, int32_t player_x,
			int32_t player_y, int32_t player_z, HudMinimapInput &base);
};

// THE CMAP USER-WAYPOINT LEGS (world/user_waypoints.h holds the table).

// The WAYPOINTNAME_DLG placement: the dialog's authored size centred on the
// click, pushed right onto the map control's left edge or left onto its right
// edge (the vertical is not clamped). Design units.
// [orig: CMapWindow_HandleEvent @0x549ef0..0x549f8a — CWnd_GetRect of the
//  dialog and of the map control, OffsetRect]
MapViewRect command_map_waypoint_dialog_rect(int32_t click_x, int32_t click_y,
		const MapViewRect &dialog, const MapViewRect &map);

// The confirm's world point: the click (the map control's local design
// point, the view's last point) against the view the last render stamped:
// P = Z x 65536 / ((W / S) x 200) x 65536, x = player x - trunc((W / S / 2 -
// click x) x P) + pan x, y = player y + trunc((H / S / 2 - click y) x P) + pan
// y, each product at single precision.
// [orig: CMap_HandleWaypointCreateConfirm @0x54a11e..0x54a193]
void command_map_waypoint_world(const CommandMapView &cmap, int32_t player_x, int32_t player_y,
		int32_t &out_x, int32_t &out_y);

// The placed-waypoint table's slot count [orig: g_ConnectionSlotTable
// @0x252dcd0, 16 x 8 bytes; the `dword_252DD50 < 16` gates].
inline constexpr int kCommandMapWaypointSlots = 16;

// One placed waypoint as the last CMAP render projected it: the device point
// of its position through the map transform and its name's bold-label size
// (device pixels). `live` false for an empty table slot.
struct CommandMapWaypointAnchor {
	bool live = false;
	int32_t x = 0;
	int32_t y = 0;
	int32_t text_w = 0;
	int32_t text_h = 0;
};

// The move leg's hover test over the table slots: each slot's hover byte
// clears, then a live slot whose design-space box (from 5 pixels plus the
// delete button's width left of the anchor to the name's device width right
// of it, from the anchor down the name's device height) holds the mouse sets
// its byte and ends the walk; the slots after it keep their bytes. The
// device anchor divides by the menu scale into design units (truncated).
// [orig: CMapWindow_HandleEvent @0x549da5..0x549e7b — Terrain_FixedPointToWorldFloat_Default,
//  HUD_MeasureTextWH(name, g_HUDLabelFont[1]), sub_63B1E0 @0x63b1e0]
void command_map_waypoint_hover(const CommandMapWaypointAnchor *anchors, bool *hover, int count,
		int32_t mouse_x, int32_t mouse_y, float scale_x, float scale_y, int32_t close_w);

// The render leg's delete-button placement: the first live hovered slot's
// device anchor less the button's width and 5 pixels, into design units (the
// button takes that corner at its own square size). False: nothing hovered,
// the button hides.
// [orig: CMapWindow_HandleEvent @0x549b6e..0x549c23 — sub_646580 @0x646580]
bool command_map_close_button_position(const CommandMapWaypointAnchor *anchors,
		const bool *hover, int count, float scale_x, float scale_y, int32_t close_w,
		int32_t &out_x, int32_t &out_y);

// The mode-4 content mask HUD_BuildMapOverlayView derives from the CMAP
// params: 0xAF937 (0xAE937 without GRID), TEXT off clears bits 11/14/17,
// WAYPOINTS off clears bit 17; mode 4 never carries bit 16.
// [orig: HUD_BuildMapOverlayView @0x5a7e8a..0x5a7eb2 and @0x5a7f82]
uint32_t command_map_mask(const CommandMapToggles &toggles);

// One spawn zone the DEATH pass letters: a transient/persistent bank slot
// whose entity's def carries attrib 0x40000.
struct DeathMapZone {
	uint16_t handle = 0xFFFF;
	// SpawnZoneList_IndexOf over the sorted registry; the letter is 'A' + it.
	int32_t index = -1;
	uint8_t team = 0; // entity +354
	// The zone-timer entry is missing or its level has reached its limit.
	bool timer_ready = true;
	// The label anchor: the entity's Euler matrix applied to its bbox centre
	// (+0x1FC) about its position, 16.16 mission units [orig: sub_59C300
	// @0x59c300 — Math_BuildFixedPointMatrixFromEulerAngles @0x613f40,
	// Math_FixedPointTransformPoint22 @0x615810].
	int32_t anchor_x = 0;
	int32_t anchor_y = 0;
	// The zone ENTITY's +550 / +548 words, which every S2C 0x6E group naming
	// the zone rewrites (the member count, the wave countdown) and a later
	// 0x6E that leaves the zone out keeps: they stay the last values written
	// [orig: NapiNPClientMsg_HandleSquadRosterSync @0x429880 — the stores
	// @0x4299bf / @0x4299c5, no reset of an unlisted zone].
	uint8_t queued = 0;
	uint16_t countdown = 0;
	// The entity reads the per-zone blip selector makes (sub_597FD0 and
	// Minimap_GetCapturePointInfo; death_map_zone_blip below).
	bool has_def = true;        // entity+0x20
	uint32_t def_type = 0;      // ItemDef+0x5C
	uint32_t def_attrib = 0;    // ItemDef+0x54
	int32_t def_id = 0;         // ItemDef+0x50
	bool dead = false;          // entity+0x24 & 2
	bool carried = false;       // entity+0x24 & 1
	bool local_player = false;  // the entity is g_LocalPlayerEntity
	bool parent_item = false;   // entity+0x28 set, its def type 1
	uint8_t zone_number = 0;    // entity+0x21A
	uint8_t capture_team = 0;   // entity+0x223, the S2C 0x53 mode_b
	bool occupant_present = false; // entity+0x170
	uint8_t occupant_team = 0;  // the occupant's +0x162
};

// The world facts the DEATH pass reads beyond the spinmap input.
struct DeathMapFacts {
	bool player_present = false;
	int32_t player_x = 0;
	int32_t player_y = 0;
	uint8_t player_team = 0;
	// g_DeployScreenActive — the player crosshair is skipped while it is up.
	bool deploy_screen_active = false;
	// dword_A85B68, the spawn-target hold seconds.
	int32_t hold_seconds = 0;
	uint32_t game_type = 0;
	// word_A85BC0, the zone whose 0x6E wave lists the local player.
	uint16_t self_zone_handle = 0xFFFF;
	std::vector<DeathMapZone> zones;
	// The local player's height: the CMAP params' position Z (params+0x14).
	int32_t player_z = 0;
	// g_NapiNPCtx.is_in_session and the HUD info entity's team byte, the
	// capture-point colour pick's gates [orig: Minimap_GetCapturePointInfo
	// @0x5971c0 — @0x5971ec, g_HUDInfoCurrentEntity+354 @0x597263].
	bool in_session = false;
	uint8_t hud_team = 0;
	// The spawn-zone AABB for the show-event fit.
	int32_t bounds_min_x = 0;
	int32_t bounds_min_y = 0;
	int32_t bounds_max_x = 0;
	int32_t bounds_max_y = 0;
	// The CMAP's placed-waypoint table (world/user_waypoints.h) as the hover
	// and delete-button legs read it: each slot's position and name, `live`
	// false for an empty slot [orig: g_ConnectionSlotTable @0x252dcd0 — the
	// entity's +4 position and +244 name].
	struct UserWaypoint {
		bool live = false;
		int32_t x = 0;
		int32_t y = 0;
		std::string name;
	};
	std::array<UserWaypoint, kCommandMapWaypointSlots> user_waypoints;
};

// The DEATH window's compiled pass: the spinmap-shaped pass, then the zone
// walk's segments, then the player crosshair (the device submits them in that
// order). The walk draws each zone's blip and then that zone's letters before
// the next zone, so a later zone's blip covers an earlier zone's letters: one
// segment per walked zone, each the half-open end of its blip overlays /
// sprites / lines and of its labels inside `zones`.
// [orig: MapOverlay_DrawView @0x5a5a4d..0x5a5d2c — sub_597FD0 @0x5a5adc then
//  the HUD_DrawTextCentered_HalfBright calls @0x5a5c59..0x5a5d14 per slot]
struct HudMapWindowSegment {
	size_t overlay_end = 0;
	size_t sprite_end = 0;
	size_t line_end = 0;
	size_t label_end = 0;
};
struct HudMapWindowPass {
	HudMapPass map;
	HudMapPass zones;
	std::vector<HudMapWindowSegment> segments;
	std::vector<HudMapLine> over_lines;
};

// sub_597FD0's pick for one zone: whether Minimap_DrawBlip runs, and its
// colour and cell (the size argument). By def type: 5 without attrib 0x20000
// the team colour or 0xFF707070, cell 0; 3 (a person) the team colour, cell
// 3, halved while it is the dead local player on the 0x10 blink phase; 2
// nothing; else attrib 0x20 (an attached item) 0xFF907000 cell 4 unless its
// parent is a vehicle or it is dead, otherwise the capture-point pick or the
// team colour (0xFF707070 past team 2), cell 0. Attrib 0x20000 (a capture
// zone) then forces cell 0 and, for a neutral zone being taken (+0x223), the
// taker's colour on the 0x20 blink phase, or for a zone without a zone number
// the halved colour on that phase. `frame` is the unpaused main-frame
// counter (dword_A87060).
// [orig: sub_597FD0 @0x597fd0 — type 5 @0x597fe1..0x598018, type 3
//  @0x59801d..0x598060, type 2 @0x598065, attrib 0x20 @0x598071..0x59809e,
//  Minimap_GetCapturePointInfo @0x5980ab, the team fallback @0x5980b7..0x5980e5,
//  attrib 0x20000 @0x5980ef..0x598149, the halving `sar esi, 1; and esi,
//  7F7F7F7Fh` @0x598147, Minimap_DrawBlip @0x598170]
struct DeathMapZoneBlip {
	bool draw = false;
	uint32_t color = 0;
	uint8_t cell = 0;
};
DeathMapZoneBlip death_map_zone_blip(const DeathMapZone &zone, const DeathMapFacts &facts,
		int32_t frame);

// The player crosshair both windows draw after their map: two full-window
// lines through the projected local player, the team colour pulsed at alpha
// 0x70, skipped while the deploy overlay is up. `view` is the compiled view's
// input (the projection), `rect` the payload's device rect.
// [orig: MapOverlay_DrawView @0x5a5d32..0x5a5ed6; CMapWindow_HandleEvent
//  @0x54999e..0x549b4a — g_DeployScreenActive gate, team 1 0xFF4040FF /
//  2 0xFFFF4040 / else 0xFFA0A0A0, HIBYTE = 0x70, LINELIST (x1, py)-(x2, py),
//  (px, y1)-(px, y2) through pass 0x10300000]
void map_view_crosshair(const HudMinimapInput &view, const MapViewRect &rect,
		const DeathMapFacts &facts, int32_t frame_counter, std::vector<HudMapLine> &out);

// The DEATH map mask handed to the spinmap compiler: the marker walk (bit 1)
// and the entity labels with names (bits 5 and 13). MapOverlay_DrawView calls
// the same drawers HUD_DrawMapOverlay gates on those bits, with the mode-4
// ctx values (draw mode 0xFF, labels alpha 0xFF); it draws no backing, no
// disc, no waypoint line, no grid and no compass. Its tracked-target call
// passes no ctx and a zero radius (no label race).
// [orig: MapOverlay_DrawView @0x5a58e0 — MapOverlay_RenderAllLayers(rect,
//  0x3FFFFFC0, 0xFF) @0x5a5a11, HUD_DrawEntityLabelsAndMarkers(rect,
//  0x40000000, 1, 1, 0xFF) @0x5a5a29, Render_LaserSightEffect(rect, 0, 0.0,
//  0) @0x5a5a40]
inline constexpr uint32_t kDeathMapMask = 0x2022u;

// Compiles the DEATH MAP window: the terrain + marker walk through the
// spinmap compiler's windowed mode (the device rect, north-up, the caller's
// float scale), then the zone walk — per zone, in bank-slot order, the blip
// Minimap_DrawBlip draws from the zone entity with sub_597FD0's colour and
// cell (death_map_zone_blip), then that zone's letters with their colours /
// pulse / score lines — and the player crosshair. Lives across frames so its
// scratch keeps capacity. The tracked-target call draws only what the frame's
// tracked-target feed holds (D-HUD-34 owns its missing sources).
// [orig: MapOverlay_DrawView @0x5a58e0]
class DeathMapCompiler {
public:
	// `input` arrives carrying the frame's terrain, markers, footprints, icon
	// strip, the surface size and the HUD frame counter (ticks); its view
	// fields are overwritten.
	void compile(HudMinimapInput &input, const DeathMapFrame &frame,
			const DeathMapFacts &facts, HudMapWindowPass &out);

private:
	struct ZoneSlot {
		const HudMinimapMarker *marker;
		const DeathMapZone *zone;
	};
	HudMinimapCompiler compiler_;
	std::vector<ZoneSlot> walk_;
	// Minimap_DrawBlip's clip / crop scratch for the per-zone redraw.
	std::vector<HudMapVertex> clip_a_;
	std::vector<HudMapVertex> clip_b_;
	std::vector<HudMapGeomVertex> geom_a_;
	std::vector<HudMapGeomVertex> geom_b_;
	std::vector<const HudMinimapFootprint *> footprint_index_;
	uint32_t tracked_color_ = 0;
	uint8_t tracked_alpha_ = 0;
};

// Compiles the CMAP MAP / ORDERS_MAP window: the render pass clears the rect
// (colour 0x18 into the target), HUD_BuildMapOverlayView mode 4 draws the map
// through the spinmap compiler (CommandMapView::render builds its input),
// then the player crosshair; each placed waypoint's anchor comes out for the
// hover test and the delete button (command_map_waypoint_hover /
// command_map_close_button_position).
// [orig: CMapWindow_HandleEvent render @0x54981c..0x549b4a — the clear
//  CGfxDevice_SetClearColor(24) + CGfxDevice_Clear(rect, 3) @0x549826..0x54985c]
class CommandMapCompiler {
public:
	// `anchors` (kCommandMapWaypointSlots entries) receive each live table
	// slot's projected device point; the caller measures the names.
	void compile(HudMinimapInput &input, CommandMapView &cmap, const MapViewRect &rect,
			int32_t scaled_800, const DeathMapFacts &facts, HudMapWindowPass &out,
			CommandMapWaypointAnchor *anchors);

private:
	HudMinimapCompiler compiler_;
};

} // namespace opennova::hud
