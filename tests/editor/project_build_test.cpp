// Pins the build (ADR 0046 d8): the routing into the three boot-table archives and the
// loose set, the validation gate, the content-addressed immutable build directory the
// engine's own VFS re-mounts, the incremental reuse of unchanged archives, what a
// failure leaves behind, and the archives' name limit binding only the files they take; and
// (S13 A1) the build stepped by bytes: every step bounded by its budget, a cancel between two
// steps leaving nothing, a file rewritten under a read of several steps failing the build, and
// archives byte-identical to the single-call writer's however small the steps. S13 D5: every
// kind's slot, read from its row, as the switch it replaced answered it, and the kinds it added
// planned where their rows say.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#include <base/vfs/vfs.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/blank/create_missing.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
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

// Every kind's slot as the switch route_asset read before S13 D5 answered it, now its row's
// (asset_kinds). Every kind the build packs routes as it did. The two it never packs answered
// Resource only because the switch had to answer and route to None: an archive, which the build
// refuses, and an import source, whose outputs pack. The kinds S13 D5 added: a face with the art,
// a wave and a map project in localres, the score table loose where retail ships it.
struct Route {
	AssetKind kind;
	ArchiveSlot slot;
};
const Route kRoutes[] = {
	{AssetKind::Unknown, ArchiveSlot::Resource}, // packed all the same until S13 A8
	{AssetKind::Archive, ArchiveSlot::None},
	{AssetKind::Model, ArchiveSlot::Resource},
	{AssetKind::Animation, ArchiveSlot::Resource},
	{AssetKind::AnimationMap, ArchiveSlot::Resource},
	{AssetKind::FaceAnimation, ArchiveSlot::Resource},
	{AssetKind::AiProfile, ArchiveSlot::Resource},
	{AssetKind::Texture, ArchiveSlot::Resource},
	{AssetKind::Font, ArchiveSlot::Localres},
	{AssetKind::Strings, ArchiveSlot::Language},
	{AssetKind::MusicScript, ArchiveSlot::Localres},
	{AssetKind::RawBin, ArchiveSlot::Language},
	{AssetKind::CountryCode, ArchiveSlot::Loose}, // CC.BIN, a RawBin (language.pff) before
	{AssetKind::Credits, ArchiveSlot::Localres},
	{AssetKind::Mission, ArchiveSlot::Localres},
	{AssetKind::MapProject, ArchiveSlot::Localres},
	{AssetKind::Terrain, ArchiveSlot::Resource},
	{AssetKind::TerrainPolyData, ArchiveSlot::Resource},
	{AssetKind::TileInfo, ArchiveSlot::Resource},
	{AssetKind::Environment, ArchiveSlot::Resource},
	{AssetKind::Menu, ArchiveSlot::Localres},
	{AssetKind::MenuStyle, ArchiveSlot::Localres},
	{AssetKind::MusicBank, ArchiveSlot::Loose},   // the .sbf, SoundBank before S13 D5
	{AssetKind::SoundBank, ArchiveSlot::Resource}, // the .lwf, WaveBank before S13 D5
	{AssetKind::Wave, ArchiveSlot::Localres},
	{AssetKind::DialogBank, ArchiveSlot::Localres},
	{AssetKind::Particles, ArchiveSlot::Resource},
	{AssetKind::Script, ArchiveSlot::Localres},
	{AssetKind::ItemDefs, ArchiveSlot::Localres},
	{AssetKind::WeaponDefs, ArchiveSlot::Localres},
	{AssetKind::AmmoDefs, ArchiveSlot::Localres},
	{AssetKind::HudPosDefs, ArchiveSlot::Localres},
	{AssetKind::HudFxDefs, ArchiveSlot::Localres},
	{AssetKind::AvatarDefs, ArchiveSlot::Localres},
	{AssetKind::SoundProfileDefs, ArchiveSlot::Localres},
	{AssetKind::CharAttrDefs, ArchiveSlot::Localres},
	{AssetKind::PowerupDefs, ArchiveSlot::Localres},
	{AssetKind::OtherDefs, ArchiveSlot::Localres},
	{AssetKind::StringTableCoo, ArchiveSlot::Loose},
	{AssetKind::Video, ArchiveSlot::Loose},
	{AssetKind::PlayerSave, ArchiveSlot::Loose},
	{AssetKind::Shader, ArchiveSlot::Resource},
	{AssetKind::Config, ArchiveSlot::Loose},
	{AssetKind::Score, ArchiveSlot::Loose}, // a Config before S13 D5
	{AssetKind::Text, ArchiveSlot::Loose},
	{AssetKind::ImageSource, ArchiveSlot::None},
};

static int test_routing() {
	TEST_EXPECT(sizeof(kRoutes) / sizeof(kRoutes[0]) == kAssetKindCount);
	for (size_t i = 0; i < sizeof(kRoutes) / sizeof(kRoutes[0]); ++i) {
		const Route &route = kRoutes[i];
		TEST_EXPECT(route.kind == AssetKind(i)); // each kind once, in the enum's order
		if (route_asset(route.kind) != route.slot)
			std::fprintf(stderr, "route_asset(%s) moved\n", asset_kind_token(route.kind));
		TEST_EXPECT(route_asset(route.kind) == route.slot);
		TEST_EXPECT(asset_kind_packed(route.kind) == (route.slot != ArchiveSlot::None));
	}
	TEST_EXPECT(route_asset(entry_of("gametext.bin", AssetKind::Strings)) == ArchiveSlot::Language);
	TEST_EXPECT(route_asset(entry_of("menumus.sbf", AssetKind::MusicBank)) == ArchiveSlot::Loose);
	TEST_EXPECT(!asset_is_packable(entry_of("resource.pff", AssetKind::Archive)));
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Language)) == "language.pff");
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::Loose)).empty());
	TEST_EXPECT(std::string(archive_slot_file_name(ArchiveSlot::None)).empty());
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
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		return plan_build(paths, scan, evaluate_requirements(doc, scan),
				validate_project({ paths, doc, scan, open }, graph, cache));
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
	for (const Diagnostic &d : plan.diagnostics) TEST_EXPECT(d.code() != "requirement.optional_missing");
	TEST_EXPECT(plan.archives.size() == 3);
	TEST_EXPECT(plan.archives[0].file_name == "language.pff" && !plan.archives[0].entries.empty());
	TEST_EXPECT(plan.archives[1].file_name == "localres.pff" && !plan.archives[1].entries.empty());
	TEST_EXPECT(plan.archives[2].file_name == "resource.pff" && plan.archives[2].entries.empty());
	TEST_EXPECT(plan.loose.size() == 2); // menumus.sbf and nw_cdata.coo

	const BuildReport report = run_build(plan, p.output_root());
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
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
	for (const Diagnostic &d : changed.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
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
	const BuildReport second = run_build(p.plan(), p.output_root(), protect_dirs({first.build_dir}));
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
	for (const Diagnostic &d : plan.diagnostics) reported = reported || d.code() == "build.archive_in_project";
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
			std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
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
		if (d.code() == "asset.name.too_long")
			too_long.push_back(d.asset);
	TEST_EXPECT(too_long == std::vector<std::string>{ texture });
	return 0;
}

// The kinds S13 D5 added, planned where their rows say: a wave and a map project in localres.pff,
// a face in resource.pff, the score table copied loose (as a Config it was before).
static int test_new_kinds_land_where_their_rows_say() {
	Project p("opennova_editor_build_new_kinds_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::write_text(p.root + "/sounds/boom.wav", "RIFF"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/ASP_G7.npz", "0ZPN"));
	TEST_EXPECT(editor_test::write_text(p.root + "/faces/head.grm", "BASE_TEXTURE face.tga\r\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/score.ini", "VERSION 40\r\n"));
	const BuildPlan plan = p.plan();
	for (const Diagnostic &d : plan.diagnostics)
		std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(plan.ok && plan.archives.size() == 3);
	const auto in = [](const std::vector<BuildEntry> &entries, const char *name) {
		for (const BuildEntry &entry : entries)
			if (entry.logical_name == name) return true;
		return false;
	};
	TEST_EXPECT(in(plan.archives[1].entries, "boom.wav"));
	TEST_EXPECT(in(plan.archives[1].entries, "ASP_G7.npz"));
	TEST_EXPECT(in(plan.archives[2].entries, "head.grm"));
	TEST_EXPECT(in(plan.loose, "score.ini"));
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
	for (const Diagnostic &d : run.report().diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
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

// Rewrites `path` in place through a handle of its own (a build's read of it stays open) with
// other bytes of the same size, dated two seconds after its last write: a read of the file over
// several steps sees both, and neither its size nor its end tells.
static bool rewrite_in_place(const std::string &path) {
	std::error_code ec;
	const uintmax_t size = fs::file_size(path, ec);
	if (ec) return false;
	const fs::file_time_type written = fs::last_write_time(path, ec);
	if (ec) return false;
	{
		std::fstream file(path, std::ios::binary | std::ios::in | std::ios::out);
		if (!file) return false;
		const std::vector<char> bytes(static_cast<size_t>(size), 'X');
		file.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
		if (!file) return false;
	}
	fs::last_write_time(path, written + std::chrono::seconds(2), ec);
	return !ec && fs::file_size(path, ec) == size;
}

// S13 A1: a file rewritten in place while the build reads it over several steps, even to the
// same size, is never built torn: each file is checked again when its last byte is read (its
// size and its last write), so the hash pass, an archive's entry and a loose copy alike fail with
// build.changed, naming the file, and nothing is published.
static int test_file_rewritten_mid_read_fails() {
	enum class Pass { Hash, Archive, Loose };
	for (const Pass pass : {Pass::Hash, Pass::Archive, Pass::Loose}) {
		Project p(pass == Pass::Hash      ? "opennova_editor_build_torn_hash_test"
		          : pass == Pass::Archive ? "opennova_editor_build_torn_archive_test"
		                                  : "opennova_editor_build_torn_loose_test");
		TEST_EXPECT(p.create());
		TEST_EXPECT(p.fill());
		constexpr uint64_t kSize = 256 * 1024;
		TEST_EXPECT(write_filler(p.root + "/music/big.sbf", kSize));  // a loose file
		TEST_EXPECT(write_filler(p.root + "/extra/torn.xyz", kSize)); // packed: resource.pff
		const BuildPlan plan = p.plan();
		TEST_EXPECT(plan.ok);
		// Where the file's bytes lie in the run's progress: the hash pass reads every archive's
		// entries in the plan's order, then the loose files; the packing and the copies follow in
		// the same order.
		const std::string name = pass == Pass::Archive ? "torn.xyz" : "big.sbf";
		std::string source;
		uint64_t total = 0, offset = UINT64_MAX;
		const auto walk = [&](const BuildEntry &entry) {
			if (entry.logical_name == name) {
				offset = total;
				source = entry.source_path;
			}
			total += fs::file_size(entry.source_path);
		};
		bool packed = false;
		for (const BuildArchive &archive : plan.archives)
			for (const BuildEntry &entry : archive.entries) {
				packed = packed || entry.logical_name == "torn.xyz";
				walk(entry);
			}
		for (const BuildEntry &entry : plan.loose) walk(entry);
		TEST_EXPECT(packed && offset != UINT64_MAX && !source.empty());
		const uint64_t start = pass == Pass::Hash ? offset : total + offset;
		BuildRun run(plan, p.output_root());
		bool rewritten = false;
		for (size_t steps = 0; !run.step(64 * 1024) && steps < 100000; ++steps) {
			// A step into the file and not out of it: the next ones read the rest.
			if (!rewritten && run.bytes_done() > start && run.bytes_done() < start + kSize) {
				TEST_EXPECT(rewrite_in_place(source));
				rewritten = true;
			}
		}
		TEST_EXPECT(rewritten && run.done() && !run.cancelled() && !run.report().ok);
		const std::vector<Diagnostic> &found = run.report().diagnostics;
		TEST_EXPECT(found.size() == 1 && found[0].code() == "build.changed" && found[0].asset == name);
		TEST_EXPECT(run.report().build_dir.empty() && staging_dirs(p.output_root()) == 0 &&
		            last_good_build_dir(p.output_root()).empty());
	}
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
	failures += test_file_rewritten_mid_read_fails();
	failures += test_archives_match_the_single_call_writer();
	failures += test_routing();
	failures += test_new_kinds_land_where_their_rows_say();
	failures += test_empty_project_is_blocked();
	failures += test_filled_project_builds_and_mounts();
	failures += test_protected_build_survives_and_archives_are_refused();
	failures += test_long_names_bind_packed_files_only();
	if (failures == 0) std::printf("editor_project_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
