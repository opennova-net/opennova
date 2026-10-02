#include "menu_style.h"

#include <base/io/strutil.h>

namespace opennova::menu {

// [orig: Menu_InitShellResources @ 0x552500: @ 0x5525f7..0x552609 and @ 0x552614..0x55261b]
const ShellStylesheet kShellStylesheets[2] = {
	{"menu_style.mns", false},
	{"brand.mns", true},
};

bool is_shell_stylesheet(const std::string &name) {
	for (const ShellStylesheet &sheet : kShellStylesheets)
		if (strutil::iequals(name, sheet.name)) return true;
	return false;
}

bool ShellStyle::any_present() const {
	for (const ShellSheetRead &sheet : sheets)
		if (sheet.present) return true;
	return false;
}

ShellSheetRead load_include_file(const std::string &name, const ShellStyleReader &read, bool append,
                                 mns::KeyValueList &list) {
	ShellSheetRead out;
	out.name = name;
	std::string bytes;
	// No file, or no bytes: nothing changes, the list is not cleared [orig: @ 0x63b982..0x63b989,
	// FileSystem_GetFileSize <= 0 returns 0; @ 0x63b9a6 a failed load returns 0].
	if (!read(name, bytes) || bytes.empty()) return out;
	out.present = true;
	// [orig: @ 0x63b9d7..0x63ba2b] append 0 frees every node first
	if (!append) list.nodes.clear();
	out.read = mns::parse_key_value_buffer(bytes.data(), bytes.size(), list); // [orig: @ 0x63ba3b]
	if (out.read.status != mns::ReadStatus::Read)
		out.stopped_line = mns::line_at_offset(bytes.data(), bytes.size(), out.read.offset);
	return out;
}

ShellStyle load_shell_style(const ShellStyleReader &read) {
	ShellStyle style;
	// Neither result is tested [orig: @ 0x552609, @ 0x55261b].
	for (const ShellStylesheet &sheet : kShellStylesheets)
		style.sheets.push_back(load_include_file(sheet.name, read, sheet.append, style.list));
	return style;
}

} // namespace opennova::menu
