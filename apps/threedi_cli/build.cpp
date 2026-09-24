// opennova-3di build: read the .o3d scene text a DCC exporter writes (the
// Blender add-on under tools/blender/opennova_3di is the first one; the
// grammar is docs/threedi/o3d-scene-format.md) into the engine's construction
// API (formats/threedi/threedi_build.h) and serialize it through the parity
// writer, so a shipped model is produced by the same writer every fixture is
// (ADR 0003). The .o3d carries geometry in MISSION axes (x forward, y left,
// z up); the model-axis conversion is threedi_build's, never the exporter's.

#include <array>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace threedi_cli {

namespace {

struct Parser {
	std::string path;
	int line = 0;
	std::vector<std::string> errors;
	int degenerate_faces = 0;
	int stray_bones = 0;

	void error(const std::string &what) {
		errors.push_back(path + ":" + std::to_string(line) + ": " + what);
	}
};

// Numbers read through strtod so the nan/inf that retail files carry
// (occlusion planes, light matrices) survive a scene round trip.
bool read_double(std::istringstream &in, double &out) {
	std::string token;
	if (!(in >> token)) return false;
	char *end = nullptr;
	out = std::strtod(token.c_str(), &end);
	return end != nullptr && *end == '\0' && end != token.c_str();
}

bool read_doubles(std::istringstream &in, double *out, int n) {
	for (int i = 0; i < n; ++i)
		if (!read_double(in, out[i])) return false;
	return true;
}

// A name field: a bare token, or "a quoted one" that may hold spaces or be
// empty (retail user points such as `FLARE 01`, `ground `; empty CTRL names).
bool read_name(std::istringstream &in, std::string &out) {
	out.clear();
	in >> std::ws;
	if (in.peek() != '"') return static_cast<bool>(in >> out);
	in.get();
	std::getline(in, out, '"');
	return !in.bad();
}

// Strip a comment: `#` at the start of a line or after whitespace (retail
// shader tags such as `VS_PHONGT#UV` hold a '#'), never inside quotes.
void strip_comment(std::string &line) {
	bool quoted = false;
	for (size_t i = 0; i < line.size(); ++i) {
		if (line[i] == '"') quoted = !quoted;
		if (!quoted && line[i] == '#' && (i == 0 || line[i - 1] == ' ' || line[i - 1] == '\t')) {
			line.erase(i);
			return;
		}
	}
}

// An integer field in decimal or 0x hex (PANM and light flag words).
bool read_word(std::istringstream &in, long long &out) {
	std::string token;
	if (!(in >> token)) return false;
	char *end = nullptr;
	out = std::strtoll(token.c_str(), &end, 0);
	return end != nullptr && *end == '\0';
}

bool fits_s16(long long v) { return v >= SHRT_MIN && v <= SHRT_MAX; }

// The PANM flags word the tracks imply: a rotation type 2 when any rotation
// track animates, scale type 2 for any scale track, and the translate axis.
uint32_t panm_flags_for(const ThreediPartAnimation &pa, int trans_axis) {
	const auto live = [](const ThreediTransform &t) { return t.control != 0; };
	const bool rot = live(pa.rotation_x) || live(pa.rotation_y) || live(pa.rotation_z);
	const bool scale = live(pa.scale_x) || live(pa.scale_y) || live(pa.scale_z);
	return threedi_panm_pack_flags(scale ? 2 : 0, rot ? 2 : 0, 0,
			live(pa.translation) ? static_cast<uint8_t>(trans_axis) : 0);
}

ThreediVertex render_vertex(const double *p, const double *n, const double *uv, const double *uv1) {
	const ThreediBuildVec3 pm = threedi_build_to_model(ThreediBuildVec3{p[0], p[1], p[2]});
	const ThreediBuildVec3 nm = threedi_build_to_model(ThreediBuildVec3{n[0], n[1], n[2]});
	ThreediVertex v{};
	v.position[0] = static_cast<float>(pm.x);
	v.position[1] = static_cast<float>(pm.y);
	v.position[2] = static_cast<float>(pm.z);
	v.normal[0] = static_cast<float>(nm.x);
	v.normal[1] = static_cast<float>(nm.y);
	v.normal[2] = static_cast<float>(nm.z);
	v.uv0[0] = static_cast<float>(uv[0]);
	v.uv0[1] = static_cast<float>(uv[1]);
	v.uv1[0] = static_cast<float>(uv1[0]);
	v.uv1[1] = static_cast<float>(uv1[1]);
	return v;
}

// A rotation frame given in mission axes (row-major, p' = p * R) as the model
// axes frame the MTRX table stores: M = C^T R C, C the mission -> model map.
ThreediMatrix4x4 frame_to_model(const double r[9]) {
	// Rows of C: mission x -> model z, mission y -> -model x, mission z -> model y.
	static const double kC[3][3] = {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}};
	ThreediMatrix4x4 m;
	threedi_mat4_identity(&m);
	for (int a = 0; a < 3; ++a)
		for (int b = 0; b < 3; ++b) {
			double sum = 0.0;
			for (int i = 0; i < 3; ++i)
				for (int j = 0; j < 3; ++j) sum += kC[i][a] * r[i * 3 + j] * kC[j][b];
			m.m[a * 4 + b] = static_cast<float>(sum);
		}
	return m;
}

// An occlusion record being read: `occ` opens it, `ov`/`op`/`of` fill it.
struct PendingOcc {
	bool open = false;
	int type = 0, section_a = 0, section_b = 0;
	std::vector<ThreediBuildVec3> verts;
	std::vector<std::array<double, 4>> planes;
	std::vector<std::array<int, 4>> faces;
};

bool parse_scene(Parser &ps, std::istream &file, ThreediBuildModel &model) {
	std::string raw;
	int lod = -1, part = -1, material = -1, cobj = -1, volume_open = -1;
	bool uv1 = false;
	ThreediBuildStrip *strip = nullptr;
	int strip_lod = -1, strip_part = -1;
	ThreediBuildStrip pending;
	bool have_pending = false;
	PendingOcc occ;
	std::map<std::pair<int, int>, int> trans_axis;  // (lod, panm row) -> axis
	std::map<std::pair<int, int>, bool> raw_flags;  // (lod, panm row) -> flags given verbatim
	const auto flush_strip = [&]() {
		if (have_pending) {
			model.lods[strip_lod].parts[strip_part].strips.push_back(std::move(pending));
			pending = ThreediBuildStrip{};
			have_pending = false;
		}
		strip = nullptr;
	};
	const auto flush_occ = [&]() {
		if (!occ.open) return;
		occ.open = false;
		const bool given = !occ.planes.empty();
		for (const std::array<int, 4> &f : occ.faces) {
			if (given != (f[3] >= 0)) {
				ps.error("occ: give every face a plane index with explicit 'op' planes, or none without");
				return;
			}
			if (given && f[3] >= static_cast<int>(occ.planes.size())) {
				ps.error("occ: a face names a plane the record lacks");
				return;
			}
		}
		if (!model.add_occ_record(static_cast<uint8_t>(occ.type), occ.section_a, occ.section_b, occ.verts, occ.faces,
					occ.planes))
			ps.error("occ: the record needs more than 32 planes");
		occ = PendingOcc{};
	};
	// A generator's register field: a declared register index for styles
	// above 112 (retail stores the CTRL index in the phase byte), else unused.
	const auto check_register = [&](int style, int reg, const char *what) {
		if (style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD &&
				(reg < 0 || reg >= static_cast<int>(model.control_registers.size())))
			ps.error(std::string(what) + " register " + std::to_string(reg) + " is not a declared 'register'");
	};
	bool header = false;
	while (std::getline(file, raw)) {
		++ps.line;
		strip_comment(raw);
		std::istringstream in(raw);
		std::string key;
		if (!(in >> key)) continue;
		if (!header) {
			int version = 0;
			if (key != "o3d" || !(in >> version) || version != 1) {
				ps.error("expected the header line 'o3d 1'");
				return false;
			}
			header = true;
			continue;
		}
		if (key == "texfile") continue;  // import metadata (`scene` writes it); nothing to build
		if (key == "ov" || key == "op" || key == "of") {
			if (!occ.open) {
				ps.error("'" + key + "' outside an occ record");
				continue;
			}
			if (key == "ov") {
				double p[3];
				if (!read_doubles(in, p, 3)) {
					ps.error("ov needs x y z");
					continue;
				}
				if (occ.verts.size() >= 128) {
					ps.error("an occlusion record holds at most 128 vertices (7-bit edge words)");
					continue;
				}
				occ.verts.push_back(ThreediBuildVec3{p[0], p[1], p[2]});
			} else if (key == "op") {
				double p[4];
				if (!read_doubles(in, p, 4)) {
					ps.error("op needs nx ny nz d");
					continue;
				}
				occ.planes.push_back({p[0], p[1], p[2], p[3]});
			} else {
				int a, b, c, plane = -1;
				if (!(in >> a >> b >> c)) {
					ps.error("of needs three vertex indices");
					continue;
				}
				in >> plane;
				const int count = static_cast<int>(occ.verts.size());
				if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count) {
					ps.error("of references a vertex not yet declared in this occ record");
					continue;
				}
				occ.faces.push_back({a, b, c, plane});
			}
			continue;
		}
		flush_occ();
		if (key == "v" || key == "t" || key == "bones") {
			if (strip == nullptr) {
				ps.error("'" + key + "' outside a strip");
				continue;
			}
			if (key == "bones") {
				// The skinned strip's bone table: the parts its vertices'
				// local bone indices address (at most 16, STRP bone_table).
				if (!model.skinned || !strip->vertices.empty()) {
					ps.error("bones needs a skinned model and must precede the strip's vertices");
					continue;
				}
				int bone = 0;
				while (in >> bone) {
					if (bone < 0 || bone > 255 || strip->bone_table.size() >= 16) {
						ps.error("bones takes at most 16 part indices");
						break;
					}
					strip->bone_table.push_back(static_cast<uint8_t>(bone));
				}
				continue;
			}
			if (key == "v") {
				double p[3], n[3], uv[2], second[2];
				if (!read_doubles(in, p, 3) || !read_doubles(in, n, 3) || !read_doubles(in, uv, 2)) {
					ps.error("v needs px py pz nx ny nz u v");
					continue;
				}
				if (uv1) {
					if (!read_doubles(in, second, 2)) {
						ps.error("with 'uv1 1' a v carries u1 v1 after its u v");
						continue;
					}
				} else {
					second[0] = uv[0];
					second[1] = uv[1];
				}
				if (strip->vertices.size() >= 65535) {
					ps.error("strip exceeds 65535 vertices (u16 indices)");
					continue;
				}
				ThreediVertex vert = render_vertex(p, n, uv, second);
				if (model.skinned) {
					// Three influences: local indices into the strip's bone
					// table and their weights (the fourth index stays 0).
					int bi[3];
					double w[3];
					if (!(in >> bi[0] >> bi[1] >> bi[2]) || !read_doubles(in, w, 3)) {
						ps.error("a skinned v needs i0 i1 i2 w0 w1 w2 after the uv");
						continue;
					}
					for (int k = 0; k < 3; ++k) {
						// Bone slots are bytes. Retail ships weighted slots past the
						// strip's table (FSldr03: slot 255 at weight 0.21), so only a
						// scene that authors one is told.
						if (bi[k] < 0 || bi[k] > 255) {
							ps.error("v bone index is a byte");
							break;
						}
						if (w[k] != 0.0 && bi[k] >= static_cast<int>(strip->bone_table.size())) ++ps.stray_bones;
						vert.bone_indices[k] = static_cast<uint8_t>(bi[k]);
						vert.bone_weights[k] = static_cast<float>(w[k]);
					}
					vert.is_skinned = 1;
				}
				strip->vertices.push_back(vert);
			} else {
				long a, b, c;
				if (!(in >> a >> b >> c) || a < 0 || b < 0 || c < 0) {
					ps.error("t needs three vertex indices");
					continue;
				}
				const long count = static_cast<long>(strip->vertices.size());
				if (a >= count || b >= count || c >= count) {
					ps.error("t references a vertex not yet declared in this strip");
					continue;
				}
				// The scene winds counter-clockwise about the outward normal in
				// mission axes; model axes mirror mission, and retail winds
				// counter-clockwise in MODEL axes (threedi_build add_box), so
				// the mirror swaps the second and third corners.
				strip->indices.push_back(static_cast<uint16_t>(a));
				strip->indices.push_back(static_cast<uint16_t>(c));
				strip->indices.push_back(static_cast<uint16_t>(b));
			}
			continue;
		}
		if (key == "cv") {
			double p[3];
			if (cobj < 0 || !read_doubles(in, p, 3)) {
				ps.error("cv needs an open cobj and x y z");
				continue;
			}
			if (model.collision[cobj].vertices.size() > SHRT_MAX) {
				ps.error("a collision section exceeds " + std::to_string(SHRT_MAX + 1) + " vertices (signed int16 face indices)");
				continue;
			}
			model.add_collision_vertex(cobj, ThreediBuildVec3{p[0], p[1], p[2]});
			continue;
		}
		if (key == "cp") {
			// One plane of the volume the last 'cvolume' opened: outward normal,
			// n . p + d == 0 on the plane, flags (retail sets 1 on seams).
			double n[3], d;
			int flags = 0;
			if (volume_open < 0 || !read_doubles(in, n, 3) || !(in >> d)) {
				ps.error("cp needs an open cvolume and nx ny nz d [flags]");
				continue;
			}
			in >> flags;
			ThreediBuildCollisionObject &o = model.collision[volume_open];
			ThreediBoundingPlane plane{};
			plane.flags = static_cast<int16_t>(flags);
			for (int k = 0; k < 3; ++k) plane.normal[k] = threedi_q14f(n[k]);
			plane.radius = threedi_q16f(d);
			o.planes.push_back(plane);
			++o.volumes.back().plane_count;
			continue;
		}
		volume_open = -1;
		if (key == "cf") {
			// cf a b c [poly flags [nx ny nz]]: an explicit normal is for a face
			// whose corners collapse on the 8.8 grid (`scene` writes retail's).
			long a, b, c;
			int poly = 1;
			unsigned long flags = 0;
			double n[3] = {};
			if (cobj < 0 || !(in >> a >> b >> c)) {
				ps.error("cf needs an open cobj and three vertex indices");
				continue;
			}
			in >> poly >> flags;
			const bool given = read_doubles(in, n, 3);
			const long count = static_cast<long>(model.collision[cobj].vertices.size());
			if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count) {
				ps.error("cf references a collision vertex not yet declared in this cobj");
				continue;
			}
			// Counter-clockwise about the outward normal in mission axes, the
			// order retail stores collision faces in.
			const ThreediBuildVec3 normal{n[0], n[1], n[2]};
			if (!model.add_face(cobj, static_cast<uint16_t>(a), static_cast<uint16_t>(b), static_cast<uint16_t>(c),
						static_cast<uint8_t>(poly), static_cast<uint32_t>(flags), given ? &normal : nullptr))
				++ps.degenerate_faces;
			continue;
		}
		flush_strip();
		if (key == "model") {
			std::string name;
			if (!read_name(in, name) || name.size() > 15) ps.error("model needs a name of at most 15 characters");
			model.name = name;
		} else if (key == "tangents") {
			int on = 0;
			in >> on;
			model.tangents = on != 0;
		} else if (key == "skinned" || key == "uv1") {
			// skinned: GHDR mesh type 2 (parts are the skeleton's bones, strips
			// carry bone tables and vertices weights). uv1: every vertex
			// carries its second UV set (the detail stage of FF_MT shaders).
			int on = 0;
			in >> on;
			if (!model.lods.empty()) ps.error(key + " must precede the first lod");
			if (key == "skinned") model.skinned = on != 0;
			else uv1 = on != 0;
		} else if (key == "register") {
			std::string name;
			if (!read_name(in, name) || name.size() > 24) {
				ps.error("register needs a name of at most 24 characters");
				continue;
			}
			// Retail ships names outside the catalog (VEHICLE_TIRE14, an empty
			// one); the loader aliases them to LOD_FRAC, so they only warn.
			// [orig: ThreediGp_LoadCtrlRegisters @ 0x5B4640, via
			// threedi_ctrl_register_loader_ordinal]
			if (threedi_ctrl_register_ordinal(name.c_str()) == THREEDI_CTRL_REGISTER_NOT_FOUND)
				std::fprintf(stderr, "%s:%d: note: CTRL register '%s' is not in the catalog; the loader reads LOD_FRAC\n",
						ps.path.c_str(), ps.line, name.c_str());
			model.add_control_register(name.c_str());
		} else if (key == "mtrx") {
			// A rotation frame (MTRX row 1, 2, ...; row 0 is the identity) in
			// mission axes, row-major; a PANM row selects it by index.
			double r[9];
			if (!read_doubles(in, r, 9)) {
				ps.error("mtrx needs nine values (a 3x3 rotation, row-major)");
				continue;
			}
			model.frames.push_back(frame_to_model(r));
		} else if (key == "material") {
			std::string shader;
			if (!read_name(in, shader) || shader.size() > 32) {
				ps.error("material needs a shader tag");
				continue;
			}
			material = model.add_material(shader.c_str(), nullptr);
		} else if (key == "texture") {
			std::string name;
			int slot = THREEDI_TEX_SLOT_DIFFUSE, type = THREEDI_TEX_TYPE_DIFFUSE, flags = 0, frame = 0;
			if (material < 0 || !read_name(in, name)) {
				ps.error("texture needs an open material and a file name");
				continue;
			}
			in >> slot >> type >> flags >> frame;
			if (name.size() > 16) ps.error("texture name '" + name + "' exceeds 16 characters");
			ThreediMaterial &m = model.materials[material];
			if (m.texture_count >= 24) {
				ps.error("material has more than 24 textures");
				continue;
			}
			ThreediMaterialTexture &t = m.textures[m.texture_count++];
			std::snprintf(t.name, sizeof(t.name), "%s", name.c_str());
			t.slot = static_cast<uint8_t>(slot);
			t.type = static_cast<uint8_t>(type);
			t.flags = static_cast<uint8_t>(flags);
			t.frame = static_cast<uint8_t>(frame);
		} else if (key == "texanim") {
			int frames = 0, type = 0, time = 0;
			if (material < 0 || !(in >> frames >> type >> time)) {
				ps.error("texanim needs an open material and frames type time");
				continue;
			}
			if (frames < 0 || frames > 255 || type < 0 || type > 1 || !fits_s16(time)) {
				ps.error("texanim frames is a byte, type is 0 or 1, and time/register is int16");
				continue;
			}
			if (type == 1) check_register(THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD + 1, time, "texanim");
			ThreediTexAnim &a = model.materials[material].animation;
			a.num_frames = static_cast<uint8_t>(frames);
			a.animation_type = static_cast<uint8_t>(type);
			a.cycle_frame_time = static_cast<int16_t>(time);
		} else if (key == "reflect") {
			int c[4];
			if (material < 0 || !(in >> c[0] >> c[1] >> c[2] >> c[3])) {
				ps.error("reflect needs an open material and r g b a (0..255)");
				continue;
			}
			for (int k = 0; k < 4; ++k) model.materials[material].reflect_color[k] = threedi_byte_unit(c[k]);
		} else if (key == "matflags" || key == "alphatest" || key == "glass" || key == "emissive") {
			int value = 0;
			if (material < 0 || !(in >> value)) {
				ps.error(key + " needs an open material and a value");
				continue;
			}
			ThreediMaterial &m = model.materials[material];
			if (key == "matflags") m.material_flags = static_cast<uint8_t>(value);
			else if (key == "alphatest") m.alpha_test_value_byte = static_cast<uint8_t>(value);
			else if (key == "glass") m.is_glass = static_cast<uint8_t>(value);
			else m.emissive_type = static_cast<uint8_t>(value);
		} else if (key == "rgbgen") {
			int style = 0, reg = -1, s[3], e[3];
			double rate = 0, phase = 0;
			if (material < 0 || !(in >> style >> reg >> rate >> s[0] >> s[1] >> s[2] >> e[0] >> e[1] >> e[2])) {
				ps.error("rgbgen needs style reg rate r g b r g b [phase]");
				continue;
			}
			in >> phase;
			check_register(style, reg, "rgbgen");
			model.set_rgb_gen(material, static_cast<uint8_t>(style), reg, rate, s, e);
			if (style <= THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD)
				model.materials[material].rgb_gen.phase = threedi_q8f(phase);
		} else if (key == "alphagen" || key == "ugen" || key == "vgen") {
			int style = 0, reg = -1;
			double rate = 0, start = 0, end = 0, phase = 0;
			if (material < 0 || !(in >> style >> reg >> rate >> start >> end)) {
				ps.error(key + " needs style reg rate start end [phase]");
				continue;
			}
			in >> phase;
			check_register(style, reg, key.c_str());
			ThreediMaterial &m = model.materials[material];
			if (key == "alphagen") {
				m.alpha_gen.style = static_cast<uint8_t>(style);
				m.alpha_gen.reg = style > 112 ? reg : -1;
				m.alpha_gen.rate = threedi_q8f(rate);
				m.alpha_gen.phase = threedi_q8f(phase);
				m.alpha_gen.start = static_cast<int16_t>(start);
				m.alpha_gen.end = static_cast<int16_t>(end);
			} else {
				ThreediUvParams &g = key == "ugen" ? m.u_params : m.v_params;
				g.style = static_cast<uint8_t>(style);
				g.reg = style > 112 ? reg : -1;
				g.gen_rate = threedi_q8f(rate);
				g.phase = threedi_q8f(phase);
				g.start = threedi_q8f(start);
				g.end = threedi_q8f(end);
			}
		} else if (key == "lod") {
			int threshold = 0;
			std::string type = "gnrc";
			in >> threshold >> type;
			if (type.size() > 4) ps.error("lod type is four characters");
			lod = model.add_lod(threshold, type.c_str());
			part = -1;
		} else if (key == "part") {
			int parent = 0;
			double p[3];
			if (lod < 0 || !(in >> parent) || !read_doubles(in, p, 3)) {
				ps.error("part needs an open lod, a parent index and a pivot x y z");
				continue;
			}
			if (parent < -1 || parent > 255) {
				ps.error("part parent is -1 or a part index");
				continue;
			}
			part = model.add_part(lod, parent, ThreediBuildVec3{p[0], p[1], p[2]});
		} else if (key == "strip") {
			int mat = 0, alpha = 0;
			if (part < 0 || !(in >> mat)) {
				ps.error("strip needs an open part and a material index");
				continue;
			}
			in >> alpha;
			if (mat < 0 || mat >= static_cast<int>(model.materials.size())) {
				ps.error("strip material index out of range");
				continue;
			}
			pending = ThreediBuildStrip{};
			pending.material = mat;
			pending.alpha = alpha != 0;
			have_pending = true;
			strip = &pending;
			strip_lod = lod;
			strip_part = part;
		} else if (key == "panm") {
			// panm part parent [flags [matrix]]: flags given verbatim override
			// the ones the tracks imply; matrix selects an MTRX frame.
			int p = 0, parent = 0;
			if (lod < 0 || !(in >> p >> parent)) {
				ps.error("panm needs an open lod, a part and its parent");
				continue;
			}
			if (p < 0 || p > 255 || parent < -1 || parent > 255) {
				ps.error("panm part is a byte and parent is -1 or a byte");
				continue;
			}
			ThreediPartAnimation &pa = model.add_panm(lod, p, parent);
			long long flags = 0, frame = 0;
			if (read_word(in, flags)) {
				pa.flags = static_cast<uint32_t>(flags);
				raw_flags[{lod, static_cast<int>(model.lods[lod].panm.size()) - 1}] = true;
				if (read_word(in, frame)) {
					if (frame < 0 || frame > 255) ps.error("panm matrix index is a byte");
					pa.matrix_index = static_cast<uint8_t>(frame);
				}
			}
		} else if (key == "track") {
			// track target style REG|-|param rate start end [axis]: styles above
			// 0x70 name a declared register; the others may carry a phase byte.
			std::string target, reg;
			long long style = 0, rate = 0, start = 0, end = 0;
			if (lod < 0 || model.lods[lod].panm.empty() || !(in >> target >> style) || !read_name(in, reg) ||
					!(in >> rate >> start >> end)) {
				ps.error("track needs an open panm and target style register|-|param rate start end [axis]");
				continue;
			}
			const int t = track_index(target);
			if (t < 0) {
				ps.error("unknown track target '" + target + "'");
				continue;
			}
			if (style < 0 || style > 255 || !fits_s16(rate) || !fits_s16(start) || !fits_s16(end)) {
				ps.error("track style is a byte and rate/start/end are int16 (rotations 1/16384 turn, others 8.8)");
				continue;
			}
			int param = 0;
			if (reg != "-") {
				char *stop = nullptr;
				const long value = std::strtol(reg.c_str(), &stop, 10);
				if (!reg.empty() && stop != nullptr && *stop == '\0') {
					if (value < 0 || value > 255) {
						ps.error("track param is a byte");
						continue;
					}
					param = static_cast<int>(value);
				} else {
					param = -1;
					for (size_t r = 0; r < model.control_registers.size(); ++r)
						if (model.control_registers[r] == reg) param = static_cast<int>(r);
					if (param < 0) {
						ps.error("track register '" + reg + "' is not declared with 'register'");
						continue;
					}
				}
			}
			check_register(static_cast<int>(style), param, "track");
			ThreediPartAnimation &pa = model.lods[lod].panm.back();
			ThreediTransform *tracks[] = {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y,
					&pa.scale_z, &pa.translation};
			*tracks[t] = threedi_build_track(static_cast<uint8_t>(style), static_cast<uint8_t>(param),
					static_cast<int16_t>(rate), static_cast<int16_t>(start), static_cast<int16_t>(end));
			const int row = static_cast<int>(model.lods[lod].panm.size()) - 1;
			if (t == 6) {
				int axis = THREEDI_TRANS_Z;
				in >> axis;
				trans_axis[{lod, row}] = axis;
			}
			if (!raw_flags.count({lod, row}))
				pa.flags = panm_flags_for(pa, trans_axis.count({lod, row}) ? trans_axis[{lod, row}] : 0);
		} else if (key == "userpoint") {
			std::string name;
			double p[3], d[3];
			int sub = 0, type = THREEDI_USER_POINT_GAMEPLAY;
			if (!read_name(in, name) || !read_doubles(in, p, 3) || !read_doubles(in, d, 3) || !(in >> sub)) {
				ps.error("userpoint needs name x y z dx dy dz part [type]");
				continue;
			}
			in >> type;
			if (name.size() > 15) ps.error("user point name '" + name + "' exceeds 15 characters");
			model.add_user_point(name.c_str(), ThreediBuildVec3{p[0], p[1], p[2]}, ThreediBuildVec3{d[0], d[1], d[2]},
					sub, type);
		} else if (key == "light") {
			// light part x y z atten_start atten_end style rate phase|reg r g b r g b
			//       flags [dx dy dz falloff]
			int sub = 0, style = 0, s[3], e[3];
			double p[3], atten[2], rate = 0, phase = 0, dir[3] = {0.0, 0.0, -1.0}, falloff = 0.0;
			long long flags = 0;
			if (!(in >> sub) || !read_doubles(in, p, 3) || !read_doubles(in, atten, 2) || !(in >> style >> rate >> phase) ||
					!(in >> s[0] >> s[1] >> s[2] >> e[0] >> e[1] >> e[2]) || !read_word(in, flags)) {
				ps.error("light needs part x y z atten_start atten_end style rate phase|reg r g b r g b flags");
				continue;
			}
			double spot[4];
			if (read_doubles(in, spot, 3)) {
				if (!(in >> spot[3])) {
					ps.error("a spot light needs dx dy dz falloff");
					continue;
				}
				dir[0] = spot[0];
				dir[1] = spot[1];
				dir[2] = spot[2];
				falloff = spot[3];
			}
			if (sub < 0 || sub > 255 || style < 0 || style > 255 || flags < 0 || flags > 255) {
				ps.error("light part, style and flags are bytes");
				continue;
			}
			// Retail stores phase * 256 for styles up to 0x70, else the CTRL
			// index (the retired OED writer's packing).
			uint8_t phase_byte;
			if (style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD) {
				check_register(style, static_cast<int>(phase), "light");
				phase_byte = static_cast<uint8_t>(static_cast<int>(phase));
			} else {
				phase_byte = static_cast<uint8_t>(std::lround(phase * 256.0) & 0xFF);
			}
			model.add_light(ThreediBuildVec3{p[0], p[1], p[2]}, atten[0], atten[1], static_cast<uint8_t>(style), sub, s, e,
					static_cast<uint8_t>(flags), phase_byte, static_cast<uint16_t>(std::lround(rate * 256.0)),
					ThreediBuildVec3{dir[0], dir[1], dir[2]}, falloff);
		} else if (key == "occ") {
			if (!(in >> occ.type >> occ.section_a >> occ.section_b) || occ.type < 0 || occ.type > 255 ||
					occ.section_a < 0 || occ.section_a > 255 || occ.section_b < 0 || occ.section_b > 255) {
				ps.error("occ needs type section connecting (bytes)");
				occ = PendingOcc{};
				continue;
			}
			occ.open = true;
		} else if (key == "cobj") {
			int parent = 0;
			double o[3] = {0, 0, 0};
			if (!(in >> parent)) {
				ps.error("cobj needs a parent part");
				continue;
			}
			read_doubles(in, o, 3);
			cobj = model.add_cobj(parent, ThreediBuildVec3{o[0], o[1], o[2]});
		} else if (key == "csphere") {
			// A bone section's hit sphere (retail persons: one per bone).
			double c[3], r = 0;
			if (cobj < 0 || !read_doubles(in, c, 3) || !(in >> r) || r < 0) {
				ps.error("csphere needs an open cobj and cx cy cz radius");
				continue;
			}
			ThreediBuildCollisionObject &o = model.collision[cobj];
			o.sphere = true;
			o.sphere_center = ThreediBuildVec3{c[0], c[1], c[2]};
			o.sphere_radius = r;
		} else if (key == "cvol") {
			int type = 0, flags = 0;
			double b[6];
			if (cobj < 0 || !(in >> type >> flags) || !read_doubles(in, b, 6)) {
				ps.error("cvol needs an open cobj, type flags and a box minx miny minz maxx maxy maxz");
				continue;
			}
			model.add_volume(cobj, type, flags, ThreediBuildBox{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}});
		} else if (key == "cvolume") {
			// A convex volume over an explicit plane list: the 'cp' lines that
			// follow. The box is the volume's AABB (mission axes).
			int type = 0, flags = 0;
			double b[6];
			if (cobj < 0 || !(in >> type >> flags) || !read_doubles(in, b, 6)) {
				ps.error("cvolume needs an open cobj, type flags and a box minx miny minz maxx maxy maxz");
				continue;
			}
			model.add_volume_planes(cobj, type, flags, ThreediBuildBox{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}}, {});
			volume_open = cobj;
		} else {
			ps.error("unknown record '" + key + "'");
		}
	}
	flush_strip();
	flush_occ();
	if (!header) ps.error("empty scene");
	return ps.errors.empty();
}

// Whole-model checks retail imposes that no single record can see.
void validate(Parser &ps, const ThreediBuildModel &m) {
	ps.line = 0;
	if (m.name.empty()) ps.error("no 'model' record");
	if (m.lods.empty()) ps.error("no 'lod' record");
	for (size_t li = 0; li < m.lods.size(); ++li) {
		// A LOD with no parts is legal: retail ships them (Dblkhwk1's LOD 4).
		const ThreediBuildLod &lod = m.lods[li];
		if (lod.parts.size() > 255) ps.error("lod " + std::to_string(li) + " has more than 255 parts");
		for (const ThreediBuildPart &p : lod.parts)
			for (const ThreediBuildStrip &s : p.strips) {
				if (s.indices.size() > 65535) ps.error("a strip exceeds 65535 indices (u16 STRP count)");
				if (m.skinned && s.bone_table.empty()) ps.error("a skinned strip has no 'bones' table");
				for (uint8_t b : s.bone_table)
					if (b >= lod.parts.size()) ps.error("a strip's bone table names a part the LOD lacks");
			}
		for (const ThreediBuildPart &p : lod.parts)
			if (p.parent >= static_cast<int>(lod.parts.size()))
				ps.error("lod " + std::to_string(li) + " has a part whose parent it lacks");
		for (const ThreediPartAnimation &pa : lod.panm) {
			if (pa.subobject_index >= lod.parts.size())
				ps.error("panm in lod " + std::to_string(li) + " names a missing part");
			if (static_cast<int8_t>(pa.matrix_index) > static_cast<int>(m.frames.size()))
				ps.error("panm in lod " + std::to_string(li) + " selects an mtrx frame the model lacks");
		}
	}
	for (size_t o = 0; o < m.collision.size(); ++o) {
		if (m.collision[o].normals.size() > static_cast<size_t>(SHRT_MAX) + 1)
			ps.error("cobj " + std::to_string(o) + " exceeds " + std::to_string(SHRT_MAX + 1) + " collision normals (signed int16 indices)");
		for (const ThreediBoundingVolume &v : m.collision[o].volumes)
			if (v.plane_count < 4) ps.error("cobj " + std::to_string(o) + " has a volume with fewer than 4 planes");
	}
	for (const ThreediLight &l : m.lights)
		if (l.subobj_index != 0 && (m.lods.empty() || l.subobj_index >= m.lods[0].parts.size()))
			ps.error("a light names a part LOD 0 lacks");
	int seats = 0;
	for (const ThreediUserPoint &u : m.user_points)
		if (strncmp(u.name, "sitex", 5) == 0 || strncmp(u.name, "SITEX", 5) == 0) ++seats;
	if (seats > 8) ps.error("more than 8 sitex seats (retail's scan stops at 8)");
	if (m.user_points.size() > 16)
		std::fprintf(stderr, "opennova-3di: note: %zu user points; the item-effect attach scan only reads the first 16\n",
				m.user_points.size());
	if (ps.stray_bones > 0)
		std::fprintf(stderr, "opennova-3di: note: %d weighted bone slots lie outside their strip's bone table\n",
				ps.stray_bones);
	if (ps.degenerate_faces > 0)
		std::fprintf(stderr, "opennova-3di: note: skipped %d collinear collision faces\n", ps.degenerate_faces);
}

} // namespace

int cmd_build(const char *scene_path, const char *out_path) {
	std::ifstream file(scene_path);
	if (!file) {
		std::fprintf(stderr, "opennova-3di: cannot open %s\n", scene_path);
		return 1;
	}
	Parser ps;
	ps.path = scene_path;
	ThreediBuildModel model;
	parse_scene(ps, file, model);
	if (ps.errors.empty()) validate(ps, model);
	if (!ps.errors.empty()) {
		for (const std::string &e : ps.errors) std::fprintf(stderr, "%s\n", e.c_str());
		return 1;
	}
	std::vector<uint8_t> bytes;
	if (!threedi_build_mint(model, bytes)) {
		std::fprintf(stderr, "opennova-3di: the writer refused the model\n");
		return 1;
	}
	// Read the bytes back through the retail-shape reader before shipping them.
	Threedi3di3 check{};
	if (threedi_3di3_read_memory(bytes.data(), bytes.size(), &check) != 0) {
		std::fprintf(stderr, "opennova-3di: the written model does not read back\n");
		return 1;
	}
	threedi_3di3_free(&check);
	FILE *f = std::fopen(out_path, "wb");
	if (f == nullptr || std::fwrite(bytes.data(), 1, bytes.size(), f) != bytes.size()) {
		if (f != nullptr) std::fclose(f);
		std::fprintf(stderr, "opennova-3di: cannot write %s\n", out_path);
		return 1;
	}
	std::fclose(f);
	std::printf("wrote %s (%zu bytes)\n", out_path, bytes.size());
	return 0;
}

} // namespace threedi_cli
