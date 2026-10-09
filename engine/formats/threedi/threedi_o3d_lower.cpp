// The `.o3d` parse lowered to a target (threedi_o3d_lower.h).

#include <formats/threedi/threedi_o3d_lower.h>

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_o3d_read.h>
#include <formats/threedi/threedi_panm.h>

namespace opennova::threedi {

namespace {

// 2^15: what a signed 16-bit index reaches, and past what a 16.16 word's
// whole part does not.
constexpr long long kWholePartReach = 32768;
// The smallest buffer of the GPU pools a LOD's vertices and indices go into
// (threedi_retail_limits).
constexpr long long kPoolVertexBuffer = 0x200000;
constexpr long long kPoolIndexBuffer = 0x80000;

} // namespace

ThreediTargetLimits threedi_retail_limits() {
	ThreediTargetLimits l;
	// A skinned strip's palette: the strip's STRP bone table (16 bytes) is
	// uploaded entry for entry and the shader holds MAX_SKIN_MATRICES 16
	// [orig: CRenderBatchQueue_FlushBatches @ 0x5D9F50, the table byte read
	// @0x5DA170, SetMatrixArray @ 0x5DA1B2]; every skinned vertex shader
	// blends NumBones = 4 influences over the four index bytes VERT stores
	// with three weights [orig: ThreediGp_ConvertVerticesToGPUFormat @
	// 0x5B4C90; _BaseInc.fx CalcSkinWorldPosAndNormal]
	// (docs/threedi/3di-gp-format-re.md, the retail skinned vertex blend).
	l.strip_palette = {16};
	l.influences = {4};
	// STRP counts a strip's indices in a u16 (three per triangle), and they
	// index its vertex window in u16 words.
	l.strip_triangles = {65535 / 3};
	l.strip_vertices = {65535};
	// A LOD's vertices and indices are uploaded whole, each into one buffer
	// of a GPU pool, at the offset the pool gives the LOD: vertices into the
	// static pool's 0x200000-byte buffers or the dynamic pool's (at least as
	// large: max(VRAM / 8, 8 MB) over at most eight), indices into the static
	// pool's 0x100000-byte buffers or the dynamic pool's (at least 0x80000:
	// max(VRAM / 64, 1.5 MB) over at most four). The lock refuses a range past
	// its buffer, and the LOD is left unfilled [orig: allocate_lod_gpu_buffers
	// @ 0x5B2610, the requests @0x5B26CC, @0x5B2752, @0x5B27F3, @0x5B2882;
	// CParticleSystem_Init_Wrapper @ 0x5876D0, the pool sizes; GStaticVB_Lock
	// @ 0x6840D0, the bound @0x6840E0; GStaticIB_Lock @ 0x684430, the bound
	// @0x684440]. A vertex takes 40 bytes, 56 skinned, 24 more with tangents
	// [orig: Model_GetRenderVertexStride @ 0x5B1480]. The draw's base vertex
	// is the pool offset plus the strip's start vertex, in 32 bits, so 65,535
	// bounds a strip's index window, never the LOD's buffer
	// [orig: CRenderBatchQueue_FlushBatches @ 0x5D9F50, DrawIndexedPrimitive
	// @0x5DAA07].
	l.lod_vertex_bytes = {kPoolVertexBuffer};
	l.lod_index_bytes = {kPoolIndexBuffer};
	l.vertex_stride = {40, 64, 56, 80};
	// The loader's LOD tables hold eight levels: eight render-model pointers,
	// eight thresholds and eight RMDL words, then the material count, and
	// eight 56-byte GPU blocks before the render-mode words; it never bounds
	// GHDR's count, so a ninth LOD's pointer lands in the first threshold's
	// slot [orig: ThreediGp_LoadFromFile @ 0x5B5B84..0x5B5BE5;
	// GPM_LoadRenderModel @ 0x5B56F8, the block at loader + 0x204 + 56 x
	// level; allocate_lod_gpu_buffers @ 0x5B2637]. A model's materials are one
	// allocation of the MTRL count, each strip naming its own by a dword
	// index, so no count bounds them [orig: ThreediGp_LoadFromFile @ 0x5B59C3;
	// GPM_LoadRenderModel @ 0x5B5184].
	l.lods = {8};
	// A part is a byte wherever it is named (PANM part and parent, a STRP
	// bone table entry, LGHT and OOBJ), and a PANM parent of -1 is the byte
	// 255.
	l.parts = {255};
	// GHDR and USRP give a name 16 bytes; 15 keeps the NUL the loader's C
	// strings end on (no JO name is longer than 9, so nothing witnesses how
	// an unterminated one reads).
	l.model_name = {15};
	l.user_point_name = {15};
	l.shader = {32};
	l.register_name = {24};
	// A register is named by a byte: a PANM track's param, a LGHT phase, a
	// generator's phase byte above style 0x70.
	l.register_index = {255};
	l.lod_type = {4};
	l.texture_rows = {24};
	// The MTRL row's 16-byte name, which retail fills with no NUL (124 names
	// such as `bo105blur.dds.tg`).
	l.texture_name = {16};
	l.flipbook_frames = {255};
	// CVRT stores 8.8 in an int16; 16.16 words and Q14 normals are int32 and
	// int16.
	l.collision_extent = {128};
	l.fixed16_extent = {kWholePartReach};
	l.fixed14_extent = {2};
	// A bullet face names its corners and its normal by signed 16-bit
	// indices [orig: Physics_RaycastAgainstBoneCollision @ 0x4E5079].
	l.section_vertices = {kWholePartReach};
	l.section_normals = {kWholePartReach};
	// An occlusion record's edge words index its vertices in 7 bits, and the
	// runtime's occlusion clip mask is a 32-bit word per record
	// (runtime/world/occlusion.cpp).
	l.occlusion_vertices = {128};
	l.occlusion_planes = {32};
	// WriteLGHT packs the rate times 256 into a u16 and the cone into a byte
	// of whole degrees [orig: WriteLGHT @ 0x456DF0 (ModSuperOed.exe)].
	l.light_rate = {256};
	l.light_cone = {256};
	// The item-effect attach scan reads a model's first 16 user points
	// [orig: ItemDef_GetBoneMaskByName @ 0x49ea40]; the seat scan reads the
	// `sitex` points without case, and a ninth takes the control seat and
	// ends it (THREEDI_SITEX_SEAT_LIMIT) [orig: Entity_GetBoneSlotType @
	// 0x434ED0; EntityDef_LoadModelsAndCallbacks, the store @ 0x43A4F0, the
	// scan end @ 0x43A5AF]. The game loads such a model: notes.
	l.user_point_scan = {THREEDI_USER_POINT_SCAN_LIMIT};
	l.seat_scan = {THREEDI_SITEX_SEAT_LIMIT};
	return l;
}

namespace {

std::string format_metres(double v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.3f", v);
	return buf;
}

ThreediVertex render_vertex(const ThreediO3dVertex &in) {
	const ThreediBuildVec3 pm = threedi_build_to_model(ThreediBuildVec3{in.position[0], in.position[1], in.position[2]});
	const ThreediBuildVec3 nm = threedi_build_to_model(ThreediBuildVec3{in.normal[0], in.normal[1], in.normal[2]});
	ThreediVertex v{};
	v.position[0] = static_cast<float>(pm.x);
	v.position[1] = static_cast<float>(pm.y);
	v.position[2] = static_cast<float>(pm.z);
	v.normal[0] = static_cast<float>(nm.x);
	v.normal[1] = static_cast<float>(nm.y);
	v.normal[2] = static_cast<float>(nm.z);
	v.uv0[0] = static_cast<float>(in.uv0[0]);
	v.uv0[1] = static_cast<float>(in.uv0[1]);
	v.uv1[0] = static_cast<float>(in.uv1[0]);
	v.uv1[1] = static_cast<float>(in.uv1[1]);
	return v;
}

void model_axes(const double *mission, float out[3]) {
	const ThreediBuildVec3 m = threedi_build_to_model(ThreediBuildVec3{mission[0], mission[1], mission[2]});
	out[0] = static_cast<float>(m.x);
	out[1] = static_cast<float>(m.y);
	out[2] = static_cast<float>(m.z);
}

struct Lowering {
	const ThreediO3dModel &in;
	const ThreediTarget &target;
	const ThreediTargetLimits &lim;
	ThreediBuildModel &out;
	std::vector<SceneFinding> findings;

	void error_at(int line, const std::string &what) { findings.push_back(SceneFinding{line, true, what}); }
	void note_at(int line, const std::string &what) { findings.push_back(SceneFinding{line, false, what}); }
	bool failed() const {
		for (const SceneFinding &f : findings)
			if (f.error) return true;
		return false;
	}
	// `value` within `limit` (at most its max): true; past it, an error and
	// false. `what` says what holds `value`, `why` what imposes the limit.
	bool within(const ThreediLimit &limit, long long value, int line, const std::string &what, const std::string &why) {
		if (value <= limit.max) return true;
		error_at(line, what + ": " + why + " (at most " + strutil::grouped(limit.max) + ")");
		return false;
	}
	// A value its fixed-point word must hold: finite, |v| < `limit`.
	bool extent(const ThreediLimit &limit, double v, int line, const std::string &what, const std::string &word) {
		if (std::fabs(v) < static_cast<double>(limit.max)) return true;
		error_at(line, what + " is " + f9(v) + ": " + word + " holds a number under " + strutil::grouped(limit.max) + " either way");
		return false;
	}
	bool extents(const ThreediLimit &limit, const double *v, int n, int line, const std::string &what,
			const std::string &word) {
		bool ok = true;
		for (int k = 0; k < n && ok; ++k) ok = extent(limit, v[k], line, what, word);
		return ok;
	}
	bool fixed16(const double *v, int n, int line, const std::string &what) {
		return extents(lim.fixed16_extent, v, n, line, what, "a 16.16 word");
	}
	// A name in `limit` characters (bytes).
	bool name(const ThreediLimit &limit, const std::string &value, int line, const std::string &what, const std::string &why) {
		return within(limit, static_cast<long long>(value.size()), line,
				what + " '" + value + "' is " + std::to_string(value.size()) + " characters", why);
	}
	// A part index a byte field holds.
	bool part_byte(long long part, int line, const std::string &what) {
		return within(lim.parts, part, line, what + " names part " + std::to_string(part), "a part index is a byte");
	}
	bool register_byte(long long reg, int line, const std::string &what) {
		return within(lim.register_index, reg, line, what + " names register " + std::to_string(reg),
				"its register field is a byte");
	}
};

void lower_registers(Lowering &lw) {
	for (const ThreediO3dNamed &reg : lw.in.registers) {
		lw.name(lw.lim.register_name, reg.name, reg.line, "the register", "a CTRL record holds that many bytes");
		// Retail ships names outside the catalog (VEHICLE_TIRE14, an empty
		// one); the loader aliases them to LOD_FRAC, so they only warn.
		// [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640, via
		// threedi_ctrl_register_loader_ordinal]
		if (threedi_ctrl_register_ordinal(reg.name.c_str()) == THREEDI_CTRL_REGISTER_NOT_FOUND)
			lw.note_at(reg.line, "CTRL register '" + reg.name + "' is not in the catalog; the loader reads LOD_FRAC");
		lw.out.control_registers.push_back(reg.name);
	}
}

void lower_texture(Lowering &lw, const ThreediO3dTexture &t, ThreediMaterial &m) {
	// The name is the MTRL row's field, and the loader looks its file up by
	// that string: printable ASCII, a file name alone.
	if (!lw.within(lw.lim.texture_name, static_cast<long long>(t.name.size()), t.line,
				"texture name '" + t.name + "' is " + std::to_string(t.name.size()) + " bytes", "the MTRL field's width"))
		return;
	const auto unprintable = std::find_if(t.name.begin(), t.name.end(), [](char c) {
		return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E;
	});
	if (unprintable != t.name.end()) {
		char byte[8];
		std::snprintf(byte, sizeof(byte), "0x%02X", static_cast<unsigned char>(*unprintable));
		lw.error_at(t.line, "texture name '" + t.name + "' holds the byte " + byte +
				": a texture name is printable ASCII, as the game's file names are");
		return;
	}
	if (t.name.find_first_of("/\\") != std::string::npos) {
		lw.error_at(t.line, "texture name '" + t.name + "' names a folder: the game finds a texture by its file name alone");
		return;
	}
	ThreediMaterialTexture &row = m.textures[m.texture_count++];
	std::snprintf(row.name, sizeof(row.name), "%s", t.name.c_str());
	row.slot = t.slot;
	row.type = t.type;
	row.flags = t.flags;
	row.frame = t.frame;
	// The loader opens the name cut three characters past its first '.' and
	// decodes a .tga, .mdt or .pcx file itself; any other name loads only as
	// the .dds of its stem, when one lies beside it [orig:
	// Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA, @ 0x58B53C..0x58B598,
	// @ 0x58B66F..0x58B6E6] (the target's textures: the rows that loader
	// reads, by their type). An empty name is a row with no file (retail
	// ships 65).
	std::string opens, loads;
	if (lw.target.textures != nullptr && lw.target.textures(t.name.c_str(), t.type, opens, loads))
		lw.note_at(t.line, "texture '" + t.name + "' loads only as '" + loads + "': the game opens '" + opens +
				"' (the name cut three characters past its first '.') and decodes .tga, .mdt and .pcx files itself");
}

// The materials, and whether a shader reads the TANGENT semantic.
bool lower_materials(Lowering &lw) {
	bool tangents = false;
	for (const ThreediO3dMaterial &src : lw.in.materials) {
		if (!lw.name(lw.lim.shader, src.shader, src.line, "the shader tag", "MTRL holds that many bytes")) {
			lw.out.materials.push_back(ThreediMaterial{}); // keeps the indices meshes name
			continue;
		}
		// The vertex layout carries tangents when any material's shader reads
		// the TANGENT semantic (ComputeVertexFormatFlags [orig: @ 0x457a10
		// (ModSuperOed.exe)]; docs/threedi/3di-gp-format-re.md); retail's
		// object-space bump shaders (VS_PHONGO, VS_SKBUMPDIFFOBJ) carry none:
		// Colt_1st, Boonie.
		bool reads_tangents = false;
		const bool known = lw.target.shaders != nullptr && lw.target.shaders(src.shader.c_str(), reads_tangents);
		tangents = tangents || (known && reads_tangents);
		if (!known && lw.target.shaders != nullptr)
			lw.note_at(src.line, "shader '" + src.shader + "' is not in the engine's shader table");
		const int index = lw.out.add_material(src.shader.c_str(), nullptr);
		ThreediMaterial &m = lw.out.materials[static_cast<size_t>(index)];
		for (size_t t = 0; t < src.textures.size(); ++t) {
			if (!lw.within(lw.lim.texture_rows, static_cast<long long>(t + 1), src.textures[t].line,
						"the material's texture row " + std::to_string(t + 1), "MTRL holds that many rows"))
				break;
			if (m.texture_count >= sizeof(m.textures) / sizeof(m.textures[0])) {
				lw.error_at(src.textures[t].line, "the construction API holds 24 texture rows a material");
				break;
			}
			lower_texture(lw, src.textures[t], m);
		}
		if (src.texanim_line != 0 &&
				lw.within(lw.lim.flipbook_frames, src.frames, src.texanim_line,
						"the flipbook's " + std::to_string(src.frames) + " frames", "its frame count is a byte")) {
			m.animation.num_frames = static_cast<uint8_t>(src.frames);
			m.animation.animation_type = src.animation_type;
			m.animation.cycle_frame_time = src.time;
		}
		for (int k = 0; k < 4; ++k) m.reflect_color[k] = threedi_byte_unit(src.reflect[k]);
		m.material_flags = src.flags;
		m.alpha_test_value_byte = src.alpha_test;
		m.is_glass = src.glass;
		m.emissive_type = src.emissive;
		const ThreediO3dGenerator &rgb = src.rgbgen;
		if (rgb.line != 0) {
			const bool names = threedi_generator_names_register(rgb.style);
			if (!names || lw.register_byte(rgb.reg, rgb.line, "rgbgen")) {
				lw.out.set_rgb_gen(index, rgb.style, static_cast<int>(rgb.reg), rgb.rate, rgb.start_rgb, rgb.end_rgb);
				if (!names) m.rgb_gen.phase = threedi_q8f(rgb.phase);
			}
		}
		const ThreediO3dGenerator &alpha = src.alphagen;
		if (alpha.line != 0) {
			const bool names = threedi_generator_names_register(alpha.style);
			if (!names || lw.register_byte(alpha.reg, alpha.line, "alphagen")) {
				m.alpha_gen.style = alpha.style;
				m.alpha_gen.reg = names ? static_cast<int>(alpha.reg) : -1;
				m.alpha_gen.rate = threedi_q8f(alpha.rate);
				m.alpha_gen.phase = threedi_q8f(alpha.phase);
				m.alpha_gen.start = static_cast<int16_t>(alpha.start);
				m.alpha_gen.end = static_cast<int16_t>(alpha.end);
			}
		}
		for (const ThreediO3dGenerator *g : {&src.ugen, &src.vgen}) {
			if (g->line == 0) continue;
			const bool names = threedi_generator_names_register(g->style);
			if (names && !lw.register_byte(g->reg, g->line, g == &src.ugen ? "ugen" : "vgen")) continue;
			ThreediUvParams &p = g == &src.ugen ? m.u_params : m.v_params;
			p.style = g->style;
			p.reg = names ? static_cast<int>(g->reg) : -1;
			p.gen_rate = threedi_q8f(g->rate);
			p.phase = threedi_q8f(g->phase);
			p.start = threedi_q8f(g->start);
			p.end = threedi_q8f(g->end);
		}
	}
	return tangents;
}

// One strip of a mesh as the partition fills it.
struct StripPlan {
	std::vector<uint32_t> triangles; // the mesh's, in its order
	std::vector<long long> table;    // parts, in first-use order
	std::unordered_map<long long, uint32_t> slot;
	std::vector<uint32_t> window;    // mesh vertices, in first-use order
	std::unordered_map<uint32_t, uint32_t> local;
};

// One mesh into the strips the target holds, appended to `part`.
void lower_mesh(Lowering &lw, const ThreediO3dMesh &mesh, ThreediBuildPart &part) {
	const ThreediTargetLimits &lim = lw.lim;
	const bool skinned = lw.in.skinned;
	const size_t vertex_count = mesh.vertices.size();

	// (1) Influences: a vertex blending more than the target does keeps its
	// first, the bone the lit skinned shaders light it by (IndexArray[0]:
	// runtime/renderer/object_shader_template.h), and the heaviest of the
	// rest (the earlier on a tie, in the author's order), renormalized.
	std::vector<ThreediO3dInfluence> influences;
	std::vector<std::array<uint32_t, 2>> span(vertex_count); // first, count in `influences`
	size_t reduced = 0;
	// The construction API's vertex holds four slots: a target blending more
	// would need a container of its own (threedi_o3d_lower.h).
	const size_t keep = static_cast<size_t>(std::min<long long>(4, std::max<long long>(1, lim.influences.max)));
	for (size_t vi = 0; skinned && vi < vertex_count; ++vi) {
		const ThreediO3dVertex &v = mesh.vertices[vi];
		const ThreediO3dInfluence *given = mesh.influences.data() + v.first_influence;
		span[vi] = {static_cast<uint32_t>(influences.size()), 0};
		if (v.influence_count <= keep) {
			influences.insert(influences.end(), given, given + v.influence_count);
		} else {
			++reduced;
			std::vector<uint32_t> rest;
			for (uint32_t k = 1; k < v.influence_count; ++k) rest.push_back(k);
			std::stable_sort(rest.begin(), rest.end(),
					[given](uint32_t a, uint32_t b) { return given[a].weight > given[b].weight; });
			rest.resize(keep - 1);
			std::sort(rest.begin(), rest.end());
			double total = given[0].weight;
			for (uint32_t k : rest) total += given[k].weight;
			const size_t at = influences.size();
			influences.push_back(given[0]);
			for (uint32_t k : rest) influences.push_back(given[k]);
			if (total > 0.0)
				for (size_t k = at; k < influences.size(); ++k) influences[k].weight /= total;
		}
		span[vi][1] = static_cast<uint32_t>(influences.size()) - span[vi][0];
	}
	if (reduced > 0)
		lw.note_at(mesh.line, strutil::grouped(static_cast<long long>(reduced)) + " vertices blend more than " +
				std::to_string(keep) + " influences: each keeps its first (the bone it is lit by) and its " +
				std::to_string(keep - 1) + " heaviest others, renormalized (the game blends " + std::to_string(keep) + ")");

	// (2) Strips. A rigid mesh fills one strip to the target's triangles and
	// starts another. A skinned one splits by OED's palette rule (the retired
	// port's rdta.cpp, WriteRDTA_Skinned's grouping; the JO corpus's 56 split
	// materials, docs/threedi/3di-gp-format-re.md STRP): a triangle joins the
	// first strip whose table, counting each corner's parts anew where it
	// lacks them, stays within the palette, else starts one, and a table
	// lists its parts in the order they come. The corners count in the order
	// the file stores them, `a c b` of the text: re-lowering the JOCA skinned
	// strips gives back 68 of 70 models' tables entry for entry so, 23
	// counted `a b c`. No strip outgrows the vertices its indices reach
	// either (with 3DI3's words a strip's triangles never do).
	std::vector<StripPlan> strips;
	std::vector<long long> bones;
	for (uint32_t ti = 0; ti < mesh.triangles.size(); ++ti) {
		const std::array<uint32_t, 3> &tri = mesh.triangles[ti];
		bones.clear();
		if (skinned)
			for (const uint32_t corner : {tri[0], tri[2], tri[1]})
				for (uint32_t k = 0; k < span[corner][1]; ++k) bones.push_back(influences[span[corner][0] + k].part);
		const auto fresh_bones = [&bones](const StripPlan &s) {
			long long n = 0;
			for (const long long b : bones) n += s.slot.count(b) ? 0 : 1;
			return n;
		};
		const auto fresh_vertices = [&tri](const StripPlan &s) {
			long long n = 0;
			for (size_t k = 0; k < 3; ++k) {
				bool earlier = false;
				for (size_t j = 0; j < k; ++j) earlier = earlier || tri[j] == tri[k];
				n += s.local.count(tri[k]) || earlier ? 0 : 1;
			}
			return n;
		};
		const auto fits = [&](const StripPlan &s) {
			return static_cast<long long>(s.triangles.size()) < lim.strip_triangles.max &&
					static_cast<long long>(s.table.size()) + fresh_bones(s) <= lim.strip_palette.max &&
					static_cast<long long>(s.window.size()) + fresh_vertices(s) <= lim.strip_vertices.max;
		};
		StripPlan *into = nullptr;
		if (skinned) {
			for (StripPlan &s : strips)
				if (fits(s)) {
					into = &s;
					break;
				}
		} else if (!strips.empty() && fits(strips.back())) {
			into = &strips.back();
		}
		if (into == nullptr) {
			strips.emplace_back();
			into = &strips.back();
			if (!fits(*into)) {
				lw.error_at(mesh.vertices[tri[0]].line, "a triangle of the mesh at line " + std::to_string(mesh.line) +
						" names " + std::to_string(fresh_bones(*into)) + " parts: a strip's palette holds " +
						strutil::grouped(lim.strip_palette.max));
				return;
			}
		}
		for (const long long b : bones)
			if (into->slot.emplace(b, static_cast<uint32_t>(into->table.size())).second) into->table.push_back(b);
		for (const uint32_t corner : tri)
			if (into->local.emplace(corner, static_cast<uint32_t>(into->window.size())).second)
				into->window.push_back(corner);
		into->triangles.push_back(ti);
	}

	// (3) Vertex windows. A mesh one strip holds keeps its vertices as the
	// author gave them, any no triangle uses included; a split mesh's strips
	// each hold the vertices their triangles use, in first-use order, and a
	// vertex no triangle uses goes.
	const bool whole = strips.size() <= 1 && static_cast<long long>(vertex_count) <= lim.strip_vertices.max;
	if (whole) {
		if (strips.empty()) strips.emplace_back();
		StripPlan &s = strips.front();
		s.window.resize(vertex_count);
		for (uint32_t vi = 0; vi < vertex_count; ++vi) {
			s.window[vi] = vi;
			s.local[vi] = vi;
		}
		// A vertex's parts join the table too: skinned tables name every
		// part the strip's vertices blend.
		for (uint32_t vi = 0; skinned && vi < vertex_count; ++vi)
			for (uint32_t k = 0; k < span[vi][1]; ++k) {
				const long long b = influences[span[vi][0] + k].part;
				if (s.slot.emplace(b, static_cast<uint32_t>(s.table.size())).second) s.table.push_back(b);
			}
		if (!lw.within(lim.strip_palette, static_cast<long long>(s.table.size()), mesh.line,
					"the mesh's vertices blend " + std::to_string(s.table.size()) + " parts and no triangle splits them",
					"a strip's palette holds that many"))
			return;
	} else if (strips.empty()) {
		lw.error_at(mesh.line, "the mesh holds " + strutil::grouped(static_cast<long long>(vertex_count)) +
				" vertices and no triangle: a strip's indices reach " + strutil::grouped(lim.strip_vertices.max));
		return;
	} else {
		std::vector<bool> used(vertex_count, false);
		for (const StripPlan &s : strips)
			for (const uint32_t v : s.window) used[v] = true;
		const long long dropped = static_cast<long long>(std::count(used.begin(), used.end(), false));
		if (dropped > 0)
			lw.note_at(mesh.line, strutil::grouped(dropped) + " vertices no triangle uses are left out: the mesh splits into " +
					std::to_string(strips.size()) + " strips, each holding the vertices its triangles use");
	}

	// (4) The strips: strip-relative indices in retail's winding (model axes
	// mirror mission, and retail winds counter-clockwise in MODEL axes, so
	// the second and third corners swap: `opennova-3di info` prints the
	// share, near 100% for the corpus), the bone-table slot of each kept
	// influence in order with the first's repeated as padding at no weight
	// (retail pads with the first slot or the previous one, which blend the
	// same), and the first three weights (slot 3 takes the rest, 1 - (w0 +
	// w1 + w2), as the shader blends). Authored tangent frames ride along.
	for (const StripPlan &s : strips) {
		ThreediBuildStrip strip;
		strip.material = static_cast<int>(mesh.material);
		strip.alpha = mesh.alpha;
		strip.tangents_given = !mesh.frames.empty();
		for (const long long b : s.table) strip.bone_table.push_back(static_cast<uint8_t>(b));
		strip.vertices.reserve(s.window.size());
		for (const uint32_t vi : s.window) {
			ThreediVertex v = render_vertex(mesh.vertices[vi]);
			if (skinned) {
				const uint32_t count = span[vi][1];
				for (uint32_t k = 0; k < 4; ++k) {
					const uint32_t from = k < count ? k : 0;
					v.bone_indices[k] = static_cast<uint8_t>(s.slot.at(influences[span[vi][0] + from].part));
				}
				for (uint32_t k = 0; k < 3; ++k)
					v.bone_weights[k] = k < count ? static_cast<float>(influences[span[vi][0] + k].weight) : 0.0f;
				v.is_skinned = 1;
			}
			if (strip.tangents_given) {
				model_axes(mesh.frames[vi].data(), v.tangent);
				model_axes(mesh.frames[vi].data() + 3, v.bitangent);
			}
			strip.vertices.push_back(v);
		}
		const auto local = [&s, whole](uint32_t vi) { return static_cast<uint16_t>(whole ? vi : s.local.at(vi)); };
		const std::vector<uint32_t> *order = &s.triangles;
		std::vector<uint32_t> all;
		if (whole) {
			all.resize(mesh.triangles.size());
			for (uint32_t t = 0; t < all.size(); ++t) all[t] = t;
			order = &all;
		}
		strip.indices.reserve(order->size() * 3);
		for (const uint32_t t : *order) {
			const std::array<uint32_t, 3> &tri = mesh.triangles[t];
			strip.indices.push_back(local(tri[0]));
			strip.indices.push_back(local(tri[2]));
			strip.indices.push_back(local(tri[1]));
		}
		part.strips.push_back(std::move(strip));
	}
}

void lower_lods(Lowering &lw) {
	const ThreediO3dModel &in = lw.in;
	if (!in.lods.empty())
		lw.within(lw.lim.lods, static_cast<long long>(in.lods.size()), in.lods.back().line,
				"the model holds " + std::to_string(in.lods.size()) + " LODs", "the loader's LOD tables hold that many");
	const ThreediVertexStride &st = lw.lim.vertex_stride;
	const long long stride = lw.out.skinned ? (lw.out.tangents ? st.skinned_tangents : st.skinned)
	                                        : (lw.out.tangents ? st.rigid_tangents : st.rigid);
	for (const ThreediO3dLod &src : in.lods) {
		lw.name(lw.lim.lod_type, src.type, src.line, "the LOD type", "RMDL holds that many bytes");
		// The loader stores the threshold shifted left 16 in the level's own
		// slot [orig: ThreediGp_LoadFromFile @ 0x5b5bdf..0x5b5be5].
		const double threshold = src.threshold;
		lw.extent(lw.lim.fixed16_extent, threshold, src.line, "the LOD threshold", "the loader's 16.16 slot");
		const int lod = lw.out.add_lod(src.threshold, src.type.c_str());
		if (!src.parts.empty() &&
				!lw.within(lw.lim.parts, static_cast<long long>(src.parts.size()), src.parts.back().line,
						"the LOD holds " + std::to_string(src.parts.size()) + " parts", "a part index is a byte"))
			continue;
		long long lod_vertices = 0, lod_indices = 0;
		for (const ThreediO3dPart &p : src.parts) {
			const int index = lw.out.add_part(lod, static_cast<int>(p.parent), p.pivot);
			ThreediBuildPart &part = lw.out.lods[lod].parts[static_cast<size_t>(index)];
			part.has_center = p.has_center;
			part.center = p.center;
			for (const ThreediO3dMesh &mesh : p.meshes) {
				// GHDR's radius is the farthest render vertex in 16.16
				// (threedi_build.cpp, WriteGHDR's rule); a NaN position (no
				// distance) leaves it as it is.
				bool reach = true;
				for (size_t vi = 0; vi < mesh.vertices.size() && reach; ++vi) {
					const double *q = mesh.vertices[vi].position;
					const double d = std::sqrt(q[0] * q[0] + q[1] * q[1] + q[2] * q[2]);
					reach = std::isnan(d) || lw.extent(lw.lim.fixed16_extent, d, mesh.vertices[vi].line,
													 "the vertex's distance from the origin", "GHDR's 16.16 radius");
				}
				if (!reach) continue;
				const size_t before = part.strips.size();
				lower_mesh(lw, mesh, part);
				for (size_t s = before; s < part.strips.size(); ++s) {
					lod_vertices += static_cast<long long>(part.strips[s].vertices.size());
					lod_indices += static_cast<long long>(part.strips[s].indices.size());
				}
			}
		}
		lw.within(lw.lim.lod_vertex_bytes, lod_vertices * stride, src.line,
				"the LOD's vertex buffer takes " + strutil::grouped(lod_vertices * stride) + " bytes (" + strutil::grouped(lod_vertices) +
						" vertices of " + std::to_string(stride) + ")",
				"the game uploads it whole into one GPU pool buffer");
		lw.within(lw.lim.lod_index_bytes, lod_indices * 2, src.line,
				"the LOD's index buffer takes " + strutil::grouped(lod_indices * 2) + " bytes (" + strutil::grouped(lod_indices) + " indices)",
				"the game uploads it whole into one GPU pool buffer");
		for (const ThreediO3dPanm &pa : src.panm) {
			if (!lw.part_byte(pa.parent < 0 ? 0 : pa.parent, pa.line, "panm's parent")) continue;
			ThreediPartAnimation &row = lw.out.add_panm(lod, static_cast<int>(pa.part), static_cast<int>(pa.parent));
			if (pa.flags_given) {
				row.flags = pa.flags;
				row.matrix_index = static_cast<uint8_t>(pa.matrix);
			}
			int trans_axis = 0;
			for (const ThreediO3dTrack &t : pa.tracks) {
				if (t.names_register && !lw.register_byte(t.param, t.line, "track")) continue;
				*threedi_panm_tracks(row)[t.target] = threedi_build_track(t.style, static_cast<uint8_t>(t.param), t.rate,
						t.start, t.end);
				if (t.target == 6) trans_axis = t.axis != 0 ? t.axis : THREEDI_TRANS_Z;
			}
			if (!pa.flags_given) row.flags = threedi_build_panm_flags(row, static_cast<uint8_t>(trans_axis));
		}
	}
}

void lower_points_and_lights(Lowering &lw) {
	int seats = 0;
	for (size_t i = 0; i < lw.in.user_points.size(); ++i) {
		const ThreediO3dUserPoint &u = lw.in.user_points[i];
		if (!lw.name(lw.lim.user_point_name, u.name, u.line, "the user point name",
					"USRP keeps that many and the NUL the loader's C strings end on") ||
				!lw.fixed16(u.position, 3, u.line, "the user point's position") ||
				!lw.fixed16(u.direction, 3, u.line, "the user point's direction"))
			continue;
		if (u.part < INT32_MIN || u.part > INT32_MAX) {
			lw.error_at(u.line, "the user point's part " + std::to_string(u.part) + " is past USRP's 32-bit word");
			continue;
		}
		if (static_cast<long long>(i) == lw.lim.user_point_scan.max)
			lw.note_at(u.line, std::to_string(lw.in.user_points.size()) + " user points; the item-effect attach scan only reads "
					"the first " + std::to_string(lw.lim.user_point_scan.max));
		if (threedi_user_point_is_sitex(u.name) && ++seats == lw.lim.seat_scan.max + 1)
			lw.note_at(u.line, "more than " + std::to_string(lw.lim.seat_scan.max) +
					" sitex seats: the game takes the ninth for the control seat, and its seat scan reads no user "
					"point after it");
		lw.out.add_user_point(u.name.c_str(), ThreediBuildVec3{u.position[0], u.position[1], u.position[2]},
				ThreediBuildVec3{u.direction[0], u.direction[1], u.direction[2]}, static_cast<int>(u.part), u.type);
	}
	for (const ThreediO3dLight &l : lw.in.lights) {
		if (!lw.part_byte(l.part, l.line, "the light")) continue;
		// The rate packs into an unsigned word (times 256), the cone into a
		// byte of whole degrees.
		if (!(l.rate >= 0.0) ||
				!lw.extent(lw.lim.light_rate, l.rate, l.line, "the light's rate", "a u16 of 1/256 steps")) {
			if (!(l.rate >= 0.0)) lw.error_at(l.line, "the light's rate is " + f9(l.rate) + ": a rate is 0 or more");
			continue;
		}
		if (!(l.falloff >= 0.0) ||
				!lw.extent(lw.lim.light_cone, l.falloff, l.line, "the light's cone half-angle", "a byte of degrees")) {
			if (!(l.falloff >= 0.0))
				lw.error_at(l.line, "the light's cone half-angle is " + f9(l.falloff) + ": an angle is 0 or more");
			continue;
		}
		// The phase byte: phase * 256 for styles up to 0x70, else the CTRL
		// index (threedi_build_light_phase).
		uint8_t phase_byte;
		if (threedi_generator_names_register(l.style)) {
			if (!lw.register_byte(static_cast<long long>(l.phase), l.line, "the light")) continue;
			phase_byte = static_cast<uint8_t>(static_cast<int>(l.phase));
		} else {
			phase_byte = threedi_build_light_phase(l.phase);
		}
		lw.out.add_light(ThreediBuildVec3{l.position[0], l.position[1], l.position[2]}, l.atten_start, l.atten_end,
				l.style, static_cast<int>(l.part), l.start_rgb, l.end_rgb, l.flags, phase_byte,
				threedi_build_light_rate(l.rate), ThreediBuildVec3{l.direction[0], l.direction[1], l.direction[2]},
				l.falloff);
	}
}

void lower_occlusion(Lowering &lw) {
	for (const ThreediO3dOcclusion &o : lw.in.occlusion) {
		if (!lw.part_byte(o.section, o.line, "the occlusion record's section") ||
				!lw.part_byte(o.connecting, o.line, "the occlusion record's connecting section") ||
				!lw.within(lw.lim.occlusion_vertices, static_cast<long long>(o.vertices.size()), o.line,
						"the occlusion record holds " + std::to_string(o.vertices.size()) + " vertices",
						"its edge words index them in 7 bits") ||
				!lw.within(lw.lim.occlusion_planes, static_cast<long long>(o.planes.size()), o.line,
						"the occlusion record holds " + std::to_string(o.planes.size()) + " planes",
						"the runtime's occlusion clip mask is a 32-bit word per record"))
			continue;
		if (!lw.out.add_occ_record(o.type, static_cast<int>(o.section), static_cast<int>(o.connecting), o.vertices,
					o.faces, o.planes, o.sphere_given ? &o.sphere : nullptr, static_cast<size_t>(lw.lim.occlusion_planes.max)))
			lw.error_at(o.line, "occ: the record needs more than " + std::to_string(lw.lim.occlusion_planes.max) +
					" planes (the runtime's occlusion clip mask is a 32-bit word per record)");
	}
}

void lower_volume(Lowering &lw, int cobj, const ThreediO3dVolume &v) {
	if (v.kind != ThreediO3dVolume::Kind::mesh) {
		const double box[6] = {v.box.min.x, v.box.min.y, v.box.min.z, v.box.max.x, v.box.max.y, v.box.max.z};
		if (!lw.fixed16(box, 6, v.line, "the volume's box")) return;
	}
	if (v.kind == ThreediO3dVolume::Kind::box) {
		lw.out.add_volume(cobj, v.type, v.flags, v.box);
	} else if (v.kind == ThreediO3dVolume::Kind::planes) {
		std::vector<ThreediBoundingPlane> planes;
		for (const ThreediO3dPlane &p : v.planes) {
			if (!lw.extents(lw.lim.fixed14_extent, p.normal, 3, p.line, "the plane's normal", "a Q14 word") ||
					!lw.fixed16(&p.distance, 1, p.line, "the plane's distance"))
				return;
			ThreediBoundingPlane plane{};
			plane.flags = p.flags;
			for (int k = 0; k < 3; ++k) plane.normal[k] = threedi_q14f(p.normal[k]);
			plane.radius = threedi_q16f(p.distance);
			planes.push_back(plane);
		}
		lw.out.add_volume_planes(cobj, v.type, v.flags, v.box, planes);
	} else {
		for (const ThreediBuildVec3 &p : v.vertices) {
			const double q[3] = {p.x, p.y, p.z};
			if (!lw.fixed16(q, 3, v.line, "a vertex of the volume")) return;
		}
		const double outside = lw.out.add_volume_mesh(cobj, v.type, v.flags, v.vertices, v.triangles);
		// A volume is the solid all its face planes bound: past a
		// centimetre, the author's shape is not what collides.
		if (outside > 0.01)
			lw.note_at(v.line, "volume " + (v.label.empty() ? std::string("?") : v.label) +
					" is not convex: its vertices reach " + format_metres(outside) +
					" outside the solid its faces bound, which is all that collides (split it into convex volumes)");
	}
}

void lower_collision(Lowering &lw) {
	for (size_t si = 0; si < lw.in.sections.size(); ++si) {
		const ThreediO3dSection &s = lw.in.sections[si];
		const std::string at = "collision section " + std::to_string(si);
		if (s.parent < INT32_MIN || s.parent > INT32_MAX) {
			lw.error_at(s.line, at + "'s parent " + std::to_string(s.parent) + " is past COBJ's 32-bit word");
			continue;
		}
		const double offset[3] = {s.offset.x, s.offset.y, s.offset.z};
		if (!lw.fixed16(offset, 3, s.line, at + "'s offset")) continue;
		const int cobj = lw.out.add_cobj(static_cast<int>(s.parent), s.offset);
		ThreediBuildCollisionObject &o = lw.out.collision[static_cast<size_t>(cobj)];
		if (s.sphere_line != 0) {
			const double c[4] = {s.sphere_center.x, s.sphere_center.y, s.sphere_center.z, s.sphere_radius};
			const ThreediBuildBox &b = s.sphere_bounds;
			const double box[6] = {b.min.x, b.min.y, b.min.z, b.max.x, b.max.y, b.max.z};
			if (lw.fixed16(c, 4, s.sphere_line, at + "'s hit sphere") &&
					(!s.sphere_bounded || lw.fixed16(box, 6, s.sphere_line, at + "'s bounds"))) {
				o.sphere = true;
				o.sphere_center = s.sphere_center;
				o.sphere_radius = s.sphere_radius;
				o.sphere_bounded = s.sphere_bounded;
				o.sphere_bounds = s.sphere_bounds;
			}
		}
		// Retail reads a bullet face's corners as signed 16-bit indices, so a
		// section addresses at most 32,768 vertices (ThreediCollisionFace).
		if (!s.vertices.empty() &&
				!lw.within(lw.lim.section_vertices, static_cast<long long>(s.vertices.size()), s.vertices.back().line,
						at + " holds " + strutil::grouped(static_cast<long long>(s.vertices.size())) + " vertices",
						"retail reads a bullet face's corners as signed 16-bit indices (simplify its collision mesh, or "
						"split it over more parts)"))
			continue;
		bool placed = true;
		for (const ThreediO3dCollisionVertex &v : s.vertices) {
			const double p[3] = {v.position.x, v.position.y, v.position.z};
			if (!lw.extents(lw.lim.collision_extent, p, 3, v.line, "the collision vertex's coordinate", "CVRT's 8.8 int16")) {
				placed = false;
				break;
			}
			lw.out.add_collision_vertex(cobj, v.position);
		}
		if (!placed) continue;
		size_t degenerate = 0;
		int first_degenerate = 0;
		for (const ThreediO3dCollisionFace &f : s.faces) {
			const double n[3] = {f.normal.x, f.normal.y, f.normal.z};
			if (f.normal_given && !lw.extents(lw.lim.fixed14_extent, n, 3, f.line, "the face's normal", "a Q14 word")) break;
			// Counter-clockwise about the outward normal in mission axes, the
			// order retail stores collision faces in.
			if (!lw.out.add_face(cobj, static_cast<uint16_t>(f.corner[0]), static_cast<uint16_t>(f.corner[1]),
						static_cast<uint16_t>(f.corner[2]), f.poly_type, f.flags, f.normal_given ? &f.normal : nullptr) &&
					degenerate++ == 0)
				first_degenerate = f.line;
		}
		if (degenerate > 0)
			lw.note_at(first_degenerate, "skipped " + std::to_string(degenerate) + " collinear collision faces of " + at);
		// A bullet face names its normal by a signed 16-bit index too [orig:
		// Physics_RaycastAgainstBoneCollision @ 0x4E5079]; section i pairs
		// with part i of the collision LOD.
		lw.within(lw.lim.section_normals, static_cast<long long>(o.normals.size()), s.line,
				at + " (part " + std::to_string(si) + ") has " + strutil::grouped(static_cast<long long>(o.normals.size())) +
						" distinct bullet-face normals",
				"a face names its normal by a signed 16-bit index (simplify its bullet faces: faces in one plane "
				"share a normal; split them over more parts, or give the part none)");
		for (const ThreediO3dVolume &v : s.volumes) lower_volume(lw, cobj, v);
	}
	lw.out.translations_given = lw.in.translations_given;
	for (const ThreediO3dTranslation &t : lw.in.translations) {
		const double p[3] = {t.position.x, t.position.y, t.position.z};
		if (lw.fixed16(p, 3, t.line, "the CXLT row")) lw.out.translations.push_back(t.position);
	}
}

// The line of the model's first `vt`, 0 with none.
int first_frames_line(const ThreediO3dModel &model) {
	for (const ThreediO3dLod &lod : model.lods)
		for (const ThreediO3dPart &p : lod.parts)
			for (const ThreediO3dMesh &m : p.meshes)
				if (!m.frames.empty()) return m.frames_line;
	return 0;
}

} // namespace

bool threedi_o3d_lower(const ThreediO3dModel &model, const ThreediTarget &target, ThreediBuildModel &out,
		std::vector<SceneFinding> &findings) {
	out = ThreediBuildModel{};
	Lowering lw{model, target, target.limits, out, {}};
	lw.name(lw.lim.model_name, model.name.name, model.name.line, "the model name",
			"GHDR keeps that many and the NUL the loader's C strings end on");
	out.name = model.name.name;
	out.skinned = model.skinned;
	lower_registers(lw);
	for (const ThreediO3dFrameRecord &f : model.frames) out.frames.push_back(threedi_build_frame_to_model(f.rotation));
	const bool shader_tangents = lower_materials(lw);
	out.tangents = model.tangents_line != 0 || shader_tangents;
	const int frames_line = first_frames_line(model);
	if (!out.tangents && frames_line != 0)
		lw.note_at(frames_line, "the tangent frames ('vt') go unused: no shader reads tangents and no 'tangents 1' "
				"lays them out");
	lower_lods(lw);
	lower_points_and_lights(lw);
	lower_occlusion(lw);
	lower_collision(lw);
	// By line, the whole model's last.
	std::stable_sort(lw.findings.begin(), lw.findings.end(), [](const SceneFinding &a, const SceneFinding &b) {
		return (a.line == 0 ? INT_MAX : a.line) < (b.line == 0 ? INT_MAX : b.line);
	});
	const bool clean = !lw.failed();
	findings.insert(findings.end(), lw.findings.begin(), lw.findings.end());
	return clean;
}

bool threedi_o3d_build(std::istream &text, const ThreediTarget &target, std::vector<uint8_t> &out,
		std::vector<SceneFinding> &findings) {
	ThreediO3dModel model;
	if (!threedi_o3d_read(text, model, findings)) return false;
	ThreediBuildModel built;
	if (!threedi_o3d_lower(model, target, built, findings)) return false;
	ThreediBuildRefusal refusal;
	if (!threedi_build_mint(built, out, &refusal)) {
		findings.push_back(SceneFinding{0, true, refusal.what});
		return false;
	}
	// Read the bytes back through the retail-shape reader before handing them on.
	Threedi3di3 check{};
	if (threedi_3di3_read_memory(out.data(), out.size(), &check) != 0) {
		findings.push_back(SceneFinding{0, true, "the written model does not read back"});
		return false;
	}
	threedi_3di3_free(&check);
	return true;
}

} // namespace opennova::threedi
