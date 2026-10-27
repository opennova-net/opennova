// The game's particle parse hazard (documents/particle_token_slots.h): the line tokenizer's process-wide token table,
// whose slots past a line keep what earlier, longer lines left there or NULL, and the lists that read past their line
// into it (flags and move ending " ;" as the game's writer ends them, a pdefs list or a short table row whose ';'
// stands apart). The model: a flags list read into a slot no line filled (a fault), the same after a file of longer
// lines (none), a ';' attached ending the list, pdefs and a table row likewise. Through a session: two particle files
// whose names put the flags first, an error that gates the build (particle.unfilled_token_slot, the use check);
// renamed so the longer lines parse first, none. Through a validation whose paths read their files from a source of
// their own (ProjectPaths::files: the game install as the game serves it, the game's own data's check): the same fault
// from files the project's folder does not hold.
#include <base/vfs/file_source.h>
#include <editor/assets/asset_registry.h>
#include <editor/documents/particle_token_slots.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include <cstdio>
#include <filesystem>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

ParticleLoadFile file(const std::string &path, const std::string &text) {
	return {path, std::vector<uint8_t>(text.begin(), text.end())};
}

// A particle whose flags line the game's own writer would write (" ;" apart from the last name).
const std::string kFlags = "[particledef]\r\n{\r\n\tid = Dot;\r\n\tflags\t=  EMITVECTOR AMBIENTCOLOR ;\r\n}\r\n";

// A table of rows of 10 tokens (the key, '=', 8 values).
std::string table() {
	std::string out = "[tabledef]\r\n{\r\n\tid = grow;\r\n";
	for (int row = 1; row <= 32; ++row) out += "\ttl" + std::to_string(row) + " = 1, 2, 3, 4, 5, 6, 7, 8;\r\n";
	return out + "}\r\n";
}

int test_model() {
	// The flags first: slot 4 (past "flags", "=", two names) no line reached.
	std::vector<ParticleSlotFault> faults = particle_token_slot_faults({file("a.ptl", kFlags), file("b.ptl", table())});
	TEST_EXPECT(faults.size() == 1 && faults[0].file == 0 && faults[0].line == 4 && faults[0].key == "flags" &&
	            faults[0].slot == 4 && faults[0].filled == 4 && !faults[0].outside);
	// The table first: its rows filled slots 0 to 9, and slot 4 holds no flag name: the list stops.
	TEST_EXPECT(particle_token_slot_faults({file("b.ptl", table()), file("a.ptl", kFlags)}).empty());
	// In one file, a longer line before the flags guards them; after them, not.
	TEST_EXPECT(particle_token_slot_faults({file("c.ptl", table() + kFlags)}).empty());
	TEST_EXPECT(particle_token_slot_faults({file("c.ptl", kFlags + table())}).size() == 1);
	// A ';' attached to the last name ends the list there.
	TEST_EXPECT(particle_token_slot_faults(
	                    {file("a.ptl", "[particledef]\r\n{\r\n\tflags = EMITVECTOR AMBIENTCOLOR;\r\n\tmove = WANDER;\r\n}\r\n")})
	                    .empty());
	// The move list as the writer ends it, alike.
	faults = particle_token_slot_faults({file("a.ptl", "[particledef]\r\n{\r\n\tmove\t=  WANDER ;\r\n}\r\n")});
	TEST_EXPECT(faults.size() == 1 && faults[0].key == "move" && faults[0].slot == 3);
	// pdefs: its ';' attached ends the list; standing apart, the list reads on.
	TEST_EXPECT(particle_token_slot_faults({file("e.ptl", "[effectdef]\r\n{\r\n\tid = Puff;\r\n\tpdefs = Dot, Ring;\r\n}\r\n")})
	                    .empty());
	faults = particle_token_slot_faults({file("e.ptl", "[effectdef]\r\n{\r\n\tid = Puff;\r\n\tpdefs = Dot, Ring ;\r\n}\r\n")});
	TEST_EXPECT(faults.size() == 1 && faults[0].key == "pdefs" && faults[0].slot == 4);
	// A short table row whose ';' stands apart reads past its values; one with 8 values stops at 8.
	faults = particle_token_slot_faults({file("t.ptl", "[tabledef]\r\n{\r\n\tid = t1;\r\n\ttl1 = 1, 2 ;\r\n}\r\n")});
	TEST_EXPECT(faults.size() == 1 && faults[0].key == "tl1" && faults[0].slot == 4);
	TEST_EXPECT(particle_token_slot_faults({file("t.ptl", "[tabledef]\r\n{\r\n\tid = t1;\r\n\ttl1 = 1, 2, 3, 4, 5, 6, 7, 8 ;\r\n}\r\n")})
	                    .empty());
	// After the edit handles' header the game reads no more of the file.
	TEST_EXPECT(particle_token_slot_faults({file("h.ptl", "[tabledef_edithandles]\r\n{\r\n}\r\n" + kFlags)}).empty());
	// Lines end at CR LF alone: a file of LF lines is one line to the game, its first 30 tokens.
	TEST_EXPECT(particle_token_slot_faults({file("l.ptl", "[particledef]\n{\n\tflags = EMITVECTOR ;\n}\n")}).empty());
	std::printf("particle token slots: the flags fault at slot 4, guarded by a table parsed first\n");
	return 0;
}

int test_session() {
	test_temp::TempDir dir("opennova_editor_particle_token_slots");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Slots"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(test_io::write_text(root + "/particles/effects.ptl", kFlags) &&
	            test_io::write_text(root + "/particles/ontable.ptl", table()));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const auto rows = [&]() {
		std::vector<Diagnostic> out;
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == "particle.unfilled_token_slot") out.push_back(d);
		return out;
	};
	std::vector<Diagnostic> found = rows();
	TEST_EXPECT(found.size() == 1 && found[0].severity == DiagnosticSeverity::Error && found[0].row() &&
	            found[0].row()->gates_build && found[0].asset == "particles/effects.ptl" && found[0].line == 4 &&
	            found[0].message.find("token slot 4") != std::string::npos);
	// Renamed so the table's rows parse first: none.
	std::error_code ec;
	std::filesystem::rename(std::filesystem::u8path(root + "/particles/ontable.ptl"),
	                        std::filesystem::u8path(root + "/particles/atable.ptl"), ec);
	TEST_EXPECT(!ec);
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	TEST_EXPECT(rows().empty());
	return 0;
}

// A source of its own, as the game install's view serves its files by their logical names.
class Served final : public opennova::FileSource {
public:
	std::map<std::string, std::string> files;
	bool read(const std::string &name, std::vector<uint8_t> &out) const override {
		const auto found = files.find(name);
		if (found == files.end()) return false;
		out.assign(found->second.begin(), found->second.end());
		return true;
	}
	uint64_t stamp(const std::string &name) const override { return files.count(name) ? 1 : 0; }
};

int test_source() {
	test_temp::TempDir dir("opennova_editor_particle_token_slots_source");
	const std::string root = dir.file("install");
	ProjectDocument document;
	Diagnostic created;
	TEST_EXPECT(create_project(root, "Served", "jo", document, created));
	auto served = std::make_shared<Served>();
	served->files["effects.ptl"] = kFlags;
	served->files["ontable.ptl"] = table();
	ProjectPaths paths = ProjectPaths::for_root(root);
	paths.files = served;
	AssetScan scan;
	for (const auto &[name, text] : served->files) {
		AssetEntry entry = editor_test::file_entry(name, name, AssetKind::Particles);
		entry.size_bytes = text.size();
		scan.entries.push_back(entry);
	}
	scan.index();
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const ValidationInput input{paths, document, scan, open};
	AssetGraph graph;
	ValidationCache cache;
	size_t faults = 0;
	for (const Diagnostic &d : validate_project(input, graph, cache))
		if (d.code() == "particle.unfilled_token_slot") faults += d.asset == "effects.ptl" && d.line == 4 ? 1 : 100;
	TEST_EXPECT(faults == 1);
	// Another source of the same names at the same sizes (the install mounted again over a patched archive, whose
	// members the scan lists by their sizes alone): its files are read, not the first source's findings kept. Its flags
	// end on an attached ';', the same length, and fault nowhere.
	auto patched = std::make_shared<Served>();
	patched->files = served->files;
	std::string guarded = kFlags;
	const size_t apart = guarded.find(" ;");
	TEST_EXPECT(apart != std::string::npos);
	guarded.replace(apart, 2, "; ");
	patched->files["effects.ptl"] = guarded;
	TEST_EXPECT(guarded.size() == kFlags.size());
	ProjectPaths patched_paths = paths;
	patched_paths.files = patched;
	const ValidationInput patched_input{patched_paths, document, scan, open};
	AssetGraph patched_graph;
	ValidationCache patched_cache;
	for (const Diagnostic &d : validate_project(patched_input, patched_graph, patched_cache))
		TEST_EXPECT(d.code() != "particle.unfilled_token_slot");
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_model();
	failed += test_session();
	failed += test_source();
	if (failed == 0) std::printf("editor particle token slots: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
