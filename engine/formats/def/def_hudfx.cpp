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

HudFxFile parse(const uint8_t *data, size_t size, textlayout::Notes *notes) {
	HudFxFile file;
	const char *text = reinterpret_cast<const char *>(data);
	textlayout::Noter noter(text, data ? size : 0, notes, textlayout::cut_ascii_walk);
	file.note = noter.root();
	if (!data) return file;
	io::ConfigTokens tokens;
	io::for_each_config_line_span(text, size, tokens, [&](io::ConfigTokens &line, const io::ConfigLineSpan &span) {
		noter.line(span.begin, span.end + 2);
		// The walk's gate [orig: File_ParseASCIIFile @0x53D915 / @0x53D91E].
		if (line.count == 0 || line.tokens[0][0] == '/') return;
		const int slot = hudfx_slot_of(line.tokens[0]);
		if (slot < 0) return;
		HudFxLine row;
		row.slot = uint8_t(slot);
		row.model = line.token(1); // params[2], the reset token's "" past the count
		row.note = noter.open(noter.root());
		noter.entry(row.note, "tag");
		noter.close(row.note);
		file.lines.push_back(std::move(row));
	});
	noter.finish();
	return file;
}

// A model's name as one token of the walk.
bool one_token(const std::string &name) {
	return !name.empty() && name.find_first_of(" \t,\";\r\n") == std::string::npos && name.find("//") == std::string::npos;
}

textlayout::OutRecord records_of(const HudFxFile &file) {
	textlayout::OutRecord root;
	root.note = file.note;
	for (const HudFxLine &line : file.lines) {
		textlayout::OutRecord record;
		record.note = line.note;
		record.kind = "line";
		const char *tag = hudfx_tag(line.slot);
		record.lines.push_back({"tag", std::string(tag ? tag : "") + "\t" + line.model});
		root.lines.push_back({"", "", int(root.children.size())});
		root.children.push_back(std::move(record));
	}
	return root;
}

bool same_lines(const HudFxFile &a, const HudFxFile &b) {
	if (a.lines.size() != b.lines.size()) return false;
	for (size_t i = 0; i < a.lines.size(); ++i)
		if (a.lines[i].slot != b.lines[i].slot || a.lines[i].model != b.lines[i].model) return false;
	return true;
}

} // namespace

const char *hudfx_tag(size_t slot) { return slot < kHudFxSlots ? kTags[slot] : nullptr; }

int hudfx_slot_of(std::string_view tag) {
	for (size_t slot = 0; slot < kHudFxSlots; ++slot)
		if (strutil::iequals(tag, kTags[slot])) return int(slot);
	return -1;
}

HudFxFile hudfx_parse(const uint8_t *data, size_t size) { return parse(data, size, nullptr); }

HudFxFile hudfx_parse(const uint8_t *data, size_t size, textlayout::Notes &notes) {
	HudFxFile file = parse(data, size, &notes);
	textlayout::model(notes, records_of(file), textlayout::cut_ascii_walk);
	return file;
}

std::array<std::string, kHudFxSlots> hudfx_slot_names(const HudFxFile &file) {
	std::array<std::string, kHudFxSlots> names;
	if (file.lines.empty()) return names;
	// The first tagged line's model copied with its terminator from its slot on [orig: @ 0x58F992..0x58FB42].
	std::string bytes(kHudFxSlots * kHudFxNameBytes, '\0');
	const HudFxLine &read = file.lines.front();
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

bool hudfx_write(const HudFxFile &file, const textlayout::Notes *notes, std::string &text, std::string &error,
                 bool *rewritten) {
	if (rewritten) *rewritten = false;
	for (const HudFxLine &line : file.lines) {
		if (line.slot >= kHudFxSlots) {
			error = "A line's slot " + std::to_string(line.slot) + " is no tag of the nine.";
			return false;
		}
		if (!one_token(line.model)) {
			error = std::string(kTags[line.slot]) + "'s model \"" + line.model +
			        "\" is no one word the walk reads: a name with no blank, comma, quote, `;` or `//`.";
			return false;
		}
	}
	const textlayout::OutRecord root = records_of(file);
	// CR LF after each line, the one break the walk splits at [orig: File_ParseASCIIFile @ 0x53D8C7].
	const std::string eol = notes ? textlayout::file_eol(*notes, "\r\n") : std::string("\r\n");
	text = textlayout::compose(notes, root, textlayout::cut_ascii_walk, eol);
	if (notes) {
		const HudFxFile again = hudfx_parse(reinterpret_cast<const uint8_t *>(text.data()), text.size());
		if (!same_lines(again, file)) {
			text = textlayout::compose(nullptr, root, textlayout::cut_ascii_walk, "\r\n");
			if (rewritten) *rewritten = true;
		}
	}
	return true;
}

} // namespace opennova::def
