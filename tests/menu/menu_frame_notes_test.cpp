// The frame compiler's notes (engine/runtime/menu/menu_frame_notes.cpp, ADR 0046 S9j2):
// one small screen per code, each note on the window (and the list, record and field)
// that causes it; the loader's notes through MenuFrameAssets over a fake file source; the
// notes only observe (the configure hooks are const, and the rig screen compiles to the
// draw list the compiler drew before it had notes, a golden; a compile, then layout_notes
// and add_load_note, then the same compile again emit the same draw list byte for byte);
// two configures note the same;
// solve_local_rect and widget_parent. The rule each code stands for is the compiler's
// (docs/mnu/menu-re.md "Compiler notes").

#include <runtime/menu/menu_frame.h>
#include <runtime/menu/menu_frame_assets.h>
#include <runtime/menu/menu_text_tables.h>

#include <formats/fnt/fnt.h>
#include <formats/mnu/mnu.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "common/test_font.h"
#include "menu/fake_file_source.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

using opennova::menu::MenuDrawList;
using opennova::menu::MenuFrameCompiler;
using opennova::menu::MenuFrameNote;
using opennova::menu::MenuFrameNoteBasis;
using opennova::menu::MenuFrameNoteCode;
using opennova::menu::MenuFrameState;
using opennova::menu::MenuWidgetState;
using opennova::mnu::Appearance;
using opennova::mnu::Window;
using opennova::mnu::WindowType;
using Code = MenuFrameNoteCode;

Window window(WindowType type, const char *name, int left, int top, int right, int bottom) {
	Window w;
	w.type = type;
	w.name = name;
	w.position.left = left;
	w.position.top = top;
	w.position.right = right;
	w.position.bottom = bottom;
	w.position.has_left = w.position.has_top = w.position.has_right = w.position.has_bottom = true;
	return w;
}

Appearance appearance(const char *state, const char *type, const char *value) {
	Appearance a;
	a.state = state;
	a.type = type;
	a.value = value;
	return a;
}

// One screen compiled: the root window `root` (with whatever it holds), the font
// "f.fnt" registered, the textures given sizes.
struct Rig {
	opennova::mnu::Screen screen;
	opennova::fnt::fnt_font_t font = test_font::uniform_test_font();
	MenuFrameCompiler compiler;
	MenuFrameState state;
	opennova::menu::MenuTextTables tables;
	~Rig() { opennova::fnt::fnt_free(&font); }
	void configure(const Window &root, const std::map<std::string, std::pair<int, int>> &textures = {},
	               const std::map<std::string, std::string> &vars = {}) {
		screen = opennova::mnu::Screen();
		screen.name = "S";
		screen.roots.push_back(root);
		compiler.set_style_vars(vars);
		compiler.set_text_tables(&tables);
		compiler.clear_registered_fonts();
		compiler.register_font("f.fnt", &font);
		compiler.configure(&screen);
		for (size_t slot = 0; slot < compiler.texture_names().size(); ++slot) {
			const auto found = textures.find(compiler.texture_names()[slot]);
			if (found != textures.end())
				compiler.set_texture_size(int32_t(slot), found->second.first, found->second.second);
		}
	}
	std::vector<MenuFrameNote> notes() const {
		std::vector<MenuFrameNote> all = compiler.build_notes();
		for (const MenuFrameNote &note : compiler.layout_notes(state)) all.push_back(note);
		return all;
	}
};

void with_font(Window &w) { w.font.name = "f.fnt"; }

void with_text(Window &w, const char *text, const char *type = "") {
	w.string_data.present = true;
	w.string_data.type = type;
	w.string_data.value = text;
}

const MenuFrameNote *find(const std::vector<MenuFrameNote> &notes, Code code) {
	for (const MenuFrameNote &note : notes)
		if (note.code == code) return &note;
	return nullptr;
}

size_t count(const std::vector<MenuFrameNote> &notes, Code code) {
	size_t n = 0;
	for (const MenuFrameNote &note : notes) n += note.code == code ? 1 : 0;
	return n;
}

bool is(const MenuFrameNote *note, int widget, const char *list, int record, const char *field,
        const char *subject = nullptr) {
	if (note == nullptr) return false;
	bool ok = note->widget == widget && note->list == list && note->record == record && note->field == field &&
	          (subject == nullptr || note->subject == subject);
	if (!ok)
		std::fprintf(stderr, "  note %s: widget %d list '%s' record %d field '%s' subject '%s'\n",
		             opennova::menu::menu_frame_note_token(note->code), note->widget, note->list.c_str(), note->record,
		             note->field.c_str(), note->subject.c_str());
	return ok;
}

// A draw list as text, every field (floats as hex): equal text is an equal list.
std::string digest(const MenuDrawList &list) {
	std::string out;
	char line[256];
	for (const auto &q : list.quads) {
		std::snprintf(line, sizeof(line), "q %a %a %a %a %a %a %a %a %08x %d %d %d\n", q.x0, q.y0, q.x1, q.y1, q.u0,
		              q.v0, q.u1, q.v1, unsigned(q.color), q.texture, q.texture2, q.tiled ? 1 : 0);
		out += line;
	}
	for (const auto &l : list.lines) {
		std::snprintf(line, sizeof(line), "l %a %a %a %a %08x\n", l.x0, l.y0, l.x1, l.y1, unsigned(l.color));
		out += line;
	}
	for (const auto &g : list.glyphs) {
		std::snprintf(line, sizeof(line), "g %a %a %a %a %a %a %a %a %a %a %08x %u\n", g.x_top_left, g.x_top_right,
		              g.x_bottom_left, g.x_bottom_right, g.y_top, g.y_bottom, g.u0, g.v0, g.u1, g.v1,
		              unsigned(g.color), unsigned(g.page));
		out += line;
	}
	for (const auto &u : list.underlines) {
		std::snprintf(line, sizeof(line), "u %a %a %a %08x\n", u.x0, u.x1, u.y, unsigned(u.color));
		out += line;
	}
	for (const auto &r : list.font_runs) {
		std::snprintf(line, sizeof(line), "r %d %d %d %d %d\n", r.font, r.first, r.count, r.underline_first,
		              r.underline_count);
		out += line;
	}
	for (const auto &op : list.draw_ops) {
		std::snprintf(line, sizeof(line), "o %d %d\n", int(op.kind), op.index);
		out += line;
	}
	std::snprintf(line, sizeof(line), "overlay %d widgets %lld\n", list.overlay_op_start,
	              static_cast<long long>(list.widgets_drawn));
	return out + line;
}

// A draw list's FNV-1a 64 over every field's bits (little-endian words), the same on every
// platform: a golden a test can pin.
struct Fnv {
	uint64_t h = 1469598103934665603ull;
	void u(uint32_t v) {
		for (int shift = 0; shift < 32; shift += 8) {
			h ^= (v >> shift) & 0xFFu;
			h *= 1099511628211ull;
		}
	}
	void f(float v) {
		uint32_t bits = 0;
		std::memcpy(&bits, &v, sizeof(bits));
		u(bits);
	}
	void i(int64_t v) {
		u(uint32_t(uint64_t(v)));
		u(uint32_t(uint64_t(v) >> 32));
	}
};

uint64_t golden(const MenuDrawList &list) {
	Fnv h;
	for (const auto &q : list.quads) {
		for (float v : {q.x0, q.y0, q.x1, q.y1, q.u0, q.v0, q.u1, q.v1}) h.f(v);
		h.u(q.color);
		h.i(q.texture);
		h.i(q.texture2);
		h.i(q.tiled ? 1 : 0);
	}
	for (const auto &l : list.lines) {
		for (float v : {l.x0, l.y0, l.x1, l.y1}) h.f(v);
		h.u(l.color);
	}
	for (const auto &g : list.glyphs) {
		for (float v : {g.x_top_left, g.x_top_right, g.x_bottom_left, g.x_bottom_right, g.y_top, g.y_bottom, g.u0, g.v0,
		                g.u1, g.v1})
			h.f(v);
		h.u(g.color);
		h.i(g.page);
	}
	for (const auto &u : list.underlines) {
		for (float v : {u.x0, u.x1, u.y}) h.f(v);
		h.u(u.color);
	}
	for (const auto &r : list.font_runs) {
		for (int64_t v : {int64_t(r.font), int64_t(r.first), int64_t(r.count), int64_t(r.underline_first),
		                  int64_t(r.underline_count)})
			h.i(v);
	}
	for (const auto &op : list.draw_ops) {
		h.i(int64_t(op.kind));
		h.i(op.index);
	}
	h.i(list.overlay_op_start);
	h.i(static_cast<int64_t>(list.widgets_drawn));
	return h.h;
}

} // namespace

// Every code has its own token and a basis; only the port's gaps are Deferred.
static int test_tokens() {
	std::set<std::string> tokens;
	for (int at = 0; at < opennova::menu::kMenuFrameNoteCodeCount; ++at) {
		const Code code = static_cast<Code>(at);
		const std::string token = opennova::menu::menu_frame_note_token(code);
		TEST_EXPECT(!token.empty() && tokens.insert(token).second);
		const MenuFrameNoteBasis basis = opennova::menu::menu_frame_note_basis(code);
		const bool deferred = code == Code::TypeInteriorDeferred || code == Code::ItemKindNotDrawn ||
		                      code == Code::TableCellsDeferred;
		TEST_EXPECT((basis == MenuFrameNoteBasis::Deferred) == deferred);
		TEST_EXPECT((basis == MenuFrameNoteBasis::PortPolicy) == (code == Code::MarqueeRuntimeContent));
	}
	TEST_EXPECT(std::string(opennova::menu::menu_frame_note_token(Code::TextTruncated)) == "text_truncated");
	return 0;
}

// The configure notes: an APPEARANCE row the parse does not use, a colour that does not
// read as written, a %VAR% the list lacks, a TYPE the factory does not match.
static int test_configure_notes() {
	{
		Rig rig;
		Window root = window(WindowType::Static, "ROOT", 0, 0, 100, 20);
		root.appearances = {appearance("HOVER", "COLOR", "FF00FF00"), appearance("DEFAULT", "SPARKLE", "x"),
		                    appearance("DEFAULT", "", ""), appearance("MOUSEOVER", "CUSTOM", ""),
		                    appearance("DEFAULT", "COLOR", "FFZZ0000"), appearance("DEFAULT", "COLOR", "FF0000"),
		                    appearance("SELECTED", "OUTLINE", "%NOPE%"), appearance("DISABLED", "COLOR", "%GREY%"),
		                    appearance("MOUSEOVER", "COLOR", "00FF0000")};
		root.font.default_fg = "red";
		rig.configure(root, {}, {{"GREY", "FF808080"}});
		const std::vector<MenuFrameNote> &notes = rig.compiler.build_notes();
		// Six digits leave the alpha 0; eight with an alpha of 00 are taken as written.
		TEST_EXPECT(count(notes, Code::ColorTransparent) == 1);
		TEST_EXPECT(is(find(notes, Code::AppearanceStateUnknown), 0, "appearance", 0, "state", "HOVER"));
		// An unknown TYPE marks the state; an empty one is the marker on purpose (no note).
		TEST_EXPECT(count(notes, Code::AppearanceTypeUnknown) == 1);
		TEST_EXPECT(is(find(notes, Code::AppearanceTypeUnknown), 0, "appearance", 1, "type", "SPARKLE"));
		TEST_EXPECT(is(find(notes, Code::AppearanceCustom), 0, "appearance", 3, "type", "mouseover"));
		// Two DEFAULT COLOR rows: the later one's value is the one read.
		TEST_EXPECT(is(find(notes, Code::AppearanceReplaced), 0, "appearance", 4, "value", "default color"));
		TEST_EXPECT(is(find(notes, Code::ColorTransparent), 0, "appearance", 5, "value", "FF0000"));
		TEST_EXPECT(count(notes, Code::StyleVarUnresolved) == 1);
		TEST_EXPECT(is(find(notes, Code::StyleVarUnresolved), 0, "appearance", 6, "value", "%NOPE%"));
		TEST_EXPECT(count(notes, Code::ColorUnparsed) == 2);
		TEST_EXPECT(is(&notes[0], 0, "", -1, "font.default_fg", "red")); // the window's own fields first
		bool row_unparsed = false;
		for (const MenuFrameNote &note : notes)
			row_unparsed = row_unparsed || (note.code == Code::ColorUnparsed && note.list == "appearance" &&
			                                note.record == 4 && note.subject == "FFZZ0000");
		TEST_EXPECT(row_unparsed);
	}
	{
		// A name the game's expansion stops inside (a space), or an empty one ("%%"), is no
		// variable: the value is read as the colour it is, never looked up; blanks and a sign
		// around eight digits still read whole (wcstoul).
		Rig rig;
		Window root = window(WindowType::Static, "ROOT", 0, 0, 100, 20);
		root.appearances = {appearance("DEFAULT", "COLOR", "%A B%"), appearance("MOUSEOVER", "COLOR", "%%"),
		                    appearance("SELECTED", "COLOR", " +FF00FF00 ")};
		rig.configure(root, {}, {{"A B", "FF808080"}});
		const std::vector<MenuFrameNote> &notes = rig.compiler.build_notes();
		TEST_EXPECT(count(notes, Code::StyleVarUnresolved) == 0);
		TEST_EXPECT(count(notes, Code::ColorUnparsed) == 2 && count(notes, Code::ColorTransparent) == 0);
		TEST_EXPECT(is(find(notes, Code::ColorUnparsed), 0, "appearance", 0, "value", "%A B%"));
	}
	{
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		root.type_token = "SLIDER";
		Window radio_edit = window(WindowType::RadioEdit, "RE", 0, 0, 10, 10);
		Window list = window(WindowType::List, "LIST", 0, 0, 100, 100);
		list.items.present = true;
		list.items.items.resize(2);
		list.items.items[0].text = "a";
		list.items.items[1].type = "IMAGE";
		list.items.items[1].text = "x.tga";
		Window table = window(WindowType::Table, "TABLE", 0, 0, 100, 100);
		table.table_data.column.bodies.resize(1);
		table.table_data.column.bodies[0].display = "BITMAP_DRAW";
		// A SCROLL with no HEIGHT or WIDTH keeps the default arrow length; one with it does not.
		Window scroll = window(WindowType::Scroll, "SCROLL", 0, 0, 20, 100);
		Window sized = window(WindowType::Scroll, "SIZED", 0, 0, 20, 100);
		sized.has_scroll_extent = true;
		sized.scroll_extent = 12;
		root.children = {radio_edit, list, table, scroll, sized};
		rig.configure(root);
		const std::vector<MenuFrameNote> &notes = rig.compiler.build_notes();
		TEST_EXPECT(is(find(notes, Code::TypeUnknown), 0, "", -1, "type", "SLIDER"));
		TEST_EXPECT(is(find(notes, Code::TypeInteriorDeferred), 1, "", -1, "type"));
		TEST_EXPECT(is(find(notes, Code::ItemKindNotDrawn), 2, "items.item", 1, "type", "IMAGE"));
		TEST_EXPECT(is(find(notes, Code::TableCellsDeferred), 3, "column.body", 0, "display", "BITMAP_DRAW"));
		TEST_EXPECT(count(notes, Code::ScrollExtentDefault) == 1);
		TEST_EXPECT(is(find(notes, Code::ScrollExtentDefault), 4, "", -1, "scroll_extent", "20"));
	}
	{
		// A spin arrow's rows are its list's SPINUP record.
		Rig rig;
		Window spin = window(WindowType::SpinList, "SPIN", 0, 0, 100, 20);
		with_font(spin);
		Window &up = spin.spinup.author(WindowType::Button);
		up.position = window(WindowType::Button, "", 100, 0, 116, 10).position;
		up.appearances = {appearance("DEFAULT", "SPARKLE", "")};
		rig.configure(spin);
		TEST_EXPECT(is(find(rig.compiler.build_notes(), Code::AppearanceTypeUnknown), 0, "spinup", 0, ""));
	}
	return 0;
}

// The loader's notes: every window naming a font, a texture or a table that did not load,
// told apart by whether the files have it.
static int test_loader_notes() {
	FakeFileSource files;
	files.put("bad.tga", {1, 2, 3}, 1);    // there, and not a texture
	files.put("broken.fnt", {9, 9, 9}, 1); // there, and not a font
	files.put("bad.bin", {1, 2, 3}, 1);    // there, and not a string table
	const char *const xml =
	        "<SCREEN><NAME>S</NAME>"
	        "<WINDOW type=\"window\" name=\"ROOT\"><POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>800</RIGHT>"
	        "<BOTTOM>600</BOTTOM></POSITION><TEXT_RSRC>gone.bin</TEXT_RSRC>"
	        "<FONT><NAME>gone.fnt</NAME></FONT>"
	        "<WINDOW type=\"static\" name=\"A\"><TEXT_RSRC>bad.bin</TEXT_RSRC>"
	        "<APPEARANCE state=\"default\" type=\"image\">gone.tga</APPEARANCE>"
	        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>"
	        "<WINDOW type=\"static\" name=\"B\" DRAW_FRAME><FRAME><STENCIL>bad.tga</STENCIL></FRAME>"
	        "<FONT><NAME>broken.fnt</NAME></FONT>"
	        "<POSITION><LEFT>0</LEFT><TOP>0</TOP><RIGHT>10</RIGHT><BOTTOM>10</BOTTOM></POSITION></WINDOW>"
	        "</WINDOW></SCREEN>";
	opennova::mnu::Document doc;
	std::string error;
	TEST_EXPECT(opennova::mnu::parse(std::string(xml), doc, error));
	struct Sizes : opennova::menu::MenuTextureDecoder {
		bool decode(const std::string &, opennova::menu::MenuTextureFormat format, const std::vector<uint8_t> &bytes,
		            int &w, int &h) override {
			uint32_t width = 0, height = 0;
			if (format != opennova::menu::MenuTextureFormat::Tga ||
			    !opennova::tga::tga_header_size(bytes.data(), bytes.size(), width, height))
				return false;
			w = static_cast<int>(width);
			h = static_cast<int>(height);
			return true;
		}
		void release(const std::string &) override {}
	} decoder;
	MenuFrameCompiler compiler;
	opennova::menu::MenuFrameAssets assets;
	assets.configure(compiler, &doc, &doc.screens[0], files, decoder, {});
	const std::vector<MenuFrameNote> &notes = compiler.build_notes();
	TEST_EXPECT(is(find(notes, Code::TextTableMissing), 0, "", -1, "text_rsrc", "gone.bin"));
	TEST_EXPECT(is(find(notes, Code::TextTableUnreadable), 1, "", -1, "text_rsrc", "bad.bin"));
	TEST_EXPECT(is(find(notes, Code::FontMissing), 0, "", -1, "font.name", "gone.fnt"));
	TEST_EXPECT(is(find(notes, Code::FontUnreadable), 2, "", -1, "font.name", "broken.fnt"));
	TEST_EXPECT(is(find(notes, Code::TextureMissing), 1, "appearance", 0, "value", "gone.tga"));
	TEST_EXPECT(is(find(notes, Code::TextureUnreadable), 2, "", -1, "frame.stencil", "bad.tga"));
	// The frame whose stencil did not load draws nothing: the layout says so.
	TEST_EXPECT(is(find(compiler.layout_notes(MenuFrameState()), Code::FrameStencilUnloaded), 2, "", -1,
	               "draw_frame", "bad.tga"));
	// A name no window names is the screen's.
	compiler.add_load_note(Code::TextureMissing, "nobody.tga");
	TEST_EXPECT(is(&compiler.build_notes().back(), -1, "", -1, "", "nobody.tga"));
	assets.clear(compiler, decoder);
	return 0;
}

// The layout notes: the rect, the label, the string id, the bands, the states held, the
// frame, the arrows, the list, the table, the marquee.
static int test_layout_notes() {
	{
		// A rect with no area that draws: its cause and the empty axis.
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		Window flat = window(WindowType::Static, "FLAT", 10, 10, 10, 30);
		flat.appearances = {appearance("DEFAULT", "COLOR", "FF00FF00")};
		Window pic = window(WindowType::Static, "PIC", 10, 10, 0, 0);
		pic.position.has_right = pic.position.has_bottom = false;
		pic.appearances = {appearance("DEFAULT", "IMAGE", "pic.tga")};
		Window holder = window(WindowType::Window, "HOLDER", 5, 5, 5, 5); // draws nothing: quiet
		// A frame with a BRUSH keeps its border pieces, which hang outside the rect: quiet.
		// One with no BRUSH draws only its tiled middle, which the rect holds: noted.
		Window braced = window(WindowType::Window, "BRACED", 5, 5, 5, 40);
		braced.draw_frame = true;
		braced.frame.stencil = "st.tga";
		braced.frame.brush = "br.tga";
		Window plain = window(WindowType::Window, "PLAIN", 5, 5, 5, 40);
		plain.draw_frame = true;
		plain.frame.stencil = "st.tga";
		// An OUTLINE still draws its edge lines: quiet.
		Window line = window(WindowType::Static, "LINE", 10, 10, 10, 30);
		line.appearances = {appearance("DEFAULT", "OUTLINE", "FFFFFFFF")};
		root.children = {flat, pic, holder, braced, plain, line};
		rig.configure(root, {{"st.tga", {64, 64}}, {"br.tga", {32, 32}}});
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		std::vector<const MenuFrameNote *> empty;
		for (const MenuFrameNote &note : notes)
			if (note.code == Code::RectEmpty) empty.push_back(&note);
		TEST_EXPECT(empty.size() == 3);
		TEST_EXPECT(empty.size() == 3 && is(empty[0], 1, "", -1, "position.right", "position") &&
		            is(empty[1], 2, "", -1, "position.right", "image") &&
		            is(empty[2], 5, "", -1, "position.right", "position"));
		// Both frames pass the gate, the braced one's border pieces draw with area, and the
		// outline draws its lines.
		TEST_EXPECT(!find(notes, Code::FrameStencilUnloaded) && !find(notes, Code::FrameTileZero));
		const MenuDrawList &drawn = rig.compiler.compile(rig.state, 1.0f, 1.0f);
		bool pieces = false;
		for (const auto &quad : drawn.quads)
			pieces = pieces || (quad.texture2 != opennova::menu::kMenuTexNone && quad.x1 > quad.x0 && quad.y1 > quad.y0);
		TEST_EXPECT(pieces && drawn.lines.size() >= 2);
	}
	{
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		with_font(root);
		Window cut = window(WindowType::Static, "CUT", 0, 0, 40, 20);
		with_text(cut, "ABCDEFGHIJ");
		Window none = window(WindowType::Static, "NONE", 0, 0, 40, 20);
		with_text(none, "AB");
		none.string_data.has_edge = true;
		none.string_data.edge = 30;
		Window id = window(WindowType::Button, "ID", 0, 0, 400, 20);
		with_text(id, "NOPE", "ID");
		root.children = {cut, none, id};
		rig.configure(root);
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		const MenuFrameNote *truncated = find(notes, Code::TextTruncated);
		TEST_EXPECT(is(truncated, 1, "", -1, "string.value"));
		TEST_EXPECT(!truncated->subject.empty() && truncated->subject.size() < 10 &&
		            std::string("ABCDEFGHIJ").compare(0, truncated->subject.size(), truncated->subject) == 0);
		TEST_EXPECT(is(find(notes, Code::TextNoRoom), 2, "", -1, "string.value", "AB"));
		TEST_EXPECT(is(find(notes, Code::TextIdMissing), 3, "", -1, "string.value", "NOPE"));
		// A table that defines the id quiets it.
		auto table = std::make_shared<opennova::rtxt::File>();
		table->sections.push_back({"menu", 1});
		opennova::rtxt::Entry entry;
		entry.key = "NOPE";
		entry.text = "Found";
		table->entries.push_back(entry);
		rig.tables.set_table("t.bin", table);
		rig.screen.roots[0].children[2].has_text_rsrc = true;
		rig.screen.roots[0].children[2].text_rsrc = "t.bin";
		Window copy = rig.screen.roots[0];
		rig.configure(copy);
		TEST_EXPECT(!find(rig.compiler.layout_notes(rig.state), Code::TextIdMissing));
	}
	{
		// Text and no font up the chain: neither measured nor drawn.
		Rig rig;
		Window label = window(WindowType::Static, "LABEL", 0, 0, 100, 20);
		with_text(label, "Hello");
		rig.configure(label);
		TEST_EXPECT(is(find(rig.compiler.layout_notes(rig.state), Code::TextNoFont), 0, "", -1, "font.name", "Hello"));
	}
	{
		// The IMAGE bands: a band past the texture draws nothing; a row whose HEIGHT is not
		// the texture's first load draws that load's band.
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		Window first = window(WindowType::Static, "FIRST", 0, 0, 10, 10);
		first.appearances = {appearance("DEFAULT", "IMAGE", "sheet.tga")};
		first.appearances[0].has_height = true;
		first.appearances[0].height = 20;
		Window second = window(WindowType::Static, "SECOND", 0, 0, 10, 10);
		second.appearances = {appearance("DEFAULT", "IMAGE", "sheet.tga")};
		second.appearances[0].has_height = true;
		second.appearances[0].height = 24;
		second.appearances[0].has_map_state = true;
		second.appearances[0].map_state = 4;
		// No HEIGHT and a HEIGHT that is the texture's own ask for the same band: quiet,
		// whichever loads first.
		Window own = window(WindowType::Static, "OWN", 0, 0, 10, 10);
		own.appearances = {appearance("DEFAULT", "IMAGE", "whole.tga")};
		Window full = window(WindowType::Static, "FULL", 0, 0, 10, 10);
		full.appearances = {appearance("DEFAULT", "IMAGE", "whole.tga")};
		full.appearances[0].has_height = true;
		full.appearances[0].height = 80;
		Window full_first = full;
		full_first.name = "FULL_FIRST";
		full_first.appearances[0].value = "other.tga";
		Window own_later = own;
		own_later.name = "OWN_LATER";
		own_later.appearances[0].value = "other.tga";
		root.children = {first, second, own, full, full_first, own_later};
		rig.configure(root, {{"sheet.tga", {64, 80}}, {"whole.tga", {64, 80}}, {"other.tga", {64, 80}}});
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		TEST_EXPECT(is(find(notes, Code::ImageBandEmpty), 2, "appearance", 0, "map_state", "default"));
		TEST_EXPECT(count(notes, Code::ImageHeightShared) == 1);
		TEST_EXPECT(is(find(notes, Code::ImageHeightShared), 2, "appearance", 0, "height", "20"));
	}
	{
		// The state held: MOUSEOVER not authored draws DEFAULT; a checked box with no
		// SELECTED appearance draws none.
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		Window button = window(WindowType::Button, "B", 0, 0, 10, 10);
		button.appearances = {appearance("DEFAULT", "COLOR", "FF00FF00")};
		Window box = window(WindowType::CheckBox, "C", 0, 0, 10, 10);
		box.checked = true;
		box.appearances = {appearance("DEFAULT", "COLOR", "FF00FF00")};
		root.children = {button, box};
		rig.configure(root);
		MenuWidgetState hover;
		hover.index = 1;
		hover.hovered = true;
		rig.state.widgets.push_back(hover);
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		TEST_EXPECT(is(find(notes, Code::StateFallback), 1, "", -1, "", "mouseover"));
		TEST_EXPECT(is(find(notes, Code::CheckedNoArt), 2, "", -1, "", "selected"));
	}
	{
		// The frame gate: no FRAME up the chain, a stencil whose tile is 0.
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		Window bare = window(WindowType::Static, "BARE", 0, 0, 10, 10);
		bare.draw_frame = true;
		Window thin = window(WindowType::Static, "THIN", 0, 0, 10, 10);
		thin.draw_frame = true;
		thin.frame.stencil = "thin.tga";
		// A FRAME with a BRUSH and no STENCIL sets nothing up; its child inherits it.
		Window brushed = window(WindowType::Static, "BRUSHED", 0, 0, 10, 10);
		brushed.draw_frame = true;
		brushed.frame.brush = "br.tga";
		Window inner = window(WindowType::Static, "INNER", 0, 0, 5, 5);
		inner.draw_frame = true;
		brushed.children = {inner};
		root.children = {bare, thin, brushed};
		rig.configure(root, {{"thin.tga", {3, 3}}, {"br.tga", {32, 32}}});
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		TEST_EXPECT(is(find(notes, Code::FrameAbsent), 1, "", -1, "draw_frame"));
		TEST_EXPECT(is(find(notes, Code::FrameTileZero), 2, "", -1, "draw_frame", "THIN"));
		TEST_EXPECT(count(notes, Code::FrameNoStencil) == 2 && !find(notes, Code::FrameStencilUnloaded));
		TEST_EXPECT(is(find(notes, Code::FrameNoStencil), 3, "", -1, "draw_frame", "BRUSHED"));
	}
	{
		// A spin arrow with no area; a list's rows past its rect with no scrollbar; a
		// table with no column set up, one whose second column passes its right edge; a
		// marquee's credits.
		Rig rig;
		Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
		with_font(root);
		Window spin = window(WindowType::SpinList, "SPIN", 0, 0, 100, 20);
		Window &down = spin.spindown.author(WindowType::Button);
		down.position.has_left = down.position.has_top = true;
		Window list = window(WindowType::List, "LIST", 0, 0, 100, 40);
		list.table_data.has_min_item_height = true;
		list.table_data.min_item_height = 20;
		list.items.present = true;
		list.items.items.resize(3);
		for (auto &item : list.items.items) item.text = "row";
		Window empty_table = window(WindowType::Table, "EMPTY", 0, 0, 100, 100);
		Window wide = window(WindowType::Table, "WIDE", 0, 0, 100, 100);
		wide.table_data.column.has_count = true;
		wide.table_data.column.count = 2;
		wide.table_data.column.headers.resize(2);
		for (int c = 0; c < 2; ++c) {
			wide.table_data.column.headers[size_t(c)].has_column = true;
			wide.table_data.column.headers[size_t(c)].column = c;
			wide.table_data.column.headers[size_t(c)].has_width = true;
			wide.table_data.column.headers[size_t(c)].width = 60;
			wide.table_data.column.headers[size_t(c)].text = c == 0 ? "One" : "Two";
		}
		Window marquee = window(WindowType::Marquee, "CREDITS", 0, 0, 100, 100);
		marquee.datasources = {"credits.txt"};
		// A HEADER of WIDTH 0 skips its column; the one after it carries on.
		Window zero = window(WindowType::Table, "ZERO", 0, 0, 100, 100);
		zero.table_data.column.has_count = true;
		zero.table_data.column.count = 2;
		zero.table_data.column.headers.resize(2);
		for (int c = 0; c < 2; ++c) {
			zero.table_data.column.headers[size_t(c)].has_column = true;
			zero.table_data.column.headers[size_t(c)].column = c;
			zero.table_data.column.headers[size_t(c)].has_width = true;
			zero.table_data.column.headers[size_t(c)].width = c == 0 ? 0 : 50;
			zero.table_data.column.headers[size_t(c)].text = c == 0 ? "Hidden" : "Shown";
		}
		root.children = {spin, list, empty_table, wide, marquee, zero};
		rig.configure(root);
		const std::vector<MenuFrameNote> notes = rig.compiler.layout_notes(rig.state);
		TEST_EXPECT(is(find(notes, Code::SpinArrowEmpty), 1, "spindown", 0, "position.right", "spindown"));
		TEST_EXPECT(is(find(notes, Code::ListRowsClipped), 2, "", -1, "", "1"));
		TEST_EXPECT(count(notes, Code::TableNoColumns) == 1);
		TEST_EXPECT(is(find(notes, Code::TableNoColumns), 3, "", -1, ""));
		TEST_EXPECT(is(find(notes, Code::TableHeaderClipped), 4, "", -1, "", "Two"));
		TEST_EXPECT(is(find(notes, Code::MarqueeRuntimeContent), 5, "datasource", 0, "", "credits.txt"));
		TEST_EXPECT(count(notes, Code::TableHeaderWidthZero) == 1);
		TEST_EXPECT(is(find(notes, Code::TableHeaderWidthZero), 6, "column.header", 0, "width", "Hidden"));
	}
	return 0;
}

// The notes observe. configure()'s hooks are const (they write only the note log), and this
// screen compiles to the draw list the compiler drew before it had notes: the golden is
// the S9j1 compiler's (a one-off build of that code, 2026-09-24, design scale, the hash
// below). layout_notes and add_load_note leave the next compile as it was, and two
// configures note the same.
static int test_notes_only_observe() {
	Rig rig;
	Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
	with_font(root);
	root.draw_frame = true;
	root.frame.stencil = "border.tga";
	root.frame.brush = "tile.tga";
	root.appearances = {appearance("DEFAULT", "COLOR", "FF102030"), appearance("DEFAULT", "COLOR", "FF0000")};
	Window cut = window(WindowType::Button, "CUT", 20, 20, 60, 40);
	with_text(cut, "A long label");
	cut.appearances = {appearance("DEFAULT", "IMAGE", "sheet.tga"), appearance("MOUSEOVER", "OUTLINE", "FFFFFFFF")};
	cut.appearances[0].has_map_state = true;
	cut.appearances[0].map_state = 1;
	cut.appearances[0].has_height = true;
	cut.appearances[0].height = 20;
	Window list = window(WindowType::List, "LIST", 0, 100, 200, 140);
	list.items.present = true;
	list.items.items.resize(4);
	for (auto &item : list.items.items) item.text = "row";
	root.children = {cut, list};
	rig.configure(root, {{"border.tga", {64, 64}}, {"tile.tga", {32, 32}}, {"sheet.tga", {64, 80}}});
	MenuWidgetState hover;
	hover.index = 1;
	hover.hovered = true;
	rig.state.widgets.push_back(hover);
	const uint64_t design = golden(rig.compiler.compile(rig.state, 1.0f, 1.0f));
	if (design != 0x9a964873881e7ce7ull) std::fprintf(stderr, "  golden %016llx\n", (unsigned long long)design);
	TEST_EXPECT(design == 0x9a964873881e7ce7ull);
	const std::string before = digest(rig.compiler.compile(rig.state, 1.6f, 1.2f));
	const std::vector<MenuFrameNote> first = rig.notes();
	TEST_EXPECT(!first.empty());
	rig.compiler.add_load_note(Code::TextureMissing, "sheet.tga");
	(void)rig.compiler.layout_notes(rig.state);
	TEST_EXPECT(digest(rig.compiler.compile(rig.state, 1.6f, 1.2f)) == before);
	// Configured again over the same screen: the same notes.
	Window again = rig.screen.roots[0];
	rig.configure(again, {{"border.tga", {64, 64}}, {"tile.tga", {32, 32}}, {"sheet.tga", {64, 80}}});
	TEST_EXPECT(rig.notes() == first);
	TEST_EXPECT(digest(rig.compiler.compile(rig.state, 1.6f, 1.2f)) == before);
	return 0;
}

// The drag planner's queries: a window's parent and its rect under another POSITION, the
// same three-stage solve the draw uses.
static int test_solve_local_rect() {
	Rig rig;
	Window root = window(WindowType::Window, "ROOT", 0, 0, 800, 600);
	Window child = window(WindowType::Static, "CHILD", 10, 10, 50, 30);
	child.appearances = {appearance("DEFAULT", "IMAGE", "pic.tga")};
	root.children = {child};
	rig.configure(root, {{"pic.tga", {40, 16}}});
	TEST_EXPECT(rig.compiler.widget_parent(1) == 0 && rig.compiler.widget_parent(0) == -1);
	opennova::mnu::RectEdges rect;
	TEST_EXPECT(rig.compiler.solve_local_rect(1, window(WindowType::Static, "", 5, 6, 25, 16).position, &rect));
	TEST_EXPECT(rect.left == 5 && rect.top == 6 && rect.right == 25 && rect.bottom == 16);
	// Only the leading edges: the image's size fills the rest.
	opennova::mnu::Position lead;
	lead.has_left = lead.has_top = true;
	lead.left = 7;
	lead.top = 3;
	TEST_EXPECT(rig.compiler.solve_local_rect(1, lead, &rect));
	TEST_EXPECT(rect.left == 7 && rect.top == 3 && rect.right == 47 && rect.bottom == 19);
	TEST_EXPECT(!rig.compiler.solve_local_rect(2, lead, &rect) && !rig.compiler.solve_local_rect(-1, lead, &rect));
	return 0;
}

int main() {
	const std::vector<std::pair<const char *, std::function<int()>>> cases = {
	        {"tokens", test_tokens},
	        {"configure_notes", test_configure_notes},
	        {"loader_notes", test_loader_notes},
	        {"layout_notes", test_layout_notes},
	        {"notes_only_observe", test_notes_only_observe},
	        {"solve_local_rect", test_solve_local_rect},
	};
	int failures = 0;
	for (const auto &entry : cases) {
		const int result = entry.second();
		std::printf("%s %s\n", result == 0 ? "PASS" : "FAIL", entry.first);
		failures += result;
	}
	return failures == 0 ? 0 : 1;
}
