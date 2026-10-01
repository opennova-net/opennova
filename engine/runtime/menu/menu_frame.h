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
#include <runtime/menu/menu_credits.h>
#include <runtime/menu/menu_edit.h>
#include <runtime/menu/menu_table.h>
#include <runtime/menu/menu_table_row.h>
#include <runtime/menu/menu_text_tables.h>
#include <formats/mnu/mnu.h>
#include <formats/mnu/mnu_layout.h>

#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace opennova::menu {

// The runtime visual state written to elem+236 every frame by the input pump
// [orig: CWnd_ProcessMouseEvent @ 0x647a00, its availability fallback
// @ 0x647c89..0x647cb5]. The numeric values are the original's: they index the
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

// A design-space edge on the device: scaled and truncated to an int per element, the
// way every emitted coordinate is [orig: @ 0x647d40]. An embedder that draws over the
// picture (the editor's outlines) lands on the same pixels.
inline float menu_scaled_edge(int design, float scale) {
	return static_cast<float>(static_cast<int>(static_cast<double>(design) * scale));
}

// One draw-list quad. `texture` indexes the compiler's interned texture-name
// table (texture_names()); kMenuTexNone is an untextured color fill. A valid
// `texture2` asks the device leg for retail's two-stage frame material:
// 2 * texture * texture2, with the first texture's alpha masking the result.
// [orig: CUIElement_InitBorderMaterials @ 0x646f70 creates border_material from the
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
	// The custom-draw slot (MenuFrameState::custom_slot_index): the op index
	// its widget's CUSTOM appearance pass runs at, -1 when it did not draw.
	// Every op before it paints under the embedder's slot draw and every op
	// from it on above; it never precedes overlay_op_start (a slot before the
	// split pulls the split to it), so the embedder layers its slot item
	// between the overlay ops before and after it.
	// [orig: CUIElement_Draw @ 0x64a8a0 — the CUSTOM (&4) pass,
	//  CUIElement_DispatchCustomDrawEvent @ 0x647f10, after COLOR / IMAGE /
	//  OUTLINE and before the frame and the children]
	int32_t custom_slot_op = -1;
	int64_t widgets_drawn = 0;
};

// Per-widget per-frame state, keyed by the widget's pre-order index over the
// configured screen tree (0 = the root window; children in authored order).
// Unlisted widgets take the defaults. The fields mirror the original runtime
// widget fields: shown (+224), enabled (+228), the pump's hover/press verdict
// (+236) [orig: CWnd_ProcessMouseEvent @ 0x647a00], checked (+772),
// keyboard focus (g_UIFocusWnd @ 0x31C16D4), and the caret char index
// (+764) the edit render feeds the shared text draw
// [orig: CEditWnd_Render @ 0x6619e0].
struct MenuWidgetState {
	int32_t index = 0;
	bool hide = false;        // runtime WINDOW HIDE override
	bool show = false;        // runtime WINDOW SHOW override
	// The runtime enabled flag (+228) once code or an ACTION wrote it: it
	// replaces the authored DISABLE either way, so an ENABLE re-enables an
	// authored-disabled window [orig: UIWidget_SetInteractiveRecursive
	// @ 0x6462e0 writes +228]. Unwritten: the authored DISABLE.
	bool has_disabled = false;
	bool disabled = false;    // runtime disable (visual state 1)
	bool hovered = false;     // mouse over, button up (visual state 2)
	bool pressed = false;     // mouse held on the widget (visual state 3)
	// Which SPINUP (1) or SPINDOWN (2) arrow of a spin list the hover / press
	// is over (0: the list itself): the arrows are child buttons that claim the
	// mouse themselves [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0].
	int32_t spin_part = 0;
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
	// A runtime rect replacing the POSITION solve (CWnd_SetRect), relative to
	// the parent's origin.
	bool has_rect = false;
	mnu::RectEdges rect;
	// TABLE data rows (menu_table_row.h: the texts, cell values, row state,
	// flags and colour of the 40-byte row records) [orig: CUITable_Render
	// @ 0x6411d0]. scroll_row above is the first visible row.
	std::vector<MenuTableRow> table_rows;
	// The columns code installed (they replace the XML ones) and the sorted
	// column whose header shows the sort indicator (-1: none) [orig:
	// CTableWnd_SortByColumn @ 0x640900 -> CTableWnd_SetRowTooltip].
	bool has_table_columns = false;
	std::vector<MenuTableColumn> table_columns;
	int32_t table_sort_column = -1;
	// A clip rect (CWnd_SetClipRect, absolute design units): the widget's own
	// passes draw through the viewport it scales to; its children do not.
	// [orig: CWnd_SetClipRect @0x646210; CWnd_ApplyClipViewport @0x6472a0 /
	//  CWnd_RestoreViewport @0x6473f0 around CStaticWnd_Render's own passes
	//  @0x657b10]
	bool has_clip = false;
	mnu::RectEdges clip;
	// MARQUEE credits (what the widget's DATASOURCEs loaded, menu_credits.h).
	MarqueeCredits marquee;
	// Restart the roll from the initial layout on the next compile.
	bool marquee_reset = false;
};

struct MenuFrameState {
	std::vector<MenuWidgetState> widgets;
	// The open popup (a shown MODAL window) as a pre-order index, -1 none: while
	// one is open only its subtree takes the mouse [orig: g_UIOpenPopupWnd;
	// UI_DispatchMouseEvent @ 0x63ab00 and CUIScene_EndFrame @ 0x63e600 send the
	// mouse to the popup alone]. The runtime keeps it (MenuRuntime).
	int32_t popup_root = -1;
	// Milliseconds clock for the caret blink: the caret draws while
	// (time_ms & 0x3FF) > 0x200 [orig: CEditWnd_Render @ 0x661c63].
	uint32_t time_ms = 0;
	// The mouse cursor pass [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60]:
	// drawn LAST at the raw mouse position, native texture size, UNSCALED.
	bool cursor_visible = false;
	float cursor_x = 0.0f;
	float cursor_y = 0.0f;
	// The custom-draw widget an embedder mounts its own Control over (a map
	// window), -1 none: every op the walk emits after that widget's subtree
	// joins the menu-top overlay, so the later siblings still paint over the
	// custom draw like the retail walk [orig: the forward child walk —
	// CMapWindow_HandleEvent's pass runs inside MAP's draw, before
	// WAYPOINTNAME_DLG / USERWP_CLOSE paint].
	int32_t mount_index = -1;
	// The widget whose CUSTOM appearance pass the embedder draws into the
	// walk (a registered event-1 handler that draws no Control of its own,
	// like CMAP's CHAT_MSGS console), -1 none (MenuDrawList::custom_slot_op).
	int32_t custom_slot_index = -1;
};

// The table's custom-draw event (0x8000002) for a CUSTOM_DRAW column: the
// header cell (row -1, state 0, value 0) or a body cell (the row index, its
// state +24 and the column's cell value), with the cell's device rect — the
// design rect plus the ancestors' offsets, scaled.
// [orig: CUITable_Render @0x6413b3..0x641419 (header), @0x641730..0x6417bc
//  (body) — the payload {this, name, row, column, state, value, L, T, R, B}]
struct MenuTableCellEvent {
	int row = -1;
	int column = 0;
	int32_t state = 0;
	int32_t value = 0;
	float left = 0.0f, top = 0.0f, right = 0.0f, bottom = 0.0f;
};

// What a custom-draw handler may draw from inside the table's walk, in the
// walk's order: the table's own cell draw (flags 1 the row-state appearance
// pass, 2 the content), a device clear of a device rect, and a device line.
// [orig: CTableWnd_DrawCell @0x640be0; CGfxDevice_SetClearColor @0x677050 +
//  CGfxDevice_Clear @0x677100; CGfxDevice_SetQuadDiffuse @0x677060 +
//  sub_678850 @0x678850]
class MenuTableCellCanvas {
public:
	virtual ~MenuTableCellCanvas() = default;
	virtual void draw_cell(int row, int column, int flags) = 0;
	virtual void clear_rect(uint32_t argb, float left, float top, float right, float bottom) = 0;
	virtual void line(uint32_t argb, float x0, float y0, float x1, float y1) = 0;
};

// A table's registered custom-draw handler (the control callback bound on the
// 0x08 event class) [orig: CUIScene_RegisterControlCallback @0x63c060].
using MenuTableCellPainter =
		std::function<void(const MenuTableCellEvent &, MenuTableCellCanvas &)>;

// What a compile made of a screen where the picture may not be what the author meant: a
// row the parse does not use, a colour that does not read as written, a rect with no
// area, a label cut short, a file that did not load (ADR 0046 S9j2; the table and the
// witness per code: docs/mnu/menu-re.md "Compiler notes"). The engine keeps codes, never
// text: the editor words them. configure() records its own (build_notes), the loader adds
// the files that did not load (add_load_note), and layout_notes() reads a frame state
// without drawing; none of it changes what the compile emits.
enum class MenuFrameNoteCode : uint16_t {
	// configure
	AppearanceStateUnknown, // a STATE the parse does not know: the row sets nothing
	AppearanceTypeUnknown,  // a TYPE it does not know: the row only marks its state
	AppearanceCustom,       // CUSTOM: the shell's draw hook; the compile draws nothing for it
	AppearanceReplaced,     // a later row of the same state and type replaces this row's value
	ColorUnparsed,          // a COLOR, OUTLINE or FONT colour the hex read stops short in
	ColorTransparent,       // a COLOR or OUTLINE of fewer than eight digits: it reads alpha 0
	StyleVarUnresolved,     // a whole-value %VAR% the shell's list lacks: kept literal
	TypeUnknown,            // a TYPE token the factory does not match: a generic window
	TypeInteriorDeferred,   // RADIOEDIT: drawn as a generic window (D-MNU-13)
	ItemKindNotDrawn,       // a list or combo row of TYPE IMAGE or COLOR (D-MNU-5)
	TableCellsDeferred,     // a table's bitmap, custom or SUBST cells, IMAGEROW rows (D-MNU-13)
	ScrollExtentDefault,    // a SCROLL with no HEIGHT or WIDTH: its arrows are the default 20 long
	// the loader's
	FontMissing,         // the FONT's .fnt is not in the files
	FontUnreadable,      // it is, and does not parse
	TextureMissing,      // a texture the files lack
	TextureUnreadable,   // one they hold that does not decode, or of a kind the loader skips
	TextTableMissing,    // a TEXT_RSRC the files lack
	TextTableUnreadable, // one they hold that does not parse
	// layout
	RectEmpty,             // the solved rect has no area
	TextTruncated,         // the label drawn cut short to fit
	TextNoRoom,            // no room at all for the label
	TextNoFont,            // text, and no FONT up the chain loaded: neither measured nor drawn
	TextIdMissing,         // a string id no table the window reads defines: the id is shown
	ImageBandEmpty,        // an IMAGE band outside its texture: nothing drawn
	ImageHeightShared,     // an IMAGE row's HEIGHT unlike the texture's first load, which wins
	StateFallback,         // the state held has no APPEARANCE: DEFAULT (or nothing) draws
	CheckedNoArt,          // checked with no SELECTED appearance: no appearance draws
	FrameAbsent,           // DRAW_FRAME with no FRAME up the chain
	FrameStencilUnloaded,  // the FRAME's STENCIL names no texture that loaded
	FrameNoStencil,        // the FRAME names no STENCIL (a BRUSH alone): no frame is set up
	FrameTileZero,         // the stencil's tile size is 0
	SpinArrowEmpty,        // a SPINUP or SPINDOWN with no area: never drawn or clicked
	ListRowsClipped,       // more rows than fit, and no SCROLLBAR
	TableNoColumns,        // no HEADER sets a column up: the table draws no header or cell
	TableHeaderClipped,    // a column past the table's right edge: not drawn
	TableHeaderWidthZero,  // a HEADER that sets its column's width to 0: skipped, not drawn
	MarqueeRuntimeContent, // the credits a DATASOURCE loads are the embedder's (not rolled here)
};
inline constexpr int kMenuFrameNoteCodeCount = static_cast<int>(MenuFrameNoteCode::MarqueeRuntimeContent) + 1;

// Why a note holds: the game does this (Witnessed: cited where the compiler does it), the
// port's own choice with no witness (PortPolicy), or a known gap in the port, a
// divergence-ledger row (Deferred).
enum class MenuFrameNoteBasis : uint8_t { Witnessed, PortPolicy, Deferred };

struct MenuFrameNote {
	int widget = -1; // pre-order index (a spin arrow's is its list's); -1 = the screen
	MenuFrameNoteCode code = MenuFrameNoteCode::RectEmpty;
	std::string subject; // the token, name, value or drawn prefix the note is about
	// Where it sits: the window's list holding the record ("appearance", "items.item",
	// "spinup"; "" = the window itself), that record's index (-1 = none) and the field, by
	// element path (the element names down from the window, lowercase, joined by '.':
	// "position.right", "value"; mnu::WriteIssue::locator's names).
	std::string list;
	int record = -1;
	std::string field;
	bool operator==(const MenuFrameNote &o) const {
		return widget == o.widget && code == o.code && subject == o.subject && list == o.list &&
		       record == o.record && field == o.field;
	}
};

// "text_truncated": the code's stable token.
const char *menu_frame_note_token(MenuFrameNoteCode code);
MenuFrameNoteBasis menu_frame_note_basis(MenuFrameNoteCode code);
// "witnessed", "port_policy", "deferred".
const char *menu_frame_note_basis_token(MenuFrameNoteBasis basis);

// Deep in-process module: configure() walks the screen once (interning every
// texture and font name it will reference); compile() emits one frame's draw
// list in the witnessed walk order. Scale is the 800x600 anamorphic pair
// [orig: CUIScene_SetScreenScale @ 0x639480]; every scaled coordinate is
// truncated to int PER ELEMENT [orig: @ 0x647d40], and glyph runs are laid
// out at the same pair [orig: CFontCache_DrawTextScaled @ 0x653170
// forwards scaleX/scaleY into CGameFont_DrawText]. The edit scroll window
// (start/end per widget [orig: CEditWnd_UpdateScrollRange @ 0x661790]) is
// compiler runtime state and survives across compiles.
class MenuFrameCompiler {
public:
	// Out-of-line: WidgetNode is complete only in the .cpp.
	MenuFrameCompiler();
	~MenuFrameCompiler();
	MenuFrameCompiler(const MenuFrameCompiler &) = delete;
	MenuFrameCompiler &operator=(const MenuFrameCompiler &) = delete;

	// Borrow the screen (not owned; the caller keeps the document, the fonts and
	// the text tables alive). There is no default font: a widget draws with the
	// nearest self-or-ancestor FONT that loaded (register_font), else nothing
	// [orig: CWnd_GetFontAndColors @ 0x646a70].
	void configure(const mnu::Screen *screen);

	// The texture loads a document makes, in retail's parse order, for the
	// per-texture band height: retail's texture cache bakes the HEIGHT of an
	// IMAGE row's FIRST load of a texture (keyed by the name and the FLAGS) into
	// the texture, and every later row that finds it in the cache draws that
	// height's band [orig: CTextureManager_LoadOrFindTexture @ 0x654980 — the
	// stricmp name + subtype search, CEffect_BeginPassTraced's texFormat].
	// First load wins across calls (the cache lives for the menu session);
	// configure() notes its own screen as well. reset_texture_loads() forgets
	// them (a reload of every texture).
	void note_texture_loads(const mnu::Document &doc);
	void reset_texture_loads();

	// The flattened stylesheet (menu_style.mns evaluate output); %VAR% color
	// and texture values resolve through it, case-insensitive, unresolved
	// stays literal [orig: NapiXML_ExpandVariablesInText @ 0x63a000]. Set
	// before configure().
	void set_style_vars(const std::map<std::string, std::string> &vars);

	// The string tables String/Item/header type=="id" lookups read, each
	// widget through its own TEXT_RSRC else its root window's [orig:
	// CWnd_GetInheritedTextRsrc @ 0x646AB0]; a miss keeps the id literal.
	// Borrowed; set before configure().
	void set_text_tables(const MenuTextTables *tables);

	// Per-name font registration (case-insensitive, the FONT NAME as authored
	// after %VAR%): only a font that loaded is registered; an unregistered name
	// is one that did not load. Set before configure() (a marquee's node fonts
	// may register later); clear before re-registering when the backing storage
	// is reloaded (the compiler borrows the pointers).
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
	// (FontRun::font indexes this; slot 0 = no font, a compile may add slots).
	const std::vector<std::string> &texture_names() const {
		return texture_names_;
	}
	const std::vector<std::string> &font_names() const { return font_names_; }

	// Report a loaded texture's pixel size. The three-stage POSITION
	// fallback, native-size item images, the frame stencil atlas, the IMAGE
	// band, and the cursor consume these; an unreported texture did not load:
	// it measures 0 and draws nothing [orig: the parse-tail POSITION solve
	// @ 0x648120; CTextureManager_LoadOrFindTexture @ 0x654980 zeroes the
	// handle of a load that fails].
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
	// The pre-order index of the window a widget hangs under (-1: a root window, or out
	// of range).
	int widget_parent(int index) const;
	// The FONT a widget's text draws with, its own or the nearest ancestor's that loaded,
	// and that font's colours by visual state (kStateDefault ..), AARRGGBB; false when no
	// font up the chain loaded (the widget draws no text) [orig: CWnd_GetFontAndColors
	// @ 0x646a70].
	bool widget_font(int index, std::string *name, uint32_t colors[4]) const;
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
	// The widget's own solved rect, relative to its parent's origin (no
	// ancestor offsets) [orig: CWnd_GetRect @0x6465c0 — the rect at +0xD0].
	bool widget_local_rect(int index, const MenuFrameState &state,
			mnu::RectEdges *out) const;
	// The item-row count the draw uses (runtime rows when seeded, else the
	// authored <ITEM> rows; combo popups prefer the authored LIST_BOX rows).
	int item_count(int index, const MenuFrameState &state) const;
	// The text a list-like widget's row displays (runtime rows when seeded,
	// else the authored row after the string-table lookup).
	std::string item_display_text(int index, const MenuFrameState &state, int row) const;

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
	// Spin arrow under the mouse: 0 none, 1 up, 2 down — each arrow's own
	// solved rect, art or none [orig: CSpinListWnd_CreateUpDownChildren
	// @ 0x64b8b0 child rects; CWnd_HitTestPoint @ 0x646700 is PtInRect].
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
	// The table's hit test on the raw mouse [orig: CTableWnd_HitTest @ 0x63fe90;
	// UI_DispatchMouseEvent @ 0x63ab00 hands it the design point]: false where retail
	// fails (E_FAIL: a header click past the table's right edge, a row past the
	// last, no rows). Otherwise *row is the data row index (-1: the header strip, or
	// the point is outside the table) and *column the column whose span holds the
	// x (-1: none, so a table with no HEADER hits rows with column -1). The row
	// is the y below the header over the row pitch (the body row height once per
	// line the column widths wrap onto), clamped to the last visible row, mapped
	// past the scroll offset over the rows that are not hidden. The table's
	// embedded scrollbar strip is its child and takes the point first (row -1).
	bool table_hit(int index, const MenuFrameState &state, float mx, float my,
			float sx, float sy, int *row, int *column) const;
	// The table's COLUMN COUNT (1 without an authored count of 1 or more),
	// 0 for a widget that is not a table [orig: resize_column_count @0x63f6c0].
	int table_column_count(int index) const;
	// Bind (or clear, with an empty function) the custom-draw handler of the
	// table at `index`; kept across configure() while the index stays a table.
	void set_table_cell_painter(int index, MenuTableCellPainter painter);
	// Non-mutating front-most hit (the pump's claim walk without the state
	// writes) — editor/preview picking.
	int hit_widget(const MenuFrameState &state, float mx, float my, float sx,
			float sy) const;
	// The multiline edit's wrapped-line counts at scale 1.0 — the scroll
	// range twin [orig: CFontCache_CountWrappedLines @ 0x653b90 via
	// CMEditWnd_UpdateScrollRange @ 0x661180]: *fit = rows that fit the
	// widget rect, *total = wrapped line count; scroll range = [0,
	// total - fit]. False when the index is out of range.
	bool multiline_line_counts(int index, const MenuFrameState &state,
			int *fit_lines, int *total_lines) const;
	// The label mnemonic a widget's parse registers: the byte after the first
	// {hot} of its authored STRING as resolved at parse (the string table
	// looked up, not a runtime relabel), for the classes whose parse reads a
	// STRING (STATIC and every class built on it: BUTTON, EDIT, MULTILINE_EDIT,
	// RADIO, CHECKBOX, SPINLIST, LIST, LAN_LIST, TABLE, COMBOBOX, RADIOEDIT).
	// Empty: none [orig: CUIButtonWidget_ParseXMLAttributes @ 0x657c30 ->
	// CButtonWnd_SetLabel @ 0x6572F0 appends [char, 0] to the widget's
	// accelerator rows; the screen table copies them at registration,
	// CWnd_RegisterHotkeysRecursive @ 0x649d90, so a later relabel never
	// reaches the scan].
	std::string widget_mnemonic(int index) const;

	// --- notes (menu_frame_notes.cpp) ----------------------------------------
	// What configure() noted, the loader's notes after it; configure() starts them over.
	const std::vector<MenuFrameNote> &build_notes() const { return notes_; }
	// The loader's: a font, texture or string table (code FontMissing .. TextTableUnreadable)
	// that did not load, on every window naming it (the screen when none does).
	void add_load_note(MenuFrameNoteCode code, const std::string &name);
	// What drawing `state` would come to, at design scale, without drawing it: the rects,
	// the labels, the bands, the frames, the states held.
	std::vector<MenuFrameNote> layout_notes(const MenuFrameState &state) const;
	// A document widget's rect in its parent had its POSITION been `candidate`: the same
	// three-stage solve the draw uses. False when the index is out of range.
	bool solve_local_rect(int index, const mnu::Position &candidate, mnu::RectEdges *out) const;

	// The witnessed per-frame mouse pump [orig: CUIScene_EndFrame @ 0x63e600 ->
	// CWnd_ProcessMouseEvent @ 0x647a00 (vtable+20)]: ONE widget claims
	// the mouse per frame — front-most = last drawn (the reverse sibling walk
	// + the per-frame claim scene+16; equivalently the LAST hit of the
	// forward draw walk). A disabled claimant keeps visual state 1 (no
	// hover/press); hit + button down -> pressed (3); hit + button up ->
	// hovered (2); every other row's hover/press clears (0). Hidden subtrees
	// never hit. The mouse is RAW screen coordinates against the scaled
	// rects. The cursor rides back for the unscaled cursor pass: the claimed
	// widget's own CURSOR else its root window's (a spin arrow's own when the
	// claim is over one), counting only a cursor whose texture loaded, else
	// (nothing claimed or no cursor) the capturing scroll part owner's the same
	// way, else the first root window with one [orig: CWnd_ProcessMouseEvent
	// @ 0x647ad0..0x647b09; CUIScene_EndFrame @ 0x63e600].
	struct MouseClaim {
		int hovered = -1;               // claimed widget index; -1 = none
		int spin_part = 0;              // the claimed spin list's arrow (1 up, 2 down)
		int32_t cursor = kMenuTexNone;  // the cursor texture slot
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
	// One state's appearance record [orig: the 28-byte state records at elem+8,
	// CUIElement_ParseXMLDefinition @ 0x648120]: the rows of a state OR their
	// type flags and the last row of each type writes its value.
	struct StatePass {
		bool present = false;     // the availability bit [orig: or [edi+4] @ 0x648530]
		bool has_color = false;   // COLOR=1 -> the stretched fill [orig: entry+12]
		uint32_t color = 0;       // the wcstoul word as it stands (6 digits: alpha 0)
		bool has_image = false;   // IMAGE=2 [orig: the texture at entry+28]
		int32_t texture = kMenuTexNone;
		int32_t map_state = 0;    // MAP_STATE [orig: entry+32], 0 by default
		int32_t band_height = -1; // the texture's baked HEIGHT (-1: its own height)
		int image_row = -1;       // the IMAGE row that wrote it (its index in its list)
		int32_t row_height = -1;  // that row's own HEIGHT (-1: none authored)
		bool has_outline = false; // OUTLINE=8 [orig: entry+16]
		uint32_t outline = 0;
		// CUSTOM=4: the event-1 custom-draw hook; the compiler draws nothing for it
		// and marks the custom-draw slot [orig: the parse @ 0x64839c..0x6483ae;
		// CUIElement_DispatchCustomDrawEvent @ 0x647f10].
		bool custom = false;
	};
	struct EditScroll {
		int start = 0;
		int end = 0;
	};
	enum class ScrollbarKind { Standalone,
		Embedded,
		Popup };
	// Per-marquee roll state (compiler runtime state, like edit_scroll_): the
	// distance every node has scrolled and the last frame's clock [orig: node y
	// -= rate per rendered frame; the whole roll resets when the last node
	// passes the top — CMarqueeWnd_RenderScrollingCredits @ 0x65ca00].
	struct MarqueeScroll {
		double scrolled = 0.0;
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
	int build_node(const mnu::Window &w, int parent, int root, bool part);
	void resolve_node_(int index);
	// `list` names the rows for the notes: the window's list ("appearance"), or a part's
	// ("list_box", with `part_rows`: the rows sit inside that one part record).
	void build_state_passes(const std::vector<mnu::Appearance> &rows,
			StatePass (&states)[4], WidgetNode *image_rows_of = nullptr,
			const char *list = nullptr, bool part_rows = false);
	// The notes configure() makes (menu_frame_notes.cpp): one note on the node being
	// built (building_), a whole-value %VAR% the list lacks, a colour's hex read, the
	// window-level checks. The hooks are const: they read the compiler and write only the
	// note log (notes_, mutable), so a note cannot change what the compile emits.
	void note_(MenuFrameNoteCode code, const std::string &subject, const char *list, int record,
			const char *field) const;
	void note_var_(const std::string &value, const char *list, int record, const char *field) const;
	void note_color_(const std::string &value, bool keeps_alpha, const char *list, int record,
			const char *field) const;
	// One APPEARANCE row as build_state_passes reads it: a STATE or TYPE the parse does not
	// know, CUSTOM, a value a later row of its state and type replaces, its colour or
	// %VAR%.
	void note_appearance_row_(const std::vector<mnu::Appearance> &rows, size_t row,
			const char *list, bool part_rows) const;
	void note_window_(const mnu::Window &w) const;
	// A part node's notes (from `from` on) onto its spin list (the arrow's record in the
	// list's SPINUP or SPINDOWN), once the parts are built.
	void settle_part_notes_(size_t from = 0) const;
	void note_window_loads_(const mnu::Window &w);
	int32_t band_height_(const std::string &name, const std::string &flags) const;
	std::string resolve_var(const std::string &value) const;
	uint32_t resolve_text_color(const std::string &value) const;
	ResolvedText resolve_text_value(const std::string *table, const std::string &type,
			const std::string &raw) const;
	int32_t intern_texture(const std::string &name);
	// The slot of a FONT name that loaded (interned on first use), 0 when the
	// name did not load [orig: CFontCache_LoadOrGetFont @ 0x652f70 leaves the
	// handle 0 on a failed load].
	int32_t font_slot_(const std::string &name);
	bool texture_loaded_(int32_t slot) const;

	// compile-time walk
	int walk_widget(int index, int origin_x, int origin_y,
			const MenuFrameState &state, const WalkScale &s);
	int skip_widget(int index) const;
	// True when `index` is `root` or one of its descendants (the pre-order
	// subtree [root, skip_widget(root))); a root below 0 holds everything.
	bool in_subtree_(int index, int root) const;
	// The enabled flag the pump reads: the runtime's once written, else the
	// authored DISABLE.
	static bool disabled_(const mnu::Window &w, const MenuWidgetState *ws);
	int hit_walk(int index, int origin_x, int origin_y,
			const MenuFrameState &state, float mx, float my, float sx,
			float sy, int *io_hit, int *io_part) const;
	// hit_walk over every root window in draw order; with an open popup, over
	// the popup's subtree alone.
	void hit_roots_(const MenuFrameState &state, float mx, float my, float sx,
			float sy, int *io_hit, int *io_part) const;
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
	// One TABLE column as the XML sets it up, or as code installs it (a MenuTableColumn:
	// no SUBST rows, no cell offsets) [orig: the 180-byte column records at +780: width
	// +124, label +0, header justification +128 / +132, cell justification +144 / +148,
	// cell offsets +152 / +156, draw kind +108, the sort compare +112 and direction +120,
	// SCALE_BITMAP +168, the SUBST list +164].
	struct TableColumnSetup {
		int width = 0;
		std::string label;
		int header_justify = 0;
		int header_vjustify = 0;
		int body_justify = 0;
		int body_vjustify = 0;
		int body_x = 0;
		int body_y = 0;
		int cell_type = 0;
		bool numeric_sort = false;
		bool ascending = false;
		bool scale_bitmap = false;
		struct Subst {
			std::string value;
			int32_t texture = kMenuTexNone;
		};
		std::vector<Subst> subst;
	};
	// The draw kinds (+108): text, image (BITMAP_DRAW), custom (CUSTOM_DRAW),
	// image-else-text (BITMAP_TEXT).
	static constexpr int kTableCellText = 0;
	static constexpr int kTableCellImage = 1;
	static constexpr int kTableCellCustom = 2;
	static constexpr int kTableCellImageText = 4;
	class TableCanvas;
	void build_table_columns_(WidgetNode &node);
	// The columns a table draws: the ones code installed (they replace the XML ones),
	// else the XML set-up [orig: init_table_row @ 0x63f9c0 per HEADER, the BODY stores
	// @ 0x6427d0; StatScreen_PopulateStatResultsList @ 0x562240 installs its own].
	std::vector<TableColumnSetup> table_columns_(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	int32_t table_cell_image_(const std::vector<TableColumnSetup> &columns, int column,
			const std::string &text) const;
	static int table_pitch_(const std::vector<TableColumnSetup> &columns,
			const mnu::RectEdges &rect, int row_h);
	// Rows the body shows [orig: CTableWnd_RecalcLayout @ 0x63f1a0].
	int table_visible_rows_(const WidgetNode &node, const mnu::RectEdges &rect,
			const std::vector<TableColumnSetup> &columns) const;
	static int table_live_rows_(const MenuWidgetState *ws);
	static uint32_t table_row_color_(const WidgetNode &node, const MenuTableRow &row);
	// A cell's text aligned and drawn (CTableWnd_CalculateAlignedTextRect, then the
	// wrapped drawer's 0x20000 mode at the aligned corner plus the cell offset); returns
	// the aligned rect.
	mnu::RectEdges emit_table_text_(const WidgetNode &node, const std::string &text, int align,
			int dx, int dy, const mnu::RectEdges &cell, const WalkScale &s, uint32_t color);
	void emit_table_texture_(int32_t texture, int align, int dx, int dy, bool scale,
			const mnu::RectEdges &cell, const WalkScale &s);
	void emit_table_cell_(int index, const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, const MenuWidgetState *ws, int row, int column, int flags);
	// The sorted column's header taper [orig: CTableWnd_DrawRuleLine @ 0x6410a0].
	void emit_table_sort_rule_(const TableColumnSetup &column, const mnu::RectEdges &aligned,
			const WalkScale &s);
	// The clip viewport of a widget's own passes: every op from `first_op`
	// on cut to the scaled clip rect [orig: CWnd_ApplyClipViewport @ 0x6472a0].
	void clip_ops_(int32_t first_op, const mnu::RectEdges &clip, const WalkScale &s);
	bool widget_shown_(int index, const MenuFrameState &state) const;
	int pump_visual_state(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	int appearance_state_with_fallback(const WidgetNode &node,
			int state) const;
	mnu::RectEdges solve_rect(const WidgetNode &node) const;
	// The rect the walks use: a runtime CWnd_SetRect's, else solve_rect.
	mnu::RectEdges node_rect_(const WidgetNode &node, const MenuWidgetState *ws) const;
	// The same solve over another POSITION (solve_rect's is the window's own).
	mnu::RectEdges solve_rect_at(const WidgetNode &node, const mnu::Position &position) const;
	// The single-line label fit [orig: CStaticWnd_DrawLabel @ 0x656fb0 — the < 1
	// early-out, the full measure, the prefix-measure loop @ 0x657199..0x6571e4]: false
	// when the span is under 1 (nothing drawn); else the prefix drawn, its width and the
	// line height. The draw and the notes both read it.
	bool fit_label_(const WidgetNode &node, const std::string &text, int avail,
			std::string *drawn, int *drawn_w, int *line_h) const;
	// The frame draw's gate [orig: CUIElement_DrawFrame @ 0x64a210 — no inherited FRAME
	// (CWnd_FindInheritedFrameBlock @ 0x647190), no stencil that loaded, a zero tile:
	// nothing drawn]; the tile size out when it draws.
	enum class FrameGate : uint8_t { Draws, NoFrame, NoStencil, TileZero };
	FrameGate frame_gate_(const WidgetNode &node, int *tile) const;
	// Arrow hit over the spin list's ABSOLUTE rect (0 none / 1 up / 2 down) —
	// shared by spin_arrow_at and the claim walk's arrow claim
	// [orig: CSpinListWnd_CreateUpDownChildren @ 0x64b8b0; CWnd_HitTestPoint
	// @ 0x646700].
	int spin_arrow_hit_(const WidgetNode &node, const mnu::RectEdges &rect,
			const MenuFrameState &state, float mx, float my, float sx,
			float sy) const;
	ResolvedText resolved_widget_text(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	std::string widget_text(const WidgetNode &node,
			const MenuWidgetState *ws) const;
	const opennova::fnt::fnt_font_t *font_for(const WidgetNode &node) const;
	const opennova::fnt::fnt_font_t *font_at_(int32_t slot) const;
	void measure_text(const WidgetNode &node, const std::string &text,
			int *out_w, int *out_h) const;
	void measure_with_(const opennova::fnt::fnt_font_t *font,
			const std::string &text, int *out_w, int *out_h) const;
	// A widget's cursor by the handles that loaded: its own, else its root
	// window's (kMenuTexNone: neither loaded).
	int32_t inherited_cursor_(int index) const;
	// The cursor a claim shows: the hovered widget's (the arrow's when the
	// claim is over a spin arrow), then the scroll capture's, then the first
	// root's that loaded one.
	int32_t claim_cursor_(int hovered, int spin_part) const;
	int32_t first_root_cursor_() const;

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
	void emit_glyph_run_with_(int32_t font_slot, const std::string &text,
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
	// The wrapped-text drawer: `discard_rest` is its 0x20000 mode (an overflow
	// breaks the line and the rest of the source line to the next LF is
	// dropped, no bottom clip: the table cells); else the 0x40000 mode (the
	// wrap with the bottom clip: the multiline edit).
	void emit_wrapped_text(const WidgetNode &node, const mnu::RectEdges &rect,
			const WalkScale &s, uint32_t color, const std::string &text,
			int first_visible_line, int caret, bool discard_rest = false);
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
	// The draw-op count when the mount widget's subtree closed (-1 not yet).
	int32_t mount_split_ = -1;
	// The custom-draw slot widget's CUSTOM pass point.
	void mark_custom_slot_(int index, const WidgetNode &node, int appearance_slot,
			const MenuFrameState &state);
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
	void emit_table(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s,
			const MenuWidgetState *ws);
	void emit_marquee(int index, const WidgetNode &node,
			const mnu::RectEdges &rect, const WalkScale &s,
			const MenuFrameState &frame, const MenuWidgetState *ws);
	void emit_cursor(const MenuFrameState &state);

	const mnu::Screen *screen_ = nullptr;
	std::map<std::string, std::string> style_vars_;
	const MenuTextTables *text_tables_ = nullptr;
	std::map<std::string, const opennova::fnt::fnt_font_t *> registered_fonts_;
	// The first-load HEIGHT per texture key (name + FLAGS, lowercased); -1 is a
	// load with no HEIGHT (the texture's own height).
	std::map<std::string, int32_t> texture_loads_;
	std::vector<std::string> texture_names_;
	std::vector<std::pair<int, int>> texture_sizes_;
	std::vector<std::string> font_names_;
	std::vector<const opennova::fnt::fnt_font_t *> fonts_;
	std::vector<WidgetNode> nodes_;
	// nodes_[0, document_nodes_) is the document's pre-order index space; the
	// spin arrow parts follow it.
	int document_nodes_ = 0;
	std::map<int, EditScroll> edit_scroll_;
	std::map<int, MarqueeScroll> marquee_scroll_;
	std::map<int, MenuTableCellPainter> table_painters_;
	MenuDrawList draw_list_;
	// The node configure() is building (the owner of a note_), and what it noted: the
	// log is the one thing the const note hooks write.
	int building_ = -1;
	mutable std::vector<MenuFrameNote> notes_;
};

} // namespace opennova::menu
