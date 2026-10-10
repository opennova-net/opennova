// Fonts (ADR 0046, round S23 lane A): a .fnt as a record document over the engine's own reader and from-scratch
// writer (fnt_parse, fnt_write): the minted fixtures (fixtures/fnt, tests/fixtures/minimal_fnt_gen.cpp) read and
// written back byte for byte; the header and a glyph's page and rect edited and written; the findings; the blank;
// the font a sheet import makes, opened clean. The retail leg (OPENNOVA_JO_ASSETS): every shipped .fnt through the
// document and back, byte for byte, its pages' coloured texels kept.
#include <editor/blank/blank_factory.h>
#include <editor/documents/document_types.h>
#include <editor/documents/font_document.h>
#include <editor/preview/font_viewport.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <formats/fnt/fnt.h>

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "common/file_io.h"
#include "common/retail_paths.h"
#include "common/test_expect.h"
#include "common/test_paths.h"
#include "editor/test_platform.h"
#include "editor/viewport_test_support.h"

using namespace opennova;
using namespace opennova::editor;
using opennova::io::JsonValue;

namespace {

constexpr NodeKind kFont = node_kind(FontKind::Font);
constexpr NodeKind kGlyph = node_kind(FontKind::Glyph);

std::vector<uint8_t> fixture(const char *name) {
	return test_io::read_file(std::string(test_paths_repo_root(__FILE__)) + "/fixtures/fnt/" + name);
}

Edit set_edit(NodeAddress address, const char *field, Value value) {
	Edit edit;
	edit.address = address;
	edit.field = field;
	edit.value = std::move(value);
	return edit;
}

bool has_code(const std::vector<Diagnostic> &findings, const char *code) {
	return std::any_of(findings.begin(), findings.end(), [&](const Diagnostic &d) { return d.code() == code; });
}

NodeAddress glyph_at(const FontDocument &font, uint8_t byte) {
	const FontRow &row = *font.font_row();
	return {row.id, kGlyph, row.ids.lists[0][size_t(byte - 0x20)].id};
}

bool load(FontDocument &font, const std::vector<uint8_t> &bytes, Diagnostic &error) {
	return font.load_bytes(bytes, "fonts/synth.fnt", AssetKind::Font, "jo", error);
}

std::vector<uint8_t> saved(const FontDocument &font) {
	const SerializeResult result = font.serialize();
	return result.ok() ? std::vector<uint8_t>(result.text.begin(), result.text.end()) : std::vector<uint8_t>();
}

// The minted fonts through the document and back; their records as the reader read them.
int test_reads_and_writes_back() {
	for (const char *name : {"synth_1page.fnt", "synth_3page.fnt"}) {
		const std::vector<uint8_t> bytes = fixture(name);
		TEST_EXPECT(!bytes.empty());
		if (test_io::is_lfs_pointer(bytes)) continue;
		FontDocument font;
		Diagnostic error;
		TEST_EXPECT(load(font, bytes, error));
		TEST_EXPECT(font.font_row() && font.font_row()->glyphs.size() == 224 && font.issues().empty());
		TEST_EXPECT(saved(font) == bytes);
		TEST_EXPECT(validate_font_file(font).empty());
		Value value;
		TEST_EXPECT(font.get({font.font_row()->id, kFont, 0}, "pages", value) &&
		            std::get<int64_t>(value) == int64_t(font.font_row()->pages));
	}
	return 0;
}

// The header and a glyph through the table, written and read back by the engine's reader; the refusals.
int test_edits() {
	FontDocument font;
	Diagnostic error;
	TEST_EXPECT(load(font, fixture("synth_1page.fnt"), error));
	const NodeAddress row{font.font_row()->id, kFont, 0};
	const NodeAddress a = glyph_at(font, 'A');
	TEST_EXPECT(font.apply({set_edit(row, "design_width", int64_t(1024)), set_edit(row, "spacing", int64_t(-2)),
	                        set_edit(a, "x", int64_t(10)), set_edit(a, "y", int64_t(20)), set_edit(a, "width", int64_t(7)),
	                        set_edit(a, "height", int64_t(12))},
	                       error));
	const std::vector<uint8_t> bytes = saved(font);
	fnt::fnt_font_t read{};
	TEST_EXPECT(!bytes.empty() && fnt::fnt_parse(bytes.data(), bytes.size(), &read) == fnt::FNT_OK);
	int x0 = 0, y0 = 0, x1 = 0, y1 = 0;
	fnt::fnt_uv_to_pixels(&read.glyphs['A' - 0x20].uv, &x0, &y0, &x1, &y1);
	const bool header = read.design_width == 1024 && read.glyph_spacing == -2;
	fnt::fnt_free(&read);
	TEST_EXPECT(header && x0 == 10 && y0 == 20 && x1 == 17 && y1 == 32);
	Value value;
	TEST_EXPECT(font.get(row, "pages", value));
	TEST_EXPECT(font_glyph_advance(*font.font_row(), font.font_row()->glyphs['A' - 0x20]) == 3); // (7 - 3) * 0.78125
	TEST_EXPECT(font.record_title(a) == "0x41 A");
	// Refused: a rect past its page, a width past it, a design width of 0, a page past the format's 16; the derived
	// fields read only; the font keeps its glyphs.
	TEST_EXPECT(!font.apply(set_edit(a, "x", int64_t(250)), error));
	TEST_EXPECT(!font.apply(set_edit(a, "width", int64_t(300)), error));
	TEST_EXPECT(!font.apply(set_edit(row, "design_width", int64_t(0)), error));
	TEST_EXPECT(!font.apply(set_edit(a, "page", int64_t(16)), error));
	TEST_EXPECT(!font.apply(set_edit(row, "pages", int64_t(2)), error));
	Edit remove;
	remove.operation = EditOperation::Remove;
	remove.address = a;
	TEST_EXPECT(!font.apply(remove, error));
	return 0;
}

// The findings: a glyph on a page the font lacks (the save refuses it), one past its page, one of another height
// than the space; input a save leaves out.
int test_findings() {
	FontDocument font;
	Diagnostic error;
	TEST_EXPECT(load(font, fixture("synth_1page.fnt"), error));
	const NodeAddress row{font.font_row()->id, kFont, 0};
	TEST_EXPECT(font.apply({set_edit(glyph_at(font, 'B'), "page", int64_t(3)), set_edit(glyph_at(font, 'C'), "height", int64_t(40))},
	                       error));
	std::vector<Diagnostic> findings = validate_font_file(font);
	TEST_EXPECT(has_code(findings, "font.unserializable") && has_code(findings, "font.glyph_height"));
	TEST_EXPECT(!font.serialize().ok());
	(void)row;
	// A rect reaching past the page, set as the file's coordinates.
	std::vector<uint8_t> bytes = fixture("synth_1page.fnt");
	const float past = 1.25f;
	std::memcpy(&bytes[fnt::FNT_HEADER_SIZE + size_t('D' - 0x20) * fnt::FNT_GLYPH_SIZE + 12], &past, 4);
	// The header's unread words and bytes past the last page: listed, a save writes neither.
	bytes[20] = 7;
	bytes.push_back(1);
	FontDocument odd;
	TEST_EXPECT(load(odd, bytes, error));
	findings = validate_font_file(odd);
	TEST_EXPECT(has_code(findings, "font.glyph_outside") && has_code(findings, "font.ignored_input") && odd.issues().size() == 2);
	TEST_EXPECT(saved(odd).size() == bytes.size() - 1);
	return 0;
}

// The blank (the built-in bitmap font) opens clean; the kind's document is the font type.
int test_blank() {
	BlankRequest request;
	request.logical_name = "newfont.fnt";
	std::vector<uint8_t> bytes;
	Diagnostic error;
	TEST_EXPECT(make_blank(request, AssetKind::Font, bytes, error));
	FontDocument font;
	TEST_EXPECT(load(font, bytes, error) && saved(font) == bytes);
	TEST_EXPECT(!has_code(validate_font_file(font), "font.unserializable"));
	TEST_EXPECT(document_type_for(AssetKind::Font) && document_type_for(AssetKind::Font)->id == DocumentTypeId::Font);
	return 0;
}

// The font viewport (the Main role of the font type) through a session: the font open, its text laid out by the
// game's text engine (quads, each glyph drawn named), a Rebuild of the device; an option a SetViewport, the text laid
// out again with no Rebuild; a page with its glyphs; the hit of a glyph naming its record; an edit of the spacing
// laid out again (an Update); a font that cannot be made, Failed.
struct FontRig {
	editor_test::TempProjectDir dir{"opennova_editor_font_viewport"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::FakeDevices devices;
	const std::string font = "fonts/synth.fnt";
	std::string root() const { return dir.file("project"); }
	const SessionView &view() const { return session.view(); }
	const FontViewport *viewport() {
		return static_cast<const FontViewport *>(session.viewports().find(font, ViewportKind::Font));
	}
	void pump() {
		session.run_operations();
		devices.sync(session);
	}
	JsonValue query(const std::string &name, const std::string &args_json) {
		JsonValue args;
		std::string error;
		opennova::io::json_parse(args_json, args, error);
		return session.query(name, args, error);
	}
};

int test_viewport() {
	FontRig rig;
	editor_test::handle_to_end(rig.session, request::new_project(rig.root(), "Fonts"));
	editor_test::create_missing_files(rig.session);
	TEST_EXPECT(editor_test::write_bytes(rig.root() + "/" + rig.font, fixture("synth_1page.fnt")));
	editor_test::handle_to_end(rig.session, request::rescan());
	editor_test::handle_to_end(rig.session, request::open_document(rig.font));
	TEST_EXPECT(rig.session.outcome().done() && rig.view().documents.active == rig.font);
	rig.pump();
	const FontViewport *viewport = rig.viewport();
	TEST_EXPECT(viewport && viewport->status() == ViewportStatus::Ready && viewport->shown_page() == -1);
	if (!viewport) return 1;
	TEST_EXPECT(!viewport->run().quads.empty() && !viewport->glyphs().empty());
	editor_test::FakeDevice *device = rig.devices.held(rig.font, ViewportKind::Font);
	TEST_EXPECT(device && device->last() == ViewportAction::Rebuild);
	// "AB": two quads, each naming its glyph; the colour halved as every caller halves it.
	FontViewportOptions options = viewport->options();
	options.text = "AB";
	options.color = 0xFF8040;
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.font, font_options_change(options)));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(viewport->run().quads.size() == 2 && viewport->glyphs().size() == 2 &&
	            viewport->glyphs()[0].glyph == size_t('A' - 0x20) && viewport->glyphs()[1].glyph == size_t('B' - 0x20));
	TEST_EXPECT(viewport->run().quads[0].color == 0xFF7F4020u);
	TEST_EXPECT(device->last() != ViewportAction::Rebuild);
	// The hit of A's middle (the picture's pixels, at the zoom) names its record.
	const FontPictureGlyph &a = viewport->glyphs()[0];
	const float zoom = float(options.zoom);
	const std::string at = "\"x\":" + std::to_string((a.x0 + a.x1) * 0.5f * zoom) + ",\"y\":" + std::to_string((a.y0 + a.y1) * 0.5f * zoom);
	const JsonValue hit = rig.query("viewport", "{\"path\":\"" + rig.font + "\",\"op\":\"hit\"," + at + "}");
	TEST_EXPECT(hit.get_string("kind", "") == "glyph" && hit.get_number("index", -1) == double('A' - 0x20) &&
	            hit.get_string("name", "").rfind("0x41 A: page 0", 0) == 0);
	// A page: its glyphs with their rects, the items the wire lists.
	options.page = 0;
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.font, font_options_change(options)));
	rig.pump();
	TEST_EXPECT(viewport->shown_page() == 0 && viewport->glyphs().size() > 50 && viewport->run().quads.empty());
	const JsonValue state = rig.query("viewport", "{\"path\":\"" + rig.font + "\",\"op\":\"state\"}");
	TEST_EXPECT(state.get_string("kind", "") == "font" && state.get("body") && state.get("body")->get_number("pages", 0) == 1);
	// Refused: a zoom past 8, an option it has not.
	options.page = -1;
	options.zoom = 9;
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.font, font_options_change(options)));
	TEST_EXPECT(!rig.session.outcome().done());
	editor_test::handle_to_end(rig.session, request::set_viewport(rig.font, "{\"kind\":\"font\",\"options\":{\"size\":3}}"));
	TEST_EXPECT(!rig.session.outcome().done());
	// The spacing edited: the text laid out again over the same pages (no Rebuild).
	const FontDocument *document = dynamic_cast<const FontDocument *>(rig.session.document_base_for(rig.font));
	TEST_EXPECT(document != nullptr);
	if (!document) return 1;
	const uint64_t laid = viewport->layout_serial();
	const size_t taken = device->taken.size();
	editor_test::handle_to_end(rig.session, request::edit_record(rig.font, {set_edit({document->font_row()->id, kFont, 0},
	                                                                                 "spacing", int64_t(4))}));
	TEST_EXPECT(rig.session.outcome().done());
	rig.pump();
	TEST_EXPECT(viewport->layout_serial() > laid);
	bool rebuilt = false;
	for (const ViewportAction action : device->since(taken)) rebuilt |= action == ViewportAction::Rebuild;
	TEST_EXPECT(!rebuilt);
	// A glyph on a page the font lacks: the game could not draw it.
	editor_test::handle_to_end(rig.session, request::edit_record(rig.font, {set_edit(glyph_at(*document, 'A'), "page", int64_t(5))}));
	rig.pump();
	TEST_EXPECT(viewport->status() == ViewportStatus::Failed && std::string(viewport->reason()) == "unwritable");
	return 0;
}

// The retail leg: every shipped font through the document and back, byte for byte.
int test_retail() {
	if (!retail::selected()) return 0;
	const std::string assets = retail::assets();
	if (assets.empty()) {
		retail::skip_leg("OPENNOVA_JO_ASSETS (the shipped .fnt files)");
		return 0;
	}
	size_t fonts = 0, coloured = 0;
	std::error_code ec;
	for (const auto &entry : std::filesystem::directory_iterator(std::filesystem::u8path(assets), ec)) {
		std::string ext = entry.path().extension().u8string();
		for (char &c : ext) c = char(std::tolower(static_cast<unsigned char>(c)));
		if (ext != ".fnt") continue;
		const std::vector<uint8_t> bytes = test_io::read_file(entry.path().u8string());
		FontDocument font;
		Diagnostic error;
		TEST_EXPECT(load(font, bytes, error));
		TEST_EXPECT(font.issues().empty() && saved(font) == bytes);
		TEST_EXPECT(!has_code(validate_font_file(font), "font.unserializable"));
		for (size_t i = 0; i + 3 < font.font_row()->texels->size(); i += 4)
			if ((*font.font_row()->texels)[i] != 0xFF) {
				++coloured;
				break;
			}
		++fonts;
	}
	TEST_EXPECT(fonts >= 30);
	std::printf("retail: %zu fonts through the document byte for byte (%zu with coloured pages)\n", fonts, coloured);
	return 0;
}

} // namespace

int main(int argc, char **argv) {
	retail::configure_mixed(argc, argv);
	int failed = 0;
	failed += test_reads_and_writes_back();
	failed += test_edits();
	failed += test_findings();
	failed += test_blank();
	failed += test_viewport();
	failed += test_retail();
	if (failed == 0) std::printf("editor font document: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
