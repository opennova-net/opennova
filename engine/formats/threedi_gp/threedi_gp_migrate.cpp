// The GP to 3DI3 migration (threedi_gp_migrate.h). Section by section, the words BHD's loader
// keeps become the 3DI3 words JO's loader turns into the same runtime values; the map and its
// witnesses are docs/threedi/3di-gp-format-re.md, "GP to 3DI3". The model is assembled over
// owned arrays and serialized by the 3DI3 parity writer, every float set to the value the
// writer quantizes back to the GP word (a 16.16, Q14 or 8.8 word over its scale).

#include <formats/threedi_gp/threedi_gp_migrate.h>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <map>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>

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

// The model the writer serializes and the arrays its pointers name.
struct Assembled {
	Threedi3di3 model{};
	std::vector<ThreediLod> lods;
	std::vector<std::vector<ThreediVertex>> vertices;
	std::vector<std::vector<uint16_t>> indices;
	std::vector<std::vector<ThreediTriangleStrip>> strips;
	std::vector<std::vector<ThreediRenderObject>> parts;
	std::vector<std::vector<ThreediPartAnimation>> panm;
	std::vector<ThreediMaterial> materials;
	std::vector<ThreediLight> lights;
	std::vector<ThreediUserPoint> user_points;
	std::vector<ThreediControlRegister> registers;
	std::vector<ThreediMatrix4x4> matrices;
	ThreediCollisionModel collision{};
	std::vector<ThreediBoundingPlane> planes;
	std::vector<ThreediBoundingVolume> volumes;
	std::vector<ThreediCollisionVertex> collision_vertices;
	std::vector<ThreediCollisionNormal> normals;
	std::vector<ThreediCollisionFace> faces;
	std::vector<ThreediCollisionObject> objects;
	std::vector<ThreediCollisionTranslation> translations;
	std::vector<ThreediOcclusionVertex> occ_vertices;
	std::vector<ThreediOcclusionPlane> occ_planes;
	std::vector<ThreediOcclusionFace> occ_faces;
	std::vector<ThreediOcclusionObject> occ_objects;
};

bool skinned(Kind kind) { return kind != Kind::Gpm; }

void copy_name(char *dst, size_t size, const std::string &src) {
	std::memset(dst, 0, size);
	std::memcpy(dst, src.data(), std::min(src.size(), size - 1));
}

// --- materials -----------------------------------------------------------------------

ThreediAlphaGen alpha_gen(const Generator &g) {
	ThreediAlphaGen a{};
	a.style = g.style;
	threedi_generator_split_param_byte(g.style, g.param, &a.phase, &a.reg);
	a.rate = static_cast<float>(g.rate) / 256.0f;
	a.start = g.start;
	a.end = g.end;
	return a;
}

ThreediUvParams uv_params(const Generator &g) {
	ThreediUvParams u{};
	u.style = g.style;
	threedi_generator_split_param_byte(g.style, g.param, &u.phase, &u.reg);
	u.gen_rate = static_cast<float>(g.rate) / 256.0f;
	u.start = static_cast<float>(g.start) / 256.0f;
	u.end = static_cast<float>(g.end) / 256.0f;
	return u;
}

// The colours are D3DCOLOR bytes B, G, R, x, as the 3DI3 block stores them.
ThreediRgbGen rgb_gen(const RgbGenerator &g) {
	ThreediRgbGen c{};
	c.style = g.style;
	threedi_generator_split_param_byte(g.style, g.param, &c.phase, &c.reg);
	c.rate = static_cast<float>(g.rate) / 256.0f;
	for (int k = 0; k < 3; ++k) {
		c.start_color[2 - k] = static_cast<float>(g.start[static_cast<size_t>(k)]) / 255.0f;
		c.end_color[2 - k] = static_cast<float>(g.end[static_cast<size_t>(k)]) / 255.0f;
	}
	c.start_color[3] = static_cast<float>(g.start[3]) / 255.0f;
	c.end_color[3] = static_cast<float>(g.end[3]) / 255.0f;
	return c;
}

// A generator whose style's high nibble is 0 is inactive: the alpha it gives is 1.0 [orig:
// AlphaGen_EvaluateValue @ 0x4FC35D (dfbhd)].
bool active(uint8_t style) { return (style >> 4) != 0; }

std::string normal_name(const std::string &texture) {
	const size_t dot = texture.find_last_of('.');
	return (dot == std::string::npos ? texture : texture.substr(0, dot)) + ".mdt";
}

// Whether two MTRL records are the same record field for field (padding never compared).
bool same_material(const ThreediMaterial &a, const ThreediMaterial &b) {
	auto same_alpha = [](const ThreediAlphaGen &x, const ThreediAlphaGen &y) {
		return x.style == y.style && x.phase == y.phase && x.reg == y.reg && x.rate == y.rate && x.start == y.start &&
				x.end == y.end;
	};
	auto same_rgb = [](const ThreediRgbGen &x, const ThreediRgbGen &y) {
		return x.style == y.style && x.phase == y.phase && x.reg == y.reg && x.rate == y.rate &&
				std::equal(x.start_color, x.start_color + 4, y.start_color) &&
				std::equal(x.end_color, x.end_color + 4, y.end_color);
	};
	auto same_uv = [](const ThreediUvParams &x, const ThreediUvParams &y) {
		return x.style == y.style && x.phase == y.phase && x.reg == y.reg && x.gen_rate == y.gen_rate &&
				x.start == y.start && x.end == y.end;
	};
	if (std::strcmp(a.shader_name, b.shader_name) != 0 || a.texture_count != b.texture_count) return false;
	for (uint32_t i = 0; i < a.texture_count; ++i) {
		const ThreediMaterialTexture &x = a.textures[i], &y = b.textures[i];
		if (std::strcmp(x.name, y.name) != 0 || x.slot != y.slot || x.type != y.type || x.flags != y.flags ||
				x.frame != y.frame)
			return false;
	}
	return a.material_flags == b.material_flags && same_alpha(a.alpha_gen, b.alpha_gen) &&
			same_rgb(a.rgb_gen, b.rgb_gen) && same_rgb(a.rgb_gen2, b.rgb_gen2) && same_uv(a.u_params, b.u_params) &&
			same_uv(a.v_params, b.v_params) && std::equal(a.reflect_color, a.reflect_color + 4, b.reflect_color) &&
			std::equal(a.reflect_color2, a.reflect_color2 + 4, b.reflect_color2) &&
			a.emissive_type == b.emissive_type && a.emissive_type2 == b.emissive_type2 && a.is_glass == b.is_glass &&
			a.glass_type2 == b.glass_type2 && a.alpha_test_value_byte == b.alpha_test_value_byte &&
			a.animation.num_frames == b.animation.num_frames &&
			a.animation.animation_type == b.animation.animation_type &&
			a.animation.cycle_frame_time == b.animation.cycle_frame_time;
}

struct MaterialBuild {
	const File &gp;
	const MigrateOptions &options;
	Notes &notes;
	std::vector<std::string> &registers;
	std::string &error;
	bool has_stream;

	// The texture the record draws for the chosen region [orig: GP_LoadRenderModel
	// @ 0x515D16..0x515D89 (dfbhd)]: the region's row when the colour comes from the texture,
	// else +0x2B's when only the alpha does, else none.
	std::string texture(const Material &m) {
		int id = -1;
		if (m.colour_source == 2) {
			id = m.region_textures[static_cast<size_t>(options.region)];
			for (int8_t other : m.region_textures)
				if (other != id) {
					notes.add("a material's texture depends on BHD's mission region; region " +
							std::to_string(options.region) + "'s is taken (JO has no regions)");
					break;
				}
		} else if (m.alpha_source == 2) {
			id = m.alpha_texture;
		}
		if (id < 0) return std::string();
		for (const TextureRow &row : gp.textures)
			if (row.id == id) {
				if ((row.flags & 0x108u) != 0 && !row.alpha.empty())
					notes.add("a texture's separate alpha file is not carried (JO takes the texture's own alpha)");
				return row.name;
			}
		notes.add("a material names a texture row the model does not hold; it draws untextured");
		return std::string();
	}

	// The JO technique for the record's state [orig: GPMaterial_BuildModeWord @ 0x5163F0;
	// GPMaterial_CompileGPM @ 0x516E70; GPMaterial_CompileShaderPath @ 0x5164B0 (dfbhd)].
	std::string technique(const Material &m, bool &lum, bool &bump) {
		const bool sk = skinned(gp.header.kind);
		const bool uv = m.u.style != 0 || m.v.style != 0;
		bump = (m.attributes & kMaterialBump) != 0 && m.shader != 0;
		if (bump && !has_stream) {
			notes.add("a bump material has no tangent stream; it draws without its normal map");
			bump = false;
		}
		if (bump) {
			notes.add("a bump material takes the JO technique of its structure; BHD's exact shading is not witnessed");
			lum = false;
			if (sk) {
				switch (m.shader) {
				case 11:
				case 12: return "VS_SKBUMPPHONGOBJ";
				case 8: return m.paired ? "VS_SKBUMPDIFFT2" : "VS_SKBUMPDIFFT";
				case 15: return "VS_SKBUMPDIFFT";
				case 16: return "VS_SKBUMPPHONGT";
				default: break;
				}
			} else {
				switch (m.shader) {
				case 1: return m.paired ? "VS_DOT3DIFF2" : (uv ? "VS_DOT3DIFF#UV" : "VS_DOT3DIFF");
				case 2:
				case 4:
					if (m.paired) notes.add("a bump material's second stage is dropped (JO's VS_PHONGT has none)");
					return uv ? "VS_PHONGT#UV" : "VS_PHONGT";
				case 11:
				case 12: return "VS_PHONGO";
				case 14: return "VS_TRACER";
				default: break;
				}
			}
			notes.add("a bump technique BHD's corpus never ships draws as a plain material");
			bump = false;
		}
		if (m.env_pass != 0) notes.add("an environment-map pass is dropped (JO has no lit texture with a cube pass)");
		if (m.attributes & kMaterialClamp) notes.add("a clamped texture wraps under JO");
		if (m.colour_source == 1) notes.add("a flat-colour (untextured) material has no JO form; it draws as a textured one");
		if (sk) {
			if (m.paired) notes.add("a skinned material's second stage is dropped (JO's VS_SKBASIC has none)");
			if (lum) notes.add("a skinned material's colour generator is dropped (JO's VS_SKBASIC has none)");
			if (m.blend > 1) notes.add("a skinned material's blend is dropped (JO's VS_SKBASIC is opaque)");
			lum = false;
			return uv ? "VS_SKBASIC#UV" : "VS_SKBASIC";
		}
		std::string blend = "OP";
		if (m.blend == 2) blend = "AB";
		else if (m.blend == 4) blend = "AD";
		else if (m.blend > 2) notes.add("a material's blend (one/src-alpha or multiply) has no JO form; it draws opaque");
		if (m.blend == 2 && lum) {
			notes.add("an alpha-blended self-lit material loses its colour generator (JO's _LUM alpha is 0)");
			lum = false;
		}
		if (lum && (m.attributes & kMaterialUnlit) == 0)
			notes.add("a lit material with a colour generator turns self-lit (JO's _LUM replaces the lighting)");
		if (lum && (m.attributes & (kMaterialAlphaTest | kMaterialAlphaTestInv)))
			notes.add("a self-lit alpha-tested material fails JO's test (JO's _LUM alpha is 0)");
		if (m.blend == 4) notes.add("an additive material fogs under JO (BHD draws it unfogged)");
		if (m.alpha_source != 2 && (m.blend == 2 || (m.attributes & (kMaterialAlphaTest | kMaterialAlphaTestInv))))
			notes.add("a material that ignored its texture's alpha takes it under JO");
		return std::string(m.paired ? "FF_MT_" : "FF_ST_") + blend + (lum ? "_LUM" : "") + (uv ? "#UV" : "");
	}

	bool add_row(ThreediMaterial &out, const std::string &name, uint8_t slot, uint8_t type, bool animated, int frame) {
		if (name.empty()) return true;
		if (out.texture_count >= 24) {
			error = "a material needs more than 24 texture rows";
			return false;
		}
		if (name.size() > 15) {
			error = "a texture name is longer than 15 characters: " + name;
			return false;
		}
		ThreediMaterialTexture &row = out.textures[out.texture_count++];
		copy_name(row.name, sizeof(row.name), name);
		row.slot = slot;
		row.type = type;
		row.flags = animated ? THREEDI_TEX_FLAG_ANIMATED : 0;
		row.frame = static_cast<uint8_t>(frame);
		return true;
	}

	int register_index(const char *name) {
		for (size_t i = 0; i < registers.size(); ++i)
			if (registers[i] == name) return static_cast<int>(i);
		registers.emplace_back(name);
		return static_cast<int>(registers.size()) - 1;
	}

	// One LOD's record `index` (with its second stage and flipbook frames) as a MTRL record.
	bool build(const std::vector<Material> &records, size_t index, ThreediMaterial &out) {
		std::memset(&out, 0, sizeof(out));
		const Material &m = records[index];
		const size_t step = m.paired ? 2 : 1;
		if (m.paired && index + 1 >= records.size()) {
			error = "a material's second stage runs past its LOD's materials";
			return false;
		}
		// A flipbook: the next records are its frames [orig: GPMaterial_ApplyForDraw
		// @ 0x4FD4D6..0x4FD526 (dfbhd)].
		int frames = 1;
		bool team = false;
		if ((m.attributes & kMaterialFlipbook) && m.frame_counter < 16 && m.frame_count > 0) {
			if (index + static_cast<size_t>(m.frame_count) * step > records.size()) {
				error = "a flipbook's frames run past its LOD's materials";
				return false;
			}
			if (m.frame_counter == 4) {
				// Counter 4 is the player's team, JO's TEX_TEAM [orig: sub_488A10 @ 0x488AD0 (dfbhd)].
				frames = m.frame_count;
				team = true;
			} else {
				notes.add("a flipbook on a BHD counter JO has no control for keeps its first frame");
			}
		}
		bool lum = (m.attributes & kMaterialUnlit) != 0 || active(m.rgb.style);
		bool bump = false;
		copy_name(out.shader_name, sizeof(out.shader_name), technique(m, lum, bump));
		const bool animated = frames > 1;
		std::vector<std::string> diffuse;
		for (int f = 0; f < frames; ++f) diffuse.push_back(texture(records[index + static_cast<size_t>(f) * step]));
		for (int f = 0; f < frames; ++f)
			if (!add_row(out, diffuse[static_cast<size_t>(f)], THREEDI_TEX_SLOT_DIFFUSE, THREEDI_TEX_TYPE_DIFFUSE, animated, f))
				return false;
		const std::string tag = out.shader_name;
		const bool second = tag.find("_MT_") != std::string::npos || tag.find("DIFF2") != std::string::npos ||
				tag.find("DIFFT2") != std::string::npos;
		if (second)
			for (int f = 0; f < frames; ++f)
				if (!add_row(out, texture(records[index + static_cast<size_t>(f) * step + 1]), THREEDI_TEX_SLOT_DETAIL,
						THREEDI_TEX_TYPE_DIFFUSE, animated, f))
					return false;
		if (bump)
			for (int f = 0; f < frames; ++f) {
				const std::string &name = diffuse[static_cast<size_t>(f)];
				if (name.empty()) continue;
				const std::string mdt = normal_name(name);
				const bool has_mdt = options.texture_exists && options.texture_exists(mdt);
				if (!add_row(out, has_mdt ? mdt : name, THREEDI_TEX_SLOT_NORMAL,
						has_mdt ? THREEDI_TEX_TYPE_NORMAL_MDT : THREEDI_TEX_TYPE_NORMAL_TGA, animated, f))
					return false;
			}
		if (team) {
			out.animation.num_frames = static_cast<uint8_t>(frames);
			out.animation.animation_type = 1;
			out.animation.cycle_frame_time = static_cast<int16_t>(register_index("TEX_TEAM"));
		}
		// The generators copy byte for byte; the two engines evaluate them alike [orig:
		// Material_ComputeUVTransformMatrix @ 0x518230; RgbGen_EvaluateColor @ 0x4FC170;
		// AlphaGen_EvaluateValue @ 0x4FC350 (dfbhd)].
		out.u_params = uv_params(m.u);
		out.v_params = uv_params(m.v);
		if (lum) {
			out.rgb_gen = rgb_gen(m.rgb);
			out.emissive_type = THREEDI_EMISSIVE_FULL;
		}
		Generator alpha = m.alpha_gen;
		if (m.alpha_source == 1) {
			// The constant alpha multiplies the generator's [orig: GPMaterial_ApplyD3DMaterial
			// @ 0x4FCEDF..0x4FCEEE (dfbhd)]: a constant generator when there is none.
			if (!active(alpha.style)) {
				alpha = Generator{24, 0, 0, m.alpha, m.alpha};
			} else {
				alpha.start = static_cast<int16_t>(std::lround(alpha.start * (m.alpha / 255.0)));
				alpha.end = static_cast<int16_t>(std::lround(alpha.end * (m.alpha / 255.0)));
			}
		}
		out.alpha_gen = alpha_gen(alpha);
		const bool test = (m.attributes & (kMaterialAlphaTest | kMaterialAlphaTestInv)) != 0;
		out.material_flags = static_cast<uint8_t>((test ? THREEDI_MATERIAL_FLAG_ALPHA_TEST : 0) |
				((m.attributes & kMaterialAlphaTestInv) ? THREEDI_MATERIAL_FLAG_ALPHA_INVERT : 0) |
				((m.attributes & kMaterialTwoSided) ? THREEDI_MATERIAL_FLAG_TWO_SIDED : 0));
		out.alpha_test_value_byte = test ? m.alpha_ref : 0;
		return true;
	}
};

// --- render ----------------------------------------------------------------------------

// The primitives BHD draws: each part's first batch (or the global table's), the only ones
// filled and drawn [orig: GP_FillModelIndexBuffer @ 0x515E30 (dfbhd)], with each part's opaque
// and alpha counts; `dropped` counts the batches past a part's first that hold any.
struct DrawnPrimitives {
	std::vector<const Primitive *> prims;
	std::vector<int> opaque, alpha;
	int dropped = 0;
};

DrawnPrimitives drawn_primitives(const RenderModel &rm) {
	DrawnPrimitives out;
	out.opaque.assign(rm.parts.size(), 0);
	out.alpha.assign(rm.parts.size(), 0);
	size_t cursor = 0;
	std::vector<bool> seen(rm.parts.size(), false);
	for (const Batch &b : rm.batches) {
		const size_t count = static_cast<size_t>(b.opaque_count) + static_cast<size_t>(b.alpha_count);
		const size_t part = static_cast<size_t>(b.part);
		if (part < seen.size() && !seen[part]) {
			seen[part] = true;
			for (size_t k = 0; k < count && cursor + k < rm.primitives.size(); ++k)
				out.prims.push_back(&rm.primitives[cursor + k]);
			out.opaque[part] = b.opaque_count;
			out.alpha[part] = b.alpha_count;
		} else if (count != 0) {
			++out.dropped;
		}
		cursor += count;
	}
	return out;
}

int32_t vertex_count(Kind kind, const Primitive &p) {
	// GPM and GPS store the last index, GPP the count [orig: GP_DrawLocalBatchRModel
	// @ 0x4FF5CB; GP_DrawSkinnedRModel @ 0x50056F (dfbhd)].
	return kind == Kind::Gpp ? p.vertex_word : p.vertex_word + 1;
}

// A part's animation row as JO's loader takes it [orig: GPM_LoadRenderModel @ 0x5B5425..0x5B56A6
// (Jointops)] for the row BHD's builder reads [orig: Model_TransformBoneMatrices @ 0x4FD850
// (dfbhd)]: the flag word becomes the scale, rotation and order type bytes.
ThreediPartAnimation part_animation(const PartAnimation &a, Notes &notes) {
	ThreediPartAnimation out{};
	out.parent_subobject = a.parent;
	out.subobject_index = a.part;
	out.matrix_index = static_cast<uint8_t>(a.frame);
	out.bind_matrix_index = a.frame;
	if ((a.flags & kPanmAnimated) == 0) return out;
	const uint8_t scale = (a.flags & kPanmScaleUniform) ? 1 : (a.flags & kPanmScale3) ? 2 : 0;
	uint8_t rotation = 0;
	if (a.flags & kPanmSpinner) rotation = 1;
	else if (a.flags & kPanmEuler) rotation = 2;
	else if (a.flags & kPanmViewA) rotation = 3;
	else if (a.flags & kPanmViewB) rotation = 4;
	const uint8_t order = ((a.flags & kPanmEuler) && (a.flags & kPanmEulerZxy)) ? 1 : 0;
	out.flags = static_cast<uint32_t>(scale) | (static_cast<uint32_t>(rotation) << 8) | (static_cast<uint32_t>(order) << 16);
	auto track = [](const Generator &g) { return threedi_build_track(g.style, g.param, g.rate, g.start, g.end); };
	if (rotation == 2) {
		out.rotation_x = track(a.tracks[0]);
		out.rotation_y = track(a.tracks[1]);
		out.rotation_z = track(a.tracks[2]);
	} else if (rotation == 1) {
		// JO's loader sets no flag for a spinner: the part stands still under JO; the rates
		// stay where BHD keeps them.
		notes.add("a spinning part (fan, rotor) stands still under JO, whose loader has no spinner");
		uint8_t b[12];
		std::memcpy(b, a.spin.data(), sizeof(b));
		auto i16 = [&](int at) { return static_cast<int16_t>(b[at] | (b[at + 1] << 8)); };
		out.rotation_x = threedi_build_track(b[0], b[1], i16(2), i16(4), i16(6));
		out.rotation_y.control = b[8];
		out.rotation_y.control_param = b[9];
		out.rotation_y.rate = i16(10);
	}
	if (scale >= 1) out.scale_x = track(a.tracks[3]);
	if (scale == 2) {
		out.scale_y = track(a.tracks[4]);
		out.scale_z = track(a.tracks[5]);
	}
	return out;
}

bool render_lod(const File &gp, size_t li, const std::vector<std::vector<int>> &remap, Assembled &a, Notes &notes,
		std::string &error) {
	const RenderModel &rm = gp.lods[li];
	const Kind kind = gp.header.kind;
	const bool sk = skinned(kind);
	const bool global = (rm.flags & kRenderGlobalBatch) != 0;
	const size_t nv = gp.vertices.size();
	const bool tangents = kind != Kind::Gps && !gp.vertex_stream.empty() && gp.vertex_stream.size() >= nv * 24;
	if (rm.extra_count != 0 || rm.record_count != 0) {
		error = "a render model holds extra primitives or records BHD never ships";
		return false;
	}
	const DrawnPrimitives drawn = drawn_primitives(rm);
	const std::vector<const Primitive *> &prims = drawn.prims;
	const std::vector<int> &part_opaque = drawn.opaque, &part_alpha = drawn.alpha;
	notes.add("a batch past a part's first is dropped (BHD never draws it)", drawn.dropped);
	// This LOD's window of the model's shared vertices.
	int32_t lo = 0, hi = 0;
	if (!prims.empty()) {
		lo = prims.front()->first_vertex;
		for (const Primitive *p : prims) {
			const int32_t n = vertex_count(kind, *p);
			if (p->first_vertex < 0 || n < 0 || static_cast<size_t>(p->first_vertex) + static_cast<size_t>(n) > nv) {
				error = "a primitive's vertices run past the model's";
				return false;
			}
			lo = std::min(lo, p->first_vertex);
			hi = std::max(hi, p->first_vertex + n);
		}
	}
	const uint32_t vflags = 1u | (sk ? THREEDI_VERTEX_FLAG_SKINNED : 0u) | (tangents ? THREEDI_VERTEX_FLAG_TANGENTS : 0u);
	std::vector<ThreediVertex> &verts = a.vertices[li];
	for (int32_t i = lo; i < hi; ++i) {
		const Vertex &v = gp.vertices[static_cast<size_t>(i)];
		ThreediVertex o{};
		std::memcpy(o.position, v.position.data(), sizeof(o.position));
		std::memcpy(o.normal, v.normal.data(), sizeof(o.normal));
		std::memcpy(o.uv0, v.uv0.data(), sizeof(o.uv0));
		std::memcpy(o.uv1, v.uv1.data(), sizeof(o.uv1));
		if (kind == Kind::Gpp) {
			std::memcpy(o.bone_weights, v.weights.data(), sizeof(o.bone_weights));
			std::memcpy(o.bone_indices, v.palette.data(), sizeof(o.bone_indices));
		} else if (kind == Kind::Gps) {
			// GPS blends two bones by one weight: beta on bone A, the rest on bone B [orig:
			// GP_DrawSkinOldRModel @ 0x5007FE..0x500819 (dfbhd)]; JO weighs slots 0..2 and gives
			// slot 3 the remainder, the dominant bone in slot 0.
			if (v.blend >= 0.5f) {
				o.bone_weights[0] = v.blend;
				const uint8_t idx[4] = {0, 0, 0, 1};
				std::memcpy(o.bone_indices, idx, 4);
			} else {
				o.bone_weights[0] = 1.0f - v.blend;
				const uint8_t idx[4] = {1, 1, 1, 0};
				std::memcpy(o.bone_indices, idx, 4);
			}
		}
		if (tangents) {
			// The stream holds S then T; BHD's shaders take (S, -T, N) [orig: dfbhd shader
			// text @ 0x651824], JO the stored tangent and bitangent.
			const uint8_t *s = gp.vertex_stream.data() + static_cast<size_t>(i) * 24;
			float st[6];
			std::memcpy(st, s, sizeof(st));
			for (int k = 0; k < 3; ++k) {
				o.tangent[k] = st[k];
				o.bitangent[k] = -st[3 + k];
			}
			o.has_tangents = 1;
		}
		o.flags = vflags;
		o.is_skinned = sk ? 1 : 0;
		verts.push_back(o);
	}
	std::vector<uint16_t> &indices = a.indices[li];
	std::vector<ThreediTriangleStrip> &strips = a.strips[li];
	for (const Primitive *p : prims) {
		ThreediTriangleStrip s{};
		if (p->material < 0 || static_cast<size_t>(p->material) >= remap[li].size() || remap[li][static_cast<size_t>(p->material)] < 0) {
			error = "a primitive names a material its LOD does not hold";
			return false;
		}
		if (p->topology != 0 && p->topology != 1) {
			error = "a primitive's topology is neither a list nor a strip";
			return false;
		}
		const size_t n = p->indices.size();
		s.material_index = remap[li][static_cast<size_t>(p->material)];
		s.index_offset = static_cast<int32_t>(indices.size());
		s.num_indices = static_cast<uint16_t>(n);
		s.num_triangles = static_cast<uint16_t>(p->topology == 1 ? (n >= 3 ? n - 2 : 0) : n / 3);
		s.is_strip = p->topology;
		s.start_vertex = p->first_vertex - lo;
		s.num_vertices = vertex_count(kind, *p);
		indices.insert(indices.end(), p->indices.begin(), p->indices.end());
		if (kind == Kind::Gpp) {
			std::memcpy(s.bone_table, p->palette.data(), sizeof(s.bone_table));
			s.bone_table_length = p->palette_length;
		} else if (kind == Kind::Gps) {
			// The two bones are the low bytes of the words at +24 and +28 [orig:
			// GP_LoadRenderModel @ 0x515934..0x515947 (dfbhd)].
			s.bone_table[0] = p->palette[0];
			s.bone_table[1] = p->palette[4];
			s.bone_table_length = 2;
		} else {
			// The box of the vertices the strip draws (JO keeps its centre).
			for (int k = 0; k < 3; ++k) {
				s.min[k] = 1e30f;
				s.max[k] = -1e30f;
			}
			for (uint16_t i : p->indices) {
				const size_t at = static_cast<size_t>(s.start_vertex) + i;
				if (at >= verts.size()) {
					error = "a primitive's index runs past its vertices";
					return false;
				}
				for (int k = 0; k < 3; ++k) {
					s.min[k] = std::min(s.min[k], verts[at].position[k]);
					s.max[k] = std::max(s.max[k], verts[at].position[k]);
				}
			}
			if (n == 0)
				for (int k = 0; k < 3; ++k) s.min[k] = s.max[k] = 0.0f;
		}
		strips.push_back(s);
	}
	std::vector<ThreediRenderObject> &parts = a.parts[li];
	for (size_t pi = 0; pi < rm.parts.size(); ++pi) {
		const Part &src = rm.parts[pi];
		ThreediRenderObject o{};
		o.num_strips = global ? (pi == 0 ? part_opaque[0] : 0) : part_opaque[pi];
		o.num_alpha_strips = global ? (pi == 0 ? part_alpha[0] : 0) : part_alpha[pi];
		o.parent_index = src.parent;
		const Part &parent = src.parent >= 0 && static_cast<size_t>(src.parent) < rm.parts.size()
				? rm.parts[static_cast<size_t>(src.parent)] : src;
		for (int k = 0; k < 3; ++k) {
			o.abs[k] = src.abs[static_cast<size_t>(k)];
			// BHD's rel has no reader; JO's is the pivot less its parent's.
			o.rel[k] = src.abs[static_cast<size_t>(k)] - parent.abs[static_cast<size_t>(k)];
			o.bounding_center[k] = src.centre[static_cast<size_t>(k)];
		}
		o.bounding_radius = static_cast<float>(src.radius) / 65536.0f;
		parts.push_back(o);
	}
	std::vector<ThreediPartAnimation> &panm = a.panm[li];
	bool live = false;
	for (const PartAnimation &row : rm.part_animations) {
		panm.push_back(part_animation(row, notes));
		live = live || (panm.back().flags & 0x0000FFFFu) != 0; // a scale or rotation type
	}
	// A table with no animated row: JO allocates none [orig: GPM_LoadRenderModel
	// @ 0x5B5447..0x5B5471 (Jointops)], so none is written.
	if (!live) panm.clear();
	ThreediLod &lod = a.lods[li];
	std::memcpy(lod.model_type, &gp.header.tags[li], 4);
	lod.model_type[4] = '\0';
	lod.lod_threshold = gp.header.thresholds[li] >> 16;
	lod.rmdl_render_object_count = static_cast<int32_t>(parts.size());
	lod.vertices.count = static_cast<uint32_t>(verts.size());
	lod.vertices.stride = 40u + (sk ? 16u : 0u) + (tangents ? 24u : 0u);
	lod.vertices.flags = vflags;
	lod.vertices.items = verts.data();
	lod.indices.count = static_cast<uint32_t>(indices.size());
	lod.indices.indices = indices.data();
	lod.strips = strips.data();
	lod.strip_count = strips.size();
	lod.strip_record_size = sk ? 68u : 48u;
	lod.render_objects = parts.data();
	lod.render_object_count = parts.size();
	lod.part_animations = panm.data();
	lod.part_animation_count = panm.size();
	lod.part_animation_record_size = 68u;
	return true;
}

// --- collision (mission axes on both sides) -----------------------------------------------

void collision(const File &gp, Assembled &a, Notes &notes) {
	const Collision &c = gp.collision;
	ThreediCollisionModel &out = a.collision;
	// The header's box is interleaved; CMDL groups the minima, then the maxima.
	const int order[6] = {0, 2, 4, 1, 3, 5};
	for (int k = 0; k < 6; ++k) {
		out.model_data.bbox_fp16[k] = c.box[static_cast<size_t>(order[k])];
		out.model_data.bbox[k] = static_cast<float>(c.box[static_cast<size_t>(order[k])]) / 65536.0f;
	}
	out.model_data.has_bbox_fp16 = 1;
	for (int k = 0; k < 3; ++k) out.model_data.radii[k] = static_cast<float>(c.radii[static_cast<size_t>(k)]) / 65536.0f;
	for (const CollisionVertex &v : c.vertices) {
		ThreediCollisionVertex o{};
		o.position[0] = static_cast<float>(v.x) / 256.0f;
		o.position[1] = static_cast<float>(v.y) / 256.0f;
		o.position[2] = static_cast<float>(v.z) / 256.0f;
		a.collision_vertices.push_back(o);
	}
	for (const CollisionNormal &n : c.normals) {
		ThreediCollisionNormal o{};
		o.normal[0] = static_cast<float>(n.x) / 16384.0f;
		o.normal[1] = static_cast<float>(n.y) / 16384.0f;
		o.normal[2] = static_cast<float>(n.z) / 16384.0f;
		o.dominate_axis = n.axis;
		a.normals.push_back(o);
	}
	int passing = 0;
	for (const CollisionFace &f : c.faces) {
		ThreediCollisionFace o{};
		for (int k = 0; k < 3; ++k) o.vert_index[k] = f.corners[static_cast<size_t>(k)];
		o.normal_index = f.normal;
		o.plane_dist_fp16 = f.plane;
		o.min_x_fp16 = f.box[0];
		o.max_x_fp16 = f.box[1];
		o.min_y_fp16 = f.box[2];
		o.max_y_fp16 = f.box[3];
		o.min_z_fp16 = f.box[4];
		o.max_z_fp16 = f.box[5];
		o.material_flags = f.flags;
		o.poly_type = f.surface;
		if ((f.flags & 0x2u) && !(f.flags & THREEDI_CFAC_FLAG_BULLETS_PASS)) ++passing;
		a.faces.push_back(o);
	}
	// BHD's projectile pool also lets rounds by on flag 0x2; JO's only on 0x100 [orig:
	// sub_48FF40 @ 0x490401 (dfbhd); Projectile_RaycastEntityPool @ 0x4E8E1C (Jointops)].
	notes.add("a face BHD's rounds pass through (flag 0x2) stops rounds under JO", passing);
	size_t volume_cursor = 0;
	for (const CollisionSection &s : c.sections) {
		ThreediCollisionObject o{};
		o.unk0 = s.flags;
		o.num_vertices = s.vertex_count;
		o.num_faces = s.face_count;
		o.num_normals = s.normal_count;
		o.num_bounding_volumes = s.volume_count;
		o.parent_subobject_index = s.parent;
		std::array<int32_t, 6> box = s.box;
		// The loader widens a section's box by its volumes' boxes, keeping its centre and
		// radius [orig: GP_LoadCollisionModel @ 0x514F4A..0x514F88 (dfbhd)].
		for (int32_t v = 0; v < s.volume_count && volume_cursor + static_cast<size_t>(v) < c.volumes.size(); ++v) {
			const std::array<int32_t, 6> &vb = c.volumes[volume_cursor + static_cast<size_t>(v)].box;
			for (int k = 0; k < 3; ++k) {
				box[static_cast<size_t>(2 * k)] = std::min(box[static_cast<size_t>(2 * k)], vb[static_cast<size_t>(2 * k)]);
				box[static_cast<size_t>(2 * k + 1)] = std::max(box[static_cast<size_t>(2 * k + 1)], vb[static_cast<size_t>(2 * k + 1)]);
			}
		}
		volume_cursor += static_cast<size_t>(std::max(0, s.volume_count));
		for (int k = 0; k < 3; ++k) {
			o.offset[k] = s.offset[static_cast<size_t>(k)];
			o.min[k] = box[static_cast<size_t>(2 * k)];
			o.max[k] = box[static_cast<size_t>(2 * k + 1)];
			o.med[k] = s.centre[static_cast<size_t>(k)];
		}
		o.radius = s.radius;
		a.objects.push_back(o);
	}
	for (const auto &t : c.translations) {
		ThreediCollisionTranslation o{};
		for (int k = 0; k < 3; ++k) o.translation[k] = t[static_cast<size_t>(k)];
		a.translations.push_back(o);
	}
	for (const CollisionPlane &p : c.planes) {
		// A 16.16 normal becomes Q14 by a truncating division (JO's Q14 loses BHD's two low bits).
		ThreediBoundingPlane o{};
		for (int k = 0; k < 3; ++k) o.normal[k] = static_cast<float>(p.normal[static_cast<size_t>(k)] / 4) / 16384.0f;
		o.radius = static_cast<float>(p.d) / 65536.0f;
		a.planes.push_back(o);
	}
	for (const CollisionVolume &v : c.volumes) {
		ThreediBoundingVolume o{};
		o.collidable_type = v.type;
		o.flags = v.flags;
		o.min_x_fp16 = v.box[0];
		o.max_x_fp16 = v.box[1];
		o.min_y_fp16 = v.box[2];
		o.max_y_fp16 = v.box[3];
		o.min_z_fp16 = v.box[4];
		o.max_z_fp16 = v.box[5];
		o.plane_count = v.plane_count;
		a.volumes.push_back(o);
	}
	ThreediCollisionModelData &d = out.model_data;
	d.num_vertices = static_cast<int32_t>(a.collision_vertices.size());
	d.num_normals = static_cast<int32_t>(a.normals.size());
	d.num_faces = static_cast<int32_t>(a.faces.size());
	d.num_objects = static_cast<int32_t>(a.objects.size());
	d.num_transforms = static_cast<int32_t>(a.translations.size());
	d.num_bounding_planes = static_cast<int32_t>(a.planes.size());
	d.num_bounding_volumes = static_cast<int32_t>(a.volumes.size());
	out.vertices = a.collision_vertices.data();
	out.vertex_count = a.collision_vertices.size();
	out.normals = a.normals.data();
	out.normal_count = a.normals.size();
	out.faces = a.faces.data();
	out.face_count = a.faces.size();
	out.objects = a.objects.data();
	out.object_count = a.objects.size();
	out.translations = a.translations.data();
	out.translation_count = a.translations.size();
	out.planes = a.planes.data();
	out.plane_count = a.planes.size();
	out.volumes = a.volumes.data();
	out.volume_count = a.volumes.size();
}

// --- occlusion (model axes) ------------------------------------------------------------

// The loader recomputes a record's sphere and its portal-slot weight from the vertices
// [orig: GP_LoadOcclusionData @ 0x5150C9..0x515258 (dfbhd)]; JO reads them as stored, the
// weight at OOBJ +20 [orig: ThreediGp_LoadOcclusionModelData @ 0x5B4B0C..0x5B4C72 (Jointops)].
void occlusion(const File &gp, Assembled &a) {
	for (const OcclusionObject &src : gp.occlusion) {
		ThreediOcclusionObject o{};
		o.type = src.type;
		o.parent_subobject_index = src.parent;
		o.connecting_subobject = src.connecting;
		const size_t n = src.vertices.size();
		double sum[3] = {0.0, 0.0, 0.0};
		for (const auto &v : src.vertices)
			for (int k = 0; k < 3; ++k) sum[k] += v[static_cast<size_t>(k)];
		const double inv = n != 0 ? 1.0 / static_cast<double>(n) : 0.0;
		const double c[3] = {sum[0] * inv, sum[1] * inv, sum[2] * inv};
		double r3 = 0.0, ryz = 0.0, rxz = 0.0, rxy = 0.0;
		for (const auto &v : src.vertices) {
			const double dx = v[0] - c[0], dy = v[1] - c[1], dz = v[2] - c[2];
			r3 = std::max(r3, std::sqrt(dx * dx + dy * dy + dz * dz));
			ryz = std::max(ryz, std::sqrt(dy * dy + dz * dz));
			rxz = std::max(rxz, std::sqrt(dx * dx + dz * dz));
			rxy = std::max(rxy, std::sqrt(dx * dx + dy * dy));
		}
		// dfbhd's pick, ties included: yz when strictly below both others, else xz when strictly
		// below both, else xy (so a yz/xz tie below xy takes xy).
		double r2 = rxy;
		if (ryz < rxz && ryz < rxy) r2 = ryz;
		else if (rxz < ryz && rxz < rxy) r2 = rxz;
		for (int k = 0; k < 3; ++k) o.position[k] = static_cast<float>(c[k]);
		o.radius = static_cast<float>(r3);
		o.slot_priority_scale = r3 > 0.0 ? static_cast<float>(r2 / r3) : 0.0f;
		o.num_vertices = static_cast<int32_t>(n);
		o.num_planes = static_cast<int32_t>(src.planes.size());
		o.face_count = static_cast<int32_t>(src.faces.size());
		a.occ_objects.push_back(o);
		for (const auto &v : src.vertices) {
			ThreediOcclusionVertex ov{};
			std::memcpy(ov.position, v.data(), sizeof(ov.position));
			a.occ_vertices.push_back(ov);
		}
		for (const auto &p : src.planes) {
			ThreediOcclusionPlane op{};
			std::memcpy(op.normal, p.data(), sizeof(op.normal));
			op.radius = p[3];
			a.occ_planes.push_back(op);
		}
		for (const auto &f : src.faces) a.occ_faces.push_back(ThreediOcclusionFace{f[0], f[1], f[2]});
	}
}

} // namespace

bool migrate(const File &gp, std::vector<uint8_t> &out, std::vector<MigrateNote> &note_list, std::string &error,
		const MigrateOptions &options) {
	out.clear();
	note_list.clear();
	error.clear();
	Notes notes(note_list);
	if (options.region < 0 || options.region > 2) {
		error = "the region is outside 0..2";
		return false;
	}
	const Header &h = gp.header;
	if (gp.lods.size() != static_cast<size_t>(h.lod_count) || gp.lods.empty()) {
		error = "the model holds no render model";
		return false;
	}
	Assembled a;
	Threedi3di3 &model = a.model;
	model.header.has_header = 1;
	// JO copies the name unbounded into a 16-byte field [orig: ThreediGp_LoadFromFile
	// @ 0x5B597E (Jointops)]: at most 15 characters.
	if (h.name.size() > 15) notes.add("the model's name is cut to 15 characters");
	copy_name(model.header.name, 16, h.name);
	model.header.mesh_type = skinned(h.kind) ? THREEDI_MESH_SKINNED : THREEDI_MESH_BASIC;
	model.header.lod_count_decl = h.lod_count;
	model.header.max_radius_fp16 = h.radius;

	// Materials: each LOD's referenced records, one MTRL record each, the identical ones shared.
	std::vector<std::string> registers = gp.control_registers;
	const bool stream = h.kind != Kind::Gps && !gp.vertex_stream.empty() && gp.vertex_stream.size() >= gp.vertices.size() * 24;
	MaterialBuild mb{gp, options, notes, registers, error, stream};
	std::vector<std::vector<int>> remap(gp.lods.size());
	for (size_t li = 0; li < gp.lods.size(); ++li) {
		const RenderModel &rm = gp.lods[li];
		remap[li].assign(rm.materials.size(), -1);
		for (const Primitive *drawn : drawn_primitives(rm).prims) {
			const Primitive &p = *drawn;
			if (p.material < 0 || static_cast<size_t>(p.material) >= rm.materials.size()) {
				error = "a primitive names a material its LOD does not hold";
				return false;
			}
			int &slot = remap[li][static_cast<size_t>(p.material)];
			if (slot >= 0) continue;
			ThreediMaterial m;
			if (!mb.build(rm.materials, static_cast<size_t>(p.material), m)) return false;
			for (size_t k = 0; k < a.materials.size() && slot < 0; ++k)
				if (same_material(a.materials[k], m)) slot = static_cast<int>(k);
			if (slot < 0) {
				a.materials.push_back(m);
				slot = static_cast<int>(a.materials.size()) - 1;
			}
		}
	}
	for (ThreediMaterial &m : a.materials) m.index = static_cast<int32_t>(&m - a.materials.data());

	const size_t lods = gp.lods.size();
	a.lods.resize(lods);
	a.vertices.resize(lods);
	a.indices.resize(lods);
	a.strips.resize(lods);
	a.parts.resize(lods);
	a.panm.resize(lods);
	for (size_t li = 0; li < lods; ++li)
		if (!render_lod(gp, li, remap, a, notes, error)) return false;
	model.lods = a.lods.data();
	model.lod_count = lods;
	model.materials = a.materials.data();
	model.material_count = static_cast<uint32_t>(a.materials.size());
	model.material_record_size = 584u;

	for (const UserPoint &p : gp.user_points) {
		ThreediUserPoint o{};
		o.x = p.position[0];
		o.y = p.position[1];
		o.z = p.position[2];
		o.rot_x = p.direction[0];
		o.rot_y = p.direction[1];
		o.rot_z = p.direction[2];
		o.subobject_index = p.part;
		o.userpoint_type = p.type;
		copy_name(o.name, sizeof(o.name), p.name);
		// A lens-flare point (type 'L') drives a BHD pass JO does not have [orig:
		// Model_RenderUserpointLensFlares @ 0x50F760 (dfbhd)].
		if (p.type == 'L') notes.add("a lens-flare user point has no JO pass");
		a.user_points.push_back(o);
	}
	model.user_points = a.user_points.data();
	model.user_point_count = a.user_points.size();

	// CTRL: the names in order (a material, track and light name a register by its local
	// index), then any JO selector a flipbook added.
	for (const std::string &name : registers) {
		ThreediControlRegister r{};
		copy_name(r.name, sizeof(r.name), name);
		a.registers.push_back(r);
	}
	model.ctrl.count = static_cast<uint32_t>(a.registers.size());
	model.ctrl.record_size = 24u;
	model.ctrl.registers = a.registers.data();

	for (const auto &m : gp.matrices) {
		ThreediMatrix4x4 o{};
		std::memcpy(o.m, m.data(), sizeof(o.m));
		a.matrices.push_back(o);
	}
	model.mtrx.count = static_cast<uint32_t>(a.matrices.size());
	model.mtrx.record_size = 64u;
	model.mtrx.matrices = a.matrices.data();

	// Lights: BHD's 48-byte record is the head of JO's runtime light [orig:
	// ThreediGp_ParseLightsChunk @ 0x5B4836..0x5B490D (Jointops)]; an omni light's direction
	// and projection take the OED defaults.
	for (const Light &l : gp.lights) {
		ThreediLight o{};
		std::memcpy(o.offset, l.position.data(), sizeof(o.offset));
		o.atten_start = l.atten_start;
		o.atten_end = l.atten_end;
		o.style = l.style;
		o.phase = l.phase;
		o.rate = l.rate;
		std::memcpy(o.color_start, &l.colour_start, 4);
		std::memcpy(o.color_end, &l.colour_end, 4);
		o.subobj_index = static_cast<uint8_t>(l.part);
		const ThreediBuildVec3 d = threedi_build_to_model(ThreediBuildVec3{0.0, 0.0, -1.0});
		o.rotation[0] = static_cast<float>(d.x) + 0.0f;
		o.rotation[1] = static_cast<float>(d.y) + 0.0f;
		o.rotation[2] = static_cast<float>(d.z) + 0.0f;
		o.rotation[3] = threedi_build_light_cone_cos(0.0f);
		threedi_build_light_view_proj(o, 0.0f);
		a.lights.push_back(o);
	}
	model.lights = a.lights.data();
	model.light_count = a.lights.size();

	collision(gp, a, notes);
	model.collision = &a.collision;

	occlusion(gp, a);
	model.occlusion_vertices = a.occ_vertices.data();
	model.occlusion_vertex_count = a.occ_vertices.size();
	model.occlusion_vertex_record_size = 12u;
	model.occlusion_planes = a.occ_planes.data();
	model.occlusion_plane_count = a.occ_planes.size();
	model.occlusion_plane_record_size = 16u;
	model.occlusion_faces = a.occ_faces.data();
	model.occlusion_face_count = a.occ_faces.size();
	model.occlusion_face_record_size = 12u;
	model.occlusion_objects = a.occ_objects.data();
	model.occlusion_object_count = a.occ_objects.size();
	model.occlusion_object_record_size = 36u;

	ThreediChunkOverflow overflow{};
	if (threedi_3di3_write_memory(&model, out, &overflow) != 0) {
		error = overflow.chunk[0] != '\0'
				? "the model is too large for 3DI3: its " + std::string(overflow.chunk) + " chunk passes a chunk's 24-bit length"
				: "the 3DI3 writer refused the model";
		out.clear();
		return false;
	}
	return true;
}

} // namespace opennova::threedi_gp
