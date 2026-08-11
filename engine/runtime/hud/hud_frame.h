#pragma once

// The HUD frame compiler (ADR 0033 R2): one compile turns the per-frame HUD
// state + the hudpos.def layout into a typed draw list — screen-space quads,
// lines, and game-font glyph quads — in the witnessed element order. The
// embedder keeps texture upload and rasterization only.
// [orig: HUD_BuildEntityInfo @ 0x4b8440 -> HUD_RenderAllOverlays @ 0x5a8070
//  -> HUD_RenderOverlays @ 0x5a7bb0 -> the per-element draws]
// Witness record: docs/interface/hud-re.md; per-element policy math lives in
// hud/hud_math.h and stays the single source.

#include "hud/game_font.h"
#include "hud/hud_math.h"

#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace opennova::hud {

// Texture slots the embedder registers at configure time; draw-list quads
// reference these (negative = untextured fill/wireframe).
enum HudTexture : int32_t {
	kHudTexNone = -1,
	kHudTexFrame = 0,
	kHudTexCrosshair,
	kHudTexClipGfx,
	kHudTexRoundGfx,
	kHudTexStance0, // six consecutive stance frames
	kHudTexStance1,
	kHudTexStance2,
	kHudTexStance3,
	kHudTexStance4,
	kHudTexStance5,
	kHudTexSightsBase, // authored SIGHTS rows: kHudTexSightsBase + row index
};

struct HudQuad {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
	uint32_t color = 0xFFFFFFFFu; // 0xAARRGGBB
	int32_t texture = kHudTexNone;
	bool filled = true;    // false = 1px wireframe rect
	bool additive = false; // SIGHTS add-blend rows
};

// One textured triangle (the crosshair's tapered arm strips)
// [orig: HUD_DrawCrosshairCornerQuad @ 0x590f50].
struct HudTriVertex {
	float x = 0.0f;
	float y = 0.0f;
	float u = 0.0f;
	float v = 0.0f;
};

struct HudTri {
	HudTriVertex a;
	HudTriVertex b;
	HudTriVertex c;
	uint32_t color = 0xFFFFFFFFu;
	int32_t texture = kHudTexNone;
};

struct HudLine {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float width = 1.0f;
	uint32_t color = 0xFFFFFFFFu;
};

// The authored 4-field hudpos position record (x, y, hidden, align).
struct HudPosRecord {
	int x = 0;
	int y = 0;
	int hidden = 0;
	int align = 0;
	bool present = false;
};

struct HudRectRecord {
	float x = 0.0f;
	float y = 0.0f;
	float w = 0.0f;
	float h = 0.0f;
	bool present = false;
};

struct HudSightsRow {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	bool additive = false;
	bool texture_valid = false;
};

// The layout globals parsed once from hudpos.def (the embedder resolves
// texture names to the slots above and hands the parsed .fnt).
// [orig: the dword_27237xx.. layout global block, loc_59F370 parse]
struct HudLayout {
	HudPosRecord ammo_count;
	HudPosRecord weapon_name;
	HudPosRecord game_info;
	HudPosRecord wpd_info;
	HudPosRecord chat_text;
	HudPosRecord clip_pos;
	HudPosRecord stance_pos;
	HudPosRecord frame_pos;
	HudRectRecord health_rect;
	HudRectRecord heat_rect;
	HudRectRecord power_rect;
	// The six stance frames' authored per-frame offsets + frame-0 dims.
	std::array<int, 6> stance_offset_x{};
	std::array<int, 6> stance_offset_y{};
	int stance_frame0_w = 0;
	int stance_frame0_h = 0;
	std::array<bool, 6> stance_texture_valid{};
	int frame_tex_w = 0;
	int frame_tex_h = 0;
	int crosshair_tex_w = 0;
	int crosshair_tex_h = 0;
	int clip_tex_w = 0;
	int clip_tex_h = 0;
	int round_tex_w = 0;
	int round_tex_h = 0;
	bool frame_texture_valid = false;
	bool crosshair_texture_valid = false;
	// Colors (0xAARRGGBB).
	uint32_t health_border = 0xC759C7C7u;
	uint32_t tag_good = 0xFF05FA0Du;
	uint32_t tag_middle = 0xFFFAA608u;
	uint32_t tag_bad = 0xFFB00A0Au;
	uint32_t hud_text = 0xFFFAD605u;
	uint32_t weapon_text = 0xFFFAD605u;
	uint32_t stance_tint = 0xFFFFFFFFu;
	uint32_t heat_border = 0xFFFFFFFFu;
	uint32_t stance_bad = 0xFFB00A0Au;
	// ALPHAFADE (percent, percent, seconds) [orig: parse @ 0x5a086c].
	float alpha_fade_base = 0.0f;
	float alpha_fade_max = 0.0f;
	float alpha_fade_seconds = 0.0f;
	int chat_lines = 8;
	std::vector<HudSightsRow> sights;
};

struct HudWeaponState {
	bool active = false;
	int clip = -1;
	int reserve = -1;
	int capacity = 0;
	int rounds_per_icon = 1;
	int heat = 0; // 0..0xFFFF
	std::string display_name;
	// The clip-indicator flash key: the original stamps on the (ammo class,
	// reserve, pool) triple; the single-pool port keys (round_type, reserve)
	// [orig: draw_hud_ammo_indicator restamp; D-HUD-5].
	std::string round_type;
	// The HUDCLIPGFX/HUDRNDGFX placement (per-weapon authored offsets/step)
	// and the registered textures' dims.
	int clipgfx_offset_x = 0;
	int clipgfx_offset_y = 0;
	int rndgfx_offset_x = 0;
	int rndgfx_offset_y = 0;
	int rndgfx_step_x = 0;
	int rndgfx_step_y = 0;
	int clip_tex_w = 0;
	int clip_tex_h = 0;
	int round_tex_w = 0;
	int round_tex_h = 0;
	bool clip_texture_valid = false;
	bool round_texture_valid = false;
	bool sights_card_up = false;
};

struct HudWaypointState {
	bool present = false;
	std::string name;
	int distance_m = 0;
};

struct HudObjectiveRow {
	std::string text;
	bool done = false;
};

struct HudAttachLabel {
	float screen_x = 0.0f;
	float screen_y = 0.0f;
	std::string text;
	bool nearest = false;
};

// One projected friendly tag (D-HUD-20) [orig: HUD_DrawEntityLabel @ 0x5a39b0
// via HUD_DrawFriendlyTagsPass @ 0x5a4480]. The presenter projects the anchor
// (entity position + display height + 0.25 u) and supplies the view distance;
// the compiler owns every witnessed draw rule.
struct HudFriendlyTag {
	float screen_x = 0.0f;
	float screen_y = 0.0f;
	int32_t dist_q16 = 0;   // |anchor - view position|, 16.16 world units
	std::string name;       // authored/callsign; empty resolves the fallback
	uint16_t entity_id = 0; // the fallback-name index [orig: (pool<<12)|slot]
	int32_t health_ratio_fp16 = 0x10000;
	bool medic = false;     // CharAttr class flag 8 [orig: charattr.def Medic]
	bool speaking = false;  // entity == g_voicePlaybackEntity @ 0xC6EC38
	bool player = false;    // slot-walk entry (empty callsign draws the bar leg)
};

struct HudMessageLine {
	std::string text;
	int expire_tick = 0;
};

struct HudFrameState {
	int ticks = 0;
	float health_fraction = 1.0f;
	int stance = 0;
	bool binoculars_view_active = false;
	bool aimed_shot_available = false;
	bool keep_crosshair_while_aimed = false;
	int32_t hud_spread_fp16 = 0;
	float fov_deg = 80.0f;
	// The projected aim point in SCREEN pixels; aim_valid=false pins the 1P
	// design center [orig: @ 0x5928a0].
	bool aim_valid = false;
	float aim_screen_x = 0.0f;
	float aim_screen_y = 0.0f;
	bool windup_active = false;
	int windup_held_ticks = 0;
	std::string objective_text;
	// The objectives-panel header line, resolved by the embedder from gametext
	// (Overlays/STROVER_MISSIONOBJECTIVES); empty falls back to the literal
	// [orig: header STROVER_MISSIONOBJECTIVES @ 0x5ba986].
	std::string objectives_header;
	HudWeaponState weapon;
	HudWaypointState waypoint;
	std::vector<HudObjectiveRow> objectives;
	std::vector<HudAttachLabel> attach_labels;
	// Friendly tags (D-HUD-20). Mode default 2 = FULL [orig: Game_Run
	// @ 0x4a7fed]; fog cull against the environment's current fog distance
	// [orig: Env_FogDistCurrent @ 0x5a3b28]; one speaking level shared by the
	// (single) speaking entity [orig: g_audioOutLevelStage1].
	std::vector<HudFriendlyTag> friendly_tags;
	int friendly_tag_mode = 2;
	int32_t fog_dist_q16 = INT32_MAX;
	int speaking_level255 = 0;
};

struct HudDrawList {
	std::vector<HudQuad> quads;
	std::vector<HudTri> tris;
	std::vector<HudLine> lines;
	std::vector<GameFontQuad> glyphs;
	std::vector<GameFontUnderline> underlines;
	int64_t elements_drawn = 0;
};

// Deep in-process module: the whole witnessed element walk, stance cross-fade
// state, the clip-indicator flash state, and the triggered-text message ring
// live here; compile() emits everything for one frame in retail's order.
class HudFrameCompiler {
public:
	void configure(const HudLayout &layout, const fnt_font_t *font);

	// Swap the layout WITHOUT resetting runtime state (stance fade, clip
	// flash, the message ring) — the texture-table refresh path, e.g. the
	// options crosshair-style reload [orig: HUD_LoadAllTextures @ 0x59e3d6
	// re-registers textures without touching the live HUD state].
	void update_layout(const HudLayout &layout);

	// The stance cross-fade restamp [orig: @ 0x599f8a] and the message ring
	// [orig: Chat_AddDebugMessage @ 0x4987f0] are compiler state.
	void push_message(const std::string &text, int now_ticks);
	void reset_runtime_state();

	const HudDrawList &compile(const HudFrameState &state, float surface_w,
			float surface_h);

	const HudDrawList &last_draw_list() const { return draw_list_; }

private:
	struct StanceFade {
		int prev = 0;
		int cur = 0;
		int stamp = 0;
	};

	float sx(float design_x, float surface_w) const;
	float sy(float design_y, float surface_h) const;
	void emit_rect(float x0, float y0, float x1, float y1, uint32_t color,
			bool filled, int32_t texture = kHudTexNone, bool additive = false);
	void emit_wire_rect(float x0, float y0, float x1, float y1, uint32_t color);
	void emit_text(const char *text, float design_x, float design_y,
			float surface_w, float surface_h, uint32_t argb, uint32_t flags);
	float measure_text_w(const char *text) const;
	float text_line_h() const;

	void element_frame(const HudFrameState &state, float w, float h);
	void element_health(const HudFrameState &state, float w, float h);
	void element_stance(const HudFrameState &state, float w, float h);
	void element_weapon_cluster(const HudFrameState &state, float w, float h);
	void element_heat(const HudFrameState &state, float w, float h);
	void element_power(const HudFrameState &state, float w, float h);
	void element_waypoint(const HudFrameState &state, float w, float h);
	void element_objectives(const HudFrameState &state, float w, float h);
	void element_attach_labels(const HudFrameState &state, float w, float h);
	void element_friendly_tags(const HudFrameState &state, float w, float h);
	void element_objective_line(const HudFrameState &state, float w, float h);
	void element_messages(const HudFrameState &state, float w, float h);
	void element_sights_card(const HudFrameState &state, float w, float h);
	void element_crosshair(const HudFrameState &state, float w, float h);
	void element_clip_indicator(const HudFrameState &state, float w, float h);

	HudLayout layout_{};
	GameFont font_;
	HudDrawList draw_list_;
	StanceFade stance_;
	// The clip-indicator flash latch [orig: draw_hud_ammo_indicator flash
	// @ 0x599af9]: the round count drop stamps the flash start.
	int flash_prev_rounds_ = -1;
	int flash_stamp_ = 0;
	std::vector<HudMessageLine> messages_;
};

} // namespace opennova::hud
