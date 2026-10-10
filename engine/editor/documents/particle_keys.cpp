#include <editor/documents/particle_keys.h>

#include <algorithm>
#include <optional>
#include <sstream>
#include <string>
#include <utility>

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

bool whole(const std::string &text, int &out) {
	const std::optional<int> parsed = strutil::parse_int(text);
	if (!parsed) return false;
	out = *parsed;
	return true;
}

bool real(const std::string &text) { return strutil::parse_float(text).has_value(); }

// `count` values, ',' between, each a whole number from `min` to `max` or each a number.
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
			why = "'" + part + "' is no number, which the game reads as 0";
			return false;
		}
		if (whole_numbers && (n < min || n > max)) {
			why = "the game reads " + part + " as " + std::to_string(std::clamp(n, min, max));
			return false;
		}
	}
	return true;
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

std::vector<ParticleKeyBlock> particle_key_blocks(const TextDocument &document) {
	std::vector<ParticleKeyBlock> out;
	particle::ParticleFile file;
	particle::ParseError error;
	const std::string &text = document.text();
	if (!particle::load_particles_from_buffer(text.data(), text.size(), file, error)) return out;
	for (const particle::BlockPlace &place : file.block_places) {
		ParticleKeyBlock block;
		block.kind = place.kind;
		block.index = place.index;
		block.first_line = size_t(place.first_line);
		block.last_line = size_t(place.last_line);
		block.close_offset = place.close_offset;
		for (const particle::KeyPlace &key : file.key_places) {
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
	case KeyValueKind::TableRow: words = "eight bytes, each 0 to 255"; break;
	}
	if (row.clamped)
		words += row.max == 0x7FFFFFFF ? ", which the game takes as at least " + std::to_string(row.min)
		                               : ", which the game takes as " + std::to_string(row.min) + " to " +
		                                         std::to_string(row.max);
	if (!row.read) words += "; the game's reader has no case for it (only its writer writes it)";
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
		if (!real(trimmed)) why = "'" + trimmed + "' is no number, which the game reads as 0";
		return why.empty();
	case KeyValueKind::Whole: {
		int n = 0;
		if (!whole(trimmed, n)) {
			why = "'" + trimmed + "' is no whole number, which the game reads as 0";
			return false;
		}
		if (row.clamped && (n < row.min || n > row.max)) {
			why = "the game reads " + trimmed + " as " + std::to_string(std::clamp(n, row.min, row.max));
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
		const std::string &value, Edit &out, std::string &why) {
	const KeyRow *row = particle::key_row(block.kind, key);
	if (!row) {
		why = "a " + std::string(block_tag(block.kind)) + " block has no key '" + key + "' the game reads";
		return false;
	}
	if (!particle_key_value_ok(*row, value, why)) return false;
	const std::string written = strutil::trim(value);
	// The key where the block writes it: the last line of it, the one the reader keeps.
	const ParticleKeyField *present = nullptr;
	for (const ParticleKeyField &field : block.fields)
		if (field.present && strutil::iequals(field.key, key)) present = &field;
	if (present) {
		out = TextDocument::replace(present->span, written);
		return true;
	}
	// A key the block lacks: its line before the block's closing brace.
	particle::ParticleFile file;
	particle::ParseError error;
	const std::string &text = document.text();
	if (!particle::load_particles_from_buffer(text.data(), text.size(), file, error)) {
		why = "the game's reader stops in the text";
		return false;
	}
	std::string indent, eol;
	layout_at(text, block, file.key_places, indent, eol);
	out = TextDocument::replace(document.span_at(block.close_offset, 0), indent + key + " = " + written + ";" + eol);
	return true;
}

} // namespace opennova::editor
