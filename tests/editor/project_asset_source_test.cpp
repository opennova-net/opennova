// The project's files as the engine looks them up (editor/assets/project_asset_source.h):
// every file the build packs by its flat logical name, compared without case, the
// subfolders and the import outputs under .opennova/ included (an import source is not);
// payloads decoded like a document's (an SCR file reads decrypted); an open document
// stands in for its file with the bytes its Save would write, its stamp following its
// revision; a file added outside the editor resolves after a rescan.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <editor/import/importer.h>
#include <editor/import/sidecar.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/session_view.h>
#include <formats/rtxt/rtxt.h>
#include <formats/scr/scr.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"
#include "editor/png_test_support.h"

using namespace opennova::editor;

namespace {

using editor_test::NoProcess;

bool file_bytes(const std::string &path, std::vector<uint8_t> &out) {
	std::string error;
	return read_file_bytes(path, out, error);
}

// The retail SCR container over `plain` (the codec reverses, then XORs; this is its
// inverse): what a packed .def looks like on disk.
std::vector<uint8_t> scr_encoded(const std::string &plain, uint32_t key) {
	std::vector<uint8_t> body(plain.rbegin(), plain.rend());
	opennova::scr::scr_decrypt(body.data(), body.size(), key); // the plain bytes XORed
	std::reverse(body.begin(), body.end());
	std::vector<uint8_t> out = {'S', 'C', 'R', 1};
	out.insert(out.end(), body.begin(), body.end());
	return out;
}

bool has_section(const std::vector<uint8_t> &bytes, const std::string &section) {
	opennova::rtxt::File table;
	std::string error;
	if (!opennova::rtxt::parse(bytes.data(), bytes.size(), table, error)) return false;
	for (const auto &s : table.sections)
		if (s.name == section) return true;
	return false;
}

} // namespace

static int test_names_decode_and_rescan() {
	editor_test::TempProjectDir dir("opennova_editor_asset_source");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Assets"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	TEST_EXPECT(view.assets != nullptr);
	const ProjectAssetSource &assets = *view.assets;
	const std::string root = view.project_root;

	// Flat, case-blind names; the folder is organization only.
	const AssetEntry *menu = view.scan.find("main.mnu");
	TEST_EXPECT(menu && menu->relative_path.find('/') != std::string::npos);
	TEST_EXPECT(assets.path_of("MAIN.MNU") == menu->relative_path);
	TEST_EXPECT(assets.path_of("menus/Main.mnu") == menu->relative_path); // a path reads as its name
	TEST_EXPECT(assets.stamp("main.mnu") != 0 && assets.stamp("absent.tga") == 0);
	std::vector<uint8_t> read, disk;
	TEST_EXPECT(assets.read("Main.Mnu", read) && file_bytes(root + "/" + menu->relative_path, disk) && read == disk);
	TEST_EXPECT(!assets.read("absent.tga", read));

	// A file added outside the editor resolves after a rescan; an SCR payload reads decoded.
	const std::string plain = "WEAPON\r\nNAME test\r\n";
	TEST_EXPECT(editor_test::write_bytes(root + "/notes/secret.txt", scr_encoded(plain, opennova::scr::SCR_KEY_JO_DFX2)));
	TEST_EXPECT(assets.stamp("secret.txt") == 0);
	const uint64_t before = assets.generation();
	session.handle(request::rescan());
	TEST_EXPECT(assets.generation() != before && assets.stamp("secret.txt") != 0);
	TEST_EXPECT(assets.read("SECRET.TXT", read) && std::string(read.begin(), read.end()) == plain);

	// An import output under .opennova/imported/ resolves by its own name.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/logo.png", editor_test::gradient_png(8, 8)));
	const Importer *importer = importer_for(root + "/art/logo.png");
	TEST_EXPECT(importer != nullptr);
	ImportSidecar sidecar;
	sidecar.importer = importer->id;
	sidecar.version = importer->version;
	sidecar.options = importer->default_options;
	Diagnostic error;
	TEST_EXPECT(save_import_sidecar(root + "/art/logo.png.import", sidecar, error));
	// A PNG with no .import record is a texture the build packs as it is.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/plain.png", editor_test::gradient_png(4, 4)));
	session.handle(request::rescan());
	TEST_EXPECT(assets.path_of("logo.pcx").find(".opennova/imported/") == 0);
	TEST_EXPECT(assets.read("LOGO.PCX", read) && !read.empty());
	// The import source is not: the build never packs it, so the game never finds it.
	TEST_EXPECT(view.scan.find("logo.png") != nullptr);
	TEST_EXPECT(assets.stamp("logo.png") == 0 && assets.path_of("logo.png").empty());
	TEST_EXPECT(!assets.read("logo.png", read));
	TEST_EXPECT(assets.stamp("plain.png") != 0 && assets.read("plain.png", read) && !read.empty());

	// A changed file moves its stamp on the next rescan.
	const uint64_t stamp = assets.stamp("secret.txt");
	TEST_EXPECT(editor_test::write_text(root + "/notes/secret.txt", "a different, longer plain text file"));
	session.handle(request::rescan());
	TEST_EXPECT(assets.stamp("secret.txt") != stamp);
	std::printf("test_names_decode_and_rescan passed\n");
	return 0;
}

static int test_open_document_stands_in() {
	editor_test::TempProjectDir dir("opennova_editor_asset_source_open");
	NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	session.handle(request::new_project(dir.file("project"), "Open"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const ProjectAssetSource &assets = *view.assets;
	const uint64_t on_disk = assets.stamp("gametext.bin");
	TEST_EXPECT(on_disk != 0);

	session.handle(request::open_document("gametext.bin"));
	Document *strings = session.document_for("gametext.bin");
	TEST_EXPECT(strings != nullptr);
	const uint64_t opened = assets.stamp("gametext.bin");
	TEST_EXPECT(opened != 0 && opened != on_disk);

	// An edit, unsaved: the name reads the bytes Save would write, under a new stamp.
	EditorRequest add = request::edit_record(strings->path(), Edit());
	add.edits[0].operation = EditOperation::Add;
	add.edits[0].address = {0, strings->kind_from_name("section"), 0};
	session.handle(add);
	TEST_EXPECT(session.last_edit_ok());
	EditorRequest name = request::edit_record(strings->path(), Edit());
	name.edits[0].address = {strings->last_added(), strings->kind_from_name("section"), 0};
	name.edits[0].field = "name";
	name.edits[0].value = std::string("Unsaved");
	session.handle(name);
	TEST_EXPECT(session.last_edit_ok() && strings->dirty());
	const uint64_t edited = assets.stamp("gametext.bin");
	TEST_EXPECT(edited != opened);
	std::vector<uint8_t> bytes;
	TEST_EXPECT(assets.read("gametext.bin", bytes) && has_section(bytes, "Unsaved"));
	std::vector<uint8_t> disk;
	TEST_EXPECT(file_bytes(session.view().project_root + "/" + strings->path(), disk) && !has_section(disk, "Unsaved"));

	// Undone twice: the state (and so the stamp) it was opened in.
	session.handle(request::undo(strings->path()));
	session.handle(request::undo(strings->path()));
	TEST_EXPECT(assets.stamp("gametext.bin") == opened);
	TEST_EXPECT(assets.read("gametext.bin", bytes) && !has_section(bytes, "Unsaved"));

	// Closed: the file again.
	session.handle(request::close_document(strings->path()));
	TEST_EXPECT(assets.stamp("gametext.bin") == on_disk);

	// A closed project resolves nothing.
	session.handle(request::close_project());
	TEST_EXPECT(assets.stamp("gametext.bin") == 0 && assets.path_of("main.mnu").empty());
	std::printf("test_open_document_stands_in passed\n");
	return 0;
}

int main() {
	int failures = 0;
	failures += test_names_decode_and_rescan();
	failures += test_open_document_stands_in();
	return failures == 0 ? 0 : 1;
}
