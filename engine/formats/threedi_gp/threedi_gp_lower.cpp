// The 3DI3 to GP lowering (threedi_gp_lower.h): threedi_gp_migrate.cpp's map run backwards,
// section by section, each 3DI3 word turned into the GP word the migration reads it from. The
// words the migration drops take what BHD's model tool writes [orig: sub_4488F0, the GP save
// (df4oed)]; docs/threedi/3di-gp-format-re.md, "3DI3 to GP", is the map.

#include <formats/threedi_gp/threedi_gp_lower.h>

#include <algorithm>
#include <cmath>
#include <cstring>

#include <base/io/strutil.h>

#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_strip_decode.h>

namespace opennova::threedi_gp {
namespace {

using namespace opennova::threedi;

class Notes {
public:
	explicit Notes(std::vector<MigrateNote> &out) : out_(out) {}
	void add(const std::string &text, int count = 1) {
		if (count <= 0) return;
		for (MigrateNote &n : out_)
			if (n.text == text) {
				n.count += count;
				return;
			}
		out_.push_back({text, count});
	}

private:
	std::vector<MigrateNote> &out_;
};

// The GP word a 3DI3 float was quantized from (16.16, Q14 or 8.8 over its scale).
int32_t fp16(double v) { return static_cast<int32_t>(std::lround(v * 65536.0)); }
int16_t q14(double v) { return static_cast<int16_t>(std::lround(v * 16384.0)); }
int16_t q8(double v) { return static_cast<int16_t>(std::lround(v * 256.0)); }
int16_t times256(double v) { return static_cast<int16_t>(std::lround(v * 256.0)); }

std::string field(const char *s, size_t cap) {
	size_t n = 0;
	while (n < cap && s[n] != '\0') ++n;
	return std::string(s, n);
}

// A LOD's render tag. The tool packs the tag's four characters into one dword, first character
// highest [orig: GP_WriteModel @ 0x448C38 (df4oed)], so "gnrc" is stored as the bytes "crng" both
// engines search their packed table for; a migrated 3DI3 already holds the stored bytes and keeps
// them. The readable tags are the ones BHD's corpus ships.
uint32_t render_tag(const char model_type[5]) {
	static const char *const kTags[] = {"gnrc", "bldg", "door", "veh0", "org0", "tree", "cpln", "ewpn"};
	const uint8_t *b = reinterpret_cast<const uint8_t *>(model_type);
	for (const char *tag : kTags)
		if (std::memcmp(model_type, tag, 4) == 0)
			return (uint32_t(b[0]) << 24) | (uint32_t(b[1]) << 16) | (uint32_t(b[2]) << 8) | b[3];
	return uint32_t(b[0]) | (uint32_t(b[1]) << 8) | (uint32_t(b[2]) << 16) | (uint32_t(b[3]) << 24);
}

Generator generator(uint8_t style, float phase, int32_t reg, double rate, double start, double end, double scale) {
	Generator g;
	g.style = style;
	g.param = threedi_generator_param_byte(style, phase, reg);
	g.rate = times256(rate);
	g.start = static_cast<int16_t>(std::lround(start * scale));
	g.end = static_cast<int16_t>(std::lround(end * scale));
	return g;
}

Generator track(const ThreediTransform &t) {
	Generator g;
	g.style = t.control;
	g.param = t.control_param;
	g.rate = t.rate;
	g.start = t.start;
	g.end = t.end;
	return g;
}

bool ends_with(const std::string &s, const char *suffix) {
	const size_t n = std::strlen(suffix);
	return s.size() >= n && s.compare(s.size() - n, n, suffix) == 0;
}

// The GP state a JO technique was migrated from (threedi_gp_migrate.cpp, technique()).
struct Technique {
	bool bump = false;
	uint8_t shader = 0;   // +0x32: the bump technique
	bool paired = false;  // +0x3D: a second stage in the next record
	uint8_t blend = 1;    // +0x3C: 1 opaque, 2 alpha, 4 additive
	bool lum = false;     // _LUM: unlit
};

bool technique(std::string tag, Technique &t) {
	if (ends_with(tag, "#UV")) tag.resize(tag.size() - 3); // the U and V generators say it
	if (tag.rfind("FF_ST_", 0) == 0 || tag.rfind("FF_MT_", 0) == 0) {
		t.paired = tag[3] == 'M';
		std::string rest = tag.substr(6);
		if (ends_with(rest, "_LUM")) {
			t.lum = true;
			rest.resize(rest.size() - 4);
		}
		if (rest == "OP") t.blend = 1;
		else if (rest == "AB") t.blend = 2;
		else if (rest == "AD") t.blend = 4;
		else return false;
		return true;
	}
	// GPM bump 4 is VS_PHONGT's (BHD ships 97 materials of shader 4 to 4 of shader 2); 1, 11 and
	// 14 VS_DOT3DIFF's, VS_PHONGO's and VS_TRACER's [the migration's technique map].
	t.bump = true;
	if (tag == "VS_PHONGT") t.shader = 4;
	else if (tag == "VS_DOT3DIFF") t.shader = 1;
	else if (tag == "VS_DOT3DIFF2") {
		t.shader = 1;
		t.paired = true;
	} else if (tag == "VS_PHONGO") t.shader = 11;
	else if (tag == "VS_TRACER") t.shader = 14;
	else return false;
	return true;
}

std::string stem(const std::string &name) {
	const size_t dot = name.find_last_of('.');
	return dot == std::string::npos ? name : name.substr(0, dot);
}

struct MaterialLower {
	const Threedi3di3 &model;
	const LowerOptions &options;
	Notes &notes;
	std::string &error;
	std::vector<TextureRow> &rows;
	bool tangents;
	bool &bump_any;
	bool &team_used;

	// A texture row by name, one a spelling: the tool folds case [orig: sub_443080
	// @ 0x44311F..0x4431E4 (df4oed)], but BHD ships rows a case apart (Jglow.tga, jglow.tga) and
	// the 3DI3 keeps each, so each keeps its row. A colour row's flags are the material's 0x200
	// and 7 [orig: sub_4484E0 @ 0x448506, @ 0x448570 (df4oed)]; the loader tests only 0x8 and
	// 0x100 [orig: @ 0x516070..0x516091 (dfbhd)].
	int texture(const std::string &name) {
		if (name.empty()) return -1;
		for (const TextureRow &r : rows)
			if (r.name == name) return r.id;
		if (name.size() > 15) {
			error = "a texture name is longer than 15 characters: " + name;
			return -2;
		}
		TextureRow row;
		row.name = name;
		row.id = static_cast<int16_t>(rows.size());
		row.flags = 0x207;
		row.width = row.height = 256;
		uint16_t w = 0, h = 0;
		if (options.texture_size && options.texture_size(name, w, h)) {
			row.width = w;
			row.height = h;
		} else {
			notes.add("a texture's size was not found beside the model; its row says 256 x 256");
		}
		rows.push_back(row);
		return row.id;
	}

	// One MTRL record as the GP records it migrated from: the record, its second stage when
	// paired, a team flipbook's frames after it.
	bool build(const ThreediMaterial &m, std::vector<Material> &out) {
		const std::string tag = m.shader_name;
		Technique t;
		if (!technique(tag, t)) {
			error = "material " + std::to_string(m.index) + "'s technique " + tag + " has no Black Hawk Down form";
			return false;
		}
		if (t.bump && !tangents) {
			notes.add("a bump material has no tangents; it draws without its normal map");
			t = Technique{};
		}
		// Each slot's rows by frame (a flipbook's frame byte; a frame the migration found no
		// texture for has no row and draws untextured).
		std::vector<std::string> diffuse, detail;
		std::string normal;
		auto place = [](std::vector<std::string> &frames, const ThreediMaterialTexture &row, const std::string &name) {
			const size_t at = (row.flags & THREEDI_TEX_FLAG_ANIMATED) ? row.frame : frames.size();
			if (frames.size() <= at) frames.resize(at + 1);
			frames[at] = name;
		};
		for (uint32_t i = 0; i < m.texture_count; ++i) {
			const ThreediMaterialTexture &row = m.textures[i];
			const std::string name = field(row.name, sizeof(row.name));
			if (row.slot == THREEDI_TEX_SLOT_DIFFUSE) place(diffuse, row, name);
			else if (row.slot == THREEDI_TEX_SLOT_DETAIL) place(detail, row, name);
			else if (row.slot == THREEDI_TEX_SLOT_NORMAL && normal.empty()) normal = name;
		}
		if (diffuse.empty()) diffuse.push_back(std::string());
		// A team flipbook: the migration's TEX_TEAM selector is BHD's counter 4.
		int frames = 1;
		if (m.animation.num_frames > 1 && m.animation.animation_type == 1) {
			const int reg = m.animation.cycle_frame_time;
			if (reg >= 0 && static_cast<uint32_t>(reg) < model.ctrl.count &&
					std::strcmp(model.ctrl.registers[reg].name, "TEX_TEAM") == 0) {
				frames = m.animation.num_frames;
				diffuse.resize(static_cast<size_t>(frames));
				team_used = true;
			} else {
				notes.add("a flipbook on a JO control BHD has no counter for keeps its first frame");
			}
		} else if (m.animation.num_frames > 1) {
			notes.add("a timed flipbook keeps its first frame (BHD's counters 1 and 2 have no JO source)");
		}
		if (t.bump) {
			bump_any = true;
			// BHD reads a bump material's normal map as `<texture>.mdt` beside the model, else its
			// texture's alpha [orig: Texture_LoadAsNormalMap @ 0x4FAEBD..0x4FAF5F (dfbhd)].
			if (!normal.empty() && !strutil::iequals(normal, diffuse[0]) && !strutil::iequals(normal, stem(diffuse[0]) + ".mdt"))
				notes.add("BHD reads a bump material's normal map as <texture>.mdt: give " + normal + " the name " +
						stem(diffuse[0]) + ".mdt beside the model");
		}
		const bool test = (m.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) != 0;
		Material base;
		// The record is named after its texture, as BHD's corpus names most of them; no reader.
		base.name = diffuse[0].substr(0, 15);
		// [orig: GP_WriteRenderModel @ 0x4478F8..0x447A41 (df4oed): the attribute bits]
		base.attributes = ((m.material_flags & THREEDI_MATERIAL_FLAG_TWO_SIDED) ? kMaterialTwoSided : 0u) |
				(test ? ((m.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_INVERT) ? kMaterialAlphaTestInv : kMaterialAlphaTest) : 0u) |
				(t.lum ? kMaterialUnlit : 0u) | (t.bump ? kMaterialBump : 0u) | (frames > 1 ? kMaterialFlipbook : 0u) |
				((m.u_params.style || m.v_params.style) ? 0x20u : 0u) | (m.rgb_gen.style ? 0x40u : 0u) |
				(m.alpha_gen.style ? 0x80u : 0u);
		base.frame_count = static_cast<int8_t>(frames > 1 ? frames : 0);
		base.frame_counter = frames > 1 ? 4 : 0;
		base.alpha_texture = 0;
		base.shader = t.shader;
		base.colour_source = 2;
		base.alpha_source = (t.blend == 2 || test) ? 2 : 0;
		base.alpha_ref = test ? m.alpha_test_value_byte : 0;
		base.blend = t.blend;
		base.u = generator(m.u_params.style, m.u_params.phase, m.u_params.reg, m.u_params.gen_rate, m.u_params.start,
				m.u_params.end, 256.0);
		base.v = generator(m.v_params.style, m.v_params.phase, m.v_params.reg, m.v_params.gen_rate, m.v_params.start,
				m.v_params.end, 256.0);
		base.alpha_gen = generator(m.alpha_gen.style, m.alpha_gen.phase, m.alpha_gen.reg, m.alpha_gen.rate,
				m.alpha_gen.start, m.alpha_gen.end, 1.0);
		const ThreediRgbGen &c = m.rgb_gen;
		base.rgb.style = c.style;
		base.rgb.param = threedi_generator_param_byte(c.style, c.phase, c.reg);
		base.rgb.rate = times256(c.rate);
		for (int k = 0; k < 3; ++k) {
			base.rgb.start[static_cast<size_t>(k)] = static_cast<uint8_t>(std::lround(c.start_color[2 - k] * 255.0));
			base.rgb.end[static_cast<size_t>(k)] = static_cast<uint8_t>(std::lround(c.end_color[2 - k] * 255.0));
		}
		base.rgb.start[3] = static_cast<uint8_t>(std::lround(c.start_color[3] * 255.0));
		base.rgb.end[3] = static_cast<uint8_t>(std::lround(c.end_color[3] * 255.0));
		for (int f = 0; f < frames; ++f) {
			Material r = base;
			const int id = texture(diffuse[static_cast<size_t>(f)]);
			if (id == -2) return false;
			r.region_textures = {static_cast<int8_t>(id), static_cast<int8_t>(id), static_cast<int8_t>(id)};
			r.paired = t.paired ? 1 : 0;
			out.push_back(r);
			if (t.paired) {
				// The second stage: the next record, whose texture is the detail row [orig:
				// GPMaterial_ApplyForDraw @ 0x4FD4D6..0x4FD526 (dfbhd)].
				Material second = base;
				const size_t at = std::min(static_cast<size_t>(f), detail.empty() ? 0 : detail.size() - 1);
				const int sid = texture(detail.empty() ? std::string() : detail[at]);
				if (sid == -2) return false;
				second.region_textures = {static_cast<int8_t>(sid), static_cast<int8_t>(sid), static_cast<int8_t>(sid)};
				second.paired = 0;
				out.push_back(second);
			}
		}
		return true;
	}
};

// --- collision (mission axes on both sides) ----------------------------------------------

// A 3DI3 box {min x, y, z, max x, y, z} as GP's interleaved one.
std::array<int32_t, 6> interleave(const int32_t mn[3], const int32_t mx[3]) {
	return {mn[0], mx[0], mn[1], mx[1], mn[2], mx[2]};
}

void collision(const ThreediCollisionModel *c, Collision &out) {
	if (c == nullptr) return;
	const ThreediCollisionModelData &d = c->model_data;
	int32_t mn[3], mx[3];
	for (int k = 0; k < 3; ++k) {
		mn[k] = d.has_bbox_fp16 ? d.bbox_fp16[k] : fp16(d.bbox[k]);
		mx[k] = d.has_bbox_fp16 ? d.bbox_fp16[3 + k] : fp16(d.bbox[3 + k]);
		out.radii[static_cast<size_t>(k)] = fp16(d.radii[k]);
	}
	out.box = interleave(mn, mx);
	// The vertices carry their section's ordinal, as the tool writes them (279,799 of BHD's
	// 327,466); no BHD reader takes it.
	size_t vi = 0;
	for (size_t s = 0; s < c->object_count; ++s)
		for (int32_t k = 0; k < c->objects[s].num_vertices && vi < c->vertex_count; ++k, ++vi) {
			const ThreediCollisionVertex &v = c->vertices[vi];
			out.vertices.push_back({q8(v.position[0]), q8(v.position[1]), q8(v.position[2]), static_cast<int16_t>(s)});
		}
	for (; vi < c->vertex_count; ++vi) {
		const ThreediCollisionVertex &v = c->vertices[vi];
		out.vertices.push_back({q8(v.position[0]), q8(v.position[1]), q8(v.position[2]), 0});
	}
	for (size_t i = 0; i < c->normal_count; ++i) {
		const ThreediCollisionNormal &n = c->normals[i];
		out.normals.push_back({q14(n.normal[0]), q14(n.normal[1]), q14(n.normal[2]), n.dominate_axis});
	}
	for (size_t i = 0; i < c->face_count; ++i) {
		const ThreediCollisionFace &f = c->faces[i];
		CollisionFace o;
		for (int k = 0; k < 3; ++k) o.corners[static_cast<size_t>(k)] = f.vert_index[k];
		o.normal = f.normal_index;
		o.plane = f.plane_dist_fp16;
		o.box = {f.min_x_fp16, f.max_x_fp16, f.min_y_fp16, f.max_y_fp16, f.min_z_fp16, f.max_z_fp16};
		o.flags = f.material_flags;
		o.surface = f.poly_type;
		out.faces.push_back(o);
	}
	vi = 0;
	for (size_t i = 0; i < c->object_count; ++i) {
		const ThreediCollisionObject &s = c->objects[i];
		CollisionSection o;
		o.flags = s.unk0;
		o.vertex_count = s.num_vertices;
		o.face_count = s.num_faces;
		o.normal_count = s.num_normals;
		o.volume_count = s.num_bounding_volumes;
		o.parent = s.parent_subobject_index;
		for (int k = 0; k < 3; ++k) {
			o.offset[static_cast<size_t>(k)] = s.offset[k];
			o.centre[static_cast<size_t>(k)] = s.med[k];
		}
		o.box = interleave(s.min, s.max);
		o.radius = s.radius;
		// The xy radius about the box's centre over the section's vertices, and the box's
		// height, as the tool derives them [orig: GP_WriteCollision @ 0x4446FE..0x4449ED (df4oed)].
		const double cx = (static_cast<double>(s.min[0]) + s.max[0]) * 0.5;
		const double cy = (static_cast<double>(s.min[1]) + s.max[1]) * 0.5;
		double rxy = 0.0;
		for (int32_t k = 0; k < s.num_vertices && vi < c->vertex_count; ++k, ++vi) {
			const double dx = c->vertices[vi].position[0] * 65536.0 - cx;
			const double dy = c->vertices[vi].position[1] * 65536.0 - cy;
			rxy = std::max(rxy, std::sqrt(dx * dx + dy * dy));
		}
		o.radius_xy = static_cast<int32_t>(rxy);
		o.height = s.max[2] - s.min[2];
		out.sections.push_back(o);
	}
	for (size_t i = 0; i < c->translation_count; ++i) {
		const int32_t *t = c->translations[i].translation;
		out.translations.push_back({t[0], t[1], t[2]});
	}
	for (size_t i = 0; i < c->plane_count; ++i) {
		// The migration's Q14 normal is the 16.16 one over 4 (its two low bits are lost).
		const ThreediBoundingPlane &p = c->planes[i];
		out.planes.push_back({{q14(p.normal[0]) * 4, q14(p.normal[1]) * 4, q14(p.normal[2]) * 4}, fp16(p.radius)});
	}
	for (size_t i = 0; i < c->volume_count; ++i) {
		const ThreediBoundingVolume &v = c->volumes[i];
		CollisionVolume o;
		o.type = v.collidable_type;
		o.flags = v.flags;
		const int32_t vmn[3] = {v.min_x_fp16, v.min_y_fp16, v.min_z_fp16};
		const int32_t vmx[3] = {v.max_x_fp16, v.max_y_fp16, v.max_z_fp16};
		o.box = interleave(vmn, vmx);
		o.plane_count = v.plane_count;
		// The group the tool stores and no dfbhd reader takes: the box's centre, half extents
		// and the radii over them [orig: GP_WriteCollision @ 0x444D44..0x444DDE (df4oed)].
		double h[3];
		for (int k = 0; k < 3; ++k) {
			o.centre[static_cast<size_t>(k)] = static_cast<int32_t>((static_cast<int64_t>(vmn[k]) + vmx[k]) / 2);
			h[k] = (static_cast<double>(vmx[k]) - vmn[k]) * 0.5;
			o.half[static_cast<size_t>(k)] = static_cast<int32_t>(h[k]);
		}
		o.radius = static_cast<int32_t>(std::sqrt(h[0] * h[0] + h[1] * h[1] + h[2] * h[2]));
		o.radius_xy = static_cast<int32_t>(std::sqrt(h[0] * h[0] + h[1] * h[1]));
		out.volumes.push_back(o);
	}
}

// A part's animation row from its type bytes (the migration's part_animation() run backwards):
// rotation 1 the spinner (its three float rates in the first 12 track bytes), 2 Euler, 3 and 4
// the view modes; scale 1 uniform, 2 three-axis.
bool part_animation(const ThreediPartAnimation &a, PartAnimation &out, std::string &error) {
	out.parent = a.parent_subobject;
	out.part = a.subobject_index;
	out.frame = static_cast<int8_t>(a.matrix_index);
	const uint8_t scale = threedi_panm_scale_type(a.flags);
	const uint8_t rotation = threedi_panm_rotation_type(a.flags);
	if (threedi_panm_translate_type(a.flags) != 0) {
		error = "a part animation translates its part, which BHD's part animations cannot";
		return false;
	}
	if (scale > 2 || rotation > 4) {
		error = "a part animation's type has no Black Hawk Down form";
		return false;
	}
	if (scale == 0 && rotation == 0) return true;
	out.flags = kPanmAnimated;
	if (rotation == 1) out.flags |= kPanmSpinner;
	if (rotation == 2) out.flags |= kPanmEuler | (threedi_panm_rotation_reversed(a.flags) ? kPanmEulerZxy : 0u);
	if (rotation == 3) out.flags |= kPanmViewA;
	if (rotation == 4) out.flags |= kPanmViewB;
	if (scale == 1) out.flags |= kPanmScaleUniform;
	if (scale == 2) out.flags |= kPanmScale3;
	if (rotation == 1 || rotation == 2) {
		out.tracks[0] = track(a.rotation_x);
		out.tracks[1] = track(a.rotation_y);
		if (rotation == 2) out.tracks[2] = track(a.rotation_z);
		if (rotation == 1) {
			uint8_t b[12];
			std::memcpy(b, &a.rotation_x, 8);
			std::memcpy(b + 8, &a.rotation_y, 4);
			std::memcpy(out.spin.data(), b, sizeof(b));
		}
	}
	if (scale >= 1) out.tracks[3] = track(a.scale_x);
	if (scale == 2) {
		out.tracks[4] = track(a.scale_y);
		out.tracks[5] = track(a.scale_z);
	}
	return true;
}

} // namespace

bool lower(const Threedi3di3 &model, File &out, std::vector<MigrateNote> &note_list, std::string &error,
		const LowerOptions &options) {
	out = File{};
	note_list.clear();
	error.clear();
	Notes notes(note_list);
	if (model.lods == nullptr || model.lod_count == 0 || model.lod_count > static_cast<size_t>(kMaxLods)) {
		error = "a GP model holds 1 to 4 LODs; this one holds " + std::to_string(model.lod_count);
		return false;
	}
	bool skinned = model.header.mesh_type == THREEDI_MESH_SKINNED;
	for (size_t li = 0; li < model.lod_count; ++li)
		skinned = skinned || (model.lods[li].vertices.flags & THREEDI_VERTEX_FLAG_SKINNED) != 0;
	if (skinned) {
		error = "the model is skinned; only rigid models (GPM) lower to GP so far";
		return false;
	}
	Header &h = out.header;
	h.kind = Kind::Gpm;
	h.revision = kMaxRevision; // [orig: GP_WriteModel @ 0x448B9A (df4oed)]
	h.name = options.name.empty() ? field(model.header.name, sizeof(model.header.name)) : options.name;
	if (h.name.size() > 15) {
		error = "the model's name is longer than 15 characters: " + h.name;
		return false;
	}
	h.lod_count = static_cast<int32_t>(model.lod_count);
	h.radius = model.header.max_radius_fp16;
	if (model.collision != nullptr) {
		h.radius_xy = fp16(model.collision->model_data.radii[1]);
		h.height = fp16(model.collision->model_data.radii[2]);
	}

	for (size_t i = 0; i < model.user_point_count; ++i) {
		const ThreediUserPoint &p = model.user_points[i];
		UserPoint o;
		o.position = {p.x, p.y, p.z};
		o.direction = {p.rot_x, p.rot_y, p.rot_z};
		o.part = p.subobject_index;
		o.type = static_cast<uint8_t>(p.userpoint_type);
		o.name = std::string(threedi_user_point_name(p));
		out.user_points.push_back(o);
	}

	collision(model.collision, out.collision);

	bool tangents = true;
	for (size_t li = 0; li < model.lod_count; ++li)
		tangents = tangents && (model.lods[li].vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) != 0;
	bool bump_any = false, team_used = false;
	MaterialLower ml{model, options, notes, error, out.textures, tangents, bump_any, team_used};
	std::vector<std::array<float, 6>> frames; // each vertex's tangent S and bitangent T
	out.lods.resize(model.lod_count);
	for (size_t li = 0; li < model.lod_count; ++li) {
		const ThreediLod &lod = model.lods[li];
		RenderModel &rm = out.lods[li];
		h.thresholds[li] = lod.lod_threshold * 65536;
		h.tags[li] = render_tag(lod.model_type);
		// The LOD's vertices join the model's; its primitives count from where they start.
		const int32_t base = static_cast<int32_t>(out.vertices.size());
		for (uint32_t i = 0; i < lod.vertices.count; ++i) {
			const ThreediVertex &v = lod.vertices.items[i];
			Vertex o;
			std::memcpy(o.position.data(), v.position, sizeof(v.position));
			std::memcpy(o.normal.data(), v.normal, sizeof(v.normal));
			std::memcpy(o.uv0.data(), v.uv0, sizeof(v.uv0));
			std::memcpy(o.uv1.data(), v.uv1, sizeof(v.uv1));
			out.vertices.push_back(o);
			// The stream holds S and T, BHD's shaders taking (S, -T, N) [the migration's rule].
			frames.push_back({v.tangent[0], v.tangent[1], v.tangent[2], -v.bitangent[0], -v.bitangent[1], -v.bitangent[2]});
		}
		// The collision block's translations, as floats [orig: GP_WriteRenderModel @ 0x4470D5..0x44712C (df4oed)].
		for (const auto &t : out.collision.translations)
			rm.attach_points.push_back({t[0] / 65536.0f, t[1] / 65536.0f, t[2] / 65536.0f});
		std::vector<int> slot_of(model.material_count, -1); // MTRL index -> this LOD's first record
		size_t cursor = 0;
		for (size_t pi = 0; pi < lod.render_object_count; ++pi) {
			const ThreediRenderObject &ro = lod.render_objects[pi];
			Part p;
			p.parent = ro.parent_index; // BHD's roots name themselves or -1, as stored
			const float *q = ro.parent_index >= 0 && static_cast<size_t>(ro.parent_index) < lod.render_object_count
					? lod.render_objects[ro.parent_index].abs : ro.abs;
			for (int k = 0; k < 3; ++k) {
				p.abs[static_cast<size_t>(k)] = ro.abs[k];
				p.centre[static_cast<size_t>(k)] = ro.bounding_center[k];
			}
			// rel, which no BHD reader takes, as the tool computes it: its pivot record holds the
			// model's (z, -x, y), and it subtracts the parent's record one word along [orig:
			// GP_WriteRenderModel @ 0x447190..0x4471E7 (df4oed)].
			p.rel = {-(ro.abs[2] + q[0]), -ro.abs[0] - q[1], ro.abs[1] - q[2]};
			p.radius = fp16(ro.bounding_radius);
			rm.parts.push_back(p);
			Batch batch;
			batch.part = static_cast<int>(pi);
			batch.opaque_count = ro.num_strips;
			batch.alpha_count = ro.num_alpha_strips;
			rm.batches.push_back(batch);
			const int32_t count = ro.num_strips + ro.num_alpha_strips;
			for (int32_t s = 0; s < count; ++s, ++cursor) {
				if (cursor >= lod.strip_count) {
					error = "a render object counts more strips than its LOD holds";
					return false;
				}
				const ThreediTriangleStrip &st = lod.strips[cursor];
				const int mi = threedi_material_array_index_for_id(model, st.material_index);
				if (mi < 0) {
					error = "a strip names a material the model does not hold";
					return false;
				}
				if (slot_of[static_cast<size_t>(mi)] < 0) {
					slot_of[static_cast<size_t>(mi)] = static_cast<int>(rm.materials.size());
					if (!ml.build(model.materials[mi], rm.materials)) return false;
				}
				if (st.index_offset < 0 || static_cast<size_t>(st.index_offset) + st.num_indices > lod.indices.count ||
						st.start_vertex < 0 || st.num_vertices < 1 ||
						static_cast<uint32_t>(st.start_vertex) + static_cast<uint32_t>(st.num_vertices) > lod.vertices.count) {
					error = "a strip's indices or vertices run past its LOD's";
					return false;
				}
				Primitive prim;
				prim.part = static_cast<int>(pi);
				prim.alpha = s >= ro.num_strips;
				prim.material = slot_of[static_cast<size_t>(mi)];
				prim.topology = st.is_strip != 0 ? 1 : 0;
				prim.first_vertex = base + st.start_vertex;
				prim.vertex_word = st.num_vertices - 1; // GPM stores the last index [the migration's rule]
				prim.indices.assign(lod.indices.indices + st.index_offset, lod.indices.indices + st.index_offset + st.num_indices);
				rm.primitives.push_back(std::move(prim));
			}
		}
		if (cursor != lod.strip_count) notes.add("a strip no render object counts is dropped (neither game draws it)",
				static_cast<int>(lod.strip_count - cursor));
		// Part animations: a table only when a row is typed (the migration writes none otherwise).
		bool live = false;
		for (size_t i = 0; i < lod.part_animation_count; ++i)
			live = live || (lod.part_animations[i].flags & 0xFF00FFFFu) != 0;
		if (live) {
			if (lod.part_animation_count != rm.parts.size()) {
				error = "a LOD's part animations are not one a part";
				return false;
			}
			rm.flags |= kRenderPartAnimations;
			for (size_t i = 0; i < lod.part_animation_count; ++i) {
				PartAnimation a;
				if (!part_animation(lod.part_animations[i], a, error)) return false;
				rm.part_animations.push_back(a);
			}
		}
	}
	if (bump_any) {
		h.flags |= kFlagVertexStream;
		out.vertex_stream.resize(frames.size() * 24);
		std::memcpy(out.vertex_stream.data(), frames.data(), out.vertex_stream.size());
	}

	// CTRL: the names in order, but a TEX_TEAM the migration appended for a team flipbook
	// (BHD's counter 4 needs no register) when nothing else names it.
	for (uint32_t i = 0; i < model.ctrl.count; ++i) out.control_registers.push_back(field(model.ctrl.registers[i].name, 25));
	if (team_used && !out.control_registers.empty() && out.control_registers.back() == "TEX_TEAM") {
		const int last = static_cast<int>(out.control_registers.size()) - 1;
		bool named = false;
		auto names = [&](uint8_t style, int32_t reg) { return threedi_generator_names_register(style) && reg == last; };
		for (uint32_t i = 0; i < model.material_count && !named; ++i) {
			const ThreediMaterial &m = model.materials[i];
			named = names(m.alpha_gen.style, m.alpha_gen.reg) || names(m.rgb_gen.style, m.rgb_gen.reg) ||
					names(m.u_params.style, m.u_params.reg) || names(m.v_params.style, m.v_params.reg);
		}
		for (size_t i = 0; i < model.light_count && !named; ++i)
			named = threedi_generator_names_register(model.lights[i].style) && model.lights[i].phase == last;
		for (size_t li = 0; li < model.lod_count && !named; ++li)
			for (size_t i = 0; i < model.lods[li].part_animation_count && !named; ++i) {
				const ThreediPartAnimation &a = model.lods[li].part_animations[i];
				for (const ThreediTransform *t : {&a.rotation_x, &a.rotation_y, &a.rotation_z, &a.scale_x, &a.scale_y, &a.scale_z})
					named = named || (threedi_generator_names_register(t->control) && t->control_param == last);
			}
		if (!named) out.control_registers.pop_back();
	}
	for (uint32_t i = 0; i < model.mtrx.count; ++i) {
		std::array<float, 16> m;
		std::memcpy(m.data(), model.mtrx.matrices[i].m, sizeof(m));
		out.matrices.push_back(m);
	}

	// Lights: the head of JO's light is BHD's whole record; a spot's cone has no BHD form.
	for (size_t i = 0; i < model.light_count; ++i) {
		const ThreediLight &l = model.lights[i];
		Light o;
		o.style = l.style;
		o.phase = l.phase;
		o.rate = l.rate;
		std::memcpy(&o.colour_start, l.color_start, 4);
		std::memcpy(&o.colour_end, l.color_end, 4);
		std::memcpy(o.position.data(), l.offset, sizeof(l.offset));
		o.atten_start = l.atten_start;
		o.atten_end = l.atten_end;
		o.part = l.subobj_index;
		if (l.flags & THREEDI_LIGHT_FLAG_TYPE_TARGET) notes.add("a spot light is an omni light under BHD");
		out.lights.push_back(o);
	}
	if (!out.lights.empty()) h.flags |= kFlagLights;

	// Occlusion: each OOBJ with its runs of OVRT, OPLN and OFAC; the sphere the tool stores is
	// the one the loader recomputes.
	size_t ov = 0, op = 0, of = 0;
	for (size_t i = 0; i < model.occlusion_object_count; ++i) {
		const ThreediOcclusionObject &src = model.occlusion_objects[i];
		OcclusionObject o;
		o.type = src.type;
		o.parent = src.parent_subobject_index;
		o.connecting = src.connecting_subobject;
		std::memcpy(o.centre.data(), src.position, sizeof(src.position));
		o.radius = src.radius;
		for (int32_t k = 0; k < src.num_vertices && ov < model.occlusion_vertex_count; ++k, ++ov) {
			const float *p = model.occlusion_vertices[ov].position;
			o.vertices.push_back({p[0], p[1], p[2]});
		}
		for (int32_t k = 0; k < src.num_planes && op < model.occlusion_plane_count; ++k, ++op) {
			const ThreediOcclusionPlane &p = model.occlusion_planes[op];
			o.planes.push_back({p.normal[0], p.normal[1], p.normal[2], p.radius});
		}
		for (int32_t k = 0; k < src.face_count && of < model.occlusion_face_count; ++k, ++of) {
			const ThreediOcclusionFace &f = model.occlusion_faces[of];
			o.faces.push_back({f.raw_indices, f.edge_data, f.other_edge_data});
		}
		out.occlusion.push_back(std::move(o));
	}
	if (!out.occlusion.empty()) h.flags |= kFlagOcclusion;
	h.vertex_count = static_cast<int32_t>(out.vertices.size());
	h.user_point_count = static_cast<int32_t>(out.user_points.size());
	h.control_register_count = static_cast<int32_t>(out.control_registers.size());
	h.matrix_count = static_cast<int32_t>(out.matrices.size());
	h.occlusion_count = static_cast<int32_t>(out.occlusion.size());
	return true;
}

} // namespace opennova::threedi_gp
