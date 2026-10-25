#pragma once

// The packed update a release ships (ADR 0061 d4): a 1SFS single-file update built from two
// install trees, the product's baseline (`from`, the install as it shipped) and the tree to
// update it to (`to`). Every file and PFF entry of `to` that differs from `from` becomes a row
// of the product's command file and a 1ZLN blob beside it, every one `to` lacks a REMOVE row;
// Native releases use full current rows and the previous package's removal history so
// reverted files and skipped versions converge; baseline diff mode remains available for
// retail tooling (docs/update/updater-re.md §6-§9).
//
// What UPDATE.EXE can install bounds what the builder takes: a loose file only at its product's
// root (the precheck stats a loose row's bare name, never under a TARGET), names of at most 15
// characters (a 1SFS entry's field) and unique across a product (every blob lands in one flat
// update directory), PFF archives at the product's root, and a loose change the precheck can see:
// retail diff mode refuses a same-size change whose defective file CRC is unchanged. Native
// full mode fetches every row of a changed command. Empty files are refused in either mode
// because retail's precheck repeatedly fetches them. An archive `to` lacks is a REMOVE row of
// its own at the root.

#include <formats/update/sfs.h>

#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace opennova::update_tool {

struct ProductSpec {
	std::string subdir; // the product's root under each tree: "" or "expansion/jox01"
	std::string expansion; // its PACKAGEBREAK name: "" for the base product, else "jox01"
	std::string command_file; // JOPTR.CMD
	std::string product; // the command file's PRODUCT, as the release names the product
	std::string version; // VERSION, the release's (no version file is read: ADR 0067)
	std::string exe; // EXE, launched at DONE when /RUN; empty for none
};

struct BuildSpec {
	std::filesystem::path from;
	std::filesystem::path to;
	std::string package_product; // the 1SFS trailer's product (the install's registry name)
	std::vector<ProductSpec> products;
	std::vector<std::string> excludes; // product-relative paths or directories ("cache/") left out
	bool removes = true; // write REMOVE rows for what `to` lacks
	// Native releases publish the whole current tree. The preceding package carries
	// earlier removals, and names introduced after the original baseline, across releases.
	bool full = false;
	std::optional<update::SfsPackage> previous;
};

struct ProductReport {
	std::string command_file;
	size_t loose_rows = 0;
	size_t copy_rows = 0;
	size_t remove_rows = 0;
	size_t blob_bytes = 0;
};

// The package, or false with `error`. `report` gets a line per product.
bool build_update_package(const BuildSpec &spec, update::SfsPackage &out,
		std::vector<ProductReport> &report, std::string &error);

} // namespace opennova::update_tool
