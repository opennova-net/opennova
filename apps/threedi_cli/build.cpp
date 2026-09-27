// opennova-3di build: read the .o3d scene text a DCC exporter writes (the
// Blender add-on under tools/blender/opennova_3di is the first one; the
// grammar is docs/threedi/o3d-scene-format.md) into the engine's construction
// API (formats/threedi/threedi_build.h) and serialize it through the parity
// writer, so a shipped model is produced by the same writer every fixture is
// (ADR 0003). The .o3d carries geometry in MISSION axes (x forward, y left,
// z up); the model-axis conversion is threedi_build's, never the exporter's.
//
// The reader is strict: a field that does not parse, a value its word cannot
// hold, or a token past the record's fields fails the build naming the line,
// so nothing an exporter writes is silently wrapped or dropped.

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include <base/io/strutil.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_ctrl_catalog.h>
#include <formats/threedi/threedi_panm.h>
// The shader table, and the texture-name cut the loader applies.
#include <runtime/renderer/material_descriptor.h>
#include <runtime/renderer/material_texture.h>

#include "scene_text.h"
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

	void error(const std::string &what) { error_at(line, what); }
	void error_at(int at, const std::string &what) {
		errors.push_back(path + ":" + std::to_string(at) + ": " + what);
	}
	// A whole-model check no single line owns.
	void model_error(const std::string &what) { errors.push_back(path + ": " + what); }
};

// A name is at most 15 characters: GHDR and USRP give it a 16-byte field, and
// 15 keeps the NUL the loader's C strings end on (no JO name is longer than 9,
// so nothing witnesses how an unterminated one reads).
constexpr size_t kNameChars = 15;

bool fits_s16(long long v) { return v >= SHRT_MIN && v <= SHRT_MAX; }

// A count with thousands separators, for messages: 16,777,215.
std::string grouped(size_t n) {
	std::string digits = std::to_string(n), out;
	for (size_t i = 0; i < digits.size(); ++i) {
		if (i > 0 && (digits.size() - i) % 3 == 0) out += ',';
		out += digits[i];
	}
	return out;
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

// An occlusion record being read: `occ` opens it, `ov`/`op`/`of` fill it.
struct PendingOcc {
	bool open = false;
	int line = 0;
	int type = 0, section_a = 0, section_b = 0;
	std::vector<ThreediBuildVec3> verts;
	std::vector<std::array<double, 4>> planes;
	std::vector<std::array<int, 4>> faces;
};

// A collision volume authored as triangles, being read: `cvmesh` opens it,
// `vv`/`vf` fill it; the builder derives its planes by the OED rule.
struct PendingVolume {
	bool open = false;
	int cobj = -1, type = 0, flags = 0, line = 0;
	std::string label;
	std::vector<ThreediBuildVec3> verts;
	std::vector<std::array<int, 3>> tris;
};

bool parse_scene(Parser &ps, std::istream &file, ThreediBuildModel &model) {
	std::string raw;
	int lod = -1, part = -1, material = -1, cobj = -1, volume_open = -1;
	int full_section = -1; // the last section told it holds too many vertices
	bool strip_full = false; // the open strip was told it holds too many vertices
	bool uv1 = false;
	ThreediBuildStrip *strip = nullptr;
	int strip_lod = -1, strip_part = -1;
	ThreediBuildStrip pending;
	bool have_pending = false;
	PendingOcc occ;
	PendingVolume vol;
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
		const PendingOcc done = occ;
		occ = PendingOcc{};
		const bool given = !done.planes.empty();
		for (const std::array<int, 4> &f : done.faces) {
			if (given != (f[3] >= 0)) {
				ps.error_at(done.line, "occ: give every face a plane index with explicit 'op' planes, or none without");
				return;
			}
			if (given && f[3] >= static_cast<int>(done.planes.size())) {
				ps.error_at(done.line, "occ: a face names a plane the record lacks");
				return;
			}
		}
		if (!model.add_occ_record(static_cast<uint8_t>(done.type), done.section_a, done.section_b, done.verts, done.faces,
					done.planes))
			ps.error_at(done.line, "occ: the record needs more than 32 planes");
	};
	const auto flush_volume = [&]() {
		if (!vol.open) return;
		if (vol.verts.size() < 4 || vol.tris.empty())
			ps.error_at(vol.line, "cvmesh: a volume needs at least 4 vertices and a triangle");
		else {
			const double outside = model.add_volume_mesh(vol.cobj, vol.type, vol.flags, vol.verts, vol.tris);
			// A volume is the solid all its face planes bound: past a
			// centimetre, the author's shape is not what collides.
			if (outside > 0.01)
				std::fprintf(stderr, "%s:%d: note: volume %s is not convex: its vertices reach %.3f outside the solid "
						"its faces bound, which is all that collides (split it into convex volumes)\n",
						ps.path.c_str(), vol.line, vol.label.empty() ? "?" : vol.label.c_str(), outside);
		}
		vol = PendingVolume{};
	};
	// A generator's register field: a declared register index for styles
	// above 0x70 (retail stores the CTRL index in the phase byte), else unused.
	const auto check_register = [&](long long style, long long reg, const char *what) {
		if (style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD &&
				(reg < 0 || reg >= static_cast<long long>(model.control_registers.size())))
			ps.error(std::string(what) + " register " + std::to_string(reg) + " is not a declared 'register'");
	};
	bool header = false;
	while (std::getline(file, raw)) {
		++ps.line;
		// A UTF-8 byte order mark ahead of the header is no field.
		if (ps.line == 1 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
		const std::string text = strip_comment(raw);
		SceneLine in(text, SceneNumbers::any);
		if (!in.bad.empty()) {
			ps.error(in.bad);
			continue;
		}
		if (in.tokens.empty()) continue;
		const std::string key = in.key();
		if (key == "texfile") continue;  // import metadata (`scene` writes it); nothing to build
		bool ok = true; // false: the record already reported an error
		if (!header) {
			long long version = 0;
			if (key != "o3d" || !in.integer(version) || version != 1 || in.more()) {
				ps.error("expected the header line 'o3d 1'");
				return false;
			}
			header = true;
			continue;
		}
		if (key == "ov" || key == "op" || key == "of") {
			if (!occ.open) {
				ps.error("'" + key + "' outside an occ record");
				continue;
			}
			if (key == "ov") {
				double p[3];
				if (!in.numbers(p, 3)) {
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
				if (!in.numbers(p, 4)) {
					ps.error("op needs nx ny nz d");
					continue;
				}
				// The runtime's occlusion clip mask is a 32-bit word per record
				// (occlusion.cpp): a 33rd plane would alias the first.
				if (occ.planes.size() >= 32) {
					ps.error("an occlusion record holds at most 32 planes (the runtime's 32-bit clip mask)");
					continue;
				}
				occ.planes.push_back({p[0], p[1], p[2], p[3]});
			} else {
				long long a = 0, b = 0, c = 0, plane = -1;
				const long long count = static_cast<long long>(occ.verts.size());
				if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
					ps.error("of needs three vertices already declared in this occ record");
					continue;
				}
				if (in.more() && !in.integer(plane, 0, 31)) {
					ps.error("of's plane is an index 0..31");
					continue;
				}
				occ.faces.push_back({static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(plane)});
			}
			if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
			continue;
		}
		flush_occ();
		if (key == "vv" || key == "vf") {
			if (!vol.open) {
				ps.error("'" + key + "' outside a cvmesh volume");
				continue;
			}
			if (key == "vv") {
				double p[3];
				if (!in.numbers(p, 3)) {
					ps.error("vv needs x y z");
					continue;
				}
				vol.verts.push_back(ThreediBuildVec3{p[0], p[1], p[2]});
			} else {
				long long a = 0, b = 0, c = 0;
				const long long count = static_cast<long long>(vol.verts.size());
				if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
					ps.error("vf needs three vertices already declared in this cvmesh");
					continue;
				}
				vol.tris.push_back({static_cast<int>(a), static_cast<int>(b), static_cast<int>(c)});
			}
			if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
			continue;
		}
		flush_volume();
		if (key == "v" || key == "t" || key == "bones") {
			if (strip == nullptr) {
				ps.error("'" + key + "' outside a strip");
				continue;
			}
			if (key == "bones") {
				// The skinned strip's bone table: the parts its vertices'
				// local bone indices address (1 to 16, STRP bone_table).
				if (!model.skinned || !strip->vertices.empty()) {
					ps.error("bones needs a skinned model and must precede the strip's vertices");
					continue;
				}
				long long bone = 0;
				while (in.more()) {
					if (!in.integer(bone, 0, 255) || strip->bone_table.size() >= 16) {
						ps.error("bones takes 1 to 16 part indices");
						ok = false;
						break;
					}
					strip->bone_table.push_back(static_cast<uint8_t>(bone));
				}
				if (ok && strip->bone_table.empty()) ps.error("bones takes 1 to 16 part indices");
				continue;
			}
			if (key == "v") {
				double p[3], n[3], uv[2], second[2];
				if (!in.numbers(p, 3) || !in.numbers(n, 3) || !in.numbers(uv, 2)) {
					ps.error("v needs px py pz nx ny nz u v");
					continue;
				}
				if (uv1) {
					if (!in.numbers(second, 2)) {
						ps.error("with 'uv1 1' a v carries u1 v1 after its u v");
						continue;
					}
				} else {
					second[0] = uv[0];
					second[1] = uv[1];
				}
				if (strip->vertices.size() >= 65535) {
					// Said once per strip.
					if (!strip_full)
						ps.error("strip exceeds 65,535 vertices: its triangles index them with u16 words (split the "
								"strip's mesh, or use fewer vertices)");
					strip_full = true;
					continue;
				}
				ThreediVertex vert = render_vertex(p, n, uv, second);
				if (model.skinned) {
					// Four local indices into the strip's bone table and three
					// weights: slot i3 takes the rest, 1 - (w0 + w1 + w2), as
					// retail's vertex shader blends (threedi_skin_influences).
					long long bi[4];
					double w[3];
					if (!in.integer(bi[0], 0, 255) || !in.integer(bi[1], 0, 255) || !in.integer(bi[2], 0, 255) ||
							!in.integer(bi[3], 0, 255) || !in.numbers(w, 3)) {
						ps.error("a skinned v needs bone slots i0 i1 i2 i3 (bytes) and weights w0 w1 w2 after the uv");
						continue;
					}
					for (int k = 0; k < 3 && ok; ++k)
						if (!(w[k] >= 0.0 && w[k] <= 1.0)) {
							ps.error("skinned v weight w" + std::to_string(k) + " is " + f9(w[k]) +
									": a weight is a finite number from 0 to 1");
							ok = false;
						}
					if (!ok) continue;
					for (int k = 0; k < 4; ++k) vert.bone_indices[k] = static_cast<uint8_t>(bi[k]);
					for (int k = 0; k < 3; ++k) vert.bone_weights[k] = static_cast<float>(w[k]);
					// The sum as the shader adds it, in float: retail's
					// four-decimal weights reach 1.0001 (ArmGlovD), giving slot
					// i3 a hair of negative weight; past that, the vertex is
					// not what its author weighted.
					float sum = 0.0f;
					for (const float weight : vert.bone_weights) sum += weight;
					if (sum > 1.0001f) {
						ps.error("skinned v weights w0 + w1 + w2 sum to " + f9(sum) +
								": they sum to at most 1 (slot i3 takes the rest, 1 - (w0 + w1 + w2))");
						continue;
					}
					// Retail ships weighted slots past the strip's table
					// (FSldr03: slot 255 at weight 0.21), so only a scene that
					// authors one is told.
					ThreediSkinInfluence influences[4];
					threedi_skin_influences(&vert, strip->bone_table.data(), static_cast<int32_t>(strip->bone_table.size()),
							influences);
					for (const ThreediSkinInfluence &influence : influences)
						if (influence.weight != 0.0f && influence.part < 0) ++ps.stray_bones;
					vert.is_skinned = 1;
				}
				strip->vertices.push_back(vert);
			} else {
				long long a = 0, b = 0, c = 0;
				const long long count = static_cast<long long>(strip->vertices.size());
				if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
					ps.error("t needs three vertices already declared in this strip");
					continue;
				}
				if (a == b || b == c || a == c) {
					ps.error("t repeats a vertex (the loader drops such a triangle)");
					continue;
				}
				// The scene winds counter-clockwise about the outward normal in
				// mission axes; model axes mirror mission, and retail winds
				// counter-clockwise in MODEL axes (`opennova-3di info` prints the
				// share, near 100% for the corpus), so the mirror swaps the
				// second and third corners.
				strip->indices.push_back(static_cast<uint16_t>(a));
				strip->indices.push_back(static_cast<uint16_t>(c));
				strip->indices.push_back(static_cast<uint16_t>(b));
			}
			if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
			continue;
		}
		if (key == "cv") {
			double p[3];
			if (cobj < 0 || !in.numbers(p, 3)) {
				ps.error("cv needs an open cobj and x y z");
				continue;
			}
			// CVRT stores 8.8 in an int16: |x| must stay under 128.
			if (!(std::fabs(p[0]) < 128.0 && std::fabs(p[1]) < 128.0 && std::fabs(p[2]) < 128.0)) {
				ps.error("a collision vertex lies 128 or more from the origin (8.8 in an int16)");
				continue;
			}
			// Retail reads a bullet face's corners as signed 16-bit indices, so
			// a section addresses at most 32,768 vertices (ThreediCollisionFace).
			// Said once per section.
			if (model.collision[cobj].vertices.size() > SHRT_MAX) {
				if (full_section != cobj)
					ps.error("collision section " + std::to_string(cobj) + " exceeds 32,768 vertices: retail reads a bullet "
							"face's corners as signed 16-bit indices (simplify its collision mesh, or split it over more parts)");
				full_section = cobj;
				continue;
			}
			model.add_collision_vertex(cobj, ThreediBuildVec3{p[0], p[1], p[2]});
			if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
			continue;
		}
		if (key == "cp") {
			// One plane of the volume the last 'cvolume' opened: outward normal,
			// n . p + d == 0 on the plane, flags (retail sets 1 on seams).
			double n[3], d;
			long long flags = 0;
			if (volume_open < 0 || !in.numbers(n, 3) || !in.number(d)) {
				ps.error("cp needs an open cvolume and nx ny nz d [flags]");
				continue;
			}
			if (in.more() && !in.integer(flags, SHRT_MIN, SHRT_MAX)) {
				ps.error("cp's flags are an int16");
				continue;
			}
			ThreediBuildCollisionObject &o = model.collision[volume_open];
			ThreediBoundingPlane plane{};
			plane.flags = static_cast<int16_t>(flags);
			for (int k = 0; k < 3; ++k) plane.normal[k] = threedi_q14f(n[k]);
			plane.radius = threedi_q16f(d);
			o.planes.push_back(plane);
			++o.volumes.back().plane_count;
			if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
			continue;
		}
		volume_open = -1;
		if (key == "cf") {
			// cf a b c [poly flags [nx ny nz]]: an explicit normal is for a face
			// whose corners collapse on the 8.8 grid (`scene` writes retail's).
			long long a = 0, b = 0, c = 0, poly = 1, flags = 0;
			double n[3] = {};
			if (cobj < 0) {
				ps.error("cf needs an open cobj");
				continue;
			}
			const long long count = static_cast<long long>(model.collision[cobj].vertices.size());
			if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
				ps.error("cf needs three collision vertices already declared in this cobj");
				continue;
			}
			if ((in.more() && !in.integer(poly, 0, 255)) || (in.more() && !in.integer(flags, 0, 0xFFFFFFFFll))) {
				ps.error("cf's poly_type is a byte and its flags a 32-bit word (decimal or 0x)");
				continue;
			}
			const bool given = in.more();
			if (given && !in.numbers(n, 3)) {
				ps.error("cf's normal needs nx ny nz");
				continue;
			}
			if (in.more()) {
				ps.error("unexpected '" + in.peek() + "' after the record");
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
			if (!in.name(name) || name.size() > kNameChars) {
				ps.error("model needs a name of at most 15 characters");
				continue;
			}
			model.name = name;
		} else if (key == "tangents" || key == "skinned" || key == "uv1") {
			// tangents: VERT carries tangent/bitangent (a TANGENT shader turns
			// it on anyway). skinned: GHDR mesh type 2 (parts are the
			// skeleton's bones, strips carry bone tables and vertices weights).
			// uv1: every vertex carries its second UV set (the detail stage of
			// FF_MT shaders).
			long long on = 0;
			if (!in.integer(on, 0, 1)) {
				ps.error(key + " takes 0 or 1");
				continue;
			}
			if (key != "tangents" && !model.lods.empty()) ps.error(key + " must precede the first lod");
			if (key == "tangents") model.tangents = on != 0;
			else if (key == "skinned") model.skinned = on != 0;
			else uv1 = on != 0;
		} else if (key == "register") {
			std::string name;
			if (!in.name(name) || name.size() > 24) {
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
			if (!in.numbers(r, 9)) {
				ps.error("mtrx needs nine values (a 3x3 rotation, row-major)");
				continue;
			}
			model.frames.push_back(threedi_build_frame_to_model(r));
		} else if (key == "material") {
			std::string shader;
			if (!in.name(shader) || shader.size() > 32) {
				ps.error("material needs a shader tag of at most 32 characters");
				continue;
			}
			material = model.add_material(shader.c_str(), nullptr);
		} else if (key == "texture") {
			std::string name;
			long long slot = THREEDI_TEX_SLOT_DIFFUSE, type = THREEDI_TEX_TYPE_DIFFUSE, flags = 0, frame = 0;
			if (material < 0 || !in.name(name)) {
				ps.error("texture needs an open material and a file name");
				continue;
			}
			// The name is the MTRL row's 16-byte field, which retail fills with
			// no NUL (124 names such as `bo105blur.dds.tg`), and the loader looks
			// its file up by that string: printable ASCII, a file name alone.
			if (name.size() > 16) {
				ps.error("texture name '" + name + "' is " + std::to_string(name.size()) +
						" bytes: the MTRL field holds 16");
				continue;
			}
			const auto unprintable = std::find_if(name.begin(), name.end(), [](char c) {
				return static_cast<unsigned char>(c) < 0x20 || static_cast<unsigned char>(c) > 0x7E;
			});
			if (unprintable != name.end()) {
				char byte[8];
				std::snprintf(byte, sizeof(byte), "0x%02X", static_cast<unsigned char>(*unprintable));
				ps.error("texture name '" + name + "' holds the byte " + byte +
						": a texture name is printable ASCII, as the game's file names are");
				continue;
			}
			if (name.find_first_of("/\\") != std::string::npos) {
				ps.error("texture name '" + name + "' names a folder: the game finds a texture by its file name alone");
				continue;
			}
			long long *optional[] = {&slot, &type, &flags, &frame};
			for (long long *field : optional)
				if (ok && in.more() && !in.integer(*field, 0, 255)) {
					ps.error("texture's slot, type, flags and frame are bytes");
					ok = false;
				}
			if (!ok) continue;
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
			// The loader opens the name cut three characters past its first
			// '.' and decodes a .tga, .mdt or .pcx file itself; any other name
			// loads only as the .dds of its stem, when one lies beside it
			// [orig: Texture_LoadByNameWithChannel @ 0x58B4E1..0x58B4FA,
			// @ 0x58B53C..0x58B598, @ 0x58B66F..0x58B6E6] (material_texture.h).
			// An empty name is a row with no file (retail ships 65).
			const std::string query = opennova::renderer::material_texture_query(name);
			const std::string dds = opennova::renderer::material_dds_sibling(query);
			if (!name.empty() &&
					opennova::renderer::plain_material_image_source(query).decoder ==
							opennova::renderer::MaterialImageDecoder::None &&
					!opennova::strutil::iequals(dds, query))
				std::fprintf(stderr,
						"%s:%d: note: texture '%s' loads only as '%s': the game opens '%s' (the name cut three "
						"characters past its first '.') and decodes .tga, .mdt and .pcx files itself\n",
						ps.path.c_str(), ps.line, name.c_str(), dds.c_str(), query.c_str());
		} else if (key == "texanim") {
			long long frames = 0, type = 0, time = 0;
			if (material < 0 || !in.integer(frames) || !in.integer(type) || !in.integer(time)) {
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
			long long c[4];
			if (material < 0 || !in.integer(c[0], 0, 255) || !in.integer(c[1], 0, 255) || !in.integer(c[2], 0, 255) ||
					!in.integer(c[3], 0, 255)) {
				ps.error("reflect needs an open material and r g b a (0..255)");
				continue;
			}
			for (int k = 0; k < 4; ++k) model.materials[material].reflect_color[k] = threedi_byte_unit(static_cast<int>(c[k]));
		} else if (key == "matflags" || key == "alphatest" || key == "glass" || key == "emissive") {
			long long value = 0;
			if (material < 0 || !in.integer(value, 0, 255)) {
				ps.error(key + " needs an open material and a byte (0..255)");
				continue;
			}
			ThreediMaterial &m = model.materials[material];
			if (key == "matflags") m.material_flags = static_cast<uint8_t>(value);
			else if (key == "alphatest") m.alpha_test_value_byte = static_cast<uint8_t>(value);
			else if (key == "glass") m.is_glass = static_cast<uint8_t>(value);
			else m.emissive_type = static_cast<uint8_t>(value);
		} else if (key == "rgbgen") {
			long long style = 0, reg = -1, s[3], e[3];
			double rate = 0, phase = 0;
			if (material < 0 || !in.integer(style, 0, 255) || !in.integer(reg) || !in.number(rate)) {
				ps.error("rgbgen needs style (a byte) reg rate r g b r g b [phase]");
				continue;
			}
			for (long long *c : {&s[0], &s[1], &s[2], &e[0], &e[1], &e[2]})
				if (ok && !in.integer(*c, 0, 255)) {
					ps.error("rgbgen's colours are bytes (0..255)");
					ok = false;
				}
			if (!ok) continue;
			if (in.more() && !in.number(phase)) {
				ps.error("rgbgen's phase is a number");
				continue;
			}
			check_register(style, reg, "rgbgen");
			const int start[3] = {static_cast<int>(s[0]), static_cast<int>(s[1]), static_cast<int>(s[2])};
			const int end[3] = {static_cast<int>(e[0]), static_cast<int>(e[1]), static_cast<int>(e[2])};
			model.set_rgb_gen(material, static_cast<uint8_t>(style), static_cast<int>(reg), rate, start, end);
			if (style <= THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD)
				model.materials[material].rgb_gen.phase = threedi_q8f(phase);
		} else if (key == "alphagen" || key == "ugen" || key == "vgen") {
			long long style = 0, reg = -1;
			double rate = 0, start = 0, end = 0, phase = 0;
			if (material < 0 || !in.integer(style, 0, 255) || !in.integer(reg) || !in.numbers(&rate, 1) ||
					!in.number(start) || !in.number(end)) {
				ps.error(key + " needs style (a byte) reg rate start end [phase]");
				continue;
			}
			if (in.more() && !in.number(phase)) {
				ps.error(key + "'s phase is a number");
				continue;
			}
			// The alpha generator stores start and end as int16 values.
			if (key == "alphagen" &&
					!(start == std::floor(start) && end == std::floor(end) && fits_s16(static_cast<long long>(start)) &&
							fits_s16(static_cast<long long>(end)) && std::fabs(start) < 1e6 && std::fabs(end) < 1e6)) {
				ps.error("alphagen's start and end are whole int16 values");
				continue;
			}
			check_register(style, reg, key.c_str());
			ThreediMaterial &m = model.materials[material];
			if (key == "alphagen") {
				m.alpha_gen.style = static_cast<uint8_t>(style);
				m.alpha_gen.reg = style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD ? static_cast<int>(reg) : -1;
				m.alpha_gen.rate = threedi_q8f(rate);
				m.alpha_gen.phase = threedi_q8f(phase);
				m.alpha_gen.start = static_cast<int16_t>(start);
				m.alpha_gen.end = static_cast<int16_t>(end);
			} else {
				ThreediUvParams &g = key == "ugen" ? m.u_params : m.v_params;
				g.style = static_cast<uint8_t>(style);
				g.reg = style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD ? static_cast<int>(reg) : -1;
				g.gen_rate = threedi_q8f(rate);
				g.phase = threedi_q8f(phase);
				g.start = threedi_q8f(start);
				g.end = threedi_q8f(end);
			}
		} else if (key == "lod") {
			long long threshold = 0;
			std::string type = "gnrc";
			if (in.more() && !in.integer(threshold, INT32_MIN, INT32_MAX)) {
				ps.error("lod's threshold is a whole number");
				continue;
			}
			if (in.more() && (!in.name(type) || type.size() > 4)) {
				ps.error("lod type is at most four characters");
				continue;
			}
			lod = model.add_lod(static_cast<int32_t>(threshold), type.c_str());
			part = -1;
		} else if (key == "part") {
			long long parent = 0;
			double p[3];
			if (lod < 0 || !in.integer(parent) || !in.numbers(p, 3)) {
				ps.error("part needs an open lod, a parent index and a pivot x y z");
				continue;
			}
			if (parent < -1 || parent > 255) {
				ps.error("part parent is -1 or a part index");
				continue;
			}
			part = model.add_part(lod, static_cast<int>(parent), ThreediBuildVec3{p[0], p[1], p[2]});
			// A part that draws nothing may give the point its sphere sits on
			// (radius 0): the exporter seeds such a part with a placeholder
			// vertex (the retail parts' `_## center` helper mesh, near the pivot).
			if (in.more()) {
				double c[3];
				if (!in.numbers(c, 3)) {
					ps.error("part's sphere centre needs cx cy cz");
					continue;
				}
				model.lods[lod].parts[part].has_center = true;
				model.lods[lod].parts[part].center = ThreediBuildVec3{c[0], c[1], c[2]};
			}
		} else if (key == "strip") {
			long long mat = 0, alpha = 0;
			if (part < 0 || !in.integer(mat)) {
				ps.error("strip needs an open part and a material index");
				continue;
			}
			if (in.more() && !in.integer(alpha, 0, 1)) {
				ps.error("strip's alpha is 0 or 1");
				continue;
			}
			if (mat < 0 || mat >= static_cast<long long>(model.materials.size())) {
				ps.error("strip material index out of range");
				continue;
			}
			pending = ThreediBuildStrip{};
			pending.material = static_cast<int>(mat);
			pending.alpha = alpha != 0;
			have_pending = true;
			strip = &pending;
			strip_full = false;
			strip_lod = lod;
			strip_part = part;
		} else if (key == "panm") {
			// panm part parent [flags [matrix]]: flags given verbatim override
			// the ones the tracks imply; matrix selects an MTRX frame.
			long long p = 0, parent = 0;
			if (lod < 0 || !in.integer(p) || !in.integer(parent)) {
				ps.error("panm needs an open lod, a part and its parent");
				continue;
			}
			if (p < 0 || p > 255 || parent < -1 || parent > 255) {
				ps.error("panm part is a byte and parent is -1 or a byte");
				continue;
			}
			// The runtime reads the table by row and poses a part by its last
			// row (threedi_panm_pose.cpp); every retail table is canonical,
			// row i transforming part i (all 3,250 JO tables).
			if (p != static_cast<long long>(model.lods[lod].panm.size())) {
				ps.error("panm rows go in part order, one per part: this row must transform part " +
						std::to_string(model.lods[lod].panm.size()));
				continue;
			}
			long long flags = 0, frame = 0;
			const bool given = in.more();
			if (given && !in.integer(flags, 0, 0xFFFFFFFFll)) {
				ps.error("panm's flags are a 32-bit word (decimal or 0x)");
				continue;
			}
			if (in.more() && !in.integer(frame, 0, 255)) {
				ps.error("panm matrix index is a byte");
				continue;
			}
			ThreediPartAnimation &pa = model.add_panm(lod, static_cast<int>(p), static_cast<int>(parent));
			if (given) {
				pa.flags = static_cast<uint32_t>(flags);
				pa.matrix_index = static_cast<uint8_t>(frame);
				raw_flags[{lod, static_cast<int>(model.lods[lod].panm.size()) - 1}] = true;
			}
		} else if (key == "track") {
			// track target style REG|-|param rate start end [axis]: styles above
			// 0x70 name a declared register; the others may carry a phase byte.
			std::string target, reg;
			long long style = 0, rate = 0, start = 0, end = 0;
			if (lod < 0 || model.lods[lod].panm.empty() || !in.name(target) || !in.integer(style) || !in.name(reg) ||
					!in.integer(rate) || !in.integer(start) || !in.integer(end)) {
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
			long long axis = THREEDI_TRANS_Z;
			if (in.more() && (t != 6 || !in.integer(axis, THREEDI_TRANS_X, THREEDI_TRANS_Z))) {
				ps.error("only a trans track takes an axis, 1 (x), 2 (y) or 3 (z)");
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
			check_register(style, param, "track");
			ThreediPartAnimation &pa = model.lods[lod].panm.back();
			*panm_tracks(pa)[t] = threedi_build_track(static_cast<uint8_t>(style), static_cast<uint8_t>(param),
					static_cast<int16_t>(rate), static_cast<int16_t>(start), static_cast<int16_t>(end));
			const int row = static_cast<int>(model.lods[lod].panm.size()) - 1;
			if (t == 6) trans_axis[{lod, row}] = static_cast<int>(axis);
			if (!raw_flags.count({lod, row}))
				pa.flags = threedi_build_panm_flags(pa,
						trans_axis.count({lod, row}) ? static_cast<uint8_t>(trans_axis[{lod, row}]) : 0);
		} else if (key == "userpoint") {
			std::string name;
			double p[3], d[3];
			long long sub = 0, type = THREEDI_USER_POINT_GAMEPLAY;
			if (!in.name(name) || !in.numbers(p, 3) || !in.numbers(d, 3) || !in.integer(sub, INT32_MIN, INT32_MAX)) {
				ps.error("userpoint needs name x y z dx dy dz part [type]");
				continue;
			}
			if (in.more() && !in.integer(type, INT32_MIN, INT32_MAX)) {
				ps.error("userpoint's type is a whole number (71 G, 83 S)");
				continue;
			}
			if (name.size() > kNameChars) {
				ps.error("user point name '" + name + "' exceeds 15 characters");
				continue;
			}
			model.add_user_point(name.c_str(), ThreediBuildVec3{p[0], p[1], p[2]}, ThreediBuildVec3{d[0], d[1], d[2]},
					static_cast<int>(sub), static_cast<int32_t>(type));
		} else if (key == "light") {
			// light part x y z atten_start atten_end style rate phase|reg r g b r g b
			//       flags [dx dy dz falloff]
			long long sub = 0, style = 0, s[3], e[3], flags = 0;
			double p[3], atten[2], rate = 0, phase = 0, dir[3] = {0.0, 0.0, -1.0}, falloff = 0.0;
			if (!in.integer(sub, 0, 255) || !in.numbers(p, 3) || !in.numbers(atten, 2) || !in.integer(style, 0, 255) ||
					!in.number(rate) || !in.number(phase)) {
				ps.error("light needs part x y z atten_start atten_end style rate phase|reg r g b r g b flags "
						"(part and style bytes)");
				continue;
			}
			for (long long *c : {&s[0], &s[1], &s[2], &e[0], &e[1], &e[2]})
				if (ok && !in.integer(*c, 0, 255)) {
					ps.error("light's colours are bytes (0..255)");
					ok = false;
				}
			if (!ok) continue;
			if (!in.integer(flags, 0, 255)) {
				ps.error("light's flags are a byte");
				continue;
			}
			if (in.more()) {
				double spot[4];
				if (!in.numbers(spot, 4)) {
					ps.error("a spot light needs dx dy dz falloff");
					continue;
				}
				dir[0] = spot[0];
				dir[1] = spot[1];
				dir[2] = spot[2];
				falloff = spot[3];
			}
			// The rate packs into an unsigned word (times 256), the cone into
			// a byte of whole degrees.
			if (!(rate >= 0.0 && rate < 256.0)) {
				ps.error("light's rate is 0 up to 256 (a u16 of 1/256 steps)");
				continue;
			}
			if (!(falloff >= 0.0 && falloff < 256.0)) {
				ps.error("light's cone half-angle is 0 up to 256 degrees (a byte)");
				continue;
			}
			// The phase byte: phase * 256 for styles up to 0x70, else the CTRL
			// index (threedi_build_light_phase).
			uint8_t phase_byte;
			if (style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD) {
				if (phase != std::floor(phase) || !(std::fabs(phase) < 1e6)) {
					ps.error("a register-driven light's phase field is its register index");
					continue;
				}
				check_register(style, static_cast<long long>(phase), "light");
				phase_byte = static_cast<uint8_t>(static_cast<int>(phase));
			} else {
				phase_byte = threedi_build_light_phase(phase);
			}
			const int start[3] = {static_cast<int>(s[0]), static_cast<int>(s[1]), static_cast<int>(s[2])};
			const int end[3] = {static_cast<int>(e[0]), static_cast<int>(e[1]), static_cast<int>(e[2])};
			model.add_light(ThreediBuildVec3{p[0], p[1], p[2]}, atten[0], atten[1], static_cast<uint8_t>(style),
					static_cast<int>(sub), start, end, static_cast<uint8_t>(flags), phase_byte, threedi_build_light_rate(rate),
					ThreediBuildVec3{dir[0], dir[1], dir[2]}, falloff);
		} else if (key == "occ") {
			long long type = 0, a = 0, b = 0;
			if (!in.integer(type, 0, 255) || !in.integer(a, 0, 255) || !in.integer(b, 0, 255)) {
				ps.error("occ needs type section connecting (bytes)");
				continue;
			}
			occ.open = true;
			occ.line = ps.line;
			occ.type = static_cast<int>(type);
			occ.section_a = static_cast<int>(a);
			occ.section_b = static_cast<int>(b);
		} else if (key == "cxlt") {
			// A CXLT row (mission axes, the frame of the section offsets), in
			// order; a bare `cxlt` declares the table empty. Any cxlt record
			// replaces the rows build would derive from the sections.
			model.translations_given = true;
			if (!in.more()) continue;
			double p[3];
			if (!in.numbers(p, 3)) {
				ps.error("cxlt needs x y z (or nothing: an empty table)");
				continue;
			}
			model.translations.push_back(ThreediBuildVec3{p[0], p[1], p[2]});
		} else if (key == "cobj") {
			long long parent = 0;
			double o[3] = {0, 0, 0};
			if (!in.integer(parent, INT32_MIN, INT32_MAX)) {
				ps.error("cobj needs a parent part");
				continue;
			}
			if (in.more() && !in.numbers(o, 3)) {
				ps.error("cobj's offset needs ox oy oz");
				continue;
			}
			cobj = model.add_cobj(static_cast<int>(parent), ThreediBuildVec3{o[0], o[1], o[2]});
		} else if (key == "csphere") {
			// A bone section's hit sphere (retail persons: one per bone), and
			// optionally the bounds of the vertices the bone moves.
			double c[3], r = 0, b[6];
			if (cobj < 0 || !in.numbers(c, 3) || !in.number(r) || !(r >= 0)) {
				ps.error("csphere needs an open cobj and cx cy cz radius [minx miny minz maxx maxy maxz]");
				continue;
			}
			ThreediBuildCollisionObject &o = model.collision[cobj];
			o.sphere = true;
			o.sphere_center = ThreediBuildVec3{c[0], c[1], c[2]};
			o.sphere_radius = r;
			if (in.more()) {
				if (!in.numbers(b, 6)) {
					ps.error("csphere's bounds need minx miny minz maxx maxy maxz");
					continue;
				}
				o.sphere_bounded = true;
				o.sphere_bounds = ThreediBuildBox{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}};
			}
		} else if (key == "cvol" || key == "cvolume") {
			// cvol: an axis box (six planes). cvolume: a convex volume over an
			// explicit plane list, the 'cp' lines that follow; the box is its
			// AABB (mission axes).
			long long type = 0, flags = 0;
			double b[6];
			if (cobj < 0 || !in.integer(type, INT32_MIN, INT32_MAX) || !in.integer(flags, INT32_MIN, INT32_MAX) ||
					!in.numbers(b, 6)) {
				ps.error(key + " needs an open cobj, type flags and a box minx miny minz maxx maxy maxz");
				continue;
			}
			const ThreediBuildBox box{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}};
			if (key == "cvol") {
				model.add_volume(cobj, static_cast<int32_t>(type), static_cast<int32_t>(flags), box);
			} else {
				model.add_volume_planes(cobj, static_cast<int32_t>(type), static_cast<int32_t>(flags), box, {});
				volume_open = cobj;
			}
		} else if (key == "cvmesh") {
			// A volume given as its authored triangles: `vv` vertices and `vf`
			// faces follow; its planes, box and seam flags are derived.
			long long type = 0, flags = 0;
			if (cobj < 0 || !in.integer(type, INT32_MIN, INT32_MAX) || !in.integer(flags, INT32_MIN, INT32_MAX)) {
				ps.error("cvmesh needs an open cobj and type flags [label]");
				continue;
			}
			std::string label;
			if (in.more()) in.name(label);
			vol.open = true;
			vol.cobj = cobj;
			vol.type = static_cast<int>(type);
			vol.flags = static_cast<int>(flags);
			vol.label = label;
			vol.line = ps.line;
		} else {
			ps.error("unknown record '" + key + "'");
			continue;
		}
		if (in.more()) ps.error("unexpected '" + in.peek() + "' after the record");
	}
	flush_strip();
	flush_occ();
	flush_volume();
	// The vertex layout carries tangents when any material's shader reads the
	// TANGENT semantic (ComputeVertexFormatFlags [orig: @ 0x457a10
	// (ModSuperOed.exe)]; docs/threedi/3di-gp-format-re.md); the builder derives
	// their values. Retail's object-space bump shaders (VS_PHONGO,
	// VS_SKBUMPDIFFOBJ) carry none: Colt_1st, Boonie.
	for (const ThreediMaterial &mat : model.materials) {
		const opennova::renderer::MaterialDescriptorRecord *desc = opennova::renderer::find_material_descriptor(mat.shader_name);
		if (desc != nullptr && (desc->shader_flags & opennova::renderer::MATERIAL_FLAG_TANGENT) != 0) model.tangents = true;
		if (desc == nullptr)
			std::fprintf(stderr, "%s: note: shader '%s' is not in the engine's shader table\n", ps.path.c_str(),
					mat.shader_name);
	}
	if (!header) ps.model_error("empty scene");
	return ps.errors.empty();
}

// Whole-model checks retail imposes that no single record can see.
void validate(Parser &ps, const ThreediBuildModel &m) {
	if (m.name.empty()) ps.model_error("no 'model' record");
	if (m.lods.empty()) ps.model_error("no 'lod' record");
	for (size_t li = 0; li < m.lods.size(); ++li) {
		// A LOD with no parts is legal: retail ships them (Dblkhwk1's LOD 4).
		const ThreediBuildLod &lod = m.lods[li];
		if (lod.parts.size() > 255) ps.model_error("lod " + std::to_string(li) + " has more than 255 parts");
		for (const ThreediBuildPart &p : lod.parts)
			for (const ThreediBuildStrip &s : p.strips) {
				if (s.indices.size() > 65535) ps.model_error("a strip exceeds 65535 indices (u16 STRP count)");
				if (m.skinned && s.bone_table.empty()) ps.model_error("a skinned strip has no 'bones' table");
				for (uint8_t b : s.bone_table)
					if (b >= lod.parts.size()) ps.model_error("a strip's bone table names a part the LOD lacks");
			}
		for (const ThreediBuildPart &p : lod.parts) {
			if (p.parent >= static_cast<int>(lod.parts.size()))
				ps.model_error("lod " + std::to_string(li) + " has a part whose parent it lacks");
			if (p.has_center && !p.strips.empty())
				ps.model_error("lod " + std::to_string(li) + ": a part that draws takes its sphere from its vertices (no centre)");
		}
		for (const ThreediPartAnimation &pa : lod.panm) {
			if (pa.subobject_index >= lod.parts.size())
				ps.model_error("panm in lod " + std::to_string(li) + " names a missing part");
			if (static_cast<int8_t>(pa.matrix_index) > static_cast<int>(m.frames.size()))
				ps.model_error("panm in lod " + std::to_string(li) + " selects an mtrx frame the model lacks");
		}
	}
	for (size_t o = 0; o < m.collision.size(); ++o) {
		// A bullet face names its normal by a signed 16-bit index too
		// [orig: Physics_RaycastAgainstBoneCollision @ 0x4E5079]; section o
		// pairs with part o of the collision LOD.
		if (m.collision[o].normals.size() > static_cast<size_t>(SHRT_MAX) + 1)
			ps.model_error("collision section " + std::to_string(o) + " (part " + std::to_string(o) + ") has " +
					grouped(m.collision[o].normals.size()) +
					" distinct bullet-face normals, past the 32,768 a face's signed 16-bit normal index reaches: "
					"simplify its bullet faces (faces in one plane share a normal), split them over more parts, or "
					"give the part none");
		for (const ThreediBoundingVolume &v : m.collision[o].volumes)
			if (v.plane_count < 4) ps.model_error("cobj " + std::to_string(o) + " has a volume with fewer than 4 planes");
	}
	for (const ThreediLight &l : m.lights)
		if (l.subobj_index != 0 && (m.lods.empty() || l.subobj_index >= m.lods[0].parts.size()))
			ps.model_error("a light names a part LOD 0 lacks");
	// The seat scan reads `sitex` as a case-insensitive prefix [orig:
	// Entity_GetBoneSlotType @ 0x434ED0, the strnicmp @ 0x434F16] and stops
	// after 8 [orig: the scan end `cmp ebp, 8; jg` @ 0x43A5AF]
	// (runtime/mission/seat_spec_extract.cpp).
	int seats = 0;
	for (const ThreediUserPoint &u : m.user_points)
		if (opennova::strutil::starts_with_icase(u.name, "sitex")) ++seats;
	if (seats > 8) ps.model_error("more than 8 sitex seats (retail's scan stops at 8)");
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
	ThreediChunkOverflow overflow{};
	if (!threedi_build_mint(model, bytes, &overflow)) {
		if (overflow.chunk[0] != '\0')
			std::fprintf(stderr,
					"opennova-3di: the model is too large to write: its %s chunk holds %s bytes, past the %s a 3DI3 "
					"chunk's 24-bit length can say (ROOT holds the whole model and each RLOD one LOD: use fewer "
					"vertices, triangles, LODs or collision faces)\n",
					overflow.chunk, grouped(overflow.bytes).c_str(), grouped(THREEDI_3DI3_LENGTH_MASK).c_str());
		else
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
	if (!write_output(out_path, bytes.data(), bytes.size())) return 1;
	std::printf("wrote %s (%zu bytes)\n", out_path, bytes.size());
	return 0;
}

} // namespace threedi_cli
