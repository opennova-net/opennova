// ADR 0046 S18, the external program round trip (edit_externally, refresh_changed_sources): a plain TGA
// given a source once (a copy in art/ under a name of its own, its record reproducing it, the TGA set
// aside) and opened (the open_externally view event naming the source on disk); an import's output's own
// source opened, nothing made; a change a program saves there imported by refresh_changed_sources once
// it settles (never read while it may be half-written), that source alone, the scan read again for it
// alone, the open texture document of the output read again, the import cache keeping the other
// sources; nothing changed, nothing done; a file an import read moving its source; the tab's open of a
// source (open_texture_source: an output's opened, a plain texture's refused until one is made, a PNG with
// unsaved edits refused); a PNG edited in place by its program read again; a file that is no texture
// refused; a name the project lacks refused.
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>

#include <base/io/file_time.h>
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
// The same 4 x 4 texels as a PNG: what a source in art/ holds.
std::vector<uint8_t> png32(uint8_t r, uint8_t g, uint8_t b) {
	std::vector<uint8_t> rgba;
	for (int i = 0; i < 16; ++i) rgba.insert(rgba.end(), {r, g, b, 255});
	return encode_png_rgba(rgba.data(), 4, 4);
}

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
	TEST_EXPECT(editor_test::write_bytes(root + "/textures/rock.tga", tga32(200, 0, 0)) &&
	            editor_test::write_bytes(root + "/textures/moss.tga", tga32(0, 0, 200)));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	editor_test::handle_to_end(session, request::open_document("textures/rock.tga"));
	const auto changes = [&] {
		return external_changes(ProjectPaths::for_root(root), *view.project.scan,
		                        view.project.imports ? *view.project.imports : std::vector<ImportedSource>(),
		                        io::file_clock_now_ticks());
	};

	// A plain TGA: its source made once, the TGA set aside (its clean document closed), the source opened.
	editor_test::handle_to_end(session, request::edit_externally("textures/rock.tga"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();
	TEST_EXPECT(opened(view) == root + "/art/rock.png");
	const AssetEntry *rock = view.project.scan->find("rock.tga");
	TEST_EXPECT(rock && rock->imported_from == "art/rock.png" && !fs::exists(root + "/textures/rock.tga") &&
	            !session.document_base_for("textures/rock.tga"));
	if (!rock) return 1;
	const std::string output = rock->relative_path;
	editor_test::handle_to_end(session, request::open_document(output));
	TEST_EXPECT(first_texel(session.document_base_for(output)) == std::vector<uint8_t>({200, 0, 0, 255}));

	// A second texture given its source: two sources, of which the program saves one.
	editor_test::handle_to_end(session, request::edit_externally("textures/moss.tga"));
	TEST_EXPECT(session.outcome().done());
	session.run_operations();

	// Nothing changed: nothing done.
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == 0);
	TEST_EXPECT(changes().empty());

	// Its program is saving the source: written just now, it waits, never read half-written.
	TEST_EXPECT(editor_test::write_bytes(root + "/art/rock.png", png32(0, 200, 0)));
	const ExternalChanges waiting = changes();
	TEST_EXPECT(waiting.empty() && waiting.unsettled == 1);
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation == 0);
	// Settled (its last write a while back): that source alone imported again, the scan read again for it
	// alone (no walk of the project), its output's open document read again.
	TEST_EXPECT(editor_test::backdate(root + "/art/rock.png", std::chrono::seconds(60)));
	TEST_EXPECT(changes().sources == std::vector<std::string>({"art/rock.png"}) &&
	            changes().files == std::vector<std::string>({"art/rock.png"}));
	editor_test::handle_to_end(session, request::refresh_changed_sources());
	TEST_EXPECT(session.outcome().done() && session.outcome().operation != 0);
	session.run_operations();
	TEST_EXPECT(first_texel(session.document_base_for(output)) == std::vector<uint8_t>({0, 200, 0, 255}));
	TEST_EXPECT(session.files_scanned() <= 2 && changes().empty());
	// The import cache kept what it knew of the other source: a whole Rescan imports nothing again.
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	size_t imported_again = 0;
	for (const ImportedSource &source : *view.project.imports) imported_again += source.reimported ? 1 : 0;
	TEST_EXPECT(view.project.imports->size() == 2 && imported_again == 0);

	// A file an import read (an input) moved: its source imported again.
	{
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/grain.tga", tga32(1, 1, 1)));
		editor_test::handle_to_end(session, request::rescan());
		session.run_operations();
		std::vector<uint8_t> bigger = tga32(2, 2, 2);
		bigger.push_back(0);
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/grain.tga", bigger) &&
		            editor_test::backdate(root + "/textures/grain.tga", std::chrono::seconds(60)));
		ImportedSource reads;
		reads.source = "art/moss.png";
		reads.inputs = {"textures/grain.tga"};
		const ExternalChanges input = external_changes(ProjectPaths::for_root(root), *view.project.scan, {reads},
		                                               io::file_clock_now_ticks());
		TEST_EXPECT(input.sources == std::vector<std::string>({"art/moss.png"}) &&
		            input.files == std::vector<std::string>({"textures/grain.tga"}));
	}

	// An output's own source opened, nothing made.
	const size_t entries = view.project.scan->entries.size();
	editor_test::handle_to_end(session, request::edit_externally("rock.tga"));
	TEST_EXPECT(session.outcome().done() && opened(view) == root + "/art/rock.png");
	session.run_operations();
	TEST_EXPECT(view.project.scan->entries.size() == entries);

	// The tab's "Edit in its program" on a texture with a source of its own: open_texture_source opens it,
	// nothing made; one with none is refused (the dialog's preview asks before making one).
	editor_test::handle_to_end(session, request::open_texture_source("rock.tga"));
	TEST_EXPECT(session.outcome().done() && opened(view) == root + "/art/rock.png");
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
		// Its program saves it: watched as any source is, the scan read again for it alone, its open document
		// read again.
		std::vector<uint8_t> red;
		for (int i = 0; i < 16; ++i) red.insert(red.end(), {220, 10, 10, 255});
		const std::vector<uint8_t> saved = encode_png_rgba(red.data(), 4, 4);
		TEST_EXPECT(editor_test::write_bytes(root + "/textures/pic.png", saved) &&
		            editor_test::backdate(root + "/textures/pic.png", std::chrono::seconds(60)));
		TEST_EXPECT(changes().files == std::vector<std::string>({"textures/pic.png"}) && changes().sources.empty());
		editor_test::handle_to_end(session, request::refresh_changed_sources());
		TEST_EXPECT(session.outcome().done() && session.outcome().operation != 0);
		session.run_operations();
		TEST_EXPECT(first_texel(session.document_base_for("textures/pic.png")) == std::vector<uint8_t>({220, 10, 10, 255}) &&
		            session.files_scanned() == 1 && changes().empty());
	}
	// A file that is no texture (an item table): its program is no paint program, refused.
	{
		TEST_EXPECT(editor_test::write_text(root + "/defs/items.def", "begin \"Brick\"\nid 100300\ntype building\nend\n"));
		editor_test::handle_to_end(session, request::rescan());
		session.run_operations();
		session.handle(request::edit_externally("defs/items.def"));
		TEST_EXPECT(session.outcome().refused && session.outcome().findings.back().message.find("no texture") != std::string::npos);
		session.handle(request::open_texture_source("defs/items.def"));
		TEST_EXPECT(session.outcome().refused);
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
