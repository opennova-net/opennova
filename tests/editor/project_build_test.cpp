// Pins the build (ADR 0046 d8): the routing into the three boot-table archives and the
// loose set, the validation gate, the content-addressed immutable build directory the
// engine's own VFS re-mounts, the incremental reuse of unchanged archives, what a
// failure leaves behind, and the archives' name limit binding only the files they take; and
// (S13 A1) the build stepped by bytes: every step bounded by its budget, a cancel between two
// steps leaving nothing, a file rewritten under a read of several steps failing the build, and
// archives byte-identical to the single-call writer's however small the steps. S13 D5: every
// kind's slot, read from its row, as the switch it replaced answered it, and the kinds it added
// planned where their rows say. S13 A8: a file of no kind the game knows left out, a material
// chunk packed, the hash cache (one changed file read alone, the other archives linked; a file
// written within the settle window read until it settles; a rehash reading every file), an output
// root past MAX_PATH, and no write through a name the last good build's archive stands behind.
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <fcntl.h>
#include <io.h>
#include <share.h>
#include <sys/stat.h>
#endif

#include <base/resource_index/boot_policy.h>
#include <base/vfs/vfs.h>
#include <editor/assets/asset_kinds.h>
#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/assets/project_scan.h>
#include <editor/blank/create_missing.h>
#include <editor/documents/document_types.h>
#include <editor/graph/asset_graph.h>
#include <editor/graph/project_validation.h>
#include <editor/graph/reference_kinds.h>
#include <editor/project/project_document.h>
#include <editor/project/project_files.h>
#include <editor/project_build/archive_routing.h>
#include <formats/pff/pff.h>
#include <formats/rtxt/rtxt.h>
#include <runtime/renderer/material_texture.h>
#include <base/io/json.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/project_build/export_build.h>
#include <editor/requirements/requirements.h>

#include "common/test_expect.h"
#include "editor/editor_test_support.h"

using namespace opennova::editor;
using opennova::pff::normalized_logical_name;
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
	return opennova::rtxt::write(table, bytes, error) && opennova::io::write_file_atomic(path, bytes.data(), bytes.size(), error);
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
// a wave and a map project in localres, the score table loose where retail ships it. S13 A8: a
// file of no kind the game knows packs nowhere, and a material chunk (a texture by its bytes
// before) with the art.
struct Route {
	AssetKind kind;
	ArchiveSlot slot;
};
const Route kRoutes[] = {
	{AssetKind::Unknown, ArchiveSlot::None}, // resource.pff before S13 A8
	{AssetKind::Archive, ArchiveSlot::None},
	{AssetKind::Model, ArchiveSlot::Resource},
	{AssetKind::Animation, ArchiveSlot::Resource},
	{AssetKind::AnimationMap, ArchiveSlot::Resource},
	{AssetKind::FaceAnimation, ArchiveSlot::Resource},
	{AssetKind::AiProfile, ArchiveSlot::Resource},
	{AssetKind::Texture, ArchiveSlot::Resource},
	{AssetKind::MaterialChunk, ArchiveSlot::Resource},
	{AssetKind::Font, ArchiveSlot::Localres},
	{AssetKind::Strings, ArchiveSlot::Language},
	{AssetKind::MusicScript, ArchiveSlot::Localres},
	{AssetKind::RawBin, ArchiveSlot::Language},
	{AssetKind::CountryCode, ArchiveSlot::Loose}, // CC.BIN, a RawBin (language.pff) before
	{AssetKind::Credits, ArchiveSlot::Localres},
	{AssetKind::Mission, ArchiveSlot::Localres},
	{AssetKind::MissionText, ArchiveSlot::None}, // a .mis, a Mission (localres.pff) before S14
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
	// .mnx, no kind (and left out) before S13 A8; loose until S16, which the front door reads only under
	// /d [orig: FileSystem_OpenFile @ 0x75b1c0; Game_InitSubsystems @ 0x4a6fac]
	{AssetKind::NovaWorldScreen, ArchiveSlot::Localres},
	{AssetKind::Video, ArchiveSlot::Loose},
	{AssetKind::PlayerSave, ArchiveSlot::Loose},
	{AssetKind::Shader, ArchiveSlot::Resource},
	{AssetKind::Config, ArchiveSlot::Loose},
	{AssetKind::Score, ArchiveSlot::Loose}, // a Config before S13 D5
	{AssetKind::Text, ArchiveSlot::Loose},
	{AssetKind::Notes, ArchiveSlot::None}, // a .md (no kind, said) and a .txt the game never reads (Text) before
	{AssetKind::ImportSource, ArchiveSlot::None}, // ImageSource, a .png's, before S13 A8
	{AssetKind::ImportInput, ArchiveSlot::None}, // a terrain set's images (S20)
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
	TEST_EXPECT(route_asset(entry_of("resource.pff", AssetKind::Archive)) == ArchiveSlot::None);
	TEST_EXPECT(route_asset(entry_of("notes.xyz", AssetKind::Unknown)) == ArchiveSlot::None);
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
		const CreateMissingResult result = create_missing_requirements(paths, doc, scan, report, unmet_required_roles(report));
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
	// resource.pff holds what the blanks make that goes there: the pointer the startup screen names and
	// the renderer's own shader, _ffp.fx.
	TEST_EXPECT(plan.archives[2].file_name == "resource.pff" && plan.archives[2].entries.size() == 2);
	for (const auto &entry : plan.archives[2].entries)
		TEST_EXPECT(entry.logical_name == "newarow1.tga" || entry.logical_name == "_ffp.fx");
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
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "resource.pff"));
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
	// What it lists: every archive the last build's, kept (the first build wrote each).
	size_t listed = 0;
	for (const BuiltFile &file : report.built) listed += file.archive && !file.reused;
	TEST_EXPECT(listed == 3);
	listed = 0;
	for (const BuiltFile &file : again.built) listed += file.archive && file.reused;
	TEST_EXPECT(listed == 3 && again.built.size() == report.built.size());

	// One changed file: a new build in which only its archive is re-packed.
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "one")); // a valid table with new content
	const BuildReport changed = run_build(p.plan(), p.output_root());
	for (const Diagnostic &d : changed.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(changed.ok && !changed.reused_existing);
	TEST_EXPECT(changed.build_id != report.build_id);
	TEST_EXPECT(changed.archives_written == std::vector<std::string>{"language.pff"});
	TEST_EXPECT(changed.archives_reused.size() == 2);
	for (const BuiltFile &file : changed.built)
		if (file.archive) TEST_EXPECT(file.reused == (file.name != "language.pff"));
	TEST_EXPECT(last_good_build_dir(p.output_root()) == changed.build_dir);
	TEST_EXPECT(!fs::exists(report.build_dir)); // the older build is pruned
	return 0;
}

// A project with missions on, filled by Create missing (every Required row, and the optional
// mission-start rows a factory fills), validates without an error or a warning, builds, and its
// build's own mount resolves every name a mission's start and its screens read.
static int test_mission_project_builds_clean() {
	Project p("opennova_editor_build_mission_test");
	TEST_EXPECT(p.create());
	p.doc.features.mission = true;
	TEST_EXPECT(p.fill());
	{
		const AssetScan scan = scan_project_assets(p.paths, p.doc);
		const CreateMissingResult extras = create_missing_requirements(
		        p.paths, p.doc, scan, evaluate_requirements(p.doc, scan),
		        {"font_arials18", "font_arial22", "font_couri20b", "game_wac", "server_wac", "loadscrn_pcx", "monogram_tga",
		         "boxtile_tga", "border_tga"});
		TEST_EXPECT(extras.diagnostics.empty() && extras.unavailable.empty() && extras.created.size() == 9);
	}
	const BuildPlan plan = p.plan();
	for (const Diagnostic &d : plan.diagnostics)
		if (d.severity != DiagnosticSeverity::Info)
			std::fprintf(stderr, "  %s: %s (%s)\n", d.code().c_str(), d.message.c_str(), d.asset.c_str());
	TEST_EXPECT(plan.ok);
	for (const Diagnostic &d : plan.diagnostics) TEST_EXPECT(d.severity == DiagnosticSeverity::Info);
	const BuildReport report = run_build(plan, p.output_root());
	TEST_EXPECT(report.ok);
	opennova::Vfs vfs;
	TEST_EXPECT(vfs.mount_game(report.build_dir, std::string(), opennova::VfsMountMode::Packed,
	                           opennova::VfsArchiveDiscovery::RetailTable));
	for (const char *name : {"ammo.def", "powerup.def", "cmap.mnu", "game.mnu", "weapon.mnu", "vehicle.mnu", "stat.mnu",
	                         "death.mnu", "mp.mnu", "Arials18.fnt", "Arial22.fnt", "couri20b.fnt", "game.wac", "server.wac",
	                         "loadscrn.pcx", "monogram.tga", "boxtile.tga", "border.tga"}) {
		if (!vfs.has_file(name)) std::fprintf(stderr, "  not in the build: %s\n", name);
		TEST_EXPECT(vfs.has_file(name));
	}
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
	if (!report.ok)
		for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "  build: %s %s\n", d.code().c_str(), d.message.c_str());
	// Read through the system's path: under a deep TEMP the copy lies past MAX_PATH.
	TEST_EXPECT(report.ok && fs::is_regular_file(system_path((fs::path(report.build_dir) / video).generic_string())));

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
	// S16: the NovaWorld screens in localres.pff with the menus and nowhere else, the one place the
	// game reads them without /d [orig: @ 0x63e1c6 -> FileSystem_OpenFile @ 0x75b1c0, whose loose
	// search only /d turns on @ 0x4a6fac].
	TEST_EXPECT(editor_test::write_text(p.root + "/nw_error.mnx", "<HTML/>") &&
	            editor_test::write_text(p.root + "/nw_startup.mnx", "<HTML/>"));
	const BuildPlan screens = p.plan();
	TEST_EXPECT(screens.ok && in(screens.archives[1].entries, "nw_error.mnx") && in(screens.archives[1].entries, "nw_startup.mnx"));
	TEST_EXPECT(!in(screens.loose, "nw_error.mnx") && !in(screens.loose, "nw_startup.mnx") &&
	            !in(screens.archives[0].entries, "nw_error.mnx") && !in(screens.archives[2].entries, "nw_error.mnx"));
	// One of a name no archive holds, which the game never reads (a backup): left out and said, the build
	// going ahead.
	const std::string backup = "nw_startup_backup.mnx";
	TEST_EXPECT(editor_test::write_text(p.root + "/" + backup, "<HTML/>"));
	const BuildPlan unread = p.plan();
	size_t said = 0;
	for (const Diagnostic &d : unread.diagnostics)
		if (d.code() == "build.unread" && d.asset == backup && d.severity == DiagnosticSeverity::Warning && !blocks_build(d)) ++said;
	TEST_EXPECT(unread.ok && said == 1 && !in(unread.loose, backup.c_str()));
	for (const BuildArchive &archive : unread.archives) TEST_EXPECT(!in(archive.entries, backup.c_str()));
	return 0;
}

// ADR 0046 S16: an expansion's routing. The language slot's kinds go to <b>L.pff, the localres and
// resource slots' to <b>.pff, a loose kind into the expansion's folder where its reader looks there
// and nowhere where the game reads it from the install's folder alone; the files the game reads by
// the expansion's name where their kind would put them elsewhere (<b>.bin also loose, version.txt in
// the folder).
static int test_expansion_routing() {
	for (size_t i = 0; i < kAssetKindCount; ++i) {
		const AssetKind kind = AssetKind(i);
		const ExpansionPlace place = route_for_expansion(kind);
		switch (route_asset(kind)) {
		case ArchiveSlot::Language: TEST_EXPECT(place == ExpansionPlace::LanguageArchive); break;
		case ArchiveSlot::Localres:
		case ArchiveSlot::Resource: TEST_EXPECT(place == ExpansionPlace::Archive); break;
		case ArchiveSlot::Loose:
			TEST_EXPECT(place == ExpansionPlace::Folder || place == ExpansionPlace::RootOnly);
			break;
		case ArchiveSlot::None: TEST_EXPECT(place == ExpansionPlace::None); break;
		}
	}
	TEST_EXPECT(route_for_expansion(AssetKind::Video) == ExpansionPlace::Folder);
	TEST_EXPECT(route_for_expansion(AssetKind::MusicBank) == ExpansionPlace::Folder);
	TEST_EXPECT(route_for_expansion(AssetKind::CountryCode) == ExpansionPlace::RootOnly);
	TEST_EXPECT(route_for_expansion(AssetKind::StringTableCoo) == ExpansionPlace::RootOnly);
	TEST_EXPECT(route_for_expansion(AssetKind::NovaWorldScreen) == ExpansionPlace::Archive);
	TEST_EXPECT(route_for_expansion(entry_of("JXM.bin", AssetKind::Strings), "jxm") == ExpansionPlace::Folder);
	TEST_EXPECT(route_for_expansion(entry_of("gametext.bin", AssetKind::Strings), "jxm") == ExpansionPlace::LanguageArchive);
	TEST_EXPECT(route_for_expansion(entry_of("version.txt", AssetKind::Text), "jxm") == ExpansionPlace::Folder);
	TEST_EXPECT(route_for_expansion(entry_of("gt.ssc", AssetKind::Config), "jxm") == ExpansionPlace::Folder);
	TEST_EXPECT(route_for_expansion(entry_of("notes.txt", AssetKind::Text), "jxm") == ExpansionPlace::RootOnly);
	TEST_EXPECT(route_for_expansion(entry_of("nw_error.mnx", AssetKind::NovaWorldScreen), "jxm") == ExpansionPlace::Archive);
	return 0;
}

// Files as an archive or a folder holds them: (name, bytes).
using Files = std::vector<std::pair<std::string, std::vector<uint8_t>>>;

static std::vector<uint8_t> bytes_of(const std::string &text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

static std::vector<uint8_t> file_bytes(const std::string &path) {
	std::vector<uint8_t> bytes;
	std::string error;
	opennova::io::read_file_bytes(path, bytes, error);
	return bytes;
}

// The project file of the name, wherever the project keeps it ("" for none).
static std::string find_file(const std::string &root, const std::string &name) {
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		if (it->is_regular_file() && it->path().filename().string() == name) return it->path().generic_string();
	return std::string();
}

static bool archive_has(const std::string &archive_path, const char *name) {
	opennova::pff::PffArchive archive{};
	if (opennova::pff::pff_open(&archive, archive_path.c_str()) != 0) return false;
	const bool found = opennova::pff::pff_find(&archive, name) != nullptr;
	opennova::pff::pff_close(&archive);
	return found;
}

static bool write_archive(const std::string &path, const Files &files) {
	std::vector<opennova::pff::PffWriteEntry> entries;
	for (const auto &[name, bytes] : files)
		entries.push_back({name.c_str(), bytes.empty() ? nullptr : bytes.data(), uint32_t(bytes.size()), 0,
		                   opennova::pff::PFF_NEW_ENTRY_TIMESTAMP, 0});
	fs::create_directories(fs::path(path).parent_path());
	return opennova::pff::pff_write_archive(path.c_str(), opennova::pff::PFF_FORMAT_PFF3,
	                                        entries.empty() ? nullptr : entries.data(),
	                                        uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK;
}

// A base game's install (ADR 0046 S16): its three boot archives and the loose files beside them, and
// the names it serves, sorted as the session's listing sorts them (BaseNames).
struct BaseInstall {
	std::string root;
	std::vector<std::string> names;
	bool written = false;
	BaseNames base() const { return BaseNames{&names}; }
};

static BaseInstall make_base(const std::string &root, const Files &language, const Files &localres, const Files &resource,
                             const Files &loose = {}) {
	BaseInstall out;
	out.root = root;
	out.written = write_archive(root + "/language.pff", language) && write_archive(root + "/localres.pff", localres) &&
	              write_archive(root + "/resource.pff", resource);
	for (const Files *files : {&language, &localres, &resource, &loose})
		for (const auto &[name, bytes] : *files) out.names.push_back(name);
	for (const auto &[name, bytes] : loose) {
		std::string error;
		out.written = out.written && opennova::io::write_file_atomic(root + "/" + name, bytes.data(), bytes.size(), error);
	}
	std::sort(out.names.begin(), out.names.end(), [](const std::string &a, const std::string &b) {
		return normalized_logical_name(a) < normalized_logical_name(b);
	});
	return out;
}

// ADR 0046 S16: the project built as the expansion jxm. Its folder holds its two archives (the
// language slot's kinds in jxmL.pff, the rest in jxm.pff), its loose files where the game reads them
// under /exp jxm (a video, its music bank, its version.txt, its jxm.bin; a NovaWorld screen packed
// alone), nothing at the build's root; a file the game reads only from the install's folder
// left out and said (build.expansion.root_only); the build mounts with /exp jxm and resolves every
// name; its id is another than the standalone game's of the same files and than another expansion
// name's.
static int test_expansion_layout() {
	Project p("opennova_editor_build_expansion_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(write_table(p.root + "/strings/jxm.bin", "The expansion"));
	TEST_EXPECT(editor_test::write_text(p.root + "/video/header.bik", "BIKi"));
	TEST_EXPECT(editor_test::write_text(p.root + "/music/Mjxm.sbf", "SBF!"));
	TEST_EXPECT(editor_test::write_text(p.root + "/version.txt", "1.0"));
	TEST_EXPECT(editor_test::write_text(p.root + "/cc.bin", "us"));
	TEST_EXPECT(editor_test::write_text(p.root + "/nw_error.mnx", "<HTML/>"));
	const BaseInstall install = make_base(p.dir.file("install"), { { "basetable.bin", bytes_of("base") } }, {}, {});
	TEST_EXPECT(install.written);
	const BaseNames base = install.base();
	BuildTarget target;
	target.expansion = "jxm";
	target.install = install.root;
	target.game = "jo";
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const auto plan_as = [&](const BuildTarget &as) {
		return plan_build(p.paths, scan, evaluate_requirements(p.doc, scan),
				validate_project({ p.paths, p.doc, scan, open }, graph, cache), as, &base);
	};
	const BuildPlan plan = plan_as(target);
	for (const Diagnostic &d : plan.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(plan.ok && plan.target.expansion == "jxm");
	TEST_EXPECT(plan.archives.size() == 2);
	TEST_EXPECT(plan.archives[0].file_name == "expansion/jxm/jxmL.pff" && plan.archives[0].slot == ArchiveSlot::Language);
	TEST_EXPECT(plan.archives[1].file_name == "expansion/jxm/jxm.pff");
	const auto in = [](const std::vector<BuildEntry> &entries, const char *name) {
		for (const BuildEntry &entry : entries)
			if (entry.logical_name == name) return true;
		return false;
	};
	const auto loose_at = [&plan](const char *path) {
		for (const BuildEntry &entry : plan.loose)
			if (entry.build_path == path) return true;
		return false;
	};
	TEST_EXPECT(!in(plan.archives[0].entries, "jxm.bin") && in(plan.archives[0].entries, "gametext.bin"));
	TEST_EXPECT(in(plan.archives[1].entries, "main.mnu") && in(plan.archives[1].entries, "nw_error.mnx"));
	TEST_EXPECT(!in(plan.loose, "nw_error.mnx"));
	TEST_EXPECT(loose_at("expansion/jxm/jxm.bin") && loose_at("expansion/jxm/header.bik"));
	TEST_EXPECT(loose_at("expansion/jxm/Mjxm.sbf") && loose_at("expansion/jxm/version.txt"));
	TEST_EXPECT(!in(plan.loose, "cc.bin") && !in(plan.archives[0].entries, "cc.bin") && in(plan.root_only, "cc.bin"));
	for (const BuildEntry &entry : plan.loose) TEST_EXPECT(entry.build_path.rfind("expansion/jxm/", 0) == 0);

	const BuildReport report = run_build(plan, p.output_root());
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(report.ok && report.archives_written.size() == 2 && report.same_as_base_files == 0);
	// The base has no cc.bin of its own: the expansion's cannot be shipped, and is said.
	bool root_only = false;
	for (const Diagnostic &d : report.diagnostics)
		root_only = root_only || (d.code() == "build.expansion.root_only" && d.asset == "cc.bin" &&
		                          d.severity == DiagnosticSeverity::Warning && !blocks_build(d));
	TEST_EXPECT(root_only);
	const fs::path dir = fs::path(report.build_dir);
	TEST_EXPECT(fs::is_regular_file(dir / "expansion/jxm/jxmL.pff") && fs::is_regular_file(dir / "expansion/jxm/jxm.pff"));
	TEST_EXPECT(fs::is_regular_file(dir / "expansion/jxm/version.txt") && fs::is_regular_file(dir / "expansion/jxm/jxm.bin"));
	TEST_EXPECT(!fs::exists(dir / "language.pff") && !fs::exists(dir / "header.bik") && !fs::exists(dir / "cc.bin"));
	{
		opennova::Vfs vfs;
		opennova::LaunchFlags flags;
		flags.expansion = "jxm";
		TEST_EXPECT(opennova::mount_install(vfs, report.build_dir, flags) && vfs.mounted_expansion() == "jxm");
		for (const BuildArchive &archive : plan.archives)
			for (const BuildEntry &entry : archive.entries) TEST_EXPECT(vfs.has_file(entry.logical_name));
	}
	// The same content is the same build; as the standalone game, or as another expansion, another.
	const BuildReport again = run_build(plan_as(target), p.output_root());
	TEST_EXPECT(again.ok && again.reused_existing && again.build_id == report.build_id);
	const BuildReport standalone = run_build(plan_as(BuildTarget()), p.paths.build_dir + "/standalone");
	BuildTarget other = target;
	other.expansion = "jxn";
	const BuildReport renamed = run_build(plan_as(other), p.paths.build_dir + "/renamed");
	TEST_EXPECT(standalone.ok && renamed.ok);
	TEST_EXPECT(standalone.build_id != report.build_id && renamed.build_id != report.build_id);
	TEST_EXPECT(fs::is_regular_file(fs::path(renamed.build_dir) / "expansion/jxn/jxnL.pff"));
	return 0;
}

// ADR 0046 S16: an expansion with nothing to pack still has both archives, empty: the game opens
// the pair by name, and a missing <b>.pff is no expansion at all [orig: Expansion_LoadAssets @
// 0x4a4767]. The empty pair mounts with /exp and verifies.
static int test_expansion_empty_pair() {
	editor_test::TempProjectDir dir("opennova_editor_build_expansion_empty_test");
	const ProjectPaths paths = ProjectPaths::for_root(dir.file("Game"));
	const BaseInstall install = make_base(dir.file("install"), { { "gametext.bin", bytes_of("text") } }, {}, {});
	const BaseNames base = install.base();
	BuildTarget target;
	target.expansion = "x1";
	target.install = install.root;
	const BuildPlan plan = plan_build(paths, AssetScan(), RequirementReport(), {}, target, &base);
	TEST_EXPECT(plan.ok && plan.archives.size() == 2 && plan.archives[0].entries.empty() && plan.archives[1].entries.empty());
	const BuildReport report = run_build(plan, dir.file("out"));
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(report.ok && report.archives_written.size() == 2);
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "expansion/x1/x1L.pff"));
	TEST_EXPECT(fs::is_regular_file(fs::path(report.build_dir) / "expansion/x1/x1.pff"));
	opennova::Vfs vfs;
	opennova::LaunchFlags flags;
	flags.expansion = "x1";
	TEST_EXPECT(opennova::mount_install(vfs, report.build_dir, flags) && vfs.mounted_expansion() == "x1");
	return 0;
}

// Every file under `root` given a last write an hour back: settled (io::file_stamp_settled), so a
// cache keeps what it read of them.
static void settle_files(const std::string &root) {
	std::error_code ec;
	const fs::file_time_type past = fs::file_time_type::clock::now() - std::chrono::hours(1);
	for (auto it = fs::recursive_directory_iterator(root, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
		std::error_code set;
		if (it->is_regular_file()) fs::last_write_time(it->path(), past, set);
	}
}

// ADR 0046 S16, lean packing: an expansion's build leaves out what its base game serves the same under
// the name (a string table and a menu packed alike, a video the install's folder holds alike), so the
// game reads the base's; a file of the name that differs, or one the base lacks, stays. A mission (or a
// map project) the expansion ships keeps its text table in <b>L.pff however like the base's (the
// mission list titles it from the pair's own text archive); one with no table in the pair is said
// (build.expansion.mission_untitled), as one of a name the base lists too
// (build.expansion.mission_twice). A root-only file like the base's is said nothing, one unlike it
// build.expansion.root_only. The report counts what the base serves (same_as_base); the next build of
// the same content reads none of the base's bytes (its hashes cached) and is the same build; stepped,
// the comparison moves the progress; the name in another case is another build; an install whose base
// does not mount fails the build, build.expansion.base_missing.
static int test_lean_packing() {
	Project p("opennova_editor_build_lean_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::write_text(p.root + "/video/intro.bik", "the base's intro"));
	TEST_EXPECT(editor_test::write_text(p.root + "/video/header.bik", "a header of our own"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/m2.bms", "our mission"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/m2.bin", "its table"));
	TEST_EXPECT(editor_test::write_text(p.root + "/cc.bin", "us"));
	TEST_EXPECT(editor_test::write_text(p.root + "/score.ini", "ours"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/ASP_G7.npz", "a map project of ours"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/ASP_G7.bin", "its own table"));
	TEST_EXPECT(editor_test::write_text(p.root + "/missions/m3.bms", "a mission with the base's text"));
	const std::string gametext = find_file(p.root, "gametext.bin"), menu = find_file(p.root, "main.mnu"),
	                  coo = find_file(p.root, "nw_cdata.coo");
	TEST_EXPECT(!gametext.empty() && !menu.empty() && !coo.empty());
	const BaseInstall install = make_base(
	        p.dir.file("install"), { { "gametext.bin", file_bytes(gametext) }, { "m2.bin", bytes_of("its table") },
	                                 { "ASP_G7.bin", bytes_of("its own table") }, { "m3.bin", bytes_of("the base's") } },
	        { { "main.mnu", file_bytes(menu) }, { "m2.bms", bytes_of("the base's mission") } }, {},
	        { { "intro.bik", bytes_of("the base's intro") }, { "header.bik", bytes_of("the base's header") },
	          { "cc.bin", bytes_of("us") }, { "score.ini", bytes_of("theirs") }, { "nw_cdata.coo", file_bytes(coo) } });
	TEST_EXPECT(install.written);
	settle_files(install.root); // the base cache keeps what it reads of settled files alone
	const BaseNames base = install.base();
	BuildTarget target;
	target.expansion = "jxm";
	target.install = install.root;
	target.game = "jo";
	const auto plan_of = [&] {
		const AssetScan scan = scan_project_assets(p.paths, p.doc);
		return plan_build(p.paths, scan, evaluate_requirements(p.doc, scan), {}, target, &base);
	};
	const BuildPlan plan = plan_of();
	for (const Diagnostic &d : plan.diagnostics)
		if (blocks_build(d, &base)) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(plan.ok);
	const BuildReport report = run_build(plan, p.output_root());
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(report.ok);
	const std::string folder = report.build_dir + "/expansion/jxm";
	TEST_EXPECT(!archive_has(folder + "/jxmL.pff", "gametext.bin") && !archive_has(folder + "/jxm.pff", "main.mnu"));
	TEST_EXPECT(!fs::exists(folder + "/intro.bik") && fs::is_regular_file(folder + "/header.bik"));
	TEST_EXPECT(archive_has(folder + "/jxm.pff", "m2.bms") && archive_has(folder + "/jxmL.pff", "m2.bin"));
	// A map project lists as a mission does [orig: Mission_BuildMapListFromPFF @ 0x562910]: its table stays.
	TEST_EXPECT(archive_has(folder + "/jxm.pff", "ASP_G7.npz") && archive_has(folder + "/jxmL.pff", "ASP_G7.bin"));
	// What the lists make of the missions it ships, said and refusing nothing: m3.bms has no table in its
	// pair (untitled); m2.bms is a name the base lists too (twice).
	std::vector<std::string> untitled, twice;
	for (const Diagnostic &d : report.diagnostics) {
		if (d.code() == "build.expansion.mission_untitled") untitled.push_back(d.asset);
		if (d.code() == "build.expansion.mission_twice") twice.push_back(d.asset);
		if (d.code().rfind("build.expansion.mission_", 0) == 0)
			TEST_EXPECT(d.severity == DiagnosticSeverity::Warning && !blocks_build(d, &base));
	}
	TEST_EXPECT(untitled == std::vector<std::string>{ "missions/m3.bms" } && twice == std::vector<std::string>{ "missions/m2.bms" });
	TEST_EXPECT(report.same_as_base_files == 3 && report.same_as_base_bytes == file_bytes(gametext).size() +
	                                                                               file_bytes(menu).size() + 16);
	TEST_EXPECT(report.base_bytes_read > 0);
	size_t root_only = 0;
	for (const Diagnostic &d : report.diagnostics)
		if (d.code() == "build.expansion.root_only") {
			++root_only;
			TEST_EXPECT(d.asset == "score.ini");
		}
	TEST_EXPECT(root_only == 1);
	std::printf("editor_project_build: lean packing left out %zu file(s), %llu byte(s), reading %llu of the base's\n",
	            report.same_as_base_files, static_cast<unsigned long long>(report.same_as_base_bytes),
	            static_cast<unsigned long long>(report.base_bytes_read));
	// The same content: the same build, none of the base's bytes read (the build cache's `base`).
	const BuildReport again = run_build(plan_of(), p.output_root());
	TEST_EXPECT(again.ok && again.reused_existing && again.build_id == report.build_id && again.base_bytes_read == 0);
	// An archive written within the settle window (a rewrite of the same size may keep its stamp, git's
	// racy rule): read again by every build until it settles, the build the same. Its last write a minute
	// ahead, as a clock that stood still would leave it: stamped now, two builds under a loaded machine's
	// parallel tests could take the window's two seconds, the second then finding it settled.
	{
		std::error_code touched;
		fs::last_write_time(install.root + "/localres.pff", fs::file_time_type::clock::now() + std::chrono::minutes(1),
		                    touched);
		const BuildReport racy = run_build(plan_of(), p.output_root());
		const BuildReport racy_again = run_build(plan_of(), p.output_root());
		TEST_EXPECT(racy.ok && racy.base_bytes_read > 0 && racy_again.ok && racy_again.base_bytes_read > 0 &&
		            racy_again.build_id == report.build_id);
		settle_files(install.root);
		const BuildReport settled = run_build(plan_of(), p.output_root());
		const BuildReport settled_again = run_build(plan_of(), p.output_root());
		TEST_EXPECT(settled.ok && settled.base_bytes_read > 0 && settled_again.base_bytes_read == 0);
	}
	// Stepped, the comparison with the base moves the progress, which only goes up, to a total the dropped
	// files left.
	{
		BuildRun stepped(plan_of(), p.paths.build_dir + "/stepped");
		uint64_t last = 0;
		bool compared = false;
		while (!stepped.step(4096)) {
			TEST_EXPECT(stepped.bytes_done() >= last && stepped.bytes_done() <= stepped.bytes_total());
			if (stepped.label().rfind("Comparing", 0) == 0 && stepped.bytes_done() > last) compared = true;
			last = stepped.bytes_done();
		}
		TEST_EXPECT(stepped.report().ok && compared && stepped.bytes_done() == stepped.bytes_total());
	}
	// The name in another case is another build, published under the new spelling (a host compares the
	// name as spelled [orig: String_ExactMatch @ 0x5122f0]).
	BuildTarget upper = target;
	upper.expansion = "JXM";
	const BuildReport recased = run_build(plan_build(p.paths, scan_project_assets(p.paths, p.doc),
	                                                 evaluate_requirements(p.doc, scan_project_assets(p.paths, p.doc)), {},
	                                                 upper, &base),
	                                      p.paths.build_dir + "/recased");
	std::string spelled;
	std::error_code listed;
	for (const fs::directory_entry &each : fs::directory_iterator(recased.build_dir + "/expansion", listed))
		spelled = each.path().filename().string();
	TEST_EXPECT(recased.ok && recased.build_id != report.build_id && spelled == "JXM");
	// A base that does not mount fails the build.
	BuildTarget gone = target;
	gone.install = p.dir.file("no_install");
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	const BuildReport failed = run_build(plan_build(p.paths, scan, evaluate_requirements(p.doc, scan), {}, gone, &base),
	                                     p.paths.build_dir + "/gone");
	bool missing = false;
	for (const Diagnostic &d : failed.diagnostics) missing = missing || d.code() == "build.expansion.base_missing";
	TEST_EXPECT(!failed.ok && missing);
	return 0;
}

// An expansion's table with its [exp_info] name and description.
static bool write_exp_info(const std::string &path, const std::string &name, const std::string &description) {
	opennova::rtxt::File table;
	table.sections.push_back({"exp_info", 2});
	opennova::rtxt::Entry title;
	title.key = "EXP_NAME";
	title.text = name;
	opennova::rtxt::Entry text;
	text.key = "EXP_DESC";
	text.text = description;
	table.entries.push_back(title);
	table.entries.push_back(text);
	std::vector<uint8_t> bytes;
	std::string error;
	fs::create_directories(fs::path(path).parent_path());
	return opennova::rtxt::write(table, bytes, error) && opennova::io::write_file_atomic(path, bytes.data(), bytes.size(), error);
}

// ADR 0046 S16 (IDA item 3): the Mods list copies EXP_NAME whole into a 64-byte name, after the folder
// name it then runs over: 63 bytes build, 64 refuse the build (build.expansion.exp_name); EXP_DESC's
// 272-byte description spills into the next record, a listed warning (build.expansion.exp_desc).
static int test_expansion_table_texts() {
	Project p("opennova_editor_build_exp_info_test");
	TEST_EXPECT(p.create());
	const BaseInstall install = make_base(p.dir.file("install"), { { "basetable.bin", bytes_of("base") } }, {}, {});
	const BaseNames base = install.base();
	BuildTarget target;
	target.expansion = "jxm";
	target.install = install.root;
	const auto codes = [&](const std::string &name, const std::string &description, bool &ok) {
		std::vector<std::string> out;
		if (!write_exp_info(p.root + "/strings/jxm.bin", name, description)) return std::vector<std::string>{ "unwritten" };
		const BuildPlan plan =
		        plan_build(p.paths, scan_project_assets(p.paths, p.doc), RequirementReport(), {}, target, &base);
		ok = plan.ok;
		for (const Diagnostic &d : plan.diagnostics)
			if (d.code().rfind("build.expansion.exp_", 0) == 0) out.push_back(d.code());
		return out;
	};
	bool ok = false;
	TEST_EXPECT(codes(std::string(63, 'N'), std::string(271, 'D'), ok).empty() && ok);
	TEST_EXPECT(codes(std::string(64, 'N'), "", ok) == std::vector<std::string>{ "build.expansion.exp_name" } && !ok);
	TEST_EXPECT(codes("Mod", std::string(272, 'D'), ok) == std::vector<std::string>{ "build.expansion.exp_desc" } && ok);
	return 0;
}

// ADR 0046 S16, the gate over the base: an expansion's required file the project lacks blocks nothing
// where the base serves it (requirement.missing), nor a gating reference's file (a mission's terrain);
// a required file of another kind still blocks (requirement.wrong_kind); with no base listing the
// expansion does not build (build.expansion.base_missing); the standalone game's gate is as it was.
static int test_base_gate() {
	Project p("opennova_editor_build_base_gate_test");
	TEST_EXPECT(p.create());
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	const RequirementReport requirements = evaluate_requirements(p.doc, scan);
	std::vector<std::string> required;
	for (const RequirementRow &row : requirements.rows)
		if (row.required) required.push_back(row.name);
	std::sort(required.begin(), required.end(), [](const std::string &a, const std::string &b) {
		return normalized_logical_name(a) < normalized_logical_name(b);
	});
	const BaseNames base{&required};
	BuildTarget target;
	target.expansion = "jxm";
	TEST_EXPECT(!plan_build(p.paths, scan, requirements, {}).ok);          // the standalone game: blocked
	TEST_EXPECT(plan_build(p.paths, scan, requirements, {}, target, &base).ok); // the base serves them
	const std::vector<std::string> nothing{ "other.bin" };
	const BaseNames thin{&nothing};
	TEST_EXPECT(!plan_build(p.paths, scan, requirements, {}, target, &thin).ok); // the base lacks them
	const BuildPlan unmounted = plan_build(p.paths, scan, requirements, {}, target, nullptr);
	bool missing = false;
	for (const Diagnostic &d : unmounted.diagnostics) missing = missing || d.code() == "build.expansion.base_missing";
	TEST_EXPECT(!unmounted.ok && missing);
	// A gating reference (a mission's terrain) the base serves; one it does not.
	Diagnostic terrain = make_finding(CoreFinding::ReferenceMissing, DiagnosticSeverity::Error, "no terrain");
	terrain.subject = ReferenceSubject{ ReferenceKind::Terrain, "hills", std::string(), -1 };
	const std::vector<std::string> hills{ "hills.trn" };
	const BaseNames with_hills{&hills};
	TEST_EXPECT(blocks_build(terrain) && blocks_build(terrain, &thin) && !blocks_build(terrain, &with_hills));
	// A required file of another kind blocks over any base.
	Diagnostic wrong = make_finding(CoreFinding::RequirementWrongKind, DiagnosticSeverity::Error, "wrong kind");
	wrong.subject = RequirementSubject{ "gametext", "gametext.bin" };
	TEST_EXPECT(blocks_build(wrong) && blocks_build(wrong, &base));
	// The demo round's bug 6: the words follow the gate's rule. A required file the base serves says the game
	// reads the base's, never what the game does without it; one the base lacks says that as before.
	const RequirementReport over_base = evaluate_requirements(p.doc, scan, nullptr, &required);
	const RequirementReport over_thin = evaluate_requirements(p.doc, scan, nullptr, &nothing);
	size_t served = 0, lacking = 0;
	for (const Diagnostic &d : over_base.diagnostics)
		if (d.code() == "requirement.missing") {
			++served;
			TEST_EXPECT(d.message.find("The game reads the base game's, which the expansion builds on.") != std::string::npos &&
			            d.message.find("exits") == std::string::npos);
		}
	for (const Diagnostic &d : over_thin.diagnostics)
		if (d.code() == "requirement.missing") lacking += d.message.find("base game's") == std::string::npos ? 1 : 0;
	TEST_EXPECT(served > 0 && lacking == served);
	// A mission's terrain the base serves: the graph's words over the base's names.
	AssetGraph graph;
	GraphEdge edge;
	edge.source = "missions/test.bms";
	edge.kind = ReferenceKind::Terrain;
	edge.value = edge.target = "hills";
	edge.field = "terrain";
	const Diagnostic alone = graph.missing_finding(edge);
	TEST_EXPECT(alone.message.find("the game refuses to start the mission") != std::string::npos);
	graph.set_base_names(hills);
	const Diagnostic over = graph.missing_finding(edge);
	TEST_EXPECT(over.message.find("the game reads the base game's hills.trn, which the expansion builds on.") != std::string::npos &&
	            over.message.find("refuses") == std::string::npos && !blocks_build(over, &with_hills));
	return 0;
}

// The findings of `code` among `findings`.
static size_t count_code(const std::vector<Diagnostic> &findings, const char *code) {
	size_t count = 0;
	for (const Diagnostic &d : findings) count += d.code() == code ? 1 : 0;
	return count;
}

// An export's listed files: every file it wrote but its record.
static std::vector<std::string> expected_files(std::vector<std::string> files) {
	files.erase(std::remove(files.begin(), files.end(), std::string(kExportRecordFileName)), files.end());
	return files;
}

// The files under `dir`, '/'-separated and relative, sorted.
static std::vector<std::string> files_under(const std::string &dir) {
	std::vector<std::string> out;
	std::error_code ec;
	for (auto it = fs::recursive_directory_iterator(dir, ec); !ec && it != fs::recursive_directory_iterator(); it.increment(ec))
		if (it->is_regular_file()) out.push_back(it->path().lexically_relative(dir).generic_string());
	std::sort(out.begin(), out.end());
	return out;
}

// ADR 0046 S16: Export copies the build whole (its record left behind) into a folder of its own with
// export.json naming the project; again over its own export (what the person added there kept, a file
// of the export they changed replaced, an earlier export's file the build no longer holds removed, each
// said: export.replaced); into an empty folder; never over a folder of the person's files nor another
// project's export (export.folder, nothing written), nor into the build; a staging folder its own
// cut-short export left, and a set-aside folder an export could not remove (export.cleanup), removed
// first, one of anything else refused and left as it is; a folder named with a trailing separator;
// stepped by bytes and cancelled leaving the folder as it was (export.cancelled); the scan passing over
// the export folder's staging and set-aside siblings; an expansion's laid out as in an install; the
// runtime's folder under runtime/ when asked. The build is only read.
static int test_export() {
	Project p("opennova_editor_export_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::write_text(p.root + "/video/intro.bik", "BIKi"));
	const BuildReport build = run_build(p.plan(), p.output_root());
	TEST_EXPECT(build.ok);
	const std::string tree = editor_test::tree_digest(build.build_dir);
	ExportRequest request;
	request.build_dir = build.build_dir;
	request.build_id = build.build_id;
	request.export_dir = p.dir.file("shipped/Game");
	request.project_id = p.doc.project_id;
	ExportReport shipped = export_build(request);
	for (const Diagnostic &d : shipped.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(shipped.ok && shipped.export_dir == request.export_dir);
	std::vector<std::string> expected = files_under(build.build_dir);
	expected.erase(std::remove(expected.begin(), expected.end(), std::string(kBuildRecordFileName)), expected.end());
	TEST_EXPECT(shipped.files == expected);
	expected.push_back(kExportRecordFileName);
	std::sort(expected.begin(), expected.end());
	TEST_EXPECT(files_under(request.export_dir) == expected);
	std::string text, error;
	opennova::io::JsonValue record;
	TEST_EXPECT(opennova::io::read_file_text(request.export_dir + "/" + kExportRecordFileName, text, error) &&
	            opennova::io::json_parse(text, record, error) && record.get_string("project_id", "") == p.doc.project_id &&
	            record.get_string("build_id", "") == build.build_id);
	std::error_code ec;
	TEST_EXPECT(!fs::equivalent(request.export_dir + "/language.pff", build.build_dir + "/language.pff", ec)); // copied
	TEST_EXPECT(!fs::exists(request.export_dir + kExportStagingSuffix) && !fs::exists(request.export_dir + kExportPreviousSuffix));

	// Again over its own export: what the person added there kept (and said), a file of the export they
	// changed replaced by the build's (and said), the record naming the export's own files alone.
	TEST_EXPECT(editor_test::write_text(request.export_dir + "/notes.txt", "mine"));
	std::this_thread::sleep_for(std::chrono::milliseconds(20)); // a last write after the record's
	TEST_EXPECT(editor_test::write_text(request.export_dir + "/intro.bik", "changed"));
	shipped = export_build(request);
	std::vector<std::string> with_notes = expected;
	with_notes.push_back("notes.txt");
	std::sort(with_notes.begin(), with_notes.end());
	TEST_EXPECT(shipped.ok && files_under(request.export_dir) == with_notes && shipped.files == expected_files(expected));
	TEST_EXPECT(opennova::io::read_file_text(request.export_dir + "/notes.txt", text, error) && text == "mine" &&
	            opennova::io::read_file_text(request.export_dir + "/intro.bik", text, error) && text == "BIKi");
	TEST_EXPECT(shipped.kept == std::vector<std::string>{"notes.txt"} &&
	            shipped.replaced == std::vector<std::string>{"intro.bik"} && shipped.removed.empty());
	TEST_EXPECT(count_code(shipped.diagnostics, "export.replaced") == 1 && shipped.diagnostics.back().severity == DiagnosticSeverity::Info &&
	            shipped.diagnostics.back().message.find("notes.txt") != std::string::npos);
	TEST_EXPECT(opennova::io::read_file_text(request.export_dir + "/" + kExportRecordFileName, text, error) &&
	            text.find("notes.txt") == std::string::npos);
	// A cut-short export's staging folder, and a set-aside one an export could not remove, go first.
	TEST_EXPECT(opennova::io::read_file_text(request.export_dir + "/" + kExportRecordFileName, text, error));
	TEST_EXPECT(editor_test::write_text(request.export_dir + kExportStagingSuffix + "/" + kExportRecordFileName, text) &&
	            editor_test::write_text(request.export_dir + kExportPreviousSuffix + "/" + kExportRecordFileName, text) &&
	            editor_test::write_text(request.export_dir + kExportPreviousSuffix + "/language.pff", "old"));
	TEST_EXPECT(export_build(request).ok && !fs::exists(request.export_dir + kExportStagingSuffix) &&
	            !fs::exists(request.export_dir + kExportPreviousSuffix));
	// A set-aside folder that cannot be removed (a file of it open elsewhere): the new export is in, and said.
	const RemoveTree held = [](const std::string &, std::string &reason) {
		reason = "a file of it is in use";
		return false;
	};
	shipped = export_build(request, held);
	TEST_EXPECT(shipped.ok && count_code(shipped.diagnostics, "export.cleanup") == 1 &&
	            fs::is_regular_file(request.export_dir + kExportPreviousSuffix + "/" + kExportRecordFileName) &&
	            files_under(request.export_dir) == with_notes);
	TEST_EXPECT(export_build(request).ok && !fs::exists(request.export_dir + kExportPreviousSuffix));
	// A folder named with a trailing separator is the folder.
	ExportRequest trailing = request;
	trailing.export_dir = request.export_dir + "/";
	shipped = export_build(trailing);
	TEST_EXPECT(shipped.ok && shipped.export_dir == request.export_dir && files_under(request.export_dir) == with_notes &&
	            !fs::exists(request.export_dir + "/" + kExportStagingSuffix) && !fs::exists(request.export_dir + "/.old"));
	// Stepped by a budget of bytes, progress only going up; cancelled before its folder is replaced, the
	// folder as it was and the staging folder gone.
	{
		const std::string before = editor_test::tree_digest(request.export_dir);
		ExportRun run(request);
		uint64_t last = 0;
		int steps = 0;
		while (!run.done() && steps < 3) {
			run.step(1024);
			TEST_EXPECT(run.bytes_done() >= last && run.bytes_done() <= run.bytes_total());
			last = run.bytes_done();
			++steps;
		}
		TEST_EXPECT(!run.done() && run.bytes_total() > 0 && fs::exists(request.export_dir + kExportStagingSuffix));
		run.cancel();
		TEST_EXPECT(run.done() && !run.report().ok && count_code(run.report().diagnostics, "export.cancelled") == 1 &&
		            !fs::exists(request.export_dir + kExportStagingSuffix) &&
		            editor_test::tree_digest(request.export_dir) == before);
		ExportRun whole(request);
		int whole_steps = 0;
		while (!whole.step(64 * 1024)) ++whole_steps;
		TEST_EXPECT(whole.report().ok && whole_steps > 2 && whole.bytes_done() == whole.bytes_total());
	}
	// The scan passes over a staging or set-aside folder beside the project's own export folder, which a
	// build would otherwise refuse as archives in the project.
	const std::string own = p.paths.export_dir(p.doc);
	TEST_EXPECT(editor_test::write_text(own + kExportStagingSuffix + "/language.pff", "PFF3") &&
	            editor_test::write_text(own + kExportPreviousSuffix + "/expansion/jxm/jxm.pff", "PFF3"));
	{
		const BuildPlan beside = p.plan();
		TEST_EXPECT(beside.ok && count_code(beside.diagnostics, "build.archive_in_project") == 0);
		fs::remove_all(own + kExportStagingSuffix, ec);
		fs::remove_all(own + kExportPreviousSuffix, ec);
	}
	// An empty folder takes it.
	ExportRequest empty = request;
	empty.export_dir = p.dir.file("empty");
	fs::create_directories(empty.export_dir);
	TEST_EXPECT(export_build(empty).ok && fs::is_regular_file(empty.export_dir + "/localres.pff"));

	// Never over the person's files, another project's export, a foreign staging folder, the build.
	ExportRequest theirs = request;
	theirs.export_dir = p.dir.file("theirs");
	TEST_EXPECT(editor_test::write_text(theirs.export_dir + "/keep.txt", "mine"));
	ExportReport refused = export_build(theirs);
	TEST_EXPECT(!refused.ok && refused.diagnostics.size() == 1 && refused.diagnostics[0].code() == "export.folder");
	TEST_EXPECT(files_under(theirs.export_dir) == std::vector<std::string>{"keep.txt"});
	TEST_EXPECT(!fs::exists(theirs.export_dir + kExportStagingSuffix));
	ExportRequest other = request;
	other.project_id = "another-project";
	refused = export_build(other);
	TEST_EXPECT(!refused.ok && refused.diagnostics[0].code() == "export.folder" && files_under(request.export_dir) == with_notes);
	ExportRequest staged = request;
	staged.export_dir = p.dir.file("staged");
	TEST_EXPECT(editor_test::write_text(staged.export_dir + kExportStagingSuffix + "/keep.txt", "mine"));
	refused = export_build(staged);
	TEST_EXPECT(!refused.ok && refused.diagnostics[0].code() == "export.folder" &&
	            fs::is_regular_file(staged.export_dir + kExportStagingSuffix + "/keep.txt") && !fs::exists(staged.export_dir));
	ExportRequest into = request;
	into.export_dir = build.build_dir + "/shipped";
	TEST_EXPECT(!export_build(into).ok && !fs::exists(into.export_dir));
	TEST_EXPECT(editor_test::tree_digest(build.build_dir) == tree);

	// The runtime's folder under runtime/ when asked; one not there refused.
	ExportRequest runtime = request;
	runtime.export_dir = p.dir.file("with_runtime");
	runtime.runtime_dir = p.dir.file("runtime_package");
	TEST_EXPECT(editor_test::write_text(runtime.runtime_dir + "/opennova.exe", "exe") &&
	            editor_test::write_text(runtime.runtime_dir + "/data/opennova.pck", "pck"));
	TEST_EXPECT(export_build(runtime).ok && fs::is_regular_file(runtime.export_dir + "/runtime/opennova.exe") &&
	            fs::is_regular_file(runtime.export_dir + "/runtime/data/opennova.pck") &&
	            fs::is_regular_file(runtime.export_dir + "/language.pff"));
	runtime.runtime_dir = p.dir.file("no_runtime");
	runtime.export_dir = p.dir.file("without_runtime");
	refused = export_build(runtime);
	TEST_EXPECT(!refused.ok && refused.diagnostics[0].code() == "export.runtime" && !fs::exists(runtime.export_dir));

	// An expansion's, laid out as in an install.
	const BaseInstall install = make_base(p.dir.file("install"), { { "basetable.bin", bytes_of("base") } }, {}, {});
	const BaseNames base = install.base();
	BuildTarget target;
	target.expansion = "jxm";
	target.install = install.root;
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	AssetGraph graph;
	ValidationCache cache;
	const std::vector<std::shared_ptr<const DocumentBase>> open;
	const BuildReport expansion =
	        run_build(plan_build(p.paths, scan, evaluate_requirements(p.doc, scan),
	                             validate_project({ p.paths, p.doc, scan, open }, graph, cache), target, &base),
	                  p.paths.build_dir + "/expansion");
	TEST_EXPECT(expansion.ok);
	ExportRequest mod = request;
	mod.build_dir = expansion.build_dir;
	mod.build_id = expansion.build_id;
	mod.expansion = "jxm";
	mod.export_dir = p.dir.file("mod");
	TEST_EXPECT(export_build(mod).ok);
	TEST_EXPECT(fs::is_regular_file(mod.export_dir + "/expansion/jxm/jxm.pff") &&
	            fs::is_regular_file(mod.export_dir + "/expansion/jxm/jxmL.pff") &&
	            fs::is_regular_file(mod.export_dir + "/expansion/jxm/intro.bik") && !fs::exists(mod.export_dir + "/language.pff"));
	TEST_EXPECT(opennova::io::read_file_text(mod.export_dir + "/" + kExportRecordFileName, text, error) &&
	            opennova::io::json_parse(text, record, error) && record.get_string("expansion", "") == "jxm");
	// The expansion over the standalone game's export: the earlier export's files the build no longer
	// holds removed (and said), the person's kept.
	ExportRequest over = mod;
	over.export_dir = request.export_dir;
	shipped = export_build(over);
	TEST_EXPECT(shipped.ok && fs::is_regular_file(over.export_dir + "/expansion/jxm/jxm.pff") &&
	            !fs::exists(over.export_dir + "/language.pff") && fs::is_regular_file(over.export_dir + "/notes.txt"));
	TEST_EXPECT(std::find(shipped.removed.begin(), shipped.removed.end(), "language.pff") != shipped.removed.end() &&
	            shipped.kept == std::vector<std::string>{"notes.txt"});
	return 0;
}

// A file of `size` bytes that is no text the game reads: a loose music bank's bytes.
static bool write_filler(const std::string &path, size_t size) {
	std::vector<uint8_t> bytes(size);
	for (size_t i = 0; i < size; ++i) bytes[i] = uint8_t((i * 2654435761u) >> 24);
	std::string error;
	fs::create_directories(fs::path(path).parent_path());
	return opennova::io::write_file_atomic(path, bytes.data(), bytes.size(), error);
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
		TEST_EXPECT(write_filler(p.root + "/extra/torn.aip", kSize)); // packed: resource.pff
		const BuildPlan plan = p.plan();
		TEST_EXPECT(plan.ok);
		// Where the file's bytes lie in the run's progress: the hash pass reads every archive's
		// entries in the plan's order, then the loose files; the packing and the copies follow in
		// the same order.
		const std::string name = pass == Pass::Archive ? "torn.aip" : "big.sbf";
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
				packed = packed || entry.logical_name == "torn.aip";
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
				TEST_EXPECT(opennova::io::read_file_bytes(archive.entries[i].source_path, payloads[i], error));
			for (size_t i = 0; i < archive.entries.size(); ++i)
				entries.push_back({archive.entries[i].logical_name.c_str(), payloads[i].empty() ? nullptr : payloads[i].data(),
				                   uint32_t(payloads[i].size()), 0, opennova::pff::PFF_NEW_ENTRY_TIMESTAMP, 0});
			const std::string single = p.dir.file(archive.file_name.c_str());
			TEST_EXPECT(opennova::pff::pff_write_archive(single.c_str(), opennova::pff::PFF_FORMAT_PFF3,
			                                             entries.empty() ? nullptr : entries.data(),
			                                             uint32_t(entries.size())) == opennova::pff::PFF_WRITE_OK);
			std::vector<uint8_t> built, expected;
			TEST_EXPECT(opennova::io::read_file_bytes((fs::path(run.report().build_dir) / archive.file_name).generic_string(), built, error));
			TEST_EXPECT(opennova::io::read_file_bytes(single, expected, error));
			TEST_EXPECT(!built.empty() && built == expected);
			// D-VFS-12: no entry stamped 0, which the game's effect loaders skip.
			opennova::pff::PffArchive opened{};
			const std::string at = (fs::path(run.report().build_dir) / archive.file_name).generic_string();
			TEST_EXPECT(opennova::pff::pff_open(&opened, at.c_str()) == 0);
			for (uint32_t i = 0; i < opened.entry_count; ++i)
				TEST_EXPECT(opened.entries[i].timestamp == opennova::pff::PFF_NEW_ENTRY_TIMESTAMP);
			opennova::pff::pff_close(&opened);
		}
	}
	return 0;
}

// A material chunk container under a name no rule types (renderer::load_material_chunk): an 8-byte
// header, then an NQ8B chunk of a 2 x 2 image, its size at +12, its width and height at +28 and +32.
static std::vector<uint8_t> chunk_file() {
	std::vector<uint8_t> chunk(8 + 8 + 28 + 2 * 2 * 4, 0);
	chunk[8] = 'N', chunk[9] = 'Q', chunk[10] = '8', chunk[11] = 'B';
	chunk[12] = uint8_t(chunk.size() - 16);
	chunk[28] = 2, chunk[32] = 2;
	return chunk;
}

static bool in_build(const BuildPlan &plan, const std::string &name) {
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries)
			if (entry.logical_name == name) return true;
	for (const BuildEntry &entry : plan.loose)
		if (entry.logical_name == name) return true;
	return false;
}

// S13 A8: a file of no kind the game knows is left out of the build, in no archive and not copied
// loose, whatever its name (the archives' limit binds it no more), the scan's warning saying so;
// asking whether such a file is a material chunk reads its chunk headers, never the whole of it.
// A material chunk under a name no rule types (a texture by its bytes before) packs with the art.
static int test_unknown_kinds_are_left_out() {
	Project p("opennova_editor_build_unknown_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	const std::string notes = "a_long_design_notes_file.xyz";
	TEST_EXPECT(editor_test::write_text(p.root + "/notes/" + notes, "design notes"));
	TEST_EXPECT(editor_test::write_bytes(p.root + "/art/field.nq8", chunk_file()));
	TEST_EXPECT(write_filler(p.root + "/art/sketch.blend", 4 * 1024 * 1024));
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	const AssetEntry *unknown = scan.find(notes);
	const AssetEntry *chunk = scan.find("field.nq8");
	const AssetEntry *sketch = scan.find("sketch.blend");
	TEST_EXPECT(unknown && unknown->kind == AssetKind::Unknown && sketch && sketch->kind == AssetKind::Unknown);
	TEST_EXPECT(chunk && chunk->kind == AssetKind::MaterialChunk);
	size_t warned = 0;
	for (const Diagnostic &d : scan.diagnostics) {
		TEST_EXPECT(d.code() != "asset.name.too_long");
		if (d.code() == "asset.kind.unknown" && d.asset == "notes/" + notes) {
			++warned;
			TEST_EXPECT(d.message.find("the build leaves it out") != std::string::npos);
		}
	}
	TEST_EXPECT(warned == 1);
	std::string key;
	AssetScan::Visit visit;
	uint64_t read = 0;
	TEST_EXPECT(scan_project_file(p.paths, p.doc, "art/sketch.blend", key, visit, &read));
	TEST_EXPECT(read > 0 && read <= 3 * (opennova::renderer::kChunkHeaderReads * 8 + 28));
	std::printf("editor_project_build: a 4 MB file no rule names: %llu bytes read to tell it holds no material chunk\n",
	            static_cast<unsigned long long>(read));
	const BuildPlan plan = p.plan();
	TEST_EXPECT(plan.ok && !in_build(plan, notes) && !in_build(plan, "sketch.blend") && in_build(plan, "field.nq8"));
	const BuildReport report = run_build(plan, p.output_root());
	TEST_EXPECT(report.ok && !fs::exists(fs::path(report.build_dir) / notes));
	opennova::Vfs vfs;
	TEST_EXPECT(vfs.mount_game(report.build_dir, std::string(), opennova::VfsMountMode::Packed,
	                           opennova::VfsArchiveDiscovery::RetailTable));
	TEST_EXPECT(!vfs.has_file(notes) && !vfs.has_file("sketch.blend") && vfs.has_file("field.nq8"));
	return 0;
}

// The project's notes, which the game never reads (asset_kinds' Notes row): a README and a list of sources in
// Markdown, a licence with no extension, a .txt of a name the game does not read. The scan gives them their kind
// and says nothing; the build leaves them out without a word. A .txt the game reads by its name still builds,
// loose beside the archives (earlyerr.txt, and _VIDTEST.TXT, whose being there the game checks); a file with no
// extension that holds bytes stays of no kind and is said, as it may be data the game misses.
static int test_project_notes_are_left_out_silently() {
	Project p("opennova_editor_build_notes_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::write_text(p.root + "/README.md", "# The game\r\n\r\nWhat this folder holds.\r\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/art/SOURCES.md", "| File | Source |\n|---|---|\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/LICENSE", "MIT License\n\nCopyright (c) the authors\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/notes/todo.txt", "the hut's roof\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/earlyerr.txt", "Line one\r\nLine two\r\n"));
	TEST_EXPECT(editor_test::write_text(p.root + "/_VIDTEST.TXT", ""));
	TEST_EXPECT(editor_test::write_bytes(p.root + "/data/blob", {0x4E, 0x00, 0x01, 0xFF}));
	const AssetScan scan = scan_project_assets(p.paths, p.doc);
	for (const char *name : {"README.md", "SOURCES.md", "LICENSE", "todo.txt"})
		TEST_EXPECT(scan.find(name) && scan.find(name)->kind == AssetKind::Notes);
	TEST_EXPECT(scan.find("earlyerr.txt") && scan.find("earlyerr.txt")->kind == AssetKind::Text);
	TEST_EXPECT(scan.find("_VIDTEST.TXT") && scan.find("_VIDTEST.TXT")->kind == AssetKind::Text);
	TEST_EXPECT(scan.find("blob") && scan.find("blob")->kind == AssetKind::Unknown);
	size_t unknown = 0;
	for (const Diagnostic &d : scan.diagnostics) {
		const AssetEntry *entry = scan.at_path(d.asset);
		TEST_EXPECT(!entry || entry->kind != AssetKind::Notes);
		if (d.code() == "asset.kind.unknown") {
			++unknown;
			TEST_EXPECT(d.asset == "data/blob");
		}
	}
	TEST_EXPECT(unknown == 1);
	const BuildPlan plan = p.plan();
	TEST_EXPECT(plan.ok);
	for (const Diagnostic &d : plan.diagnostics) {
		const AssetEntry *entry = scan.at_path(d.asset);
		TEST_EXPECT(!entry || entry->kind != AssetKind::Notes);
	}
	for (const char *name : {"README.md", "SOURCES.md", "LICENSE", "todo.txt", "blob"}) TEST_EXPECT(!in_build(plan, name));
	TEST_EXPECT(in_build(plan, "earlyerr.txt") && in_build(plan, "_VIDTEST.TXT"));
	const BuildReport report = run_build(plan, p.output_root());
	TEST_EXPECT(report.ok && fs::exists(fs::path(report.build_dir) / "earlyerr.txt") &&
	            fs::exists(fs::path(report.build_dir) / "_VIDTEST.TXT"));
	TEST_EXPECT(!fs::exists(fs::path(report.build_dir) / "README.md") && !fs::exists(fs::path(report.build_dir) / "LICENSE"));
	return 0;
}

// S13 A8: the build keeps each file's content hash by the size and last write it was read at (the
// plan's hash cache, the import cache's rule), so it reads only the files that changed: the first
// build every file, an unchanged one none (the same build), one with a file changed that file
// alone, writing its archive and linking the two others from the last build (no byte of them
// copied); a file written again with the same bytes is read again (its last write moved) and
// makes the same build; a file gone leaves the cache; a plan with no cache reads every file.
static int test_hash_cache() {
	Project p("opennova_editor_build_hash_cache_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(write_filler(p.root + "/music/extra.sbf", 1000));
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "zero"));
	// The project written a while ago: a file the cache keeps by its stamp (one written within the
	// settle window is read by every build until it settles, below).
	TEST_EXPECT(editor_test::backdate_tree(p.root, std::chrono::hours(1)));
	const BuildPlan plan = p.plan();
	TEST_EXPECT(plan.ok && plan.hash_cache == p.paths.build_cache_file);
	size_t files = 0;
	uint64_t bytes = 0;
	for (const BuildArchive &archive : plan.archives)
		for (const BuildEntry &entry : archive.entries) ++files, bytes += entry.size_bytes;
	for (const BuildEntry &entry : plan.loose) ++files, bytes += entry.size_bytes;
	TEST_EXPECT(!fs::exists(p.paths.build_cache_file));
	const BuildReport first = run_build(plan, p.output_root());
	TEST_EXPECT(first.ok && first.files_hashed == files && first.bytes_hashed == bytes && files > 4);
	std::printf("editor_project_build: a project of %zu files (%llu bytes) built, every file read; ", files,
	            static_cast<unsigned long long>(bytes));
	TEST_EXPECT(fs::is_regular_file(p.paths.build_cache_file));

	const BuildReport again = run_build(p.plan(), p.output_root());
	TEST_EXPECT(again.ok && again.reused_existing && again.files_hashed == 0 && again.bytes_hashed == 0);

	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "one") &&
	            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(50)));
	const BuildReport changed = run_build(p.plan(), p.output_root());
	TEST_EXPECT(changed.ok && !changed.reused_existing && changed.build_id != first.build_id);
	TEST_EXPECT(changed.files_hashed == 1 && changed.bytes_hashed == fs::file_size(p.root + "/strings/menutxt.bin"));
	TEST_EXPECT(changed.archives_written == std::vector<std::string>{"language.pff"});
	TEST_EXPECT(changed.archives_reused == std::vector<std::string>({"localres.pff", "resource.pff"}));
	TEST_EXPECT(changed.archives_linked == changed.archives_reused);
	std::printf("one string table changed: %zu file read (%llu bytes), %zu archive written, %zu linked\n",
	            changed.files_hashed, static_cast<unsigned long long>(changed.bytes_hashed),
	            changed.archives_written.size(), changed.archives_linked.size());
	for (const BuildArchive &archive : plan.archives) {
		std::vector<uint8_t> built;
		std::string error;
		TEST_EXPECT(opennova::io::read_file_bytes(changed.build_dir + "/" + archive.file_name, built, error) && !built.empty());
	}

	// The same bytes written again: read again, the same build.
	std::vector<uint8_t> table;
	std::string error;
	TEST_EXPECT(opennova::io::read_file_bytes(p.root + "/strings/menutxt.bin", table, error) &&
	            opennova::io::write_file_atomic(p.root + "/strings/menutxt.bin", table.data(), table.size(), error) &&
	            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(40)));
	const BuildReport touched = run_build(p.plan(), p.output_root());
	TEST_EXPECT(touched.ok && touched.reused_existing && touched.files_hashed == 1 && touched.build_id == changed.build_id);

	// Written within the settle window (its last write a minute ahead, as a clock that stood still
	// would leave it): read by every build, never kept by the cache, so the same bytes swapped in
	// under the same stamp are read too, the content they hold built.
	const fs::file_time_type ahead = fs::file_time_type::clock::now() + std::chrono::minutes(1);
	fs::last_write_time(p.root + "/strings/menutxt.bin", ahead);
	const BuildReport unsettled = run_build(p.plan(), p.output_root());
	TEST_EXPECT(unsettled.ok && unsettled.files_hashed == 1 && unsettled.build_id == changed.build_id);
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "two"));
	fs::last_write_time(p.root + "/strings/menutxt.bin", ahead);
	const BuildReport swapped = run_build(p.plan(), p.output_root());
	TEST_EXPECT(swapped.ok && swapped.files_hashed == 1 && swapped.build_id != changed.build_id);
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "one") &&
	            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(30)));
	TEST_EXPECT(run_build(p.plan(), p.output_root()).build_id == changed.build_id);

	// A file gone leaves the cache; the cache names the plan's files alone.
	fs::remove(p.root + "/music/extra.sbf");
	const BuildReport fewer = run_build(p.plan(), p.output_root());
	std::string text;
	TEST_EXPECT(fewer.ok && fewer.files_hashed == 0 && opennova::io::read_file_text(p.paths.build_cache_file, text, error));
	TEST_EXPECT(text.find("extra.sbf") == std::string::npos && text.find("menutxt.bin") != std::string::npos);

	// No cache: every file read, the same build. A rehash reads every file as well, the cache set
	// aside, and keeps it: the next build reads none.
	BuildPlan uncached = p.plan();
	uncached.hash_cache.clear();
	size_t now = uncached.loose.size();
	for (const BuildArchive &archive : uncached.archives) now += archive.entries.size();
	const BuildReport all = run_build(uncached, p.output_root());
	TEST_EXPECT(all.ok && all.reused_existing && all.files_hashed == now && all.build_id == fewer.build_id);
	BuildPlan rehashed = p.plan();
	rehashed.rehash = true;
	const BuildReport again_all = run_build(rehashed, p.output_root());
	TEST_EXPECT(again_all.ok && again_all.reused_existing && again_all.files_hashed == now);
	const BuildReport after = run_build(p.plan(), p.output_root());
	TEST_EXPECT(after.ok && after.files_hashed == 0 && after.build_id == fewer.build_id);
	return 0;
}

// The file at `path` held open as a running game holds an archive (S13 A8 review): read access,
// shared for reading and writing but not for deletion (the CRT's _SH_DENYNO), so none of its names
// can be removed while it is held. Elsewhere a plain read handle, which holds nothing back.
struct HeldFile {
#ifdef _WIN32
	int fd = -1;
	explicit HeldFile(const std::string &path) {
		_wsopen_s(&fd, system_path(path).c_str(), _O_RDONLY | _O_BINARY, _SH_DENYNO, _S_IREAD);
	}
	~HeldFile() {
		if (fd >= 0) _close(fd);
	}
	bool held() const { return fd >= 0; }
	// What the holder reads through its own handle, whatever became of the file's names.
	std::vector<uint8_t> bytes() {
		std::vector<uint8_t> out;
		if (fd < 0 || _lseeki64(fd, 0, SEEK_SET) != 0) return out;
		uint8_t chunk[4096];
		for (int n = 0; (n = _read(fd, chunk, sizeof(chunk))) > 0;) out.insert(out.end(), chunk, chunk + n);
		return out;
	}
#else
	std::ifstream in;
	explicit HeldFile(const std::string &path) : in(path, std::ios::binary) {}
	bool held() const { return static_cast<bool>(in); }
	std::vector<uint8_t> bytes() {
		std::vector<uint8_t> out;
		in.clear();
		in.seekg(0);
		char chunk[4096];
		while (in.read(chunk, sizeof(chunk)) || in.gcount() > 0)
			out.insert(out.end(), chunk, chunk + in.gcount());
		return out;
	}
#endif
};

// S13 A8 review: a publish refused while a file in the staging directory is held (a scanner, an
// indexer: a rename of a folder whose file is open is refused on Windows) is tried again on the next
// steps, a few seconds at most, and lands once the file is let go.
static int test_publish_waits_for_a_held_file() {
	Project p("opennova_editor_build_publish_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	BuildRun run(p.plan(), p.output_root());
	size_t steps = 0;
	while (!run.done() && run.items_done() < run.items_total() && ++steps < 100000) run.step(uint64_t(1) << 20);
	TEST_EXPECT(!run.done() && run.items_done() == run.items_total() && staging_dirs(p.output_root()) == 1);
	std::string staged;
	std::error_code ec;
	for (const fs::directory_entry &entry : fs::directory_iterator(p.output_root(), ec))
		if (entry.path().extension() == kBuildStagingSuffix) staged = (entry.path() / "language.pff").generic_string();
	{
		const HeldFile scanner(staged);
		TEST_EXPECT(scanner.held());
		for (int i = 0; i < 3; ++i) run.step(uint64_t(1) << 20);
#ifdef _WIN32
		TEST_EXPECT(!run.done() && run.label() == "Publishing the build"); // refused, tried again
#endif
	}
	while (!run.done() && ++steps < 100000) run.step(uint64_t(1) << 20);
	for (const Diagnostic &d : run.report().diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(run.done() && run.report().ok && staging_dirs(p.output_root()) == 0);
	TEST_EXPECT(last_good_build_dir(p.output_root()) == run.report().build_dir);
	return 0;
}

// S13 A8 review: no build writes through a name another file stands behind. A game running on the
// last good build holds its archives; a build that linked two of them into its staging directory
// and was cancelled cannot take those links away; the same content built again stages under another
// name (`<id>.1.tmp`), links the archive afresh, and the last good build keeps every byte (it was
// written through before: its archive cut to nothing under the game). Its links go once the game
// lets go. An archive whose size moved since its build recorded it is packed again, never linked.
static int test_held_archives_are_never_written_through() {
	Project p("opennova_editor_build_held_test");
	TEST_EXPECT(p.create());
	TEST_EXPECT(p.fill());
	TEST_EXPECT(editor_test::backdate_tree(p.root, std::chrono::hours(1)));
	const BuildReport first = run_build(p.plan(), p.output_root());
	TEST_EXPECT(first.ok && !first.build_dir.empty());
	const std::string held_path = first.build_dir + "/localres.pff";
	std::vector<uint8_t> before;
	std::string error;
	TEST_EXPECT(opennova::io::read_file_bytes(held_path, before, error) && !before.empty());
	std::string record;
	TEST_EXPECT(opennova::io::read_file_text(first.build_dir + "/" + kBuildRecordFileName, record, error));
	TEST_EXPECT(record.find("\"size\"") != std::string::npos);
	{
		HeldFile game(held_path);
		TEST_EXPECT(game.held() && game.bytes() == before);
		// The string table changes: language.pff is packed, localres.pff and resource.pff link.
		TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "held") &&
		            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(50)));
		const BuildPlan plan = p.plan();
		{
			BuildRun cancelled(plan, p.output_root());
			size_t steps = 0;
			while (!cancelled.done() && cancelled.report().archives_linked.size() < 2 && ++steps < 100000)
				cancelled.step(4096);
			TEST_EXPECT(!cancelled.done() && cancelled.report().archives_linked.size() == 2);
			cancelled.cancel();
		}
		const BuildReport rebuilt = run_build(plan, p.output_root());
		for (const Diagnostic &d : rebuilt.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
		TEST_EXPECT(rebuilt.ok && rebuilt.build_dir != first.build_dir);
		TEST_EXPECT(rebuilt.archives_written == std::vector<std::string>{"language.pff"});
		TEST_EXPECT(rebuilt.archives_linked == std::vector<std::string>({"localres.pff", "resource.pff"}));
		// The game's archive, read through the game's own handle: every byte kept.
		TEST_EXPECT(game.bytes() == before);
		std::vector<uint8_t> linked;
		TEST_EXPECT(opennova::io::read_file_bytes(rebuilt.build_dir + "/localres.pff", linked, error) && linked == before);
#ifdef _WIN32
		// Held, the last good build is not pruned (a file that will not go keeps its directory, its
		// record last): its archive reads as it did by its name too.
		std::vector<uint8_t> after;
		TEST_EXPECT(opennova::io::read_file_bytes(held_path, after, error) && after == before);
#endif
		TEST_EXPECT(last_good_build_dir(p.output_root()) == rebuilt.build_dir);
	}
	// The game gone, the next publish prunes what the cancel left.
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "free") &&
	            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(40)));
	const BuildReport freed = run_build(p.plan(), p.output_root());
	TEST_EXPECT(freed.ok && staging_dirs(p.output_root()) == 0);

	// localres.pff grown a byte since its build recorded it: packed again, not linked.
	{
		std::ofstream grow(freed.build_dir + "/localres.pff", std::ios::binary | std::ios::app);
		grow.put('\0');
	}
	TEST_EXPECT(write_table(p.root + "/strings/menutxt.bin", "grown") &&
	            editor_test::backdate(p.root + "/strings/menutxt.bin", std::chrono::minutes(30)));
	const BuildReport repacked = run_build(p.plan(), p.output_root());
	TEST_EXPECT(repacked.ok && repacked.archives_linked == std::vector<std::string>{"resource.pff"});
	TEST_EXPECT(std::find(repacked.archives_written.begin(), repacked.archives_written.end(), "localres.pff") !=
	            repacked.archives_written.end());
	return 0;
}

// S13 A8: an output root deep enough that the build's staging and its files pass Windows' MAX_PATH
// (260 characters) builds, mounts and publishes as a short one does (every call to the system takes
// the path in its extended form, system_path), and the next build prunes it; a project folder
// some 205 characters long whatever the temp folder, its own files short of the limit.
static int test_deep_output_root() {
	editor_test::TempProjectDir dir("opennova_editor_build_deep_test");
	std::string root = dir.root();
	while (root.size() < 200) root += "/" + std::string(std::min<size_t>(48, std::max<size_t>(1, 199 - root.size())), 'd');
	root += "/Game";
	ProjectDocument doc;
	Diagnostic created;
	TEST_EXPECT(create_project(root, "Deep", "jo", doc, created));
	const ProjectPaths paths = ProjectPaths::for_root(root);
	{
		const AssetScan scan = scan_project_assets(paths, doc);
		const RequirementReport report = evaluate_requirements(doc, scan);
		const CreateMissingResult made = create_missing_requirements(paths, doc, scan, report, unmet_required_roles(report));
		TEST_EXPECT(made.unavailable.empty() && made.diagnostics.empty());
	}
	const std::string video = "a_long_intro_movie_name.bik";
	TEST_EXPECT(editor_test::write_text(root + "/video/" + video, "BIKi"));
	TEST_EXPECT(write_table(root + "/strings/menutxt.bin", "shallow"));
	TEST_EXPECT(editor_test::backdate_tree(root, std::chrono::hours(1))); // written a while ago
	const auto plan_of = [&]() {
		const AssetScan scan = scan_project_assets(paths, doc);
		AssetGraph graph;
		ValidationCache cache;
		const std::vector<std::shared_ptr<const DocumentBase>> open;
		return plan_build(paths, scan, evaluate_requirements(doc, scan),
		                  validate_project({ paths, doc, scan, open }, graph, cache));
	};
	const std::string output_root = paths.build_dir + "/play";
	const BuildPlan plan = plan_of();
	TEST_EXPECT(plan.ok);
	const BuildReport report = run_build(plan, output_root);
	for (const Diagnostic &d : report.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(report.ok && !report.build_dir.empty());
	const std::string copied = report.build_dir + "/" + video;
	TEST_EXPECT(copied.size() > 260); // past MAX_PATH, where the build failed before S13 A8
	std::printf("editor_project_build: a project folder %zu characters long built, its loose copy's path %zu\n",
	            root.size(), copied.size());
	std::error_code ec;
	TEST_EXPECT(fs::is_regular_file(system_path(copied), ec));
	TEST_EXPECT(fs::is_regular_file(system_path(report.build_dir + "/localres.pff"), ec));
	TEST_EXPECT(last_good_build_dir(output_root) == report.build_dir);

	TEST_EXPECT(write_table(root + "/strings/menutxt.bin", "deep") &&
	            editor_test::backdate(root + "/strings/menutxt.bin", std::chrono::minutes(50)));
	const BuildReport next = run_build(plan_of(), output_root);
	for (const Diagnostic &d : next.diagnostics) std::fprintf(stderr, "%s: %s\n", d.code().c_str(), d.message.c_str());
	TEST_EXPECT(next.ok && next.build_dir != report.build_dir && next.files_hashed == 1);
	TEST_EXPECT(!fs::exists(system_path(report.build_dir), ec)); // the deep build before it pruned
	TEST_EXPECT(fs::is_regular_file(system_path(next.build_dir + "/" + video), ec));
	return 0;
}

// Several projects building into one folder (Build to folder; the review's M6): each build names its project
// in its record, reuses, prunes and replaces only its own project's builds, and says how many of the others'
// it left. Another project's build of the same files is left as it is: the build is refused, saying so.
static int test_shared_folder() {
	Project a("opennova_editor_build_shared_a"), b("opennova_editor_build_shared_b");
	TEST_EXPECT(a.create() && a.fill() && b.create() && b.fill());
	const std::string shared = a.dir.file("shared");
	const auto planned = [](Project &p, const char *project) {
		BuildPlan plan = p.plan();
		plan.project = project;
		return plan;
	};
	const BuildReport first_a = run_build(planned(a, "project-a"), shared);
	TEST_EXPECT(first_a.ok && first_a.others.empty());
	// b with the same files: a's build of them is a's, left as it is.
	const BuildReport same = run_build(planned(b, "project-b"), shared);
	bool said = false;
	for (const Diagnostic &d : same.diagnostics) said = said || d.message.find("another project's build of the same files") != std::string::npos;
	TEST_EXPECT(!same.ok && said && fs::is_directory(first_a.build_dir));
	// b with files of its own: built beside a's, which it names and leaves.
	TEST_EXPECT(editor_test::write_text(b.root + "/music/menumus.sbf", "SBF!"));
	const BuildReport first_b = run_build(planned(b, "project-b"), shared);
	TEST_EXPECT(first_b.ok && first_b.build_id != first_a.build_id && fs::is_directory(first_a.build_dir));
	TEST_EXPECT(first_b.others == std::vector<std::string>({first_a.build_id}));
	// a changed: its own last build pruned, b's left and named.
	TEST_EXPECT(editor_test::write_text(a.root + "/music/menumus.sbf", "SBF?"));
	const BuildReport second_a = run_build(planned(a, "project-a"), shared);
	TEST_EXPECT(second_a.ok && !fs::exists(first_a.build_dir) && fs::is_directory(first_b.build_dir));
	TEST_EXPECT(second_a.others == std::vector<std::string>({first_b.build_id}));
	// Unchanged: handed back, b's named still.
	const BuildReport again = run_build(planned(a, "project-a"), shared);
	TEST_EXPECT(again.ok && again.reused_existing && again.others == std::vector<std::string>({first_b.build_id}));
	return 0;
}

int main() {
	int failures = 0;
	failures += test_shared_folder();
	failures += test_steps_are_bounded();
	failures += test_cancel_publishes_nothing();
	failures += test_file_rewritten_mid_read_fails();
	failures += test_archives_match_the_single_call_writer();
	failures += test_routing();
	failures += test_new_kinds_land_where_their_rows_say();
	failures += test_empty_project_is_blocked();
	failures += test_filled_project_builds_and_mounts();
	failures += test_mission_project_builds_clean();
	failures += test_protected_build_survives_and_archives_are_refused();
	failures += test_long_names_bind_packed_files_only();
	failures += test_unknown_kinds_are_left_out();
	failures += test_project_notes_are_left_out_silently();
	failures += test_hash_cache();
	failures += test_held_archives_are_never_written_through();
	failures += test_publish_waits_for_a_held_file();
	failures += test_deep_output_root();
	failures += test_expansion_routing();
	failures += test_expansion_layout();
	failures += test_expansion_empty_pair();
	failures += test_export();
	failures += test_lean_packing();
	failures += test_base_gate();
	failures += test_expansion_table_texts();
	if (failures == 0) std::printf("editor_project_build: all tests passed\n");
	return failures == 0 ? 0 : 1;
}
