#include "commands.h"

#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_import.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/create_missing.h>
#include <editor/model/diagnostic.h>
#include <editor/project_build/build_plan.h>
#include <editor/project_build/build_session.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

#include <editor/documents/catalog_validation.h>
#include <cstring>
#include <map>
#include <string>

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
	             "       opennova-project build <dir> [--out <dir>]\n"
	             "  new             create an empty project (project.opennova + .opennova/) in <dir>\n"
	             "  status          the project's title, game, asset count and requirements summary\n"
	             "  validate        list every finding; exit 1 when a required file is missing or wrong\n"
	             "  create-missing  create every missing required file from scratch (or one, by role)\n"
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
	AssetScan scan;
	RequirementReport requirements;
};

bool open_for_report(const std::string &dir, OpenedProject &project, std::FILE *err) {
	project.paths = ProjectPaths::for_root(dir);
	Diagnostic error;
	if (!open_project(dir, project.doc, error)) {
		report_error(err, error);
		return false;
	}
	if (!load_local_settings(project.paths.local_settings_file, project.local, error)) {
		report_error(err, error);
		return false;
	}
	project.scan = scan_project_assets(project.paths, project.doc);
	project.requirements = evaluate_requirements(project.doc, project.scan);
	return true;
}

void print_summary(std::FILE *out, const OpenedProject &project) {
	std::fprintf(out, "project: %s (%s) at %s\n", project.doc.title.c_str(),
	             project.doc.target_game.c_str(), project.paths.root.c_str());
	std::fprintf(out, "runtime: %s\n",
	             project.local.runtime_executable.empty() ? "(beside the editor)"
	                                                       : project.local.runtime_executable.c_str());
	if (!project.local.retail_root.empty())
		std::fprintf(out, "retail data: %s\n", project.local.retail_root.c_str());
	std::map<std::string, int> by_kind;
	for (const AssetEntry &asset : project.scan.entries) ++by_kind[asset_kind_label(asset.kind)];
	std::fprintf(out, "assets: %zu file(s)\n", project.scan.entries.size());
	for (const auto &[label, count] : by_kind) std::fprintf(out, "  %d %s\n", count, label.c_str());
	const RequirementReport &r = project.requirements;
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

int command_validate(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc != 2) return usage(err, "validate needs a directory");
	OpenedProject project;
	if (!open_for_report(argv[1], project, err)) return 2;
	print_summary(out, project);
	int errors = 0;
	for (const auto &d : validate_catalogs(project.paths, project.doc, project.scan)) {
		print_diagnostic(out, d);
		if (d.severity == DiagnosticSeverity::Error) ++errors;
	}
	for (const Diagnostic &d : project.scan.diagnostics) {
		print_diagnostic(out, d);
		if (d.severity == DiagnosticSeverity::Error) ++errors;
	}
	for (const Diagnostic &d : project.requirements.diagnostics) {
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
	const CreateMissingResult result =
	        create_missing_requirements(project.paths, project.doc, project.requirements, role);
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

int command_import(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc < 3) return usage(err, "import needs a project directory and a source file");
	std::vector<ImportSource> sources;
	bool replace = false;
	for (int i = 3; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--replace") replace = true;
		else if (arg == "--entry" && i + 1 < argc) sources.push_back({argv[2], argv[++i]});
		else return usage(err, ("unknown or incomplete import option " + arg).c_str());
	}
	if (sources.empty()) {
		if (opennova::strutil::ends_with_icase(argv[2], ".pff"))
			return usage(err, "choose PFF members with --entry <name> (repeat for more files)");
		sources.push_back({argv[2], {}});
	}
	OpenedProject project;
	if (!open_for_report(argv[1], project, err)) return 2;
	const ImportResult result = import_assets(sources, project.paths, project.doc, replace);
	for (const auto &path : result.imported) std::fprintf(out, "imported %s\n", path.c_str());
	for (const auto &d : result.diagnostics) print_diagnostic(err, d);
	return result.diagnostics.empty() ? 0 : 1;
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
	const BuildPlan plan = plan_build(project.paths, project.doc, project.scan, project.requirements);
	if (!plan.ok) {
		for (const Diagnostic &d : plan.diagnostics) print_diagnostic(out, d);
		std::fprintf(out, "not ok: the project cannot be built until these are fixed\n");
		return 1;
	}
	const std::string output_root = out_dir.empty() ? project.paths.build_dir + "/play" : out_dir;
	PrintProgress progress(out);
	const BuildReport report = run_build(plan, project.doc, output_root, {}, &progress);
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
	if (command == "build") return command_build(argc, argv, out, err);
	if (command == "-h" || command == "--help" || command == "help") return usage(err, nullptr);
	return usage(err, ("unknown command " + command).c_str());
}

} // namespace opennova::project_cli
