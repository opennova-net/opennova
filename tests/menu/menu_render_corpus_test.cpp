// The shipped menus through the frame compiler, in three legs, each screen compiled
// with its own assets read the way the game reads them: the TEXT_RSRC tables (section
// "menu"), the FONT files by retail's name rule, the textures by retail's extension
// dispatch, and the stylesheet's %VAR%s. A table of what each screen drew and what did
// not load is printed.
//  1. Every screen of the fifteen revx02 JO menus of the reference fixture set
//     configures and compiles (the whole test is gated on them; no assets).
//  2. The 2026-09-23 grill's census corpus, the .mnu files loose at the extracted
//     tree's root (a SKIP-LEG leg): every screen compiles, and the grill's shipped-case
//     counts are the oracle. The 134 statics with no MOUSEOVER, SELECTED or DISABLED
//     row keep their default label colour under the mouse; the SHADOWQUALITY and
//     RED_PW bands are the HEIGHT their texture's first load baked (20 and 24, not
//     their own 24 and 2); the 68 spin arrows draw each of their four state rows; and
//     the 216 typeless APPEARANCE rows (and the one unknown TYPE) mark their state and
//     draw nothing.
//  3. Every .mnu the packed install serves (a SKIP-LEG leg, base mount and each
//     expansion) compiles.
// [orig: CUIScene_DrawScreensAndCursor @ 0x63bf60; CUIStringTable_LookupString
//  @ 0x6527c0; CFontCache_LoadOrGetFont @ 0x652f70; CTextureManager_LoadOrFindTexture
//  @ 0x654980; CWnd_ProcessMouseEvent @ 0x647a00 (the state fallback
//  @ 0x647c89..0x647cb5); Render_DrawTiledTextureStrip @ 0x67aed0;
//  CUISpinList_ParseXMLDefinition @ 0x64bd10; CUIElement_ParseXMLDefinition @ 0x648120]
// Witness record: docs/mnu/menu-re.md.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <base/vfs/vfs.h>
#include <formats/dds/dds.h>
#include <formats/mnu/mnu.h>
#include <formats/mns/mns_document.h>
#include <formats/pcx/pcx_io.h>
#include <formats/rtxt/rtxt.h>
#include <formats/tga/tga.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>

#include "common/file_io.h"
#include "common/retail_paths.h"

using opennova::menu::MenuDrawList;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuFrameState;
using opennova::menu::MenuQuad;
using opennova::menu::MenuWidgetState;
using opennova::mnu::Appearance;
using opennova::mnu::Window;
using opennova::strutil::iequals;

namespace {

uint32_t be32(const std::vector<uint8_t> &b, size_t at) {
	return at + 3 < b.size() ? (static_cast<uint32_t>(b[at]) << 24) | (static_cast<uint32_t>(b[at + 1]) << 16) |
					(static_cast<uint32_t>(b[at + 2]) << 8) | b[at + 3]
							 : 0u;
}

// A texture's size from its header, by the format the dispatch picked: the TGA and DDS
// header readers, and a PCX through the port of the game's menu decoder.
bool texture_size(opennova::menu::MenuTextureFormat format, const std::vector<uint8_t> &b, int *w, int *h) {
	using F = opennova::menu::MenuTextureFormat;
	uint32_t width = 0, height = 0;
	switch (format) {
		case F::Tga:
			if (!opennova::tga::tga_header_size(b.data(), b.size(), width, height)) return false;
			break;
		case F::Dds:
			if (!opennova::dds::dds_header_size(b.data(), b.size(), width, height)) return false;
			break;
		case F::Pcx: {
			opennova::RgbaImage image;
			std::string error;
			if (!opennova::decode_pcx_menu_rgba(b.data(), b.size(), image, error)) return false;
			width = static_cast<uint32_t>(image.width);
			height = static_cast<uint32_t>(image.height);
			break;
		}
		case F::Png:
			width = be32(b, 16);
			height = be32(b, 20);
			break;
		case F::None:
			return false;
	}
	*w = static_cast<int>(width);
	*h = static_cast<int>(height);
	return *w > 0 && *h > 0;
}

// Where a screen's files come from: the mount, the loose tree, or nothing.
struct AssetSource {
	std::function<bool(const std::string &, std::vector<uint8_t> &)> read;
	std::function<bool(const std::string &)> has;
};

// Every window of a tree, its parts included.
void walk_windows(const Window &w, const std::function<void(const Window &)> &visit) {
	visit(w);
	for (const opennova::mnu::WindowPart *part : { &w.list_box, &w.spinup, &w.spindown, &w.scrollbar })
		if (part->present()) walk_windows(**part, visit);
	for (const Window &child : w.children) walk_windows(child, visit);
}
void walk_windows(Window &w, const std::function<void(Window &)> &visit) {
	visit(w);
	for (opennova::mnu::WindowPart *part : { &w.list_box, &w.spinup, &w.spindown, &w.scrollbar })
		if (part->present()) walk_windows(**part, visit);
	for (Window &child : w.children) walk_windows(child, visit);
}

// An AssetSource as the engine's file source: a name that resolves has stamp 1.
class AssetFiles : public opennova::FileSource {
public:
	explicit AssetFiles(const AssetSource *assets) : assets_(assets) {}
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		return assets_ != nullptr && assets_->read(name, out);
	}
	uint64_t stamp(const std::string &name) const override { return assets_ != nullptr && assets_->has(name) ? 1 : 0; }

private:
	const AssetSource *assets_;
};

// A texture's size from its header: what a headless check decodes.
class HeaderSizes : public opennova::menu::MenuTextureDecoder {
public:
	bool decode(const std::string &key, opennova::menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes,
			int &width, int &height) override {
		if (!texture_size(format, bytes, &width, &height)) return false;
		heights[key] = height;
		return true;
	}
	void release(const std::string &key) override { heights.erase(key); }
	std::map<std::string, int> heights;
};

// One screen's compiler with everything the screen reads loaded through the engine's own
// loaders (menu_screen_inputs.h, menu_frame_assets.h). Built in place: the compiler
// borrows the tables and the fonts.
class LoadedScreen {
public:
	LoadedScreen(const opennova::mnu::Document &doc, const opennova::mnu::Screen &screen,
			const AssetSource *assets, const std::map<std::string, std::string> &vars) :
			files_(assets) {
		assets_missing = assets_.configure(compiler, &doc, &screen, files_, decoder_, vars);
		tables_missing = static_cast<int>(assets_.missing_tables().size());
	}
	~LoadedScreen() { assets_.clear(compiler, decoder_); }
	LoadedScreen(const LoadedScreen &) = delete;
	LoadedScreen &operator=(const LoadedScreen &) = delete;

	// A texture's slot by name (case-insensitive), -1 when the screen names none.
	int32_t slot(const std::string &name) const {
		const std::vector<std::string> &textures = compiler.texture_names();
		for (size_t i = 0; i < textures.size(); ++i)
			if (iequals(textures[i], name)) return static_cast<int32_t>(i);
		return -1;
	}
	int height(int32_t slot) const {
		const std::vector<std::string> &keys = assets_.slot_keys();
		if (slot < 0 || static_cast<size_t>(slot) >= keys.size()) return 0;
		const auto found = decoder_.heights.find(keys[static_cast<size_t>(slot)]);
		return found != decoder_.heights.end() ? found->second : 0;
	}

private:
	AssetFiles files_;
	HeaderSizes decoder_;
	opennova::menu::MenuFrameAssets assets_;

public:
	MenuFrameCompiler compiler;
	int tables_missing = 0;
	int assets_missing = 0; // fonts and textures
};

struct Totals {
	int screens = 0;
	int failures = 0;
};

// One document's screens: each compiles, and a line of what it drew is printed.
void compile_document(const std::string &label, const opennova::mnu::Document &doc,
		const AssetSource *assets, const std::map<std::string, std::string> &vars, Totals &totals) {
	for (const opennova::mnu::Screen &screen : doc.screens) {
		++totals.screens;
		LoadedScreen loaded(doc, screen, assets, vars);
		MenuFrameState state;
		const MenuDrawList &list = loaded.compiler.compile(state, 1.0f, 1.0f);
		const bool ok = loaded.compiler.widget_count() > 0 && list.widgets_drawn > 0;
		std::printf("  %-34s %-18s widgets %4d drawn %4lld quads %5zu glyphs %6zu | not loaded: tables %d fonts and textures %d%s\n",
				label.c_str(), screen.name.c_str(), loaded.compiler.widget_count(),
				static_cast<long long>(list.widgets_drawn), list.quads.size(), list.glyphs.size(),
				loaded.tables_missing, loaded.assets_missing, ok ? "" : "  FAIL");
		if (!ok) ++totals.failures;
	}
}

std::map<std::string, std::string> style_vars(const AssetSource &assets) {
	std::map<std::string, std::string> vars;
	std::vector<uint8_t> style;
	if (assets.read("menu_style.mns", style)) {
		const opennova::mns::EvaluationResult evaluated = opennova::mns::Document::parse(
				reinterpret_cast<const char *>(style.data()), style.size()).evaluate();
		for (const auto &kv : evaluated.sheet.variables) vars[kv.first] = kv.second;
	}
	return vars;
}

// Every .mnu one mount layer of the packed install serves.
void sweep_mount(const std::string &install, const std::string &expansion, Totals &totals, int &menus) {
	opennova::Vfs vfs;
	if (!vfs.mount_game(install, expansion, opennova::VfsMountMode::Packed)) {
		std::printf("  FAIL mount_game(%s, %s): %s\n", install.c_str(), expansion.c_str(), vfs.last_error().c_str());
		++totals.failures;
		return;
	}
	AssetSource assets;
	assets.read = [&vfs](const std::string &name, std::vector<uint8_t> &out) { return vfs.read_file(name, out); };
	assets.has = [&vfs](const std::string &name) { return vfs.has_file(name); };
	const std::map<std::string, std::string> vars = style_vars(assets);
	for (const auto &loc : vfs.list_files()) {
		const std::string &name = loc.logical_name;
		if (name.size() < 4 || retail::lower_ascii(name.substr(name.size() - 4)) != ".mnu") continue;
		std::vector<uint8_t> bytes;
		opennova::mnu::Document doc;
		std::string error;
		if (!vfs.read_file_raw(name, bytes) || !opennova::mnu::parse(bytes.data(), bytes.size(), doc, error)) {
			std::printf("  FAIL %s (%s)\n", name.c_str(), error.c_str());
			++totals.failures;
			continue;
		}
		++menus;
		compile_document((expansion.empty() ? std::string() : expansion + "/") + name, doc, &assets, vars, totals);
	}
}

// ---- the grill's census ------------------------------------------------------

// The screen's document windows in the compiler's pre-order index space (roots in
// document order, each window then its children; the parts are outside it).
struct Indexed {
	const Window *window = nullptr;
	int parent = -1;
};
void index_tree(const Window &w, int parent, std::vector<Indexed> &out) {
	const int self = static_cast<int>(out.size());
	out.push_back({ &w, parent });
	for (const Window &child : w.children) index_tree(child, self, out);
}
std::vector<Indexed> index_screen(const opennova::mnu::Screen &screen) {
	std::vector<Indexed> out;
	for (const Window &root : screen.roots) index_tree(root, -1, out);
	return out;
}
// The same windows of a copy, mutable, in the same order.
void mutable_tree(Window &w, std::vector<Window *> &out) {
	out.push_back(&w);
	for (Window &child : w.children) mutable_tree(child, out);
}
std::vector<Window *> mutable_screen(opennova::mnu::Screen &screen) {
	std::vector<Window *> out;
	for (Window &root : screen.roots) mutable_tree(root, out);
	return out;
}

MenuWidgetState &row_for(MenuFrameState &state, int index) {
	for (MenuWidgetState &row : state.widgets)
		if (row.index == index) return row;
	MenuWidgetState row;
	row.index = index;
	state.widgets.push_back(row);
	return state.widgets.back();
}

// Shown, with every ancestor: a widget on a tab the screen opens hidden draws too.
void show_chain(MenuFrameState &state, const std::vector<Indexed> &tree, int index) {
	for (int i = index; i >= 0; i = tree[static_cast<size_t>(i)].parent) row_for(state, i).show = true;
}

bool has_state_row(const std::vector<Appearance> &rows, const char *state) {
	for (const Appearance &row : rows)
		if (iequals(row.state, state)) return true;
	return false;
}

bool drawing_type(const std::string &type) {
	return iequals(type, "image") || iequals(type, "color") || iequals(type, "outline") || iequals(type, "custom");
}

// The last IMAGE row of a state, or null.
const Appearance *image_row(const std::vector<Appearance> &rows, const char *state) {
	const Appearance *found = nullptr;
	for (const Appearance &row : rows)
		if (iequals(row.state, state) && iequals(row.type, "image")) found = &row;
	return found;
}

// The quad drawn with `slot` whose top-left corner is (x, y), or null.
const MenuQuad *quad_at(const MenuDrawList &list, int32_t slot, int x, int y) {
	for (const MenuQuad &q : list.quads)
		if (slot >= 0 && q.texture == slot && q.x0 == static_cast<float>(x) && q.y0 == static_cast<float>(y)) return &q;
	return nullptr;
}

// The quad's texel band [top, bottom) in a texture `height` texels tall.
bool band_is(const MenuQuad *q, int height, int top, int bottom) {
	return q != nullptr && height > 0 && std::lround(q->v0 * static_cast<float>(height)) == top &&
			std::lround(q->v1 * static_cast<float>(height)) == bottom;
}

bool same_draw(const MenuDrawList &a, const MenuDrawList &b) {
	if (a.quads.size() != b.quads.size() || a.lines.size() != b.lines.size() || a.glyphs.size() != b.glyphs.size())
		return false;
	for (size_t i = 0; i < a.quads.size(); ++i) {
		const MenuQuad &p = a.quads[i];
		const MenuQuad &q = b.quads[i];
		if (p.x0 != q.x0 || p.y0 != q.y0 || p.x1 != q.x1 || p.y1 != q.y1 || p.u0 != q.u0 || p.v0 != q.v0 ||
				p.u1 != q.u1 || p.v1 != q.v1 || p.color != q.color || p.texture != q.texture ||
				p.texture2 != q.texture2)
			return false;
	}
	for (size_t i = 0; i < a.lines.size(); ++i) {
		const opennova::menu::MenuLine &p = a.lines[i];
		const opennova::menu::MenuLine &q = b.lines[i];
		if (p.x0 != q.x0 || p.y0 != q.y0 || p.x1 != q.x1 || p.y1 != q.y1 || p.color != q.color) return false;
	}
	for (size_t i = 0; i < a.glyphs.size(); ++i) {
		const auto &p = a.glyphs[i];
		const auto &q = b.glyphs[i];
		if (p.x_top_left != q.x_top_left || p.y_top != q.y_top || p.color != q.color) return false;
	}
	return true;
}

std::vector<uint32_t> glyph_colors(const MenuDrawList &list) {
	std::vector<uint32_t> out;
	out.reserve(list.glyphs.size());
	for (const auto &g : list.glyphs) out.push_back(g.color);
	return out;
}

struct Census {
	int menus = 0;
	int screens = 0;
	int quiet_statics = 0;     // statics with no MOUSEOVER, SELECTED or DISABLED row
	int quiet_recoloured = 0;  // of them, one whose label changed colour under the mouse
	int quiet_would_turn = 0;  // of them, one whose label follows a MOUSEOVER row once authored
	int arrows = 0;
	int arrows_all_states = 0; // arrows that drew all four of their state rows
	int typeless = 0;          // APPEARANCE rows with no TYPE
	int unknown_type = 0;      // APPEARANCE rows with a TYPE the parse does not know
	int marker_widgets = 0;    // document widgets put in a state only markers author
	int marker_draw_diffs = 0; // compiles where a marker drew something
	bool shadowquality = false;
	bool red_pw = false;
	int failures = 0;
};

// The statics (the shipped red-hover case): under the mouse, a static with no
// MOUSEOVER row resolves to DEFAULT and keeps the default label colour; the same
// static with a MOUSEOVER row authored takes the hover colour.
void census_statics(const opennova::mnu::Document &doc, size_t screen_at, const AssetSource &assets,
		const std::map<std::string, std::string> &vars, Census &census) {
	const opennova::mnu::Screen &screen = doc.screens[screen_at];
	const std::vector<Indexed> tree = index_screen(screen);
	std::vector<int> quiet;
	for (size_t i = 0; i < tree.size(); ++i) {
		const Window &w = *tree[i].window;
		if (w.type == opennova::mnu::WindowType::Static && !has_state_row(w.appearances, "mouseover") &&
				!has_state_row(w.appearances, "selected") && !has_state_row(w.appearances, "disabled"))
			quiet.push_back(static_cast<int>(i));
	}
	if (quiet.empty()) return;
	census.quiet_statics += static_cast<int>(quiet.size());
	opennova::mnu::Document marked = doc;
	const std::vector<Window *> marked_tree = mutable_screen(marked.screens[screen_at]);
	for (const int i : quiet) {
		Appearance marker;
		marker.state = "MOUSEOVER";
		marked_tree[static_cast<size_t>(i)]->appearances.push_back(marker);
	}
	LoadedScreen base(doc, screen, &assets, vars);
	LoadedScreen alt(marked, marked.screens[screen_at], &assets, vars);
	MenuFrameState shown;
	for (const int i : quiet) show_chain(shown, tree, i);
	const std::vector<uint32_t> at_rest = glyph_colors(base.compiler.compile(shown, 1.0f, 1.0f));
	for (const int i : quiet) {
		MenuFrameState hovered = shown;
		row_for(hovered, i).hovered = true;
		if (glyph_colors(base.compiler.compile(hovered, 1.0f, 1.0f)) != at_rest) ++census.quiet_recoloured;
		if (glyph_colors(alt.compiler.compile(hovered, 1.0f, 1.0f)) != at_rest) ++census.quiet_would_turn;
	}
}

// The two shipped rows whose own HEIGHT differs from the one their texture's first
// load baked: options.mnu SHADOWQUALITY (24 on btn1e.tga, loaded first at 20) and
// mp.mnu RED_PW's DISABLED row (2 on btn3.tga, loaded first at 24).
void census_bands(const opennova::mnu::Document &doc, size_t screen_at, const AssetSource &assets,
		const std::map<std::string, std::string> &vars, Census &census) {
	struct Case {
		const char *widget;
		const char *texture;
		int band;
		bool *seen;
	};
	const Case cases[] = {
		{ "SHADOWQUALITY", "btn1e.tga", 20, &census.shadowquality },
		{ "RED_PW", "btn3.tga", 24, &census.red_pw },
	};
	const opennova::mnu::Screen &screen = doc.screens[screen_at];
	const std::vector<Indexed> tree = index_screen(screen);
	for (const Case &c : cases) {
		int index = -1;
		for (size_t i = 0; i < tree.size(); ++i)
			if (tree[i].window->name == c.widget) index = static_cast<int>(i);
		if (index < 0) continue;
		LoadedScreen loaded(doc, screen, &assets, vars);
		const int32_t slot = loaded.slot(c.texture);
		const int height = loaded.height(slot);
		MenuFrameState state;
		show_chain(state, tree, index);
		opennova::mnu::RectEdges rect;
		loaded.compiler.widget_rect(index, state, &rect);
		const bool rest = band_is(quad_at(loaded.compiler.compile(state, 1.0f, 1.0f), slot, rect.left, rect.top),
				height, 0, c.band);
		row_for(state, index).has_disabled = true;
		row_for(state, index).disabled = true;
		const bool disabled = band_is(quad_at(loaded.compiler.compile(state, 1.0f, 1.0f), slot, rect.left, rect.top),
				height, 3 * c.band, 4 * c.band);
		std::printf("  %-14s %s (%d texels tall): DEFAULT band [0, %d) %s, DISABLED band [%d, %d) %s\n", c.widget,
				c.texture, height, c.band, rest ? "ok" : "WRONG", 3 * c.band, 4 * c.band, disabled ? "ok" : "WRONG");
		if (rest && disabled) *c.seen = true;
	}
}

// The spin arrows: whole buttons that draw the IMAGE row of the state they are in.
void census_arrows(const opennova::mnu::Document &doc, size_t screen_at, const AssetSource &assets,
		const std::map<std::string, std::string> &vars, Census &census) {
	const opennova::mnu::Screen &screen = doc.screens[screen_at];
	const std::vector<Indexed> tree = index_screen(screen);
	std::vector<int> lists;
	for (size_t i = 0; i < tree.size(); ++i) {
		const Window &w = *tree[i].window;
		if (w.type == opennova::mnu::WindowType::SpinList && w.spinup.present() && w.spindown.present())
			lists.push_back(static_cast<int>(i));
	}
	if (lists.empty()) return;
	opennova::mnu::Document disabled_doc = doc;
	for (Window *w : mutable_screen(disabled_doc.screens[screen_at])) {
		if (w->spinup.present()) w->spinup->disabled = true;
		if (w->spindown.present()) w->spindown->disabled = true;
	}
	LoadedScreen base(doc, screen, &assets, vars);
	LoadedScreen off(disabled_doc, disabled_doc.screens[screen_at], &assets, vars);
	MenuFrameState shown;
	for (const int i : lists) show_chain(shown, tree, i);
	// arrow state -> how the frame state puts every list's arrow `kind` in it.
	const auto draw = [&](LoadedScreen &loaded, int kind, int visual) -> const MenuDrawList & {
		MenuFrameState state = shown;
		for (const int i : lists) {
			MenuWidgetState &row = row_for(state, i);
			row.spin_part = visual == opennova::menu::kStateDefault ? 0 : kind;
			row.hovered = visual == opennova::menu::kStateMouseover;
			row.pressed = visual == opennova::menu::kStateSelected;
		}
		return loaded.compiler.compile(state, 1.0f, 1.0f);
	};
	static const char *const kStates[] = { "default", "disabled", "mouseover", "selected" };
	std::map<std::pair<int, int>, int> drawn; // (list, kind) -> states drawn
	for (int kind = 1; kind <= 2; ++kind) {
		for (int visual = 0; visual < 4; ++visual) {
			LoadedScreen &loaded = visual == opennova::menu::kStateDisabled ? off : base;
			const MenuDrawList &list = draw(loaded, kind, visual);
			for (const int i : lists) {
				const Window &spin = *tree[static_cast<size_t>(i)].window;
				const Window &arrow = kind == 1 ? *spin.spinup : *spin.spindown;
				const Appearance *row = image_row(arrow.appearances, kStates[visual]);
				opennova::mnu::RectEdges rect;
				loaded.compiler.widget_rect(i, shown, &rect);
				const int x = rect.left + (arrow.position.has_left ? arrow.position.left : 0);
				const int y = rect.top + (arrow.position.has_top ? arrow.position.top : 0);
				const int32_t slot = row != nullptr ? loaded.slot(row->value) : -1;
				const int band = row != nullptr && row->has_height ? row->height : loaded.height(slot);
				const int top = row != nullptr && row->has_map_state ? row->map_state * band : 0;
				if (row != nullptr && band_is(quad_at(list, slot, x, y), loaded.height(slot), top, top + band))
					++drawn[{ i, kind }];
			}
		}
	}
	for (const int i : lists) {
		for (int kind = 1; kind <= 2; ++kind) {
			++census.arrows;
			if (drawn[{ i, kind }] == 4) ++census.arrows_all_states;
		}
	}
}

// The typeless rows: each marks its state present and draws nothing, so retyping
// every one as CUSTOM (a type the compiler draws nothing for) changes no draw, with
// every widget whose state only markers author put in that state.
void census_markers(const opennova::mnu::Document &doc, size_t screen_at, const AssetSource &assets,
		const std::map<std::string, std::string> &vars, Census &census) {
	const opennova::mnu::Screen &screen = doc.screens[screen_at];
	opennova::mnu::Document custom = doc;
	for (Window &root : custom.screens[screen_at].roots) {
		walk_windows(root, [](Window &w) {
			for (std::vector<Appearance> *rows : { &w.appearances, &w.items.appearances, &w.shuttle, &w.scrollup,
						 &w.scrolldown })
				for (Appearance &row : *rows)
					if (!drawing_type(row.type)) row.type = "CUSTOM";
		});
	}
	const std::vector<Indexed> tree = index_screen(screen);
	LoadedScreen base(doc, screen, &assets, vars);
	LoadedScreen alt(custom, custom.screens[screen_at], &assets, vars);
	if (base.compiler.texture_names() != alt.compiler.texture_names()) ++census.marker_draw_diffs;
	static const char *const kStates[] = { "default", "disabled", "mouseover", "selected" };
	for (int visual = 0; visual < 4; ++visual) {
		MenuFrameState state;
		int widgets = 0;
		for (size_t i = 0; i < tree.size(); ++i) {
			const std::vector<Appearance> &rows = tree[i].window->appearances;
			bool marker = false;
			bool typed = false;
			for (const Appearance &row : rows) {
				if (!iequals(row.state, kStates[visual])) continue;
				(drawing_type(row.type) ? typed : marker) = true;
			}
			if (!marker || typed) continue;
			++widgets;
			show_chain(state, tree, static_cast<int>(i));
			MenuWidgetState &row = row_for(state, static_cast<int>(i));
			row.disabled = visual == opennova::menu::kStateDisabled;
			row.hovered = visual == opennova::menu::kStateMouseover;
			row.pressed = visual == opennova::menu::kStateSelected;
		}
		census.marker_widgets += widgets;
		if (!same_draw(base.compiler.compile(state, 1.0f, 1.0f), alt.compiler.compile(state, 1.0f, 1.0f)))
			++census.marker_draw_diffs;
	}
}

// The grill's census corpus: the .mnu files at the extracted tree's root, read with
// the files beside them (the loose tree, case-insensitive).
int census_leg(Totals &totals) {
	const std::string root = retail::assets();
	std::map<std::string, std::string> files; // lowercased name -> path
	std::vector<std::string> menus;
	std::error_code ec;
	if (!root.empty()) {
		for (const auto &entry : std::filesystem::directory_iterator(root, ec)) {
			if (!entry.is_regular_file(ec)) continue;
			const std::string name = entry.path().filename().string();
			files[retail::lower_ascii(name)] = entry.path().generic_string();
			if (name.size() > 4 && retail::lower_ascii(name.substr(name.size() - 4)) == ".mnu")
				menus.push_back(entry.path().generic_string());
		}
	}
	if (menus.empty()) return retail::skip_leg("OPENNOVA_JO_ASSETS/*.mnu (the grill's census corpus)");
	std::sort(menus.begin(), menus.end());
	// Read once: the census builds each screen's compiler several times.
	std::map<std::string, std::vector<uint8_t>> cache;
	AssetSource assets;
	assets.read = [&files, &cache](const std::string &name, std::vector<uint8_t> &out) {
		const std::string key = retail::lower_ascii(std::filesystem::path(name).filename().string());
		const auto cached = cache.find(key);
		if (cached != cache.end()) {
			out = cached->second;
			return true;
		}
		const auto found = files.find(key);
		if (found == files.end() || !test_io::read_file(found->second, out)) return false;
		cache[key] = out;
		return true;
	};
	assets.has = [&files](const std::string &name) {
		return files.count(retail::lower_ascii(std::filesystem::path(name).filename().string())) != 0;
	};
	const std::map<std::string, std::string> vars = style_vars(assets);
	std::printf("the grill's census corpus (%s), with its assets:\n", root.c_str());
	Census census;
	for (const std::string &path : menus) {
		opennova::mnu::Document doc;
		std::string error;
		const std::string label = std::filesystem::path(path).filename().string();
		if (!opennova::mnu::parse_file(path, doc, error)) {
			std::printf("  FAIL %s (%s)\n", label.c_str(), error.c_str());
			++totals.failures;
			continue;
		}
		++census.menus;
		compile_document(label, doc, &assets, vars, totals);
		for (const opennova::mnu::Screen &screen : doc.screens) {
			for (const Window &root_window : screen.roots) {
				walk_windows(root_window, [&census](const Window &w) {
					for (const std::vector<Appearance> *rows : { &w.appearances, &w.items.appearances, &w.shuttle,
								 &w.scrollup, &w.scrolldown })
						for (const Appearance &row : *rows) {
							if (row.type.empty()) ++census.typeless;
							else if (!drawing_type(row.type)) ++census.unknown_type;
						}
				});
			}
		}
		for (size_t s = 0; s < doc.screens.size(); ++s) {
			++census.screens;
			census_statics(doc, s, assets, vars, census);
			census_bands(doc, s, assets, vars, census);
			census_arrows(doc, s, assets, vars, census);
			census_markers(doc, s, assets, vars, census);
		}
	}
	std::printf("census: %d menus, %d screens\n", census.menus, census.screens);
	std::printf("  statics with no MOUSEOVER/SELECTED/DISABLED row %d (grill: 134); recoloured under the mouse %d "
				"(retail: 0); would follow an authored MOUSEOVER row %d\n",
			census.quiet_statics, census.quiet_recoloured, census.quiet_would_turn);
	std::printf("  spin arrows %d (grill: 68); drawing all four state rows %d\n", census.arrows,
			census.arrows_all_states);
	std::printf("  typeless APPEARANCE rows %d (grill: 216), unknown TYPE %d (grill: 1); widgets put in a "
				"marker-only state %d; compiles where a marker drew %d\n",
			census.typeless, census.unknown_type, census.marker_widgets, census.marker_draw_diffs);
	const auto expect = [&](bool ok, const char *what) {
		if (!ok) {
			std::printf("  FAIL %s\n", what);
			++census.failures;
		}
	};
	expect(census.menus == 16, "the census corpus is the grill's sixteen menus");
	expect(census.quiet_statics == 134, "134 statics author no MOUSEOVER, SELECTED or DISABLED row");
	expect(census.quiet_recoloured == 0, "none of them changes its label colour under the mouse");
	expect(census.quiet_would_turn > 0, "the label follows the state once a MOUSEOVER row is authored");
	expect(census.shadowquality, "SHADOWQUALITY draws the 20-texel band btn1e.tga's first load baked");
	expect(census.red_pw, "RED_PW draws the 24-texel band btn3.tga's first load baked");
	expect(census.arrows == 68 && census.arrows_all_states == 68, "the 68 spin arrows draw all four state rows");
	expect(census.typeless == 216 && census.unknown_type == 1, "216 typeless rows and one unknown TYPE");
	expect(census.marker_widgets > 0 && census.marker_draw_diffs == 0, "a typeless row marks its state, draws nothing");
	totals.failures += census.failures;
	return 0;
}

} // namespace

int main() {
	static const char *const kMenus[] = {
		"jo_main", "jo_sp", "jo_mp", "jo_options", "jo_game", "jo_player", "jo_weapon", "jo_loadout",
		"jo_color", "jo_cmap", "jo_stat", "jo_death", "jo_vehicle", "jo_item_db", "jo_splash",
	};
	Totals totals;
	std::printf("the fixture menus (no assets):\n");
	for (const char *name : kMenus) {
		const std::string path = retail::reference_fixture((std::string("mnu/") + name + ".mnu").c_str());
		if (path.empty())
			return retail::skip("OPENNOVA_JO_ASSETS/fixtures/mnu/jo_*.mnu (the fifteen shipped revx02 menus)");
		opennova::mnu::Document doc;
		std::string error;
		if (!opennova::mnu::parse_file(path, doc, error)) {
			std::printf("  FAIL %s (%s)\n", name, error.c_str());
			++totals.failures;
			continue;
		}
		compile_document(name, doc, nullptr, {}, totals);
	}
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d fixture screen(s) FAILED\n", totals.failures);
		return 1;
	}
	census_leg(totals);
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d census check(s) or screen(s) FAILED\n", totals.failures);
		return 1;
	}
	const std::string install = retail::install();
	if (install.empty()) return retail::skip_leg("OPENNOVA_JO_DIR (the packed install's .mnu set)");
	std::printf("the installed menus, with their assets:\n");
	int menus = 0;
	sweep_mount(install, std::string(), totals, menus);
	for (const std::string &expansion : retail::expansions()) sweep_mount(install, expansion, totals, menus);
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d installed screen(s) FAILED\n", totals.failures);
		return 1;
	}
	if (menus == 0) {
		std::fprintf(stderr, "\nthe packed install served no .mnu\n");
		return 1;
	}
	std::printf("retail leg: %d installed menu(s), %d screen(s) compiled\n", menus, totals.screens);
	return 0;
}
