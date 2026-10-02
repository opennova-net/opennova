// A member's number in the units the file writes it (def_schema.h, ADR 0046 S12): the
// writer's own argument for it, and a set that goes through the writer and the family's
// parser as a saved file would.
#include "def_schema.h"
#include "def.h"
#include "def_write_record.h"

#include <charconv>
#include <climits>
#include <cmath>
#include <cstring>
#include <system_error>

namespace opennova::def {
namespace {

thread_local DefAuthoredParses g_parses;

// The authored number's type of the member at `index` of `property`, by its encoding.
DefAuthored authored_type(DefRecordKind kind, const DefProperty &property, size_t index) {
	const DefField *field = def_field(kind, property.fields[index]);
	if (!field || field->type == DefFieldType::Text || field->read_only) return DefAuthored::None;
	switch (property.encoding) {
	case DefEncoding::ScaledInteger: case DefEncoding::Degrees: case DefEncoding::HalfDegrees:
	case DefEncoding::Percent:
		return DefAuthored::Integer;
	case DefEncoding::Fixed16: case DefEncoding::ScaledReal: case DefEncoding::FixedSeconds:
	case DefEncoding::TurnRate: case DefEncoding::Heat: case DefEncoding::Pose:
		return DefAuthored::Real;
	case DefEncoding::LightMove: return index == 0 ? DefAuthored::Real : DefAuthored::None;
	case DefEncoding::LightImpact: return index == 0 || index == 2 ? DefAuthored::Real : DefAuthored::None;
	case DefEncoding::ShotTiming: return index > 0 ? DefAuthored::Real : DefAuthored::None;
	default: return DefAuthored::None;
	}
}

// Which argument of the line the member's number is: its own place, but a light's colour
// takes three (r g b) [orig: AmmoDef_ParseProperty 'light_impact' @ 0x40af79 -> +132 / +128 /
// +136] and a region-0 timing with no flag name writes no name (particletesttime)
// [orig: ItemDef_ParseProperty @ 0x49EB00, particletesttime @ 0x49FAD1].
size_t argument_of(const DefProperty &property, size_t index, const std::vector<DefValue> &values) {
	switch (property.encoding) {
	case DefEncoding::LightImpact: return index == 0 ? 0 : 4;
	case DefEncoding::ShotTiming: return std::get<std::string>(values[0]).empty() ? index - 1 : index;
	default: return index;
	}
}

// The line's members as the record holds them, by the member's line (DefMember::line).
bool member_values(const DefMember &member, const void *record, std::vector<DefValue> &out) {
	out.clear();
	for (const DefField *field : member.line) {
		if (!field) return false;
		out.push_back(def_get(record, *field));
	}
	return true;
}

bool parse_number(const std::string &text, DefAuthored type, DefValue &out) {
	const char *first = text.data(), *last = text.data() + text.size();
	if (type == DefAuthored::Integer) {
		int64_t number = 0;
		const auto read = std::from_chars(first, last, number);
		if (read.ec != std::errc() || read.ptr != last) return false;
		out = number;
		return true;
	}
	double number = 0.0;
	const auto read = std::from_chars(first, last, number, std::chars_format::fixed);
	if (read.ec != std::errc() || read.ptr != last) return false;
	out = number;
	return true;
}

// A record's header line, which each family's parser opens a record on.
const char *record_header(DefRecordKind kind) {
	return kind == DefRecordKind::Item     ? "begin \"authored\"\r\n"
	       : kind == DefRecordKind::Weapon ? "weapon \"authored\"\r\n"
	       : kind == DefRecordKind::Ammo   ? "ammo authored\r\n"
	                                       : nullptr;
}

// One record's text read through its family's parser into `out` (a record's storage). False
// when the parser does not read it as one record, or blocks on it.
bool parse_record(DefRecordKind kind, const std::string &text, std::vector<uint64_t> &out) {
	const auto *bytes = reinterpret_cast<const uint8_t *>(text.data());
	const size_t size = text.size();
	out.assign((def_record_size(kind) + 7) / 8, 0);
	DefParseReport report;
	bool one = false;
	// The parsed record's own arrays are freed with its file; only its members are read.
	if (kind == DefRecordKind::Item) {
		DefItemsFile file{};
		def_parse_items_memory(bytes, size, &file, &report);
		one = file.count == 1;
		if (one) std::memcpy(out.data(), &file.entries[0], sizeof(DefItemDef));
		def_free_items(&file);
	} else if (kind == DefRecordKind::Weapon) {
		DefWeaponsFile file{};
		def_parse_weapons_memory(bytes, size, &file, &report);
		one = file.count == 1;
		if (one) std::memcpy(out.data(), &file.entries[0], sizeof(DefWeaponDef));
		def_free_weapons(&file);
	} else {
		DefAmmoFile file{};
		def_parse_ammo_memory(bytes, size, &file, &report);
		one = file.count == 1;
		if (one) std::memcpy(out.data(), &file.entries[0], sizeof(DefAmmoDef));
		def_free_ammo(&file);
	}
	return one && !def_report_blocks(report);
}

// The record as the writer puts it down, the line of `replaced` (when given) written as
// `key` with `args`, read back through its family's parser into `out`.
bool read_back(DefRecordKind kind, const void *record, const DefProperty *replaced, const std::string &key,
               const std::vector<std::string> &args, std::vector<uint64_t> &out) {
	const char *header = record_header(kind);
	if (!header) return false;
	++g_parses.records;
	DefRecordWriter writer;
	writer.replaced = replaced;
	writer.replaced_key = key;
	writer.replacement = args;
	writer.result.text = header;
	writer.record(kind, record, "authored");
	writer.result.text += "end\r\n";
	return parse_record(kind, writer.result.text, out);
}

// Whether the line `key` `args`, written alone, reads the member `field` back as the record
// holds it: the gate on showing a member in written units (a stored word past what the line's
// numbers carry, such as an overflowed turn rate, reads back as another).
bool line_keeps(DefRecordKind kind, const void *record, const DefField &field, const std::string &key,
                const std::vector<std::string> &args) {
	const char *header = record_header(kind);
	if (!header) return false;
	++g_parses.lines;
	DefRecordWriter writer;
	writer.result.text = header;
	writer.line(key, args);
	writer.result.text += "end\r\n";
	std::vector<uint64_t> parsed;
	return parse_record(kind, writer.result.text, parsed) && def_get(parsed.data(), field) == def_get(record, field);
}

std::string shortest(double number) {
	char text[400];
	const auto written = std::to_chars(text, text + sizeof(text), number, std::chars_format::fixed);
	return written.ec == std::errc() ? std::string(text, written.ptr) : std::string();
}

} // namespace

DefAuthoredParses def_authored_parses() { return g_parses; }

const DefProperty *def_member_property(DefRecordKind kind, const std::string &id, size_t *index) {
	for (const DefProperty &property : def_properties(kind))
		for (size_t i = 0; i < property.fields.size(); ++i)
			if (property.fields[i] == id) {
				if (index) *index = i;
				return &property;
			}
	return nullptr;
}

DefMember def_member(DefRecordKind kind, const std::string &id) {
	DefMember out;
	out.kind = kind;
	out.field = def_field(kind, id);
	if (!out.field) return out;
	out.property = def_member_property(kind, id, &out.index);
	if (!out.property) return out;
	for (const std::string &member : out.property->fields) out.line.push_back(def_field(kind, member));
	out.authored = authored_type(kind, *out.property, out.index);
	if (!out.property->present_field.empty()) out.present = def_field(kind, out.property->present_field);
	return out;
}

bool def_authored_get(const DefMember &member, const void *record, DefValue &out) {
	std::vector<DefValue> values;
	if (member.authored == DefAuthored::None || !member.property || !member_values(member, record, values))
		return false;
	// The line's numbers as the writer puts them down, whether it writes the line or not (a
	// failure, such as a nameless timing outside region 0, still puts its numbers down).
	DefRecordWriter writer;
	std::string key;
	std::vector<std::string> args;
	writer.property_args(member.kind, *member.property, record, values, "", key, args);
	const size_t at = argument_of(*member.property, member.index, values);
	return at < args.size() && parse_number(args[at], member.authored, out) &&
	       line_keeps(member.kind, record, *member.field, key, args);
}

bool def_authored_set(const DefMember &member, void *record, const DefValue &value, std::string &error) {
	const DefRecordKind kind = member.kind;
	const DefProperty *property = member.property;
	const DefAuthored type = member.authored;
	std::vector<DefValue> values;
	if (type == DefAuthored::None || !property || !member_values(member, record, values)) {
		error = "This field is not a number of its own on its line.";
		return false;
	}
	// The argument as the file would write it: a whole number, or a real's shortest decimal
	// (never an exponent: the engine's digit walker reads none [orig: Math_ParseFixedPoint16
	// @ 0x6131f0]).
	std::string text;
	if (type == DefAuthored::Integer) {
		const auto *number = std::get_if<int64_t>(&value);
		if (!number) {
			error = "This field takes a whole number.";
			return false;
		}
		// The parser reads a whole number into a 32-bit word [orig: ItemDef_ParsePhysicsProperty
		// @ 0x49D870 over j__atol @ 0x76ab1b]: past it, each platform's own narrowing would
		// decide what is stored.
		if (*number < INT32_MIN || *number > INT32_MAX) {
			error = "The game reads a whole number from -2147483648 to 2147483647 here.";
			return false;
		}
		text = std::to_string(*number);
	} else {
		const double number = std::holds_alternative<double>(value)    ? std::get<double>(value)
		                      : std::holds_alternative<int64_t>(value) ? double(std::get<int64_t>(value))
		                                                               : std::nan("");
		if (!std::isfinite(number)) {
			error = "Enter a finite number.";
			return false;
		}
		text = shortest(number);
	}
	DefValue current, wanted;
	if (def_authored_get(member, record, current)) {
		if (parse_number(text, type, wanted) && current == wanted) return true; // the number the member already writes
	} else {
		// A member its line cannot write in the file's units shows as stored: a Set of that
		// stored number changes nothing either.
		const DefValue stored = def_get(record, *member.field);
		const auto *whole = std::get_if<int64_t>(&stored);
		const auto *real = std::get_if<double>(&stored);
		const double number = std::holds_alternative<double>(value) ? std::get<double>(value)
		                      : std::holds_alternative<int64_t>(value) ? double(std::get<int64_t>(value))
		                                                               : std::nan("");
		if ((whole && double(*whole) == number) || (real && *real == number)) return true;
	}
	DefRecordWriter writer;
	std::string key;
	std::vector<std::string> args;
	writer.property_args(kind, *property, record, values, "", key, args);
	const size_t at = argument_of(*property, member.index, values);
	if (!writer.result.ok() || at >= args.size()) {
		// A line the file has no form for (a timing with no flag name outside region 0).
		error = writer.result.ok() ? "This line has no place for the number." : writer.result.diagnostics.front().message;
		return false;
	}
	args[at] = text;
	std::vector<uint64_t> parsed;
	if (!read_back(kind, record, property, key, args, parsed)) {
		error = "The game's parser does not read that line back.";
		return false;
	}
	// The line's members as the parser read them, on a copy of the record; kept only where
	// the writer writes them back as they are.
	std::vector<uint64_t> trial((def_record_size(kind) + 7) / 8);
	std::memcpy(trial.data(), record, def_record_size(kind));
	for (const DefField *field : member.line)
		std::memcpy(reinterpret_cast<uint8_t *>(trial.data()) + field->offset,
		            reinterpret_cast<const uint8_t *>(parsed.data()) + field->offset, field->width);
	// A text of the line read otherwise (a name the tokenizer drops, so the number is read as
	// the name) is a line the file cannot hold: the numbers alone may move, as the parser moves
	// them (an unset fade read as 10 ticks [orig: AmmoDef_ParseProperty @ 0x40b005]).
	bool kept = true;
	for (size_t i = 0; i < member.line.size(); ++i)
		if (std::holds_alternative<std::string>(values[i]))
			kept = kept && def_get(trial.data(), *member.line[i]) == values[i];
	std::vector<uint64_t> again;
	kept = kept && read_back(kind, trial.data(), nullptr, std::string(), {}, again);
	for (const DefField *field : member.line)
		kept = kept && def_get(trial.data(), *field) == def_get(again.data(), *field);
	if (!kept) {
		error = "The file cannot write this number back as the game reads it.";
		return false;
	}
	for (const DefField *field : member.line)
		std::memcpy(static_cast<uint8_t *>(record) + field->offset,
		            reinterpret_cast<const uint8_t *>(trial.data()) + field->offset, field->width);
	return true;
}

} // namespace opennova::def
