// Pins the build (ADR 0046 d8): the routing into the three boot-table archives and the
// loose set, the validation gate, the content-addressed immutable build directory the
// engine's own VFS re-mounts, the incremental reuse of unchanged archives, and what a
// failure leaves behind; and (S13 A1) the build stepped by bytes: every step bounded by its
// budget, a cancel between two steps leaving nothing, and archives byte-identical to the
// single-call writer's however small the steps.
#include <algorithm>
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
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
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

// A file of `size` bytes that is no text the game reads: a loose music bank's bytes.
static bool write_filler(const std::string &path, size_t size) {
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t((i * 2654435761u) >> 24);
	std::string error;
	fs::create_directories(fs::path(path).parent_path());
	return write_file_atomic(path, bytes.data(), bytes.size(), error);
}

// The staging directories under an output root (`<id>.tmp`).
static size_t staging_dirs(const std::string &output_root) {
	size_t count = 0;
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(output_root, ec)) {
		const std::string name = entry.path().filename().string();
		if (entry.is_directory(ec) && name.size() > 4 && name.compare(name.size() - 4, 4, kBuildStagingSuffix) == 0) ++count;
	}
	return count;
}

// S13 A1: a build steps by bytes. One 5 MB file under a 64 KB budget takes at least 80 steps
// (hashed, then copied, a chunk a step), no step moves more than its budget, and the progress
// only goes up, to the total.
static int test_steps_are_bounded() {
	Project p("opennova_editor_build_steps_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(write_filler(p.root + "/music/big.sbf", 5 * 1024 * 1024));
	const BuildPlan plan = p.plan();
	TEST_EXPECT(plan.ok);
	constexpr uint64_t budget = 64 * 1024;
	BuildRun run(plan, p.output_root());
	TEST_EXPECT(run.bytes_total() >= 2 * uint64_t(5 * 1024 * 1024) && run.bytes_done() == 0);
	size_t steps = 0;
	uint64_t largest = 0, done = 0;
	bool upward = true;
	while (!run.step(budget)) {
		++steps;
		upward = upward && run.bytes_done() >= done && run.bytes_done() <= run.bytes_total();
		largest = std::max(largest, run.bytes_done() - done);
		done = run.bytes_done();
		TEST_EXPECT(steps < 100000);
		if (steps >= 100000) break;
	}
	for (const Diagnostic &d : run.report().diagnostics) std::fprintf(stderr, "%s: %s\n", d.code.c_str(), d.message.c_str());
	TEST_EXPECT(run.report().ok && !run.cancelled());
	TEST_EXPECT(steps >= 80);
	TEST_EXPECT(upward && largest <= budget);
	TEST_EXPECT(run.bytes_done() == run.bytes_total());
	TEST_EXPECT(run.items_done() == run.items_total() && run.item_name(0) == "language.pff");
	TEST_EXPECT(fs::file_size(fs::path(run.report().build_dir) / "big.sbf") == 5u * 1024u * 1024u);
	return 0;
}

// S13 A1: a cancel between two steps, mid-archive, removes the staging directory (and the
// archive's temp file inside it), publishes nothing and leaves the last good build as it was.
static int test_cancel_publishes_nothing() {
	Project p("opennova_editor_build_cancel_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	const BuildReport first = run_build(p.plan(), p.output_root());
	TEST_EXPECT(first.ok);
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "a change"));
	BuildRun run(p.plan(), p.output_root());
	size_t steps = 0;
	// Mid-archive: the hash pass in large steps (half the total), then 16 bytes a step until the
	// language archive's writer is open and 32 bytes of it are written.
	const uint64_t hashed = run.bytes_total() / 2;
	while (steps < 100000 && (run.label() != "Packing language.pff" || run.bytes_done() < hashed + 32)) {
		if (run.step(run.bytes_done() < hashed ? uint64_t(1) << 20 : 16)) break;
		++steps;
	}
	TEST_EXPECT(!run.done() && run.label() == "Packing language.pff" && staging_dirs(p.output_root()) == 1);
	run.cancel();
	TEST_EXPECT(run.done() && run.cancelled() && !run.report().ok && run.report().diagnostics.empty());
	TEST_EXPECT(run.report().build_dir.empty() && staging_dirs(p.output_root()) == 0);
	TEST_EXPECT(last_good_build_dir(p.output_root()) == first.build_dir && fs::is_directory(first.build_dir));
	TEST_EXPECT(run.step(16) && run.cancelled()); // a cancelled run stays done
	size_t builds = 0;
	for (const fs::directory_entry &entry : fs::directory_iterator(p.output_root()))
		builds += entry.is_directory() ? 1 : 0;
	TEST_EXPECT(builds == 1);
	// A run dropped unfinished cancels itself.
	{
		BuildRun dropped(p.plan(), p.output_root());
		while (!dropped.done() && staging_dirs(p.output_root()) == 0) dropped.step(uint64_t(1) << 20);
		TEST_EXPECT(!dropped.done() && staging_dirs(p.output_root()) == 1);
	}
	TEST_EXPECT(staging_dirs(p.output_root()) == 0 && last_good_build_dir(p.output_root()) == first.build_dir);
	return 0;
}

// S13 A1: the stepped build's archives are byte for byte what the single-call writer makes of the
// same entries (pff_write_archive over each file's bytes, in the plan's order), at steps of 7
// bytes and of a whole build alike: the stream writer resumes across steps without a seam.
static int test_archives_match_the_single_call_writer() {
	for (const uint64_t budget : {uint64_t(7), uint64_t(1) << 30}) {
		Project p(budget == 7 ? "opennova_editor_build_bytes7_test" : "opennova_editor_build_bytes_test");
		TEST_EXPECT(p.create());
		TEST_EXPECT(p.fill());
		const BuildPlan plan = p.plan();
		BuildRun run(plan, p.output_root());
		for (size_t steps = 0; !run.step(budget) && steps < 10000000; ++steps) {}
		TEST_EXPECT(run.report().ok);
		for (const BuildArchive &archive : plan.archives) {
			std::vector<std::vector<uint8_t>> payloads(archive.entries.size());
			std::vector<opennova::pff::PffWriteEntry> entries;
			std::string error;
			for (size_t i = 0; i < archive.entries.size(); ++i)
				TEST_EXPECT(read_file_bytes(archive.entries[i].source_path, payloads[i], error));
			for (size_t i = 0; i < archive.entries.size(); ++i)
				entries.push_back({archive.entries[i].logical_name.c_str(), payloads[i].empty() ? nullptr : payloads[i].data(),
				                   uint32_t(payloads[i].size()), 0, 0, 0});
			const std::string single = p.dir.file(archive.file_name.c_str());
			TEST_EXPECT(opennova::pff::pff_write_archive(single.c_str(), opennova::pff::PFF_FORMAT_PFF3,
			                                             entries.empty() ? nullptr : entries.data(),
			                                             uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK);
			std::vector<uint8_t> built, expected;
			TEST_EXPECT(read_file_bytes((fs::path(run.report().build_dir) / archive.file_name).generic_string(), built, error));
			TEST_EXPECT(read_file_bytes(single, expected, error));
			TEST_EXPECT(!built.empty() && built == expected);
		}
	}
	return 0;
}

int main() {
	int failures = 0;
	failures += test_steps_are_bounded();
	failures += test_cancel_publishes_nothing();
	failures += test_archives_match_the_single_call_writer();
	failures += test_routing();
	failures += test_empty_project_is_blocked();
	failures += test_filled_project_builds_and_mounts();
	failures += test_protected_build_survives_and_archives_are_refused();
	if (failures == 0) std::printf("editor_project_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
