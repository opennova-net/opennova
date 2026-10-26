#include "data_set_files.h"

#include <base/io/strutil.h>
#include <base/resource_index/file_kind.h>
#include <base/vfs/vfs.h>

namespace opennova::novaworld_server::hosted {

namespace {

// The boot table's archive of `slot` (base/vfs kBootArchiveTable).
std::string boot_archive(ArchiveSlot slot) {
	return archive_slot_file_name(slot);
}

} // namespace

std::vector<TreeFileRule> tree_file_rules(DataSetKind kind, std::string_view expansion_code) {
	if (kind == DataSetKind::Update)
		return {};
	if (kind == DataSetKind::Base) {
		return { { boot_archive(ArchiveSlot::Resource), true },
			{ boot_archive(ArchiveSlot::Localres), true },
			{ boot_archive(ArchiveSlot::Language), true }, { "version.txt", false },
			{ "score.ini", false }, { "_NSTMOUT.TXT", false }, { "gt.ssc", false } };
	}
	const std::string code(expansion_code);
	return { { vfs_expansion_archive_name(code, false), true },
		{ vfs_expansion_archive_name(code, true), true }, { "version.txt", true },
		{ "gt.ssc", false } };
}

std::string canonical_tree_file(DataSetKind kind, std::string_view expansion_code, std::string_view name) {
	for (const TreeFileRule &rule : tree_file_rules(kind, expansion_code))
		if (strutil::iequals(rule.name, name))
			return rule.name;
	return std::string();
}

std::vector<std::string> missing_tree_files(DataSetKind kind, std::string_view expansion_code,
		const std::vector<std::string> &names) {
	std::vector<std::string> missing;
	for (const TreeFileRule &rule : tree_file_rules(kind, expansion_code)) {
		if (!rule.required)
			continue;
		bool found = false;
		for (const std::string &name : names)
			if (strutil::iequals(name, rule.name))
				found = true;
		if (!found)
			missing.push_back(rule.name);
	}
	return missing;
}

MissionPair mission_pair_of(DataSetKind kind, std::string_view expansion_code) {
	if (kind == DataSetKind::Base)
		return { boot_archive(ArchiveSlot::Localres), boot_archive(ArchiveSlot::Language) };
	const std::string code(expansion_code);
	return { vfs_expansion_archive_name(code, false), vfs_expansion_archive_name(code, true) };
}

} // namespace opennova::novaworld_server::hosted
