// Two synthetic install trees for the updater's tests: a product's baseline (`from`) and the
// tree to update it to (`to`), each a loose version.txt and a .pff at the root plus a jox01
// expansion product, with a loose file added, one dropped, one unchanged, an entry changed, one
// added, one dropped, and the updater's own update.log, never shipped. opennova-update-tool
// builds a package from them (update_tool_test), the apply library installs that package over
// a copy of `from`, which must then equal `to` (update_apply_test), and the hosted data sets'
// jobs test writes its synthetic install with the same helpers (data_set_jobs_test).
#pragma once

#include "common/temp_dir.h"

#include <base/io/file_io.h>
#include <base/io/os_path.h>
#include <formats/pff/pff.h>

#include <filesystem>
#include <string>
#include <string_view>
#include <vector>

namespace synthetic_trees {

inline std::vector<uint8_t> bytes(std::string_view text) {
	return std::vector<uint8_t>(text.begin(), text.end());
}

inline bool write_text(const std::string &path, std::string_view text) {
	std::string error;
	std::filesystem::create_directories(
			std::filesystem::path(opennova::io::os_path(path)).parent_path());
	return opennova::io::write_file_atomic(path, text.data(), text.size(), error);
}

inline bool make_archive(
		const std::string &path, const std::vector<opennova::pff::PffEditEntry> &entries) {
	std::filesystem::create_directories(
			std::filesystem::path(opennova::io::os_path(path)).parent_path());
	bool rewritten = false;
	return opennova::pff::pff_apply_edits(path.c_str(), entries, {}, &rewritten) ==
			opennova::pff::PFF_WRITE_OK;
}

// The entries `from`'s res.pff and `to`'s hold.
inline const std::vector<opennova::pff::PffEditEntry> &from_archive() {
	static const std::vector<opennova::pff::PffEditEntry> entries = {
		{ "keep.def", bytes("kept"), 1100000000 },
		{ "wpn.def", bytes("rifle 1"), 1100000000 },
		{ "drop.def", bytes("x"), 1100000000 },
	};
	return entries;
}
inline const std::vector<opennova::pff::PffEditEntry> &to_archive() {
	static const std::vector<opennova::pff::PffEditEntry> entries = {
		{ "keep.def", bytes("kept"), 1100000000 },
		{ "wpn.def", bytes("rifle 2"), 1700000000 },
		{ "add.def", bytes("new"), 1700000001 },
	};
	return entries;
}

// `from` and `to` under `dir`.
inline bool make_trees(const test_temp::TempDir &dir) {
	const std::string from = dir.file("from"), to = dir.file("to");
	return write_text(from + "/version.txt", "Synthetic Product, V1.0.0.0; English") &&
			write_text(from + "/old.txt", "gone in the update") &&
			write_text(from + "/same.txt", "unchanged") &&
			write_text(from + "/update.log", "the updater's own file") &&
			make_archive(from + "/res.pff", from_archive()) &&
			write_text(from + "/expansion/jox01/version.txt",
					"Synthetic Expansion, V1.0.0.0; English") &&
			write_text(to + "/version.txt", "Synthetic Product, V1.0.0.10; English") &&
			write_text(to + "/same.txt", "unchanged") && write_text(to + "/new.txt", "added") &&
			write_text(to + "/update.log", "a different log, never shipped") &&
			make_archive(to + "/res.pff", to_archive()) &&
			write_text(
					to + "/expansion/jox01/version.txt", "Synthetic Expansion, V1.0.0.11; English");
}

} // namespace synthetic_trees
