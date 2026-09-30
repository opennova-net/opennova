#include <runtime/hud/hud_map_view.h>

#include <base/io/crt_ftol.h>
#include <runtime/hud/hud_math.h>

#include <algorithm>
#include <cstdio>

namespace opennova::hud {

namespace {

// The x87 runs at 24-bit precision control in game (the device is created
// without D3DCREATE_FPU_PRESERVE): every arithmetic result rounds to single
// precision. [orig: CGfxDevice_CreateDevice @0x67e9fd / @0x67ea35]
float pc24(double value) {
	return static_cast<float>(value);
}

// Wrapping 32-bit add/sub (the handlers' plain `add`/`sub` on the globals).
int32_t wrap_add(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) + static_cast<uint32_t>(b));
}
int32_t wrap_sub(int32_t a, int32_t b) {
	return static_cast<int32_t>(static_cast<uint32_t>(a) - static_cast<uint32_t>(b));
}

// The two-sided fcom/fnstsw clamp the drag-zoom and the fit run on the
// already-stored product: below 0.1 stores 0.1, above 10 stores 10.
// [orig: @0x5544ba..0x554504 (DEATH drag); @0x553385..0x5533b9 (the fit);
//  @0x549d2f..0x549d72 (CMAP drag)]
float clamp_zoom(float zoom) {
	if (zoom < kMapViewZoomMin) return kMapViewZoomMin;
	if (zoom > kMapViewZoomMax) return kMapViewZoomMax;
	return zoom;
}

// The zone-letter and crosshair pulse: (frame - 8) & 0x3F folded above 0x20.
// [orig: MapOverlay_DrawView @0x5a5b30..0x5a5b56 / @0x5a5d5a..0x5a5da6]
int32_t pulse_phase(int32_t frame) {
	int32_t phase = (frame - 8) & 0x3F;
	if (phase > 0x20) phase = 63 - phase;
	return phase;
}

// Each colour byte moves phase/32 of the way to 0xFF (a byte add).
// [orig: @0x5a5b9b..0x5a5bf6 / @0x5a5da8..0x5a5e83]
uint32_t brighten(uint32_t argb, int32_t phase) {
	uint32_t out = argb & 0xFF000000u;
	for (int shift = 0; shift <= 16; shift += 8) {
		const uint32_t c = (argb >> shift) & 0xFFu;
		const uint32_t lifted =
				(c + ((static_cast<uint32_t>(phase) * (255u - c)) >> 5)) & 0xFFu;
		out |= lifted << shift;
	}
	return out;
}

// The payload rect in the virtual space the windowed compile scales back:
// device x extent / surface lands exactly on the device edge again through
// Viewport_ScaleToVirtualCoords's round-to-nearest (hud_math scale_axis).
float device_to_virtual(int32_t device, float surface, double extent) {
	if (surface <= 0.0f) return 0.0f;
	return static_cast<float>(static_cast<double>(device) * extent /
			static_cast<double>(surface));
}

void add_label(HudMapPass &pass, int32_t x, int32_t y, const char *text, uint32_t color) {
	HudMapLabel label;
	label.x = static_cast<float>(x);
	label.y = static_cast<float>(y);
	std::snprintf(label.text, sizeof(label.text), "%s", text);
	label.color = color;
	label.align = 0; // HUD_DrawTextCentered_HalfBright
	label.font = 0;  // g_HUDLabelFontBold
	pass.labels.push_back(label);
}

// The zone team colours [orig: g_MapOverlayTeamColor @0x840A38 = 0xFF304080
// (team 1), dword_840A34 = 0xFF802020 (team 2), dword_840A40 = 0xFF208020
// (else), read @0x5a5ae9..0x5a5b11; the local player's wave zone
// 0xFFC8C814 @0x5a5b28]
uint32_t zone_color(uint8_t team) {
	if (team == 1) return 0xFF304080u;
	if (team == 2) return 0xFF802020u;
	return 0xFF208020u;
}

} // namespace

double map_view_integer_pow(double base, int exponent) {
	// [orig: Math_IntegerPow @0x54b500 — |exponent| @0x54b50a..0x54b50e, the
	//  square-and-multiply loop @0x54b514..0x54b520, 1/acc @0x54b52a]
	unsigned int bits = exponent < 0 ? 0u - static_cast<unsigned int>(exponent)
			: static_cast<unsigned int>(exponent);
	double accumulator = 1.0;
	while (true) {
		if ((bits & 1u) != 0) accumulator = pc24(accumulator * base);
		bits >>= 1;
		if (bits == 0) break;
		base = pc24(base * base);
	}
	if (exponent < 0) return pc24(1.0 / accumulator);
	return accumulator;
}

int32_t map_view_design_to_device(int32_t design, float scale) {
	// [orig: CUIScene_ScaleRectDesignToDevice @0x63b210 — (int)(v * scale)]
	return io::retail_ftol_sse2(static_cast<double>(pc24(
			static_cast<double>(design) * static_cast<double>(scale))));
}

void MapViewPan::stamp_window(const MapViewRect &rect, int32_t scaled_800) {
	// [orig: dword_25A3998 = R - L, dword_25A399C = B - T @0x554336..0x554356;
	//  flt_25A3994 = scaled(800) x 0.00125 @0x554369..0x55437f (flt_7D2294);
	//  the CMAP twins dword_252DD58/5C, flt_252DD54 @0x549861..0x5498b3]
	window_w = wrap_sub(rect.right, rect.left);
	window_h = wrap_sub(rect.bottom, rect.top);
	pixel_ratio = pc24(static_cast<double>(scaled_800) * static_cast<double>(0.00125f));
}

float MapViewPan::draw_scale() const {
	// [orig: fld flt_25A39B8; fmul 65536.0 (flt_7C32BC); fidiv (W x 200)
	//  @0x554369..0x5543b1]
	const double extent = static_cast<double>(zoom) * 65536.0;
	return pc24(extent / static_cast<double>(window_w * 200));
}

void MapViewPan::drag_pan(int32_t x, int32_t y, int32_t &out_dx, int32_t &out_dy) {
	const int32_t dx = wrap_sub(x, last_x);
	const int32_t dy = wrap_sub(y, last_y);
	last_x = x;
	last_y = y;
	// K = Z x 65536 / ((W / S) x 200), each step at single precision
	// [orig: @0x5543df..0x554429].
	const double extent = static_cast<double>(zoom) * 65536.0;
	const float window_world = pc24(static_cast<double>(
			pc24(static_cast<double>(window_w) / static_cast<double>(pixel_ratio))) * 200.0);
	const float k = pc24(extent / static_cast<double>(window_world));
	// x: -(dx x K) x 65536; y: (K x dy) x 65536, each truncated by _ftol2_sse
	// [orig: @0x55442b..0x554444].
	out_dx = io::retail_ftol_sse2(
			-(static_cast<double>(pc24(static_cast<double>(dx) * k)) * 65536.0));
	out_dy = io::retail_ftol_sse2(
			static_cast<double>(pc24(static_cast<double>(k) * dy)) * 65536.0);
}

void MapViewPan::drag_zoom(int32_t x, int32_t y) {
	const int32_t dx = wrap_sub(x, last_x);
	last_x = x;
	last_y = y;
	// zoom x= 1.005^(-dx), stored, then clamped [orig: @0x55447d..0x5544b4].
	zoom = pc24(map_view_integer_pow(kMapViewDragZoomBase, -dx) * static_cast<double>(zoom));
	zoom = clamp_zoom(zoom);
}

void MapViewPan::zoom_step(int direction) {
	// One-sided: an in step only floors, an out step only caps.
	// [orig: sub_5535D0 @0x5535d6..0x553625; loc_548230 @0x548230..0x548287]
	if (direction > 0) {
		zoom = pc24(static_cast<double>(zoom) * static_cast<double>(kMapViewZoomInStep));
		if (zoom < kMapViewZoomMin) zoom = kMapViewZoomMin;
	} else if (direction < 0) {
		zoom = pc24(static_cast<double>(zoom) * static_cast<double>(kMapViewZoomOutStep));
		if (zoom > kMapViewZoomMax) zoom = kMapViewZoomMax;
	}
}

void DeathMapView::on_load() {
	// [orig: HUD_InitDeathScreen @0x5546d0 — the latch dword_25A39BC
	//  @0x5546d0/@0x554724; `0.0 == flt_25A39B8` @0x5546de..0x5546f0 seeds
	//  4.0 (flt_7C44B8) and the pan zero @0x5546f2..0x55471a]
	if (initialized) return;
	if (view.zoom == 0.0f) {
		view.zoom = kMapViewZoomInit;
		view.pan_x = 0;
		view.pan_y = 0;
	}
	initialized = true;
}

void DeathMapView::fit(const DeathMapFitInput &in) {
	const auto reset_on_max = [this](float zoom) {
		// [orig: `10.0 == zoom` @0x5533e5..0x5533f2 -> the pan, the target and
		//  dword_25A3988 zeroed @0x5533f4..0x55340c]
		if (zoom == kMapViewZoomMax) {
			view.pan_x = 0;
			view.pan_y = 0;
			target_x = 0;
			target_y = 0;
		}
	};
	if (!in.shroud_present) {
		// No DEATH_SHROUD: the held zoom reaches the 10.0 test directly
		// [orig: @0x5533d9..0x5533df].
		reset_on_max(view.zoom);
		return;
	}
	// The spawn-zone AABB, widened to hold the player +-20 wu when any bound
	// is nonzero [orig: Entity_GetWorldBounds @0x43b950; @0x5532ae..0x553306,
	// 0xFFEC0000 / 0x140000].
	int32_t min_x = in.bounds_min_x;
	int32_t min_y = in.bounds_min_y;
	int32_t max_x = in.bounds_max_x;
	int32_t max_y = in.bounds_max_y;
	if (min_x != 0 || min_y != 0 || max_x != 0 || max_y != 0) {
		const int32_t lo_x = wrap_add(in.player_x, -0x140000);
		const int32_t lo_y = wrap_add(in.player_y, -0x140000);
		const int32_t hi_x = wrap_add(in.player_x, 0x140000);
		const int32_t hi_y = wrap_add(in.player_y, 0x140000);
		if (min_x > lo_x) min_x = lo_x;
		if (min_y > lo_y) min_y = lo_y;
		if (max_x < hi_x) max_x = hi_x;
		if (max_y < hi_y) max_y = hi_y;
	}
	// The larger AABB side [orig: @0x55330a..0x553322].
	const int32_t span_x = wrap_sub(max_x, min_x);
	const int32_t span_y = wrap_sub(max_y, min_y);
	const int32_t extent = span_x > span_y ? span_x : span_y;
	if (extent == 0) {
		// [orig: @0x553328..0x553342 — zoom 4.0, pan 0, the 10.0 test skipped]
		view.pan_x = 0;
		view.pan_y = 0;
		view.zoom = kMapViewZoomInit;
		return;
	}
	// zoom = extent / (larger shroud side x 0.4) / 65536 x 1.5, then clamped
	// [orig: @0x553347..0x5533b9 — flt_7C56A0 = 0.4, flt_7C3310 = 2^-16,
	//  flt_7D4B80 = 1.5; the shroud rect is CWnd_GetRect @0x6465c0, the
	//  authored 800x600 position].
	const int32_t dim = in.shroud_h > in.shroud_w ? in.shroud_h : in.shroud_w;
	const float divisor = pc24(static_cast<double>(dim) * static_cast<double>(0.4f));
	const float ratio = pc24(static_cast<double>(extent) / static_cast<double>(divisor));
	const float zoom = pc24(static_cast<double>(ratio) * (1.0 / 65536.0) *
			static_cast<double>(1.5f));
	view.zoom = clamp_zoom(zoom);
	// The pan centres the AABB [orig: @0x5533bb..0x5533cf].
	view.pan_x = wrap_sub(wrap_add(min_x, max_x) >> 1, in.player_x);
	view.pan_y = wrap_sub(wrap_add(min_y, max_y) >> 1, in.player_y);
	reset_on_max(view.zoom);
}

void DeathMapView::ease_frame() {
	// [orig: DeathScreen_UpdateShroudReveal @0x554784..0x5547b4 — pan +=
	//  (target - pan) >> 3 on both axes]
	view.pan_x = wrap_add(view.pan_x, wrap_sub(target_x, view.pan_x) >> 3);
	view.pan_y = wrap_add(view.pan_y, wrap_sub(target_y, view.pan_y) >> 3);
}

DeathMapFrame DeathMapView::render(const MapViewRect &rect, int32_t scaled_800,
		int32_t player_x, int32_t player_y) {
	// [orig: CMap_OverlayInputHandler event 1 @0x554332..0x5543b7 — the
	//  window stamp, then MapOverlay_DrawView(rect, pan + player X,
	//  pan + player Y, scale)]
	view.stamp_window(rect, scaled_800);
	DeathMapFrame frame;
	frame.rect = rect;
	frame.center_x = wrap_add(view.pan_x, player_x);
	frame.center_y = wrap_add(view.pan_y, player_y);
	frame.scale = view.draw_scale();
	return frame;
}

bool DeathMapView::on_event(MapViewEvent event, int32_t x, int32_t y, uint32_t buttons,
		int32_t wheel) {
	switch (event) {
	case MapViewEvent::kLeftDown:
		// [orig: @0x554561..0x55457e — the point, left on, right off]
		view.last_x = x;
		view.last_y = y;
		view.left_drag = true;
		view.right_drag = false;
		return false;
	case MapViewEvent::kLeftUp:
		view.left_drag = false; // [orig: @0x5545b1]
		return false;
	case MapViewEvent::kRightDown:
		// [orig: @0x554589..0x5545a6]
		view.last_x = x;
		view.last_y = y;
		view.left_drag = false;
		view.right_drag = true;
		return false;
	case MapViewEvent::kRightUp:
		view.right_drag = false; // [orig: @0x55457e]
		return false;
	case MapViewEvent::kWheel:
		// [orig: sub_5535D0(event, 0, g_MouseState.wheelRemainder) @0x5545c6]
		view.zoom_step(wheel > 0 ? 1 : (wheel < 0 ? -1 : 0));
		return false;
	case MapViewEvent::kMove:
		if (view.left_drag && (buttons & kMapViewButtonLeft) != 0) {
			// The step lands on the pan and on the eased target alike
			// [orig: @0x554449..0x55445b].
			int32_t dx = 0, dy = 0;
			view.drag_pan(x, y, dx, dy);
			view.pan_x = wrap_add(view.pan_x, dx);
			view.pan_y = wrap_add(view.pan_y, dy);
			target_x = wrap_add(target_x, dx);
			target_y = wrap_add(target_y, dy);
		} else if (view.right_drag && (buttons & kMapViewButtonRight) != 0) {
			view.drag_zoom(x, y);
		} else {
			view.left_drag = false; // [orig: @0x554510..0x554517]
			view.right_drag = false;
		}
		// [orig: UI_FindScreenControl(g_GameMenu, "DEATH", "SPAWNPOINTS_TABLE")
		//  @0x55452e -> CTableWnd_SetRowSelected(-1, 0) @0x554541]
		return true;
	}
	return false;
}

void CommandMapView::on_load() {
	// [orig: sub_54B280 @0x54b280 — the latch dword_252DD7C, zoom 4.0
	//  @0x54b293, the three toggles @0x54b2ab..0x54b2b7, the pan zero
	//  @0x54b2bd..0x54b2c7]
	if (initialized) return;
	view.zoom = kMapViewZoomInit;
	toggles.grid = true;
	toggles.text = true;
	toggles.waypoints = true;
	view.pan_x = 0;
	view.pan_y = 0;
	initialized = true;
}

void CommandMapView::on_event(MapViewEvent event, int32_t x, int32_t y, uint32_t buttons,
		int32_t wheel) {
	switch (event) {
	case MapViewEvent::kLeftDown:
		// CREATE_WAYPOINTS routes the click to the waypoint-name dialog
		// (unported); otherwise the point and the pan latch — the right latch
		// is left as it is [orig: @0x549eaa, @0x549fe4..0x54a000].
		if (toggles.create_waypoints) return;
		view.last_x = x;
		view.last_y = y;
		view.left_drag = true;
		return;
	case MapViewEvent::kLeftUp:
		view.left_drag = false; // [orig: @0x54a04f]
		return;
	case MapViewEvent::kRightDown:
		// [orig: @0x54a01b..0x54a02b]
		view.last_x = x;
		view.last_y = y;
		view.right_drag = true;
		return;
	case MapViewEvent::kRightUp:
		view.right_drag = false; // [orig: @0x54a03d]
		return;
	case MapViewEvent::kWheel:
		// Ignored while the right drag runs [orig: @0x54a05e..0x54a087].
		if (view.right_drag) return;
		view.zoom_step(wheel > 0 ? 1 : (wheel < 0 ? -1 : 0));
		return;
	case MapViewEvent::kMove:
		if (view.left_drag && (buttons & kMapViewButtonLeft) != 0) {
			// [orig: @0x549c64..0x549cd3]
			int32_t dx = 0, dy = 0;
			view.drag_pan(x, y, dx, dy);
			view.pan_x = wrap_add(view.pan_x, dx);
			view.pan_y = wrap_add(view.pan_y, dy);
		} else if (view.right_drag && (buttons & kMapViewButtonRight) != 0) {
			view.drag_zoom(x, y); // [orig: @0x549cfe..0x549d72]
		} else {
			view.left_drag = false; // [orig: @0x549d89..0x549d90]
			view.right_drag = false;
		}
		return;
	}
}

void CommandMapView::toggle_changed(int which, bool checked) {
	switch (which) {
	case kCommandToggleGrid: toggles.grid = checked; break;       // [orig: @0x548011]
	case kCommandToggleText: toggles.text = checked; break;       // [orig: @0x5481c1]
	case kCommandToggleWaypoints: toggles.waypoints = checked; break; // [orig: @0x5480a1]
	case kCommandToggleCreateWaypoints:
		toggles.create_waypoints = !toggles.create_waypoints; // [orig: @0x54818a..0x548194]
		break;
	default: break;
	}
}

bool CommandMapView::toggle(int which) const {
	switch (which) {
	case kCommandToggleGrid: return toggles.grid;
	case kCommandToggleText: return toggles.text;
	case kCommandToggleWaypoints: return toggles.waypoints;
	case kCommandToggleCreateWaypoints: return toggles.create_waypoints;
	default: return false;
	}
}

uint32_t command_map_mask(const CommandMapToggles &toggles) {
	// [orig: HUD_BuildMapOverlayView @0x5a7e8a..0x5a7eb2 — 0xAF937 / 0xAE937
	//  on params+5, & 0xFFFDB7FF without params+7, & 0xFFFDFFFF without
	//  params+6; mode 4 clears the |0x10000 @0x5a7f82]
	uint32_t mask = toggles.grid ? 0xAF937u : 0xAE937u;
	if (!toggles.text) mask &= 0xFFFDB7FFu;
	if (!toggles.waypoints) mask &= 0xFFFDFFFFu;
	return mask & ~0x10000u;
}

void CommandMapView::render(const MapViewRect &rect, int32_t scaled_800, int32_t player_x,
		int32_t player_y, int32_t player_z, HudMinimapInput &base) {
	// [orig: CMapWindow_HandleEvent render @0x549861..0x5498b3 — the stamp]
	view.stamp_window(rect, scaled_800);
	// The params block: mode 4, custom position (player + pan, player Z),
	// custom zoom, custom rect [orig: @0x5498c7..0x549912].
	base.map_mode = 4;
	base.player_x = wrap_add(player_x, view.pan_x);
	base.player_y = wrap_add(player_y, view.pan_y);
	base.player_z = player_z;
	base.flags = command_map_mask(toggles);
	// The payload rect into the 1024x768 virtual space [orig:
	// HUD_BuildMapOverlayView mode 4 @0x5a7f29..0x5a7f68, Viewport_ScreenToVirtual
	// @0x5d2c70 on both corners].
	const int32_t surface_w = static_cast<int32_t>(base.surface_w);
	const int32_t surface_h = static_cast<int32_t>(base.surface_h);
	const int32_t vx1 = screen_to_design_x(rect.left, surface_w);
	const int32_t vy1 = screen_to_design_y(rect.top, surface_h);
	const int32_t vx2 = screen_to_design_x(rect.right, surface_w);
	const int32_t vy2 = screen_to_design_y(rect.bottom, surface_h);
	base.rect_x1 = static_cast<float>(vx1);
	base.rect_y1 = static_cast<float>(vy1);
	base.rect_x2 = static_cast<float>(vx2);
	base.rect_y2 = static_cast<float>(vy2);
	// HUD_DrawMapOverlay's scale: the ctx zoom (Z x 65536) over the scaled
	// rect width x 200 [orig: HUD_BuildMapOverlayView @0x5a803c (params+0x18 x
	// flt_7C32BC); HUD_DrawMapOverlay @0x5a64eb..0x5a650b — fild (x2 - x1),
	// fmul 200.0, fdivr zoom].
	const int32_t x1 = static_cast<int32_t>(scale_axis(vx1, base.surface_w, kDesignWidth));
	const int32_t x2 = static_cast<int32_t>(scale_axis(vx2, base.surface_w, kDesignWidth));
	const double zoom = static_cast<double>(view.zoom) * 65536.0;
	base.window_scale = pc24(zoom / static_cast<double>(pc24(
			static_cast<double>(wrap_sub(x2, x1)) * 200.0)));
	base.window_marker_angle_bias_bam = 0;
}

void DeathMapCompiler::compile(HudMinimapInput &input, const DeathMapFrame &frame,
		const DeathMapFacts &facts, HudMapWindowPass &out) {
	out.over_lines.clear();
	// MapOverlay_DrawView's view: the payload rect, the pan-shifted centre,
	// the caller's scale, north-up 0x40000000 with no g_MapYaw180 term, and
	// the walk angle 0x3FFFFFC0 (64 BAM short of the view).
	// [orig: MapOverlay_DrawView @0x5a593c..0x5a59f3 (the rect floats and the
	//  Render_TerrainDecal(position, rect, scale, 0x40000000, 0, 0xD0606060,
	//  1) call), @0x5a5a02..0x5a5a11 (the walk angle)]
	input.map_mode = 4;
	input.rect_x1 = device_to_virtual(frame.rect.left, input.surface_w, kDesignWidth);
	input.rect_y1 = device_to_virtual(frame.rect.top, input.surface_h, kDesignHeight);
	input.rect_x2 = device_to_virtual(frame.rect.right, input.surface_w, kDesignWidth);
	input.rect_y2 = device_to_virtual(frame.rect.bottom, input.surface_h, kDesignHeight);
	input.player_x = frame.center_x;
	input.player_y = frame.center_y;
	input.flags = kDeathMapMask;
	input.flip_180 = false;
	input.window_scale = frame.scale;
	input.window_marker_angle_bias_bam = static_cast<int32_t>(0x3FFFFFC0u - 0x40000000u);
	compiler_.compile(input, out.map);
	if (!out.map.visible) return;

	// The zone walk: every non-special bank slot (transient then persistent
	// in slot order; the special view is skipped by its +3 & 0x40 flag) whose
	// entity's def carries attrib 0x40000.
	// [orig: MapOverlay_DrawView @0x5a5a4d..0x5a5aad — 1160 slots of 32 bytes
	//  from word_28E5620]
	walk_.clear();
	for (int bank : {static_cast<int>(HudMinimapBank::kTransient),
			static_cast<int>(HudMinimapBank::kPersistent)}) {
		for (const HudMinimapMarker &marker : input.markers) {
			if (marker.bank != bank || (marker.flags & 0x40u) != 0) continue;
			for (const DeathMapZone &zone : facts.zones) {
				if (zone.handle == marker.handle) {
					walk_.push_back({&marker, &zone});
					break;
				}
			}
		}
	}

	// Each zone's blip redraws through sub_597FD0 -> Minimap_DrawBlip at the
	// view angle 0x40000000 less the heading, alpha arg 0 (opaque). The port
	// draws it from the zone's own bank slot through the same walk, appended
	// after the pass (the letters still draw above every blip).
	// [orig: sub_597FD0(entity, rect, 0x40000000 - heading, 0) @0x5a5ab9..0x5a5adc;
	//  Minimap_DrawBlip @0x597890]
	if (!walk_.empty()) {
		zone_input_ = input;
		zone_input_.markers.clear();
		for (const ZoneSlot &slot : walk_) {
			// sub_597FD0 calls Minimap_DrawBlip directly: no persistent-bank
			// model gate and no Render_MinimapSlotBlip zone ring / palette
			// swap, so the redraw copy clears what would route it through
			// those bank-walk legs [orig: MapOverlay_RenderAllByLayer
			// @0x5BE6C4; Render_MinimapSlotBlip @0x5be4b8].
			HudMinimapMarker redraw = *slot.marker;
			redraw.entity_bits = static_cast<uint8_t>(redraw.entity_bits |
					kMarkerEntityHasModel);
			redraw.zone_number = 0;
			zone_input_.markers.push_back(redraw);
		}
		zone_input_.flags = 0x2u;
		zone_input_.window_marker_angle_bias_bam = 0;
		compiler_.compile(zone_input_, zone_pass_);
		out.map.overlays.insert(out.map.overlays.end(), zone_pass_.overlays.begin(),
				zone_pass_.overlays.end());
		out.map.sprites.insert(out.map.sprites.end(), zone_pass_.sprites.begin(),
				zone_pass_.sprites.end());
		out.map.lines.insert(out.map.lines.end(), zone_pass_.lines.begin(),
				zone_pass_.lines.end());
	}

	// The label offsets: 8 x 16 design pixels through the viewport scaler
	// [orig: @0x5a5923..0x5a5937 — Viewport_ScaleToVirtualCoords(&g_OverlayCtx,
	//  8, 16)].
	const int32_t label_w = static_cast<int32_t>(
			scale_axis(8.0, input.surface_w, kDesignWidth));
	const int32_t label_h = static_cast<int32_t>(
			scale_axis(16.0, input.surface_h, kDesignHeight));
	const int32_t frame_counter = input.ticks;
	char text[16];
	for (const ZoneSlot &slot : walk_) {
		const DeathMapZone &zone = *slot.zone;
		uint32_t color = zone_color(zone.team);
		if (facts.self_zone_handle != 0xFFFFu && facts.self_zone_handle == zone.handle)
			color = 0xFFC8C814u;
		// Own-team zones pulse; the others brighten a fixed 20/32 unless the
		// spawn-target hold runs [orig: @0x5a5b30..0x5a5b65].
		int32_t phase = pulse_phase(frame_counter);
		if (zone.team != facts.player_team) phase = facts.hold_seconds != 0 ? 0 : 20;
		// Drawn only on game type 0x50010 or a ready zone timer
		// [orig: CProximityList_FindEntryById(g_ZoneTimerList, e) @0x5a5b6e;
		//  EntryById[9] >= [10] @0x5a5b77..0x5a5b7f; @0x5a5b84..0x5a5b95].
		if (facts.game_type != 0x50010u && !zone.timer_ready) continue;
		color = brighten(color, phase);
		// The letter at the anchor less half the 8 x 16 cell
		// [orig: sprintf("%c", idx + 'A') @0x5a5bfa; sub_59C300 @0x5a5c05;
		//  @0x5a5c0a..0x5a5c59, HUD_DrawTextCentered_HalfBright(g_HUDLabelFontBold)]
		float anchor_x = 0.0f, anchor_y = 0.0f;
		project_spinmap_point(input, zone.anchor_x, zone.anchor_y, false, anchor_x, anchor_y);
		const float lx = pc24(static_cast<double>(anchor_x) - static_cast<double>(label_w >> 1));
		const float ly = pc24(static_cast<double>(anchor_y) - static_cast<double>(label_h >> 1));
		std::snprintf(text, sizeof(text), "%c", static_cast<char>(zone.index + 'A'));
		add_label(out.map, io::retail_ftol_sse2(lx), io::retail_ftol_sse2(ly), text, color);
		if (zone.team != facts.player_team) continue;
		// Own-team zones add the hold seconds above and the queued / countdown
		// pair below [orig: "%ld" dword_A85B68 @0x5a5c79..0x5a5cbf; "%ld/%ld"
		// (entity+550 byte, entity+548 word) @0x5a5cc7..0x5a5d14].
		if (facts.hold_seconds != 0) {
			std::snprintf(text, sizeof(text), "%ld", static_cast<long>(facts.hold_seconds));
			add_label(out.map, io::retail_ftol_sse2(lx),
					io::retail_ftol_sse2(pc24(static_cast<double>(ly) - label_h)), text, color);
		}
		std::snprintf(text, sizeof(text), "%ld/%ld", static_cast<long>(zone.queued),
				static_cast<long>(zone.countdown));
		add_label(out.map, io::retail_ftol_sse2(lx),
				io::retail_ftol_sse2(pc24(static_cast<double>(ly) + label_h)), text, color);
	}

	// The player crosshair [orig: MapOverlay_DrawView @0x5a5d32..0x5a5ed6].
	map_view_crosshair(input, frame.rect, facts, frame_counter, out.over_lines);
}

void map_view_crosshair(const HudMinimapInput &view, const MapViewRect &rect,
		const DeathMapFacts &facts, int32_t frame_counter, std::vector<HudMapLine> &out) {
	// Retail dereferences the local entity; a missing one draws nothing here.
	if (!facts.player_present || facts.deploy_screen_active) return;
	float px = 0.0f, py = 0.0f;
	project_spinmap_point(view, facts.player_x, facts.player_y, false, px, py);
	uint32_t color = facts.player_team == 1 ? 0xFF4040FFu
			: (facts.player_team == 2 ? 0xFFFF4040u : 0xFFA0A0A0u);
	color = (brighten(color, pulse_phase(frame_counter)) & 0x00FFFFFFu) | 0x70000000u;
	const float x1 = static_cast<float>(rect.left);
	const float y1 = static_cast<float>(rect.top);
	const float x2 = static_cast<float>(rect.right);
	const float y2 = static_cast<float>(rect.bottom);
	out.push_back({x1, py, x2, py, color});
	out.push_back({px, y1, px, y2, color});
}

void CommandMapCompiler::compile(HudMinimapInput &input, CommandMapView &cmap,
		const MapViewRect &rect, int32_t scaled_800, const DeathMapFacts &facts,
		HudMapWindowPass &out) {
	out.over_lines.clear();
	cmap.render(rect, scaled_800, facts.player_x, facts.player_y, facts.player_z, input);
	compiler_.compile(input, out.map);
	if (!out.map.visible) return;
	// The clear: colour 0x00000018 written straight into the target (no
	// blend), so the rect reads opaque (0, 0, 0x18) under the map
	// [orig: @0x549826..0x54985c — `lea esi, [ebx+17h]` = 24].
	const float x1 = static_cast<float>(rect.left);
	const float y1 = static_cast<float>(rect.top);
	const float x2 = static_cast<float>(rect.right);
	const float y2 = static_cast<float>(rect.bottom);
	constexpr uint32_t kClear = 0xFF000018u;
	const HudMapTri clear[2] = {
		{{x1, y1, 0.0f, 0.0f}, {x2, y1, 0.0f, 0.0f}, {x2, y2, 0.0f, 0.0f}, kClear},
		{{x1, y1, 0.0f, 0.0f}, {x2, y2, 0.0f, 0.0f}, {x1, y2, 0.0f, 0.0f}, kClear},
	};
	out.map.clear.insert(out.map.clear.begin(), clear, clear + 2);
	// The crosshair [orig: CMapWindow_HandleEvent @0x54999e..0x549b4a].
	map_view_crosshair(input, rect, facts, input.ticks, out.over_lines);
}

} // namespace opennova::hud
