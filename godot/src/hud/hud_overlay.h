#pragma once

#include <godot_cpp/classes/canvas_item_material.hpp>
#include <godot_cpp/classes/control.hpp>
#include <godot_cpp/classes/shader.hpp>
#include <godot_cpp/classes/shader_material.hpp>
#include <godot_cpp/classes/texture2d.hpp>
#include <godot_cpp/core/class_db.hpp>
#include <godot_cpp/variant/dictionary.hpp>
#include <godot_cpp/variant/packed_byte_array.hpp>
#include <godot_cpp/variant/packed_string_array.hpp>
#include <godot_cpp/variant/packed_vector2_array.hpp>
#include <godot_cpp/variant/packed_int32_array.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/fnt/fnt.h>
#include <runtime/hud/hud_config_tokens.h>
#include <runtime/hud/hud_frame.h>

#include <array>

namespace godot {

class Simulation;

class HudPos;
class ResourceRoot;
class TerrainData;

// The runtime in-game HUD overlay — the ADR 0033 device leg over the engine's
// HudFrameCompiler (engine/runtime/hud). The engine owns the whole witnessed
// element walk, the fade/flash/message state, and every policy constant
// (witness record: docs/interface/hud-re.md); this Control keeps only what a
// device leg may keep: texture upload (VFS .tga/.fnt bytes -> ImageTexture),
// the typed per-frame state marshalling the presenter feeds, and rasterizing
// the compiled HudDrawList with CanvasItem draw calls.
//
// The standard-weapon SIGHTS card and the PlayerViewEffects post stack stay
// shell-side child controls (see godot/game/world/hud_sights_card.gd): their
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
	void push_feed_line(const String &p_text, int64_t p_argb);

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
	// The resolved gametext Overlays/STROVER_MISSIONOBJECTIVES header line.
	void set_objectives_header(const String &p_text);
	// The Tab board: whether it is held open, the strings the shell resolved,
	// and the Simulation the rows are pulled from natively
	// (Simulation::fill_scoreboard_rows — no script-side row round-trip).
	// Typed cross-class args on the bound API follow the set_minimap_terrain
	// precedent; pass null when hiding.
	void set_scoreboard(bool p_shown, int64_t p_game_type, int p_frame_counter,
			const Dictionary &p_strings, Simulation *p_sim);
	// The end-of-round overlay (net-re §5.68): the resolved Impact38 text
	// ladder (hud/end_round_overlay.h lines the presenter formatted) and the
	// overlay safe-area top/bottom in design px.
	void set_end_round_overlay(bool p_shown, int p_top, int p_bottom,
			const PackedStringArray &p_texts, const PackedInt32Array &p_ys);
	// The mounted-vehicle panel: the rider's VEHICLE_HUD block (HudPos::get_vehicle_hud),
	// the rider's stance (the panel rides the stance icon's HUDSTANCE offset), and
	// the Simulation the hull band + seat rows are pulled from natively
	// (Simulation::fill_vehicle_panel). The block's `interface` silhouette is
	// loaded per sid (the set_weapon reload idiom). Pass null when hiding.
	void set_vehicle_panel(bool p_shown, const Dictionary &p_block, int p_stance,
			Simulation *p_sim);
	// One player-chat line for the CHAT ring (S2C 0x14 routed to the chat
	// sink by Simulation::drain_chat_lines); the engine ring word-wraps it.
	void push_chat_line(const String &p_text, int64_t p_argb);
	// The Recent Messages (J) window: the OldMessages toggle and its stdbox
	// title (gametext Overlays/STROVER43, resolved by the shell).
	// The SP Show Score statistics panel (hud/end_round_statistics.h).
	void set_end_round_statistics(bool p_shown, bool p_raised,
			const String &p_title, const PackedStringArray &p_labels,
			const PackedStringArray &p_values);
	void set_message_log_shown(bool p_shown);
	void set_message_log_title(const String &p_title);
	// The AAS zone status panel: shown, the session game type (the conquest
	// arm is unmodelled and draws nothing), the viewer's team, the HUD frame
	// counter the blink masks, the two status strings the shell resolved
	// ({under_attack, ready}), and the Simulation the zone rows are pulled
	// from natively (Simulation::fill_lfp_zones). Pass null when hiding.
	void set_lfp_panel(bool p_shown, int64_t p_game_type, int p_local_team,
			int p_frame_counter, const Dictionary &p_strings, Simulation *p_sim);
	void set_waypoint(const String &p_name, int p_distance_m,
			const Vector2 &p_mission_position = Vector2(),
			float p_altitude_wu = 0.0f);
	void clear_waypoint();
	void set_objectives(const PackedStringArray &p_texts,
			const PackedByteArray &p_done);
	void set_attach_labels(const PackedVector2Array &p_screens,
			const PackedStringArray &p_texts, const PackedByteArray &p_nearest);
	// The projected friendly tags (D-HUD-20): parallel typed arrays; the
	// flags word's layout is hud/friendly_tag_flags.h (medic, speaking,
	// player-slot entry, dead, has a connection slot, medic request standing,
	// bits 8..15 = the slot's revive countdown in seconds — retail PlayerSlot
	// +0x10 / +0x2C, the downed legs of the drawer), packed by the Simulation's
	// get_friendly_tags feed.
	void set_friendly_tags(const PackedVector2Array &p_screens,
			const PackedFloat32Array &p_dists_units, const PackedStringArray &p_names,
			const PackedInt32Array &p_entity_ids,
			const PackedInt32Array &p_health_ratios_fp16,
			const PackedInt32Array &p_flags);
	// Mode 0 off / 1 text < 300 m / 2 text always (default) / 3 tick marks
	// (retail g_friendlyTagsMode; the witnessed rules live in hud_math).
	void set_friendly_tag_mode(int p_mode);
	int get_friendly_tag_mode() const;
	void set_hud_color_index(int p_index);
	int get_hud_color_index() const;
	// The HUD declutter level 0..3 [orig: the persisted cfg int
	// "hud_detail" @0x24D20BC driving CRenderState_SetLayerVisibility
	// @0x59B0F0, see docs/interface/hud-re.md]. The overlay owns the
	// HUDDECLUT mask table (fed from the parsed hudpos in configure) and
	// rebuilds the compiler's per-slot visibility here.
	void set_hud_detail_level(int p_level);
	int get_hud_detail_level() const;
	// The showhud 2-bit FP-view flags [orig: g_FpWeaponViewFlags cycle
	// @0x4E0561]: bit 0 = the FP gun (consumed by the viewmodel rig, not
	// here), bit 1 = the corner spinmap block.
	void set_showhud_flags(int p_flags);
	// Per-frame environment feed: the fog cull distance in world units (<= 0
	// disables; the 16.16 form is this seam's) and the speaking entity's voice
	// level 0..255.
	void set_friendly_tag_env(float p_fog_distance_units, int p_speaking_level255);
	// Device-facing minimap feeds. TerrainData is sampled once into the
	// portable sector layout; snapshot is Simulation's versioned fixed-stride
	// retained overlay buffer.
	void set_minimap_terrain(const Ref<TerrainData> &p_terrain,
			const Ref<Texture2D> &p_water_mask = Ref<Texture2D>());
	void set_minimap_state(const Vector2 &p_mission_position,
			float p_altitude_wu, int64_t p_heading_bam, int p_zoom_q16,
			int p_big_zoom_q16, int p_map_mode, bool p_flip_180,
			const PackedInt32Array &p_snapshot);
	// The mission's type-2043 marker entity anchors the grid labels
	// (witness at HudMinimapInput::grid_origin_x).
	void set_minimap_grid_origin(const Vector2 &p_mission_position,
			bool p_present);
	// Static building/zone footprint polygons (the sim feed, baked once per
	// mission; witness at world::minimap_footprint_from_occlusion).
	void set_minimap_footprints(const PackedInt32Array &p_feed);

	// Debug/test accessor: compile at the current surface size and report the
	// draw list's element counts.
	Dictionary get_draw_list_stats();

	// F3 Stats seam: _draw() runs inside Godot's deferred flush (outside every
	// Node callback), so its compile + canvas-emit cost is timed here and
	// consumed by the HUD presenter's next tick. Clock reads only while enabled.
	void set_draw_timing_enabled(bool p_enabled);
	// [compile_us, emit_us] accumulated since the last consume; zeroes after.
	PackedInt64Array consume_draw_timing_us();

	void _draw() override;

protected:
public:
	// The HUD's persisted config tokens and session flags — defaults, clamps
	// and cycle rules — and the friendly-tag anchor lift, re-exported from
	// engine/runtime/hud/hud_config_tokens.h for the presenter's settings
	// round trip (the cfg store is the presenter's device work).
	enum ShowHudFlag {
		SHOWHUD_FLAG_GUN = opennova::hud::kShowHudFlagGun,
		SHOWHUD_FLAG_SPINMAP = opennova::hud::kShowHudFlagSpinmap,
	};
	static int hud_color_index_default();
	static int clamp_hud_color_index(int p_index);
	static int next_hud_color_index(int p_index);
	static int hud_detail_level_default();
	static int hud_detail_level_blank();
	static int clamp_hud_detail_level(int p_level);
	static int next_hud_detail_level(int p_level);
	static int showhud_flags_default();
	static int next_showhud_flags(int p_flags);
	static int friendly_tag_mode_default();
	static float friendly_tag_lift();

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
	// The HUDDECLUT mask table + level (engine hud_declutter carries the
	// witness map); apply_declutter_() restamps the compiler input's
	// visibility table after any mask or level change.
	opennova::hud::HudDeclutter declutter_;
	Ref<ResourceRoot> root_;
	std::array<Ref<Texture2D>, kTextureSlots> textures_;
	// Font glyph pages, one namespace per compiler font slot
	// (opennova::hud::kHudFontSlot*): slot * FNT_MAX_PAGES + page.
	std::array<Ref<Texture2D>, opennova::hud::kHudFontSlotCount * FNT_MAX_PAGES>
			page_textures_;
	fnt_font_t font_ = {};
	bool font_valid_ = false;
	// The Arial overlay label pair (engine hud_label_font_choice picks the
	// faces/scale), loaded lazily per surface-width tier — retail re-inits
	// its overlay fonts on resolution change.
	fnt_font_t label_font_ = {};
	bool label_font_valid_ = false;
	fnt_font_t label_font_bold_ = {};
	bool label_font_bold_valid_ = false;
	fnt_font_t label_font_large_ = {};
	bool label_font_large_valid_ = false;
	fnt_font_t label_font_impact38_ = {}; // Impac38b (the end-round overlay)
	bool label_font_impact38_valid_ = false;
	int label_tier_ = -1; // -1 = not loaded; 0 <=640 / 1 <=800 / 2 >800
	bool configured_ = false;
	int crosshair_style_ = MIN_CROSSHAIR_STYLE;
	bool draw_timing_enabled_ = false;
	int64_t draw_compile_us_ = 0;
	int64_t draw_emit_us_ = 0;
	// The HUDVEHSTANCEPOS anchor (the vehicle panel's base before the stance
	// offset) and the sid whose silhouette currently occupies the
	// kHudTexVehiclePanel slot (reloaded on change).
	Vector2i veh_stance_pos_;
	String vehicle_panel_sid_;
	// Additive rows cannot share this item's blend mode: they render through a
	// child RenderingServer canvas item carrying a BLEND_MODE_ADD material.
	Ref<CanvasItemMaterial> additive_material_;
	RID additive_item_;
	// Retail thresholds the linearly sampled depthspin height field against
	// the water plane. This material keeps that comparison in the raster pass.
	Ref<Shader> minimap_water_shader_;
	Ref<ShaderMaterial> minimap_water_material_;
	// The spinmap sandwich: the retail terrain draws twice (base + additive
	// x4-stage resubmission), so the second pass and everything the map
	// layers above it ride pinned-order child items. The M-cycle big map
	// gets its OWN trio ABOVE the flat HUD and the corner map — retail
	// draws it as a second pass over the whole overlay set, under only the
	// objectives-family legs (witness at hud_frame.h HudDrawList::big_map).
	RID map_base_item_;
	RID map_add_item_;
	RID map_water_item_;
	bool map_water_sampling_configured_ = false;
	RID map_top_item_;
	bool map_top_sampling_configured_ = false;
	RID big_map_base_item_;
	RID big_map_add_item_;
	RID big_map_water_item_;
	bool big_map_water_sampling_configured_ = false;
	RID big_map_top_item_;
	bool big_map_top_sampling_configured_ = false;

	Vector2 draw_surface_() const;
	// Restamp state_'s declutter visibility/level from declutter_.
	void apply_declutter_();
	Ref<Texture2D> load_hud_texture_(const String &p_name,
			bool p_generate_mipmaps = false) const;
	// MODULATE2X equivalence for a white-modulated static sprite: RGB x2
	// saturated, alpha unchanged (the compass ring's pipeline).
	Ref<Texture2D> double_saturate_texture_(const Ref<Texture2D> &p_texture) const;
	void load_crosshair_texture_();
	void clear_font_();
	// Parse one .fnt through the VFS and upload its pages into the slot's
	// page-texture namespace; returns parse success.
	bool load_fnt_(const String &p_name, fnt_font_t &r_font, int p_slot);
	// (Re)load the Arial label pair when the surface width crosses a retail
	// breakpoint, and hand the compiler the pair + the witnessed slot scale.
	void ensure_label_fonts_(float p_surface_w);
	void ensure_additive_item_();
	void ensure_minimap_water_material_();
	void ensure_map_items_();
	void ensure_big_map_items_();
	void render_list_(const opennova::hud::HudDrawList &p_list);
	// p_big selects the sandwich: the corner map's base rides the control's
	// own item (under the flat HUD) with its add/top children just above the
	// flat pass; the big map's whole trio sits above everything flat.
	void render_map_(const opennova::hud::HudMapPass &p_map,
			const std::vector<opennova::hud::GameFontQuad> &p_map_glyphs,
			bool p_big);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudOverlay::ShowHudFlag);
