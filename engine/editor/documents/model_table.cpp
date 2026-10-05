// The model's table (model_document.h, ADR 0046 S10; S13 D10 made it rows of the one table shape,
// model/table_shape.h, from the format's property table it was): the engine features of a model an
// editor changes, named once by a dotted path per record, over the engine's own records
// (Threedi3di3's ThreediMaterial, ThreediLight, ThreediUserPoint, ThreediPartAnimation, ...; ADR
// 0027: the parsed struct is what an editor changes). The units are the `.o3d` scene text's
// (docs/threedi/o3d-scene-format.md): mission axes, byte colours, raw PANM track words, 16.16
// truncated as the exporter stores a point; a get is `scene`'s reading and a set is `build`'s, so the
// editor and the Blender add-on agree. Each field names its unit where it has one and the row it
// shares (a position's axes, a colour's channels, a frame's row), each member named by its component.
// Geometry (vertices, strips, collision planes and faces' corners) is not here: it is authored in
// Blender.
//
// A Set of the value a field already reads changes nothing, so a retail word that sits off the text's
// grid survives a Set of every field to its own value. Only the words a field derives are derived
// again when it changes (a light's axis and view_proj follow its position, direction, cone and reach,
// never its colour).
#include <editor/documents/model_document.h>

#include <algorithm>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
#include <runtime/renderer/material_descriptor.h>

#include <editor/documents/model_collision_words.h>
#include <editor/documents/model_surfaces.h>

#include "model_document_internal.h"

namespace opennova::editor {
namespace {

using namespace threedi;

// The records an entry describes, and the native struct behind each: Model ThreediHeader, Lod
// ThreediLod, PartAnimation ThreediPartAnimation, Material ThreediMaterial, Texture
// ThreediMaterialTexture, Light ThreediLight, UserPoint ThreediUserPoint, Register
// ThreediControlRegister, Frame ThreediMatrix4x4 (an MTRX row), Section ThreediCollisionObject, Volume
// ThreediBoundingVolume, Face ThreediCollisionFace, Occlusion ThreediOcclusionObject.
enum class Shape {
	Model, Lod, PartAnimation, Material, Texture, Light, UserPoint, Register, Frame, Section, Volume, Face, Occlusion,
};

enum class Type { Integer, Real, Text };

// What a field names outside its record: a texture file, a CTRL register (by its index in the model's
// table), a part (by its index in LOD 0), an MTRX frame (by its row).
enum class Ref { None, Texture, Register, Part, Frame };

struct Choice {
	const char *name = "";
	int64_t value = 0;
	const char *label = ""; // "" = the name
};

struct Field {
	const char *path = "";        // "position.x", "rgbgen.start.r", "rotx.style"
	Type type = Type::Integer;
	size_t width = 0;             // a text's capacity in bytes, the terminator included
	int64_t min = 0, max = 0;     // an integer's range
	bool read_only = false;       // shown, never set (geometry, derived words)
	bool flags = false;           // the choices are bits of one integer
	Ref reference = Ref::None;
	std::vector<Choice> choices;
	const char *label = "";       // "" = the path; a component's names it ("Position X")
	const char *unit = "";        // what the value is in ("m", "deg"; "" = none)
	const char *group = "";       // the row it shares with its neighbours of the group ("" = none)
	bool channel = false;         // a colour's red, green or blue byte in a group of the three
	// What the game makes of the value is not witnessed: shown as the file holds it, never set (the
	// second-channel material words, a light's byte 34).
	bool unverified = false;
	const char *note = "";        // what the value is, where the label cannot say it
};

// One field: its description and how it reads and writes the native record. `arg`
// is the field's own index where one accessor serves several (an axis, a colour
// channel, a PANM track, a generator).
struct Entry {
	Field field;
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

// A texture name as build takes one (threedi_o3d_lower, the retail target): printable ASCII and a
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
std::vector<Choice> style_choices() {
	std::vector<Choice> out;
	for (int code = 0; code < 256; ++code) {
		const ThreediControlFuncInfo *info = threedi_control_func_info(static_cast<uint8_t>(code));
		if (info != nullptr && info->name != nullptr) out.push_back({info->name, code, ""});
	}
	return out;
}

// The collidable-type codes of a volume's name (docs/world/world-wac-ai-re.md §15;
// the add-on's VOLUME_CODES), each labelled by what the game does with it (model_collision_words.h:
// "ladder (CL)").
const std::vector<Choice> &volume_type_choices() {
	static const int64_t kTypes[] = {1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 18, 19};
	static const std::vector<std::string> labels = [] {
		std::vector<std::string> out;
		for (const int64_t type : kTypes)
			out.push_back(std::string(model_volume_type(type).words) + " (" + model_volume_type(type).code + ")");
		return out;
	}();
	static const std::vector<Choice> choices = [] {
		std::vector<Choice> out;
		for (size_t i = 0; i < labels.size(); ++i)
			out.push_back({model_volume_type(kTypes[i]).code, kTypes[i], labels[i].c_str()});
		return out;
	}();
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

Field integer(const char *path, int64_t min, int64_t max, const char *label = "",
		std::vector<Choice> choices = {}, bool flags = false, Ref ref = Ref::None) {
	Field f;
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
Field realf(const char *path, const char *label = "") {
	Field f;
	f.path = path;
	f.type = Type::Real;
	f.label = label;
	return f;
}
Field textf(const char *path, size_t width, const char *label = "", Ref ref = Ref::None,
		std::vector<Choice> choices = {}) {
	Field f;
	f.path = path;
	f.type = Type::Text;
	f.width = width;
	f.label = label;
	f.reference = ref;
	f.choices = std::move(choices);
	return f;
}
Field fixed(Field f) {
	f.read_only = true;
	return f;
}
// A field with the unit its value is in.
Field in(Field f, const char *unit) {
	f.unit = unit;
	return f;
}
// A field on its group's row; a colour's red, green or blue byte.
Field on_row(Field f, const char *group, bool channel = false) {
	f.group = group;
	f.channel = channel;
	return f;
}
// A field with words of what its value is, where the label cannot say it.
Field noted(Field f, const char *note) {
	f.note = note;
	return f;
}
// Shown as the file holds it, never set: what the game does with it is not witnessed.
Field unwitnessed(Field f) {
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
		{noted(in(integer("threshold", INT32_MIN, INT32_MAX, "Drawn above"), "px"),
				  "The projected radius above which this LOD draws, down to the next one's; the game's walk stops at "
				  "the first 0, so no LOD after it is drawn by distance [orig: Model_SelectRlodLevel @ 0x5c3b20]."),
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
	static const std::vector<Choice> scale = {{"none", 0, "None"}, {"uniform", 1, "Uniform (scalex)"},
			{"axes", 2, "Per axis"}};
	static const std::vector<Choice> rotation = {{"none", 0, "None"}, {"spinner", 1, "Spinner"},
			{"tracks", 2, "Tracks (rotx roty rotz)"}, {"billboard", 3, "Billboard"}, {"upright", 4, "Upright billboard"}};
	static const std::vector<Choice> reversed = {{"no", 0, ""}, {"yes", 1, ""}};
	static const std::vector<Choice> axis = {{"none", 0, "None"}, {"x", 1, "X"}, {"y", 2, "Y"},
			{"z", 3, "Z"}};
	const char *const kFlagPaths[4] = {"flags.scale", "flags.rotation", "flags.reversed", "flags.trans_axis"};
	const char *const kFlagLabels[4] = {"Scale", "Rotation", "Reversed", "Translation axis"};
	const std::vector<Choice> *kFlagChoices[4] = {&scale, &rotation, &reversed, &axis};
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
	static const std::vector<Choice> styles = style_choices();
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
	static const std::vector<Choice> styles = style_choices();
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
	// Its parameter names a register above style 0x70 as the first generator's does: the load swaps
	// it through the model's CTRL table [orig: ThreediGp_LoadFromFile @ 0x5B5D0A..0x5B5D2A]. Shown,
	// never set, as the generator's other words are.
	Field second_param =
			unwitnessed(integer("rgbgen2.param", 0, 255, "Phase (1/256) or register", {}, false, Ref::Register));
	second_param.note = "The load swaps it through the model's CTRL table above style 0x70, as it does the first "
	                    "generator's; what the game draws with the second generator is not witnessed: shown as "
	                    "the file holds it.";
	out.push_back({second_param,
			[](const void *d, int) -> Value {
				ThreediMaterial &m = const_cast<ThreediMaterial &>(as<ThreediMaterial>(d));
				return generator_param({&m.rgb_gen2.style, &m.rgb_gen2.phase, &m.rgb_gen2.reg, &m.rgb_gen2.rate});
			},
			nullptr, nullptr,
			[](const void *d, int) { return threedi_generator_names_register(as<ThreediMaterial>(d).rgb_gen2.style); }});
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
	static const std::vector<Choice> styles = style_choices();
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
	static const std::vector<Choice> names = [] {
		std::vector<Choice> out;
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
		Field cell = on_row(realf(kCells[k], kCellLabels[k]), kRows[k / 3]);
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
		// A blink box's letters as the manual and the add-on write them (V, S, W, L, O after BB in the
		// name): a letter clears its bit from 0x3E (the add-on's BLINK_LETTER_BITS), so a letter is checked
		// where its bit is clear; the game reads the bits (render-occlusion-re D-OCC-8). Read on a blink box
		// alone (type 8 [orig: @ 0x4aea68]).
		{integer("letters", 0, 0x3E, "Letters",
				 {{"V", 0x2, "V"}, {"S", 0x4, "S"}, {"W", 0x8, "W"}, {"L", 0x10, "L"}, {"O", 0x20, "O"}}, true),
				[](const void *d, int) -> Value { return int64_t(~as<ThreediBoundingVolume>(d).flags & 0x3E); },
				[](void *d, const Value &v, int, std::string &) {
					int32_t &flags = as<ThreediBoundingVolume>(d).flags;
					flags = (flags & ~0x3E) | (~static_cast<int32_t>(whole(v)) & 0x3E);
					return true;
				},
				[](const void *d, int) { return as<ThreediBoundingVolume>(d).collidable_type == 8; }},
		// The flags word as the file holds it (each letter's bit set where the letter is absent; the upper
		// bytes ride as they are).
		{integer("flags", INT32_MIN, INT32_MAX, "Flag bits",
				 {{"bit_2", 0x2, "0x2 (no V)"}, {"bit_4", 0x4, "0x4 (no S)"}, {"bit_8", 0x8, "0x8 (no W)"},
				  {"bit_10", 0x10, "0x10 (no L)"}, {"bit_20", 0x20, "0x20 (no O)"}},
				 true),
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
		{integer("poly_type", 0, 255, "Surface"),
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

// The value in the field's own type: a whole Real reads as a number, a whole number
// fills a Real; false when it does not fit.
bool normalize(const Field &f, const Value &in, Value &out, std::string &error) {
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
		out = v; // non-finite: refused unless the field already reads it (set_entry)
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


// --- the shape ------------------------------------------------------------------------------------

// The heading a field's first step groups it under.
const char *section_of(const std::string &path) {
	static const struct {
		const char *step, *label;
	} kSections[] = {
		{"position", "Position"}, {"direction", "Direction"}, {"start", "Start colour"}, {"end", "End colour"},
		{"flags", "Flags"}, {"texanim", "Flipbook"}, {"reflect", "Reflection"}, {"rgbgen", "Colour generator"},
		{"alphagen", "Alpha generator"}, {"ugen", "U generator"}, {"vgen", "V generator"}, {"offset", "Offset"},
		{"rotx", "Rotation X track"}, {"roty", "Rotation Y track"}, {"rotz", "Rotation Z track"},
		{"scalex", "Scale X track"}, {"scaley", "Scale Y track"}, {"scalez", "Scale Z track"},
		{"trans", "Translation track"}, {"rgbgen2", "Second colour generator"}, {"reflect2", "Second reflection"},
	};
	const size_t dot = path.find('.');
	if (dot == std::string::npos) return "";
	const std::string step = path.substr(0, dot);
	for (const auto &s : kSections)
		if (step == s.step) return s.label;
	return "";
}

FieldSchema field_of(const Field &f) {
	FieldSchema out;
	out.id = f.path;
	out.type = f.type == Type::Integer ? FieldType::Integer : f.type == Type::Real ? FieldType::Real : FieldType::Text;
	out.width = f.width;
	// What it may name outside its record: a texture's file; a CTRL register or an MTRX row of the
	// model by its index, a Record reference (S13 D8), which the document keeps where the record's
	// other fields make it one. A part of LOD 0 names no record (LOD 0's parts are the base's).
	out.reference = f.reference == Ref::Texture    ? ReferenceKind::Texture
	                : f.reference == Ref::Register ? ReferenceKind::ModelRegister
	                : f.reference == Ref::Frame    ? ReferenceKind::ModelFrame
	                                               : ReferenceKind::None;
	for (const Choice &c : f.choices) out.choices.push_back({c.name, c.value, c.label});
	out.flags = f.flags;
	out.read_only = f.read_only;
	out.unit = f.unit;
	out.group = f.group;
	out.description = f.note;
	if (f.channel) out.color = FieldColor::Channel;
	if (f.unverified) out.applies = Applicability::Unverified;
	// A part of LOD 0 takes any other index typed beside the parts a record offers of its own
	// (ModelDocument::record_choices).
	if (f.reference == Ref::Part) out.open_choices = true;
	// An integer keeps to the range its record's word holds (the set refuses past it).
	if (f.type == Type::Integer && f.min < f.max) {
		out.ranged = true;
		out.min = double(f.min);
		out.max = double(f.max);
	}
	out.label = f.label; // the table's own ("" = the path, which field_title shows)
	out.section = section_of(f.path);
	return out;
}

// The shader tags the engine's table knows, as the shader field's choices.
std::vector<FieldChoice> shader_choices() {
	std::vector<FieldChoice> out;
	for (size_t i = 0; i < renderer::kMaterialDescriptorTableCount; ++i)
		out.push_back({renderer::kMaterialDescriptorTable[i].name, static_cast<int64_t>(i), ""});
	return out;
}

// A value within the field's type, width and range; a Set of the value the field already reads changes
// nothing (numbers compared bit for bit, so a NaN a retail word holds, US01's MTRX, matches itself); a
// non-finite number is refused.
bool set_entry(const Entry &e, void *native, const Value &value, std::string &error) {
	if (e.set == nullptr) {
		error = "This field is read-only: it is authored in Blender or derived.";
		return false;
	}
	Value v;
	if (!normalize(e.field, value, v, error)) return false;
	// The value the field already reads: nothing changes, not even a derived word.
	const Value current = e.get(native, e.arg);
	const double *now = std::get_if<double>(&current);
	const double *next = std::get_if<double>(&v);
	if (now != nullptr && next != nullptr ? std::memcmp(now, next, sizeof(double)) == 0 : current == v) return true;
	if (next != nullptr && !std::isfinite(*next)) {
		error = "A finite number.";
		return false;
	}
	return e.set(native, v, e.arg, error);
}

// The native struct a kind's entries read: a model row's header, a LOD's ThreediLod, a material's
// ThreediMaterial, every other record's own.
void *native_of(Shape shape, const RecordHandle &record) {
	switch (shape) {
	case Shape::Model: return &record.as<ModelRow>().header;
	case Shape::Lod: return &record.as<ModelLod>().lod;
	case Shape::Material: return &record.as<ModelMaterial>().material;
	default: return record.data;
	}
}

// The records' kinds (ModelKind's order) and the entries each reads (the collision row: none).
struct KindRow {
	ModelKind kind;
	const char *token;
	const char *label;
	Shape shape;
	bool fields; // the collision row holds records, no fields
};
const KindRow kKinds[] = {
	{ModelKind::Model, "model", "Model", Shape::Model, true},
	{ModelKind::Collision, "collision", "Collision", Shape::Model, false},
	{ModelKind::Lod, "lod", "LOD", Shape::Lod, true},
	{ModelKind::PartAnimation, "part_animation", "Part animation", Shape::PartAnimation, true},
	{ModelKind::Material, "material", "Material", Shape::Material, true},
	{ModelKind::Texture, "texture", "Texture", Shape::Texture, true},
	{ModelKind::Light, "light", "Light", Shape::Light, true},
	{ModelKind::UserPoint, "user_point", "User point", Shape::UserPoint, true},
	{ModelKind::Register, "register", "Register", Shape::Register, true},
	{ModelKind::Frame, "frame", "Rotation frame", Shape::Frame, true},
	{ModelKind::Section, "section", "Section", Shape::Section, true},
	{ModelKind::Volume, "volume", "Volume", Shape::Volume, true},
	{ModelKind::Face, "face", "Bullet face", Shape::Face, true},
	{ModelKind::Occlusion, "occlusion", "Occlusion record", Shape::Occlusion, true},
};

// --- the lists ----------------------------------------------------------------------------------------

ThreediMaterial fresh_material() {
	ThreediBuildModel build;
	build.add_material("FF_ST_OP", nullptr);
	return build.materials.front();
}

ThreediLight fresh_light() {
	ThreediBuildModel build;
	const int white[3] = {255, 255, 255};
	build.add_light(ThreediBuildVec3{0.0, 0.0, 0.0}, 1.0, 10.0, 0, 0, white, white);
	return build.lights.front();
}

ThreediUserPoint fresh_user_point(const ModelRow &row) {
	std::string name;
	for (int n = 1;; ++n) {
		char buf[16];
		std::snprintf(buf, sizeof(buf), "POINT%02d", n);
		bool taken = false;
		for (const ThreediUserPoint &u : row.user_points) taken = taken || strutil::iequals(u.name, buf);
		if (!taken) {
			name = buf;
			break;
		}
	}
	ThreediBuildModel build;
	build.add_user_point(name.c_str(), ThreediBuildVec3{0.0, 0.0, 0.0}, ThreediBuildVec3{1.0, 0.0, 0.0}, -1,
	                     THREEDI_USER_POINT_GAMEPLAY);
	return build.user_points.front();
}

ThreediControlRegister fresh_register() {
	ThreediControlRegister made{};
	std::snprintf(made.name, sizeof(made.name), "%s", "LOD_FRAC");
	return made;
}

ThreediMatrix4x4 fresh_frame() {
	ThreediMatrix4x4 identity;
	threedi_mat4_identity(&identity);
	return identity;
}

// A material's texture rows: the fixed table of 24 its MTRL record holds, the rows in use first (its
// texture count) and the rest zero, as an edit leaves them. A new row is a diffuse one.
constexpr size_t kTextureRows = 24;
ListOps texture_list(NodeKind kind) {
	const auto used = [](const ThreediMaterial &m) { return std::min<size_t>(m.texture_count, kTextureRows); };
	ListOps ops;
	ops.size = [used](const RecordHandle &owner) { return used(owner.as<ModelMaterial>().material); };
	ops.at = [used, kind](const RecordHandle &owner, size_t index) {
		ThreediMaterial &m = owner.as<ModelMaterial>().material;
		return index < used(m) ? RecordHandle{kind, &m.textures[index]} : RecordHandle{};
	};
	ops.insert = [used, kind](const RecordHandle &owner, size_t index, const DetachedRecord *record, std::string &error) {
		ThreediMaterial &m = owner.as<ModelMaterial>().material;
		if (record && (!record->data || record->kind != kind)) {
			error = "This list takes records of its own kind only.";
			return false;
		}
		std::vector<ThreediMaterialTexture> rows(m.textures, m.textures + used(m));
		if (rows.size() >= kTextureRows) {
			error = "A material holds 24 texture rows.";
			return false;
		}
		ThreediMaterialTexture made{};
		if (record) made = *static_cast<const ThreediMaterialTexture *>(record->data.get());
		else made.slot = static_cast<uint8_t>(THREEDI_TEX_SLOT_DIFFUSE);
		rows.insert(rows.begin() + std::ptrdiff_t(std::min(index, rows.size())), made);
		std::memset(m.textures, 0, sizeof(m.textures));
		std::copy(rows.begin(), rows.end(), m.textures);
		m.texture_count = static_cast<uint32_t>(rows.size());
		return true;
	};
	ops.erase = [used](const RecordHandle &owner, size_t index) {
		ThreediMaterial &m = owner.as<ModelMaterial>().material;
		std::vector<ThreediMaterialTexture> rows(m.textures, m.textures + used(m));
		if (index >= rows.size()) return false;
		rows.erase(rows.begin() + std::ptrdiff_t(index));
		std::memset(m.textures, 0, sizeof(m.textures));
		std::copy(rows.begin(), rows.end(), m.textures);
		m.texture_count = static_cast<uint32_t>(rows.size());
		return true;
	};
	ops.copy = [used, kind](const RecordHandle &owner, size_t index) {
		DetachedRecord out;
		const ThreediMaterial &m = owner.as<ModelMaterial>().material;
		if (index >= used(m)) return out;
		out.kind = kind;
		out.data = std::make_shared<ThreediMaterialTexture>(m.textures[index]);
		return out;
	};
	return ops;
}

Document::CollectionSpec spec(ModelKind kind, const char *label, const char *name_field, bool fixed, size_t max = 0) {
	Document::CollectionSpec s;
	s.kind = node_kind(kind);
	s.label = label;
	s.name_field = name_field;
	s.fixed = fixed;
	s.max = max;
	return s;
}

// The model row's lists (its LODs, materials, lights, user points, CTRL registers and MTRX rows, in
// kModelLods's order, which model_table_test pins), a LOD's part animations and a material's texture
// rows, and the collision row's sections, volumes, bullet faces and occlusion records.
void add_lists(ModelKind kind, TableKind &table) {
	switch (kind) {
	case ModelKind::Model:
		table.list({spec(ModelKind::Lod, "LODs", "", true),
		            vector_list<ModelRow, ModelLod>(node_kind(ModelKind::Lod), [](ModelRow &r) -> std::vector<ModelLod> & { return r.lods; })});
		// A material's record name is its place ("Material 2"), not its shader tag: two materials share
		// a tag (the Blackhawk's two FF_ST_OP), and the graph's paths, the import plan's Needed by and
		// the Problems rows read record names (its words, texture and shader, are its title).
		table.list({spec(ModelKind::Material, "Materials", "", false),
		            vector_list<ModelRow, ModelMaterial>(
		                    node_kind(ModelKind::Material), [](ModelRow &r) -> std::vector<ModelMaterial> & { return r.materials; },
		                    [](const ModelRow &, size_t) { return ModelMaterial{fresh_material(), -1}; })});
		table.list({spec(ModelKind::Light, "Lights", "", false),
		            vector_list<ModelRow, ThreediLight>(
		                    node_kind(ModelKind::Light), [](ModelRow &r) -> std::vector<ThreediLight> & { return r.lights; },
		                    [](const ModelRow &, size_t) { return fresh_light(); })});
		table.list({spec(ModelKind::UserPoint, "User points", "name", false),
		            vector_list<ModelRow, ThreediUserPoint>(
		                    node_kind(ModelKind::UserPoint),
		                    [](ModelRow &r) -> std::vector<ThreediUserPoint> & { return r.user_points; },
		                    [](const ModelRow &r, size_t) { return fresh_user_point(r); })});
		table.list({spec(ModelKind::Register, "CTRL registers", "name", false),
		            vector_list<ModelRow, ThreediControlRegister>(
		                    node_kind(ModelKind::Register),
		                    [](ModelRow &r) -> std::vector<ThreediControlRegister> & { return r.registers; },
		                    [](const ModelRow &, size_t) { return fresh_register(); })});
		table.list({spec(ModelKind::Frame, "Rotation frames", "", false),
		            vector_list<ModelRow, ThreediMatrix4x4>(
		                    node_kind(ModelKind::Frame), [](ModelRow &r) -> std::vector<ThreediMatrix4x4> & { return r.frames; },
		                    [](const ModelRow &, size_t) { return fresh_frame(); })});
		break;
	case ModelKind::Lod:
		// Row i transforms part i, as every retail table does (threedi_o3d_read's rule): the next part's
		// inert row, added at the end (ModelDocument::list_position).
		table.list({spec(ModelKind::PartAnimation, "Part animations", "", false),
		            vector_list<ModelLod, ThreediPartAnimation>(
		                    node_kind(ModelKind::PartAnimation),
		                    [](ModelLod &l) -> std::vector<ThreediPartAnimation> & { return l.panm; },
		                    [](const ModelLod &l, size_t index) {
			                    const int part = static_cast<int>(index);
			                    const int parent = part < static_cast<int>(l.lod.render_object_count)
			                                               ? l.lod.render_objects[part].parent_index
			                                               : -1;
			                    return threedi_build_inert_panm(part, parent);
		                    })});
		break;
	case ModelKind::Material:
		table.list({spec(ModelKind::Texture, "Textures", "name", false, kTextureRows), texture_list(node_kind(ModelKind::Texture))});
		break;
	case ModelKind::Collision:
		table.list({spec(ModelKind::Section, "Sections", "", true),
		            vector_list<CollisionRow, ThreediCollisionObject>(
		                    node_kind(ModelKind::Section),
		                    [](CollisionRow &r) -> std::vector<ThreediCollisionObject> & { return r.sections; })});
		table.list({spec(ModelKind::Volume, "Volumes", "", true),
		            vector_list<CollisionRow, ThreediBoundingVolume>(
		                    node_kind(ModelKind::Volume),
		                    [](CollisionRow &r) -> std::vector<ThreediBoundingVolume> & { return r.volumes; })});
		table.list({spec(ModelKind::Face, "Bullet faces", "", true),
		            vector_list<CollisionRow, ThreediCollisionFace>(
		                    node_kind(ModelKind::Face), [](CollisionRow &r) -> std::vector<ThreediCollisionFace> & { return r.faces; })});
		table.list({spec(ModelKind::Occlusion, "Occlusion records", "", true),
		            vector_list<CollisionRow, ThreediOcclusionObject>(
		                    node_kind(ModelKind::Occlusion),
		                    [](CollisionRow &r) -> std::vector<ThreediOcclusionObject> & { return r.occlusion; })});
		break;
	default: break;
	}
}

// What each kind's fields are to the model's own rules (model_document_detail::ModelField), by place.
std::vector<std::vector<model_document_detail::ModelField>> &facts() {
	static std::vector<std::vector<model_document_detail::ModelField>> table(std::size(kKinds));
	return table;
}

model_document_detail::IndexReference index_kind(Ref reference) {
	using I = model_document_detail::IndexReference;
	switch (reference) {
	case Ref::Texture: return I::Texture;
	case Ref::Register: return I::Register;
	case Ref::Part: return I::Part;
	case Ref::Frame: return I::Frame;
	default: return I::None;
	}
}

RecordTable make_table() {
	std::vector<TableKind> kinds;
	for (const KindRow &row : kKinds) {
		const NodeKind kind = node_kind(row.kind);
		const bool top = row.kind == ModelKind::Model || row.kind == ModelKind::Collision;
		TableKind table(RecordKindRow{kind, row.token, row.label, "", top});
		std::vector<model_document_detail::ModelField> &own = facts()[size_t(kind)];
		if (row.fields)
			for (const Entry &entry : entries(row.shape)) {
				const Entry *e = &entry;
				const Shape shape = row.shape;
				LabelledField field;
				field.schema = field_of(e->field);
				if (row.kind == ModelKind::Material && field.schema.id == "shader") field.schema.choices = shader_choices();
				// A bullet face's surface by the game's names (model_surfaces.h); any other byte typed.
				if (row.kind == ModelKind::Face && field.schema.id == "poly_type") {
					field.schema.choices = model_surface_choices();
					field.schema.open_choices = true;
				}
				// A user point's name is what an item's particle slot looks it up by.
				if (row.kind == ModelKind::UserPoint && field.schema.id == "name") field.schema.defines = ReferenceKind::UserPoint;
				field.value.get = [e, shape](const RecordHandle &record, Value &out) {
					out = e->get(native_of(shape, record), e->arg);
					return true;
				};
				field.value.set = [e, shape](const RecordHandle &record, const Value &value, std::string &error) {
					return set_entry(*e, native_of(shape, record), value, error);
				};
				// Whether the game reads it here (a generator's register only above style 0x70 and its phase
				// only at or below [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640], a PANM track only when its
				// flags make it present [orig: PANM_SampleTrack @ 0x5B2270], a spot light's axis); a field
				// whose use the witness leaves open stays Unverified.
				if (e->reads && !e->field.unverified)
					field.applies = [e, shape](const RecordHandle &record, const RecordOwners &) {
						return e->reads(native_of(shape, record), e->arg) ? Applicability::Reads : Applicability::Ignored;
					};
				// What it names here: a texture row's file, a register where its style makes the byte one.
				if (e->names && e->field.reference != Ref::None) {
					const ReferenceKind declared = field.schema.reference;
					field.reference = [e, shape, declared](const RecordHandle &record, const RecordOwners &) {
						return e->names(native_of(shape, record), e->arg) ? declared : ReferenceKind::None;
					};
				}
				model_document_detail::ModelField fact;
				fact.reference = index_kind(e->field.reference);
				fact.unverified = e->field.unverified;
				fact.reads = [e, shape](const RecordHandle &record) {
					return e->reads == nullptr || e->reads(native_of(shape, record), e->arg);
				};
				fact.names = [e, shape](const RecordHandle &record) {
					return e->names == nullptr || e->names(native_of(shape, record), e->arg);
				};
				own.push_back(std::move(fact));
				table.field(std::move(field));
			}
		add_lists(row.kind, table);
		kinds.push_back(std::move(table));
	}
	return RecordTable(std::move(kinds));
}

} // namespace

const RecordTable &model_table() {
	static const RecordTable table = make_table();
	return table;
}

namespace model_document_detail {

const ModelField &model_field(NodeKind kind, size_t place) {
	model_table(); // the facts are made with the table
	return facts()[size_t(kind)][place];
}

} // namespace model_document_detail

} // namespace opennova::editor
