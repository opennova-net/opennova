#include "asset_catalog_command.h"

#include <editor/session/native_asset_catalog.h>

#include <map>
#include <string>

namespace opennova::project {
namespace {

int usage(std::FILE *err, const std::string &why) {
	std::fprintf(err, "opennova-project asset-catalog: %s\n"
			"usage: opennova-project asset-catalog <export-dir> --game <code> --out <dir>\n"
			"       [--expansion <code>] [--base-export <dir> | --base-project <dir>]\n", why.c_str());
	return 2;
}

} // namespace

int run_asset_catalog_command(int argc, const char *const *argv, std::FILE *out, std::FILE *err) {
	std::map<std::string, std::string> options;
	std::string exported;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (!arg.empty() && arg[0] == '-') {
			if (arg != "--game" && arg != "--out" && arg != "--expansion" &&
					arg != "--base-export" && arg != "--base-project")
				return usage(err, "unknown option " + arg);
			if (options.count(arg)) return usage(err, arg + " is given twice");
			if (i + 1 >= argc || std::string(argv[i + 1]).rfind("--", 0) == 0)
				return usage(err, arg + " needs a value");
			options.emplace(arg, argv[++i]);
		} else if (exported.empty()) exported = arg;
		else return usage(err, "only one export directory is accepted");
	}
	if (exported.empty() || options["--game"].empty() || options["--out"].empty())
		return usage(err, "an export directory, --game and --out are required");
	if (options.count("--base-export") && options.count("--base-project"))
		return usage(err, "choose --base-export or --base-project");
	editor::NativeAssetCatalogOptions input;
	input.project_export = exported;
	input.game = options["--game"];
	input.output = options["--out"];
	input.expansion = options["--expansion"];
	input.base_export = options["--base-export"];
	input.base_project = options["--base-project"];
	editor::NativeAssetCatalogResult result;
	std::string error;
	if (!editor::write_native_asset_catalog(input, result, error)) {
		std::fprintf(err, "asset catalog failed: %s\n", error.c_str());
		return 1;
	}
	for (const std::string &reason : result.omitted)
		std::fprintf(err, "asset catalog omitted %s\n", reason.c_str());
	std::fprintf(out, "asset catalog: %zu assets, %zu omitted\n", result.assets, result.omitted.size());
	return 0;
}

} // namespace opennova::project
