// The particle type's project check (documents/particle_type.h, make_particle_project_check): the names inside the
// particle files resolved as the effect system resolves them once it has loaded every file of the catalog, each among
// every file's, without case. An effect's particle no file defines (particle.effect_unresolved: the game empties the
// effect) and a particle's curve table no file defines (particle.curve_unresolved), each a warning at the name; a name
// another file defines in another case resolves; the gore set the project picks (.ptu, or .ptg with fgn2.bin) is the
// one that defines; a file the reader stops in defines nothing; an expansion project finds nothing (its base game's
// files resolve names too). The check's protocol: nothing moved, nothing made again. Through a session: the rows in
// Problems, and gone once the particle is defined.
#include <editor/assets/asset_registry.h>
#include <editor/assets/project_asset_source.h>
#include <editor/documents/document_types.h>
#include <editor/documents/particle_type.h>
#include <editor/documents/project_check.h>
#include <editor/documents/validation_cache.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/model/text_document.h>
#include <editor/project/project_document.h>
#include <editor/session/preferences_store.h>
#include <editor/session/project_session.h>
#include <editor/session/request_factories.h>
#include <editor/session/view/session_view.h>

#include <cstdio>
#include <memory>
#include <string>
#include <vector>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"
#include "editor/test_platform.h"

using namespace opennova::editor;

namespace {

std::string effect(const std::string &id, const std::string &pdefs) {
	return "[effectdef]\n{\n\tid = " + id + ";\n\tpdefs = " + pdefs + ";\n}\n\n";
}

std::string particle(const std::string &id, const std::string &keys = std::string()) {
	return "[particledef]\n{\n\tid = " + id + ";\n" + keys + "}\n\n";
}

std::string table(const std::string &id) {
	std::string out = "[tabledef]\n{\n\tid = " + id + ";\n";
	for (int row = 1; row <= 32; ++row) out += "\ttl" + std::to_string(row) + " = 1, 2, 3, 4, 5, 6, 7, 8;\n";
	return out + "}\n\n";
}

// a.ptl: Smoke names PUFF (b.ptl's Puff, another case) and ghost (none); Fine names puff alone; Gore names the .ptu's
// particle, German the .ptg's, Broken the particle of the file the reader stops in. b.ptl: Puff's scale curve names
// tables.ptl's grow in another case, its alpha curve a table none defines, its first graphic's red curve another.
const std::string kA = effect("Smoke", "PUFF, ghost") + effect("Fine", "puff") + effect("Gore", "goreonly") +
                       effect("German", "germanonly") + effect("Broken", "brokenpart");
const std::string kB = particle("Puff", "\tscale_func = GROW;\n\talpha_func = missingtable reverse;\n"
                                        "\tgraphic1 = dot.tga, additive;\n\tg1_red_func = nothere;\n");

struct Found {
	std::string code, asset, name;
	size_t line = 0, column = 0;
};

std::vector<Found> found(const std::vector<Diagnostic> &findings) {
	std::vector<Found> out;
	for (const Diagnostic &d : findings) {
		Found f{d.code(), d.asset, std::string(), d.line, d.column};
		for (const char *name : {"ghost", "germanonly", "goreonly", "brokenpart", "missingtable", "nothere"})
			if (d.message.find(name) != std::string::npos) f.name = name;
		if (d.severity != opennova::editor::DiagnosticSeverity::Warning) f.code += " (not a warning)";
		out.push_back(f);
	}
	return out;
}

bool has(const std::vector<Found> &rows, const std::string &code, const std::string &name, const std::string &asset,
         size_t line, size_t column) {
	for (const Found &f : rows)
		if (f.code == code && f.name == name && f.asset == asset && f.line == line && f.column == column) return true;
	std::fprintf(stderr, "no %s for %s at %s:%zu:%zu among:\n", code.c_str(), name.c_str(), asset.c_str(), line, column);
	for (const Found &f : rows)
		std::fprintf(stderr, "  %s %s %s:%zu:%zu\n", f.code.c_str(), f.name.c_str(), f.asset.c_str(), f.line, f.column);
	return false;
}

int test_check() {
	test_temp::TempDir dir("opennova_editor_particle_project_check");
	const std::string root = dir.file("project");
	ProjectDocument project;
	Diagnostic error;
	TEST_EXPECT(create_project(root, "Particles", "jo", project, error));
	TEST_EXPECT(test_io::write_text(root + "/particles/a.ptl", kA) && test_io::write_text(root + "/particles/b.ptl", kB) &&
	            test_io::write_text(root + "/particles/tables.ptl", table("grow")) &&
	            test_io::write_text(root + "/particles/gore.ptu", particle("goreonly")) &&
	            test_io::write_text(root + "/particles/gore.ptg", particle("germanonly")) &&
	            test_io::write_text(root + "/particles/broken.ptl", "[particledef]\n{\n\tid = brokenpart;\n"));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	const DocumentType *type = document_type_for(AssetKind::Particles);
	TEST_EXPECT(type && type->project_check);
	if (!type || !type->project_check) return 1;
	const auto run = [&](const ProjectDocument &as, std::unique_ptr<ProjectCheck> &check, bool &moved) {
		const AssetScan scan = scan_project_assets(paths, as);
		ProjectAssetSource files;
		files.set_scan(root, scan, as.target_game);
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		const ValidationInput validation{paths, as, scan, open};
		refresh_project(validation, graph, cache);
		const ProjectCheckInput input{validation, cache, files};
		moved = check->update(input);
		const std::vector<Diagnostic> first = check->findings();
		// Nothing moved: nothing made again.
		if (check->update(input) || check->findings() != first) moved = false;
		return found(first);
	};
	std::unique_ptr<ProjectCheck> check = type->project_check();
	bool moved = false;
	std::vector<Found> rows = run(project, check, moved);
	// ghost at its place in pdefs (line 4, after "\tpdefs = PUFF, "); the .ptg's particle (the .ptu is the set without
	// fgn2.bin) and the broken file's at theirs; the two tables at their keys' values. PUFF, puff, GROW and the .ptu's
	// goreonly resolve.
	TEST_EXPECT(moved && rows.size() == 5);
	TEST_EXPECT(has(rows, "particle.effect_unresolved", "ghost", "particles/a.ptl", 4, 16));
	TEST_EXPECT(has(rows, "particle.effect_unresolved", "germanonly", "particles/a.ptl", 22, 10));
	TEST_EXPECT(has(rows, "particle.effect_unresolved", "brokenpart", "particles/a.ptl", 28, 10));
	TEST_EXPECT(has(rows, "particle.curve_unresolved", "missingtable", "particles/b.ptl", 5, 15));
	TEST_EXPECT(has(rows, "particle.curve_unresolved", "nothere", "particles/b.ptl", 7, 16));
	const std::vector<Diagnostic> &said = check->findings();
	TEST_EXPECT(said[0].message.find("UNRESOLVED: EFFDEF Smoke missing PARDEF ghost") != std::string::npos &&
	            said[0].message.find("spawns nothing") != std::string::npos);
	// fgn2.bin picks the .ptg: germanonly resolves, goreonly no longer does.
	TEST_EXPECT(test_io::write_text(root + "/fgn2.bin", "gore"));
	rows = run(project, check, moved);
	TEST_EXPECT(moved && rows.size() == 5 && has(rows, "particle.effect_unresolved", "goreonly", "particles/a.ptl", 16, 10));
	// An expansion's base game loads its own particle files: nothing found.
	ProjectDocument expansion = project;
	expansion.expansion.name = "mymod";
	rows = run(expansion, check, moved);
	TEST_EXPECT(moved && rows.empty());
	// Cleared, then brought up again: the same rows.
	check->clear();
	rows = run(project, check, moved);
	TEST_EXPECT(moved && rows.size() == 5);
	std::printf("particle project check: %zu rows, ghost at 4:16\n", rows.size());
	return 0;
}

// Through a session: the rows among the Problems; an edit of a.ptl defining ghost takes its row away.
int test_session() {
	test_temp::TempDir dir("opennova_editor_particle_project_check_session");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Particles"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(test_io::write_text(root + "/particles/a.ptl", effect("Smoke", "Puff, ghost")) &&
	            test_io::write_text(root + "/particles/b.ptl", particle("Puff")));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const auto rows_of = [&](const char *code) {
		size_t n = 0;
		for (const Diagnostic &d : session.view().findings.diagnostics) n += d.code() == code ? 1 : 0;
		return n;
	};
	TEST_EXPECT(rows_of("particle.effect_unresolved") == 1);
	editor_test::handle_to_end(session, request::open_document("particles/a.ptl"));
	const TextDocument *text = text_of(*session.document_base_for("particles/a.ptl"));
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	editor_test::handle_to_end(session, request::edit_record("particles/a.ptl",
	                                                         TextDocument::replace(text->span_at(text->text().size(), 0),
	                                                                               particle("GHOST"))));
	session.run_operations();
	TEST_EXPECT(rows_of("particle.effect_unresolved") == 0);
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_check();
	failed += test_session();
	if (failed == 0) std::printf("editor particle project check: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
