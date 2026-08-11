#pragma once

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <fnt/fnt.h>
#include <hud/hud_frame.h>

#include <array>

namespace godot {

class HudPos;
class ResourceRoot;

// The runtime in-game HUD overlay — the ADR 0033 device leg over the engine's
// HudFrameCompiler (engine/runtime/hud). The engine owns the whole witnessed
// element walk, the fade/flash/message state, and every policy constant
// (witness record: docs/interface/hud-re.md); this Control keeps only what a
// device leg may keep: texture upload (VFS .tga/.fnt bytes -> ImageTexture),
// the typed per-frame state marshalling the presenter feeds, and rasterizing
// the compiled HudDrawList with CanvasItem draw calls.
//
// The standard-weapon SIGHTS card and the PlayerViewEffects post stack stay
// shell-side child controls (see godot/src/world/hud_sights_card.gd): their
// per-row blend modes need separate CanvasItems, so the compiler's sights
// element is left unfed here (layout.sights stays empty).
class HudOverlay : public Control {
	GDCLASS(HudOverlay, Control)

public:
	// The user crosshair-style range; retail loads "cross%02d.tga" (style + 1)
	// from the player config.
	enum {
		MIN_CROSSHAIR_STYLE = 0,
		MAX_CROSSHAIR_STYLE = 24,
	};

	HudOverlay();
	~HudOverlay();

	// Build the engine HudLayout from a parsed hudpos.def and load the layout
	// art + .fnt through the mounted VFS root (null root -> layout only: bars
	// and boxes draw, textured/text elements self-hide). Resets the compiler's
	// runtime state (a fresh HUD build).
	void configure(const Ref<HudPos> &p_hudpos, const Ref<ResourceRoot> &p_root);
	bool is_configured() const;

	// Select and (when configured) immediately reload the crosshair art;
	// runtime state (messages, fades) survives the layout refresh.
	void set_crosshair_style(int p_style);
	int get_crosshair_style() const;

	// Install the equipped weapon's HUD slice (PlayerHudWeaponDef's fields,
	// passed typed) plus its resolved display name; loads the per-weapon
	// HUDCLIPGFX/HUDRNDGFX art.
	void set_weapon(const String &p_weapon_name, const String &p_display_name,
			const String &p_round_type, int p_clipsize, int p_rounds_per_icon,
			const String &p_clipgfx_texture, const Vector2i &p_clipgfx_offset,
			const String &p_rndgfx_texture, const Vector2i &p_rndgfx_offset,
			const Vector2i &p_rndgfx_step);
	void clear_weapon();

	// A mission triggered-text line for the message feed, stamped at the last
	// set ticks (the engine ring owns life/stagger policy).
	void push_message(const String &p_text);

	// Typed per-frame state (the presenter rebuilds these each tick; the
	// compiler reads them at the next draw).
	void set_player_state(int p_ticks, float p_health_fraction, int p_stance,
			float p_fov_deg);
	void set_weapon_state(bool p_active, int p_clip, int p_reserve, int p_heat,
			int p_hud_spread_fp16, bool p_aimed_shot_available,
			bool p_keep_crosshair_while_aimed, bool p_windup_active,
			int p_windup_held_ticks);
	// aim_screen is the projected aim point in screen pixels; a non-finite
	// vector (Vector2.INF) is the first-person pin to the design center.
	void set_view_state(bool p_binoculars_view_active, const Vector2 &p_aim_screen);
	void set_objective_line(const String &p_text);
	// The resolved gametext Overlays/STROVER_MISSIONOBJECTIVES header line.
	void set_objectives_header(const String &p_text);
	void set_waypoint(const String &p_name, int p_distance_m);
	void clear_waypoint();
	void set_objectives(const PackedStringArray &p_texts,
			const PackedByteArray &p_done);
	void set_attach_labels(const PackedVector2Array &p_screens,
			const PackedStringArray &p_texts, const PackedByteArray &p_nearest);
	// The projected friendly tags (D-HUD-20): parallel typed arrays; flags
	// bit 0 = medic marker, bit 1 = speaking, bit 2 = player-slot entry.
	void set_friendly_tags(const PackedVector2Array &p_screens,
			const PackedInt32Array &p_dists_q16, const PackedStringArray &p_names,
			const PackedInt32Array &p_entity_ids,
			const PackedInt32Array &p_health_ratios_fp16,
			const PackedInt32Array &p_flags);
	// Mode 0 off / 1 text < 300 m / 2 text always (default) / 3 tick marks
	// (retail g_friendlyTagsMode; the witnessed rules live in hud_math).
	void set_friendly_tag_mode(int p_mode);
	int get_friendly_tag_mode() const;
	// Per-frame environment feed: the fog cull distance (16.16; <= 0 disables)
	// and the speaking entity's voice level 0..255.
	void set_friendly_tag_env(int p_fog_dist_q16, int p_speaking_level255);

	// Debug/test accessor: compile at the current surface size and report the
	// draw list's element counts.
	Dictionary get_draw_list_stats();

	void _draw() override;

protected:
	static void _bind_methods();
	void _notification(int p_what);

private:
	// The fixed embedder texture slots (frame, crosshair, clip, round, the six
	// stance frames); authored SIGHTS rows stay shell-side (see above).
	static constexpr int kTextureSlots = opennova::hud::kHudTexSightsBase;

	opennova::hud::HudFrameCompiler compiler_;
	opennova::hud::HudLayout layout_;
	opennova::hud::HudFrameState state_;
	Ref<ResourceRoot> root_;
	std::array<Ref<Texture2D>, kTextureSlots> textures_;
	std::array<Ref<Texture2D>, FNT_MAX_PAGES> page_textures_;
	fnt_font_t font_ = {};
	bool font_valid_ = false;
	bool configured_ = false;
	int crosshair_style_ = MIN_CROSSHAIR_STYLE;
	// Additive rows cannot share this item's blend mode: they render through a
	// child RenderingServer canvas item carrying a BLEND_MODE_ADD material.
	Ref<CanvasItemMaterial> additive_material_;
	RID additive_item_;

	Vector2 draw_surface_() const;
	Ref<Texture2D> load_hud_texture_(const String &p_name) const;
	void load_crosshair_texture_();
	void clear_font_();
	void ensure_additive_item_();
	void render_list_(const opennova::hud::HudDrawList &p_list);
};

} // namespace godot
