// The particle type's project check (documents/particle_type.h, make_particle_project_check): the names inside the
// particle files resolved as the effect system resolves them once it has loaded every file of the catalog, each among
// every file's, without case. An effect's particle no file defines (particle.effect_unresolved: the game empties the
// effect) and a particle's curve table no file defines (particle.curve_unresolved), each a warning at the name; a name
// another file defines in another case resolves; the gore set the project picks (.ptu, or .ptg with fgn2.bin) is the
// one that defines; a definition its file never closes registers nothing; an expansion project finds nothing (its base game's
// files resolve names too). The check's protocol: nothing moved, nothing made again. Through a session: the rows in
// Problems, and gone once the particle is defined; its two fixes (the particle added, the member taken out).
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
#include <editor/session/problem_fixes.h>
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
	return "[effectdef]\r\n{\r\n\tid = " + id + ";\r\n\tpdefs = " + pdefs + ";\r\n}\r\n\r\n";
}

std::string particle(const std::string &id, const std::string &keys = std::string()) {
	return "[particledef]\r\n{\r\n\tid = " + id + ";\r\n" + keys + "}\r\n\r\n";
}

std::string table(const std::string &id) {
	std::string out = "[tabledef]\r\n{\r\n\tid = " + id + ";\r\n";
	for (int row = 1; row <= 32; ++row) out += "\ttl" + std::to_string(row) + " = 1, 2, 3, 4, 5, 6, 7, 8;\r\n";
	return out + "}\r\n\r\n";
}

// a.ptl: Smoke names PUFF (b.ptl's Puff, another case) and ghost (none); Fine names puff alone; Gore names the .ptu's
// particle, German the .ptg's, Broken the particle of the block its file never closes. b.ptl: Puff's scale curve names
// tables.ptl's grow in another case, its alpha curve a table none defines, its first graphic's red curve another.
const std::string kA = effect("Smoke", "PUFF, ghost") + effect("Fine", "puff") + effect("Gore", "goreonly") +
                       effect("German", "germanonly") + effect("Broken", "brokenpart");
const std::string kB = particle("Puff", "\tscale_func = GROW;\r\n\talpha_func = missingtable reverse;\r\n"
                                        "\tgraphic1 = dot.tga, additive;\r\n\tg1_red_func = nothere;\r\n");

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
	            test_io::write_text(root + "/particles/broken.ptl", "[particledef]\r\n{\r\n\tid = brokenpart;\r\n"));
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

// particle.effect_unresolved's two fixes, each an edit of the effect's own file: "Add particle X to <file>" (the block the
// game's writer makes of the particle at the game's defaults, after the file's last particle) and "Take X out of pdefs"
// (offered only where another member stays). Each applied through Problems takes its row away; the file reads as the
// effect naming what it defines.
int test_fixes() {
	test_temp::TempDir dir("opennova_editor_particle_project_check_fixes");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Particles"));
	const std::string root = session.view().project.root;
	TEST_EXPECT(test_io::write_text(root + "/particles/a.ptl",
	                                effect("Smoke", "Puff, ghost, spook") + particle("Puff") + table("grow")) &&
	            test_io::write_text(root + "/particles/b.ptl", effect("Lone", "nobody")));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const auto row = [&](const std::string &name) -> const Diagnostic * {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == std::string("particle.effect_unresolved") && d.message.find("The particle " + name + ",") == 0)
				return &d;
		return nullptr;
	};
	const auto fix = [&](const std::string &name, const std::string &label) -> std::vector<ProblemFix> {
		const Diagnostic *d = row(name);
		std::vector<ProblemFix> out;
		if (d)
			for (const ProblemFix &each : fixes_for(*d, session.view()))
				if (each.label == label) out.push_back(each);
		return out;
	};
	const Diagnostic *ghost = row("ghost"), *nobody = row("nobody");
	TEST_EXPECT(ghost && ghost->planned.size() == 2 && ghost->planned[0].label == "Add particle ghost to a.ptl" &&
	            ghost->planned[1].label == "Take ghost out of pdefs");
	// The effect's one member: no Take, its effect would name none.
	TEST_EXPECT(nobody && nobody->planned.size() == 1 && nobody->planned[0].label == "Add particle nobody to b.ptl");
	const std::vector<ProblemFix> add = fix("ghost", "Add particle ghost to a.ptl");
	TEST_EXPECT(add.size() == 1);
	if (add.size() != 1) return 1;
	TEST_EXPECT(editor_test::handle_to_end(session, add.front().request).done());
	session.run_operations();
	TEST_EXPECT(!row("ghost") && row("spook"));
	const std::vector<ProblemFix> take = fix("spook", "Take spook out of pdefs");
	TEST_EXPECT(take.size() == 1);
	if (take.size() != 1) return 1;
	TEST_EXPECT(editor_test::handle_to_end(session, take.front().request).done());
	session.run_operations();
	TEST_EXPECT(!row("spook") && row("nobody"));
	const TextDocument *text = text_of(*session.document_base_for("particles/a.ptl"));
	TEST_EXPECT(text != nullptr);
	if (!text) return 1;
	opennova::particle::ParticleFile read;
	opennova::particle::ParseError error;
	TEST_EXPECT(read_particle_text(*text, read, error) && read.effects.size() == 1 &&
	            read.effects[0].pdefs == std::vector<std::string>({"Puff", "ghost"}) && read.particles.size() == 2 &&
	            read.particles[1].id == "ghost" && read.tables.size() == 1);
	// The particle written after the file's last particle, before its table, at the game's defaults.
	TEST_EXPECT(text->text().find("\tpdefs = Puff, ghost;\r\n") != std::string::npos &&
	            text->text().find("id\t= ghost;") < text->text().find("[tabledef]") && read.particles[1].emit_dur == 10.0f);
	std::printf("particle project check fixes: a particle added, a member taken out\n");
	return 0;
}

// The ids the game's reader registers from a text: each effect's and each particle's, in order.
std::vector<std::string> registered(const std::string &text) {
	opennova::particle::ParticleFile read;
	opennova::particle::ParseError error;
	opennova::particle::load_particles_from_buffer(text.data(), text.size(), read, error);
	std::vector<std::string> out;
	for (const auto &effect : read.effects) out.push_back("effect " + effect.id);
	for (const auto &particle : read.particles) out.push_back("particle " + particle.id);
	return out;
}

// The text a document of `text` holds once the edits apply.
std::string applied(const std::string &text, const std::vector<Edit> &edits) {
	std::unique_ptr<DocumentBase> document = make_particle_document();
	Diagnostic error;
	document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "particles/a.ptl", AssetKind::Particles, "jo", error);
	TextDocument &doc = *text_of(*document);
	doc.apply(edits, error);
	return doc.text();
}

// A block added at a CR LF file's end whose last line has no CR LF: every id the reader read before is read after, the
// new one besides. A last line "}\n" (the walk drops its LF and reads the brace) keeps its brace closing its block; a
// last brace with nothing after it (dropped by the walk, its block never registered) stays unread. Over the effect's
// Add it there (define_particle_effect) and the particle's Add (the project check's fix), and a member's Take refused
// where its cut would part a quote from its pair.
int test_fixes_at_the_end() {
	// define_particle_effect.
	for (const std::string &text : {std::string("[effectdef]\r\n{\r\n\tid = Hit;\r\n\tpdefs = puff;\r\n}\n"),
	                                std::string("[effectdef]\r\n{\r\n\tid = Hit;\r\n\tpdefs = puff;\r\n}\r\n\r\n[tabledef]\r\n"
	                                            "{\r\n\tid = t;\r\n}")}) {
		std::unique_ptr<DocumentBase> document = make_particle_document();
		Diagnostic error;
		TEST_EXPECT(document->load_bytes(std::vector<uint8_t>(text.begin(), text.end()), "particles/a.ptl", AssetKind::Particles,
		                                 "jo", error));
		PlannedFix fix;
		TEST_EXPECT(define_particle_effect(*document, ReferenceSubject{ReferenceKind::Particle, "Boom", "", -1}, fix));
		std::vector<std::string> wanted = registered(text);
		wanted.insert(wanted.begin() + 1, "effect Boom");
		TEST_EXPECT(registered(applied(text, fix.edits)) == wanted);
	}
	// The particle's Add through the project check: the last particle's brace a "}\n", and a file whose last particle
	// the walk never closes.
	test_temp::TempDir dir("opennova_editor_particle_project_check_end");
	editor_test::NoProcess platform;
	MemoryPreferencesStore preferences;
	ProjectSession session(platform, preferences);
	editor_test::handle_to_end(session, request::new_project(dir.file("project"), "Particles"));
	const std::string root = session.view().project.root;
	const std::string lf = "[effectdef]\r\n{\r\n\tid = Smoke;\r\n\tpdefs = puff, ghost;\r\n}\r\n\r\n[particledef]\r\n{\r\n"
	                       "\tid = puff;\r\n}\n";
	const std::string open = "[effectdef]\r\n{\r\n\tid = Lone;\r\n\tpdefs = keep, nobody;\r\n}\r\n\r\n[particledef]\r\n{\r\n"
	                         "\tid = keep;\r\n}";
	const std::string quoted = "[effectdef]\r\n{\r\n\tid = Quoted;\r\n\tpdefs = puff, \"spook\";\r\n}\r\n";
	// The bytes as written (write_text would end each file's last line CR LF).
	const auto raw = [](const std::string &text) { return std::vector<uint8_t>(text.begin(), text.end()); };
	TEST_EXPECT(test_io::write_file(root + "/particles/a.ptl", raw(lf)) && test_io::write_file(root + "/particles/b.ptl", raw(open)) &&
	            test_io::write_file(root + "/particles/c.ptl", raw(quoted)));
	editor_test::handle_to_end(session, request::rescan());
	session.run_operations();
	const auto row = [&](const std::string &name) -> const Diagnostic * {
		for (const Diagnostic &d : session.view().findings.diagnostics)
			if (d.code() == std::string("particle.effect_unresolved") && d.message.find("The particle " + name + ",") == 0)
				return &d;
		return nullptr;
	};
	const Diagnostic *ghost = row("ghost"), *nobody = row("nobody"), *spook = row("spook");
	TEST_EXPECT(ghost && !ghost->planned.empty() && nobody && !nobody->planned.empty() && spook);
	if (!ghost || ghost->planned.empty() || !nobody || nobody->planned.empty() || !spook) return 1;
	TEST_EXPECT(registered(applied(lf, ghost->planned.front().edits)) ==
	            std::vector<std::string>({"effect Smoke", "particle puff", "particle ghost"}));
	TEST_EXPECT(registered(applied(open, nobody->planned.front().edits)) ==
	            std::vector<std::string>({"effect Lone", "particle nobody"}));
	// The quoted member: Add offered, no Take (its cut would leave the quote's pair).
	TEST_EXPECT(spook->planned.size() == 1 && spook->planned.front().label == "Add particle spook to c.ptl");
	std::printf("particle project check fixes at a file's end: what was read stays read; a quoted member no Take\n");
	return 0;
}

} // namespace

int main() {
	int failed = 0;
	failed += test_check();
	failed += test_session();
	failed += test_fixes();
	failed += test_fixes_at_the_end();
	if (failed == 0) std::printf("editor particle project check: all tests passed\n");
	return failed == 0 ? 0 : 1;
}
