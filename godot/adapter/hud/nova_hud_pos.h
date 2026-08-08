#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <def/def.h>

namespace godot {

class NovaResourceRoot;

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
class NovaHudPos : public RefCounted {
	GDCLASS(NovaHudPos, RefCounted)

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
	NovaHudPos();
	~NovaHudPos();

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
	};
	// kPercentToAlpha (2.55 — authored percent -> 0..255 alpha), bound as a
	// method because class constants are integer-only.
	static double percent_to_alpha();

	// The HUD view-helper math (one impl in engine/runtime/hud hud_math.h;
	// the GDScript Hud* helpers delegate here and keep only the CanvasItem
	// work). Each carries its witness at the engine impl.
	static Vector2 scale_point(const Vector2 &p_design, const Vector2 &p_surface);
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
	static double crosshair_spread_px_fp16(int p_spread_fp16, double p_fov_deg,
			double p_screen_w);
	static int crosshair_total_spread_fp16(int p_error_fp16,
			int p_recoil_pitch_bam, int p_weight_spread_bam);
	static int crosshair_error_row(int p_stance, bool p_scoped);
	static bool crosshair_should_draw(bool p_aimed, bool p_keep_while_aimed);

	Error load(const String &path);
	// Load hudpos.def by flat name through the mounted resource root (VFS), so the
	// layout resolves from PFF archives at runtime. Mirrors NovaItemDatabase.
	Error load_from_resource_root(const Ref<NovaResourceRoot> &p_resource_root, const String &p_name);
	bool is_loaded() const;
	String get_source_path() const;
	String get_last_error() const;

	// Headline accessors used by the runtime hot path and the common preview.
	String get_font_hi() const;
	String get_font_lo() const;
	Rect2i get_health_rect() const;
	Vector2i get_stance_pos() const;
	Vector2i get_veh_stance_pos() const;
	// [{ id:int, offset:Vector2i, texture:String, name:String }] — the HUDSTANCE frames.
	Array get_stances() const;
	// [{ texture:String, pos:Vector2i }]
	Array get_static_frames() const;
	Dictionary get_parachute_icon() const;
	Dictionary get_armor_icon() const;
	Rect2i get_spinmap_bounds() const;
	// Named HUD colors (Godot Color, RGBA normalized): health_border, hud_textcolor,
	// stancecolor_good/middle/bad, tagcolor_*, etc.
	Dictionary get_colors() const;

	// The complete parsed layout as a nested Godot-native Dictionary (the ONED
	// preview reads this). Keys mirror DefHudPosDef field names.
	Dictionary to_dictionary() const;
};

} // namespace godot
