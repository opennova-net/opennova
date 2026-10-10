#include <editor/documents/particle_keys.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <formats/particle/parser.h>

namespace opennova::editor {

namespace {

using particle::BlockKind;
using particle::KeyRow;
using particle::KeyValueKind;

const char *block_tag(BlockKind kind) {
	switch (kind) {
	case BlockKind::Effect: return "[effectdef]";
	case BlockKind::Table: return "[tabledef]";
	case BlockKind::Handles: return "[tabledef_edithandles]";
	default: return "[particledef]";
	}
}

std::vector<std::string> split_values(const std::string &value, char by) {
	std::vector<std::string> out;
	std::string part;
	std::istringstream stream(value);
	while (std::getline(stream, part, by)) out.push_back(strutil::trim(part));
	return out;
}

std::vector<std::string> words_of(const std::string &value) {
	std::vector<std::string> out;
	std::istringstream stream(value);
	std::string word;
	while (stream >> word) out.push_back(word);
	return out;
}

// A whole number atol reads whole: a sign and digits, nothing after (a word atol reads only the start of, `12x`, is
// none).
bool whole(const std::string &text, int &out) {
	size_t i = text.empty() || (text[0] != '+' && text[0] != '-') ? 0 : 1;
	if (i == text.size()) return false;
	for (size_t j = i; j < text.size(); ++j)
		if (text[j] < '0' || text[j] > '9') return false;
	const std::optional<int> parsed = strutil::parse_int(text);
	if (!parsed) return false;
	out = *parsed;
	return true;
}

// A number atof reads whole: a sign, digits with a point among or after them, an exponent, and nothing after (a
// word atof reads only the start of, `1.5x`, is none).
bool real(const std::string &text) {
	const auto digit = [&text](size_t i) { return i < text.size() && text[i] >= '0' && text[i] <= '9'; };
	size_t i = 0, digits = 0;
	if (i < text.size() && (text[i] == '+' || text[i] == '-')) ++i;
	for (; digit(i); ++i) ++digits;
	if (i < text.size() && text[i] == '.')
		for (++i; digit(i); ++i) ++digits;
	if (digits && i < text.size() && (text[i] == 'e' || text[i] == 'E')) {
		size_t j = i + 1;
		if (j < text.size() && (text[j] == '+' || text[j] == '-')) ++j;
		const size_t first = j;
		while (digit(j)) ++j;
		if (j > first) i = j;
	}
	return digits && i == text.size();
}

// Why a number written otherwise than as one reads as what it reads: the reader's atol and atof take the number a
// value starts with and stop at the first character of no number, and read a value of none as 0 [orig: j__atol @
// 0x76AB1B, _atof @ 0x76B6A1].
std::string not_a_number(const std::string &part, bool whole_number) {
	if (whole_number) {
		const int32_t read = io::retail_atol(part.c_str());
		return "'" + part + "' is no whole number: the game's atol reads it as " + std::to_string(read);
	}
	std::ostringstream read;
	read << io::retail_atof(part.c_str());
	return "'" + part + "' is no number: the game's atof reads it as " + read.str();
}

// A byte the reader stores from atol keeps the number's low byte [orig: the colours' HIBYTE/LOBYTE packing in
// CParticleDef_ParseProperties @ 0x5EA3D0.., CConfigReader_GetPackedRGB @ 0x5F1490; a table row's (BYTE) j__atol @
// CEffectTableDef_ParseCallback @ 0x5E415D]: 300 reads as 44.
std::string byte_read(const std::string &part, int n) {
	return "the game keeps the low byte of " + part + ": it reads " + std::to_string(n & 0xFF);
}

// `count` values, ',' between, each a whole number from `min` to `max` (a byte's) or each a number.
bool numbers(const std::string &value, size_t count, bool whole_numbers, int min, int max, std::string &why,
		const char *what) {
	const std::vector<std::string> parts = split_values(value, ',');
	if (parts.size() != count) {
		why = std::string("the game reads ") + what + ": " + std::to_string(count) + " values, ',' between";
		return false;
	}
	for (const std::string &part : parts) {
		int n = 0;
		if (whole_numbers ? !whole(part, n) : !real(part)) {
			why = not_a_number(part, whole_numbers);
			return false;
		}
		if (whole_numbers && (n < min || n > max)) {
			why = min == 0 && max == 255 ? byte_read(part, n)
			                             : "the game reads " + part + " as " + std::to_string(std::clamp(n, min, max));
			return false;
		}
	}
	return true;
}

// The keys a graphicN line copies from the particle into its layer as the line is read: the four colours, the alpha,
// the scale and its adjustment, and the five curve tables [orig: CParticleDefEntry_ParseGraphicProperty @ 0x5E3550,
// the copies after the blend mode @ 0x5E3600..]; the game's reader is line ordered, so such a key only reaches a
// layer from a line before the layer's graphic line.
bool copied_into_layers(const std::string &key) {
	for (const char *copied : {"color1", "color2", "color3", "color4", "alpha", "scale", "scale_adj", "scale_func",
	                           "alpha_func", "red_func", "green_func", "blue_func"})
		if (strutil::iequals(key, copied)) return true;
	return false;
}

// A graphic line's layer as the game counts it: graphic1 the first (index 0), any other graphicN the next, up to the
// fourth [orig: @ 0x5E3550, dword_2C0654C reset to 0 on graphic1, else one more while under 3].
struct GraphicLine {
	std::string key;
	size_t line_start = 0;
	int layer = 0;
};

std::vector<GraphicLine> graphic_lines(const std::string &text, const ParticleKeyBlock &block,
		const std::vector<particle::KeyPlace> &places) {
	std::vector<GraphicLine> out;
	int layer = -1;
	for (const particle::KeyPlace &place : places) {
		if (place.block != block.kind || place.index != block.index) continue;
		const std::string key = strutil::to_lower(place.key);
		if (key.rfind("graphic", 0) != 0) continue;
		layer = key == "graphic1" ? 0 : std::min(layer + 1, 3);
		size_t start = place.offset;
		while (start > 0 && text[start - 1] != '\n') --start;
		out.push_back({key, start, layer});
	}
	return out;
}

// The indent of the block's last key line (a tab where it writes none) and the text's line end at its closing
// brace's line (CR LF where the text writes one).
void layout_at(const std::string &text, const ParticleKeyBlock &block, const std::vector<particle::KeyPlace> &places,
		std::string &indent, std::string &eol) {
	indent = "\t";
	for (const particle::KeyPlace &place : places) {
		if (place.block != block.kind || place.index != block.index) continue;
		size_t start = place.offset;
		while (start > 0 && text[start - 1] != '\n') --start;
		size_t end = start;
		while (end < text.size() && (text[end] == ' ' || text[end] == '\t')) ++end;
		indent = text.substr(start, end - start);
	}
	const size_t lf = text.find('\n', block.close_offset);
	eol = lf != std::string::npos && lf > 0 && text[lf - 1] == '\r' ? "\r\n" : "\n";
}

} // namespace

std::vector<ParticleKeyBlock> particle_key_blocks(const TextDocument &document, bool *read) {
	std::vector<ParticleKeyBlock> out;
	particle::ParticleFile file;
	particle::ParticlePlaces places;
	particle::ParseError error;
	const std::string &text = document.text();
	const bool reads = particle::load_particles_with_places(text.data(), text.size(), file, places, error);
	if (read) *read = reads;
	if (!reads) return out;
	for (const particle::BlockPlace &place : places.blocks) {
		ParticleKeyBlock block;
		block.kind = place.kind;
		block.index = place.index;
		block.first_line = size_t(place.first_line);
		block.last_line = size_t(place.last_line);
		block.close_offset = place.close_offset;
		for (const particle::KeyPlace &key : places.keys) {
			if (key.block != place.kind || key.index != place.index) continue;
			ParticleKeyField field;
			field.key = key.key;
			field.row = particle::key_row(place.kind, key.key);
			field.present = true;
			field.value = text.substr(key.offset, key.length);
			field.span = document.span_at(key.offset, key.length);
			if (strutil::iequals(key.key, "id")) block.id = field.value;
			block.fields.push_back(std::move(field));
		}
		for (const KeyRow &row : particle::key_rows()) {
			if (row.block != place.kind || std::string(row.key).find_first_of("#*") != std::string::npos) continue;
			const bool written = std::any_of(block.fields.begin(), block.fields.end(),
					[&](const ParticleKeyField &f) { return f.present && f.row == &row; });
			if (written) continue;
			ParticleKeyField field;
			field.key = row.key;
			field.row = &row;
			block.fields.push_back(std::move(field));
		}
		out.push_back(std::move(block));
	}
	return out;
}

std::string particle_block_title(const ParticleKeyBlock &block) {
	return std::string(block_tag(block.kind)) + (block.id.empty() ? std::string(" (no id)") : " " + block.id);
}

std::string particle_key_words(const KeyRow &row) {
	std::string words;
	switch (row.value) {
	case KeyValueKind::Text: words = "a name"; break;
	case KeyValueKind::Real: words = "a number"; break;
	case KeyValueKind::Whole: words = "a whole number"; break;
	case KeyValueKind::Color: words = "a colour: red, green and blue, each 0 to 255"; break;
	case KeyValueKind::Vector: words = "three numbers: x, y and z"; break;
	case KeyValueKind::Flags: words = "the particle's flags, by name, blank-separated"; break;
	case KeyValueKind::Move: words = "how it moves, by name"; break;
	case KeyValueKind::Curve: words = "a table's id, then reverse, inverse or both"; break;
	case KeyValueKind::Graphic: words = "a texture, then its blend mode"; break;
	case KeyValueKind::Members: words = "the effect's particles, by id, ',' between"; break;
	case KeyValueKind::TableRow:
		words = "the table's next row, eight bytes, each 0 to 255 (the game takes any key holding a 't' as the next "
		        "row in order, its number aside; OpenNova reads tl1 to tl32 by number, D-PTL-32)";
		break;
	}
	if (row.clamped && row.port_bound)
		words += ", which OpenNova takes as " + std::to_string(row.min) + " to " + std::to_string(row.max) +
		         " (its own bound, " + row.port_bound + "; the game keeps the number as written)";
	else if (row.clamped)
		words += row.max == 0x7FFFFFFF ? ", which the game takes as at least " + std::to_string(row.min)
		                               : ", which the game takes as " + std::to_string(row.min) + " to " +
		                                         std::to_string(row.max);
	if (!row.read)
		words += row.block == particle::BlockKind::Handles
		                 ? "; the game never reads it (the block's header ends its read of the file)"
		                 : "; the game's reader has no case for it (only its writer writes it)";
	return words;
}

bool particle_key_value_ok(const KeyRow &row, const std::string &value, std::string &why) {
	why.clear();
	if (value.find_first_of(";\r\n") != std::string::npos) {
		why = "a ';' or a line's end ends the value where the game reads it";
		return false;
	}
	const std::string trimmed = strutil::trim(value);
	if (trimmed.empty()) {
		why = "an empty value, which the game reads as none";
		return false;
	}
	switch (row.value) {
	case KeyValueKind::Text: return true;
	case KeyValueKind::Real:
		if (!real(trimmed)) why = not_a_number(trimmed, false);
		return why.empty();
	case KeyValueKind::Whole: {
		int n = 0;
		if (!whole(trimmed, n)) {
			why = not_a_number(trimmed, true);
			return false;
		}
		if (row.clamped && (n < row.min || n > row.max)) {
			why = (row.port_bound ? "OpenNova reads " : "the game reads ") + trimmed + " as " +
			      std::to_string(std::clamp(n, row.min, row.max)) +
			      (row.port_bound ? std::string(" (its own bound, ") + row.port_bound + ")" : std::string());
			return false;
		}
		return true;
	}
	case KeyValueKind::Color: return numbers(trimmed, 3, true, 0, 255, why, "a colour");
	case KeyValueKind::TableRow: return numbers(trimmed, 8, true, 0, 255, why, "a table row");
	case KeyValueKind::Vector: return numbers(trimmed, 3, false, 0, 0, why, "a vector");
	case KeyValueKind::Flags:
	case KeyValueKind::Move:
		for (const std::string &word : words_of(trimmed)) {
			const bool known = row.value == KeyValueKind::Flags ? particle::parse_particle_flags(word) != 0
			                                                    : particle::parse_move_bits(word) != 0;
			if (!known) {
				why = "'" + word + "' names no " + (row.value == KeyValueKind::Flags ? "flag" : "motion") +
				      " the game knows, which it reads as none";
				return false;
			}
		}
		return true;
	case KeyValueKind::Curve: {
		const std::vector<std::string> words = words_of(trimmed);
		for (size_t i = 1; i < words.size(); ++i) {
			const std::string word = strutil::to_lower(words[i]);
			if (word != "reverse" && word != "inverse" && word != "invert") {
				why = "after the table's id the game reads reverse or inverse alone";
				return false;
			}
		}
		return true;
	}
	case KeyValueKind::Graphic: {
		const std::vector<std::string> parts = split_values(trimmed, ',');
		if (parts.empty() || parts[0].empty()) {
			why = "a graphic names its texture first";
			return false;
		}
		if (parts.size() > 1) {
			const std::string blend = strutil::to_lower(parts[1]);
			const std::string read = particle::blend_mode_name(particle::parse_blend_mode(blend));
			if (read != blend) {
				why = "the game reads the blend mode '" + parts[1] + "' as " + read;
				return false;
			}
		}
		return true;
	}
	case KeyValueKind::Members:
		for (const std::string &part : split_values(trimmed, ','))
			if (part.empty()) {
				why = "an empty member, which the game skips";
				return false;
			}
		return true;
	}
	return true;
}

bool particle_key_edit(const TextDocument &document, const ParticleKeyBlock &block, const std::string &key,
		const std::string &value, Edit &out, std::string &why, const ParticleKeyField *field) {
	const KeyRow *row = particle::key_row(block.kind, key);
	if (!row) {
		why = "a " + std::string(block_tag(block.kind)) + " block has no key '" + key + "' the game reads";
		return false;
	}
	if (!particle_key_value_ok(*row, value, why)) return false;
	const std::string written = strutil::trim(value);
	// The line the field stands for, where one is named (each line of a repeated key its own row); else the key where
	// the block writes it, the last line of it.
	const ParticleKeyField *present = field && field->present ? field : nullptr;
	if (!present)
		for (const ParticleKeyField &each : block.fields)
			if (each.present && strutil::iequals(each.key, key)) present = &each;
	if (present) {
		out = TextDocument::replace(present->span, written);
		return true;
	}
	// A key the block lacks: on its own line where the game's line-ordered reader takes it [orig:
	// CParticleDef_ParseProperties @ 0x5EA320, one line at a time; CParticleDefEntry_ParseGraphicProperty @
	// 0x5E3550]: a key a graphic line copies into its layer before the block's first graphic line; a layer's key
	// (gN_...) after its own graphicN line and before the next graphic line, the game writing it to the layer of
	// the graphic line before it whatever N is; a graphic line at the block's end where it opens the layer its N
	// names; any other before the block's closing brace.
	particle::ParticleFile file;
	particle::ParticlePlaces places;
	particle::ParseError error;
	const std::string &text = document.text();
	if (!particle::load_particles_with_places(text.data(), text.size(), file, places, error)) {
		why = "the game's reader stops in the text";
		return false;
	}
	size_t at = block.close_offset;
	if (block.kind == BlockKind::Particle) {
		const std::vector<GraphicLine> graphics = graphic_lines(text, block, places.keys);
		const std::string lower = strutil::to_lower(key);
		const bool layer_key = lower.size() > 3 && lower[0] == 'g' && lower[1] >= '1' && lower[1] <= '4' && lower[2] == '_';
		if (copied_into_layers(lower)) {
			if (!graphics.empty()) at = graphics.front().line_start;
		} else if (layer_key) {
			const int n = lower[1] - '0';
			const std::string own = "graphic" + std::to_string(n);
			size_t found = graphics.size();
			for (size_t i = 0; i < graphics.size(); ++i)
				if (graphics[i].key == own) found = i;
			if (found == graphics.size()) {
				why = "the block has no " + own + " line, and the game writes a layer's key to the layer of the graphic "
				      "line before it whatever its number [orig: CParticleDefEntry_ParseGraphicProperty @ 0x5E3550]";
				return false;
			}
			if (graphics[found].layer != n - 1) {
				why = "the game counts the block's layers by its graphic lines in order (graphic1 the first), so after " +
				      own + " it writes to layer " + std::to_string(graphics[found].layer + 1) + " [orig: @ 0x5E3550]";
				return false;
			}
			at = found + 1 < graphics.size() ? graphics[found + 1].line_start : block.close_offset;
		} else if (lower.rfind("graphic", 0) == 0) {
			const int n = lower.size() == 8 ? lower[7] - '0' : 0;
			const int layer = n == 1 ? 0 : graphics.empty() ? -1 : std::min(graphics.back().layer + 1, 3);
			if (layer != n - 1 || (n == 1 && !graphics.empty())) {
				why = n == 1 ? std::string("the block has graphic lines already: a graphic1 at its end makes the game read "
				                           "it into the first layer again [orig: @ 0x5E3550]")
				             : "at the block's end the game opens layer " + std::to_string(layer + 1) + " with it, not " +
				                       std::to_string(n) + " (graphic1 opens the first) [orig: @ 0x5E3550]";
				return false;
			}
		}
	}
	std::string indent, eol;
	layout_at(text, block, places.keys, indent, eol);
	out = TextDocument::replace(document.span_at(at, 0), indent + key + " = " + written + ";" + eol);
	return true;
}

} // namespace opennova::editor
