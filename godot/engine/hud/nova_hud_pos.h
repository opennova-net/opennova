#pragma once

#include <godot_cpp/classes/ref_counted.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/array.hpp>
#include <godot_cpp/variant/color.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/rect2i.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2i.hpp>
#include <godot_cpp/variant/vector3i.hpp>

#include <def/def.h>

namespace godot {

class NovaResourceRoot;

// Thin GDExtension wrapper over libs/def hudpos.def parsing (def_parse_hudpos).
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
	};

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
