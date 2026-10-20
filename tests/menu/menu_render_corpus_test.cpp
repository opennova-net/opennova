// The tree's menus through the frame compiler, in three legs, each screen compiled
// with its own assets read the way the game reads them: the TEXT_RSRC tables (section
// "menu"), the FONT files by retail's name rule, the textures by retail's extension
// dispatch, and the stylesheet's %VAR%s. A table of what each screen drew and what did
// not load is printed.
//  1. Every screen of the authored menus under fixtures/mnu (shipped_forms.mnu with
//     every form the shipped JO menus take among them) configures and compiles (no
//     assets).
//  2. fixtures/mnu/render_cases.mnu, the drawing cases the 2026-09-23 grill counted in
//     the shipped screens (set B3), each authored, over the base game's fonts and
//     stylesheet with its strips minted here: the statics with no MOUSEOVER, SELECTED or
//     DISABLED row keep their default label colour under the mouse (the grill's 134,
//     retail: none recoloured); a row whose own HEIGHT differs from the one its
//     texture's first load baked draws the baked band (the grill's options.mnu
//     SHADOWQUALITY, 24 on a texture first loaded at 20, and mp.mnu RED_PW's DISABLED
//     row, 2 on one first loaded at 24); the spin arrows draw each of their four state
//     rows (the grill's 68); and the typeless APPEARANCE rows, and a TYPE the parse
//     does not know (death.mnu's one "window"), mark their state and draw nothing (the
//     grill's 217).
//  3. Every screen of the base game's menus (assets/) compiles with the base game's
//     files beside it, as an install serves its menus.
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
#include <base/vfs/file_source.h>
#include <formats/mnu/mnu.h>
#include <formats/mns/mns.h>
#include <formats/rtxt/rtxt.h>
#include <formats/tga/tga.h>
#include <runtime/menu/menu_assets.h>
#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>
#include <runtime/menu/menu_screen_inputs.h>
#include <runtime/menu/menu_text_tables.h>
#include <runtime/menu/menu_texture_header.h>

#include "common/file_io.h"
#include "mnu/repo_menus.h"

using opennova::menu::MenuDrawList;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuFrameState;
using opennova::menu::MenuQuad;
using opennova::menu::MenuWidgetState;
using opennova::mnu::Appearance;
using opennova::mnu::Window;
using opennova::strutil::iequals;

namespace {

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

// A texture's size from its header (menu_texture_header_size): what a headless check decodes.
class HeaderSizes : public opennova::menu::MenuTextureDecoder {
public:
	bool decode(const std::string &key, opennova::menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes,
			int &width, int &height) override {
		if (!opennova::menu::menu_texture_header_size(format, bytes, &width, &height)) return false;
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
		opennova::mns::KeyValueList list;
		opennova::mns::parse_key_value_buffer(reinterpret_cast<const char *>(style.data()), style.size(), list);
		for (const auto &kv : list.sheet().variables) vars[kv.first] = kv.second;
	}
	return vars;
}

// A loose tree's files, read once and by file name without case (as the game's mount
// resolves them), under an overlay of files made in memory. An LFS pointer is no file.
class LooseFiles {
public:
	explicit LooseFiles(const std::string &dir) {
		std::error_code ec;
		for (const auto &entry : std::filesystem::directory_iterator(dir, ec))
			if (entry.is_regular_file(ec))
				paths_[repo_menus::lower(entry.path().filename().string())] = entry.path().generic_string();
	}
	void add(const std::string &name, std::vector<uint8_t> bytes) { cache_[repo_menus::lower(name)] = std::move(bytes); }
	AssetSource source() {
		AssetSource assets;
		assets.read = [this](const std::string &name, std::vector<uint8_t> &out) { return read(name, out); };
		assets.has = [this](const std::string &name) {
			std::vector<uint8_t> bytes;
			return read(name, bytes);
		};
		return assets;
	}

private:
	bool read(const std::string &name, std::vector<uint8_t> &out) {
		const std::string key = repo_menus::lower(std::filesystem::path(name).filename().string());
		const auto cached = cache_.find(key);
		if (cached != cache_.end()) {
			out = cached->second;
			return true;
		}
		const auto found = paths_.find(key);
		if (found == paths_.end() || !test_io::read_file(found->second, out) || test_io::is_lfs_pointer(out)) return false;
		cache_[key] = out;
		return true;
	}
	std::map<std::string, std::string> paths_;
	std::map<std::string, std::vector<uint8_t>> cache_;
};

// Every screen of each menu compiled over `assets` (or none).
void sweep_menus(const std::vector<std::string> &paths, const AssetSource *assets,
		const std::map<std::string, std::string> &vars, Totals &totals) {
	for (const std::string &path : paths) {
		opennova::mnu::Document doc;
		std::string error;
		if (!opennova::mnu::parse_file(path, doc, error)) {
			std::printf("  FAIL %s (%s)\n", repo_menus::name(path).c_str(), error.c_str());
			++totals.failures;
			continue;
		}
		compile_document(repo_menus::name(path), doc, assets, vars, totals);
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

// The quad's texel band [top, bottom) in a texture `height` texels tall: its V runs over
// texel rows top + 0.5 .. bottom + 0.5, the strip's half texel at both ends
// [orig: Render_DrawTiledTextureStrip @ 0x67b058].
bool band_is(const MenuQuad *q, int height, int top, int bottom) {
	if (q == nullptr || height <= 0) return false;
	const float h = static_cast<float>(height);
	return std::fabs(q->v0 * h - (static_cast<float>(top) + 0.5f)) < 1.0e-3f &&
			std::fabs(q->v1 * h - (static_cast<float>(bottom) + 0.5f)) < 1.0e-3f;
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
	bool tall_rows = false;
	bool low_row = false;
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

// The rows whose own HEIGHT differs from the one their texture's first load baked, as
// the shipped screens hold two (the grill's options.mnu SHADOWQUALITY, a combo box's 24
// on a texture first loaded at 20, and mp.mnu RED_PW's DISABLED row, an edit's 2 on one
// first loaded at 24): render_cases.mnu's TALL_ROWS (24 on band20.tga, first loaded at
// 20) and LOW_ROW's DISABLED row (2 on band24.tga, first loaded at 24).
void census_bands(const opennova::mnu::Document &doc, size_t screen_at, const AssetSource &assets,
		const std::map<std::string, std::string> &vars, Census &census) {
	struct Case {
		const char *widget;
		const char *texture;
		int band;
		bool *seen;
	};
	const Case cases[] = {
		{ "TALL_ROWS", "band20.tga", 20, &census.tall_rows },
		{ "LOW_ROW", "band24.tga", 24, &census.low_row },
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

// A strip `width` x `height` texels, every texel the band it sits in (from small integer data),
// written through our TGA writer.
std::vector<uint8_t> minted_strip(uint32_t width, uint32_t height, uint32_t band) {
	std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4u);
	for (uint32_t y = 0; y < height; ++y)
		for (uint32_t x = 0; x < width; ++x) {
			uint8_t *px = &rgba[(static_cast<size_t>(y) * width + x) * 4u];
			px[0] = static_cast<uint8_t>(64u * (y / band));
			px[1] = static_cast<uint8_t>(4u * x);
			px[2] = 128;
			px[3] = 255;
		}
	std::vector<uint8_t> out;
	std::string error;
	opennova::tga::tga_write_rgba32(rgba.data(), width, height, out, error);
	return out;
}

// render_cases.mnu's drawing cases, read with the base game's files beside it (its fonts
// and menu_style.mns) and its strips minted here: four bands each, band20.tga 20 texels a
// band, band24.tga 24, arrows.tga 16; arrowup.tga one 16-texel image.
int cases_leg(Totals &totals) {
	LooseFiles files(repo_menus::path("assets"));
	files.add("band20.tga", minted_strip(32, 80, 20));
	files.add("band24.tga", minted_strip(32, 96, 24));
	files.add("arrows.tga", minted_strip(16, 64, 16));
	files.add("arrowup.tga", minted_strip(16, 16, 16));
	const AssetSource assets = files.source();
	const std::map<std::string, std::string> vars = style_vars(assets);
	const std::string path = repo_menus::path("fixtures/mnu/render_cases.mnu");
	opennova::mnu::Document doc;
	std::string error;
	if (!opennova::mnu::parse_file(path, doc, error)) {
		std::printf("  FAIL render_cases.mnu (%s)\n", error.c_str());
		++totals.failures;
		return 0;
	}
	std::printf("render_cases.mnu, with the base game's fonts and stylesheet:\n");
	compile_document("render_cases.mnu", doc, &assets, vars, totals);
	Census census;
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
	std::printf("  statics with no MOUSEOVER/SELECTED/DISABLED row %d; recoloured under the mouse %d; would follow "
				"an authored MOUSEOVER row %d\n",
			census.quiet_statics, census.quiet_recoloured, census.quiet_would_turn);
	std::printf("  spin arrows %d; drawing all four state rows %d\n", census.arrows, census.arrows_all_states);
	std::printf("  typeless APPEARANCE rows %d, unknown TYPE %d; widgets put in a marker-only state %d; compiles "
				"where a marker drew %d\n",
			census.typeless, census.unknown_type, census.marker_widgets, census.marker_draw_diffs);
	const auto expect = [&](bool ok, const char *what) {
		if (!ok) {
			std::printf("  FAIL %s\n", what);
			++census.failures;
		}
	};
	// QUIET_LABEL (a DEFAULT row) and BARE_LABEL (no row); FRAMED_LABEL authors a MOUSEOVER row.
	expect(census.quiet_statics == 2, "the two statics author no MOUSEOVER, SELECTED or DISABLED row");
	expect(census.quiet_recoloured == 0, "neither changes its label colour under the mouse");
	expect(census.quiet_would_turn == 2, "each label follows the state once a MOUSEOVER row is authored");
	expect(census.tall_rows, "TALL_ROWS draws the 20-texel band band20.tga's first load baked");
	expect(census.low_row, "LOW_ROW draws the 24-texel band band24.tga's first load baked");
	expect(census.arrows == 4 && census.arrows_all_states == 4, "the 4 spin arrows draw all four state rows");
	// QUIET_LABEL's one, MARKED's four, FRAMED_LABEL's two, and FRAMED's "window" TYPE, which the
	// reader holds as none (no TYPE the parse knows).
	expect(census.typeless == 8 && census.unknown_type == 0, "8 typeless rows: the one unknown TYPE reads as none");
	expect(census.marker_widgets > 0 && census.marker_draw_diffs == 0, "a typeless row marks its state, draws nothing");
	totals.failures += census.failures;
	return 0;
}

} // namespace

int main() {
	Totals totals;
	const std::vector<std::string> authored = repo_menus::files("fixtures/mnu", ".mnu");
	if (!repo_menus::holds(authored, "shipped_forms.mnu") || !repo_menus::holds(authored, "render_cases.mnu")) {
		std::fprintf(stderr, "fixtures/mnu/shipped_forms.mnu or render_cases.mnu is missing\n");
		return 1;
	}
	std::printf("the authored menus (no assets):\n");
	sweep_menus(authored, nullptr, {}, totals);
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d fixture screen(s) FAILED\n", totals.failures);
		return 1;
	}
	cases_leg(totals);
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d case check(s) or screen(s) FAILED\n", totals.failures);
		return 1;
	}
	std::printf("the base game's menus, with its files:\n");
	const std::vector<std::string> base = repo_menus::files("assets", ".mnu");
	LooseFiles files(repo_menus::path("assets"));
	const AssetSource assets = files.source();
	sweep_menus(base, &assets, style_vars(assets), totals);
	if (totals.failures != 0) {
		std::fprintf(stderr, "\n%d base game screen(s) FAILED\n", totals.failures);
		return 1;
	}
	if (base.empty()) {
		std::fprintf(stderr, "\nassets/ holds no .mnu\n");
		return 1;
	}
	std::printf("%zu authored and %zu base game menu(s), %d screen(s) compiled\n", authored.size(), base.size(),
			totals.screens);
	return 0;
}
