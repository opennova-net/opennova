#include <formats/filelog/file_access_log.h>

#include <algorithm>

#include <base/io/strutil.h>

namespace opennova::filelog {

namespace {

// The two forms of a file log's line [orig: File_LogFileAccess @ 0x75a510 "PFF LOADED FILE: %s\n", the
// other "LOADED FILE: %s\n"].
constexpr const char *kLoadedFromArchive = "PFF LOADED FILE: ";
constexpr const char *kLoadedFromDisk = "LOADED FILE: ";

// `name` added to `names` unless they hold it (compared without case).
void add_name_once(std::vector<std::string> &names, const std::string &name) {
	if (name.empty()) return;
	if (std::none_of(names.begin(), names.end(), [&name](const std::string &held) { return strutil::iequals(held, name); }))
		names.push_back(name);
}

} // namespace

void add_file_access_line(FileAccessLog &log, const std::string &line) {
	++log.lines;
	const size_t archive_prefix = std::char_traits<char>::length(kLoadedFromArchive);
	const size_t disk_prefix = std::char_traits<char>::length(kLoadedFromDisk);
	if (line.compare(0, archive_prefix, kLoadedFromArchive) == 0) {
		add_name_once(log.from_archives, line.substr(archive_prefix));
	} else if (line.compare(0, disk_prefix, kLoadedFromDisk) == 0) {
		const std::string name = line.substr(disk_prefix);
		add_name_once(strutil::ends_with_icase(name, ".pff") ? log.archives : log.from_disk, name);
	}
}

FileAccessLog parse_file_access_log(const std::string &text) {
	FileAccessLog log;
	size_t start = 0;
	while (start < text.size()) {
		size_t end = text.find('\n', start);
		if (end == std::string::npos) end = text.size();
		std::string line = text.substr(start, end - start);
		if (!line.empty() && line.back() == '\r') line.pop_back();
		add_file_access_line(log, line);
		start = end + 1;
	}
	return log;
}

} // namespace opennova::filelog
