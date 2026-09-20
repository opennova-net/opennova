#pragma once

// The menu frame compiler (ADR 0033 R2): one compile turns a parsed .mnu
// SCREEN + the per-frame widget state into a typed draw list — screen-space
// quads (widget art, color fills, tiled frame pieces), outline wireframes,
// and game-font glyph quads — in the witnessed per-widget draw order. The
// embedder keeps texture upload and rasterization only.
// [orig: Menu_RenderFrame @ 0x54b7c0 -> CUIScene_DrawScreensAndCursor
//  @ 0x63bf60 -> the CWnd Draw vtable family (CUIElement_Draw @ 0x64a8a0,
//  CStaticWnd_Render @ 0x657b10, CEditWnd_Render @ 0x6619e0, ...)]
// Witness record: docs/mnu/menu-re.md ("Widget render dispatch").

#include <runtime/hud/game_font.h>
#include <runtime/menu/menu_edit.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

#include <cstdint>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace opennova::menu {

// The runtime visual state written to elem+236 every frame by the input pump
// [orig: widget_process_mouse_event @ 0x647a00; setter CWnd_SetVisualState
// @ 0x646340]. The numeric values are the original's: they index the
// per-state appearance records AND select the FONT color pair, and the
// APPEARANCE STATE attribute parses to the same indices
// [orig: @ 0x6483d4..0x64845e — DEFAULT=0, DISABLED=1, MOUSEOVER=2,
// SELECTED=3].
enum WidgetVisualState : int32_t {
	kStateDefault = 0,
	kStateDisabled = 1,
	kStateMouseover = 2,
	kStateSelected = 3,
};

inline constexpr int32_t kMenuTexNone = -1;

// Menus are authored in the fixed 800x600 design space; the anamorphic scale
// pair every compile/hit call takes is surface_w / kMenuDesignWidth and
// surface_h / kMenuDesignHeight [orig: CUIScene_SetScreenScale @ 0x639480].
inline constexpr int kMenuDesignWidth = 800;
inline constexpr int kMenuDesignHeight = 600;

// One draw-list quad. `texture` indexes the compiler's interned texture-name
// table (texture_names()); kMenuTexNone is an untextured color fill. A valid
// `texture2` asks the device leg for retail's two-stage frame material:
// 2 * texture * texture2, with the first texture's alpha masking the result.
// [orig: init_border_materials @ 0x646f70 creates border_material from the
// STENCIL and BRUSH handles with mode 0x651 / two stages]. `tiled` repeats the
// selected UV region at native device pixels; frame fills select the stencil
// atlas cell (3, 0), copied by retail into border_fill_material. Everything
// else is stretched into the quad, UV 0..1 (or an atlas sub-rect)
// [orig: CUIElement_DrawStretchedTexture @ 0x647d40; the IMAGE pass
//  CUIElement_DrawTextureNative @ 0x647e40 -> CTextureManager_DrawScaledRect
//  @ 0x654e60].
struct MenuQuad {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	float u0 = 0.0f;
	float v0 = 0.0f;
	float u1 = 1.0f;
	float v1 = 1.0f;
	uint32_t color = 0xFFFFFFFFu; // 0xAARRGGBB modulate
	int32_t texture = kMenuTexNone;
	int32_t texture2 = kMenuTexNone;
	bool tiled = false;
};

// One 1px outline segment [orig: CUIElement_DrawOutlineRect @ 0x647fc0 —
// four lines around the scaled rect in the entry color].
struct MenuLine {
	float x0 = 0.0f;
	float y0 = 0.0f;
	float x1 = 0.0f;
	float y1 = 0.0f;
	uint32_t color = 0xFFFFFFFFu;
};

struct MenuDrawList {
	std::vector<MenuQuad> quads;
	std::vector<MenuLine> lines;
	std::vector<hud::GameFontQuad> glyphs;
	std::vector<hud::GameFontUnderline> underlines;
	// Glyphs group into font runs; `font` indexes font_names() so the applier
	// knows which font atlas each glyph's page index refers to.
	struct FontRun {
		int32_t font = 0;
		int32_t first = 0;
		int32_t count = 0;
		int32_t underline_first = 0;
		int32_t underline_count = 0;
	};
	std::vector<FontRun> font_runs;
	// Painter-order stream over the typed payload arrays. Primitive storage
	// stays compact by kind; this sequence preserves the original forward
	// widget/child walk across kind boundaries.
	// [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60; per-widget vtable+24]
	struct DrawOp {
		enum class Kind { Quad,
			Line,
			FontRun };
		Kind kind = Kind::Quad;
		int32_t index = 0;
	};
	std::vector<DrawOp> draw_ops;
	// The first op of the menu-top overlay: the open dropdown popups (the
	// D-MNU-12 post-walk pass) and the cursor, which retail paints after every
	// screen widget [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]. An
	// embedder that mounts its own controls over the frame (the PLAYER_INFO
	// preview and icon mounts) must draw ops from here on ABOVE those mounts.
	int32_t overlay_op_start = 0;
	int64_t widgets_drawn = 0;
};

// Per-widget per-frame state, keyed by the widget's pre-order index over the
// configured screen tree (0 = the root window; children in authored order).
// Unlisted widgets take the defaults. The fields mirror the original runtime
// widget fields: shown (+224), enabled (+228), the pump's hover/press verdict
// (+236) [orig: widget_process_mouse_event @ 0x647a00], checked (+772),
// keyboard focus (g_ui_focus_wnd @ 0x31C16D4), and the caret char index
// (+764) the edit render feeds the shared text draw
// [orig: CEditWnd_Render @ 0x6619e0].
struct MenuWidgetState {
	int32_t index = 0;
	bool hide = false;        // runtime WINDOW HIDE override
	bool show = false;        // runtime WINDOW SHOW override
	bool disabled = false;    // runtime disable (visual state 1)
	bool hovered = false;     // mouse over, button up (visual state 2)
	bool pressed = false;     // mouse held on the widget (visual state 3)
	bool has_checked = false; // runtime checked override (else authored)
	bool checked = false;
	bool focused = false;     // keyboard focus [orig: @ 0x31C16D4]
	int32_t caret = -1;       // edit caret char index into the value text
	bool has_text = false;    // runtime text override (edit value, ...)
	std::string text;
	int32_t selected_item = 0;  // spinlist/combo/list selected row
	int32_t hover_item = -1;    // list hover row (row style 2)
	int32_t scroll_row = 0;     // list first visible row
	// Standalone CScrollWnd range state. The page value is the original
	// inclusive-page field (visible count - 1), not a row count. Constructor
	// defaults are min=max=value=0, page=10; the embedder sets authored controls
	// after finding them by NAME.
	// [orig: CScrollWnd_Construct @ 0x64c450; CScrollWnd_SetPageSize
	// @ 0x64ce10; CScrollWnd_SetRangeAndClamp @ 0x64d490;
	// CScrollWnd_SetScrollPos @ 0x64ce20]
	bool has_scroll_range = false;
	int32_t scroll_min = 0;
	int32_t scroll_max = 0;
	int32_t scroll_page = 10;
	int32_t scroll_value = 0;
	bool popup_open = false;    // combo: draw the LIST_BOX popup
	// Runtime item rows (text) the embedder seeds into a list/combo/spinlist —
	// the Control-tree path seeded these via set_items; when present they
	// replace the authored <ITEM> rows for the closed cell, the list rows,
	// and the combo popup alike.
	bool has_items = false;
	std::vector<std::string> items;
	// Additional selected rows for MULTI lists (drawn with the selection
	// style alongside selected_item); the single-select widgets ignore it.
	std::vector<int32_t> selected_items;
	// TABLE data rows (runtime content the embedder seeds — the Control-tree
	// path seeded these from the shell): one vector of cell strings per row,
	// in column order [orig: the 40-byte row records, CUITable_Render
	// @ 0x6411d0]. scroll_row above is the first visible row.
	std::vector<std::vector<std::string>> table_rows;
	// MARQUEE nodes (runtime content from the widget's datasource file):
	// text lines in roll order; empty string = a blank spacer line
	// [orig: render_scrolling_credits @ 0x65ca00 walks the node list].
	std::vector<std::string> marquee_lines;
	// Restart the roll from the initial layout on the next compile.
	bool marquee_reset = false;
};

struct MenuFrameState {
	std::vector<MenuWidgetState> widgets;
	// Milliseconds clock for the caret blink: the caret draws while
	// (time_ms & 0x3FF) > 0x200 [orig: CEditWnd_Render @ 0x661c63].
	uint32_t time_ms = 0;
	// The mouse cursor pass [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]:
	// drawn LAST at the raw mouse position, native texture size, UNSCALED.
	bool cursor_visible = false;
	float cursor_x = 0.0f;
	float cursor_y = 0.0f;
};

// Deep in-process module: configure() walks the screen once (interning every
// texture and font name it will reference); compile() emits one frame's draw
// list in the witnessed walk order. Scale is the 800x600 anamorphic pair
// [orig: CUIScene_SetScreenScale @ 0x639480]; every scaled coordinate is
// truncated to int PER ELEMENT [orig: @ 0x647d40], and glyph runs are laid
// out at the same pair [orig: font_cache_draw_text_scaled @ 0x653170
// forwards scaleX/scaleY into CGameFont_DrawText]. The edit scroll window
// (start/end per widget [orig: update_edit_scroll_range @ 0x661790]) is
// compiler runtime state and survives across compiles.
class MenuFrameCompiler {
public:
	// Out-of-line: WidgetNode is complete only in the .cpp.
	MenuFrameCompiler();
	~MenuFrameCompiler();
	MenuFrameCompiler(const MenuFrameCompiler &) = delete;
	MenuFrameCompiler &operator=(const MenuFrameCompiler &) = delete;

	// Borrow the screen (not owned; the caller keeps document + fonts alive).
	void configure(const mnu::Screen *screen, const opennova::fnt::fnt_font_t *default_font);

	// The flattened stylesheet (menu_style.mns evaluate output); %VAR% color
	// and texture values resolve through it, case-insensitive, unresolved
	// stays literal [orig: NapiXML_ExpandVariablesInText @ 0x63a000]. Set
	// before configure().
	void set_style_vars(const std::map<std::string, std::string> &vars);

	// The RTXT text table for String/Item type=="id" lookups
	// [orig: CUIStringTable_LookupString @ 0x6527c0]; a miss keeps the id
	// literal. Set before configure().
	void set_text_lookup(const std::map<std::string, std::string> &table);

	// Per-name font registration (case-insensitive, as authored in <FONT>).
	// Unregistered names fall back to the default font. Set before
	// configure(); clear before re-registering when the backing storage is
	// reloaded (the compiler borrows the pointers).
	void register_font(const std::string &name, const opennova::fnt::fnt_font_t *font);
	void clear_registered_fonts();

	// Whole-value %VAR% stylesheet resolution (case-insensitive, unresolved
	// stays literal) — public so the embedder resolves asset NAMES (fonts)
	// the same way the compiler interns them
	// [orig: NapiXML_ExpandVariablesInText @ 0x63a000].
	std::string resolve_style_var(const std::string &value) const {
		return resolve_var(value);
	}

	// The embedder resolves these after configure(): interned texture names
	// in slot order (MenuQuad::texture indexes this) and interned font names
	// (FontRun::font indexes this; slot 0 = the default font).
	const std::vector<std::string> &texture_names() const {
		return texture_names_;
	}
	const std::vector<std::string> &font_names() const { return font_names_; }

	// Report a loaded texture's pixel size. The three-stage POSITION
	// fallback, spin-arrow sizing, native-size item images, the frame stencil
	// atlas, and the cursor consume these; an unreported texture keeps size
	// 0 (degenerate rects stay empty) [orig: the parse-tail POSITION solve
	// @ 0x648120].
	void set_texture_size(int32_t slot, int width, int height);

	const MenuDrawList &compile(const MenuFrameState &state, float scale_x,
			float scale_y);

	const MenuDrawList &last_draw_list() const { return draw_list_; }

	// The pre-order index of the FIRST widget whose authored NAME matches
	// (case-insensitive), or -1 — the companions' name->index seam
	// [orig: CUIScene walks resolve controls by name the same way].
	int widget_index(const std::string &name) const;

	// --- widget queries (valid after configure(); index = pre-order) --------
	// The pre-order index space is the SAME walk a document-side DFS of the
	// screen produces (root first, children in authored order), so embedders
	// can zip indices against document ids.
	int widget_count() const;
	// Effective draw/hit visibility: the widget's own shown flag folded with
	// its ancestors' (state overrides included) — the same gate the draw
	// walk and the pump use. Companion overlays a shell mounts over a widget
	// (e.g. the CBIN credits scroller) must follow it.
	bool widget_shown(int index, const MenuFrameState &state) const;
	std::string widget_name(int index) const;
	// The parsed mnu::WindowType as an int (out of range -> -1).
	int widget_kind(int index) const;
	// The authored STRING content after %VAR% + string-table resolution (the
	// text the widget draws when no runtime override is set).
	std::string widget_authored_text(int index) const;
	// Authored-or-runtime effective disabled (the pump's state-1 test).
	bool widget_disabled(int index, const MenuFrameState &state) const;
	// The widget's authored edit constraints as menu_edit.h limits (READONLY,
	// NUMBER + MINVAL/MAXVAL, MAXCHAR). False when the index is out of range.
	bool widget_edit_limits(int index, EditLimits *out) const;
	// The widget's absolute design-space rect: the three-stage POSITION solve
	// offset by every ancestor's solved origin — the rect the draw walk and
	// the hit walk both use. False when the index is out of range.
	bool widget_rect(int index, const MenuFrameState &state,
			mnu::RectEdges *out) const;
	// The item-row count the draw uses (runtime rows when seeded, else the
	// authored <ITEM> rows; combo popups prefer the authored LIST_BOX rows).
	int item_count(int index, const MenuFrameState &state) const;

	// --- interaction geometry (the same witnessed layout math the emitters
	// use; raw-mouse coordinates against the scaled rects, like pump_mouse) --
	// List/multi row under the mouse (absolute row index, honoring the scroll
	// window), -1 = none [orig: CListWnd_DrawItems @ 0x643f30 row layout].
	int list_row_at(int index, const MenuFrameState &state, float mx, float my,
			float sx, float sy) const;
	// Rows that fit the widget rect (>=1 row height only) — the scroll clamp.
	int list_visible_rows(int index, const MenuFrameState &state) const;
	// The combo LIST_BOX popup rect (authored combo-relative POSITION offset
	// to the combo's absolute rect [orig: CComboWnd @ 0x65be40 D-MNU-7]).
	bool combo_popup_rect(int index, const MenuFrameState &state,
			mnu::RectEdges *out) const;
	// True when the raw-mouse point lies inside the open popup's scaled rect.
	bool combo_popup_contains(int index, const MenuFrameState &state, float mx,
			float my, float sx, float sy) const;
	// Popup row under the mouse, -1 = none.
	int combo_popup_row_at(int index, const MenuFrameState &state, float mx,
			float my, float sx, float sy) const;
	// Spin arrow under the mouse: 0 none, 1 up, 2 down [orig:
	// CSpinListWnd_CreateUpDownChildren @ 0x64b8b0 child rects].
	// Standalone scroll interaction (the witnessed CScrollWnd map; see
	// menu_frame_scrollbar.cpp): hit parts + the thumb drag inverse.
	enum ScrollHit {
		kScrollHitNone = 0,
		kScrollHitUp = 1,
		kScrollHitDown = 2,
		kScrollHitShuttle = 3,
		kScrollHitTrackBefore = 4,
		kScrollHitTrackAfter = 5,
	};
	int scroll_hit_at(int index, const MenuFrameState &state, float mouse_x,
			float mouse_y, float scale_x, float scale_y) const;
	int scroll_drag_anchor(int index, const MenuFrameState &state,
			float mouse_x, float mouse_y, float scale_x, float scale_y) const;
	int scroll_drag_value(int index, const MenuFrameState &state,
			float mouse_x, float mouse_y, float scale_x, float scale_y,
			int anchor) const;
	// Embedded-scrollbar owners (Table/List/Multi/LanList): the max first
	// visible row and the page step (visible - 1) the driver's arrow/track
	// presses use.
	int scroll_row_limit(int index, const MenuFrameState &state) const;
	// The widget whose scrollbar parts contain the point (shipped menus
	// author scrollbars OUTSIDE the owner rect — the D-MNU-16 claim class).
	int scroll_owner_at(const MenuFrameState &state, float mouse_x,
			float mouse_y, float scale_x, float scale_y) const;
	int scroll_page_rows(int index, const MenuFrameState &state) const;
	int spin_arrow_at(int index, const MenuFrameState &state, float mx,
			float my, float sx, float sy) const;
	// Table DATA row under the mouse (absolute row index into table_rows,
	// honoring the scroll window below the header), -1 = none
	// [orig: CUITable_Render @ 0x6411d0 row layout].
	int table_row_at(int index, const MenuFrameState &state, float mx, float my,
			float sx, float sy) const;
	// Non-mutating front-most hit (the pump's claim walk without the state
	// writes) — editor/preview picking.
	int hit_widget(const MenuFrameState &state, float mx, float my, float sx,
			float sy) const;
	// The multiline edit's wrapped-line counts at scale 1.0 — the scroll
	// range twin [orig: font_cache_count_wrapped_lines @ 0x653b90 via
	// CMEditWnd_UpdateScrollRange @ 0x661180]: *fit = rows that fit the
	// widget rect, *total = wrapped line count; scroll range = [0,
	// total - fit]. False when the index is out of range.
	bool multiline_line_counts(int index, const MenuFrameState &state,
			int *fit_lines, int *total_lines) const;
	// The first shown widget carrying the hotkey (pre-order; a hidden subtree
	// never matches — the witnessed accelerator scan the Control tree ran;
	// VIRTUAL rows live in a separate namespace from character rows, and
	// VK_RETURN/VK_ENTER are interchangeable). -1 = none.
	int hotkey_widget(const std::string &key, bool virtual_key,
			const MenuFrameState &state) const;

	// The witnessed per-frame mouse pump [orig: scene_end_frame @ 0x63e600 ->
	// widget_process_mouse_event @ 0x647a00 (vtable+20)]: ONE widget claims
	// the mouse per frame — front-most = last drawn (the reverse sibling walk
	// + the per-frame claim scene+16; equivalently the LAST hit of the
	// forward draw walk). A disabled claimant keeps visual state 1 (no
	// hover/press); hit + button down -> pressed (3); hit + button up ->
	// hovered (2); every other row's hover/press clears (0). Hidden subtrees
	// never hit. The mouse is RAW screen coordinates against the scaled
	// rects; the claimed widget's inherited cursor (else the screen default)
	// rides back for the unscaled cursor pass.
	struct MouseClaim {
		int hovered = -1;               // claimed widget index; -1 = none
		int32_t cursor = kMenuTexNone;  // inherited +276 cursor, else default
		// The CScrollWnd interaction result for this sample [orig:
		// CScrollWnd_HandleEvent @ 0x64d050]: when a scrollbar part owns the
		// sample (press, latch, or drag capture) scroll_index is the owning
		// widget — the claim above stays on it, exactly like retail's child
		// BUTTON capture, so no other widget sees the held samples. A
		// completed arrow/track/drag step reports the new value (standalone
		// Scroll: the authored range value; embedded owners: the
		// first-visible row).
		int scroll_index = -1;
		bool scroll_value_changed = false;
		int scroll_value = 0;
	};
	MouseClaim pump_mouse(MenuFrameState &io_state, float mouse_x,
			float mouse_y, bool button_down, float scale_x, float scale_y);
	// The open-dropdown sample: the popup's scrollbar interaction only,
	// restricted to the open combo `index` (the popup-exclusive dispatch
	// gate). scroll_index >= 0 in the result means the scrollbar owns the
	// sample and the caller must not treat it as a row hover/pick.
	MouseClaim pump_popup_mouse(MenuFrameState &io_state, int index,
			float mouse_x, float mouse_y, bool button_down, float scale_x,
			float scale_y);
	// One wheel tick (steps > 0 scrolls rows down, < 0 up): the open popup
	// exclusively, else the front-most row owner under the point. Deliberate
	// divergence D-MNU-18 — retail ships no functioning menu wheel scroll
	// (see menu_frame_scrollbar.cpp for the witness map).
	bool pump_mouse_wheel(MenuFrameState &io_state, float mouse_x,
			float mouse_y, int steps, float scale_x, float scale_y,
			MouseClaim *claim);

private:
	struct WidgetNode;
	struct WalkScale {
		float x = 1.0f;
		float y = 1.0f;
	};
	struct StatePass {
		bool present = false;
		bool has_color = false; // COLOR=1 -> stretched fill [orig: entry+12]
		uint32_t color = 0;
		int32_t texture = kMenuTexNone; // IMAGE=2 [orig: entry+20]
		// IMAGE sprite-sheet crop. MAP_STATE selects a HEIGHT-tall row from
		// the source texture while the destination remains the widget rect.
		bool has_map_state = false;
		int32_t map_state = -1;
		bool has_image_height = false;
		int32_t image_height = 0;
		bool has_outline = false;       // OUTLINE=8 [orig: entry+16]
		uint32_t outline = 0;
	};
	struct EditScroll {
		int start = 0;
		int end = 0;
	};
	enum class ScrollbarKind { Standalone,
		Embedded,
		Popup };
	// Per-marquee roll state (compiler runtime state, like edit_scroll_):
	// the current scroll offset in design pixels and the last time_ms sample
	// [orig: node y -= rate per frame; whole-roll reset when the last node
	// passes the top — render_scrolling_credits @ 0x65ca00].
	struct MarqueeScroll {
		double offset = 0.0;
		uint32_t last_ms = 0;
		bool valid = false;
	};
	// Label text after string-table resolution, plus the accelerator metadata
	// removed from its first {hot} marker [orig: CButtonWnd_SetLabel @
	// 0x6572F0]. The position is a byte offset in text, or -1 when unmarked.
	struct ResolvedText {
		std::string text;
		std::string hotkey;
		int hotkey_pos = -1;
	};

	// configure-time build
	int build_node(const mnu::Window &w, int parent);
	void build_state_passes(const std::vector<mnu::Appearance> &rows,
			StatePass (&states)[4]);
	std::pair<int, int> state_texture_size(const StatePass &pass) const;
	std::string resolve_var(const std::string &value) const;
	uint32_t resolve_text_color(const std::string &value) const;
	ResolvedText resolve_text_value(const std::string &type,
			const std::string &raw) const;
	int32_t intern_texture(const std::string &name);
	int32_t intern_font(const std::string &name);

	// compile-time walk
	int walk_widget(int index, int origin_x, int origin_y,
			const MenuFrameState &state, const WalkScale &s);
	int skip_widget(int index) const;
	int hit_walk(int index, int origin_x, int origin_y,
			const MenuFrameState &state, float mx, float my, float sx,
			float sy, int *io_hit) const;
	const MenuWidgetState *state_for(const MenuFrameState &state,
			int index) const;
	// Shared row-height rule (authored MIN_ITEM_HEIGHT wins, else the "W"
	// measure) for list rows / combo popup rows.
	int row_height_(const WidgetNode &node) const;
	// The shared row walk behind list_row_at (the widget rect, the embedded
	// scrollbar) and combo_popup_row_at (the popup rect, the popup scrollbar).
	int row_at_in_rect_(int index, const MenuFrameState &state,
			const mnu::RectEdges &rect, ScrollbarKind kind, float mx, float my,
			float sx, float sy) const;
	void table_row_heights_(const WidgetNode &node, int *header_height,
			int *body_row_height) const;
	bool widget_shown_(int index, const MenuFrameState &state) const;
	int pump_visual_state(const mnu::Window &w,
			const MenuWidgetState *ws) const;
	int appearance_state_with_fallback(const WidgetNode &node,
			int state) const;
	mnu::RectEdges solve_rect(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	// Arrow hit over the widget's ABSOLUTE rect (0 none / 1 up / 2 down) —
	// shared by spin_arrow_at and the claim walk's outside-rect arrow claim
	// (D-MNU-16) [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
	int spin_arrow_hit_(const WidgetNode &node, const mnu::RectEdges &rect,
			float mx, float my, float sx, float sy) const;
	ResolvedText resolved_widget_text(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	std::string widget_text(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	const opennova::fnt::fnt_font_t *font_for(const WidgetNode &node) const;
	void measure_text(const WidgetNode &node, const std::string &text,
			int *out_w, int *out_h) const;

	// emitters
	static float emit_x(int design, float scale);
	void push_quad(const MenuQuad &quad);
	void push_line(const MenuLine &line);
	void push_font_run(const MenuDrawList::FontRun &run);
	void emit_rect_quad(const mnu::RectEdges &design, const WalkScale &s,
			uint32_t color, int32_t texture, bool tiled, float tile_u,
			float tile_v);
	void emit_state_texture(const mnu::RectEdges &design, const WalkScale &s,
			const StatePass &pass);
	void emit_state_pass(const mnu::RectEdges &design, const WalkScale &s,
			const StatePass &pass);
	void emit_outline(const mnu::RectEdges &design, const WalkScale &s,
			uint32_t color);
	void emit_appearance(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, int appearance_slot);
	void emit_frame(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s);
	void emit_glyph_run(const WidgetNode &node, const std::string &text,
			int design_x, int design_y, const WalkScale &s, uint32_t color,
			int caret);
	void emit_caret(hud::GameFont &gf, const std::string &text, float x,
			float y, const WalkScale &s, uint32_t color, int caret);
	void emit_widget_text(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, int color_state, const MenuWidgetState *ws,
			int caret, const std::string *override_text = nullptr);
	// The closed combo face: the popup row collection's selected row TEXT
	// (runtime rows win; else the same nested-wins/top-level-fallback set the
	// popup renders) [orig: CComboWnd_Render @ 0x65c05b..0x65c083 — the label
	// swapped to the embedded list's selected row].
	std::string combo_face_text(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	void emit_edit(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s, int visual,
			const MenuFrameState &frame, const MenuWidgetState *ws);
	void emit_multiline_edit(const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s, int visual,
			const MenuFrameState &frame, const MenuWidgetState *ws);
	void emit_wrapped_text(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, uint32_t color, const std::string &text,
			int first_visible_line, int caret);
	void emit_checkbox_label(const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s, int color_state,
			const MenuWidgetState *ws);
	void emit_item_cell(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, int color_state, const MenuWidgetState *ws);
	void emit_list_rows(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, const MenuWidgetState *ws);
	void emit_combo_popup(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, const MenuWidgetState *ws);
	// Combos with an open dropdown collected during the walk; their popups
	// emit AFTER the whole walk so the dropdown paints over later widgets
	// (the D-MNU-12 menu-top decision — retail's inline tree order visibly
	// renders popups on top via a still-unwalked mechanism).
	std::vector<int> deferred_popups_;
	bool resolve_scrollbar_rect(const WidgetNode &node, ScrollbarKind kind,
			const mnu::RectEdges &owner, int fallback_top,
			int fallback_height, int fallback_width,
			mnu::RectEdges *out) const;
	struct ScrollParts {
		mnu::RectEdges track{};
		mnu::RectEdges up{};
		mnu::RectEdges down{};
		mnu::RectEdges shuttle{};
		bool vertical = true;
		int extent = 0;
		int travel = 0;
		int shuttle_offset = 0;
		int range_min = 0;
		int range_max = 0;
	};
	bool solve_scroll_parts_(const WidgetNode &node, ScrollbarKind kind,
			const mnu::RectEdges &rect, int range_min, int range_max, int page,
			int value, ScrollParts *out) const;
	bool solve_scroll_for_widget_(int index, const MenuFrameState &state,
			ScrollParts *out) const;
	bool scroll_row_span_(int index, const MenuFrameState &state, int *rows,
			int *visible) const;
	// The embedded-scrollbar emit for a row-scrolling owner (List/Multi/
	// LanList/Table): gated on the SAME span the interaction path solves, so
	// a drawn scrollbar is always an interactive one.
	void emit_row_scrollbar_(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s,
			const MenuFrameState &state, const MenuWidgetState *ws);
	// The CScrollWnd interaction pump ahead of the claim walk (compiler
	// runtime state, like edit_scroll_): the shuttle drag capture and the
	// pressed-part latch until release
	// [orig: CScrollWnd_HandleEvent @ 0x64d050].
	struct ScrollPump {
		bool button_was_down = false;
		int captured_index = -1;  // shuttle drag capture owner
		int drag_anchor = 0;
		int latched_index = -1;   // arrow/track press owner until release
	};
	ScrollPump scroll_pump_;
	bool scroll_pump_mouse_(MenuFrameState &io_state, float mouse_x,
			float mouse_y, bool button_down, float scale_x, float scale_y,
			MouseClaim *claim, int restrict_index = -1);
	void emit_scrollbar(const WidgetNode &node, ScrollbarKind kind,
			const mnu::RectEdges &rect, const WalkScale &s,
			int range_min, int range_max, int page, int value,
			int track_state);
	void emit_spin_arrows(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s);
	void emit_table(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s,
			const MenuWidgetState *ws);
	void emit_marquee(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s,
			const MenuFrameState &frame, const MenuWidgetState *ws);
	void emit_cursor(const MenuFrameState &state);

	const mnu::Screen *screen_ = nullptr;
	const opennova::fnt::fnt_font_t *default_font_ = nullptr;
	std::map<std::string, std::string> style_vars_;
	std::map<std::string, std::string> text_lookup_;
	std::map<std::string, const opennova::fnt::fnt_font_t *> registered_fonts_;
	std::vector<std::string> texture_names_;
	std::vector<std::pair<int, int>> texture_sizes_;
	std::vector<std::string> font_names_;
	std::vector<const opennova::fnt::fnt_font_t *> fonts_;
	std::vector<WidgetNode> nodes_;
	int32_t screen_cursor_ = kMenuTexNone;
	std::map<int, EditScroll> edit_scroll_;
	std::map<int, MarqueeScroll> marquee_scroll_;
	MenuDrawList draw_list_;
};

} // namespace opennova::menu
