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
#include <godot_cpp/variant/projection.hpp>
#include <godot_cpp/variant/transform3d.hpp>
#include <godot_cpp/variant/rid.hpp>
#include <godot_cpp/variant/string.hpp>
#include <godot_cpp/variant/vector2.hpp>
#include <godot_cpp/variant/vector2i.hpp>

#include <formats/fnt/fnt.h>
#include <runtime/hud/hud_config_tokens.h>
#include <runtime/hud/hud_math.h> // FriendlyTagMode
#include <runtime/hud/hud_frame.h>
#include <runtime/hud/hud_layout_from_hudpos.h> // HudLayoutAssets (the names the fill hands back)
#include <runtime/hud/hud_map_view.h> // the DEATH window pass seam

#include "hud/hud_map_pass_renderer.h"

#include <array>

namespace godot {

class ControlsModel;
class HudChatEntry;
class HudDrawListStats;
class RtxtStringFile;
class Simulation;
class PlayerLocalView;
class HudMapOverlays;
class VehicleHudBlock;

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
	// from the player config. The colour default / mask and the spread default
	// are the engine's (HudLayout, Config_SetDefaults), re-exported so the
	// options model has one home for them.
	enum {
		MIN_CROSSHAIR_STYLE = 0,
		MAX_CROSSHAIR_STYLE = 24,
		DEFAULT_CROSSHAIR_COLOR =
				static_cast<int>(opennova::hud::HudLayout::kCrosshairColorDefault),
		CROSSHAIR_COLOR_MASK =
				static_cast<int>(opennova::hud::HudLayout::kCrosshairColorMask),
		DEFAULT_CROSSHAIR_SPREAD = opennova::hud::HudLayout::kCrosshairSpreadDefault ? 1 : 0,
	};

	HudOverlay();
	~HudOverlay();

	// Build the engine HudLayout from a parsed hudpos.def and load the layout
	// art + .fnt through the mounted VFS root (null root -> layout only: bars
	// and boxes draw, textured/text elements self-hide). Resets the compiler's
	// runtime state (a fresh HUD build).
	void configure(const Ref<HudPos> &p_hudpos, const Ref<ResourceRoot> &p_root);
	bool is_configured() const;
	void set_scope_state(const Ref<PlayerLocalView> &p_view, const Ref<RtxtStringFile> &p_gametext);
    void set_player_context(const Ref<PlayerLocalView> &p_view);
	void set_combat_state(const Ref<PlayerLocalView> &view, const Transform3D &camera,
			const Projection &projection, bool has_camera, const Ref<RtxtStringFile> &gametext,
			const String &use_key);

	// Select and (when configured) immediately reload the crosshair art;
	// runtime state (messages, fades) survives the layout refresh.
	void set_crosshair_style(int p_style);
	int get_crosshair_style() const;

	// The user crosshair colour (0xRRGGBB, forced opaque) and the spread
	// enable — the other two retail crosshair options; defaults and witness
	// live on the engine layout fields (hud_frame.h).
	void set_crosshair_color(int p_rgb);
	int get_crosshair_color() const;
	void set_crosshair_spread_enabled(bool p_enabled);
	bool is_crosshair_spread_enabled() const;

	// Install the equipped weapon's HUD slice (PlayerHudWeaponDef's fields,
	// passed typed) plus its resolved display name; loads the per-weapon
	// HUDCLIPGFX/HUDRNDGFX art.
	void set_weapon(const String &p_weapon_name, const String &p_display_name,
			int p_clipsize, int p_rounds_per_icon,
			const String &p_clipgfx_texture, const Vector2i &p_clipgfx_offset,
			const String &p_rndgfx_texture, const Vector2i &p_rndgfx_offset,
			const Vector2i &p_rndgfx_step);
	void clear_weapon();

	// A mission triggered-text line for the message feed, stamped at the last
	// set ticks (the engine ring owns life/stagger policy).
	void push_message(const String &p_text);
    void reset_overlay_buffers();
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
	// The clip-flash key's def halves (hud_frame.h HudWeaponState carries the
	// witness): the equipped def's ammo bucket and ammo-class id, off the
	// PlayerWeaponView the presenter already reads.
	void set_weapon_ammo_key(int p_ammo_bucket, int p_ammo_class_id);
	// The /NOHUD launch flag's overlay master word (hud_frame.h
	// hud_overlay_master): true clears it for the process.
	void set_no_hud(bool p_no_hud);
	// The resolved gametext Overlays/STROVER_MISSIONOBJECTIVES header line.
	void set_objectives_header(const String &p_text);
	// The Tab board: whether it is held open, the strings the shell resolved,
	// the Simulation the rows and session facts are pulled from natively
	// (Simulation::fill_scoreboard — no script-side row round-trip), and the
	// gametext table the drawers' own lookups resolve through
	// (hud::scoreboard_text). Typed cross-class args on the bound API follow
	// the set_minimap_terrain precedent; pass null when hiding.
	void set_scoreboard(bool p_shown, int64_t p_game_type, int p_frame_counter,
			const Dictionary &p_strings, const Ref<Simulation> &p_sim,
			const Ref<RtxtStringFile> &p_gametext);
	// The open chat capture's input line off the talk-key object: its prompt,
	// text and dispatch color (hud::chat_input_line_color, the Global color
	// keyed on the session-peer bit) and the frame counter the cursor blinks on.
	void set_chat_input(const Ref<HudChatEntry> &p_chat, int64_t p_frame, bool p_mp_session_peer);
	// PgUp/PgDn for the Tab board — consumed (true) only in a session with the
	// board up (hud::scoreboard_takes_page_keys); the open edge resets the page.
	bool scoreboard_page_key(bool p_forward, bool p_in_session, bool p_board_open);
	void reset_scoreboard_page();
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
	void set_vehicle_panel(bool p_shown, const Ref<VehicleHudBlock> &p_block, int p_stance,
			const Ref<Simulation> &p_sim);
	// One line for the CHAT ring (the script chat lines); the engine ring
	// word-wraps it.
	void push_chat_line(const String &p_text, int64_t p_argb);
	// Drain the sim's S2C 0x1E feed lines, 0x14 chat lines and 0x32 join/leave
	// lines (Simulation::drain_feed_posts, resolved against the gametext
	// table) into their rings in wire order. Returns the frame's last involved
	// 0x1E line ("" when none), which the shell hands the kill banner.
	String post_feed_lines(const Ref<Simulation> &p_sim, const Ref<RtxtStringFile> &p_gametext,
			bool p_mp_verbose);
	// The CMAP chat panel's send (CHAT_TEAM: `p_all` false; CHAT_ALL: true):
	// the line through the team (Chat_SendGlobalMessage, a misnomer) or the
	// global (Chat_SendTeamMessage, a misnomer) sender; a flooded line echoes
	// on the CHAT ring in that sender's colour. Returns the ChatSendResult.
	// Witness: hud-re "The windowed map views" (CMap_HandleChatSubmit).
	// CMAP's CHAT_MSGS slot: the console messages (the engine's
	// compile_console_messages over this overlay's frame state, at its own
	// surface) drawn into `p_item`, cleared first.
	void render_console_messages(const RID &p_item);
	int send_command_map_chat(const Ref<Simulation> &p_sim, bool p_all, const String &p_text,
			int64_t p_frame);
	// The Recent Messages (J) window: the OldMessages toggle and its stdbox
	// title (gametext Overlays/STROVER43, resolved by the shell).
	// The SP Show Score statistics panel (hud/end_round_statistics.h).
	void set_end_round_statistics(bool p_shown, bool p_raised,
			const String &p_title, const PackedStringArray &p_labels,
			const PackedStringArray &p_values);
	void set_message_log_shown(bool p_shown);
	// The key-toggled windows (hud_overlay_windows.cpp; engine
	// runtime/hud/hud_overlay_windows.h): the F1 help page's resolved text,
	// the F12 legend's title / labels (one per map_legend_keys() entry) and
	// pulse clock, the I briefing panel (its text read from the sim's
	// mission text) and the briefing's page keys (direction 0 resets).
	void set_help_screen(bool p_shown, const String &p_title, const String &p_page_line,
			const String &p_footer, const PackedStringArray &p_keys,
			const PackedStringArray &p_texts);
	void set_map_legend(bool p_shown, const String &p_title, const PackedStringArray &p_labels,
			int p_frame_counter);
	static PackedStringArray map_legend_keys();
	void set_briefing(bool p_shown, const Ref<Simulation> &p_sim);
	void cycle_briefing_page(int p_direction, bool p_in_session);
	// The tip panel (engine hud/tip_system.h): the showing tip, its
	// countdown, the local player's dead bit (a dead player's M-cycle map
	// frame draws no tip), with its Tips strings resolved from the gametext
	// table and its "$token$" keys from the live bindings.
	void set_tip(int p_tip, int p_countdown, bool p_local_dead,
			const Ref<RtxtStringFile> &p_gametext, const Ref<ControlsModel> &p_controls);
	void set_message_log_title(const String &p_title);
	// The AAS zone status panel: shown, the session game type (the conquest
	// arm is unmodelled and draws nothing), the viewer's team, the HUD frame
	// counter the blink masks, the two status strings the shell resolved
	// ({under_attack, ready}), and the Simulation the zone rows are pulled
	// from natively (Simulation::fill_lfp_zones). Pass null when hiding.
	void set_lfp_panel(bool p_shown, int64_t p_game_type, int p_local_team,
			int p_frame_counter, const Dictionary &p_strings, const Ref<Simulation> &p_sim);
	void set_waypoint(const String &p_name, int p_distance_m,
			const Vector2 &p_mission_position = Vector2(),
			float p_altitude_wu = 0.0f);
	void clear_waypoint();
	// The objectives panel: the sim's shown win-condition rows resolved
	// through the mission text table (Simulation::fill_objectives); hidden,
	// or no sim, clears the panel — the retail toggle's off state.
	void set_objectives(bool p_shown, const Ref<RtxtStringFile> &p_mission_text,
			const Ref<Simulation> &p_sim);
	// The floating attach labels: the sim's selection (distance/LOS/occupancy/
	// nearest, armory-zone mode; Simulation::fill_attach_labels) projected
	// through the play camera (its global transform + projection) to overlay
	// pixels, each with its label text resolved in the gametext table's
	// Overlays section. Behind-camera points drop at projection, mirroring
	// the frustum clip.
	void set_attach_labels(const Transform3D &p_camera_xform,
			const Projection &p_camera_projection, const Ref<RtxtStringFile> &p_gametext,
			const Ref<Simulation> &p_sim);
	// The read seams over the projected labels (tests and probes): the count,
	// the index of the full-bright nearest label (-1 = none) and a label's
	// resolved text and screen-pixel anchor (which may be outside the viewport).
	int get_attach_label_count() const;
	int get_attach_label_selected() const;
	String get_attach_label_text(int p_index) const;
	Vector2 get_attach_label_position(int p_index) const;
	// The overhead friendly tags (D-HUD-20): the sim's pool-0 gather
	// (Simulation::fill_friendly_tags) lifted, projected through the play
	// camera with its view distance and fed to the compiler's element; the
	// environment's live fog distance rides along for the fog cull (the
	// speaking level stays the dialog-channel follow-up). `shown` false, no
	// sim, or the OFF mode clears the tags.
	void set_friendly_tags(bool p_shown, const Transform3D &p_camera_xform,
			const Projection &p_camera_projection, float p_fog_distance_units,
			const Ref<Simulation> &p_sim);
	// The radio-request icon's viewer gate, a per-frame local-player fact
	// (engine world::friendly_tag_radio_request_viewer: a Controller/Driver
	// seat or the player's own S2C 0x6D latch); the compiler ANDs it with
	// each tag's own fold before drawing the icon cell.
	void set_radio_request_icon_viewer(bool p_viewer);
	// The HUD's per-frame role facts (Simulation::hud_role_facts ->
	// inmatch::hud_role_facts): the breath bar's samples, seconds and
	// round-over latch plus its Overlays/STROVER91 label; the MP session lines'
	// facts plus their gametext strings (hud_session_text); the HUDLS slot
	// bar's category scan with its icons, loaded only while the layout's
	// HUDLS_SYSTEM draws the bar. No sim leaves every one empty.
	void set_role_facts(const Ref<Simulation> &p_sim, const Ref<RtxtStringFile> &p_gametext);
	// The overlay-panel pass's key-toggled state (engine hud_toggles.h): the F9
	// emotes and F10 radio menus' open flags, which make the next role-facts
	// read resolve their rows (shown only with the death screen down), and the
	// single-player pause word, which draws Overlays/STROVER7.
	void set_overlay_panel_windows(bool p_emotes_menu_open, bool p_radio_menu_open,
			bool p_paused);
	// The HUDLS key labels by weapon category 0..11: the display string of the
	// binding record 200 + category (ControlsModel.display_text_for_action_code
	// of 200 + category); each drawn slot takes its recorded def's category's.
	void set_slot_bar_key_labels(const PackedStringArray &p_labels_by_category);
	// The quit dialog (engine HudToggleState::quit_dialog_open): its Overlays
	// text by role (engine hud_server_status.h quit_dialog_text_key).
	void set_quit_dialog(bool p_open, bool p_in_session, bool p_authority,
			const Ref<RtxtStringFile> &p_gametext);
	// THE SERVER-STATUS PAGE (engine hud/hud_server_status.h): while shown it
	// replaces the whole scene frame — the viewport stops drawing its 3D
	// world and this overlay draws the page, at most every 200 ms (10 s with
	// the window unfocused), keeping the last page in between. The roster
	// and host facts are the authority's (inmatch/server_status_feed.h over
	// the sim's host context); the Server strings resolve through `gametext`.
	// The witness is the engine header's.
	void set_server_status_page(bool p_shown, bool p_score_list_open,
			const Ref<RtxtStringFile> &p_gametext, const Ref<Simulation> &p_sim);
	bool is_server_status_page_shown() const { return server_status_shown_; }
	// The friendly-tags mode (hud_math.h FriendlyTagMode carries the
	// witness): OFF / FARBRIEF (text under 300 m) / FULL (text always) / BRIEF (tick marks).
	enum FriendlyTagMode {
		FRIENDLY_TAGS_OFF = static_cast<int>(opennova::hud::FriendlyTagMode::kOff),
		FRIENDLY_TAGS_FAR_BRIEF = static_cast<int>(opennova::hud::FriendlyTagMode::kFarBrief),
		FRIENDLY_TAGS_FULL = static_cast<int>(opennova::hud::FriendlyTagMode::kFull),
		FRIENDLY_TAGS_BRIEF = static_cast<int>(opennova::hud::FriendlyTagMode::kBrief),
	};
	// The friendly-tags mode (retail g_FriendlyTagsMode; the witnessed rules
	// live in hud_math). Out-of-range values clamp to the last mode.
	void set_friendly_tag_mode(FriendlyTagMode p_mode);
	FriendlyTagMode get_friendly_tag_mode() const;
	void set_hud_color_index(int p_index);
	int get_hud_color_index() const;
	// The HUD declutter level 0..3 [orig: the persisted cfg int
	// "hud_detail" @0x24D20BC driving CRenderState_SetLayerVisibility
	// @0x59B0F0, see docs/interface/hud-re.md]. The overlay owns the
	// HUDDECLUT mask table (fed from the parsed hudpos in configure) and
	// rebuilds the compiler's per-slot visibility here.
	void set_hud_detail_level(int p_level);
	int get_hud_detail_level() const;
	// BMS action 28 sub 37: HUD item flash timer `index` takes `value` and the
	// layer table rebuilds at declutter level 0 (hud_declutter.h HudItemFlash
	// / HudDeclutter::apply_level). The timers count down on the ticks
	// set_player_state carries.
	void set_item_flash(int p_index, int p_value);
	// The showhud 2-bit FP-view flags [orig: g_FpWeaponViewFlags cycle
	// @0x4E0561]: bit 0 = the FP gun (consumed by the viewmodel rig, not
	// here), bit 1 = the corner spinmap block.
	void set_showhud_flags(int p_flags);
	// The per-player sight-scale index the SIGHTS card's `scale` rows draw
	// at, held on the compiler's frame state so it outlives weapon changes
	// and starts at the engine default with each HUD build (the per-mission
	// player init); the `dotsize` action cycles it (the engine's
	// next_sight_scale_index, <runtime/hud/sight_overlay.h>). The cycle
	// returns the new index.
    void set_kill_announcement(const String &text, int64_t tick);
    void set_aspect_mode(int mode) { state_.aspect_mode = mode; queue_redraw(); }
	int cycle_sight_scale();
	int get_sight_scale_index() const;
	// Per-frame environment feed: the fog cull distance in world units (<= 0
	// disables; the 16.16 form is this seam's) and the speaking entity's voice
	// level 0..255.
	void set_friendly_tag_env(float p_fog_distance_units, int p_speaking_level255);
	// Device-facing minimap feeds. TerrainData is sampled once into the
	// portable sector layout; snapshot is Simulation's versioned fixed-stride
	// retained overlay buffer.
	void set_minimap_terrain(const Ref<TerrainData> &p_terrain,
			const Ref<Texture2D> &p_water_mask = Ref<Texture2D>());
	// The water mask the last set_minimap_terrain installed (a read seam the
	// GUT presenter pins use; nothing else reads it).
	Ref<Texture2D> get_minimap_water_mask() const;
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
	// The non-bank map legs' feed (Simulation::get_hud_minimap_overlays);
	// null clears it.
	void set_minimap_overlays(const Ref<HudMapOverlays> &p_overlays);
	// The per-frame radar-contact snapshot (Simulation.step_hud_radar), and
	// the HUD-pass gates that step rides (HudFrameCompiler::radar_frame_gates).
	void set_minimap_radar(const PackedInt32Array &p_feed);
	int get_radar_frame_gates() const;

	// Debug/test accessor: compile at the current surface size and report the
	// draw list's element counts.
	Ref<HudDrawListStats> get_draw_list_stats();

	// F3 Stats seam: _draw() runs inside Godot's deferred flush (outside every
	// Node callback), so its compile + canvas-emit cost is timed here and
	// consumed by the HUD presenter's next tick. Clock reads only while enabled.
	void set_draw_timing_enabled(bool p_enabled);
	// [compile_us, emit_us] accumulated since the last consume; zeroes after.
	PackedInt64Array consume_draw_timing_us();

	void _draw() override;

	// The menu map windows' seam (hud/map_view_window.h, C++ only): compile
	// the DEATH or the CMAP window pass over this overlay's frame state
	// (terrain, markers, footprints, fonts) at the host's surface, and hand
	// the host's renderer the textures and materials the map passes sample.
	// Null before configure().
	const opennova::hud::HudFrameCompiler::MapWindowDraw *compile_death_map(
			const opennova::hud::DeathMapFrame &p_frame,
			const opennova::hud::DeathMapFacts &p_facts, float p_surface_w, float p_surface_h);
	const opennova::hud::HudFrameCompiler::MapWindowDraw *compile_command_map(
			opennova::hud::CommandMapView &p_cmap, const opennova::hud::MapViewRect &p_rect,
			int32_t p_scaled_800, const opennova::hud::DeathMapFacts &p_facts,
			float p_surface_w, float p_surface_h);
	HudMapPassTextures map_pass_textures() const;
	RID map_additive_material();
	RID map_water_material();
	// The fixed-function MODULATE2X(TEXTURE, DIFFUSE) colour stage with the
	// MODULATE alpha stage under SRCALPHA/INVSRCALPHA, for the map sprites
	// flagged HudMapSprite::modulate2x (the bit-10 radar marks).
	RID map_modulate2x_material();

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
	static int hud_detail_level_default();
	static int hud_detail_level_blank();
	static int next_hud_detail_level(int p_level);
	// The sight-scale index policy (engine/runtime/hud/sight_overlay.h): the
	// player-init default.
	static int sight_scale_index_default();
	static FriendlyTagMode next_friendly_tag_mode(FriendlyTagMode p_mode);

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
	uint32_t voice_menus_ = 0; // inmatch::kHudVoiceMenu* bits (set_overlay_panel_windows)
	std::array<std::string, 12> slot_bar_key_labels_; // by def category (set_slot_bar_key_labels)
	bool server_status_shown_ = false;                // set_server_status_page
	bool scene_3d_was_disabled_ = false;              // the viewport's own setting under the page
	opennova::hud::ServerStatusPageState server_status_page_;
	// The HUDDECLUT mask table + level (engine hud_declutter carries the
	// witness map); apply_declutter_() restamps the compiler input's
	// visibility table after any mask or level change.
	opennova::hud::HudDeclutter declutter_;
	opennova::hud::HudItemFlash item_flash_;
	Ref<ResourceRoot> root_;
	std::array<Ref<Texture2D>, kTextureSlots> textures_;
	// Font glyph pages, one namespace per compiler font slot
	// (opennova::hud::kHudFontSlot*): slot * FNT_MAX_PAGES + page.
	std::array<Ref<Texture2D>, opennova::hud::kHudFontSlotCount * opennova::fnt::FNT_MAX_PAGES>
			page_textures_;
	opennova::fnt::fnt_font_t font_ = {};
	bool font_valid_ = false;
	// The Arial overlay label pair (engine hud_label_font_choice picks the
	// faces/scale), loaded lazily per surface-width tier — retail re-inits
	// its overlay fonts on resolution change.
	opennova::fnt::fnt_font_t label_font_ = {};
	bool label_font_valid_ = false;
	opennova::fnt::fnt_font_t label_font_bold_ = {};
	bool label_font_bold_valid_ = false;
	opennova::fnt::fnt_font_t label_font_large_ = {};
	bool label_font_large_valid_ = false;
	opennova::fnt::fnt_font_t label_font_impact38_ = {}; // Impac38b (the end-round overlay)
	bool label_font_impact38_valid_ = false;
	int label_tier_ = -1; // -1 = not loaded; 0 <=640 / 1 <=800 / 2 >800
	int label_width_ = 0;  // the width the pushed label scales were computed for
	// The hudpos FONTHUD1_LO / _HI names (engine hudpos_font_for_width picks one
	// per width); loaded into font_ by ensure_label_fonts_.
	std::string hudpos_font_lo_;
	std::string hudpos_font_hi_;
	bool configured_ = false;
	int crosshair_style_ = MIN_CROSSHAIR_STYLE;
	uint32_t crosshair_color_ = opennova::hud::HudLayout::kCrosshairColorDefault;
	bool crosshair_spread_enabled_ = true;
	bool draw_timing_enabled_ = false;
	int64_t draw_compile_us_ = 0;
	int64_t draw_emit_us_ = 0;
	// The sid whose silhouette currently occupies the kHudTexVehiclePanel
	// slot (reloaded on change).
	String vehicle_panel_sid_;
	// Additive rows cannot share this item's blend mode: they render through a
	// child RenderingServer canvas item carrying a BLEND_MODE_ADD material.
	Ref<CanvasItemMaterial> additive_material_;
	RID additive_item_;
	// Retail thresholds the linearly sampled depthspin height field against
	// the water plane. This material keeps that comparison in the raster pass.
	Ref<Shader> minimap_water_shader_;
	Ref<ShaderMaterial> minimap_water_material_;
	// The flat HUD items' material: the default canvas draw plus the second
	// texture stage a HudQuad::texture2 quad carries (the stdbox border
	// pieces' screen-anchored camo). Both flat items (this control's own and
	// the top-layer child) carry it so a stage-1 quad keeps its place in the
	// kind-grouped submission.
	Ref<Shader> flat_shader_;
	Ref<ShaderMaterial> flat_material_;
	bool flat_material_bound_ = false;
	Ref<Shader> map_modulate2x_shader_;
	Ref<ShaderMaterial> map_modulate2x_material_;
	// The spinmap sandwich: the retail terrain draws twice (base + additive
	// x4-stage resubmission), so the second pass and everything the map
	// layers above it ride pinned-order child items (hud_map_pass_renderer.h).
	// The M-cycle big map gets its OWN set ABOVE the flat HUD and the corner
	// map — retail draws it as a second pass over the whole overlay set,
	// under only the objectives-family legs (witness at hud_frame.h
	// HudDrawList::big_map).
	HudMapPassRenderer corner_map_;
	HudMapPassRenderer big_map_;
	// The gameplay-overlay windows' layer above the big map
	// (HudDrawList::top_begin).
	RID top_item_;
	// The status page's own item, drawn above every child of this control
	// (the SIGHTS card, the view effects): the page stands in for the whole
	// scene frame.
	RID page_item_;

	Vector2 draw_surface_() const;
	// Restamp state_'s declutter visibility/level from declutter_.
	void apply_declutter_();
	Ref<Texture2D> load_hud_texture_(const String &p_name,
			bool p_generate_mipmaps = false) const;
	// MODULATE2X equivalence for a white-modulated static sprite: RGB x2
	// saturated, alpha unchanged (the compass ring's pipeline).
	Ref<Texture2D> double_saturate_texture_(const Ref<Texture2D> &p_texture) const;
	void load_crosshair_texture_();
	// The combat sprites' loads (the anchors are the engine fill's).
	void configure_combat_(const opennova::hud::HudLayoutAssets &assets);
	void combat_texture_(int slot, const String &name, opennova::hud::HudSprite &sprite);
	std::array<String, kTextureSlots> combat_texture_names_;
	// Stamp the cached colour/spread options into layout_.
	void apply_crosshair_options_();
	void clear_font_();
	// Parse one .fnt through the VFS and upload its pages into the slot's
	// page-texture namespace; returns parse success.
	bool load_fnt_(const String &p_name, opennova::fnt::fnt_font_t &r_font, int p_slot);
	// (Re)load the Arial label pair when the surface width crosses a retail
	// breakpoint, and hand the compiler the pair + the witnessed slot scale.
	void ensure_label_fonts_(float p_surface_w);
	void push_label_fonts_(const opennova::hud::HudLabelFontChoice &p_choice);
	void ensure_additive_item_();
	void ensure_minimap_water_material_();
	void ensure_flat_material_();
	void ensure_map_materials_();
	void render_list_(const opennova::hud::HudDrawList &p_list);
	const opennova::hud::HudDrawList &server_status_page_draw_list_(const Vector2 &p_surface);
	// One index range per flat kind of a draw list.
	struct FlatRange {
		size_t quads_begin = 0, quads_end = 0;
		size_t tris_begin = 0, tris_end = 0;
		size_t lines_begin = 0, lines_end = 0;
		size_t glyphs_begin = 0, glyphs_end = 0;
		size_t underlines_begin = 0, underlines_end = 0;
	};
	void render_flat_(const RID &p_item, const opennova::hud::HudDrawList &p_list,
			const FlatRange &p_range);
	// The range split at the compiler's order breaks (HudDrawList::order_breaks),
	// each run submitted kind-grouped in turn.
	void render_flat_runs_(const RID &p_item, const opennova::hud::HudDrawList &p_list,
			const FlatRange &p_range);
	void ensure_top_item_();
	void ensure_page_item_();
	// p_big selects the sandwich: the corner map's base rides the control's
	// own item (under the flat HUD) with its add/top children just above the
	// flat pass; the big map's whole trio sits above everything flat.
	void render_map_(const opennova::hud::HudMapPass &p_map,
			const std::vector<opennova::hud::GameFontQuad> &p_map_glyphs,
			bool p_big);
};

} // namespace godot

VARIANT_ENUM_CAST(godot::HudOverlay::ShowHudFlag);
VARIANT_ENUM_CAST(godot::HudOverlay::FriendlyTagMode);
