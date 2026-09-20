#include "commands.h"

#include <editor/assets/asset_registry.h>
#include <editor/assets/asset_type_registry.h>
#include <editor/blank/create_missing.h>
#include <editor/model/diagnostic.h>
#include <editor/project/local_settings.h>
#include <editor/project/project_document.h>
#include <editor/requirements/requirements.h>

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
	             "  new             create an empty project (project.opennova + .opennova/) in <dir>\n"
	             "  status          the project's title, game, asset count and requirements summary\n"
	             "  validate        list every finding; exit 1 when a required file is missing or wrong\n"
	             "  create-missing  create every missing required file from scratch (or one, by role)\n");
	return 2;
}

void print_diagnostic(std::FILE *out, const Diagnostic &d) {
	std::fprintf(out, "%s %s: %s", diagnostic_severity_label(d.severity), d.code.c_str(),
	             d.message.c_str());
	if (!d.asset.empty()) std::fprintf(out, " [%s]", d.asset.c_str());
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

} // namespace

int run_project_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	if (argc < 1) return usage(err, nullptr);
	const std::string command = argv[0];
	if (command == "new") return command_new(argc, argv, out, err);
	if (command == "status") return command_status(argc, argv, out, err);
	if (command == "validate") return command_validate(argc, argv, out, err);
	if (command == "create-missing") return command_create_missing(argc, argv, out, err);
	if (command == "-h" || command == "--help" || command == "help") return usage(err, nullptr);
	return usage(err, ("unknown command " + command).c_str());
}

} // namespace opennova::project_cli
