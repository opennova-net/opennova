#include "commands.h"

#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_import.h>
#include <editor/graph/asset_graph.h>
#include <editor/import/import_plan.h>
#include <editor/import/import_run.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/create_missing.h>
#include <editor/model/diagnostic.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_run.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/project/project_findings.h>
#include <editor/project/project_state.h>
#include <editor/assets/project_asset_source.h>
#include <editor/preview/menu_render_check.h>
#include <editor/requirements/requirements.h>
#include <editor/run/play_lease.h>

#include <editor/documents/document_types.h>
#include <algorithm>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

using namespace opennova::editor;

namespace opennova::project_cli {

namespace {

int usage(std::FILE *err, const char *why) {
	if (why != nullptr) std::fprintf(err, "opennova-project: %s\n", why);
	std::fprintf(err,
	             "usage: opennova-project new <dir> [--title <text>] [--game <code>]\n"
	             "       opennova-project status <dir>\n"
	             "       opennova-project validate <dir>\n"
	             "       opennova-project create-missing <dir> [--role <token>]\n"
	             "       opennova-project import <dir> <source> [--entry <name>]... [--replace]\n"
	             "                               [--with-dependencies] [--dry-run]\n"
	             "       opennova-project import <dir> --install <game install> --entry <name>... [--replace]\n"
	             "                               [--with-dependencies] [--dry-run]\n"
	             "       opennova-project reimport <dir> [--force] [--source <path>]\n"
	             "       opennova-project build <dir> [--out <dir>]\n"
	             "  new             create an empty project (project.opennova + .opennova/) in <dir>\n"
	             "  status          the project's title, game, asset count and requirements summary\n"
	             "  validate        list every finding; exit 1 when a required file is missing or wrong\n"
	             "  create-missing  create every missing required file from scratch (or one, by role)\n"
	             "  import          copy files (loose, PFF members, or a game install's effective files) in,\n"
	             "                  the whole selection or none of it; an .o3d (a model) or an .o3a (a clip\n"
	             "                  set) the Blender add-on wrote converts to the .3di or the .adm and .bad;\n"
	             "                  --with-dependencies also copies the files they need, found beside them\n"
	             "                  or in the game install (the --install one, else the project's), 1000\n"
	             "                  files at most; an .o3d's textures come only with --with-dependencies;\n"
	             "                  --dry-run prints the plan and writes nothing (no import pass either)\n"
	             "  reimport        run the import pass now; --force imports again every source (or the\n"
	             "                  --source one) even when nothing changed\n"
	             "  every command that reads a project first imports the sources that changed, as the\n"
	             "  editor does\n"
	             "  build           pack the project into a game directory the runtime boots\n"
	             "                  (default: <dir>/.opennova/build/play/<build-id>)\n");
	return 2;
}

void print_diagnostic(std::FILE *out, const Diagnostic &d) {
	std::fprintf(out, "%s %s: %s", diagnostic_severity_label(d.severity), d.code.c_str(),
	             d.message.c_str());
	if (!d.asset.empty()) std::fprintf(out, " [%s]", d.asset.c_str());
	if (d.line) std::fprintf(out, " line %zu", d.line);
	if (!d.record.empty()) std::fprintf(out, " record %s", d.record.c_str());
	if (!d.field.empty()) std::fprintf(out, " field %s", d.field.c_str());
	std::fputc('\n', out);
}

int report_error(std::FILE *err, const Diagnostic &d) {
	std::fprintf(err, "opennova-project: %s\n", d.message.c_str());
	return 2;
}

int command_new(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	std::string dir, title, game = kDefaultTargetGame;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		const auto value = [&](std::string &into) -> bool {
			if (i + 1 >= argc) return false;
			into = argv[++i];
			return true;
		};
		if (arg == "--title") {
			if (!value(title)) return usage(err, "--title needs a text");
		} else if (arg == "--game") {
			if (!value(game)) return usage(err, "--game needs a code");
		} else if (!arg.empty() && arg[0] == '-') {
			return usage(err, ("unknown option " + arg).c_str());
		} else if (dir.empty()) {
			dir = arg;
		} else {
			return usage(err, "new takes one directory");
		}
	}
	if (dir.empty()) return usage(err, "new needs a directory");
	ProjectDocument doc;
	Diagnostic error;
	if (!create_project(dir, title, game, doc, error)) return report_error(err, error);
	const ProjectPaths paths = ProjectPaths::for_root(dir);
	std::fprintf(out, "created %s (%s) at %s\n", doc.title.c_str(), doc.target_game.c_str(),
	             paths.root.c_str());
	return 0;
}

struct OpenedProject {
	ProjectPaths paths;
	ProjectDocument doc;
	LocalSettings local;
	ProjectState state; // the imports, the scan and the requirements
};

// The project opened without the refresh: its document and local settings; false (said)
// when either does not read.
bool open_settings(const std::string &dir, OpenedProject &project, std::FILE *err) {
	project.paths = ProjectPaths::for_root(dir);
	Diagnostic error;
	if (!open_project(dir, project.doc, error)) {
		report_error(err, error);
		return false;
	}
	// The command line has no machine setting to seed a project's install from: it reads the
	// one the project names (the editor's, once the editor has opened or set it). A local.json of
	// another schema is set aside with a warning, and the command goes on as for a project with
	// none.
	Diagnostic finding;
	if (!open_local_settings(project.paths, std::string(), project.local, finding)) {
		report_error(err, finding);
		return false;
	}
	if (!finding.code.empty())
		print_diagnostic(err, finding);
	return true;
}

// The project opened the way the editor opens it: the engine's one refresh (the import
// pass, the scan, the requirements) runs first, so every command sees what the
// importers make and their findings (ADR 0046 d4, d8). `force` and `only` are the
// import pass's (reimport).
bool open_for_report(const std::string &dir, OpenedProject &project, std::FILE *err, bool force = false,
                     const std::string &only = std::string()) {
	if (!open_settings(dir, project, err)) return false;
	project.state = refresh_project_state(project.paths, project.doc, force, only);
	return true;
}

// The project opened to be read and nothing else: a scan, no import pass (which writes the
// outputs of the sources that changed, their records and the import cache).
bool open_read_only(const std::string &dir, OpenedProject &project, std::FILE *err) {
	if (!open_settings(dir, project, err)) return false;
	project.state.scan = scan_project_assets(project.paths, project.doc);
	return true;
}

// The sources this command's import pass imported (the rest were current).
void print_imported(std::FILE *out, const ImportRunResult &imports) {
	for (const ImportedSource &source : imports.sources)
		if (source.reimported)
			std::fprintf(out, "imported %s -> %zu output(s)\n", source.source.c_str(), source.outputs.size());
}

void print_summary(std::FILE *out, const OpenedProject &project) {
	std::fprintf(out, "project: %s (%s) at %s\n", project.doc.title.c_str(),
	             project.doc.target_game.c_str(), project.paths.root.c_str());
	std::fprintf(out, "runtime: %s\n",
	             project.local.runtime_executable.empty() ? "(beside the editor)"
	                                                       : project.local.runtime_executable.c_str());
	if (!project.local.game_install.empty())
		std::fprintf(out, "game install: %s\n", project.local.game_install.c_str());
	std::map<std::string, int> by_kind;
	for (const AssetEntry &asset : project.state.scan.entries) ++by_kind[asset_kind_label(asset.kind)];
	std::fprintf(out, "assets: %zu file(s)\n", project.state.scan.entries.size());
	for (const auto &[label, count] : by_kind) std::fprintf(out, "  %d %s\n", count, label.c_str());
	const ImportRunResult &imports = project.state.imports;
	if (!imports.sources.empty()) {
		size_t failed = 0;
		for (const ImportedSource &source : imports.sources) failed += source.ok ? 0 : 1;
		std::fprintf(out, "imports: %zu source(s), %zu imported now, %zu failed\n", imports.sources.size(), imports.reimported,
		             failed);
	}
	const RequirementReport &r = project.state.requirements;
	std::fprintf(out, "requirements: %d required, %d missing, %d wrong kind (missions %s)\n",
	             r.required_total, r.required_missing, r.required_wrong_kind,
	             project.doc.features.mission ? "on" : "off");
}

int command_status(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc != 2) return usage(err, "status needs a directory");
	OpenedProject project;
	if (!open_for_report(argv[1], project, err)) return 2;
	print_summary(out, project);
	return 0;
}

// Every finding the editor's Problems lists for the project as it is on disk, composed the
// one way the editor composes them (project/project_findings): the scan's (with the import
// pass's), the requirements', the documents' and the graph's, the menu render check's notes.
// A project read here has no boot report, no Play, no open document and no last build.
int command_validate(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc != 2) return usage(err, "validate needs a directory");
	OpenedProject project;
	if (!open_for_report(argv[1], project, err)) return 2;
	print_summary(out, project);
	AssetGraph graph;
	ValidationCache cache;
	MenuRenderCheck render_check;
	ProjectAssetSource files;
	files.set_scan(project.paths.root, project.state.scan, project.doc.target_game);
	const std::vector<std::shared_ptr<const Document>> open;
	const std::vector<std::string> boot_missing;
	const std::vector<Diagnostic> none;
	const ProjectFindings findings =
	        compose_project_findings({project.paths, project.doc, project.state.scan, project.state.requirements, open,
	                                  boot_missing, none, none, none},
	                                 graph, cache, render_check, files);
	int errors = 0;
	for (const Diagnostic &d : findings.rows) {
		print_diagnostic(out, d);
		if (d.severity == DiagnosticSeverity::Error) ++errors;
	}
	std::fprintf(out, "%s: %d error(s)\n", errors == 0 ? "ok" : "not ok", errors);
	return errors == 0 ? 0 : 1;
}

int command_create_missing(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	std::string dir, role;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--role") {
			if (i + 1 >= argc) return usage(err, "--role needs a token");
			role = argv[++i];
		} else if (!arg.empty() && arg[0] == '-') {
			return usage(err, ("unknown option " + arg).c_str());
		} else if (dir.empty()) {
			dir = arg;
		} else {
			return usage(err, "create-missing takes one directory");
		}
	}
	if (dir.empty()) return usage(err, "create-missing needs a directory");
	OpenedProject project;
	if (!open_for_report(dir, project, err)) return 2;
	// The one role asked for, else every Required row the project does not meet.
	const std::vector<std::string> roles =
	        role.empty() ? unmet_required_roles(project.state.requirements) : std::vector<std::string>{role};
	const CreateMissingResult result =
	        create_missing_requirements(project.paths, project.doc, project.state.requirements, roles);
	for (const std::string &path : result.created) std::fprintf(out, "created %s\n", path.c_str());
	for (const std::string &name : result.unavailable)
		std::fprintf(out, "cannot create %s yet: no writer for this kind of file\n", name.c_str());
	for (const Diagnostic &d : result.diagnostics) print_diagnostic(out, d);
	const bool complete = result.unavailable.empty() && !diagnostics_have_errors(result.diagnostics);
	std::fprintf(out, "%zu file(s) created%s\n", result.created.size(), complete ? "" : ", some requirements remain");
	return complete ? 0 : 1;
}

struct PrintProgress : BuildProgress {
	std::FILE *out;
	explicit PrintProgress(std::FILE *o) : out(o) {}
	void on_step(const std::string &what, size_t done, size_t total) override {
		std::fprintf(out, "  [%zu/%zu] %s\n", done, total, what.c_str());
	}
};

// What a reference that wants a file is: the file naming it, the record and the field.
std::string need_words(const ImportNeed &need) {
	std::string out = need.file;
	if (!need.record.empty()) out += ": " + need.record;
	if (!need.field.empty()) out += (need.record.empty() ? ": " : " ") + need.field;
	return out;
}

// An import's plan, one line per file: what it takes (and where it puts it, what wanted it,
// where it comes from, the other places that have it), what it cannot take and why, what
// is not found, the kinds not followed, and whether the cap stopped it.
void print_plan(std::FILE *out, const ImportPlan &plan) {
	size_t take = 0, missing = 0;
	for (const ImportPlanRow &row : plan.rows) {
		if (row.state == ImportPlanRow::State::NotFound) {
			++missing;
			std::fprintf(out, "not found %s (%s), needed by %s\n", row.name.c_str(), asset_kind_label(row.kind),
			             need_words(row.needed_by).c_str());
			continue;
		}
		take += row.selected ? 1 : 0;
		std::string line = std::string(row.selected ? "take " : "skip ") + row.name + " (" + asset_kind_label(row.kind) +
		                   ") -> " + row.destination;
		line += row.state == ImportPlanRow::State::Found ? ", needed by " + need_words(row.needed_by) : std::string(", chosen");
		line += row.made_from.empty() ? ", from " + row.found_in : ", made from " + row.made_from + ", " + row.found_in;
		if (!row.problem.empty()) line += ": " + row.problem;
		std::fprintf(out, "%s\n", line.c_str());
		for (const ImportRival &rival : row.rivals)
			std::fprintf(out, "  also in %s as %s (%s)\n", rival.found_in.c_str(), rival.name.c_str(),
			             rival.differs ? "the files differ" : "the same file");
	}
	for (const ImportNotFollowed &entry : plan.not_followed) {
		if (entry.reference == ReferenceKind::None)
			std::fprintf(out, "not followed: what %s files name (%zu, the first %s)\n", asset_kind_label(entry.kind),
			             entry.count, entry.first.c_str());
		else
			std::fprintf(out, "not followed: %s references, which name no file (%zu, the first in %s)\n",
			             reference_row(entry.reference).token, entry.count, entry.first.c_str());
	}
	if (plan.truncated) std::fprintf(out, "the plan stopped at %zu files: the files past them are not in it\n", kImportPlanFileCap);
	std::fprintf(out, "plan: %zu file(s) to import, %zu not found\n", take, missing);
}

int command_import(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc < 3) return usage(err, "import needs a project directory and a source file");
	std::vector<ImportSource> sources;
	bool replace = false, with_dependencies = false, dry_run = false;
	const bool install = std::string(argv[2]) == "--install";
	std::string source_path = install ? std::string() : argv[2];
	std::vector<std::string> entries;
	for (int i = 3; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--replace") replace = true;
		else if (arg == "--with-dependencies") with_dependencies = true;
		else if (arg == "--dry-run") dry_run = true;
		else if (arg == "--entry" && i + 1 < argc) entries.push_back(argv[++i]);
		else if (install && source_path.empty() && !arg.empty() && arg[0] != '-') source_path = arg; // the game install
		else return usage(err, ("unknown or incomplete import option " + arg).c_str());
	}
	if (install && source_path.empty()) return usage(err, "--install needs the game install folder");
	for (const std::string &entry : entries) {
		ImportSource source;
		source.path = source_path;
		source.entry = entry;
		source.install = install;
		sources.push_back(source);
	}
	if (sources.empty()) {
		if (install) return usage(err, "choose the game's files with --entry <name> (repeat for more files)");
		if (opennova::strutil::ends_with_icase(argv[2], ".pff"))
			return usage(err, "choose PFF members with --entry <name> (repeat for more files)");
		sources.push_back({argv[2], {}});
	}
	// A dry run reads the project and writes nothing: no import pass either.
	OpenedProject project;
	if (!(dry_run ? open_read_only(argv[1], project, err) : open_for_report(argv[1], project, err))) return 2;
	// The plan, as the editor's import dialog shows it: the files the sources need looked for
	// beside them and in the game install (the one named, else the project's).
	bool truncated = false;
	if (with_dependencies || dry_run) {
		AssetGraph graph;
		graph.update(project.paths, project.doc, project.state.scan, {});
		const ImportPlan plan = plan_import(sources, with_dependencies, project.paths, project.doc, project.state.scan,
		                                    graph, install ? source_path : project.local.game_install);
		for (const Diagnostic &d : plan.diagnostics) print_diagnostic(err, d);
		if (dry_run) {
			print_plan(out, plan);
			return diagnostics_have_errors(plan.diagnostics) || plan.truncated ? 1 : 0;
		}
		// A source that could not be read or converted: nothing is imported, as import_assets
		// refuses the selection.
		if (diagnostics_have_errors(plan.diagnostics)) return 1;
		// What the plan takes and nothing else: the sources it holds (the files a converter
		// makes from one, whole) and each dependency found that the project can take; a source
		// past its cap is not imported.
		std::vector<ImportSource> planned;
		for (const ImportPlanRow &row : plan.rows) {
			if (row.state == ImportPlanRow::State::NotFound) {
				std::fprintf(out, "not found %s (%s), needed by %s\n", row.name.c_str(), asset_kind_label(row.kind),
				             need_words(row.needed_by).c_str());
				continue;
			}
			if (!row.selected) {
				std::fprintf(err, "not importing %s: %s\n", row.name.c_str(), row.problem.c_str());
				continue;
			}
			if (std::find(planned.begin(), planned.end(), row.source) == planned.end()) planned.push_back(row.source);
		}
		truncated = plan.truncated;
		if (truncated)
			std::fprintf(err, "the plan stopped at %zu files: the files past them are not imported (import fewer at once)\n",
			             kImportPlanFileCap);
		sources = std::move(planned);
	}
	const ImportResult result = import_assets(sources, project.paths, project.doc, replace);
	for (const auto &path : result.imported) std::fprintf(out, "imported %s\n", path.c_str());
	for (const auto &path : result.not_imported) std::fprintf(out, "not imported %s\n", path.c_str());
	for (const auto &d : result.diagnostics) print_diagnostic(err, d);
	bool import_errors = false;
	if (!result.imported.empty()) {
		// As the editor rescans after an import: a copied-in source is imported now, and a
		// failure on a file this command brought in fails the command.
		project.state = refresh_project_state(project.paths, project.doc);
		print_imported(out, project.state.imports);
		for (const Diagnostic &d : project.state.imports.diagnostics) {
			print_diagnostic(err, d);
			for (const std::string &path : result.imported)
				if (d.severity == DiagnosticSeverity::Error && (d.asset == path || d.asset == path + kImportSidecarSuffix))
					import_errors = true;
		}
	}
	return result.diagnostics.empty() && !import_errors && !truncated ? 0 : 1;
}

int command_reimport(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	std::string dir, source;
	bool force = false;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--force") force = true;
		else if (arg == "--source" && i + 1 < argc) source = argv[++i];
		else if (!arg.empty() && arg[0] == '-') return usage(err, ("unknown option " + arg).c_str());
		else if (dir.empty()) dir = arg;
		else return usage(err, "reimport takes one directory");
	}
	if (dir.empty()) return usage(err, "reimport needs a directory");
	OpenedProject project;
	if (!open_for_report(dir, project, err, force, source)) return 2;
	const ImportRunResult &result = project.state.imports;
	for (const ImportedSource &imported : result.sources)
		std::fprintf(out, "%s %s -> %zu output(s)%s\n", imported.reimported ? "imported" : "kept", imported.source.c_str(),
		             imported.outputs.size(), imported.ok ? "" : " (failed)");
	int errors = 0;
	for (const Diagnostic &d : result.diagnostics) {
		print_diagnostic(err, d);
		if (d.severity == DiagnosticSeverity::Error) ++errors;
	}
	std::fprintf(out, "%zu source(s), %zu imported\n", result.sources.size(), result.reimported);
	return errors == 0 ? 0 : 1;
}

int command_build(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	std::string dir, out_dir;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--out") {
			if (i + 1 >= argc) return usage(err, "--out needs a directory");
			out_dir = argv[++i];
		} else if (!arg.empty() && arg[0] == '-') {
			return usage(err, ("unknown option " + arg).c_str());
		} else if (dir.empty()) {
			dir = arg;
		} else {
			return usage(err, "build takes one directory");
		}
	}
	if (dir.empty()) return usage(err, "build needs a directory");
	OpenedProject project;
	if (!open_for_report(dir, project, err)) return 2;
	print_imported(out, project.state.imports);
	// The document gate is the same validation `validate` prints, run once here.
	const std::vector<Diagnostic> findings =
	        validate_open_documents(project.paths, project.doc, project.state.scan, {}, nullptr, nullptr);
	const BuildPlan plan = plan_build(project.paths, project.state.scan, project.state.requirements, findings);
	if (!plan.ok) {
		for (const Diagnostic &d : plan.diagnostics) print_diagnostic(out, d);
		std::fprintf(out, "not ok: the project cannot be built until these are fixed\n");
		return 1;
	}
	const std::string output_root = out_dir.empty() ? project.paths.build_dir + "/play" : out_dir;
	if (out_dir.empty()) {
		// The default output lands under the cache, which keeps itself out of the modder's
		// repository; a cache that cannot be made fails the build's own first step.
		std::string cache_error;
		ensure_project_cache_dir(project.paths, cache_error);
	}
	PrintProgress progress(out);
	// A game the editor started may run from a build here: every directory a lease names is kept,
	// asked when the build publishes. The command line cannot tell whether a lease's game still
	// runs (the editor's platform can), so it keeps them all and deletes none (run/play_lease.h).
	const ProtectedDirs leased = [&output_root] {
		return leased_build_dirs(output_root, [](int64_t, const std::string &) { return ProcessLiveness::Unknown; });
	};
	const BuildReport report = run_build(plan, output_root, leased, &progress);
	for (const Diagnostic &d : report.diagnostics) print_diagnostic(out, d);
	if (!report.ok) {
		std::fprintf(out, "not ok: build failed\n");
		return 1;
	}
	if (report.reused_existing) {
		std::fprintf(out, "unchanged: %s\n", report.build_dir.c_str());
	} else {
		std::fprintf(out, "built %s (%zu archive(s) written, %zu reused, %zu loose file(s))\n",
		             report.build_dir.c_str(), report.archives_written.size(), report.archives_reused.size(),
		             report.loose_written.size());
	}
	std::fprintf(out, "run: opennova.exe -- --resource-dir \"%s\"\n", report.build_dir.c_str());
	return 0;
}

} // namespace

int run_project_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc < 1) return usage(err, nullptr);
	const std::string command = argv[0];
	if (command == "new") return command_new(argc, argv, out, err);
	if (command == "status") return command_status(argc, argv, out, err);
	if (command == "validate") return command_validate(argc, argv, out, err);
	if (command == "create-missing") return command_create_missing(argc, argv, out, err);
	if (command == "import") return command_import(argc, argv, out, err);
	if (command == "reimport") return command_reimport(argc, argv, out, err);
	if (command == "build") return command_build(argc, argv, out, err);
	if (command == "-h" || command == "--help" || command == "help") return usage(err, nullptr);
	return usage(err, ("unknown command " + command).c_str());
}

} // namespace opennova::project_cli
