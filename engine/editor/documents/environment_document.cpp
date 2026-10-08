// The environment document (environment_document.h; the deep-integration plan's DI-19a): a `.env`
// over the engine's own reader and writer (formats/env, docs/env/env-tod-re.md), its fields the
// keywords [orig: TimeOfDay_ParseProperty @ 0x57c590] reads, in the units the file writes them.
#include <editor/documents/environment_document.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <functional>
#include <map>
#include <sstream>
#include <utility>

#include <base/io/ascii_config.h>
#include <base/io/cp1252.h>
#include <base/io/crt_ftol.h>
#include <base/io/strutil.h>
#include <editor/assets/asset_kinds.h>
#include <editor/documents/source_issue_findings.h>
#include <editor/documents/texture_roles.h>
#include <editor/model/staged_rows.h>
#include <formats/trn/trn_io.h>

namespace opennova::editor {

namespace {

using env::Config;
using env::Keyframe;
using env::Rgb;

constexpr NodeKind kEnvironment = node_kind(EnvironmentKind::Environment);
constexpr NodeKind kKeyframe = node_kind(EnvironmentKind::Keyframe);

EnvironmentRow &row_of(const RecordHandle &record) { return record.as<EnvironmentRow>(); }
Config &config_of(const RecordHandle &record) { return row_of(record).config; }
Keyframe &keyframe_of(const RecordHandle &record) { return record.as<Keyframe>(); }

// --- values -------------------------------------------------------------------------------------

// A whole number from a Value: a whole number, or a real holding one (the wire's numbers).
bool whole_of(const Value &value, int64_t &out, std::string &error) {
	if (const auto *whole = std::get_if<int64_t>(&value)) {
		out = *whole;
		return true;
	}
	if (const auto *real = std::get_if<double>(&value); real && std::isfinite(*real) && std::floor(*real) == *real &&
	                                                    std::fabs(*real) < 9.0e15) {
		out = static_cast<int64_t>(*real);
		return true;
	}
	error = "A whole number.";
	return false;
}

bool in_range(int64_t value, int64_t min, int64_t max, const char *words, std::string &error) {
	if (value >= min && value <= max) return true;
	error = words;
	return false;
}

bool real_of(const Value &value, double &out, std::string &error) {
	if (const auto *real = std::get_if<double>(&value)) out = *real;
	else if (const auto *whole = std::get_if<int64_t>(&value)) out = static_cast<double>(*whole);
	else {
		error = "A number.";
		return false;
	}
	if (!std::isfinite(out)) {
		error = "A finite number.";
		return false;
	}
	return true;
}

// A colour channel's byte as the record holds it (0..1 floats of whole bytes, env.h).
int64_t byte_of(float channel) {
	const int byte = static_cast<int>(channel * 255.0f + 0.5f);
	return byte < 0 ? 0 : byte > 255 ? 255 : byte;
}

// An HHMM time the game's clock can read back as written: hours 0 to 23, minutes 0 to 59
// [orig: Environment_ParseTimeString @ 0x57c500 clamps hours to 23 and minutes to 59].
bool hhmm_of(const Value &value, int &out, std::string &error) {
	int64_t whole = 0;
	if (!whole_of(value, whole, error)) return false;
	if (whole < 0 || whole > env::kTodTimeMax || whole % 100 > 59) {
		error = "A time as HHMM: hours 00 to 23, minutes 00 to 59 (1230 is 12:30).";
		return false;
	}
	out = static_cast<int>(whole);
	return true;
}

std::string clock_words(int hhmm) {
	char text[8];
	std::snprintf(text, sizeof(text), "%02d:%02d", hhmm / 100, hhmm % 100);
	return text;
}

// --- the fields -----------------------------------------------------------------------------------

FieldSchema schema(const std::string &id, FieldType type, const char *label, const char *section,
                   const char *description, const char *token = "") {
	FieldSchema out;
	out.id = id;
	out.type = type;
	out.label = label;
	out.section = section;
	out.description = description;
	out.token = token;
	return out;
}

void ranged(FieldSchema &field, double min, double max) {
	field.ranged = true;
	field.min = min;
	field.max = max;
}

std::vector<FieldChoice> choices(std::initializer_list<FieldChoice> rows) { return std::vector<FieldChoice>(rows); }

using RgbAt = std::function<Rgb &(const RecordHandle &)>;

// A colour line's three bytes, one field each on one row (its `group`), the file's keyword their
// token: what the parser reads as atol of the line's three values [orig: TimeOfDay_ParseProperty
// @ 0x57c590, j__atol of tokens[2..4] in every *_rgb arm].
void colour(TableKind &kind, const std::string &key, const std::string &prefix, const char *group,
            const char *section, const char *description, const RgbAt &at,
            const LabelledField::Decides &applies = nullptr) {
	static const char *const kChannels[] = {"r", "g", "b"};
	static const char *const kLabels[] = {"Red", "Green", "Blue"};
	for (int c = 0; c < 3; ++c) {
		LabelledField field;
		field.schema = schema(prefix + key + "_" + kChannels[c], FieldType::Byte, kLabels[c], section, description,
		                      "");
		field.schema.token = key;
		field.schema.group = group;
		field.schema.color = FieldColor::Channel;
		ranged(field.schema, 0, 255);
		field.value.get = [at, c](const RecordHandle &record, Value &out) {
			const Rgb &rgb = at(record);
			out = byte_of(c == 0 ? rgb.r : c == 1 ? rgb.g : rgb.b);
			return true;
		};
		field.value.set = [at, c](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t byte = 0;
			if (!whole_of(value, byte, error) || !in_range(byte, 0, 255, "A colour's byte: 0 to 255.", error)) return false;
			Rgb &rgb = at(record);
			(c == 0 ? rgb.r : c == 1 ? rgb.g : rgb.b) = static_cast<float>(byte) / 255.0f;
			return true;
		};
		field.applies = applies;
		kind.field(std::move(field));
	}
}

// A text a line carries as the game reads it: in its code page (Windows-1252), the editor's words
// UTF-8; no '"' and no control character, which no line carries.
bool line_text_of(const Value &value, std::string &stored, std::string &error) {
	const auto *text = std::get_if<std::string>(&value);
	if (!text) {
		error = "A name.";
		return false;
	}
	if (!utf8_to_cp1252(*text, stored)) {
		error = "The game's text encoding (Windows-1252) has no character for part of this name.";
		return false;
	}
	if (!env::env_name_writable(stored)) {
		error = "A name holds no '\"' and no control character: the game's reader cannot read one.";
		return false;
	}
	return true;
}

// A name the parser copies whole (a sky map, a model, the time of day's word).
LabelledField name_field(const char *id, const char *label, const char *section, const char *description,
                         std::string Config::*member, ReferenceKind reference) {
	LabelledField field;
	field.schema = schema(id, FieldType::Text, label, section, description);
	field.schema.reference = reference;
	field.schema.code_page = true;
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = cp1252_to_utf8(config_of(record).*member);
		return true;
	};
	field.value.set = [member](const RecordHandle &record, const Value &value, std::string &error) {
		std::string stored;
		if (!line_text_of(value, stored, error)) return false;
		config_of(record).*member = std::move(stored);
		return true;
	};
	return field;
}

// The whole numbers an atol keyword holds once the parser shifts it into the engine's 32-bit store:
// fog_level and sky_height `<< 16` [orig: TimeOfDay_ParseProperty @ 0x57ccbf, @ 0x57cbdf],
// water_height `<< 15` [@ 0x57cb6a], sky_speed `<< 10` [@ 0x57cc0d]. Past them the shift wraps.
constexpr int kShift16 = 16, kShift15 = 15, kShift10 = 10;
constexpr int64_t stored_min(int shift) { return int64_t(INT32_MIN) >> shift; }
constexpr int64_t stored_max(int shift) { return int64_t(INT32_MAX) >> shift; }
// "-32768 to 32767 m: <why>": a refusal's words for a stored range.
std::string stored_range_words(int shift, const char *unit, const char *why) {
	return std::to_string(stored_min(shift)) + " to " + std::to_string(stored_max(shift)) + unit + ": " + why;
}

// A keyword the parser reads with atol, held as the Config's float (a fog distance, a dome height,
// a cloud speed, a water height), in the whole units the file writes, within the range the engine's
// store holds once the parser shifts it by `shift`.
LabelledField whole_field(const char *id, const char *label, const char *section, const char *unit,
                          const char *description, float Config::*member, int shift, const char *why) {
	LabelledField field;
	field.schema = schema(id, FieldType::Integer, label, section, description);
	field.schema.unit = unit;
	const int64_t min = stored_min(shift), max = stored_max(shift);
	ranged(field.schema, double(min), double(max));
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = static_cast<int64_t>(std::trunc(config_of(record).*member));
		return true;
	};
	const std::string words = stored_range_words(shift, *unit ? (std::string(" ") + unit).c_str() : "", why);
	field.value.set = [member, min, max, words](const RecordHandle &record, const Value &value, std::string &error) {
		int64_t whole = 0;
		if (!whole_of(value, whole, error) || !in_range(whole, min, max, words.c_str(), error)) return false;
		config_of(record).*member = static_cast<float>(whole);
		return true;
	};
	return field;
}

// A keyword the parser reads with atof, held as a float.
LabelledField real_field(const char *id, const char *label, const char *section, const char *unit,
                         const char *description, float Config::*member) {
	LabelledField field;
	field.schema = schema(id, FieldType::Real, label, section, description);
	field.schema.unit = unit;
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = static_cast<double>(config_of(record).*member);
		return true;
	};
	field.value.set = [member](const RecordHandle &record, const Value &value, std::string &error) {
		double real = 0.0;
		if (!real_of(value, real, error)) return false;
		config_of(record).*member = static_cast<float>(real);
		return true;
	};
	return field;
}

// A keyword the parser reads with atol into an int.
LabelledField int_field(const char *id, const char *label, const char *section, const char *description,
                        int Config::*member) {
	LabelledField field;
	field.schema = schema(id, FieldType::Integer, label, section, description);
	field.value.get = [member](const RecordHandle &record, Value &out) {
		out = static_cast<int64_t>(config_of(record).*member);
		return true;
	};
	field.value.set = [member](const RecordHandle &record, const Value &value, std::string &error) {
		int64_t whole = 0;
		if (!whole_of(value, whole, error) ||
		    !in_range(whole, INT32_MIN, INT32_MAX, "A whole number the game's 32-bit reader holds.", error))
			return false;
		config_of(record).*member = static_cast<int>(whole);
		return true;
	};
	return field;
}

// The twelve time-of-day colours a keyframe holds, in the order its block writes them, each with
// its row's words: what the game's time-of-day pass lerps them into [orig:
// Environment_ComputeTimeOfDayColors @ 0x57de40], and what reads each (env-honored-matrix.md).
struct TodColour {
	const char *key;
	Rgb Keyframe::*member;
	const char *group;
	const char *section;
	const char *description;
};
const TodColour kTodColours[] = {
	{"sun_rgb", &Keyframe::sun, "Sun", "Light",
	 "The sunlight's colour by day (from 06:00 to 18:45 the game lights the world with the sun, else with the moon)."},
	{"moon_rgb", &Keyframe::moon, "Moon", "Light", "The moonlight's colour by night."},
	{"sky_rgb", &Keyframe::sky, "Sky", "Light", "The light from the sky above: the ambient light from overhead."},
	{"ground_rgb", &Keyframe::ground, "Ground", "Light",
	 "The light from the ground below (the game reads ambient_rgb as this too)."},
	{"skyfog_rgb", &Keyframe::skyfog, "Sky fog", "Fog",
	 "The horizon's haze the frame clears to above the water; a fog line sets it too until a skyfog line does."},
	{"fog_rgb", &Keyframe::fog, "Fog", "Fog", "The distance fog's colour over the world (the game doubles it)."},
	{"skybase_rgb", &Keyframe::skybase, "Sky base", "Sky dome", "The sky dome's colour away from the sun."},
	{"skybright_rgb", &Keyframe::skybright, "Sky bright", "Sky dome", "The sky dome's colour toward the sun."},
	{"skyhighlight_rgb", &Keyframe::skyhighlight, "Sky highlight", "Sky dome",
	 "The sky dome's glow close around the sun."},
	{"cloudbase_rgb", &Keyframe::cloudbase, "Cloud base", "Clouds", "The clouds' colour where they are thick."},
	{"cloudhighlight_rgb", &Keyframe::cloudhighlight, "Cloud highlight", "Clouds",
	 "The clouds' colour where the sun lights them."},
	{"cloudedge_rgb", &Keyframe::cloudedge, "Cloud edge", "Clouds", "The clouds' colour at their thin edges."},
};

void environment_fields(TableKind &kind) {
	// --- the clock [orig: TimeOfDay_ParseProperty @ 0x57cdb9 (timeofday), @ 0x57d0b6 (curtime),
	// @ 0x57d0e4 (tod_rate), @ 0x57c5c3 (envscale)]
	{
		LabelledField field = name_field("timeofday", "Time of day", "Time of day",
		                                 "The kind of day the game files it as (dawn, day, dusk or night, compared without "
		                                 "case); the colours never read it.",
		                                 &Config::timeofday, ReferenceKind::None);
		field.schema.choices = choices({{"Dawn", 1, ""}, {"Day", 2, ""}, {"Dusk", 3, ""}, {"Night", 0, ""}});
		field.schema.open_choices = true;
		kind.field(std::move(field));
	}
	{
		LabelledField field;
		field.schema = schema("curtime", FieldType::Integer, "Clock time", "Time of day",
		                      "The clock's time as HHMM. Every mission sets its own start time over it as it starts (its "
		                      "header's start time, or the server's in a network game), so no mission starts at this one.");
		field.schema.unit = "HHMM";
		ranged(field.schema, 0, env::kTodTimeMax);
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(config_of(record).curtime);
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			int time = 0;
			if (!hhmm_of(value, time, error)) return false;
			config_of(record).curtime = time;
			return true;
		};
		kind.field(std::move(field));
	}
	{
		LabelledField field;
		field.schema = schema("tod_rate", FieldType::Integer, "Day length", "Time of day",
		                      "How long a whole day lasts, in real minutes (at least 60). Every mission sets its own over "
		                      "it as it starts (its header's minutes per day), so no mission runs on this one.");
		field.schema.unit = "min";
		field.schema.optional = true;
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(config_of(record).tod_rate);
			return true;
		};
		field.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t whole = 0;
			if (!whole_of(value, whole, error) ||
			    !in_range(whole, INT32_MIN, INT32_MAX, "A whole number the game's 32-bit reader holds.", error))
				return false;
			Config &config = config_of(record);
			if (config.tod_rate != whole) config.tod_rate_set = true;
			config.tod_rate = static_cast<int>(whole);
			return true;
		};
		field.value.present = [](const RecordHandle &record) { return config_of(record).tod_rate_set; };
		field.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			config_of(record).tod_rate_set = present;
			return true;
		};
		kind.field(std::move(field));
	}
	kind.field(real_field("envscale", "Colour scale", "Time of day", "",
	                      "Scales every colour line after it but the terrain's: each byte times this, cut to a whole "
	                      "byte and held at 255.",
	                      &Config::envscale));

	// --- the sky [orig: TimeOfDay_ParseProperty @ 0x57cc1b (sky_map1, made .pcx @ 0x57cc4b), @ 0x57cc5d
	// (sky_map2), @ 0x57cbc3 (sky_height << 16), @ 0x57cbf1 (sky_speed << 10), @ 0x57ce63 (advanced_clouds),
	// @ 0x57c856 (cloud_rgb)]
	kind.field(name_field("sky_map1", "Cloud layer 1", "Sky",
	                      "The near cloud layer's texture. The game reads the name as a .pcx whatever its extension.",
	                      &Config::sky_map1, ReferenceKind::Texture));
	kind.field(name_field("sky_map2", "Cloud layer 2", "Sky",
	                      "The far cloud layer's texture. The game reads the name as a .pcx whatever its extension.",
	                      &Config::sky_map2, ReferenceKind::Texture));
	{
		LabelledField field;
		field.schema = schema("sky_height", FieldType::Integer, "Dome height", "Sky",
		                      "How high the sky dome stands. Left out, the game keeps its own default of 200/65536 of a "
		                      "metre: a dome flat on the camera.");
		field.schema.unit = "m";
		field.schema.optional = true;
		ranged(field.schema, double(stored_min(kShift16)), double(stored_max(kShift16)));
		field.value.get = [](const RecordHandle &record, Value &out) {
			out = static_cast<int64_t>(row_of(record).sky_height_latent);
			return true;
		};
		const std::string words = stored_range_words(kShift16, " m", "the game keeps the height in 16.16.");
		field.value.set = [words](const RecordHandle &record, const Value &value, std::string &error) {
			int64_t whole = 0;
			if (!whole_of(value, whole, error) ||
			    !in_range(whole, stored_min(kShift16), stored_max(kShift16), words.c_str(), error))
				return false;
			EnvironmentRow &row = row_of(record);
			// A Set of another value writes the line; one of the value it holds changes nothing.
			if (whole == row.sky_height_latent && !row.sky_height_written()) return true;
			row.sky_height_latent = static_cast<int>(whole);
			row.config.sky_height = static_cast<float>(whole);
			return true;
		};
		field.value.present = [](const RecordHandle &record) { return row_of(record).sky_height_written(); };
		field.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			EnvironmentRow &row = row_of(record);
			row.config.sky_height = present ? static_cast<float>(row.sky_height_latent) : Config().sky_height;
			return true;
		};
		kind.field(std::move(field));
	}
	kind.field(whole_field("sky_speed", "Cloud speed", "Sky", "",
	                       "How fast the clouds drift: the game adds it (x 1024) to the cloud layers' offsets every "
	                       "tick, the layers at 1, 1, 2/3 and 4/3 of it.",
	                       &Config::sky_speed, kShift10, "the game keeps the speed times 1024 in 32 bits."));
	{
		LabelledField field = int_field("advanced_clouds", "Cloud pass", "Sky",
		                                "Flat (0): the dome is one pass in the cloud colour below, no texture. Layered (any "
		                                "other): the keyframes' sky dome and cloud colours over the two cloud layers.",
		                                &Config::advanced_clouds);
		field.schema.choices = choices({{"0", 0, "Flat"}, {"1", 1, "Layered"}});
		field.schema.open_choices = true;
		kind.field(std::move(field));
	}
	colour(kind, "cloud_rgb", "", "Cloud colour", "Sky", "The dome's one colour when the cloud pass is flat.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).cloud_rgb; });

	// --- the sun and the moon [orig: TimeOfDay_ParseProperty @ 0x57ccfc (sun_3di), @ 0x57cd2c, @ 0x57cd5c,
	// @ 0x57cd8c; EffectWorld_LoadCelestialModels @ 0x5adc50; Star_RenderField_Unused @ 0x5ad9c0 has no
	// caller]
	kind.field(name_field("sun_3di", "Sun", "Sun and moon", "The sun's model, drawn where the clock puts the sun.",
	                      &Config::sun_3di, ReferenceKind::Model));
	kind.field(name_field("moon_3di", "Moon", "Sun and moon", "The moon's model, drawn opposite the sun.",
	                      &Config::moon_3di, ReferenceKind::Model));
	kind.field(name_field("glare_3di", "Glare", "Sun and moon",
	                      "The sun's glare model, drawn over the scene while the sun is in sight.", &Config::glare_3di,
	                      ReferenceKind::Model));
	kind.field(name_field("star_3di", "Stars", "Sun and moon",
	                      "The stars' model: the game loads it but never draws it.", &Config::star_3di,
	                      ReferenceKind::Model));

	// --- the fog [orig: TimeOfDay_ParseProperty @ 0x57cca3 (fog_level << 16), @ 0x57ccd1 (fog_type);
	// Render_SetFogState @ 0x58a950]
	kind.field(whole_field("fog_level", "Fog distance", "Fog", "m",
	                       "Where the fog is whole: nothing past it is drawn. Overcast weather brings it in by up to half.",
	                       &Config::fog_level, kShift16, "the game keeps the distance in 16.16."));
	{
		LabelledField field = int_field("fog_type", "Fog type", "Fog",
		                                "How the fog thickens toward its distance. Any other number fogs as 1 does.",
		                                &Config::fog_type);
		field.schema.choices = choices({{"0", 0, "Exponential"},
		                                {"1", 1, "Linear from the eye"},
		                                {"2", 2, "Linear from half the distance"},
		                                {"3", 3, "Linear from a quarter of the distance"}});
		field.schema.open_choices = true;
		kind.field(std::move(field));
	}

	// --- the water [orig: TimeOfDay_ParseProperty @ 0x57caf6 (water_rgb), @ 0x57cb4e (water_height << 15),
	// @ 0x57cb7c (water_murk, held at 0.99 from above)]
	colour(kind, "water_rgb", "", "Water colour", "Water", "The water's colour, lit by the sun and the sky.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).water_rgb; });
	{
		LabelledField field = whole_field(
				"water_height", "Water height", "Water", "half m",
				"The water plane's height in half metres (60 is 30 m). A mission's header or its terrain's water height "
				"comes before it.",
				&Config::water_height, kShift15, "the game keeps half metres in 16.16.");
		field.schema.optional = true;
		const auto set = field.value.set;
		field.value.set = [set](const RecordHandle &record, const Value &value, std::string &error) {
			Config &config = config_of(record);
			const float before = config.water_height;
			if (!set(record, value, error)) return false;
			if (config.water_height != before) config.water_height_set = true;
			return true;
		};
		field.value.present = [](const RecordHandle &record) { return config_of(record).water_height_set; };
		field.value.set_present = [](const RecordHandle &record, bool present, std::string &) {
			config_of(record).water_height_set = present;
			return true;
		};
		kind.field(std::move(field));
	}
	{
		LabelledField field = real_field("water_murk", "Water murk", "Water", "",
		                                 "How murky the water is, at most 0.99: how far the eye sees under it, and how "
		                                 "thick its surface reads.",
		                                 &Config::water_murk);
		const auto set = field.value.set;
		field.value.set = [set](const RecordHandle &record, const Value &value, std::string &error) {
			double real = 0.0;
			if (!real_of(value, real, error)) return false;
			if (static_cast<float>(real) > 0.99f) {
				error = "At most 0.99: the game reads a larger murk as 0.99.";
				return false;
			}
			return set(record, value, error);
		};
		kind.field(std::move(field));
	}

	// --- the light [orig: TimeOfDay_ParseProperty @ 0x57c5ef (iris_percent), @ 0x57c61b (iris_center),
	// @ 0x57c7fe (lightning_rgb), @ 0x57c8ae (ceiling_rgb), @ 0x57c906 (floor_rgb), @ 0x57ca3a (terrain_rgb,
	// never scaled); the iris curve, Terrain_SectorComputeLighting @ 0x5c7550]
	kind.field(real_field("iris_percent", "Iris strength", "Light", "%",
	                      "How strongly the eye adjusts to the scene's brightness: 0 never, 100 fully.",
	                      &Config::iris_percent));
	kind.field(real_field("iris_center", "Iris centre", "Light", "",
	                      "The exposure the eye's adjustment works around: 1 leaves the colours as they are.",
	                      &Config::iris_center));
	colour(kind, "lightning_rgb", "", "Lightning", "Light", "The flash a lightning strike adds to the sky, fog and ground.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).lightning_rgb; });
	colour(kind, "ceiling_rgb", "", "Ceiling", "Light", "Indoors, the light from above in place of the sky's.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).ceiling_rgb; });
	colour(kind, "floor_rgb", "", "Floor", "Light", "Indoors, the light from below in place of the ground's.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).floor_rgb; });
	colour(kind, "terrain_rgb", "", "Terrain tint", "Light",
	       "Tints the terrain's placed tiles (half of it, doubled); effects are lit against it. The colour scale does "
	       "not touch it.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).terrain_rgb; });

	// --- the colours the game runs on with no keyframe: the colour lines outside every block land on
	// the scratch keyframe, packed with the envscale read before them, and become the colour blocks'
	// targets; with a keyframe the time-of-day pass writes every target each tick [orig:
	// Environment_LoadTimeOfDayConfig @ 0x57dce0..0x57dd57; Environment_ComputeTimeOfDayColors @ 0x57de40,
	// the count gate @ 0x57de8a].
	const LabelledField::Decides without_keyframes = [](const RecordHandle &record, const RecordOwners &) {
		return config_of(record).keyframes.empty() ? Applicability::Reads : Applicability::Ignored;
	};
	for (const TodColour &tod : kTodColours) {
		Rgb Keyframe::*member = tod.member;
		colour(kind, tod.key, "static_", tod.group, "Colours with no keyframe",
		       "What the game runs on where the file has no keyframe (with one, the keyframes' colours replace it "
		       "every tick). Written outside every tod_begin block, already scaled.",
		       [member](const RecordHandle &record) -> Rgb & {
			       return config_of(record).scratch.*member;
		       },
		       without_keyframes);
	}

	// --- read and kept, not the game's [orig: TimeOfDay_ParseProperty @ 0x57c590 has no enviro_name or
	// vertex_rgb arm]
	{
		LabelledField field = name_field("enviro_name", "Name", "Kept for the editor",
		                                 "A name for the environment, kept in its file: the game skips the line.",
		                                 &Config::name, ReferenceKind::None);
		field.schema.applies = Applicability::Ignored;
		kind.field(std::move(field));
	}
	colour(kind, "vertex_rgb", "", "Vertex tint", "Kept for the editor",
	       "Kept from earlier games' files: Joint Operations skips the line.",
	       [](const RecordHandle &record) -> Rgb & { return config_of(record).vertex_rgb; },
	       [](const RecordHandle &, const RecordOwners &) { return Applicability::Ignored; });
}

void keyframe_fields(TableKind &kind) {
	// [orig: TimeOfDay_ParseProperty @ 0x57c647, the time through Environment_ParseTimeString @ 0x57c500]
	LabelledField time;
	time.schema = schema("time", FieldType::Integer, "Time", "",
	                     "The clock time the keyframe's colours are reached at, as HHMM; between two keyframes the game "
	                     "blends their colours, and past the last toward the first.",
	                     "tod_begin");
	time.schema.unit = "HHMM";
	ranged(time.schema, 0, env::kTodTimeMax);
	time.value.get = [](const RecordHandle &record, Value &out) {
		out = static_cast<int64_t>(keyframe_of(record).time);
		return true;
	};
	time.value.set = [](const RecordHandle &record, const Value &value, std::string &error) {
		int hhmm = 0;
		if (!hhmm_of(value, hhmm, error)) return false;
		keyframe_of(record).time = hhmm;
		return true;
	};
	kind.field(std::move(time));
	for (const TodColour &tod : kTodColours) {
		Rgb Keyframe::*member = tod.member;
		colour(kind, tod.key, "", tod.group, tod.section, tod.description,
		       [member](const RecordHandle &record) -> Rgb & { return keyframe_of(record).*member; });
	}
}

// A new keyframe at `index`: between its neighbours' times (an hour after the one before, an hour
// before the one after; noon in an empty list), with the colours of the keyframe before it (else the
// one after, else the colours with no keyframe).
int minutes_of(int hhmm) { return hhmm / 100 * 60 + hhmm % 100; }
int hhmm_of_minutes(int minutes) {
	minutes = std::max(0, std::min(23 * 60 + 59, minutes));
	return minutes / 60 * 100 + minutes % 60;
}

Keyframe fresh_keyframe(const EnvironmentRow &row, size_t index) {
	const std::vector<Keyframe> &list = row.config.keyframes;
	const Keyframe *before = index > 0 && index <= list.size() ? &list[index - 1] : nullptr;
	const Keyframe *after = index < list.size() ? &list[index] : nullptr;
	Keyframe made = before ? *before : after ? *after : row.config.scratch;
	if (before && after) made.time = hhmm_of_minutes((minutes_of(before->time) + minutes_of(after->time)) / 2);
	else if (before) made.time = hhmm_of_minutes(minutes_of(before->time) + 60);
	else if (after) made.time = hhmm_of_minutes(minutes_of(after->time) - 60);
	else made.time = 1200;
	return made;
}

RecordTable make_table() {
	TableKind environment(RecordKindRow{kEnvironment, "environment", "Environment", "", true});
	environment_fields(environment);
	TableList keyframes;
	keyframes.spec.kind = kKeyframe;
	keyframes.spec.label = "Keyframes";
	keyframes.spec.max = size_t(env::kMaxTodKeyframes);
	keyframes.ops = vector_list<EnvironmentRow, Keyframe>(
			kKeyframe, [](EnvironmentRow &row) -> std::vector<Keyframe> & { return row.config.keyframes; }, fresh_keyframe);
	// The parser keeps 16 keyframes [orig: TimeOfDay_ParseProperty @ 0x57c65b]: the list takes no 17th.
	const auto insert = keyframes.ops.insert;
	keyframes.ops.insert = [insert](const RecordHandle &owner, size_t index, const DetachedRecord *record,
	                                std::string &error) {
		if (row_of(owner).config.keyframes.size() >= size_t(env::kMaxTodKeyframes)) {
			error = "The game reads 16 keyframes at most.";
			return false;
		}
		return insert(owner, index, record, error);
	};
	environment.list(std::move(keyframes));
	TableKind keyframe(RecordKindRow{kKeyframe, "keyframe", "Keyframe", "", false});
	keyframe_fields(keyframe);
	return RecordTable({std::move(environment), std::move(keyframe)});
}

// --- the source issues ------------------------------------------------------------------------------

// The keywords the terrain's reader reads (formats/trn's reader, trn_parser_key): its hook stays on for the .env
// pass, so it reads an environment's lines too, and the mission's terrain takes them after its .trn's
// (D-TERRAIN-18) [orig: Terrain_LoadEnvironmentConfig @ 0x6109AD pushes Terrain_ParseConfigCallback;
// Environment_LoadTimeOfDayConfig @ 0x57DB44 keeps it as g_EnvParseHook for every pass]. terrain_name and horizon
// are read by no arm of either reader: a line the game skips.
bool terrain_key(const std::string &key) { return trn_parser_key(key); }

} // namespace

void environment_source_issues(const std::string &text, std::vector<SourceIssue> &issues) {
	// The walk the reader takes (io::for_each_config_line_span: its lines and tokens, the callback's gate),
	// numbering every line it cuts.
	int keyframes = 0;
	bool on_slot = false;
	float envscale = 1.0f;
	// The colour lines a later envscale would scale otherwise than the record does (env.h divergence
	// #8): a keyframe's and the scaled global colours', each with the envscale it was read under.
	struct Scaled {
		size_t line;
		std::string key;
		float envscale;
	};
	std::vector<Scaled> scaled;
	std::map<std::string, size_t> first_line; // a keyword outside the blocks: where it was first read
	const auto issue = [&](bool blocks, size_t line, const std::string &field, const std::string &message) {
		SourceIssue out;
		out.blocks = blocks;
		out.line = line;
		out.field = field;
		out.message = message;
		issues.push_back(std::move(out));
	};
	size_t line = 0;
	// A last line no CR LF ends loses its final byte to the walk [orig: File_ParseASCIIFile
	// @ 0x53D8C7..0x53D8F5] (shipped FULL_03.ENV and FULL_05.ENV end on such a "tod_end").
	const bool ends_cut = text.size() < 2 || text.compare(text.size() - 2, 2, "\r\n") != 0;
	io::for_each_config_line_span(text.data(), text.size(), [&](io::ConfigTokens &tokens, const io::ConfigLineSpan &span) {
		++line;
		if (tokens.count == 0 || tokens.tokens[0][0] == '/') return;
		const std::string key = strutil::to_lower(tokens.tokens[0]);
		const char *value = tokens.token(1);
		const bool last_cut = ends_cut && !text.empty() && span.end + 1 >= text.size();
		if (last_cut)
			issue(false, line, key,
			      "The file's last line has no line end, so the game's reader loses its last character ('" +
			              std::string(1, text.back()) + "') and reads the line short: a save writes the line as the "
			              "game read it, ended.");
		if (!env::is_env_key(key)) {
			if (terrain_key(key)) {
				issue(true, line, key,
				      "'" + std::string(tokens.tokens[0]) +
				              "' is a terrain keyword: the game's terrain reader reads the environment's lines too and "
				              "takes it for the mission's terrain, which the editor's environment cannot keep. Move it to "
				              "the terrain's .trn.");
			} else {
				issue(false, line, key,
				      "The game's environment reader skips '" + std::string(tokens.tokens[0]) +
				              "': a save leaves the line out.");
			}
			return;
		}
		if (key == "tod_begin") {
			if (keyframes >= env::kMaxTodKeyframes) {
				issue(false, line, "time",
				      std::string("The game reads 16 keyframes: this tod_begin takes no slot and its time is not read, so "
				                  "its colours land on ") +
				              (on_slot ? "the 16th keyframe." : "the colours with no keyframe.") +
				              " A save writes them there.");
				return;
			}
			++keyframes;
			on_slot = true;
		} else if (key == "tod_end") {
			on_slot = false;
			return;
		}
		// A time that reads as another clock time (a short token, an hour past 23, a minute past 59).
		if (key == "tod_begin" || key == "curtime") {
			const int read = env::parse_tod_time(value);
			if (read != io::retail_atol(value))
				issue(false, line, key == "curtime" ? "curtime" : "time",
				      "The game reads '" + std::string(value) + "' as " + clock_words(read) + ": a save writes " +
				              clock_words(read).substr(0, 2) + clock_words(read).substr(3) + ".");
			return;
		}
		const bool colour_line = key.size() > 4 && key.compare(key.size() - 4, 4, "_rgb") == 0;
		if (colour_line && tokens.count < 4)
			issue(false, line, key,
			      "This colour line has " + std::to_string(tokens.count - 1) +
			              " of its three values: the game reads the missing ones from where earlier, longer lines left "
			              "them, and a save writes the three it read.");
		if (key == "envscale") envscale = static_cast<float>(io::retail_atof(value));
		// The scaled colours the record scales by the last envscale: a keyframe's, and the global ones.
		if (env::is_envscaled_key(key) && (on_slot || !env::is_tod_color_key(key)))
			scaled.push_back({line, key, envscale});
		// A keyword read again outside the blocks: the last line wins.
		if (!on_slot) {
			const auto first = first_line.emplace(key, line);
			if (!first.second) {
				issue(false, first.first->second, key,
				      "'" + key + "' is written again on line " + std::to_string(line) +
				              ": the game reads the last, and a save writes that one alone.");
				first.first->second = line;
			}
		}
	});
	for (const Scaled &each : scaled)
		if (each.envscale != envscale) {
			issue(true, each.line, each.key,
			      "An envscale after this colour line scales it otherwise: the game scales each colour line by the "
			      "envscale read before it, and the editor's environment holds one for every colour. Move the envscale "
			      "line above the colours.");
			break;
		}
	std::stable_sort(issues.begin(), issues.end(), [](const SourceIssue &a, const SourceIssue &b) { return a.line < b.line; });
}

// --- the row --------------------------------------------------------------------------------------

EnvironmentRow::EnvironmentRow() { kind = kEnvironment; }

RecordHandle EnvironmentRow::record() const { return {kEnvironment, const_cast<EnvironmentRow *>(this)}; }

size_t EnvironmentRow::footprint() const {
	return sizeof(*this) + footprint_of(config.name) + footprint_of(config.timeofday) + footprint_of(config.sky_map1) +
	       footprint_of(config.sky_map2) + footprint_of(config.sun_3di) + footprint_of(config.moon_3di) +
	       footprint_of(config.glare_3di) + footprint_of(config.star_3di) + footprint_of(config.keyframes) +
	       ids_footprint();
}

bool EnvironmentRow::sky_height_written() const { return config.sky_height != Config().sky_height; }

const RecordTable &environment_table() {
	static const RecordTable table = make_table();
	return table;
}

bool is_environment_kind(AssetKind kind) { return asset_kind_row(kind).document == DocumentTypeId::Environment; }

// --- the document ---------------------------------------------------------------------------------

const EnvironmentRow *EnvironmentDocument::environment_row() const {
	for (const auto &row : rows())
		if (row && row->kind == kEnvironment) return static_cast<const EnvironmentRow *>(row.get());
	return nullptr;
}

const env::Config *EnvironmentDocument::config() const {
	const EnvironmentRow *row = environment_row();
	return row ? &row->config : nullptr;
}

std::string EnvironmentDocument::record_title(const NodeAddress &address) const {
	if (address.child && address.kind == kKeyframe) {
		Value time;
		if (get(address, "time", time))
			if (const auto *hhmm = std::get_if<int64_t>(&time)) return "Keyframe " + clock_words(int(*hhmm));
	}
	return Document::record_title(address);
}

void EnvironmentDocument::refine_field(const NodeAddress &address, FieldUse &use) const {
	TableDocument::refine_field(address, use);
	// The cloud layers through ARCHIVE [orig: Terrain_InitRenderingResources @ 0x578A97], each name made
	// .pcx as the parser stores it [orig: TimeOfDay_ParseProperty @ 0x57CC41..0x57CC4B, sky_map2's
	// @ 0x57CC83..0x57CC8D] (kTextureArgPcx).
	if (use.reference == ReferenceKind::Texture && (use.schema->id == "sky_map1" || use.schema->id == "sky_map2"))
		use.loader_arg = texture_role_arg(TextureRoleId::SkyCloud, kTextureArgPcx);
}

bool EnvironmentDocument::parse(const std::vector<uint8_t> &bytes, std::vector<std::shared_ptr<Node>> &rows,
                                std::shared_ptr<const FileState> &, std::vector<SourceIssue> &issues,
                                Diagnostic &error) {
	if (!is_environment_kind(kind())) {
		error = make_finding(CoreFinding::DocumentKind, DiagnosticSeverity::Error, "This file is not an environment.",
		                     path());
		return false;
	}
	const std::string text(bytes.begin(), bytes.end());
	auto row = std::make_shared<EnvironmentRow>();
	std::istringstream input(text);
	std::string message;
	if (!env::load_env(input, row->config, message)) {
		error = make_finding(CoreFinding::DocumentParse, DiagnosticSeverity::Error,
		                     "The environment could not be read: " + message, path());
		return false;
	}
	row->sky_height_latent = row->sky_height_written() ? static_cast<int>(row->config.sky_height) : 0;
	environment_source_issues(text, issues);
	shape(*row);
	rows.push_back(row);
	return true;
}

SerializeResult EnvironmentDocument::serialize() const {
	SerializeResult result;
	const env::Config *held = config();
	if (!held) {
		result.issues.push_back({true, 0, "", "", "The environment holds no row."});
		return result;
	}
	std::ostringstream output;
	std::string error;
	if (!env::save_env(output, *held, error)) {
		result.issues.push_back({true, 0, "", "", error});
		return result;
	}
	result.text = output.str();
	// The game sorts the keyframes by time as it reads them [orig: Environment_SortAndSnapshotKeyframes
	// @ 0x57c240], and the writer writes them so.
	if (!std::is_sorted(held->keyframes.begin(), held->keyframes.end(),
	                    [](const Keyframe &a, const Keyframe &b) { return a.time < b.time; }))
		result.notes.push_back("The keyframes are written in time order, as the game sorts them.");
	return result;
}

std::string EnvironmentDocument::save_words() const {
	std::string words = "Saving writes the environment in the editor's layout: each keyword the game reads on a line "
	                    "of its own in a fixed order, the keyframes in time order";
	const size_t ignored = ignored_lines();
	if (ignored)
		words += ", without the " + std::to_string(ignored) + (ignored == 1 ? " thing" : " things") +
		         " the game reads otherwise or skips (Problems lists each)";
	return words + ". The file's comments and spacing are not kept; the game reads the same environment.";
}

std::shared_ptr<Node> EnvironmentDocument::make_node(NodeKind, NodeId, const std::vector<std::shared_ptr<const Node>> &,
                                                     std::string &error) {
	error = "An environment keeps its one row; add keyframes inside it.";
	return nullptr;
}

bool EnvironmentDocument::accept_step(const EditStep &, const StagedRows &staged, StepRefusal &refusal) const {
	// One row after any step: an edit inside it, or the file read again in its place (Restore CR LF line ends, which
	// replaces the row).
	if (staged.rows().size() == 1) return true;
	refusal.message = "An environment keeps its one row.";
	return false;
}

// --- the findings ---------------------------------------------------------------------------------

namespace {

constexpr FindingCodeEntry<EnvironmentFinding> kFindingEntries[] = {
	{ EnvironmentFinding::InvalidInput, { "environment.invalid_input", FindingFix::None, nullptr, true } },
	{ EnvironmentFinding::IgnoredInput,
	  { "environment.ignored_input", FindingFix::Rewrite, "with each line as the game reads it" } },
	// The game loads the mission all the same, its dome at the engine's default height [orig:
	// Environment_InitDefaults @ 0x57c1ab, the raw 200; SkyDome_BuildMesh @ 0x578db0 scales by it]: listed.
	{ EnvironmentFinding::SkyHeightDefault, listed_code("environment.sky_height_default") },
};
static_assert(std::size(kFindingEntries) == static_cast<size_t>(EnvironmentFinding::kCount),
              "every EnvironmentFinding has exactly one row");
static_assert(finding_entries_well_formed(kFindingEntries),
              "the environment's rows follow EnvironmentFinding's order, each token its own");
constexpr auto kFindingRows = finding_rows(kFindingEntries, FindingGroup::Environments);
static_assert(finding_rows_well_formed(kFindingRows), "every row of the table takes its group");

} // namespace

const FindingCodeRow &finding_code(EnvironmentFinding code) { return kFindingRows[static_cast<size_t>(code)]; }

FindingTable environment_finding_codes() { return {kFindingRows.data(), kFindingRows.size()}; }

std::vector<Diagnostic> validate_environment_file(const DocumentBase &document) {
	std::vector<Diagnostic> findings;
	const auto *environment = dynamic_cast<const EnvironmentDocument *>(&document);
	if (!environment) return findings;
	source_issue_findings(*environment, finding_code(EnvironmentFinding::InvalidInput),
	                      finding_code(EnvironmentFinding::IgnoredInput), findings);
	if (document.blocked()) return findings;
	const EnvironmentRow *row = environment->environment_row();
	if (row && !row->sky_height_written()) {
		Diagnostic d = make_finding(EnvironmentFinding::SkyHeightDefault, DiagnosticSeverity::Warning,
		                            "The file leaves sky_height out: the game keeps its default of 200/65536 of a metre, "
		                            "so the sky dome lies flat on the camera.",
		                            document.path(), "sky_height");
		d.row_id = row->id;
		d.record_kind = kEnvironment;
		d.record = row->name();
		findings.push_back(std::move(d));
	}
	return findings;
}

} // namespace opennova::editor
