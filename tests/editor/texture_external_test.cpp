// ADR 0046 S18, the external program round trip (edit_externally, refresh_changed_sources): a plain TGA
// given a source once (a copy in art/ under a name of its own, its record reproducing it, the TGA set
// aside) and opened (the open_externally view event naming the source on disk); an import's output's own
// source opened, nothing made; a change a program saves there imported by refresh_changed_sources, the
// open texture document of the output read again; nothing changed, nothing done; the tab's open of a
// source (open_texture_source: an output's opened, a plain texture's refused until one is made, a PNG with
// unsaved edits refused); a name the project lacks refused.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <editor/documents/texture_document.h>
#include <editor/import/import_run.h>
#include <editor/import/png_encode.h>
#include <editor/project/project_files.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>
#include <formats/tga/tga.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova;
using namespace opennova::editor;
namespace fs = std::filesystem;

namespace {

std::vector<uint8_t> tga32(uint8_t r, uint8_t g, uint8_t b) {
	std::vector<uint8_t> rgba;
	for (int i = 0; i < 16; ++i) rgba.insert(rgba.end(), {r, g, b, 255});
	std::vector<uint8_t> out;
	std::string why;
	tga::tga_write_rgba32(rgba.data(), 4, 4, out, why);
	return out;
}

// The path the last open_externally event names; "" for none.
std::string opened(const SessionView &view) {
	std::string path;
	for (const ViewEvent &event : view.events.held())
		if (event.kind == ViewEventKind::OpenExternally) path = event.path;
	return path;
}

std::vector<uint8_t> first_texel(const DocumentBase *document) {
	const auto *texture = dynamic_cast<const TextureDocument *>(document);
	if (!texture || !texture->image() || texture->image()->levels.empty()) return {};
	const std::vector<uint8_t> &rgba = texture->image()->levels[0].rgba;
	return std::vector<uint8_t>(rgba.begin(), rgba.begin() + 4);
}

int test_round_trip() {
	editor_test::TempProjectDir dir{"opennova_editor_texture_external"};
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session{platform, preferences};
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "External"));
	editor_test::create_missing_files(session);
	const SessionView &view = session.view();
	const std::string root = view.project.root;
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/rock.tga", tga32(200, 0, 0)));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	editor_test::handle_to_end(session, request::open_document("textures/rock.tga"));

	// A plain TGA: its source made once, the TGA set aside (its clean document closed), the source opened.
	editor_test::handle_to_end(session, request::edit_externally("textures/rock.tga"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	TEST_EXPECT(opened(view) == root + "/art/rock_src.tga");
	const AssetEntry *rock = view.project.scan->find("rock.tga");
	TEST_EXPECT(rock && rock->imported_from == "art/rock_src.tga" && !fs::exists(root + "/textures/rock.tga") &&
	            !session.document_base_for("textures/rock.tga"));
	if (!rock) return 1;
	const std::string output = rock->relative_path;
	editor_test::handle_to_end(session, request::open_document(output));
	TEST_EXPECT(first_texel(session.document_base_for(output)) == std::vector<uint8_t>({200, 0, 0, 255}));

	// Nothing changed: nothing done.
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == 0);
	TEST_EXPECT(changed_import_sources(ProjectPaths::for_root(root), *view.project.scan).empty());

	// Its program saves the source: the stamp moved, the import makes the texture again, its document read again.
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	TEST_EXPECT(editor_test::write_bytes(root + "/art/rock_src.tga", tga32(0, 200, 0)));
	TEST_EXPECT(changed_import_sources(ProjectPaths::for_root(root), *view.project.scan) ==
	            std::vector<std::string>({"art/rock_src.tga"}));
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation != 0);
	session.run_operations();
	TEST_EXPECT(first_texel(session.document_base_for(output)) == std::vector<uint8_t>({0, 200, 0, 255}));
	TEST_EXPECT(changed_import_sources(ProjectPaths::for_root(root), *view.project.scan).empty());

	// An output's own source opened, nothing made.
	const size_t entries = view.project.scan->entries.size();
	editor_test::handle_to_end(session, request::edit_externally("rock.tga"));
	TEST_EXPECT(session.outcome().done() && opened(view) == root + "/art/rock_src.tga");
	session.run_operations();
	TEST_EXPECT(view.project.scan->entries.size() == entries);

	// The tab's "Edit in its program" on a texture with a source of its own: open_texture_source opens it,
	// nothing made; one with none is refused (the dialog's preview asks before making one).
	editor_test::handle_to_end(session, request::open_texture_source("rock.tga"));
	TEST_EXPECT(session.outcome().done() && opened(view) == root + "/art/rock_src.tga");
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/plain.tga", tga32(9, 9, 9)));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	session.handle(request::open_texture_source("textures/plain.tga"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().message.find("has no source yet") != std::string::npos &&
	            fs::exists(root + "/textures/plain.tga"));
	session.handle(request::preview_texture_source("textures/plain.tga"));
	TEST_EXPECT(view.dialogs.texture_source.open && view.dialogs.texture_source.refusal.empty() &&
	            view.dialogs.texture_source.image.empty() && !view.dialogs.texture_source.changes.empty() &&
	            fs::exists(root + "/textures/plain.tga"));
	session.handle(request::cancel_texture_source());
	// A PNG the game reads as it is, edited in place: open with unsaved edits, its program is not given it.
	{
		std::vector<uint8_t> rgba(64, 120);
		const std::vector<uint8_t> png = encode_png_rgba(rgba.data(), 4, 4);
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/pic.png", png));
		editor_test::handle_to_end(session, request::rescan());
		session.run_operations();
		editor_test::handle_to_end(session, request::open_document("textures/pic.png"));
		editor_test::handle_to_end(session, request::texture_operation("textures/pic.png", "alpha", {{"alpha", "invert"}}, true));
		TEST_EXPECT(session.document_base_for("textures/pic.png") && session.document_base_for("textures/pic.png")->dirty());
		session.handle(request::open_texture_source("textures/pic.png"));
		TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().message.find("unsaved edits") != std::string::npos);
		editor_test::handle_to_end(session, request::save("textures/pic.png"));
		editor_test::handle_to_end(session, request::open_texture_source("textures/pic.png"));
		TEST_EXPECT(session.outcome().done() && opened(view) == root + "/textures/pic.png");
	}

	// A name the project lacks: refused.
	session.handle(request::edit_externally("nothing.tga"));
	TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().code() == "texture.external");
	// The Shell's own check (a background row) leaves the refusal's status line; a person's request clears it.
	const std::string refusal = view.activity.status;
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(!refusal.empty() && view.activity.status == refusal);
	editor_test::handle_to_end(session, request::select_file(output));
	TEST_EXPECT(view.activity.status != refusal);
	std::printf("external: a TGA's source made and opened, a change saved there imported and its document read "
	            "again, nothing done for nothing changed, an output's own source, a missing name, the Shell's "
	            "check leaving a refusal's line\n");
	return 0;
}

} // namespace

int main() {
	const int failures = test_round_trip();
	if (failures == 0) std::printf("editor_texture_external: all passed\n");
	return failures == 0 ? 0 : 1;
}
