#include "def_write_record.h"
#include "def_scan.h"

#include <base/io/crt_ftol.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <iomanip>
#include <locale>
#include <sstream>

namespace opennova::def {
namespace {

std::string decimal(double value) {
	std::ostringstream out;
	out.imbue(std::locale::classic());
	out << std::fixed << std::setprecision(48) << value;
	std::string text = out.str();
	while (text.size() > 1 && text.back() == '0') text.pop_back();
	if (text.back() == '.') text.pop_back();
	return text == "-0" ? "0" : text;
}

// A real a line's float read takes [the parsers' parse_float_n, atof then a float store]: the shortest
// plain decimal that reads as the same float as the value's whole expansion does (5.4f as "5.4", not
// "5.400000095367431640625"), else that expansion. What the game reads is the same; the form is the one a
// person writes (the plain-words lane: a save after one edit no longer rewrites every real).
std::string float_decimal(double value) {
	const std::string whole = decimal(value);
	const float read = defscan::parse_float_n(whole.data(), whole.size());
	for (int precision = 0; precision <= 9; ++precision) {
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << std::fixed << std::setprecision(precision) << value;
		std::string text = out.str();
		if (text.find_first_not_of("-0.") == std::string::npos) text = "0";
		const float again = defscan::parse_float_n(text.data(), text.size());
		if (std::memcmp(&again, &read, sizeof(float)) == 0) return text;
	}
	return whole;
}

// The shortest plain decimal near `value` that the parser's own arithmetic over its atof reading takes
// to what the line should set (`reads`), else the whole expansion of `value`: "6" for a squib rate of
// ten ticks, not "6.048780487804878...".
template <class Reads> std::string shortest(double value, Reads reads) {
	for (int precision = 0; precision <= 17; ++precision) {
		std::ostringstream out;
		out.imbue(std::locale::classic());
		out << std::fixed << std::setprecision(precision) << value;
		std::string text = out.str();
		if (text.find_first_not_of("-0.") == std::string::npos) text = "0";
		if (reads(std::strtod(text.c_str(), nullptr))) return text;
	}
	return decimal(value);
}

// The death word as a squib rate: atof, 62 over it, _ftol2_sse [orig: ItemDef_ParseProperty @0x49F06F,
// the _ftol2_sse call @0x49F093]. Zero has none (62 over a reading is never 0).
std::string squib_rate(int32_t ticks) {
	if (ticks == INT32_MIN) return "0";
	return shortest(62.0 / (ticks + (ticks < 0 ? -0.25 : 0.25)),
	                [ticks](double read) { return io::retail_ftol_sse2(62.0 / read) == ticks; });
}

// A word as a 16.16 reading: atof times 65536, _ftol2_sse (sqb_distance, sqb_error @0x49F0DC / @0x49F125).
std::string squib_q16(int32_t word) {
	return shortest(double(word) / 65536.0, [word](double read) { return io::retail_ftol_sse2(read * 65536.0) == word; });
}

// The bytes of an item's death and clip words, and its door type, that a shared-word line written from
// the words as they stand sets to them (DefRecordWriter::alias_line): a door count past 30 none (its arm
// clamps it), a squib rate of a zero word none (no reading sets 0), a door_dir of the bit no token sets
// (bit 0, or past thirty) none.
DefRecordWriter::AliasCover alias_cover(const DefItemDef &item, uint8_t step) {
	DefRecordWriter::AliasCover held;
	const auto count = [&](int at) {
		if (((uint32_t(item.deathtime_ticks) >> (8 * at)) & 0xFF) <= 30) held.death = uint8_t(1u << at);
	};
	switch (step) {
	case DEF_LINE_ORDER_SQB_RATE: if (item.deathtime_ticks != 0) held.death = 0xF; break;
	case DEF_LINE_ORDER_NUM_DOORS: count(0); break;
	case DEF_LINE_ORDER_FIRST_DOOR: count(1); break;
	case DEF_LINE_ORDER_FIRST_SUBOBJECT: count(2); break;
	case DEF_LINE_ORDER_ROTOR_PARTS: held.death = 0x3; held.clip = 0x3; break;
	case DEF_LINE_ORDER_AUX_PARTS: held.death = 0xC; held.clip = 0xC; break;
	case DEF_LINE_ORDER_SQB_ERROR: held.door_type = true; break;
	case DEF_LINE_ORDER_SQB_DISTANCE: held.clip = 0xF; break;
	case DEF_LINE_ORDER_DOOR_DIR:
		if (!(uint32_t(item.clipsize) & 1) && !(uint32_t(item.clipsize) >> 31)) held.clip = 0xF;
		break;
	default: break;
	}
	return held;
}

std::string fixed(int64_t value) {
	// Invert the original decimal-digit walker, including its biased fractional
	// accumulation. A conventional decimal division is not always an inverse.
	const double target = static_cast<uint32_t>(value);
	double low = std::max(0.0, (target - 2.0) / 65536.0);
	double high = (target + 2.0) / 65536.0;
	for (int i = 0; i < 80; ++i) {
		const std::string text = decimal((low + high) * 0.5);
		const uint32_t parsed = static_cast<uint32_t>(defscan::parse_fixed16_digits_n(text.data(), text.size()));
		if (parsed == static_cast<uint32_t>(value)) return text;
		if (parsed < target) low = (low + high) * 0.5;
		else high = (low + high) * 0.5;
	}
	return decimal(target / 65536.0); // checked by the semantic gate before publication
}

std::string float_fixed(float value, int32_t bits) {
	// Pick the shortest ordinary decimal that preserves BOTH native views.
	for (int precision = 0; precision <= 9; ++precision) {
		std::ostringstream out; out.imbue(std::locale::classic());
		out << std::fixed << std::setprecision(precision) << value;
		const std::string text = out.str();
		if (defscan::parse_float_n(text.data(), text.size()) == value &&
			defscan::parse_fixed16_digits_n(text.data(), text.size()) == bits) return text;
	}
	return decimal(value); // the writer's reparse check reports unrepresentable pairs
}

// The shortest decimal whose product with `factor`, truncated toward zero the way
// the parsers' _ftol2 store does, is `value` again: the plain quotient when it
// survives the round trip, otherwise the middle of the interval that truncates to it.
std::string scaled(int64_t value, double factor) {
	const std::string text = decimal(double(value) / factor);
	if (int64_t(std::strtod(text.c_str(), nullptr) * factor) == value) return text;
	return decimal((double(value) + (value < 0 ? -0.5 : 0.5)) / factor);
}

int64_t integer(const DefValue &v) {
	return std::holds_alternative<int64_t>(v) ? std::get<int64_t>(v) :
		std::holds_alternative<double>(v) ? static_cast<int64_t>(std::get<double>(v)) : 0;
}
double real(const DefValue &v) {
	return std::holds_alternative<double>(v) ? std::get<double>(v) : double(integer(v));
}
std::string token(const DefValue &v) {
	if (const auto *s = std::get_if<std::string>(&v)) return s->empty() ? "\"\"" : *s;
	return std::holds_alternative<double>(v) ? float_decimal(std::get<double>(v)) : std::to_string(integer(v));
}

// A value as a line's word: a text as it is ("" for an empty one), a number as the writer
// puts it down. weapon.def's lines go through the retail tokenizer, which ends an unquoted
// token at a space, comma or tab and the line at ';', while a quoted run is one token, its
// quotes dropped [orig: Terrain_TokenizeConfigLine @0x53CB60, delimiters @0x53CC33..0x53CC4C,
// ';' @0x53CC2A..0x53CC31, quote @0x53CC4E..0x53CC70]: a text holding one of them is written
// quoted ("//" is refused before). The other families read a text value to the end of its
// line. powerup.def's lines go through the same tokenizer [orig: PowerUpDef_LoadFromFile @0x443350 over
// File_ParseASCIIFile @0x53D8C7, Terrain_TokenizeConfigLine @0x53CB60].
std::string written_word(DefRecordKind kind, const DefValue &v) {
	const bool tokenized = kind == DefRecordKind::Weapon || kind == DefRecordKind::Action ||
	                       kind == DefRecordKind::Sight || kind == DefRecordKind::Carry ||
	                       kind == DefRecordKind::Powerup || kind == DefRecordKind::PowerupAmmo ||
	                       kind == DefRecordKind::PowerupAction;
	const std::string t = token(v);
	return tokenized && t.find_first_of(" ,\t;") != std::string::npos ? "\"" + t + "\"" : t;
}

// Integer multiplication in the original 32-bit fields is modular. Prefer the
// ordinary small inverse, then solve factor*x = value (mod 2^32).
int64_t unscale(int64_t value, int64_t factor) {
	if (value % factor == 0) return value / factor;
	int64_t a = factor, modulus = int64_t(1) << 32;
	int64_t t = 0, next_t = 1, remainder = modulus, next_r = a;
	while (next_r) {
		const int64_t q = remainder / next_r;
		const int64_t old_t = t; t = next_t; next_t = old_t - q * next_t;
		const int64_t old_r = remainder; remainder = next_r; next_r = old_r - q * next_r;
	}
	const uint32_t bits = static_cast<uint32_t>(value);
	if (bits % remainder) return value / factor;
	modulus /= remainder;
	t %= modulus; if (t < 0) t += modulus;
	const uint64_t answer = (uint64_t(bits / remainder) * uint64_t(t)) % uint64_t(modulus);
	return static_cast<int32_t>(static_cast<uint32_t>(answer));
}

const char *item_type(int value) {
	switch (value) {
	case 1: return "vehicle"; case 2: return "decoration"; case 3: return "person";
	case 4: return "marker"; case 5: return "building"; case 6: return "powerup"; case 8: return "effect";
	default: return "";
	}
}

} // namespace

void DefRecordWriter::fail(const std::string &record, const std::string &field, const std::string &message) {
	result.diagnostics.push_back({DefIssueCode::Unrepresentable, 0, record, field, message});
}

std::string DefRecordWriter::margin(int levels) const {
	std::string out;
	for (int i = 0; i < levels; ++i) out += indent.empty() ? std::string("\t") : indent;
	return out;
}

void DefRecordWriter::line(const std::string &key, const std::vector<std::string> &values) {
	result.text += margin(depth) + key;
	for (const auto &value : values) result.text += (key.empty() && &value == &values.front() ? "" : " ") + value;
	result.text += "\r\n";
}

void DefRecordWriter::record(DefRecordKind kind, const void *value, const std::string &name,
                             const std::function<void(uint8_t step)> &nested) {
	// Aligned storage is needed by the native member accessors.
	std::vector<uint64_t> defaults((def_record_size(kind) + 7) / 8);
	def_init_record(kind, defaults.data());
	const std::vector<DefProperty> &properties = def_properties(kind);
	// The lines in the order the record was read in, then the rest in the table's; the record's rows and
	// blocks where its order puts them, else after its lines.
	std::vector<uint8_t> steps;
	std::vector<bool> placed(properties.size(), false);
	bool rows = false, blocks = false;
	if (const DefLineOrder *order = keep_order ? def_line_order(kind, value) : nullptr) {
		for (size_t i = 0; i < order->count; ++i) {
			const uint8_t step = order->steps[i];
			if (step == DEF_LINE_ORDER_ROWS) rows = true;
			else if (step == DEF_LINE_ORDER_BLOCKS) blocks = true;
			else if (def_line_order_alias(step)) {
				if (kind != DefRecordKind::Item) continue;
			} else if (step >= properties.size() || placed[step]) continue;
			else placed[step] = true;
			steps.push_back(step);
		}
	}
	for (size_t i = 0; i < properties.size(); ++i)
		if (!placed[i]) steps.push_back(uint8_t(i));
	if (!rows) steps.push_back(DEF_LINE_ORDER_ROWS);
	if (!blocks) steps.push_back(DEF_LINE_ORDER_BLOCKS);
	// What an item's shared-word lines after each step hold: a line of the word's own whose every byte a
	// later line sets leaves nothing the game keeps (a helicopter's deathtime under its rotor_parts and
	// aux_parts), and is left out; a door line raises the Door attribute itself.
	std::vector<AliasCover> later(steps.size());
	bool door_line = false;
	if (kind == DefRecordKind::Item) {
		const auto &item = *static_cast<const DefItemDef *>(value);
		AliasCover after;
		for (size_t i = steps.size(); i-- > 0;) {
			later[i] = after;
			if (!def_line_order_alias(steps[i])) continue;
			const AliasCover held = alias_cover(item, steps[i]);
			after.death |= held.death;
			after.clip |= held.clip;
			after.door_type = after.door_type || held.door_type;
			door_line = door_line || steps[i] == DEF_LINE_ORDER_NUM_DOORS || steps[i] == DEF_LINE_ORDER_FIRST_DOOR;
		}
	}
	AliasCover cover;
	for (size_t at = 0; at < steps.size(); ++at) {
		const uint8_t step = steps[at];
		if (step == DEF_LINE_ORDER_ROWS || step == DEF_LINE_ORDER_BLOCKS) {
			if (nested) nested(step);
			continue;
		}
		if (def_line_order_alias(step)) {
			alias_line(*static_cast<const DefItemDef *>(value), step, cover);
			continue;
		}
		const DefProperty &property = properties[step];
		std::vector<DefValue> values;
		bool changed = false, missing = false;
		for (const auto &id : property.fields) {
			const DefField *field = def_field(kind, id);
			if (!field) { fail(name, id, "Missing native field description."); missing = true; break; }
			values.push_back(def_get(value, *field));
			changed |= values.back() != def_get(defaults.data(), *field);
			if (const auto *text = std::get_if<std::string>(&values.back())) {
				if (text->size() >= field->width || text->find_first_of("\r\n\"") != std::string::npos || text->find("//") != std::string::npos)
					fail(name, id, "Text cannot be represented by the native grammar.");
			}
		}
		if (missing || values.empty()) continue;
		// The authored set's rewrite: this one line with the arguments it was given.
		if (&property == replaced) {
			line(replaced_key, replacement);
			continue;
		}
		if (!property.present_field.empty()) {
			const auto *field = def_field(kind, property.present_field);
			changed = field && integer(def_get(value, *field)) != 0;
		}
		if (kind == DefRecordKind::Item) {
            const auto &item = *static_cast<const DefItemDef *>(value);
            // These properties override what an earlier line derives: written where the derived value is
            // not the record's, or where the file has the line (its order keeps it). sound_profile sets the
            // female profile too while it tracks the primary [orig: @ 0x49fb0f-0x49fb64]; acceleration sets
            // a deceleration still unset to twice itself [orig: @0x49da4b].
            const bool read = placed[step];
            if (property.key == "sound_profilefemale" && item.sound_profile[0])
                changed = read || std::strcmp(item.sound_profile_female, item.sound_profile) != 0;
            if (property.key == "deceleration" && item.acceleration)
                changed = read || item.deceleration != item.acceleration * 2;
            // The death, clip and door-type words the file wrote under other names: left to those lines
            // where they hold every byte the word has. A door's death word of 1 is what its attrib line
            // gives it while still 0 [orig: ItemDef_ParseProperty @0x4a0cb0..0x4a0cb9].
            const auto held = [](int32_t word, uint8_t bytes) {
                for (int i = 0; i < 4; ++i)
                    if (!(bytes & (1u << i)) && ((uint32_t(word) >> (8 * i)) & 0xFF)) return false;
                return true;
            };
            if (!read && property.key == "deathtime")
                changed = changed && !held(item.deathtime_ticks, cover.death) &&
                          !(item.deathtime_ticks == 1 && (item.attrib & DEF_ITEM_ATTRIB_DOOR));
            if (!read && property.key == "clipsize") changed = changed && !held(item.clipsize, cover.clip);
            if (!read && property.key == "door_type") changed = changed && !cover.door_type;
            if (read && ((property.key == "deathtime" && later[at].death == 0xF) ||
                         (property.key == "clipsize" && later[at].clip == 0xF) || (property.key == "door_type" && later[at].door_type)))
                continue;
        }
		// An item's attributes on one `attrib:` line, its first word bits, its second's and Parent, as the
		// files write them: the chain reads every token of the line [orig: ItemDef_ParseProperty @ 0x49EB00,
		// the attrib arm @ 0x4A06A8], whichever word a token sets.
		if (property.encoding == DefEncoding::ItemAttrib2 || property.encoding == DefEncoding::ItemParent) continue;
		if (property.encoding == DefEncoding::ItemAttrib) {
			const auto &item = *static_cast<const DefItemDef *>(value);
			std::vector<std::string> tokens;
			uint32_t first = item.attrib, second = item.attrib2;
			// Door raised by a door line where the file has no attrib line of its own [orig: ItemDef_ParseProperty
			// @0x49F766..0x49F7DE].
			if (door_line && !placed[step]) first &= ~DEF_ITEM_ATTRIB_DOOR;
			for (int i = 0; i < def_item_attrib_keyword_count(); ++i)
				if (first & def_item_attrib_keyword_bit(i)) {
					tokens.push_back(def_item_attrib_keyword(i));
					first &= ~def_item_attrib_keyword_bit(i);
				}
			for (int i = 0; i < def_item_attrib2_keyword_count(); ++i)
				if (second & def_item_attrib2_keyword_bit(i)) {
					tokens.push_back(def_item_attrib2_keyword(i));
					second &= ~def_item_attrib2_keyword_bit(i);
				}
			if (item.attrib_parent) tokens.push_back("parent");
			if (first || second) fail(name, property.key, "Attributes contain bits without an authored token.");
			if (!tokens.empty()) line("attrib:", tokens);
			continue;
		}
		// A line the file has stays, its value the default or not (its order notes it: `score 0`, a
		// torque the table defaults to); a record made from nothing writes what differs.
		if (!changed && !placed[step] && kind != DefRecordKind::Sight && kind != DefRecordKind::Attachment &&
			kind != DefRecordKind::Effect && kind != DefRecordKind::Carry && kind != DefRecordKind::PowerupAmmo) continue;
		const std::string &key = property.key;
		auto n = [&](size_t i) { return integer(values.at(i)); };
		// The encodings that write a line per flag or entry.
		switch (property.encoding) {
		case DefEncoding::WeaponFlags: {
			uint32_t first = uint32_t(n(0)), second = uint32_t(n(1));
			for (size_t i = 0; const auto *flag = defscan::weapon_flag_at(i); ++i) {
				if ((first & flag->bit) || (second & flag->bit2)) {
					line("flags", {flag->name}); first &= ~flag->bit; second &= ~flag->bit2;
				}
			}
			if (first || second) fail(name, key, "Flags contain bits without an authored token.");
			continue;
		}
		case DefEncoding::AmmoFlags: {
			uint32_t remaining = uint32_t(n(0));
			for (size_t i = 0; const char *flag = def_ammo_flag_keyword(i); ++i) {
				const uint32_t bit = def_ammo_flag_bit(i);
				if (remaining & bit) { line("flag", {flag}); remaining &= ~bit; }
			}
			if (remaining) fail(name, key, "Flags contain bits without an authored token.");
			continue;
		}
		// A key alone, which its parser reads as 1 [orig: PowerUpDef_ParseProperty @0x443220].
		case DefEncoding::Switch: if (n(0)) line(key, {}); continue;
		// `weapon all` (every weapon), else `weapon <name>` [orig: PowerUpDef_ParseProperty
		// @0x4431A7..0x443216]: the name of a row that names every weapon is no part of the file.
		case DefEncoding::PowerupWeapon:
			if (n(1)) line(key, {"all"});
			else if (!std::get<std::string>(values[0]).empty()) line(key, {written_word(kind, values[0])});
			continue;
		case DefEncoding::CharacterFilter: case DefEncoding::TeamFilter: {
			const auto &weapon = *static_cast<const DefWeaponDef *>(value);
			const size_t count = property.encoding == DefEncoding::CharacterFilter ? weapon.charfilter_count : weapon.teamfilter_count;
			if (count > values.size()) { fail(name, key, "Too many filter entries."); continue; }
			for (size_t i = 0; i < count; ++i) line(key, {written_word(kind, values[i])});
			continue;
		}
		case DefEncoding::ClassRounds: {
			const char *names[] = {"", "medic", "sniper", "gunner", "", "rifleman", "engineer"};
			for (size_t i = 0; i < values.size(); ++i) if (n(i)) {
				if (!*names[i]) fail(name, key, "Class slot has no authored name.");
				else line(key, {names[i], std::to_string(n(i))});
			}
			continue;
		}
		default: break;
		}
		// A particle slot with no userpoint before its secondary: no one line holds it, the
		// retail tokenizer making no token of `""` (and items.def's reader a literal one). Two
		// lines read back to it: the full line, its userpoint any name, then the effect-only
		// line, which copies the effect and an empty userpoint and leaves the secondary alone
		// (read only past three tokens) [orig: ItemDef_ParseProperty @ 0x4A140B..0x4A1488
		// particlefxs, the secondary under count > 3 @ 0x4A145E; w1 @ 0x4A14DE, w2 @ 0x4A155E;
		// Terrain_TokenizeConfigLine @ 0x53CB71..0x53CB81 resetting tokens 1 and 2].
		if (property.encoding == DefEncoding::ParticleSlot && values.size() >= 3) {
			const std::string &effect = std::get<std::string>(values[0]);
			const std::string &userpoint = std::get<std::string>(values[1]);
			const std::string &secondary = std::get<std::string>(values[2]);
			if (userpoint.empty() && !secondary.empty()) {
				const std::string placeholder = effect.empty() ? secondary : effect;
				line(property.key, {placeholder, placeholder, secondary});
				if (effect.empty()) line(property.key, {});
				else line(property.key, {effect});
				continue;
			}
		}
		std::string written_key;
		std::vector<std::string> args;
		if (!property_args(kind, property, value, values, name, written_key, args)) continue;
		while (args.size() > 1 && args.back() == "\"\"") args.pop_back();
		if (!args.empty()) line(written_key, args);
	}
}

bool DefRecordWriter::property_args(DefRecordKind kind, const DefProperty &property, const void *value,
                                    const std::vector<DefValue> &values, const std::string &name, std::string &key,
                                    std::vector<std::string> &args) {
	key = property.key;
	args.clear();
	auto n = [&](size_t i) { return integer(values.at(i)); };
	auto f = [&](size_t i) { return real(values.at(i)); };
	auto text = [&](size_t i) -> std::string { return std::get<std::string>(values.at(i)); };
	auto word = [&](const DefValue &v) { return written_word(kind, v); };
	auto color = [&](int64_t c) {
		args.push_back(std::to_string(c >> 16));
		args.push_back(std::to_string((c >> 8) & 255));
		args.push_back(std::to_string(c & 255));
	};
	switch (property.encoding) {
	case DefEncoding::ItemType:
		if (!*item_type(int(n(0)))) fail(name, key, "Unknown item type.");
		args.push_back(item_type(int(n(0)))); break;
	case DefEncoding::AmmoKillZone: {
		const char *keyword = def_ammo_kz_keyword(size_t(n(0)));
		if (!keyword) { fail(name, key, "Unknown kill-zone type."); return false; }
		args.push_back(keyword); break;
	}
	case DefEncoding::Function: {
		const auto &action = *static_cast<const DefWeaponAction *>(value);
		if (action.function_args_count > 4) { fail(name, key, "At most four function arguments are supported."); return false; }
		args.push_back(word(values[0]));
		for (size_t i = 0; i < action.function_args_count; ++i) args.push_back(std::to_string(n(i + 1)));
		break;
	}
	case DefEncoding::FloatFixed:
		for (size_t i = 0; i < values.size() / 2; ++i) args.push_back(float_fixed(float(f(i)), int32_t(n(i + values.size() / 2))));
		break;
	case DefEncoding::Fixed16:
		for (const auto &v : values) args.push_back(std::holds_alternative<std::string>(v) ? word(v) : fixed(integer(v)));
		break;
	case DefEncoding::FixedSeconds: args.push_back(fixed(int64_t(std::llround(n(0) * 65536.0 / 62.0)))); break;
	case DefEncoding::TurnRate: args.push_back(fixed(int64_t(std::llround(n(0) * 65536.0 / 192426.0)))); break;
	case DefEncoding::Degrees: args.push_back(std::to_string(unscale(n(0), 11930464))); break;
	case DefEncoding::HalfDegrees: args.push_back(std::to_string(unscale(n(0), 11930464) * 2)); break;
	case DefEncoding::ScaledInteger:
		for (const auto &v : values) args.push_back(std::to_string(unscale(integer(v), int64_t(property.factor)))); break;
	case DefEncoding::ScaledReal:
		for (const auto &v : values)
			args.push_back(std::holds_alternative<std::string>(v) ? word(v)
			               : std::holds_alternative<int64_t>(v) ? scaled(integer(v), property.factor)
			                                                    : decimal(real(v) / property.factor));
		break;
	case DefEncoding::Percent:
		// The parser reads an integer and scales it by 0.01f: 35 becomes 0.35f, which
		// is below 0.35, so the inverse rounds instead of dividing.
		args.push_back(std::to_string(std::llround(f(0) * 100.0))); break;
	case DefEncoding::ShotTiming: {
		// Region 0 without a flag name is what `particletesttime` authors; a nameless
		// timing in another region has no authored form (its two times are put down all
		// the same, as what the editor shows of them).
		const std::string flag = text(0);
		if (flag.empty()) {
			if (key != "dawnshot") fail(name, key, "A timing without a flag name has no authored form.");
			else key = "particletesttime";
		} else args.push_back(flag);
		args.push_back(scaled(n(1), 62.0)); args.push_back(scaled(n(2), 62.0));
		break;
	}
	case DefEncoding::Pose:
		for (size_t i = 0; i < 6; ++i) args.push_back(i < 3 ? decimal(f(i)) : fixed(n(i))); break;
	case DefEncoding::Delay: args.push_back(n(0) == -1 ? "auto" : std::to_string(n(0))); break;
	case DefEncoding::ScopeParallax: args.push_back(decimal((n(0) + (n(0) < 0 ? -0.25 : 0.25)) / 65535.0)); break;
	case DefEncoding::Heat: args = {fixed(n(0) * 100), fixed(n(1) * 6200)}; break;
	case DefEncoding::LightMove: case DefEncoding::LightImpact:
		args.push_back(fixed(n(0))); color(n(1));
		if (property.encoding == DefEncoding::LightImpact) args.push_back(fixed(int64_t(std::llround(n(2) * 65536.0 / 62.0))));
		break;
	case DefEncoding::ItemDeathTime: {
		// Whole seconds where the ticks are some: 62 a second and 62 of grace, an explicit 0 eight
		// seconds [orig: ItemDef_ParseProperty @ 0x49fa6c-0x49faa0]; any other count as the squib alias,
		// which writes the same native field without deathtime's integer-seconds restriction.
		const int32_t ticks = int32_t(n(0));
		if ((int64_t(ticks) - 62) % 62 == 0 && ticks != 62) {
			args.push_back(std::to_string((int64_t(ticks) - 62) / 62));
			break;
		}
		key = "sqb_rate";
		args.push_back(squib_rate(ticks));
		break;
	}
	case DefEncoding::DoorType:
		key = "sqb_error"; args.push_back(squib_q16(int32_t(uint32_t(n(0))))); break;
	// atof, then 65536 over 62 times it, _ftol2_sse [orig: ItemDef_ParseProperty @0x49F91E].
	case DefEncoding::DoorOpenRate: {
		const int32_t rate = int32_t(n(0));
		args.push_back(rate == 0 ? "0"
		                         : shortest(65536.0 / (62.0 * (rate + (rate < 0 ? -0.25 : 0.25))), [rate](double read) {
			                           return io::retail_ftol_sse2(65536.0 / (read * 62.0)) == rate;
		                           }));
		break;
	}
	// atof over 360 times 4294967295, _ftol2_sse [orig: ItemDef_ParseProperty @0x49F96D].
	case DefEncoding::DoorMaxAngle: {
		const int32_t angle = int32_t(n(0));
		args.push_back(shortest((angle + (angle < 0 ? -0.25 : 0.25)) * 360.0 / 4294967295.0, [angle](double read) {
			return io::retail_ftol_sse2(read * (1.0 / 360.0) * 4294967295.0) == angle;
		}));
		break;
	}
	case DefEncoding::HuskSeconds: args.push_back(decimal(f(0) / 62.0)); break;
	case DefEncoding::HuskSwap: {
		const auto &item = *static_cast<const DefItemDef *>(value);
		args.push_back(decimal(item.husk_swap_at_sec == 0 ? f(0) * 100.0 : f(0) / 62.0)); break;
	}
	case DefEncoding::DeathPieces: {
		// Every slot up to the last with a piece, as the files list them ("01_HULL 02_WHEEL"): HULL is
		// the table's first row, which an unwritten slot reads as too [orig: @ 0x49f314-0x49f396].
		size_t last = 0;
		for (size_t i = 0; i < values.size(); ++i)
			if (n(i)) last = i + 1;
		for (size_t i = 0; i < last; ++i) {
			const char *piece = defscan::death_piece_keyword(size_t(n(i)));
			if (!piece) {
				fail(name, key, "Unknown debris type.");
				continue;
			}
			char slot[8];
			std::snprintf(slot, sizeof(slot), "%02zu_", i + 1);
			args.push_back(slot + std::string(piece));
		}
		break;
	}
	case DefEncoding::SpawnMask:
		if (!items) { fail(name, key, "Missing file-wide vehicle spawn registry."); return false; }
		for (int i = 0; i < items->vehicle_spawn_id_count; ++i) if (uint32_t(n(0)) & (uint32_t(1) << i)) args.push_back(std::to_string(items->vehicle_spawn_ids[i]));
		break;
	case DefEncoding::Sight: {
		const auto &sight = *static_cast<const DefSightEntry *>(value);
		for (size_t i = 0; i < 5; ++i) args.push_back(word(values[i]));
		const char *blend[] = {"blend", "add", "blendat", "multiply", "addat", "multiplyat"};
		if (sight.blend < 0 || sight.blend > 5) { fail(name, key, "Unknown sight blend mode."); return false; }
		args.push_back(blend[sight.blend]);
		if (sight.scale) args.push_back("scale");
		if (sight.slide) { args.push_back("slide"); args.push_back(std::to_string(sight.slide_frames)); }
		break;
	}
	case DefEncoding::ModelOption:
		// The model, then the loader's one option [orig: WeaponDefs_ParseLineCallback @ 0x544F92].
		args.push_back(word(values[0]));
		if (n(1)) args.push_back("nocheckdepth");
		break;
	case DefEncoding::Attachment: {
		const auto &attachment = *static_cast<const DefItemEmplacementAttachment *>(value);
		key = attachment.kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_G ? "addeweapg" : attachment.kind == DEF_ITEM_EMPLACEMENT_ADDEWEAP_C ? "addeweapc" : "addeweap";
		args = {word(values[0]), word(values[1])};
		if (attachment.angle_count == 4) {
			for (size_t i = 2; i < 6; ++i) args.push_back(std::to_string(unscale(i == 3 || i == 5 ? -n(i) : n(i), 11930464)));
		} else if (attachment.angle_count != 0) fail(name, key, "An attachment has either zero or four angles.");
		break;
	}
	case DefEncoding::WeaponFlags: case DefEncoding::AmmoFlags: case DefEncoding::ItemAttrib:
	case DefEncoding::ItemAttrib2: case DefEncoding::ItemParent: case DefEncoding::CharacterFilter:
	case DefEncoding::TeamFilter: case DefEncoding::ClassRounds: case DefEncoding::Switch:
	case DefEncoding::PowerupWeapon:
		return false; // a line per flag or entry, or one record words itself: record writes them
	default: for (const auto &v : values) args.push_back(word(v)); break;
	}
	return true;
}

// Each name as its arm reads it [orig: ItemDef_ParseProperty @ 0x49EB00]: the squib arms atof through
// _ftol2_sse (@0x49F06F); num_doors, first_door and first_subobject one byte each of the death word, the
// low byte of atol read signed, the first_* keys one less, clamped to 0..30 (@0x49F766, @0x49F7BA,
// @0x49F9B0); rotor_parts and aux_parts four atol low bytes across the death and clip words (@0x49EF5D,
// @0x49EFF3); door_dir a bit past each nonzero token, up to thirty. A byte a line cannot hold as it
// stands (a door count past 30) is put down all the same and left unheld, for the word's own line.
void DefRecordWriter::alias_line(const DefItemDef &item, uint8_t step, AliasCover &cover) {
	const int32_t death = item.deathtime_ticks, clip = item.clipsize;
	const auto byte = [](int32_t word, int at) { return std::to_string((uint32_t(word) >> (8 * at)) & 0xFF); };
	const auto count = [&](int at, int more) {
		line(at == 0 ? "num_doors" : at == 1 ? "first_door" : "first_subobject",
		     {std::to_string(int((uint32_t(death) >> (8 * at)) & 0xFF) + more)});
	};
	switch (step) {
	case DEF_LINE_ORDER_SQB_RATE:
		if (death == 0) return; // no reading of the rate sets 0: the words' own lines say it
		line("sqb_rate", {squib_rate(death)});
		break;
	case DEF_LINE_ORDER_NUM_DOORS: count(0, 0); break;
	case DEF_LINE_ORDER_FIRST_DOOR: count(1, 1); break;
	case DEF_LINE_ORDER_FIRST_SUBOBJECT: count(2, 1); break;
	case DEF_LINE_ORDER_ROTOR_PARTS: line("rotor_parts", {byte(death, 0), byte(death, 1), byte(clip, 0), byte(clip, 1)}); break;
	case DEF_LINE_ORDER_AUX_PARTS: line("aux_parts", {byte(clip, 2), byte(clip, 3), byte(death, 2), byte(death, 3)}); break;
	case DEF_LINE_ORDER_SQB_ERROR: line("sqb_error", {squib_q16(int32_t(item.door_type))}); break;
	case DEF_LINE_ORDER_SQB_DISTANCE: line("sqb_distance", {squib_q16(clip)}); break;
	case DEF_LINE_ORDER_DOOR_DIR: {
		std::vector<std::string> tokens;
		const uint32_t bits = uint32_t(clip);
		for (int i = 0; i < 30 && (bits >> (i + 1)); ++i) tokens.push_back((bits >> (i + 1)) & 1 ? "1" : "0");
		if (tokens.empty()) tokens.push_back("0");
		line("door_dir", tokens);
		break;
	}
	default: return;
	}
	const AliasCover held = alias_cover(item, step);
	cover.death |= held.death;
	cover.clip |= held.clip;
	cover.door_type = cover.door_type || held.door_type;
}

} // namespace opennova::def
