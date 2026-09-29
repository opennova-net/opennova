// The .3di property table (threedi_schema.h).

#include <formats/threedi/threedi_schema.h>

#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>

namespace opennova::threedi {
namespace {

using Value = ThreediSchemaValue;
using Shape = ThreediSchemaShape;
using Type = ThreediSchemaType;
using Ref = ThreediSchemaReference;

// One field: its description and how it reads and writes the native record. `arg`
// is the field's own index where one accessor serves several (an axis, a colour
// channel, a PANM track, a generator).
struct Entry {
	ThreediSchemaField field;
	Value (*get)(const void *record, int arg) = nullptr;
	bool (*set)(void *record, const Value &value, int arg, std::string &error) = nullptr; // null: read-only
	bool (*reads)(const void *record, int arg) = nullptr;                                 // null: always
	bool (*names)(const void *record, int arg) = nullptr; // null: field.reference always applies
	int arg = 0;
};

template <typename T> const T &as(const void *d) { return *static_cast<const T *>(d); }
template <typename T> T &as(void *d) { return *static_cast<T *>(d); }

int64_t whole(const Value &v) { return std::get<int64_t>(v); }
double real(const Value &v) { return std::get<double>(v); }
const std::string &text(const Value &v) { return std::get<std::string>(v); }

// A name the text form can hold: printable, no '"' (a scene name field cannot
// carry one), within the record's field.
bool set_name(char *field, size_t capacity, const Value &v, std::string &error) {
	const std::string &name = text(v);
	for (const unsigned char c : name)
		if (c < 0x20 || c == '"' || c == 0x7F) {
			error = "A name holds no control character and no '\"'.";
			return false;
		}
	std::memset(field, 0, capacity);
	std::memcpy(field, name.data(), name.size());
	return true;
}

// A texture name as build takes one (threedi_o3d_read): printable ASCII and a
// file name alone, since the loader looks its file up by that string.
bool set_texture_name(ThreediMaterialTexture &texture, const Value &v, std::string &error) {
	const std::string &name = text(v);
	for (const unsigned char c : name)
		if (c > 0x7E) {
			error = "A texture name is printable ASCII, as the game's file names are.";
			return false;
		}
	if (name.find_first_of("/\\") != std::string::npos) {
		error = "A texture name is a file name alone: the game finds a texture by its file name.";
		return false;
	}
	return set_name(texture.name, sizeof(texture.name), v, error);
}

// --- choices ----------------------------------------------------------------------

// Every generator-style byte the catalog names (the kControlEntries table).
std::vector<ThreediSchemaChoice> style_choices() {
	std::vector<ThreediSchemaChoice> out;
	for (int code = 0; code < 256; ++code) {
		const ThreediControlFuncInfo *info = threedi_control_func_info(static_cast<uint8_t>(code));
		if (info != nullptr && info->name != nullptr) out.push_back({info->name, code, ""});
	}
	return out;
}

// The collidable-type codes of a volume's name (docs/world/world-wac-ai-re.md §15;
// the add-on's VOLUME_CODES).
const std::vector<ThreediSchemaChoice> &volume_type_choices() {
	static const std::vector<ThreediSchemaChoice> choices = {
		{"CB", 1, ""}, {"CS", 2, ""}, {"CC", 3, ""}, {"CL", 4, "CL (ladder)"}, {"CV", 5, ""}, {"CA", 6, ""},
		{"VC", 7, ""}, {"BB", 8, "BB (blink box)"}, {"CD", 9, ""}, {"CT", 10, ""}, {"CM", 11, ""}, {"VK", 12, ""},
		{"CF", 13, ""}, {"LP", 14, ""}, {"DH", 16, ""}, {"DM", 17, ""}, {"DL", 18, ""}, {"CP", 19, ""},
	};
	return choices;
}

// --- the PANM track a field names --------------------------------------------------

ThreediTransform &track(void *d, int t) { return *threedi_panm_tracks(as<ThreediPartAnimation>(d))[t]; }
const ThreediTransform &track(const void *d, int t) { return *threedi_panm_tracks(as<ThreediPartAnimation>(d))[t]; }

bool track_reads(const void *d, int t) { return threedi_panm_track_loaded(as<ThreediPartAnimation>(d), t); }

// --- the four material generators ---------------------------------------------------
// 0 alphagen, 1 rgbgen, 2 ugen, 3 vgen. On disk each holds one parameter byte: the
// phase times 256 up to style 0x70, else the register index; the reader splits it
// into `phase` and `reg` by the style, so the table shows the byte.

struct Generator {
	uint8_t *style;
	float *phase;
	int32_t *reg;
	float *rate;
};

Generator generator(ThreediMaterial &m, int g) {
	switch (g) {
	case 0: return {&m.alpha_gen.style, &m.alpha_gen.phase, &m.alpha_gen.reg, &m.alpha_gen.rate};
	case 1: return {&m.rgb_gen.style, &m.rgb_gen.phase, &m.rgb_gen.reg, &m.rgb_gen.rate};
	case 2: return {&m.u_params.style, &m.u_params.phase, &m.u_params.reg, &m.u_params.gen_rate};
	default: return {&m.v_params.style, &m.v_params.phase, &m.v_params.reg, &m.v_params.gen_rate};
	}
}
Generator generator(const void *d, int g) { return generator(const_cast<ThreediMaterial &>(as<ThreediMaterial>(d)), g); }

// The parameter byte as the writer stores it (append_*_gen).
int64_t generator_param(const Generator &g) {
	if (threedi_generator_names_register(*g.style)) return static_cast<uint8_t>(*g.reg);
	const long v = std::lround(static_cast<double>(*g.phase) * 256.0);
	return v < 0 ? 0 : v > 255 ? 255 : v;
}
void store_generator_param(const Generator &g, int64_t byte) {
	if (threedi_generator_names_register(*g.style)) {
		*g.reg = static_cast<int32_t>(byte);
		*g.phase = 0.0f;
	} else {
		*g.phase = static_cast<float>(byte) / 256.0f;
		*g.reg = -1;
	}
}

// --- light derivations ----------------------------------------------------------------
// A light's view_proj follows its position, axis, cone and reach; its axis and the
// cone's cosine follow the direction and the cone. Only the words the edited field
// feeds are derived again (threedi_build's rules, the ones `build` applies).

void light_view_proj_again(ThreediLight &l) {
	threedi_build_light_view_proj(l, threedi_build_light_cone_half_angle(l));
}

// --- the tables -------------------------------------------------------------------------

ThreediSchemaField integer(const char *path, int64_t min, int64_t max, const char *label = "",
		std::vector<ThreediSchemaChoice> choices = {}, bool flags = false, Ref ref = Ref::None) {
	ThreediSchemaField f;
	f.path = path;
	f.type = Type::Integer;
	f.min = min;
	f.max = max;
	f.label = label;
	f.choices = std::move(choices);
	f.flags = flags;
	f.reference = ref;
	return f;
}
ThreediSchemaField realf(const char *path, const char *label = "") {
	ThreediSchemaField f;
	f.path = path;
	f.type = Type::Real;
	f.label = label;
	return f;
}
ThreediSchemaField textf(const char *path, size_t width, const char *label = "", Ref ref = Ref::None,
		std::vector<ThreediSchemaChoice> choices = {}) {
	ThreediSchemaField f;
	f.path = path;
	f.type = Type::Text;
	f.width = width;
	f.label = label;
	f.reference = ref;
	f.choices = std::move(choices);
	return f;
}
ThreediSchemaField fixed(ThreediSchemaField f) {
	f.read_only = true;
	return f;
}
// A field with the unit its value is in.
ThreediSchemaField in(ThreediSchemaField f, const char *unit) {
	f.unit = unit;
	return f;
}
// A field on its group's row; a colour's red, green or blue byte.
ThreediSchemaField on_row(ThreediSchemaField f, const char *group, bool channel = false) {
	f.group = group;
	f.channel = channel;
	return f;
}
// Shown as the file holds it, never set: what the game does with it is not witnessed.
ThreediSchemaField unwitnessed(ThreediSchemaField f) {
	f.read_only = true;
	f.unverified = true;
	f.note = "What the game does with this is not witnessed: shown as the file holds it.";
	return f;
}

// The component names a group's members take after the group's own ("Position X").
const char *const kAxisLabels[3][3] = {
	{"Position X", "Position Y", "Position Z"}, {"Spot axis X", "Spot axis Y", "Spot axis Z"},
	{"Direction X", "Direction Y", "Direction Z"},
};
const char *const kStartChannels[3] = {"Start red", "Start green", "Start blue"};
const char *const kEndChannels[3] = {"End red", "End green", "End blue"};

std::vector<Entry> model_entries() {
	return {
		{textf("name", 16, "Name"), [](const void *d, int) -> Value { return std::string(as<ThreediHeader>(d).name); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_name(as<ThreediHeader>(d).name, sizeof(as<ThreediHeader>(d).name), v, e);
				}},
		{fixed(integer("skinned", 0, 1, "Skinned")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediHeader>(d).mesh_type == THREEDI_MESH_SKINNED); }},
		// The largest LOD's bounding radius, derived from the geometry, unsigned 16.16
		// [orig: WriteGHDR @ 0x452B40] (docs/threedi/3di-gp-format-re.md).
		{fixed(in(realf("max_radius", "Largest LOD radius"), "m")),
				[](const void *d, int) -> Value {
					return static_cast<double>(static_cast<uint32_t>(as<ThreediHeader>(d).max_radius_fp16)) / 65536.0;
				}},
	};
}

std::vector<Entry> lod_entries() {
	return {
		{in(integer("threshold", INT32_MIN, INT32_MAX, "Takes over below"), "px"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLod>(d).lod_threshold); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLod>(d).lod_threshold = static_cast<int32_t>(whole(v));
					return true;
				}},
		{textf("type", 5, "Type", Ref::None, {{"gnrc", 0, ""}, {"bldg", 1, ""}, {"door", 2, ""}, {"veh0", 3, ""}}),
				[](const void *d, int) -> Value { return std::string(as<ThreediLod>(d).model_type); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_name(as<ThreediLod>(d).model_type, sizeof(as<ThreediLod>(d).model_type), v, e);
				}},
		{fixed(integer("parts", 0, INT32_MAX, "Parts")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLod>(d).render_object_count); }},
		{fixed(integer("strips", 0, INT32_MAX, "Strips")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLod>(d).strip_count); }},
	};
}

std::vector<Entry> panm_entries() {
	std::vector<Entry> out = {
		{fixed(integer("part", 0, 255, "Part")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediPartAnimation>(d).subobject_index); }},
		{integer("parent", 0, 255, "Moves with part (255: none)", {}, false, Ref::Part),
				[](const void *d, int) -> Value { return int64_t(as<ThreediPartAnimation>(d).parent_subobject); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediPartAnimation>(d).parent_subobject = static_cast<uint8_t>(whole(v));
					return true;
				}},
		// Read only by the spinner and the Euler tracks, and a frame only above zero as a
		// signed byte (threedi_panm_frame_row, the pose's own rule).
		{integer("matrix", 0, 255, "Rotation frame (MTRX row; 0 or 128 to 255: none)", {}, false, Ref::Frame),
				[](const void *d, int) -> Value { return int64_t(as<ThreediPartAnimation>(d).matrix_index); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediPartAnimation>(d).matrix_index = static_cast<uint8_t>(whole(v));
					return true;
				},
				[](const void *d, int) {
					const uint8_t rotation = threedi_panm_rotation_type(as<ThreediPartAnimation>(d).flags);
					return rotation == 1 || rotation == 2;
				},
				[](const void *d, int) { return threedi_panm_frame_row(as<ThreediPartAnimation>(d)) > 0; }},
	};
	// The flag word's four bytes [threedi_panm.h's pack/unpack; PANM_SampleTrack @ 0x5B2270].
	static const std::vector<ThreediSchemaChoice> scale = {{"none", 0, "None"}, {"uniform", 1, "Uniform (scalex)"},
			{"axes", 2, "Per axis"}};
	static const std::vector<ThreediSchemaChoice> rotation = {{"none", 0, "None"}, {"spinner", 1, "Spinner"},
			{"tracks", 2, "Tracks (rotx roty rotz)"}, {"billboard", 3, "Billboard"}, {"upright", 4, "Upright billboard"}};
	static const std::vector<ThreediSchemaChoice> reversed = {{"no", 0, ""}, {"yes", 1, ""}};
	static const std::vector<ThreediSchemaChoice> axis = {{"none", 0, "None"}, {"x", 1, "X"}, {"y", 2, "Y"},
			{"z", 3, "Z"}};
	const char *const kFlagPaths[4] = {"flags.scale", "flags.rotation", "flags.reversed", "flags.trans_axis"};
	const char *const kFlagLabels[4] = {"Scale", "Rotation", "Reversed", "Translation axis"};
	const std::vector<ThreediSchemaChoice> *kFlagChoices[4] = {&scale, &rotation, &reversed, &axis};
	for (int b = 0; b < 4; ++b)
		out.push_back({integer(kFlagPaths[b], 0, 255, kFlagLabels[b], *kFlagChoices[b]),
				[](const void *d, int byte) -> Value {
					return int64_t((as<ThreediPartAnimation>(d).flags >> (8 * byte)) & 0xFFu);
				},
				[](void *d, const Value &v, int byte, std::string &) {
					uint32_t &flags = as<ThreediPartAnimation>(d).flags;
					flags = (flags & ~(0xFFu << (8 * byte))) | (static_cast<uint32_t>(whole(v)) << (8 * byte));
					return true;
				},
				nullptr, nullptr, b});
	// The seven tracks: rotations in 1/16384 turn, the others 8.8, all int16 raw
	// (the text's words); the parameter byte is a register above style 0x70.
	static const std::vector<ThreediSchemaChoice> styles = style_choices();
	static std::vector<std::string> paths;
	if (paths.empty())
		for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t)
			for (const char *leaf : {".style", ".param", ".rate", ".start", ".end"})
				paths.push_back(std::string(threedi_panm_track_label(t)) + leaf);
	for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t) {
		const std::string *p = &paths[static_cast<size_t>(t) * 5];
		out.push_back({integer(p[0].c_str(), 0, 255, "Style", styles),
				[](const void *d, int tr) -> Value { return int64_t(track(d, tr).control); },
				[](void *d, const Value &v, int tr, std::string &) {
					track(d, tr).control = static_cast<uint8_t>(whole(v));
					return true;
				},
				track_reads, nullptr, t});
		out.push_back({integer(p[1].c_str(), 0, 255, "Phase or register", {}, false, Ref::Register),
				[](const void *d, int tr) -> Value { return int64_t(track(d, tr).control_param); },
				[](void *d, const Value &v, int tr, std::string &) {
					track(d, tr).control_param = static_cast<uint8_t>(whole(v));
					return true;
				},
				track_reads, [](const void *d, int tr) { return threedi_generator_names_register(track(d, tr).control); }, t});
		const char *const kWordLabels[3] = {"Rate", "Start", "End"};
		for (int w = 0; w < 3; ++w)
			out.push_back({integer(p[2 + w].c_str(), SHRT_MIN, SHRT_MAX, kWordLabels[w]),
					[](const void *d, int arg) -> Value {
						const ThreediTransform &x = track(d, arg / 3);
						const int w = arg % 3;
						return int64_t(w == 0 ? x.rate : w == 1 ? x.start : x.end);
					},
					[](void *d, const Value &v, int arg, std::string &) {
						ThreediTransform &x = track(d, arg / 3);
						const int w = arg % 3;
						(w == 0 ? x.rate : w == 1 ? x.start : x.end) = static_cast<int16_t>(whole(v));
						return true;
					},
					[](const void *d, int arg) { return track_reads(d, arg / 3); }, nullptr, t * 3 + w});
	}
	return out;
}

std::vector<Entry> material_entries() {
	static const std::vector<ThreediSchemaChoice> styles = style_choices();
	std::vector<Entry> out = {
		{textf("shader", 33, "Shader"), [](const void *d, int) -> Value { return std::string(as<ThreediMaterial>(d).shader_name); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_name(as<ThreediMaterial>(d).shader_name, sizeof(as<ThreediMaterial>(d).shader_name), v, e);
				}},
		{integer("flags", 0, 255, "Flags",
				 {{"alpha_test", int64_t(THREEDI_MATERIAL_FLAG_ALPHA_TEST), "Alpha test"},
						 {"alpha_invert", int64_t(THREEDI_MATERIAL_FLAG_ALPHA_INVERT), "Invert alpha test"},
						 {"two_sided", int64_t(THREEDI_MATERIAL_FLAG_TWO_SIDED), "Two-sided"}},
				 true),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).material_flags); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterial>(d).material_flags = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("alpha_test", 0, 255, "Alpha-test threshold"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).alpha_test_value_byte); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterial>(d).alpha_test_value_byte = static_cast<uint8_t>(whole(v));
					return true;
				}},
		// Derived from the shader (threedi_build_material_surface).
		{fixed(integer("glass", 0, 255, "Glass")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).is_glass); }},
		{fixed(integer("emissive", 0, 255, "Emissive")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).emissive_type); }},
		{integer("texanim.frames", 0, 255, "Flipbook frames"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).animation.num_frames); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterial>(d).animation.num_frames = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("texanim.type", 0, 1, "Flipbook clock", {{"time", 0, "Time"}, {"register", 1, "Register"}}),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).animation.animation_type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterial>(d).animation.animation_type = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("texanim.time", SHRT_MIN, SHRT_MAX, "Frame time or register", {}, false, Ref::Register),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).animation.cycle_frame_time); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterial>(d).animation.cycle_frame_time = static_cast<int16_t>(whole(v));
					return true;
				},
				nullptr, [](const void *d, int) { return threedi_flipbook_reads_register(as<ThreediMaterial>(d).animation); }},
	};
	static const char *const kReflect[4] = {"reflect.r", "reflect.g", "reflect.b", "reflect.a"};
	static const char *const kReflectLabels[4] = {"Reflection red", "Reflection green", "Reflection blue",
			"Reflection alpha"};
	for (int k = 0; k < 4; ++k)
		out.push_back({on_row(integer(kReflect[k], 0, 255, kReflectLabels[k]), "Reflection"),
				[](const void *d, int c) -> Value { return int64_t(threedi_build_byte_of(as<ThreediMaterial>(d).reflect_color[c])); },
				[](void *d, const Value &v, int c, std::string &) {
					as<ThreediMaterial>(d).reflect_color[c] = threedi_byte_unit(static_cast<int>(whole(v)));
					return true;
				},
				nullptr, nullptr, k});
	// The generators: alphagen, rgbgen, ugen, vgen.
	static const char *const kGen[4] = {"alphagen", "rgbgen", "ugen", "vgen"};
	static std::vector<std::string> paths;
	if (paths.empty())
		for (const char *g : kGen)
			for (const char *leaf : {".style", ".param", ".rate"}) paths.push_back(std::string(g) + leaf);
	for (int g = 0; g < 4; ++g) {
		const std::string *p = &paths[static_cast<size_t>(g) * 3];
		out.push_back({integer(p[0].c_str(), 0, 255, "Style", styles),
				[](const void *d, int gi) -> Value { return int64_t(*generator(d, gi).style); },
				[](void *d, const Value &v, int gi, std::string &) {
					// The parameter byte stays as the file holds it: a new style
					// reads it as a phase or a register.
					const Generator gen = generator(as<ThreediMaterial>(d), gi);
					const int64_t byte = generator_param(gen);
					*gen.style = static_cast<uint8_t>(whole(v));
					store_generator_param(gen, byte);
					return true;
				},
				nullptr, nullptr, g});
		out.push_back({integer(p[1].c_str(), 0, 255, "Phase (1/256) or register", {}, false, Ref::Register),
				[](const void *d, int gi) -> Value { return generator_param(generator(d, gi)); },
				[](void *d, const Value &v, int gi, std::string &) {
					store_generator_param(generator(as<ThreediMaterial>(d), gi), whole(v));
					return true;
				},
				nullptr, [](const void *d, int gi) { return threedi_generator_names_register(*generator(d, gi).style); }, g});
		out.push_back({realf(p[2].c_str(), "Rate"),
				[](const void *d, int gi) -> Value { return double(*generator(d, gi).rate); },
				[](void *d, const Value &v, int gi, std::string &) {
					*generator(as<ThreediMaterial>(d), gi).rate = threedi_q8f(real(v));
					return true;
				},
				nullptr, nullptr, g});
	}
	// Each generator's range: the alpha one's int16 words, the RGB one's colours, the
	// UV ones' 8.8 values.
	out.push_back({integer("alphagen.start", SHRT_MIN, SHRT_MAX, "Start"),
			[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).alpha_gen.start); },
			[](void *d, const Value &v, int, std::string &) {
				as<ThreediMaterial>(d).alpha_gen.start = static_cast<int16_t>(whole(v));
				return true;
			}});
	out.push_back({integer("alphagen.end", SHRT_MIN, SHRT_MAX, "End"),
			[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).alpha_gen.end); },
			[](void *d, const Value &v, int, std::string &) {
				as<ThreediMaterial>(d).alpha_gen.end = static_cast<int16_t>(whole(v));
				return true;
			}});
	static const char *const kRgb[6] = {"rgbgen.start.r", "rgbgen.start.g", "rgbgen.start.b", "rgbgen.end.r",
			"rgbgen.end.g", "rgbgen.end.b"};
	for (int k = 0; k < 6; ++k)
		out.push_back({on_row(integer(kRgb[k], 0, 255, k < 3 ? kStartChannels[k] : kEndChannels[k - 3]),
						k < 3 ? "Start colour" : "End colour", true),
				[](const void *d, int c) -> Value {
					const ThreediRgbGen &g = as<ThreediMaterial>(d).rgb_gen;
					return int64_t(threedi_build_byte_of(c < 3 ? g.start_color[c] : g.end_color[c - 3]));
				},
				[](void *d, const Value &v, int c, std::string &) {
					ThreediRgbGen &g = as<ThreediMaterial>(d).rgb_gen;
					(c < 3 ? g.start_color[c] : g.end_color[c - 3]) = threedi_byte_unit(static_cast<int>(whole(v)));
					return true;
				},
				nullptr, nullptr, k});
	static const char *const kUv[4] = {"ugen.start", "ugen.end", "vgen.start", "vgen.end"};
	for (int k = 0; k < 4; ++k)
		out.push_back({realf(kUv[k], k % 2 == 0 ? "Start" : "End"),
				[](const void *d, int c) -> Value {
					const ThreediMaterial &m = as<ThreediMaterial>(d);
					const ThreediUvParams &g = c < 2 ? m.u_params : m.v_params;
					return double(c % 2 == 0 ? g.start : g.end);
				},
				[](void *d, const Value &v, int c, std::string &) {
					ThreediMaterial &m = as<ThreediMaterial>(d);
					ThreediUvParams &g = c < 2 ? m.u_params : m.v_params;
					(c % 2 == 0 ? g.start : g.end) = threedi_q8f(real(v));
					return true;
				},
				nullptr, nullptr, k});
	// The second channel's words, as the reader keeps them (threedi_3di3_read's MTRL
	// record; always zero in the corpus, docs/threedi/o3d-scene-format.md): the second RGB
	// generator, reflection colour, emissive and glass bytes.
	out.push_back({unwitnessed(integer("rgbgen2.style", 0, 255, "Style", styles)),
			[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).rgb_gen2.style); }});
	out.push_back({unwitnessed(integer("rgbgen2.param", 0, 255, "Phase (1/256) or register")),
			[](const void *d, int) -> Value {
				ThreediMaterial &m = const_cast<ThreediMaterial &>(as<ThreediMaterial>(d));
				return generator_param({&m.rgb_gen2.style, &m.rgb_gen2.phase, &m.rgb_gen2.reg, &m.rgb_gen2.rate});
			}});
	out.push_back({unwitnessed(realf("rgbgen2.rate", "Rate")),
			[](const void *d, int) -> Value { return double(as<ThreediMaterial>(d).rgb_gen2.rate); }});
	static const char *const kRgb2[6] = {"rgbgen2.start.r", "rgbgen2.start.g", "rgbgen2.start.b", "rgbgen2.end.r",
			"rgbgen2.end.g", "rgbgen2.end.b"};
	for (int k = 0; k < 6; ++k)
		out.push_back({unwitnessed(on_row(integer(kRgb2[k], 0, 255, k < 3 ? kStartChannels[k] : kEndChannels[k - 3]),
						k < 3 ? "Start colour" : "End colour", true)),
				[](const void *d, int c) -> Value {
					const ThreediRgbGen &g = as<ThreediMaterial>(d).rgb_gen2;
					return int64_t(threedi_build_byte_of(c < 3 ? g.start_color[c] : g.end_color[c - 3]));
				},
				nullptr, nullptr, nullptr, k});
	static const char *const kReflect2[4] = {"reflect2.r", "reflect2.g", "reflect2.b", "reflect2.a"};
	static const char *const kReflect2Labels[4] = {"Reflection red", "Reflection green", "Reflection blue",
			"Reflection alpha"};
	for (int k = 0; k < 4; ++k)
		out.push_back({unwitnessed(on_row(integer(kReflect2[k], 0, 255, kReflect2Labels[k]), "Second reflection")),
				[](const void *d, int c) -> Value {
					return int64_t(threedi_build_byte_of(as<ThreediMaterial>(d).reflect_color2[c]));
				},
				nullptr, nullptr, nullptr, k});
	out.push_back({unwitnessed(integer("emissive2", 0, 255, "Second emissive")),
			[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).emissive_type2); }});
	out.push_back({unwitnessed(integer("glass2", 0, 255, "Second glass")),
			[](const void *d, int) -> Value { return int64_t(as<ThreediMaterial>(d).glass_type2); }});
	return out;
}

std::vector<Entry> texture_entries() {
	return {
		{textf("name", 17, "File", Ref::Texture),
				[](const void *d, int) -> Value { return std::string(as<ThreediMaterialTexture>(d).name); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_texture_name(as<ThreediMaterialTexture>(d), v, e);
				}},
		{integer("slot", 0, 255, "Slot",
				 {{"diffuse", THREEDI_TEX_SLOT_DIFFUSE, "Diffuse"}, {"detail", THREEDI_TEX_SLOT_DETAIL, "Detail"},
						 {"normal", THREEDI_TEX_SLOT_NORMAL, "Normal"}, {"normal_b", THREEDI_TEX_SLOT_NORMAL_B, "Second normal"}}),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterialTexture>(d).slot); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterialTexture>(d).slot = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("type", 0, 255, "Type",
				 {{"diffuse", THREEDI_TEX_TYPE_DIFFUSE, "Diffuse"}, {"normal_mdt", THREEDI_TEX_TYPE_NORMAL_MDT, "Normal (MDT)"},
						 {"normal_tga", THREEDI_TEX_TYPE_NORMAL_TGA, "Normal (TGA alpha)"}}),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterialTexture>(d).type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterialTexture>(d).type = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("flags", 0, 255, "Flags",
				 {{"animated", int64_t(THREEDI_TEX_FLAG_ANIMATED), "Flipbook frame"},
						 {"state_override", int64_t(THREEDI_TEX_FLAG_STATE_OVERRIDE), "State override"}},
				 true),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterialTexture>(d).flags); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterialTexture>(d).flags = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{integer("frame", 0, 255, "Flipbook frame"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediMaterialTexture>(d).frame); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediMaterialTexture>(d).frame = static_cast<uint8_t>(whole(v));
					return true;
				}},
	};
}

bool light_is_spot(const void *d, int) { return (as<ThreediLight>(d).flags & THREEDI_LIGHT_FLAG_TYPE_TARGET) != 0; }

std::vector<Entry> light_entries() {
	static const std::vector<ThreediSchemaChoice> styles = style_choices();
	std::vector<Entry> out = {
		{integer("part", 0, 255, "Part", {}, false, Ref::Part),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).subobj_index); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLight>(d).subobj_index = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{in(realf("atten_start", "Full brightness to"), "m"),
				[](const void *d, int) -> Value { return double(as<ThreediLight>(d).atten_start); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLight>(d).atten_start = static_cast<float>(real(v));
					return true;
				}},
		{in(realf("atten_end", "Reach"), "m"), [](const void *d, int) -> Value { return double(as<ThreediLight>(d).atten_end); },
				[](void *d, const Value &v, int, std::string &) {
					ThreediLight &l = as<ThreediLight>(d);
					l.atten_end = static_cast<float>(real(v));
					light_view_proj_again(l);
					return true;
				}},
		{integer("style", 0, 255, "Style", styles),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).style); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLight>(d).style = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{in(realf("rate", "Rate"), "/s"),
				[](const void *d, int) -> Value { return threedi_build_light_rate_value(as<ThreediLight>(d).rate); },
				[](void *d, const Value &v, int, std::string &e) {
					if (!(real(v) >= 0.0 && real(v) < 256.0)) {
						e = "A light's rate is 0 up to 256.";
						return false;
					}
					as<ThreediLight>(d).rate = threedi_build_light_rate(real(v));
					return true;
				}},
		// The phase byte: the phase times 256 up to style 0x70, else the register
		// index (threedi_build_light_phase).
		{integer("param", 0, 255, "Phase (1/256) or register", {}, false, Ref::Register),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).phase); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLight>(d).phase = static_cast<uint8_t>(whole(v));
					return true;
				},
				nullptr, [](const void *d, int) { return threedi_generator_names_register(as<ThreediLight>(d).style); }},
		{integer("flags", 0, 255, "Flags",
				 {{"no_corona", int64_t(THREEDI_LIGHT_FLAG_DISABLE_CORONA), "No corona"},
						 {"no_terrain", int64_t(THREEDI_LIGHT_FLAG_DISABLE_TERRAIN), "Does not light terrain"},
						 {"no_objects", int64_t(THREEDI_LIGHT_FLAG_DISABLE_OBJECTS), "Does not light objects"},
						 {"spot", int64_t(THREEDI_LIGHT_FLAG_TYPE_TARGET), "Spot"}},
				 true),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).flags); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediLight>(d).flags = static_cast<uint8_t>(whole(v));
					return true;
				}},
		// The cone's half-angle in whole degrees: WriteLGHT keeps its byte, and the
		// cosine and view_proj follow it.
		{in(integer("cone", 0, 255, "Cone half-angle"), "deg"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).falloff_byte); },
				[](void *d, const Value &v, int, std::string &) {
					ThreediLight &l = as<ThreediLight>(d);
					const float falloff = static_cast<float>(whole(v));
					l.falloff_byte = static_cast<uint8_t>(whole(v));
					l.rotation[3] = threedi_build_light_cone_cos(falloff);
					threedi_build_light_view_proj(l, falloff);
					return true;
				},
				light_is_spot},
		// The record's byte 34, which the reader keeps (threedi_3di3_read's LGHT record).
		{unwitnessed(integer("unknown1", 0, 255, "Byte 34")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediLight>(d).unknown1); }},
	};
	static const char *const kPosition[3] = {"position.x", "position.y", "position.z"};
	for (int k = 0; k < 3; ++k)
		out.push_back({on_row(in(realf(kPosition[k], kAxisLabels[0][k]), "m"), "Position"),
				[](const void *d, int a) -> Value {
					const float *o = as<ThreediLight>(d).offset;
					const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{o[0], o[1], o[2]});
					return a == 0 ? m.x : a == 1 ? m.y : m.z;
				},
				[](void *d, const Value &v, int a, std::string &) {
					ThreediLight &l = as<ThreediLight>(d);
					ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{l.offset[0], l.offset[1], l.offset[2]});
					(a == 0 ? m.x : a == 1 ? m.y : m.z) = real(v);
					const ThreediBuildVec3 o = threedi_build_to_model(m);
					l.offset[0] = static_cast<float>(o.x);
					l.offset[1] = static_cast<float>(o.y);
					l.offset[2] = static_cast<float>(o.z);
					light_view_proj_again(l);
					return true;
				},
				nullptr, nullptr, k});
	static const char *const kDirection[3] = {"direction.x", "direction.y", "direction.z"};
	for (int k = 0; k < 3; ++k)
		out.push_back({on_row(realf(kDirection[k], kAxisLabels[1][k]), "Spot axis"),
				[](const void *d, int a) -> Value {
					const float *r = as<ThreediLight>(d).rotation;
					const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{r[0], r[1], r[2]});
					return a == 0 ? m.x : a == 1 ? m.y : m.z;
				},
				[](void *d, const Value &v, int a, std::string &) {
					ThreediLight &l = as<ThreediLight>(d);
					ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{l.rotation[0], l.rotation[1], l.rotation[2]});
					(a == 0 ? m.x : a == 1 ? m.y : m.z) = real(v);
					const ThreediBuildVec3 r = threedi_build_to_model(m);
					// + 0.0f folds the axis map's negative zeros, as add_light does.
					l.rotation[0] = static_cast<float>(r.x) + 0.0f;
					l.rotation[1] = static_cast<float>(r.y) + 0.0f;
					l.rotation[2] = static_cast<float>(r.z) + 0.0f;
					light_view_proj_again(l);
					return true;
				},
				light_is_spot, nullptr, k});
	// Colours as authored: r g b; LGHT packs them B, G, R.
	static const char *const kColour[6] = {"start.r", "start.g", "start.b", "end.r", "end.g", "end.b"};
	for (int k = 0; k < 6; ++k)
		out.push_back({on_row(integer(kColour[k], 0, 255, k < 3 ? kStartChannels[k] : kEndChannels[k - 3]),
						k < 3 ? "Start colour" : "End colour", true),
				[](const void *d, int c) -> Value {
					const ThreediLight &l = as<ThreediLight>(d);
					return int64_t(c < 3 ? l.color_start[2 - c] : l.color_end[5 - c]);
				},
				[](void *d, const Value &v, int c, std::string &) {
					ThreediLight &l = as<ThreediLight>(d);
					(c < 3 ? l.color_start[2 - c] : l.color_end[5 - c]) = static_cast<uint8_t>(whole(v));
					return true;
				},
				nullptr, nullptr, k});
	return out;
}

// A user point's words are 16.16 in mission axes, truncated as the exporter
// stores a point (threedi_build's add_user_point).
int32_t &user_point_word(ThreediUserPoint &u, int w) {
	int32_t *words[6] = {&u.x, &u.y, &u.z, &u.rot_x, &u.rot_y, &u.rot_z};
	return *words[w];
}

std::vector<Entry> user_point_entries() {
	std::vector<Entry> out = {
		{textf("name", 16, "Name"), [](const void *d, int) -> Value { return std::string(as<ThreediUserPoint>(d).name); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_name(as<ThreediUserPoint>(d).name, 16, v, e);
				}},
		{integer("part", INT32_MIN, INT32_MAX, "Part (-1: none)", {}, false, Ref::Part),
				[](const void *d, int) -> Value { return int64_t(as<ThreediUserPoint>(d).subobject_index); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediUserPoint>(d).subobject_index = static_cast<int32_t>(whole(v));
					return true;
				}},
		{integer("type", INT32_MIN, INT32_MAX, "Type",
				 {{"G", THREEDI_USER_POINT_GAMEPLAY, "G (gameplay)"}, {"S", THREEDI_USER_POINT_EFFECT, "S (effect)"}}),
				[](const void *d, int) -> Value { return int64_t(as<ThreediUserPoint>(d).userpoint_type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediUserPoint>(d).userpoint_type = static_cast<int32_t>(whole(v));
					return true;
				}},
	};
	static const char *const kWords[6] = {"position.x", "position.y", "position.z", "direction.x", "direction.y",
			"direction.z"};
	for (int k = 0; k < 6; ++k)
		out.push_back({k < 3 ? on_row(in(realf(kWords[k], kAxisLabels[0][k]), "m"), "Position")
						: on_row(realf(kWords[k], kAxisLabels[2][k - 3]), "Direction"),
				[](const void *d, int w) -> Value {
					return static_cast<double>(user_point_word(const_cast<ThreediUserPoint &>(as<ThreediUserPoint>(d)), w)) /
							65536.0;
				},
				[](void *d, const Value &v, int w, std::string &e) {
					if (!(std::fabs(real(v)) < 32768.0)) {
						e = "The value is past what a user point's 16.16 word holds.";
						return false;
					}
					user_point_word(as<ThreediUserPoint>(d), w) = threedi_q16_trunc(real(v));
					return true;
				},
				nullptr, nullptr, k});
	return out;
}

std::vector<Entry> register_entries() {
	static const std::vector<ThreediSchemaChoice> names = [] {
		std::vector<ThreediSchemaChoice> out;
		for (size_t i = 0;; ++i) {
			const char *name = threedi_ctrl_register_name(i);
			if (name == nullptr) break;
			out.push_back({name, static_cast<int64_t>(i), ""});
		}
		return out;
	}();
	return {
		{textf("name", 25, "Register", Ref::None, names),
				[](const void *d, int) -> Value { return std::string(as<ThreediControlRegister>(d).name); },
				[](void *d, const Value &v, int, std::string &e) {
					return set_name(as<ThreediControlRegister>(d).name, sizeof(as<ThreediControlRegister>(d).name), v, e);
				}},
	};
}

std::vector<Entry> frame_entries() {
	std::vector<Entry> out;
	static const char *const kCells[9] = {"r00", "r01", "r02", "r10", "r11", "r12", "r20", "r21", "r22"};
	// A row of the rotation (mission axes) on a row of its own, its cells by column.
	static const char *const kCellLabels[9] = {"Row 1, column 1", "Row 1, column 2", "Row 1, column 3",
			"Row 2, column 1", "Row 2, column 2", "Row 2, column 3", "Row 3, column 1", "Row 3, column 2",
			"Row 3, column 3"};
	static const char *const kRows[3] = {"Row 1", "Row 2", "Row 3"};
	for (int k = 0; k < 9; ++k) {
		ThreediSchemaField cell = on_row(realf(kCells[k], kCellLabels[k]), kRows[k / 3]);
		cell.note = "The frame's rotation, in mission axes.";
		out.push_back({cell,
				[](const void *d, int c) -> Value {
					double mission[9];
					threedi_build_frame_to_mission(as<ThreediMatrix4x4>(d), mission);
					return mission[c];
				},
				[](void *d, const Value &v, int c, std::string &) {
					double mission[9];
					threedi_build_frame_to_mission(as<ThreediMatrix4x4>(d), mission);
					mission[c] = real(v);
					as<ThreediMatrix4x4>(d) = threedi_build_frame_to_model(mission);
					return true;
				},
				nullptr, nullptr, k});
	}
	return out;
}

std::vector<Entry> section_entries() {
	std::vector<Entry> out = {
		{fixed(integer("parent", INT32_MIN, INT32_MAX, "Parent part")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediCollisionObject>(d).parent_subobject_index); }},
		{fixed(integer("volumes", 0, INT32_MAX, "Volumes")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediCollisionObject>(d).num_bounding_volumes); }},
		{fixed(integer("faces", 0, INT32_MAX, "Bullet faces")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediCollisionObject>(d).num_faces); }},
	};
	static const char *const kOffset[3] = {"offset.x", "offset.y", "offset.z"};
	static const char *const kOffsetLabels[3] = {"Offset X", "Offset Y", "Offset Z"};
	for (int k = 0; k < 3; ++k)
		out.push_back({fixed(on_row(in(realf(kOffset[k], kOffsetLabels[k]), "m"), "Offset")),
				[](const void *d, int a) -> Value { return as<ThreediCollisionObject>(d).offset[a] / 65536.0; }, nullptr,
				nullptr, nullptr, k});
	return out;
}

std::vector<Entry> volume_entries() {
	return {
		{integer("type", INT32_MIN, INT32_MAX, "Type", volume_type_choices()),
				[](const void *d, int) -> Value { return int64_t(as<ThreediBoundingVolume>(d).collidable_type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediBoundingVolume>(d).collidable_type = static_cast<int32_t>(whole(v));
					return true;
				}},
		// A blink box's enabled letters (the add-on's BLINK_LETTER_BITS); the upper
		// bytes ride as they are.
		{integer("flags", INT32_MIN, INT32_MAX, "Flags",
				 {{"V", 0x2, "V"}, {"S", 0x4, "S"}, {"W", 0x8, "W"}, {"L", 0x10, "L"}, {"O", 0x20, "O"}}, true),
				[](const void *d, int) -> Value { return int64_t(as<ThreediBoundingVolume>(d).flags); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediBoundingVolume>(d).flags = static_cast<int32_t>(whole(v));
					return true;
				}},
		{fixed(integer("planes", 0, INT32_MAX, "Planes")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediBoundingVolume>(d).plane_count); }},
	};
}

std::vector<Entry> face_entries() {
	return {
		{integer("poly_type", 0, 255, "Impact material"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediCollisionFace>(d).poly_type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediCollisionFace>(d).poly_type = static_cast<uint8_t>(whole(v));
					return true;
				}},
		// docs/threedi/o3d-scene-format.md `cf` [orig: Physics_RaycastAgainstBoneCollision
		// @ 0x4e4cb0, the test @ 0x4e5139].
		{integer("flags", 0, UINT32_MAX, "Flags",
				 {{"both_sides", 0x1, "Both sides"}, {"bullets_pass", 0x100, "Bullets pass"},
						 {"front_only", 0x800, "Front only"}},
				 true),
				[](const void *d, int) -> Value { return int64_t(as<ThreediCollisionFace>(d).material_flags); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediCollisionFace>(d).material_flags = static_cast<uint32_t>(whole(v));
					return true;
				}},
	};
}

std::vector<Entry> occlusion_entries() {
	return {
		{integer("type", 0, 255, "Type",
				 {{"occluder", 0, "Occluder"}, {"open", 1, "Open"}, {"window", 2, "Window"}, {"portal", 3, "Portal"},
						 {"oh", 4, "OH"}}),
				[](const void *d, int) -> Value { return int64_t(as<ThreediOcclusionObject>(d).type); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediOcclusionObject>(d).type = static_cast<uint8_t>(whole(v));
					return true;
				}},
		{fixed(integer("section", 0, 255, "Section")),
				[](const void *d, int) -> Value { return int64_t(as<ThreediOcclusionObject>(d).parent_subobject_index); }},
		{integer("connecting", 0, 255, "Leads to section"),
				[](const void *d, int) -> Value { return int64_t(as<ThreediOcclusionObject>(d).connecting_subobject); },
				[](void *d, const Value &v, int, std::string &) {
					as<ThreediOcclusionObject>(d).connecting_subobject = static_cast<uint8_t>(whole(v));
					return true;
				}},
	};
}

const std::vector<Entry> &entries(Shape shape) {
	static const std::vector<Entry> tables[] = {
		model_entries(), lod_entries(), panm_entries(), material_entries(), texture_entries(), light_entries(),
		user_point_entries(), register_entries(), frame_entries(), section_entries(), volume_entries(),
		face_entries(), occlusion_entries(),
	};
	return tables[static_cast<size_t>(shape)];
}

const Entry *entry(Shape shape, const std::string &path) {
	for (const Entry &e : entries(shape))
		if (path == e.field.path) return &e;
	return nullptr;
}

// The value in the field's own type: a whole Real reads as a number, a whole number
// fills a Real; false when it does not fit.
bool normalize(const ThreediSchemaField &f, const Value &in, Value &out, std::string &error) {
	switch (f.type) {
	case Type::Integer: {
		int64_t v = 0;
		if (const int64_t *i = std::get_if<int64_t>(&in)) v = *i;
		else if (const double *d = std::get_if<double>(&in); d && std::isfinite(*d) && *d == std::floor(*d) &&
				 std::fabs(*d) < 9.0e18)
			v = static_cast<int64_t>(*d);
		else {
			error = "This field takes a whole number.";
			return false;
		}
		if (v < f.min || v > f.max) {
			error = "A value from " + std::to_string(f.min) + " to " + std::to_string(f.max) + ".";
			return false;
		}
		out = v;
		return true;
	}
	case Type::Real: {
		double v = 0.0;
		if (const double *d = std::get_if<double>(&in)) v = *d;
		else if (const int64_t *i = std::get_if<int64_t>(&in)) v = static_cast<double>(*i);
		else {
			error = "This field takes a number.";
			return false;
		}
		out = v; // non-finite: refused unless the field already reads it (threedi_schema_set)
		return true;
	}
	case Type::Text: {
		const std::string *s = std::get_if<std::string>(&in);
		if (s == nullptr) {
			error = "This field takes text.";
			return false;
		}
		if (s->size() + 1 > f.width) {
			error = "At most " + std::to_string(f.width - 1) + " characters.";
			return false;
		}
		out = *s;
		return true;
	}
	}
	return false;
}

} // namespace

const std::vector<ThreediSchemaField> &threedi_schema_fields(ThreediSchemaShape shape) {
	static const std::vector<std::vector<ThreediSchemaField>> fields = [] {
		std::vector<std::vector<ThreediSchemaField>> out;
		for (int s = 0; s <= static_cast<int>(Shape::Occlusion); ++s) {
			out.emplace_back();
			for (const Entry &e : entries(static_cast<Shape>(s))) out.back().push_back(e.field);
		}
		return out;
	}();
	return fields[static_cast<size_t>(shape)];
}

const ThreediSchemaField *threedi_schema_field(ThreediSchemaShape shape, const std::string &path) {
	for (const ThreediSchemaField &f : threedi_schema_fields(shape))
		if (path == f.path) return &f;
	return nullptr;
}

bool threedi_schema_get(const ThreediSchemaRecord &record, const std::string &path, ThreediSchemaValue &out) {
	const Entry *e = record ? entry(record.shape, path) : nullptr;
	if (e == nullptr) return false;
	out = e->get(record.data, e->arg);
	return true;
}

bool threedi_schema_set(const ThreediSchemaRecord &record, const std::string &path, const ThreediSchemaValue &value,
		std::string &error) {
	const Entry *e = record ? entry(record.shape, path) : nullptr;
	if (e == nullptr) {
		error = "Unknown field.";
		return false;
	}
	if (e->set == nullptr) {
		error = "This field is read-only: it is authored in Blender or derived.";
		return false;
	}
	Value v;
	if (!normalize(e->field, value, v, error)) return false;
	// The value the field already reads: nothing changes, not even a derived word.
	// Numbers compare bit for bit, so a NaN a retail word holds (US01's MTRX)
	// matches itself.
	const Value current = e->get(record.data, e->arg);
	const double *now = std::get_if<double>(&current);
	const double *next = std::get_if<double>(&v);
	if (now != nullptr && next != nullptr ? std::memcmp(now, next, sizeof(double)) == 0 : current == v) return true;
	if (next != nullptr && !std::isfinite(*next)) {
		error = "A finite number.";
		return false;
	}
	return e->set(record.data, v, e->arg, error);
}

bool threedi_schema_reads(const ThreediSchemaRecord &record, const std::string &path) {
	const Entry *e = record ? entry(record.shape, path) : nullptr;
	return e != nullptr && (e->reads == nullptr || e->reads(record.data, e->arg));
}

ThreediSchemaReference threedi_schema_reference(const ThreediSchemaRecord &record, const std::string &path) {
	const Entry *e = record ? entry(record.shape, path) : nullptr;
	if (e == nullptr) return ThreediSchemaReference::None;
	if (e->names != nullptr && !e->names(record.data, e->arg)) return ThreediSchemaReference::None;
	return e->field.reference;
}

} // namespace opennova::threedi
