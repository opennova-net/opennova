// A loose directory's files by name without case (dir_case_index.h).
#include <base/vfs/dir_case_index.h>

#include <base/io/os_path.h>
#include <base/io/strutil.h>

#include <filesystem>
#include <system_error>

namespace opennova {

// The folder listed once: each regular file under its ASCII-lowered name, as retail's open of
// "<dir>\<name>" finds it in any case.
// [orig: FileSystem_FileExists @0x75AA50 - sprintf("%s\\%s") @0x75AAD4, _lopen @0x75AAE3]
DirCaseIndex DirCaseIndex::scan(const std::string &dir) {
	DirCaseIndex index;
	if (dir.empty()) return index;
	namespace fs = std::filesystem;
	std::error_code ec;
	const fs::directory_iterator listing(
			io::os_path(dir), fs::directory_options::skip_permission_denied, ec);
	for (const fs::directory_entry &entry : listing) {
		if (ec) break;
		std::error_code kind;
		if (!entry.is_regular_file(kind)) continue;
		const std::string name = io::utf8_path(entry.path().filename());
		const std::string key = strutil::to_lower(name);
		if (key.empty()) continue;
		const auto inserted = index.names_.emplace(key, name);
		// A second spelling of a name already listed: neither is the name's.
		if (!inserted.second) inserted.first->second.clear();
	}
	return index;
}

std::string DirCaseIndex::find(const std::string &name) const {
	const auto it = names_.find(strutil::to_lower(name));
	return it != names_.end() ? it->second : std::string();
}

bool DirCaseIndex::ambiguous(const std::string &name) const {
	const auto it = names_.find(strutil::to_lower(name));
	return it != names_.end() && it->second.empty();
}

} // namespace opennova
