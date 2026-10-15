#pragma once

// The packed update a release ships (ADR 0061 d4): a 1SFS single-file update built from two
// install trees, the product's baseline (`from`, the install as it shipped) and the tree to
// update it to (`to`). Every file and PFF entry of `to` that differs from `from` becomes a row
// of the product's command file and a 1ZLN blob beside it, every one `to` lacks a REMOVE row;
// diffing from the baseline each time keeps a package cumulative, so a client at any older
// version converges (docs/update/updater-re.md §6-§9).
//
// What UPDATE.EXE can install bounds what the builder takes: a loose file only at its product's
// root (the precheck stats a loose row's bare name, never under a TARGET), names of at most 15
// characters (a 1SFS entry's field) and unique across a product (every blob lands in one flat
// update directory), PFF archives at the product's root, and a loose change the precheck can see:
// a file of the same size whose defective file CRC is unchanged never installs.

#include <formats/update/sfs.h>

#include <filesystem>
#include <string>
#include <vector>

namespace opennova::update_tool {

struct ProductSpec {
	std::string subdir; // the product's root under each tree: "" or "expansion/jox01"
	std::string expansion; // its PACKAGEBREAK name: "" for the base product, else "jox01"
	std::string command_file; // JOPTR.CMD
	std::string product; // the command file's PRODUCT; empty: from version.txt
	std::string version; // VERSION; empty: from version.txt
	std::string exe; // EXE, launched at DONE when /RUN; empty for none
};

struct BuildSpec {
	std::filesystem::path from;
	std::filesystem::path to;
	std::string package_product; // the 1SFS trailer's product (the install's registry name)
	std::vector<ProductSpec> products;
	std::vector<std::string> excludes; // product-relative paths or directories ("cache/") left out
	bool removes = true; // write REMOVE rows for what `to` lacks
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

// "Joint Operations: Escalation, V1.3.0.0; English" -> product "Joint Operations: Escalation",
// version "1.3.0.0". False when the text has no ", V".
bool split_version_text(std::string_view text, std::string &product, std::string &version);

} // namespace opennova::update_tool
