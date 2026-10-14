// hudfx.def read as the HUD's init reads it and written from its lines (def_hudfx.h; docs/interface/hud-re.md,
// "hudfx.def").
#include "def_hudfx.h"

#include <base/io/ascii_config.h>
#include <base/io/strutil.h>

namespace opennova::def {
namespace {

// The tags in their slots' order [orig: HUD_CacheModelNameByTag @ 0x58F970: "3DIHud" @ 0x58F986, "3DIPower1"
// .. "3DIPower8" @ 0x58F9BE..0x58FB22].
constexpr const char *kTags[kHudFxSlots] = {"3DIHud",    "3DIPower1", "3DIPower2", "3DIPower3", "3DIPower4",
                                            "3DIPower5", "3DIPower6", "3DIPower7", "3DIPower8"};

// A model's name as one token of the walk (or none: the line's tag alone).
bool one_token(const std::string &name) {
	return name.find_first_of(" \t,\";\r\n") == std::string::npos && name.find("//") == std::string::npos;
}

} // namespace

const char *hudfx_tag(size_t slot) { return slot < kHudFxSlots ? kTags[slot] : nullptr; }

int hudfx_slot_of(std::string_view tag) {
	for (size_t slot = 0; slot < kHudFxSlots; ++slot)
		if (strutil::iequals(tag, kTags[slot])) return int(slot);
	return -1;
}

HudFxFile hudfx_parse(const uint8_t *data, size_t size) {
	HudFxFile file;
	if (!data) return file;
	const char *text = reinterpret_cast<const char *>(data);
	io::ConfigTokens tokens;
	io::for_each_config_line_span(text, size, tokens, [&](io::ConfigTokens &line, const io::ConfigLineSpan &) {
		// The walk ended at the first tagged line [orig: File_ParseASCIIFile @ 0x53D942].
		if (file.line) return;
		// The walk's gate [orig: File_ParseASCIIFile @0x53D915 / @0x53D91E].
		if (line.count == 0 || line.tokens[0][0] == '/') return;
		const int slot = hudfx_slot_of(line.tokens[0]);
		if (slot < 0) return;
		HudFxLine row;
		row.slot = uint8_t(slot);
		row.model = line.token(1); // params[2], the reset token's "" past the count
		file.line = std::move(row);
	});
	return file;
}

std::array<std::string, kHudFxSlots> hudfx_slot_names(const HudFxFile &file) {
	std::array<std::string, kHudFxSlots> names;
	if (!file.line) return names;
	// The line's model copied with its terminator from its slot on [orig: @ 0x58F992..0x58FB42].
	std::string bytes(kHudFxSlots * kHudFxNameBytes, '\0');
	const HudFxLine &read = *file.line;
	const size_t at = size_t(read.slot) * kHudFxNameBytes;
	for (size_t i = 0; i < read.model.size() && at + i < bytes.size(); ++i) bytes[at + i] = read.model[i];
	for (size_t slot = 0; slot < kHudFxSlots; ++slot) {
		const char *start = bytes.data() + slot * kHudFxNameBytes;
		size_t length = 0;
		while (slot * kHudFxNameBytes + length < bytes.size() && start[length] != '\0') ++length;
		names[slot].assign(start, length);
	}
	return names;
}

bool hudfx_write(const HudFxFile &file, std::string &text, std::string &error) {
	text.clear();
	if (!file.line) return true;
	const HudFxLine &line = *file.line;
	if (line.slot >= kHudFxSlots) {
		error = "The line's slot " + std::to_string(line.slot) + " is no tag of the nine.";
		return false;
	}
	if (!one_token(line.model)) {
		error = std::string(kTags[line.slot]) + "'s model \"" + line.model +
		        "\" is no one word the walk reads: a name with no blank, comma, quote, `;` or `//`.";
		return false;
	}
	// A line of no model is its tag alone: the slot's name the reset token's "", nothing loaded. CR LF after it,
	// the one break the walk splits at [orig: File_ParseASCIIFile @ 0x53D8C7].
	text = std::string(kTags[line.slot]) + (line.model.empty() ? "" : "\t" + line.model) + "\r\n";
	return true;
}

} // namespace opennova::def
