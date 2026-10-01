#pragma once

#include <godot_cpp/classes/canvas_item.hpp>
#include <godot_cpp/classes/font.hpp>
#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_color_array.hpp>
#include <godot_cpp/variant/packed_float32_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/def/def.h>

namespace godot {

class ResourceRoot;
class RtxtStringFile;
class VehicleHudBlock;
class WaypointHudView;

// Thin GDExtension wrapper over engine/formats/def hudpos.def parsing (def_parse_hudpos).
//
// Read-only: it holds the retained parse the HUD overlay applies through the
// engine's hud_layout_from_hudpos, hands out one VEHICLE_HUD block by sid, and
// carries the HUD view-helper statics. There is no writer; hudpos.def authoring
// is out of scope.
//
// Layout shape and field meanings are the witnessed originals; see
// docs/interface/hud-re.md. [orig: HUD_ParseHudposToken @0x59f370 hudpos.def parser, registered by
// HUD_InitOverlaySystem @0x5a4620, see docs/interface/hud-re.md]
class HudPos : public RefCounted {
	GDCLASS(HudPos, RefCounted)

private:
	opennova::def::DefHudPosFile file_ = {};
	bool loaded_ = false;
	String source_path_;
	String last_error_;

	void clear_();
	Error parse_bytes_(const PackedByteArray &bytes, const String &src);

protected:
	static void _bind_methods();

public:
	HudPos();
	~HudPos();

	// The virtual design space hudpos.def positions are authored in; the HUD
	// scales these to the real surface. [orig: Viewport_ScaleToVirtualCoords @0x5d2b20, see docs/interface/hud-re.md]
	enum {
		DESIGN_WIDTH = 1024,
		DESIGN_HEIGHT = 768,
		// The witnessed message-feed policy constants (hud/hud_math.h is the
		// single source; the cpp static_asserts pin these mirrors to it).
		MESSAGE_LIFE_TICKS = 930,
		MESSAGE_EXPIRY_STAGGER = 186,
		MESSAGE_TEXT_MAX = 119,
		MESSAGE_SLOT_COUNT = 40,
		// The loading screen's witnessed layout (hud/loading_screen.h is the
		// single source; the cpp static_asserts pin these mirrors to it). The
		// band values are image-space; the bar rides the loading_bar_* statics.
		LOADING_BAND_LEFT = 21,
		LOADING_BAND_TOP = 29,
		LOADING_BAND_RIGHT_CUSTOM = 661,
		LOADING_BAND_RIGHT_STOCK = 782,
		LOADING_BAND_BOTTOM = 500,
		LOADING_PRESENT_INTERVAL_MS = 100,
		// The SP start-mission splash (hud/loading_screen.h kSplash*; same
		// pinning): the centered continue-line virtual position, the 512 ms
		// blink phase bit, and the 800x600/800 scale bases.
		SPLASH_CONTINUE_X = 512,
		SPLASH_CONTINUE_Y = 730,
		SPLASH_BLINK_MASK_MS = 0x200,
		SPLASH_ARROW_SCALE_BASE_W = 800,
		SPLASH_ARROW_SCALE_BASE_H = 600,
		SPLASH_FONT_SCALE_BASE_W = 800,
		// The first-person view effects (hud/view_effects.h; same pinning):
		// the rangefinder digit advance and the 16px digit-strip cell.
		BINOCULAR_DIGIT_STEP = 10,
		VIEW_DIGIT_CELL = 16,
	};
	// kPercentToAlpha (2.55 — authored percent -> 0..255 alpha), bound as a
	// method because class constants are integer-only.
	static double percent_to_alpha();

	// The HUD view-helper math (one impl in engine/runtime/hud hud_math.h;
	// the GDScript Hud* helpers delegate here and keep only the CanvasItem
	// work). Each carries its witness at the engine impl.
	static Vector2 scale_point(const Vector2 &p_design, const Vector2 &p_surface);
	// Both corners through scale_point (the original scales x1,y1 and x2,y2
	// independently and differences them for the size).
	static Rect2 scale_rect(const Rect2 &p_design, const Vector2 &p_surface);
	static Rect2 sight_scale_rect(const Rect2 &p_design, const Vector2 &p_surface);
	// A SIGHTS row laid out over the 512-square NVG scene for the NVG Sighted
	// arm: the frame's selected ratio (the aspect mode over `surface`) drives
	// the Y correction (hud/sight_overlay.h sight_rect_to_viewport_at_ratio).
	static Rect2 nvg_scene_sight_rect(const Rect2 &p_design, const Vector2 &p_surface,
			int p_aspect_mode);
	static Vector2 pixel_delta_to_design(const Vector2 &p_delta, const Vector2 &p_surface);
	static int fade_decay(int p_elapsed_ticks, int p_ramp_ticks);
	static int fade_flash_alpha(int p_elapsed_ticks, int p_ramp_ticks,
			int p_base_alpha, int p_max_alpha);
	static int stance_current_alpha(int p_elapsed_ticks, int p_ramp_ticks,
			int p_base_alpha);
	static int stance_prev_alpha(int p_elapsed_ticks, int p_ramp_ticks);
	static int stance_scale_q16(const Vector2i &p_frame0_size);
	static int stance_scaled_dim(int p_dim, int p_q16);
	static Vector2i stance_center_offset(const Vector2i &p_frame0_size, int p_q16);
	static int health_color_band(float p_fraction);
	static int message_expire_tick(int p_now_ticks, int p_prev_expire, bool p_has_prev);
	static String format_ammo(int p_clip, int p_reserve, int p_capacity);
	static int weapon_name_x_nudge(bool p_narrow_surface, int p_align);
	static int round_icon_count(int p_clip, int p_reserve, int p_capacity, int p_divisor);
	static int folded_reserve(int p_clip, int p_reserve, int p_capacity);
	static int waypoint_distance_m(const Vector2 &p_ground_delta);
	static int heat_fill_span(int p_extent_px, int p_heat);
	static bool heat_bar_is_horizontal(const Vector2 &p_bar_size);
	static int power_fill_span(int p_progress_fp16, int p_extent_px);
	static Vector2i loading_bar_fill_span(int p_x, int p_w, int p_displayed);
	static double crosshair_spread_px_fp16(int p_spread_fp16, double p_fov_deg,
			double p_screen_w);
	static int crosshair_total_spread_fp16(int p_error_fp16,
			int p_recoil_pitch_bam, int p_weight_spread_bam);
	static int crosshair_error_row(int p_stance, bool p_scoped);
	static bool crosshair_should_draw(bool p_aimed, bool p_keep_while_aimed);

	// The mission loading screen's engine spec (hud/loading_screen.h carries
	// the values and witnesses): the GAMETYPE -> LoadingText key policy ("" =
	// unknown) plus the non-integer layout values the LoadingScreen shell
	// draws with.
	static String loading_gametype_text_key(int p_game_type);
	// The HUD's game-text compositions (hud/hud_game_text.h carries the
	// witnesses) over the mission / gametext tables: the waypoint label (the
	// current marker's resolved name composed with "m to", the CTF runs and
	// the LFP override; the session facts pick the id remap and the CTF arm),
	// a resolved subgoal's WinConditions / LoseConditions announcement ("" =
	// nothing posts), the mission's "Triggered Text" line ("" on a miss) and
	// the WepDes weapon name.
	static String waypoint_label(const Ref<RtxtStringFile> &p_mission,
			const Ref<RtxtStringFile> &p_gametext, const Ref<WaypointHudView> &p_view,
			bool p_in_session, int64_t p_game_type);
	static String subgoal_message(const Ref<RtxtStringFile> &p_mission, bool p_lost,
			int p_header_id);
	// A shown objective's two chat lines (hud_game_text.h objective_header /
	// objective_directive): the gametext header and the mission directive
	// ("" = nothing posts).
	static String objective_header(const Ref<RtxtStringFile> &p_gametext);
	static String objective_directive(const Ref<RtxtStringFile> &p_mission, bool p_win,
			int p_header_id);
	static String triggered_text(const Ref<RtxtStringFile> &p_mission, int p_text_id);
	static String weapon_display_name(const Ref<RtxtStringFile> &p_gametext,
			const String &p_weapon_id);
	// The loading screen's wrapped text block painted into a CanvasItem: the
	// engine breaks and places the lines (hud/loading_screen.h) against this
	// font's measure, this leg only draws them. `align` is a HorizontalAlignment
	// (left / center / right); returns the block's stopped_at.
	static int draw_wrapped_text(CanvasItem *p_item, const Ref<Font> &p_font, int p_font_size,
			const String &p_text, int p_x, int p_y, int p_width, int p_bottom, int p_align,
			const Color &p_color, int p_skip_lines);
	static String loading_fallback_image();
	static String loading_font_small();
	static String loading_font_large();
	static String loading_msg_label_key();
	static String loading_msg_label_fallback();
	static String loading_sidecar_image_name(const String &p_mission_file);
	static bool loading_present_due(int p_elapsed_ms, bool p_reported_changed);
	static double loading_msg_x_frac();
	static double loading_msg_right_frac();
	static double loading_msg_label_y_frac();
	static double loading_msg_body_y_frac();
	static Color loading_msg_label_color();
	static Vector2i loading_bar_pos();
	static Vector2i loading_bar_size();
	static Color loading_bar_border_gray();
	static Color loading_bar_fill_color();
	// The SP start-mission splash strings and the phase-selected continue-line
	// color, already through the witnessed half-bright fold.
	static String loading_splash_arrow_image();
	static String loading_splash_sound_set();
	static String loading_splash_continue_key();
	static String loading_splash_continue_font();
	static Color loading_splash_continue_color(bool p_phase_on);

	// The first-person view-effect spec (hud/view_effects.h carries the
	// values and witnesses): binocular/NVG overlay rects in the 1024x768
	// design space, the NVG scale modulate, and the rangefinder easing step.
	static Rect2 binocular_crosshair_rect();
	static Vector2 binocular_digit_pos();
	static Rect2 nvg_scale_rect();
	static Color nvg_scale_modulate();
	static int binocular_range_step(int p_current, int p_target);

	// The scoped-view circle mask (runtime/hud/scope_circle_mask.h carries the
	// geometry and the witnesses). One batch per call, in VIEWPORT PIXELS and
	// ready for RenderingServer.canvas_item_add_triangle_array: `batch` is
	// SCOPE_MASK_RING / _CROSS / _GRID, and the cross and grid come back empty
	// unless `draw_crosshair` -- retail's only argument to the drawer, meaning
	// the SIGHTS card drew no authored row.
	enum {
		SCOPE_MASK_RING = 0,
		SCOPE_MASK_CROSS = 1,
		SCOPE_MASK_GRID = 2,
	};
	// The surface is retail's inclusive overlay rect (0, 0)..(W - 1, H - 1).
	// `nvg_lens` builds the NVG lens's reticle instead
	// (hud/scope_circle_mask.h build_nvg_lens_reticle over the overlay rect
	// (0, 0)..(W - 1, H - 1)): no ring, the cross and grid at unit scale.
	static PackedVector2Array scope_mask_points(const Vector2 &p_surface,
			int p_screen_width, bool p_draw_crosshair, int p_batch, int p_aspect_mode,
			bool p_nvg_lens);
	static PackedColorArray scope_mask_colors(const Vector2 &p_surface,
			int p_screen_width, bool p_draw_crosshair, int p_batch, int p_aspect_mode,
			bool p_nvg_lens);
	static PackedInt32Array scope_mask_indices(const Vector2 &p_surface,
			int p_screen_width, bool p_draw_crosshair, int p_batch, int p_aspect_mode,
			bool p_nvg_lens);
	// The derived frame, in order: center x, center y, ring size, inner radius,
	// outer radius, scale x, scale y, arm half thickness, tick pitch.
	static PackedFloat32Array scope_mask_frame(const Vector2 &p_surface,
			int p_screen_width, int p_aspect_mode);
	// The scene frame's overlay fork (0 markers, 1 binocular mask, 2 sighted
	// card, 3 scoped card + circle mask) and the two selector bytes' def halves.
	static int scoped_view_overlay(bool p_binoculars_view_active, bool p_sighted,
			bool p_scoped);
	static bool scoped_selector_from_def(int p_weapon_flags, int p_weapon_flags2);
	static bool sighted_selector_from_def(int p_weapon_flags, bool p_slot_switching_from);

	Error load(const String &path);
	// Load hudpos.def by flat name through the mounted resource root (VFS), so the
	// layout resolves from PFF archives at runtime. Mirrors ItemDatabase.
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	// One VEHICLE_HUD block by items.def sid (hud/vehicle_hud_block.h); null when unknown.
	Ref<VehicleHudBlock> get_vehicle_hud(const String &p_sid) const;
	// The retained parse, for the engine folds that take the rows directly
	// (hud_layout_from_hudpos.h, hud_declutter.h declutter_from_hudpos).
	const opennova::def::DefHudPosFile &native_file() const { return file_; }
};

} // namespace godot
