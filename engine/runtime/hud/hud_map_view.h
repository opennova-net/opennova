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

#include <cstdint>
#include <vector>

namespace opennova::hud {

// The mouse-class menu events both map callbacks switch on (the payload
// carries the device mouse x/y at +8/+12 and the button mask at +16).
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
	// The same init clears the waypoint table and allocates the orders list
	// (both unported with the rest of the screen).
	// [orig: sub_54B280 @0x54b280]
	void on_load();
	// The mouse-class events. A left press with CREATE_WAYPOINTS set places a
	// waypoint (the WAYPOINTNAME_DLG flow, unported) and does not pan; the
	// wheel is ignored while the right drag runs. A move with neither drag
	// armed clears both latches (the waypoint hover test beside it is part
	// of the unported waypoint flow).
	// [orig: CMapWindow_HandleEvent @0x5497f0 — @0x549e9c..0x54a087, the move
	//  @0x549c47..0x549e7b]
	void on_event(MapViewEvent event, int32_t x, int32_t y, uint32_t buttons, int32_t wheel);
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
	uint8_t queued = 0;     // entity +550, the 0x6E queued count
	uint16_t countdown = 0; // entity +548, the 0x6E wave countdown
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
	// The spawn-zone AABB for the show-event fit.
	int32_t bounds_min_x = 0;
	int32_t bounds_min_y = 0;
	int32_t bounds_max_x = 0;
	int32_t bounds_max_y = 0;
};

// The DEATH window's compiled pass: the spinmap-shaped pass plus the player
// crosshair, which draws after the zone letters (the device submits it last).
struct HudMapWindowPass {
	HudMapPass map;
	std::vector<HudMapLine> over_lines;
};

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
// float scale), then the view's own legs in order — every zone's blip redrawn
// from its bank slot, the zone letters with their colours / pulse / score
// lines, and the player crosshair. Lives across frames so its scratch keeps
// capacity. Residuals: the letters draw above every zone blip (retail
// interleaves blip, letters per zone), and the tracked-target call draws
// nothing (no tracked-target source).
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
	HudMinimapInput zone_input_;
	HudMapPass zone_pass_;
	std::vector<ZoneSlot> walk_;
};

// Compiles the CMAP MAP / ORDERS_MAP window: the render pass clears the rect
// (colour 0x18 into the target), HUD_BuildMapOverlayView mode 4 draws the map
// through the spinmap compiler (CommandMapView::render builds its input),
// then the player crosshair. The waypoint hover labels the pass also places
// are the unported waypoint flow's.
// [orig: CMapWindow_HandleEvent render @0x54981c..0x549b4a — the clear
//  CGfxDevice_SetClearColor(24) + CGfxDevice_Clear(rect, 3) @0x549826..0x54985c]
class CommandMapCompiler {
public:
	void compile(HudMinimapInput &input, CommandMapView &cmap, const MapViewRect &rect,
			int32_t scaled_800, const DeathMapFacts &facts, HudMapWindowPass &out);

private:
	HudMinimapCompiler compiler_;
};

} // namespace opennova::hud
