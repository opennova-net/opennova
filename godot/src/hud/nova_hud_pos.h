#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3.hpp>
#include <godot_cpp/variant/vector3i.hpp>
#include <godot_cpp/variant/vector4i.hpp>

#include <def/def.h>

namespace godot {

class ResourceRoot;

// Thin GDExtension wrapper over engine/formats/def hudpos.def parsing (def_parse_hudpos).
//
// Read-only: it surfaces the original HUD layout — element positions in the
// 1024x768 virtual design space, colors, stance/graphic frames and the HUD fonts
// — for the ONED HUD preview workspace and the runtime HUD overlay. There is no
// writer; hudpos.def authoring is out of scope (preview only).
//
// Layout shape and field meanings are the witnessed originals; see
// docs/interface/hud-re.md. [orig: loc_59F370 hudpos.def parser, registered by
// HUD_InitOverlaySystem @0x5a4620]
class HudPos : public RefCounted {
	GDCLASS(HudPos, RefCounted)

private:
	DefHudPosFile file_ = {};
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
	// scales these to the real surface. [orig: Viewport_ScaleToVirtualCoords @0x5d2b20]
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
	static Color half_bright(const Color &p_color);
	static String format_ammo(int p_clip, int p_reserve, int p_capacity);
	static int weapon_name_x_nudge(bool p_narrow_surface, int p_align);
	static int round_icon_count(int p_clip, int p_reserve, int p_capacity, int p_divisor);
	static int folded_reserve(int p_clip, int p_reserve, int p_capacity);
	static int waypoint_distance_m(const Vector2 &p_ground_delta);
	static int heat_fill_span(int p_extent_px, int p_heat);
	static bool heat_bar_is_horizontal(const Vector2 &p_bar_size);
	static int power_throw_progress_fp16(int p_held_ticks);
	static int power_fill_span(int p_progress_fp16, int p_extent_px);
	static int loading_bar_step(int p_displayed, int p_reported);
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

	Error load(const String &path);
	// Load hudpos.def by flat name through the mounted resource root (VFS), so the
	// layout resolves from PFF archives at runtime. Mirrors ItemDatabase.
	Error load_from_resource_root(const Ref<ResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	// Headline accessors used by the runtime hot path and the common preview.
	String get_font_hi() const;
	String get_font_lo() const;
	Rect2i get_health_rect() const;
	Rect2i get_heat_rect() const;
	// HUDPOWERBAR is authored x,y,w,h (not corners) — see rect_from_xywh below.
	Rect2i get_powerbar_rect() const;
	Vector2i get_stance_pos() const;
	Vector2i get_veh_stance_pos() const;
	// The 4-field positioned-text records (x, y, hidden, align) — the parse
	// and hidden-gate witnesses live at the pos4 helper in the .cpp.
	Vector4i get_ammo_count_pos() const;
	Vector4i get_weapon_name_pos() const;
	Vector4i get_game_info_pos() const;
	Vector4i get_wpd_info_pos() const;
	Vector2i get_chat_text_pos() const;
	Vector2i get_sys_text_pos() const;   // HUDSYSTEXT — the SYSTEM feed anchor
	Vector2i get_clip_pos() const;
	// ALPHAFADE raw file fields (base %, max %, seconds); the parse witness is
	// on the to_dictionary misc block.
	Vector3 get_alpha_fade() const;
	int get_hud_chline() const;
	// [{ id:int, offset:Vector2i, texture:String, name:String }] — the HUDSTANCE frames.
	Array get_stances() const;
	// [{ texture:String, pos:Vector2i }]
	Array get_static_frames() const;
	// One VEHICLE_HUD block by items.def sid; empty when unknown.
	Dictionary get_vehicle_hud(const String &p_sid) const;
	Dictionary get_parachute_icon() const;
	Dictionary get_armor_icon() const;
	Rect2i get_spinmap_bounds() const;
	int get_spinmap_wp_dist_off() const;
	// MAPCOORDS x, y, suppressor. The suppressor is 0 when unauthored (the
	// retail global is BSS-zero) = the grid label draws; an authored NONZERO
	// third token suppresses it.
	Vector3i get_map_coords() const;
	// Four visibility bytes for one HUDDECLUT_* row. Empty means absent.
	PackedByteArray get_declutter_flags(const String &p_name) const;
	// Named HUD colors (Godot Color, RGBA normalized): health_border, hud_textcolor,
	// stancecolor_good/middle/bad, tagcolor_*, etc.
	Dictionary get_colors() const;

	// The complete parsed layout as a nested Godot-native Dictionary (the ONED
	// preview reads this). Keys mirror DefHudPosDef field names.
	Dictionary to_dictionary() const;
};

} // namespace godot
