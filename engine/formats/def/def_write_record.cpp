#include "def_write_record.h"
#include "def_scan.h"

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
	return std::holds_alternative<double>(v) ? decimal(std::get<double>(v)) : std::to_string(integer(v));
}

// A value as a line's word: a text as it is ("" for an empty one), a number as the writer
// puts it down. Every family's lines go through the retail tokenizer
// (defscan::for_each_def_line), which ends an unquoted token at a space, comma or tab and the
// line at ';', while a quoted run is one token, its quotes dropped [orig:
// Terrain_TokenizeConfigLine @0x53CB60, delimiters @0x53CC33..0x53CC4C, ';' @0x53CC2A..0x53CC31,
// quote @0x53CC4E..0x53CC70]: a text holding one of them is written quoted ("//" is refused
// before).
std::string written_word(DefRecordKind, const DefValue &v) {
	const std::string t = token(v);
	return t.find_first_of(" ,\t;") != std::string::npos ? "\"" + t + "\"" : t;
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

void DefRecordWriter::line(const std::string &key, const std::vector<std::string> &values) {
	result.text += "\t" + key;
	for (const auto &value : values) result.text += (key.empty() && &value == &values.front() ? "" : " ") + value;
	result.text += "\r\n";
}

void DefRecordWriter::record(DefRecordKind kind, const void *value, const std::string &name) {
	// Aligned storage is needed by the native member accessors.
	std::vector<uint64_t> defaults((def_record_size(kind) + 7) / 8);
	def_init_record(kind, defaults.data());
	for (const auto &property : def_properties(kind)) {
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
            // These properties override defaults established by an earlier property.
            if (property.key == "sound_profilefemale" && item.sound_profile[0]) changed = true;
            if (property.key == "deceleration" && item.acceleration) changed = true;
            // An item closed with no alias reads "S%06i" of its id [orig: ItemDef_ParseProperty,
            // the `end` arm @0x49EB2F..0x49EB5F]: that alias is the item's without a `sid` line.
            if (property.key == "sid") {
                char alias[16];
                std::snprintf(alias, sizeof(alias), "S%06i", item.id);
                if (std::strcmp(alias, item.sid) == 0) changed = false;
            }
        }
        if (!changed && kind != DefRecordKind::Sight && kind != DefRecordKind::Attachment &&
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
		case DefEncoding::ItemAttrib: case DefEncoding::ItemAttrib2: {
			const bool second = property.encoding == DefEncoding::ItemAttrib2;
			uint32_t remaining = uint32_t(n(0));
			const int count = second ? def_item_attrib2_keyword_count() : def_item_attrib_keyword_count();
			for (int i = 0; i < count; ++i) {
				const uint32_t bit = second ? def_item_attrib2_keyword_bit(i) : def_item_attrib_keyword_bit(i);
				if (remaining & bit) {
					line("attrib:", {second ? def_item_attrib2_keyword(i) : def_item_attrib_keyword(i)});
					remaining &= ~bit;
				}
			}
			if (remaining) fail(name, key, "Attributes contain bits without an authored token.");
			continue;
		}
		case DefEncoding::ItemParent: if (n(0)) line("attrib:", {"parent"}); continue;
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
	case DefEncoding::ItemDeathTime:
		// The squib alias writes the same native field without deathtime's
		// integer-seconds restriction. Zero is represented by the parser's
		// reciprocal-zero result. Neither spelling retains source trivia.
		key = "sqb_rate"; args.push_back(n(0) == 0 ? "0" : decimal(62.0 / (n(0) + (n(0) < 0 ? -0.25 : 0.25)))); break;
	case DefEncoding::DoorType:
		key = "sqb_error"; args.push_back(decimal(double(uint32_t(n(0))) / 65536.0)); break;
	case DefEncoding::DoorOpenRate: args.push_back(n(0) == 0 ? "0" : decimal(65536.0 / (62.0 * (n(0) + (n(0) < 0 ? -0.25 : 0.25))))); break;
	case DefEncoding::DoorMaxAngle: args.push_back(decimal((n(0) + (n(0) < 0 ? -0.25 : 0.25)) * 360.0 / 4294967295.0)); break;
	case DefEncoding::HuskSeconds: args.push_back(decimal(f(0) / 62.0)); break;
	case DefEncoding::HuskSwap: {
		const auto &item = *static_cast<const DefItemDef *>(value);
		args.push_back(decimal(item.husk_swap_at_sec == 0 ? f(0) * 100.0 : f(0) / 62.0)); break;
	}
	case DefEncoding::DeathPieces:
		for (size_t i = 0; i < values.size(); ++i) if (n(i)) {
			const char *piece = defscan::death_piece_keyword(size_t(n(i)));
			if (!piece) fail(name, key, "Unknown debris type.");
			else args.push_back(std::to_string(i + 1) + "_" + piece);
		}
		break;
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

} // namespace opennova::def
