#include "package_builder.h"

#include <base/io/file_io.h>
#include <base/io/file_time.h>
#include <base/io/os_path.h>
#include <base/io/strutil.h>
#include <formats/pff/pff.h>
#include <formats/update/command_file.h>
#include <formats/update/nlz.h>
#include <formats/update/update_crc.h>

#include <algorithm>
#include <map>
#include <set>
#include <system_error>

namespace opennova::update_tool {

namespace fs = std::filesystem;
using update::CommandFile;
using update::CommandRow;
using update::CommandType;

namespace {

// The files the updater itself keeps beside a product, never shipped as rows.
// [orig: Cupdate2Dlg_StateTick @ 0x408170 (UPDATE.EXE): update\, update.log, update.inf,
//  update.new, SERVER.LST; Cupdate2Dlg_ReadWizard @ 0x404c90: cc.bin]
const char *const kUpdaterFiles[] = { "update.log", "update.inf", "update.new", "server.lst",
	"cc.bin" };

std::string lower(std::string_view text) {
	std::string out(text);
	for (char &c : out)
		if (c >= 'A' && c <= 'Z')
			c = char(c - 'A' + 'a');
	return out;
}

bool is_pff(const std::string &name) {
	return strutil::ends_with_icase(name, ".pff");
}

// A file's bytes, read whole.
bool read_bytes(const fs::path &path, std::vector<uint8_t> &out, std::string &error) {
	return io::read_file_bytes(io::utf8_path(path), out, error);
}

// The entries of an archive by normalized name, decrypted, with their times. Empty when the
// archive is absent.
struct ArchiveEntry {
	std::string name; // as stored
	std::vector<uint8_t> content;
	uint32_t timestamp = 0;
};

bool read_archive(
		const fs::path &path, std::map<std::string, ArchiveEntry> &out, std::string &error) {
	out.clear();
	std::error_code ec;
	if (!fs::exists(path, ec))
		return true;
	pff::PffArchive archive{};
	const std::string utf8 = io::utf8_path(path);
	if (pff::pff_open(&archive, utf8.c_str()) != 0) {
		error = "cannot open the archive " + utf8;
		return false;
	}
	for (uint32_t i = 0; i < archive.entry_count; ++i) {
		const pff::PffEntry &entry = archive.entries[i];
		ArchiveEntry item;
		item.name = pff::pff_entry_stored_name(entry);
		item.timestamp = entry.timestamp;
		item.content.resize(entry.size ? entry.size : 1);
		if (pff::pff_extract(&archive, &entry, item.content.data(), item.content.size()) != 0) {
			pff::pff_close(&archive);
			error = "cannot read " + item.name + " from " + utf8;
			return false;
		}
		item.content.resize(entry.size);
		out[pff::normalized_logical_name(item.name)] = std::move(item);
	}
	pff::pff_close(&archive);
	return true;
}

// The product root's plain files (not archives), by lower-cased name, and its subdirectories'
// files as product-relative paths in `nested`.
void list_tree(const fs::path &root, const std::vector<std::string> &excludes, bool skip_expansions,
		std::map<std::string, fs::path> &files, std::map<std::string, fs::path> &archives,
		std::vector<std::string> &nested) {
	std::error_code ec;
	if (!fs::is_directory(root, ec))
		return;
	for (fs::recursive_directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
		const std::string rel = io::utf8_generic_path(fs::relative(it->path(), root, ec));
		const std::string rel_lower = lower(rel);
		bool excluded = rel_lower == "update" || (skip_expansions && rel_lower == "expansion");
		for (const std::string &ex : excludes) {
			const std::string ex_lower = lower(ex);
			if (rel_lower == ex_lower ||
					(!ex_lower.empty() && ex_lower.back() == '/' &&
							(rel_lower + "/").rfind(ex_lower, 0) == 0))
				excluded = true;
		}
		if (excluded) {
			if (it->is_directory(ec))
				it.disable_recursion_pending();
			continue;
		}
		if (!it->is_regular_file(ec))
			continue;
		if (rel.find('/') != std::string::npos) {
			nested.push_back(rel);
			continue;
		}
		if (std::find(std::begin(kUpdaterFiles), std::end(kUpdaterFiles), rel_lower) !=
				std::end(kUpdaterFiles))
			continue;
		(is_pff(rel) ? archives : files)[rel_lower] = it->path();
	}
}

bool same_bytes(const fs::path &a, const fs::path &b, bool &same, std::string &error) {
	std::vector<uint8_t> left, right;
	if (!read_bytes(a, left, error) || !read_bytes(b, right, error))
		return false;
	same = left == right;
	return true;
}

// The blob names of a product share one flat update directory and a 16-byte 1SFS field.
bool claim_name(std::set<std::string> &names, const std::string &name, std::string &error) {
	if (name.empty() || name.size() >= update::kSfsNameBytes) {
		error = "the name \"" + name + "\" does not fit a packed update's 15 characters";
		return false;
	}
	if (!names.insert(lower(name)).second) {
		error = "two files of one product are named \"" + name +
				"\" (every blob lands in one update directory)";
		return false;
	}
	return true;
}

bool build_product(const BuildSpec &spec, const ProductSpec &product, update::SfsSection &section,
		ProductReport &report, std::string &error) {
	const fs::path from_root = spec.from / io::os_path(product.subdir);
	const fs::path to_root = spec.to / io::os_path(product.subdir);
	std::error_code ec;
	if (!fs::is_directory(to_root, ec)) {
		error = "no product tree at " + io::utf8_path(to_root);
		return false;
	}
	std::vector<std::string> excludes = spec.excludes;
	excludes.push_back(product.command_file);
	const bool base = product.expansion.empty();
	std::map<std::string, fs::path> from_files, from_archives, to_files, to_archives;
	std::vector<std::string> from_nested, to_nested;
	list_tree(from_root, excludes, base, from_files, from_archives, from_nested);
	list_tree(to_root, excludes, base, to_files, to_archives, to_nested);
	for (const std::string &rel : to_nested) {
		bool changed = true;
		const fs::path before = from_root / io::os_path(rel);
		if (fs::exists(before, ec) &&
				!same_bytes(before, to_root / io::os_path(rel), changed, error))
			return false;
		if (changed) {
			error = rel +
					" differs, but UPDATE.EXE installs loose files at its product's root only";
			return false;
		}
	}

	CommandFile cmd;
	cmd.exe = product.exe;
	cmd.product = product.product;
	cmd.version = product.version;
	std::set<std::string> names;
	std::vector<update::SfsEntry> blobs;

	auto add_blob = [&](const std::string &name, const std::vector<uint8_t> &content) {
		blobs.push_back({ name, update::nlz_encode(content.data(), content.size()) });
		report.blob_bytes += blobs.back().data.size();
	};

	// Loose files at the root, then the removes there (the target is "." until a TARGET row).
	for (const auto &[key, path] : to_files) {
		std::vector<uint8_t> content;
		if (!read_bytes(path, content, error))
			return false;
		const auto before = from_files.find(key);
		if (before != from_files.end()) {
			std::vector<uint8_t> old;
			if (!read_bytes(before->second, old, error))
				return false;
			if (old == content)
				continue;
			// The precheck sees a loose file only through its size and the defective file CRC,
			// which reads the first size/4 + size%4 bytes: a change past them at the same size
			// never installs (witnessed 2026-10-15: a version.txt bump of its last digit).
			// [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE), the loose case]
			if (old.size() == content.size() &&
					update::file_crc(old.data(), old.size()) ==
							update::file_crc(content.data(), content.size())) {
				error = io::utf8_path(path.filename()) +
						" changed where UPDATE.EXE cannot see it: same size and the same file CRC "
						"(the retail CRC reads only its first " +
						std::to_string(content.size() / 4 + content.size() % 4) +
						" bytes); change its size or its start";
				return false;
			}
		}
		// The precheck fetches a loose row whenever the installed file is empty, so an empty
		// file shipped as a row is fetched again on every pass and the client loops UPDATE ->
		// REVUPDAT -> UPDATE without end: refused, like the change it cannot see.
		// [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE), the loose case
		//  @0x407970..0x4079B9 (UPDATE.EXE): missing or size 0 fetches]
		if (content.empty()) {
			error = io::utf8_path(path.filename()) +
					" is empty: UPDATE.EXE fetches an empty loose file again on every pass, so "
					"the update would never finish; give it content or leave it out";
			return false;
		}
		const std::string name = io::utf8_path(path.filename());
		if (!claim_name(names, name, error))
			return false;
		const int64_t mtime = io::file_modified_unix_seconds(path);
		cmd.rows.push_back(
				{ "UPD", CommandType::Loose, name, int32_t(mtime), int32_t(content.size()),
						update::crc_as_signed(update::file_crc(content.data(), content.size())) });
		add_blob(name, content);
		++report.loose_rows;
	}
	if (cmd.product.empty() || cmd.version.empty()) {
		std::vector<uint8_t> text;
		const fs::path version = to_root / "version.txt";
		std::string ignored;
		if (fs::exists(version, ec) && read_bytes(version, text, ignored)) {
			std::string product_text, version_text;
			if (split_version_text(
						std::string(text.begin(), text.end()), product_text, version_text)) {
				if (cmd.product.empty())
					cmd.product = product_text;
				if (cmd.version.empty())
					cmd.version = version_text;
			}
		}
	}
	if (spec.removes) {
		for (const auto &[key, path] : from_files) {
			if (to_files.count(key))
				continue;
			cmd.rows.push_back({ "REMOVE", CommandType::Remove, io::utf8_path(path.filename()), 0,
					0, std::nullopt });
			++report.remove_rows;
		}
		// An archive `to` lacks goes too: before any TARGET row the target is ".", where a
		// REMOVE deletes the named file, the archive's file included.
		// [orig: Cupdate2Dlg_PrecheckCommands @ 0x407820 (UPDATE.EXE), the REMOVE case
		//  (docs/update/updater-re.md §7)]
		for (const auto &[key, path] : from_archives) {
			if (to_archives.count(key))
				continue;
			cmd.rows.push_back({ "REMOVE", CommandType::Remove, io::utf8_path(path.filename()), 0,
					0, std::nullopt });
			++report.remove_rows;
		}
	}

	// Each archive: a TARGET, its changed entries as COPY rows, its dropped ones as REMOVE rows.
	for (const auto &[key, path] : to_archives) {
		std::map<std::string, ArchiveEntry> before, after;
		const auto from_path = from_archives.find(key);
		if (from_path != from_archives.end() && !read_archive(from_path->second, before, error))
			return false;
		if (!read_archive(path, after, error))
			return false;
		std::vector<CommandRow> rows;
		for (const auto &[entry_key, entry] : after) {
			const auto old = before.find(entry_key);
			if (old != before.end() && old->second.content == entry.content)
				continue;
			if (!claim_name(names, entry.name, error))
				return false;
			const uint32_t time = entry.timestamp ? entry.timestamp : pff::PFF_NEW_ENTRY_TIMESTAMP;
			rows.push_back({ "COPY", CommandType::Copy, entry.name, int32_t(time),
					int32_t(entry.content.size()),
					update::crc_as_signed(
							update::file_crc(entry.content.data(), entry.content.size())) });
			add_blob(entry.name, entry.content);
			++report.copy_rows;
		}
		if (spec.removes) {
			for (const auto &[entry_key, entry] : before) {
				if (after.count(entry_key))
					continue;
				rows.push_back({ "REMOVE", CommandType::Remove, entry.name, 0, 0, std::nullopt });
				++report.remove_rows;
			}
		}
		if (rows.empty())
			continue;
		cmd.rows.push_back({ "TARGET", CommandType::Target, io::utf8_path(path.filename()), 0, 0,
				std::nullopt });
		cmd.rows.insert(cmd.rows.end(), rows.begin(), rows.end());
	}

	const std::string text = update::write_command_file(cmd);
	if (text.empty()) {
		error = "the command file " + product.command_file +
				" cannot be written (a name or value "
				"the updater cannot read back)";
		return false;
	}
	if (!claim_name(names, product.command_file, error))
		return false;
	section.expansion = product.expansion;
	section.entries.push_back(
			{ product.command_file, std::vector<uint8_t>(text.begin(), text.end()) });
	section.entries.insert(section.entries.end(), blobs.begin(), blobs.end());
	report.command_file = product.command_file;
	return true;
}

} // namespace

bool split_version_text(std::string_view text, std::string &product, std::string &version) {
	const size_t marker = text.find(", V");
	if (marker == std::string_view::npos)
		return false;
	product = std::string(text.substr(0, marker));
	std::string_view rest = text.substr(marker + 3);
	const size_t end = rest.find_first_of(";\r\n");
	version = std::string(rest.substr(0, end));
	return !product.empty() && !version.empty();
}

bool build_update_package(const BuildSpec &spec, update::SfsPackage &out,
		std::vector<ProductReport> &report, std::string &error) {
	if (spec.products.empty()) {
		error = "no product to build";
		return false;
	}
	update::SfsPackage package;
	package.product = spec.package_product;
	report.clear();
	for (size_t i = 0; i < spec.products.size(); ++i) {
		const ProductSpec &product = spec.products[i];
		// The base product, when the package carries it, is the section before any PACKAGEBREAK.
		// A package without it serves online; offline, the stock updater needs the base's
		// command file first.
		if (product.expansion.empty() && i != 0) {
			error = "the base product must be the package's first";
			return false;
		}
		update::SfsSection section;
		ProductReport line;
		if (!build_product(spec, product, section, line, error))
			return false;
		package.sections.push_back(std::move(section));
		report.push_back(line);
	}
	if (update::sfs_write(package).empty()) {
		error = "the package cannot be written (a name does not fit its field)";
		return false;
	}
	out = std::move(package);
	return true;
}

} // namespace opennova::update_tool
