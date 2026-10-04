// ADR 0046 S18, the texture document's edits (documents/texture_operations, the texture_operation request):
// each whole-image operation one undo step, the file made anew through the editor's writers in the form it
// is stored in (a resize, an alpha, a stored form, an upside-down TGA's rows, an 8-bit PCX's palette
// indices); Undo and Redo between the versions, Save writing the version it is at; a file an import makes
// refused with its words, an operation it cannot do refused with nothing changed; the Problems fix
// "Re-save bottom first" on texture.tga_upside_down.
#include <cstdio>
#include <string>
#include <vector>

#include <editor/documents/texture_document.h>
#include <editor/documents/texture_image.h>
#include <editor/documents/texture_operations.h>
#include <editor/import/png_encode.h>
#include <editor/import/sidecar.h>
#include <editor/import/texture_import.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/problem_fixes.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/dds/dds.h>
#include <formats/pcx/pcx_io.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;

namespace {

// A `w` x `h` image of graded colours, its alpha falling left to right.
std::vector<uint8_t> graded(int w, int h) {
	std::vector<uint8_t> out;
	for (int y = 0; y < h; ++y)
		for (int x = 0; x < w; ++x) {
			out.push_back(uint8_t(x * 255 / std::max(1, w - 1)));
			out.push_back(uint8_t(y * 255 / std::max(1, h - 1)));
			out.push_back(uint8_t((x + y) * 16));
			out.push_back(uint8_t(255 - x * 255 / std::max(1, w - 1)));
		}
	return out;
}

std::vector<uint8_t> tga32(const std::vector<uint8_t> &rgba, int w, int h) {
	std::vector<uint8_t> out;
	std::string why;
	tga::tga_write_rgba32(rgba.data(), uint32_t(w), uint32_t(h), out, why);
	return out;
}

std::vector<uint8_t> first_level(const std::string &name, const std::vector<uint8_t> &bytes, uint32_t &w, uint32_t &h) {
	const std::shared_ptr<const TextureImage> image = decode_texture(name, bytes);
	if (!image || !image->loads || image->levels.empty()) return {};
	w = image->levels[0].width;
	h = image->levels[0].height;
	return image->levels[0].rgba;
}

TextureOperation op(TextureOperationKind kind, std::vector<std::pair<std::string, std::string>> params = {}) {
	return {kind, std::move(params)};
}

int test_operations() {
	using K = TextureOperationKind;
	const std::vector<uint8_t> image = graded(8, 8);
	const std::vector<uint8_t> file = tga32(image, 8, 8);
	std::vector<uint8_t> out;
	std::string words, why;
	uint32_t w = 0, h = 0;
	// Halved: each texel the 2 x 2 box of the four under it.
	TEST_EXPECT(apply_texture_operation("a.tga", file, op(K::Resize, {{"size", "4x4"}}), out, words, why) && words == "Resized to 4 x 4");
	std::vector<uint8_t> texels = first_level("a.tga", out, w, h);
	TEST_EXPECT(w == 4 && h == 4 && texels.size() == 64 &&
	            texels[0] == uint8_t((uint32_t(image[0]) + image[4] + image[32] + image[36]) >> 2));
	TEST_EXPECT(!apply_texture_operation("a.tga", file, op(K::Resize, {{"size", "8x8"}}), out, words, why) &&
	            why.find("already") != std::string::npos);
	TEST_EXPECT(!apply_texture_operation("a.tga", file, op(K::Resize), out, words, why));
	// The alpha: opaque, inverted; a 24-bit TGA holds none.
	TEST_EXPECT(apply_texture_operation("a.tga", file, op(K::Alpha, {{"alpha", "opaque"}}), out, words, why));
	texels = first_level("a.tga", out, w, h);
	TEST_EXPECT(texels.size() == 256 && texels[3] == 255 && texels[255] == 255);
	TEST_EXPECT(apply_texture_operation("a.tga", file, op(K::Alpha, {{"alpha", "invert"}}), out, words, why));
	texels = first_level("a.tga", out, w, h);
	TEST_EXPECT(texels.size() == 256 && texels[3] == uint8_t(255 - image[3]));
	std::vector<uint8_t> rgb;
	TEST_EXPECT(apply_texture_operation("a.tga", file, op(K::Format, {{"format", "tga24"}}), rgb, words, why) &&
	            words == "Stored as a 24-bit TGA");
	tga::TgaHeader header;
	TEST_EXPECT(tga::tga_read_header(rgb.data(), rgb.size(), header) && header.bits == 24);
	TEST_EXPECT(!apply_texture_operation("a.tga", rgb, op(K::Alpha, {{"alpha", "opaque"}}), out, words, why) &&
	            why.find("holds no alpha") != std::string::npos);
	TEST_EXPECT(!apply_texture_operation("a.tga", rgb, op(K::Format, {{"format", "tga24"}}), out, words, why) &&
	            why.find("already") != std::string::npos);
	// Another extension is another file.
	TEST_EXPECT(!apply_texture_operation("a.tga", file, op(K::Format, {{"format", "dds"}}), out, words, why) &&
	            why.find("rename") != std::string::npos);
	// A DDS's compression, its chain kept.
	std::vector<uint8_t> dds;
	TEST_EXPECT(dds::dds_write_a8r8g8b8(image.data(), 8, 8, dds, why));
	TEST_EXPECT(apply_texture_operation("a.dds", dds, op(K::Format, {{"dds", "dxt5"}, {"mips", "full"}}), out, words, why));
	dds::DdsImage read;
	TEST_EXPECT(dds::dds_read(out.data(), out.size(), read, why) && std::string(read.format.name) == "DXT5" && read.levels.size() == 4);
	// The rows of a TGA stored top first: written bottom first, the way up its header meant.
	std::vector<uint8_t> upside = file;
	upside[17] |= 0x20;
	TEST_EXPECT(apply_texture_operation("u.tga", upside, op(K::ReorderRows), out, words, why) && words == "Rows saved bottom first");
	TEST_EXPECT(tga::tga_read_header(out.data(), out.size(), header) && !(header.descriptor & 0x20));
	texels = first_level("u.tga", out, w, h);
	const std::vector<uint8_t> game = first_level("u.tga", upside, w, h);
	TEST_EXPECT(texels.size() == 256 && game.size() == 256 &&
	            std::equal(texels.begin(), texels.begin() + 32, game.end() - 32)); // the game's last row now first
	TEST_EXPECT(!apply_texture_operation("a.tga", file, op(K::ReorderRows), out, words, why) &&
	            why.find("bottom first already") != std::string::npos);
	// An 8-bit PCX's indices moved, the palette as it is.
	IndexedImage8 indexed;
	indexed.width = 2;
	indexed.height = 2;
	indexed.indices = {1, 2, 2, 3};
	for (int i = 0; i < 256; ++i) indexed.palette[i][0] = indexed.palette[i][1] = indexed.palette[i][2] = uint8_t(i);
	std::vector<uint8_t> pcx;
	TEST_EXPECT(encode_pcx_indexed(indexed, pcx, why));
	TEST_EXPECT(apply_texture_operation("f.pcx", pcx, op(K::RemapPalette, {{"2", "9"}}), out, words, why) &&
	            words == "Palette indices 2 to 9");
	PcxIndexed moved;
	RgbaImage colours;
	TEST_EXPECT(decode_pcx_menu_rgba(out.data(), out.size(), colours, why, &moved) && moved.indices.size() >= 4 &&
	            moved.indices[0] == 1 && moved.indices[1] == 9 && moved.indices[2] == 9 && moved.indices[3] == 3);
	TEST_EXPECT(!apply_texture_operation("f.pcx", pcx, op(K::RemapPalette, {{"2", "300"}}), out, words, why));
	TEST_EXPECT(!apply_texture_operation("a.tga", file, op(K::RemapPalette, {{"2", "9"}}), out, words, why));
	TextureOperationKind kind;
	TEST_EXPECT(texture_operation_kind("reorder_rows", kind) && kind == K::ReorderRows && !texture_operation_kind("blur", kind));
	std::printf("operations: a resize, the alphas, the stored forms, the rows, a palette's indices, the refusals\n");
	return 0;
}

const TextureDocument *texture_of(ProjectSession &session, const std::string &path) {
	return dynamic_cast<const TextureDocument *>(session.document_base_for(path));
}

int test_session() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_edit"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Edit"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	const std::vector<uint8_t> image = graded(8, 8);
	std::vector<uint8_t> upside = tga32(image, 8, 8);
	upside[17] |= 0x20;
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/a.tga", tga32(image, 8, 8)) &&
	            editor_test::write_bytes(root + "/textures/up.tga", upside));
	const std::vector<uint8_t> png = encode_png_rgba(image.data(), 8, 8);
	ImportSidecar record;
	record.importer = "image";
	record.version = kImageImporterVersion;
	Diagnostic error;
	TEST_EXPECT(editor_test::write_bytes(root + "/art/src.png", png) &&
	            save_import_sidecar(root + "/art/src.png" + kImportSidecarSuffix, record, error));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(view.project.scan->find("src.tga") && !view.project.scan->find("src.tga")->imported_from.empty());

	// A resize, one step: the document dirty at 4 x 4; Undo back to the file as read, Redo again; Save writes it.
	editor_test::handle_to_end(session, request::texture_operation("textures/a.tga", "resize", {{"size", "4x4"}}, true));
	TEST_EXPECT(session.outcome().done());
	const TextureDocument *a = texture_of(session, "textures/a.tga");
	TEST_EXPECT(a && a->dirty() && a->can_undo() && a->image() && a->image()->width() == 4);
	if (!a) return 1;
	TEST_EXPECT(a->undo_words() == "Resized to 4 x 4 (a.tga)" && view.activity.status == "Resized to 4 x 4 (a.tga).");
	editor_test::handle_to_end(session, request::undo("textures/a.tga"));
	TEST_EXPECT(!a->dirty() && a->image()->width() == 8 && a->can_redo());
	editor_test::handle_to_end(session, request::redo("textures/a.tga"));
	TEST_EXPECT(a->dirty() && a->image()->width() == 4);
	editor_test::handle_to_end(session, request::save("textures/a.tga"));
	TEST_EXPECT(!a->dirty());
	{
		std::vector<uint8_t> saved;
		std::string error;
		uint32_t w = 0, h = 0;
		TEST_EXPECT(read_file_bytes(root + "/textures/a.tga", saved, error) && !first_level("a.tga", saved, w, h).empty() && w == 4);
	}
	// A file an import makes: its import's options make it, never an edit.
	editor_test::handle_to_end(session, request::texture_operation("src.tga", "alpha", {{"alpha", "opaque"}}, true));
	TEST_EXPECT(session.outcome().refused && !session.outcome().findings.empty() &&
	            session.outcome().findings[0].code() == "texture.operation" &&
	            session.outcome().findings[0].message.find("made from art/src.png") != std::string::npos);
	// What it cannot do: refused, nothing changed.
	const uint64_t revision = a->revision();
	editor_test::handle_to_end(session, request::texture_operation("textures/a.tga", "blur", {}, true));
	TEST_EXPECT(session.outcome().refused && a->revision() == revision);
	editor_test::handle_to_end(session, request::texture_operation("textures/a.tga", "reorder_rows", {}, true));
	TEST_EXPECT(session.outcome().refused && a->revision() == revision);

	// Problems: the upside-down TGA's fix re-saves its rows bottom first, one step of its document.
	session.run_operations();
	const Diagnostic *upside_down = nullptr;
	for (const Diagnostic &d : view.findings.diagnostics)
		if (d.code() == "texture.tga_upside_down" && d.asset == "textures/up.tga") upside_down = &d;
	TEST_EXPECT(upside_down);
	if (!upside_down) return 1;
	const std::vector<ProblemFix> fixes = fixes_for(*upside_down, view);
	TEST_EXPECT(fixes.size() == 1 && fixes[0].label == "Save up.tga bottom first" &&
	            fixes[0].request.kind == EditorRequestKind::TextureOperation && fixes[0].request.operation == "reorder_rows");
	if (fixes.empty()) return 1;
	editor_test::handle_to_end(session, fixes[0].request);
	const TextureDocument *up = texture_of(session, "textures/up.tga");
	TEST_EXPECT(session.outcome().done() && up && up->dirty() && up->image() && !up->image()->upside_down);
	editor_test::handle_to_end(session, request::save("textures/up.tga"));
	session.run_operations();
	for (const Diagnostic &d : view.findings.diagnostics) TEST_EXPECT(d.code() != "texture.tga_upside_down");
	std::printf("session: a resize undone, redone and saved; an import's output refused; Problems' row fix\n");
	return 0;
}

} // namespace

int main() {
	int failures = test_operations();
	failures += test_session();
	if (failures == 0) std::printf("editor_texture_document_edit: all passed\n");
	return failures == 0 ? 0 : 1;
}
