// Pins the build (ADR 0046 d8): the routing into the three boot-table archives and the
// loose set, the validation gate, the content-addressed immutable build directory the
// engine's own VFS re-mounts, the incremental reuse of unchanged archives, what a
// failure leaves behind, and the archives' name limit binding only the files they take.
#include <cstdio>
#include <filesystem>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/create_missing.h>
#include <editor/documents/document_types.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <formats/rtxt/rtxt.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_session.h>
#include <editor/requirements/requirements.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
namespace fs = std::filesystem;

// A valid one-string table whose content differs per `text`: the strings validator
// gates the build, so a changed table must still be one the game can read.
static bool write_table(const std::string &path, const char *text) {
	opennova::rtxt::File table;
	table.sections.push_back({"Menu", 1});
	opennova::rtxt::Entry entry;
	entry.key = "CHANGED";
	entry.text = text;
	table.entries.push_back(entry);
	std::vector<uint8_t> bytes;
	std::string error;
	return opennova::rtxt::write(table, bytes, error) && write_file_atomic(path, bytes.data(), bytes.size(), error);
}

static AssetEntry entry_of(const char *name, AssetKind kind) {
	AssetEntry e;
	e.logical_name = name;
	e.relative_path = name;
	e.kind = kind;
	return e;
}

static int test_routing() {
	TEST_EXPECT(route_asset(entry_of("gametext.bin", AssetKind::Strings)) == ArchiveSlot::Language);
	TEST_EXPECT(route_asset(entry_of("menutxt.bin", AssetKind::Strings)) == ArchiveSlot::Language);
	TEST_EXPECT(route_asset(entry_of("main.mnu", AssetKind::Menu)) == ArchiveSlot::Localres);
	TEST_EXPECT(route_asset(entry_of("items.def", AssetKind::ItemDefs)) == ArchiveSlot::Localres);
	TEST_EXPECT(route_asset(entry_of("x.bms", AssetKind::Mission)) == ArchiveSlot::Localres);
	TEST_EXPECT(route_asset(entry_of("Arial14b.fnt", AssetKind::Font)) == ArchiveSlot::Localres);
	TEST_EXPECT(route_asset(entry_of("menumus.bin", AssetKind::MusicScript)) == ArchiveSlot::Localres);
	TEST_EXPECT(route_asset(entry_of("x.trn", AssetKind::Terrain)) == ArchiveSlot::Resource);
	TEST_EXPECT(route_asset(entry_of("x.3di", AssetKind::Model)) == ArchiveSlot::Resource);
	TEST_EXPECT(route_asset(entry_of("x.tga", AssetKind::Texture)) == ArchiveSlot::Resource);
	TEST_EXPECT(route_asset(entry_of("x.env", AssetKind::Environment)) == ArchiveSlot::Resource);
	TEST_EXPECT(route_asset(entry_of("menumus.sbf", AssetKind::SoundBank)) == ArchiveSlot::Loose);
	TEST_EXPECT(route_asset(entry_of("earlyerr.txt", AssetKind::Text)) == ArchiveSlot::Loose);
	TEST_EXPECT(route_asset(entry_of("intro.BIK", AssetKind::Video)) == ArchiveSlot::Loose);
	TEST_EXPECT(route_asset(entry_of("nw_cdata.coo", AssetKind::StringTableCoo)) == ArchiveSlot::Loose);
	TEST_EXPECT(!asset_is_packable(entry_of("resource.pff", AssetKind::Archive)));
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Language)) == "language.pff");
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Loose)).empty());
	return 0;
}

struct Project {
	editor_test::TempProjectDir dir;
	std::string root;
	ProjectDocument doc;
	ProjectPaths paths;
	explicit Project(const char *name) : dir(name), root(dir.file("Game")) {}
	bool create() {
		Diagnostic error;
		if (!create_project(root, "Build Game", "jo", doc, error)) return false;
		paths = ProjectPaths::for_root(root);
		return true;
	}
	BuildPlan plan() {
		const AssetScan scan = scan_project_assets(paths, doc);
		return plan_build(paths, scan, evaluate_requirements(doc, scan), validate_open_documents(paths, doc, scan, {}));
	}
	bool fill() {
		const AssetScan scan = scan_project_assets(paths, doc);
		const RequirementReport report = evaluate_requirements(doc, scan);
		const CreateMissingResult result = create_missing_requirements(paths, doc, report, unmet_required_roles(report));
		return result.unavailable.empty() && result.diagnostics.empty();
	}
	std::string output_root() const { return paths.build_dir + "/play"; }
};

static int test_empty_project_is_blocked() {
	Project p("opennova_editor_build_blocked_test");
	TEST_EXPECT(p.create());
	const BuildPlan plan = p.plan();
	TEST_EXPECT(!plan.ok);
	TEST_EXPECT(!plan.diagnostics.empty());
	const BuildReport report = run_build(plan, p.output_root());
	TEST_EXPECT(!report.ok && report.build_dir.empty());
	TEST_EXPECT(last_good_build_dir(p.output_root()).empty());
	return 0;
}

static int test_filled_project_builds_and_mounts() {
	Project p("opennova_editor_build_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::write_text(p.root + "/music/menumus.sbf", "SBF!")); // a loose-by-contract file
	const BuildPlan plan = p.plan();
	TEST_EXPECT(plan.ok);
	// The optional files the project lacks are notes the game does without: not the gate's.
	for (const Diagnostic &d : plan.diagnostics) TEST_EXPECT(d.code != "requirement.optional_missing");
	TEST_EXPECT(plan.archives.size() == 3);
	TEST_EXPECT(plan.archives[0].file_name == "language.pff" && !plan.archives[0].entries.empty());
	TEST_EXPECT(plan.archives[1].file_name == "localres.pff" && !plan.archives[1].entries.empty());
	TEST_EXPECT(plan.archives[2].file_name == "resource.pff" && plan.archives[2].entries.empty());
	TEST_EXPECT(plan.loose.size() == 2); // menumus.sbf and nw_cdata.coo

	const BuildReport report = run_build(plan, p.output_root());
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code.c_str(), d.message.c_str());
	TEST_EXPECT(report.ok);
	TEST_EXPECT(!report.reused_existing);
	TEST_EXPECT(report.build_id.size() == 16);
	TEST_EXPECT(report.archives_written.size() == 3 && report.archives_reused.empty());
	TEST_EXPECT(report.loose_written.size() == 2);
	TEST_EXPECT(fs::is_directory(report.build_dir));
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "language.pff"));
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "localres.pff"));
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "resource.pff")); // empty, still present
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "menumus.sbf"));
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / kBuildRecordFileName));
	TEST_EXPECT(!fs::exists(fs::path(p.output_root()) / (report.build_id + ".tmp")));
	TEST_EXPECT(last_good_build_dir(p.output_root()) == report.build_dir);

	// The runtime's own mount, archive-only over the fixed boot table, resolves every name.
	// (Scoped: an open mount holds the archives, and a held build is never pruned.)
	{
		opennova::Vfs vfs;
		TEST_EXPECT(vfs.mount_game(report.build_dir, std::string(), opennova::VfsMountMode::Packed,
		                           opennova::VfsArchiveDiscovery::RetailTable));
		TEST_EXPECT(vfs.has_mounted_archive());
		for (const BuildArchive &archive : plan.archives)
			for (const BuildEntry &entry : archive.entries) TEST_EXPECT(vfs.has_file(entry.logical_name));
		TEST_EXPECT(vfs.has_file("MAIN.MNU"));
		std::vector<uint8_t> menu;
		TEST_EXPECT(vfs.read_file("main.mnu", menu) && !menu.empty());
		TEST_EXPECT(!vfs.has_file("menumus.sbf")); // loose, never in an archive
	}

	// Same content: the same build, nothing rewritten.
	const BuildReport again = run_build(p.plan(), p.output_root());
	TEST_EXPECT(again.ok && again.reused_existing && again.build_id == report.build_id);
	TEST_EXPECT(again.build_dir == report.build_dir);

	// One changed file: a new build in which only its archive is re-packed.
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "one")); // a valid table with new content
	const BuildReport changed = run_build(p.plan(), p.output_root());
	for (const Diagnostic &d : changed.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code.c_str(), d.message.c_str());
	TEST_EXPECT(changed.ok && !changed.reused_existing);
	TEST_EXPECT(changed.build_id != report.build_id);
	TEST_EXPECT(changed.archives_written == std::vector<std::string>{"language.pff"});
	TEST_EXPECT(changed.archives_reused.size() == 2);
	TEST_EXPECT(last_good_build_dir(p.output_root()) == changed.build_dir);
	TEST_EXPECT(!fs::exists(report.build_dir)); // the older build is pruned
	return 0;
}

static int test_protected_build_survives_and_archives_are_refused() {
	Project p("opennova_editor_build_protect_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	const BuildReport first = run_build(p.plan(), p.output_root());
	TEST_EXPECT(first.ok);
	TEST_EXPECT(!fs::exists(fs::path(first.build_dir) / kBuildStagingMarkerFileName));

	// The output root may be any folder (--out): pruning deletes only what proves it is a
	// build, never a bystander, whatever its name looks like.
	const std::string out = p.output_root();
	TEST_EXPECT(editor_test::write_text(out + "/my important documents/keep.txt", "mine"));
	TEST_EXPECT(editor_test::write_text(out + "/0123456789abcdef/keep.txt", "mine"));     // id-shaped, no record
	TEST_EXPECT(editor_test::write_text(out + "/fedcba9876543210.tmp/keep.txt", "mine")); // staging-shaped, no marker
	TEST_EXPECT(editor_test::write_text(out + "/00000000000000aa/build.json",
	                                    "{\"schema_version\":1,\"build_id\":\"someone-else\"}"));

	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "two"));
	const BuildReport second = run_build(p.plan(), p.output_root(), {first.build_dir});
	TEST_EXPECT(second.ok && second.build_dir != first.build_dir);
	TEST_EXPECT(fs::is_directory(first.build_dir)); // a running child's build is never pruned
	TEST_EXPECT(fs::is_regular_file(out + "/my important documents/keep.txt"));
	TEST_EXPECT(fs::is_regular_file(out + "/0123456789abcdef/keep.txt"));
	TEST_EXPECT(fs::is_regular_file(out + "/fedcba9876543210.tmp/keep.txt"));
	TEST_EXPECT(fs::is_regular_file(out + "/00000000000000aa/build.json"));

	// Once the child is gone the next build prunes its directory too.
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "three"));
	const BuildReport third = run_build(p.plan(), p.output_root());
	TEST_EXPECT(third.ok);
	TEST_EXPECT(!fs::exists(first.build_dir) && !fs::exists(second.build_dir));
	TEST_EXPECT(fs::is_regular_file(out + "/my important documents/keep.txt"));

	// A .pff inside the project blocks the build with a plain explanation.
	TEST_EXPECT(editor_test::write_text(p.root + "/old/stuff.pff", "PFF3"));
	const BuildPlan plan = p.plan();
	TEST_EXPECT(!plan.ok);
	bool reported = false;
	for (const Diagnostic &d : plan.diagnostics) reported = reported || d.code == "build.archive_in_project";
	TEST_EXPECT(reported);
	return 0;
}

// The archives' 16-character name limit binds only a file the build packs (S13 PR0): a loose
// one (a video) is copied beside the archives under any name, so a long one builds, and a
// packed one (a texture) with a long name still blocks the build.
static int test_long_names_bind_packed_files_only() {
	Project p("opennova_editor_build_long_names_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	const std::string video = "a_long_intro_movie_name.bik";
	TEST_EXPECT(editor_test::write_text(p.root + "/video/" + video, "BIKi"));
	const BuildPlan loose = p.plan();
	if (!loose.ok)
		for (const Diagnostic &d : loose.diagnostics)
			std::fprintf(stderr, "%s: %s\n", d.code.c_str(), d.message.c_str());
	TEST_EXPECT(loose.ok);
	bool listed = false;
	for (const BuildEntry &entry : loose.loose)
		listed = listed || entry.logical_name == video;
	TEST_EXPECT(listed);
	const BuildReport report = run_build(loose, p.output_root());
	TEST_EXPECT(report.ok && fs::is_regular_file(fs::path(report.build_dir) / video));

	const std::string texture = "art/a_long_texture_name.tga";
	TEST_EXPECT(editor_test::write_text(p.root + "/" + texture, "x"));
	const BuildPlan packed = p.plan();
	TEST_EXPECT(!packed.ok);
	std::vector<std::string> too_long;
	for (const Diagnostic &d : packed.diagnostics)
		if (d.code == "asset.name.too_long")
			too_long.push_back(d.asset);
	TEST_EXPECT(too_long == std::vector<std::string>{ texture });
	return 0;
}

int main() {
	int failures = 0;
	failures += test_routing();
	failures += test_empty_project_is_blocked();
	failures += test_filled_project_builds_and_mounts();
	failures += test_protected_build_survives_and_archives_are_refused();
	failures += test_long_names_bind_packed_files_only();
	if (failures == 0) std::printf("editor_project_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
