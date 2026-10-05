// The `.o3d` scene text into its parse (threedi_o3d_read.h).

#include <formats/threedi/threedi_o3d_read.h>

#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_panm.h>

namespace opennova::threedi {

namespace {

bool fits_s16(long long v) { return v >= SHRT_MIN && v <= SHRT_MAX; }

struct Reader {
	ThreediO3dModel &model;
	std::vector<SceneFinding> &findings;
	int line = 0;
	// The open containers: a record that is not one of a container's own
	// closes it (`v`, `vt`, `t` a mesh; `cp` a cvolume; `ov`, `op`, `of` an
	// occ; `vv`, `vf` a cvmesh). A lod, part, material and cobj stay open
	// until the next of their kind.
	int lod = -1, part = -1, material = -1, section = -1;
	int mesh = -1;          // in the open part
	int plane_volume = -1;  // the open cvolume, in the open section
	int mesh_volume = -1;   // the open cvmesh, in the open section
	bool occ_open = false;  // the last occlusion record

	void error(const std::string &what) { error_at(line, what); }
	void error_at(int at, const std::string &what) { findings.push_back(SceneFinding{at, true, what}); }
	// A whole-model check no single line owns.
	void model_error(const std::string &what) { findings.push_back(SceneFinding{0, true, what}); }
	bool failed() const {
		for (const SceneFinding &f : findings)
			if (f.error) return true;
		return false;
	}

	ThreediO3dMesh &open_mesh() { return model.lods[lod].parts[part].meshes[mesh]; }

	// A record's register field: a declared register when the record names
	// one (threedi_generator_names_register, threedi_flipbook_reads_register).
	bool declared(bool names_register, long long reg, const std::string &what) {
		if (!names_register || (reg >= 0 && reg < static_cast<long long>(model.registers.size()))) return true;
		error(what + " register " + std::to_string(reg) + " is not a declared 'register'");
		return false;
	}

	void close_mesh() {
		if (mesh < 0) return;
		const ThreediO3dMesh &m = open_mesh();
		if (!m.frames.empty() && m.frames.size() != m.vertices.size())
			error_at(m.line, "the mesh gives 'vt' on " + std::to_string(m.frames.size()) + " of its " +
					std::to_string(m.vertices.size()) + " vertices: give a tangent frame on every vertex or none");
		mesh = -1;
	}
	void close_occ() {
		if (!occ_open) return;
		occ_open = false;
		const ThreediO3dOcclusion &o = model.occlusion.back();
		const bool given = !o.planes.empty();
		for (const std::array<int, 4> &f : o.faces) {
			if (given != (f[3] >= 0)) {
				error_at(o.line, "occ: give every face a plane index with explicit 'op' planes, or none without");
				return;
			}
			if (given && f[3] >= static_cast<int>(o.planes.size())) {
				error_at(o.line, "occ: a face names a plane the record lacks");
				return;
			}
		}
	}
	void close_mesh_volume() {
		if (mesh_volume < 0) return;
		const ThreediO3dVolume &v = model.sections[section].volumes[mesh_volume];
		if (v.vertices.size() < 4 || v.triangles.empty())
			error_at(v.line, "cvmesh: a volume needs at least 4 vertices and a triangle");
		mesh_volume = -1;
	}
	void close_plane_volume() {
		if (plane_volume < 0) return;
		const ThreediO3dVolume &v = model.sections[section].volumes[plane_volume];
		// A convex solid needs four planes to bound it.
		if (v.planes.size() < 4)
			error_at(v.line, "cvolume has " + std::to_string(v.planes.size()) +
					" planes: a volume is the solid its planes bound, at least 4");
		plane_volume = -1;
	}
};

// A skinned vertex's influences: `part weight` pairs after the uv, the
// primary first. Each weight is a finite number from 0 to 1, no part comes
// twice, and together they blend the vertex wholly: they sum to 1, within the
// 1e-4 retail's four-decimal weights reach (ArmGlovD's sum to 1.0001), added
// in float as the shader adds them.
bool read_influences(Reader &r, SceneLine &in, ThreediO3dMesh &mesh, ThreediO3dVertex &vert) {
	const size_t first = mesh.influences.size();
	float sum = 0.0f;
	while (in.more()) {
		long long part = 0;
		double weight = 0.0;
		if (!in.integer(part) || part < 0 || !in.number(weight)) {
			r.error("a skinned v's influences are pairs of a part (an index of its LOD) and its weight");
			mesh.influences.resize(first);
			return false;
		}
		if (!(weight >= 0.0 && weight <= 1.0)) {
			r.error("skinned v gives part " + std::to_string(part) + " the weight " + f9(weight) +
					": a weight is a finite number from 0 to 1");
			mesh.influences.resize(first);
			return false;
		}
		for (size_t k = first; k < mesh.influences.size(); ++k)
			if (mesh.influences[k].part == part) {
				r.error("skinned v names part " + std::to_string(part) + " twice: give each part once, with its whole weight");
				mesh.influences.resize(first);
				return false;
			}
		mesh.influences.push_back(ThreediO3dInfluence{part, weight});
		sum += static_cast<float>(weight);
	}
	if (mesh.influences.size() == first) {
		r.error("a skinned v needs its influences after the uv: part weight, the primary first");
		return false;
	}
	if (!(sum >= 0.9999f && sum <= 1.0001f)) {
		r.error("skinned v weights sum to " + f9(sum) + ": a vertex's weights blend it wholly, summing to 1");
		mesh.influences.resize(first);
		return false;
	}
	vert.first_influence = static_cast<uint32_t>(first);
	vert.influence_count = static_cast<uint32_t>(mesh.influences.size() - first);
	return true;
}

// A record inside a mesh: `v`, `vt` or `t`. False when it reported an error.
bool read_mesh_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dMesh &mesh = r.open_mesh();
	if (key == "v") {
		ThreediO3dVertex vert;
		vert.line = r.line;
		if (!in.numbers(vert.position, 3) || !in.numbers(vert.normal, 3) || !in.numbers(vert.uv0, 2)) {
			r.error("v needs px py pz nx ny nz u v");
			return false;
		}
		if (r.model.uv1) {
			if (!in.numbers(vert.uv1, 2)) {
				r.error("with 'uv1 1' a v carries u1 v1 after its u v");
				return false;
			}
		} else {
			vert.uv1[0] = vert.uv0[0];
			vert.uv1[1] = vert.uv0[1];
		}
		if (r.model.skinned && !read_influences(r, in, mesh, vert)) return false;
		mesh.vertices.push_back(vert);
	} else if (key == "vt") {
		ThreediO3dFrame frame;
		if (!in.numbers(frame.data(), 6)) {
			r.error("vt needs tx ty tz bx by bz");
			return false;
		}
		// The frame of the v before it, once.
		if (mesh.frames.size() + 1 != mesh.vertices.size()) {
			r.error("a vt follows its own v: give a tangent frame on every vertex of the mesh, after it, or none");
			return false;
		}
		if (mesh.frames.empty()) mesh.frames_line = r.line;
		mesh.frames.push_back(frame);
	} else {
		long long a = 0, b = 0, c = 0;
		const long long count = static_cast<long long>(mesh.vertices.size());
		if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
			r.error("t needs three vertices already declared in this mesh");
			return false;
		}
		if (a == b || b == c || a == c) {
			r.error("t repeats a vertex: a triangle has three corners");
			return false;
		}
		mesh.triangles.push_back({static_cast<uint32_t>(a), static_cast<uint32_t>(b), static_cast<uint32_t>(c)});
	}
	if (in.more()) {
		r.error("unexpected '" + in.peek() + "' after the record");
		return false;
	}
	return true;
}

// A record inside an occlusion record: `ov`, `op` or `of`.
void read_occ_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dOcclusion &occ = r.model.occlusion.back();
	if (key == "ov") {
		double p[3];
		if (!in.numbers(p, 3)) {
			r.error("ov needs x y z");
			return;
		}
		occ.vertices.push_back(ThreediBuildVec3{p[0], p[1], p[2]});
	} else if (key == "op") {
		double p[4];
		if (!in.numbers(p, 4)) {
			r.error("op needs nx ny nz d");
			return;
		}
		occ.planes.push_back({p[0], p[1], p[2], p[3]});
	} else {
		long long a = 0, b = 0, c = 0, plane = -1;
		const long long count = static_cast<long long>(occ.vertices.size());
		if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
			r.error("of needs three vertices already declared in this occ record");
			return;
		}
		if (in.more() && (!in.integer(plane) || plane < 0 || plane > INT_MAX)) {
			r.error("of's plane is an index of the record's planes");
			return;
		}
		occ.faces.push_back({static_cast<int>(a), static_cast<int>(b), static_cast<int>(c), static_cast<int>(plane)});
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
}

// A record inside a volume authored as triangles: `vv` or `vf`.
void read_volume_mesh_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dVolume &vol = r.model.sections[r.section].volumes[r.mesh_volume];
	if (key == "vv") {
		double p[3];
		if (!in.numbers(p, 3)) {
			r.error("vv needs x y z");
			return;
		}
		vol.vertices.push_back(ThreediBuildVec3{p[0], p[1], p[2]});
	} else {
		long long a = 0, b = 0, c = 0;
		const long long count = static_cast<long long>(vol.vertices.size());
		if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
			r.error("vf needs three vertices already declared in this cvmesh");
			return;
		}
		vol.triangles.push_back({static_cast<int>(a), static_cast<int>(b), static_cast<int>(c)});
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
}

// A material record: `texture`, `texanim`, `reflect`, `matflags`,
// `alphatest`, `glass`, `emissive`, `rgbgen`, `alphagen`, `ugen`, `vgen`.
// False when the key is none of them.
bool read_material_record(Reader &r, const std::string &key, SceneLine &in) {
	static const char *const kKeys[] = {"texture", "texanim", "reflect", "matflags", "alphatest", "glass",
			"emissive", "rgbgen", "alphagen", "ugen", "vgen"};
	bool known = false;
	for (const char *k : kKeys) known = known || key == k;
	if (!known) return false;
	if (r.material < 0) {
		r.error(key + " needs an open material");
		return true;
	}
	ThreediO3dMaterial &m = r.model.materials[r.material];
	bool ok = true;
	if (key == "texture") {
		ThreediO3dTexture t;
		t.line = r.line;
		long long field[4] = {THREEDI_TEX_SLOT_DIFFUSE, THREEDI_TEX_TYPE_DIFFUSE, 0, 0};
		if (!in.name(t.name)) {
			r.error("texture needs a file name");
			return true;
		}
		for (long long &f : field)
			if (ok && in.more() && !in.integer(f, 0, 255)) {
				r.error("texture's slot, type, flags and frame are bytes");
				ok = false;
			}
		if (!ok) return true;
		t.slot = static_cast<uint8_t>(field[0]);
		t.type = static_cast<uint8_t>(field[1]);
		t.flags = static_cast<uint8_t>(field[2]);
		t.frame = static_cast<uint8_t>(field[3]);
		m.textures.push_back(t);
	} else if (key == "texanim") {
		long long frames = 0, type = 0, time = 0;
		if (!in.integer(frames) || !in.integer(type) || !in.integer(time)) {
			r.error("texanim needs frames type time");
			return true;
		}
		if (frames < 0 || type < 0 || type > 1 || !fits_s16(time)) {
			r.error("texanim frames is a count, type is 0 or 1, and time/register is int16");
			return true;
		}
		// A flipbook with frames on the register clock names a register
		// (threedi_flipbook_reads_register).
		if (!r.declared(frames != 0 && type == 1, time, "texanim")) return true;
		m.texanim_line = r.line;
		m.frames = frames;
		m.animation_type = static_cast<uint8_t>(type);
		m.time = static_cast<int16_t>(time);
	} else if (key == "reflect") {
		long long c[4];
		for (long long &v : c)
			if (ok && !in.integer(v, 0, 255)) {
				r.error("reflect needs r g b a (0..255)");
				ok = false;
			}
		if (!ok) return true;
		for (int k = 0; k < 4; ++k) m.reflect[k] = static_cast<uint8_t>(c[k]);
	} else if (key == "matflags" || key == "alphatest" || key == "glass" || key == "emissive") {
		long long value = 0;
		if (!in.integer(value, 0, 255)) {
			r.error(key + " needs a byte (0..255)");
			return true;
		}
		uint8_t &field = key == "matflags" ? m.flags : key == "alphatest" ? m.alpha_test : key == "glass" ? m.glass : m.emissive;
		field = static_cast<uint8_t>(value);
	} else if (key == "rgbgen") {
		ThreediO3dGenerator g;
		g.line = r.line;
		long long style = 0, c[6];
		if (!in.integer(style, 0, 255) || !in.integer(g.reg) || !in.number(g.rate)) {
			r.error("rgbgen needs style (a byte) reg rate r g b r g b [phase]");
			return true;
		}
		for (long long &v : c)
			if (ok && !in.integer(v, 0, 255)) {
				r.error("rgbgen's colours are bytes (0..255)");
				ok = false;
			}
		if (!ok) return true;
		if (in.more() && !in.number(g.phase)) {
			r.error("rgbgen's phase is a number");
			return true;
		}
		g.style = static_cast<uint8_t>(style);
		if (!r.declared(threedi_generator_names_register(g.style), g.reg, "rgbgen")) return true;
		for (int k = 0; k < 3; ++k) {
			g.start_rgb[k] = static_cast<int>(c[k]);
			g.end_rgb[k] = static_cast<int>(c[k + 3]);
		}
		m.rgbgen = g;
	} else {
		ThreediO3dGenerator g;
		g.line = r.line;
		long long style = 0;
		if (!in.integer(style, 0, 255) || !in.integer(g.reg) || !in.number(g.rate) || !in.number(g.start) ||
				!in.number(g.end)) {
			r.error(key + " needs style (a byte) reg rate start end [phase]");
			return true;
		}
		if (in.more() && !in.number(g.phase)) {
			r.error(key + "'s phase is a number");
			return true;
		}
		// The alpha generator stores start and end as int16 values.
		if (key == "alphagen" && !(g.start == std::floor(g.start) && g.end == std::floor(g.end) &&
										 std::fabs(g.start) < 1e6 && std::fabs(g.end) < 1e6 &&
										 fits_s16(static_cast<long long>(g.start)) &&
										 fits_s16(static_cast<long long>(g.end)))) {
			r.error("alphagen's start and end are whole int16 values");
			return true;
		}
		g.style = static_cast<uint8_t>(style);
		if (!r.declared(threedi_generator_names_register(g.style), g.reg, key)) return true;
		(key == "alphagen" ? m.alphagen : key == "ugen" ? m.ugen : m.vgen) = g;
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
	return true;
}

// A collision record: `cobj`, `csphere`, `cvol`, `cvolume`, `cp`, `cvmesh`,
// `cv`, `cf`. False when the key is none of them.
bool read_collision_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dModel &model = r.model;
	if (key == "cobj") {
		ThreediO3dSection s;
		s.line = r.line;
		double o[3] = {0, 0, 0};
		if (!in.integer(s.parent)) {
			r.error("cobj needs a parent part");
			return true;
		}
		if (in.more() && !in.numbers(o, 3)) {
			r.error("cobj's offset needs ox oy oz");
			return true;
		}
		s.offset = ThreediBuildVec3{o[0], o[1], o[2]};
		model.sections.push_back(s);
		r.section = static_cast<int>(model.sections.size()) - 1;
	} else if (key == "csphere") {
		// A bone section's hit sphere (retail persons: one per bone), and
		// optionally the bounds of the vertices the bone moves.
		double c[3], radius = 0, b[6];
		if (r.section < 0 || !in.numbers(c, 3) || !in.number(radius) || !(radius >= 0)) {
			r.error("csphere needs an open cobj and cx cy cz radius [minx miny minz maxx maxy maxz]");
			return true;
		}
		ThreediO3dSection &s = model.sections[r.section];
		s.sphere_line = r.line;
		s.sphere_center = ThreediBuildVec3{c[0], c[1], c[2]};
		s.sphere_radius = radius;
		if (in.more()) {
			if (!in.numbers(b, 6)) {
				r.error("csphere's bounds need minx miny minz maxx maxy maxz");
				return true;
			}
			s.sphere_bounded = true;
			s.sphere_bounds = ThreediBuildBox{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}};
		}
	} else if (key == "cvol" || key == "cvolume" || key == "cvmesh") {
		// cvol: an axis box (six planes). cvolume: a convex volume over an
		// explicit plane list, the `cp` lines that follow; the box is its
		// AABB (mission axes). cvmesh: a volume given as its authored
		// triangles, `vv` vertices and `vf` faces; its planes, box and seam
		// flags are derived.
		ThreediO3dVolume v;
		v.line = r.line;
		long long type = 0, flags = 0;
		double b[6] = {};
		if (r.section < 0 || !in.integer(type, INT32_MIN, INT32_MAX) || !in.integer(flags, INT32_MIN, INT32_MAX)) {
			r.error(key + " needs an open cobj and type flags (32-bit words)");
			return true;
		}
		if (key != "cvmesh" && !in.numbers(b, 6)) {
			r.error(key + " needs a box minx miny minz maxx maxy maxz after its type and flags");
			return true;
		}
		if (key == "cvmesh" && in.more()) in.name(v.label);
		v.kind = key == "cvol" ? ThreediO3dVolume::Kind::box : key == "cvolume" ? ThreediO3dVolume::Kind::planes
																			  : ThreediO3dVolume::Kind::mesh;
		v.type = static_cast<int32_t>(type);
		v.flags = static_cast<int32_t>(flags);
		v.box = ThreediBuildBox{{b[0], b[1], b[2]}, {b[3], b[4], b[5]}};
		std::vector<ThreediO3dVolume> &volumes = model.sections[r.section].volumes;
		volumes.push_back(v);
		if (key == "cvolume") r.plane_volume = static_cast<int>(volumes.size()) - 1;
		if (key == "cvmesh") r.mesh_volume = static_cast<int>(volumes.size()) - 1;
	} else if (key == "cp") {
		// One plane of the open cvolume: outward normal, n . p + d == 0 on
		// the plane, flags (retail sets 1 on seams).
		ThreediO3dPlane p;
		p.line = r.line;
		long long flags = 0;
		if (r.plane_volume < 0 || !in.numbers(p.normal, 3) || !in.number(p.distance)) {
			r.error("cp needs an open cvolume and nx ny nz d [flags]");
			return true;
		}
		if (in.more() && !in.integer(flags, SHRT_MIN, SHRT_MAX)) {
			r.error("cp's flags are an int16");
			return true;
		}
		p.flags = static_cast<int16_t>(flags);
		model.sections[r.section].volumes[r.plane_volume].planes.push_back(p);
	} else if (key == "cv") {
		ThreediO3dCollisionVertex v;
		v.line = r.line;
		double p[3];
		if (r.section < 0 || !in.numbers(p, 3)) {
			r.error("cv needs an open cobj and x y z");
			return true;
		}
		v.position = ThreediBuildVec3{p[0], p[1], p[2]};
		model.sections[r.section].vertices.push_back(v);
	} else if (key == "cf") {
		// cf a b c [poly flags [nx ny nz]]: an explicit normal is for a face
		// whose corners collapse on the 8.8 grid (`scene` writes retail's).
		ThreediO3dCollisionFace f;
		f.line = r.line;
		long long a = 0, b = 0, c = 0, poly = 1, flags = 0;
		double n[3] = {};
		if (r.section < 0) {
			r.error("cf needs an open cobj");
			return true;
		}
		const long long count = static_cast<long long>(model.sections[r.section].vertices.size());
		if (!in.integer(a, 0, count - 1) || !in.integer(b, 0, count - 1) || !in.integer(c, 0, count - 1)) {
			r.error("cf needs three collision vertices already declared in this cobj");
			return true;
		}
		if ((in.more() && !in.integer(poly, 0, 255)) || (in.more() && !in.integer(flags, 0, 0xFFFFFFFFll))) {
			r.error("cf's poly_type is a byte and its flags a 32-bit word (decimal or 0x)");
			return true;
		}
		f.normal_given = in.more();
		if (f.normal_given && !in.numbers(n, 3)) {
			r.error("cf's normal needs nx ny nz");
			return true;
		}
		f.corner[0] = static_cast<uint32_t>(a);
		f.corner[1] = static_cast<uint32_t>(b);
		f.corner[2] = static_cast<uint32_t>(c);
		f.poly_type = static_cast<uint8_t>(poly);
		f.flags = static_cast<uint32_t>(flags);
		f.normal = ThreediBuildVec3{n[0], n[1], n[2]};
		model.sections[r.section].faces.push_back(f);
	} else {
		return false;
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
	return true;
}

// A record of the render LODs: `lod`, `part`, `mesh`, `panm`, `track`. False
// when the key is none of them.
bool read_lod_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dModel &model = r.model;
	if (key == "lod") {
		ThreediO3dLod lod;
		lod.line = r.line;
		long long threshold = 0;
		if (in.more() && !in.integer(threshold, INT32_MIN, INT32_MAX)) {
			r.error("lod's threshold is a whole number (32-bit)");
			return true;
		}
		if (in.more() && !in.name(lod.type)) {
			r.error("lod's type is a name");
			return true;
		}
		lod.threshold = static_cast<int32_t>(threshold);
		model.lods.push_back(lod);
		r.lod = static_cast<int>(model.lods.size()) - 1;
		r.part = -1;
	} else if (key == "part") {
		ThreediO3dPart p;
		p.line = r.line;
		double pivot[3];
		if (r.lod < 0 || !in.integer(p.parent) || !in.numbers(pivot, 3)) {
			r.error("part needs an open lod, a parent index and a pivot x y z");
			return true;
		}
		if (p.parent < -1) {
			r.error("part parent is -1 or a part index");
			return true;
		}
		p.pivot = ThreediBuildVec3{pivot[0], pivot[1], pivot[2]};
		// A part that draws nothing may give the point its sphere sits on
		// (radius 0): the exporter seeds such a part with a placeholder
		// vertex (the retail parts' `_## center` helper mesh, near the pivot).
		if (in.more()) {
			double c[3];
			if (!in.numbers(c, 3)) {
				r.error("part's sphere centre needs cx cy cz");
				return true;
			}
			p.has_center = true;
			p.center = ThreediBuildVec3{c[0], c[1], c[2]};
		}
		model.lods[r.lod].parts.push_back(p);
		r.part = static_cast<int>(model.lods[r.lod].parts.size()) - 1;
	} else if (key == "mesh") {
		ThreediO3dMesh m;
		m.line = r.line;
		long long alpha = 0;
		if (r.part < 0 || !in.integer(m.material)) {
			r.error("mesh needs an open part and a material index");
			return true;
		}
		if (in.more() && !in.integer(alpha, 0, 1)) {
			r.error("mesh's alpha is 0 or 1");
			return true;
		}
		if (m.material < 0 || m.material >= static_cast<long long>(model.materials.size())) {
			r.error("mesh names material " + std::to_string(m.material) + ": the model declares " +
					std::to_string(model.materials.size()) + " before it");
			return true;
		}
		m.alpha = alpha != 0;
		std::vector<ThreediO3dMesh> &meshes = model.lods[r.lod].parts[r.part].meshes;
		meshes.push_back(std::move(m));
		r.mesh = static_cast<int>(meshes.size()) - 1;
	} else if (key == "panm") {
		// panm part parent [flags [matrix]]: flags given verbatim override
		// the ones the tracks imply; matrix is the frame selector byte.
		ThreediO3dPanm pa;
		pa.line = r.line;
		if (r.lod < 0 || !in.integer(pa.part) || !in.integer(pa.parent)) {
			r.error("panm needs an open lod, a part and its parent");
			return true;
		}
		if (pa.part < 0 || pa.parent < -1) {
			r.error("panm's part is a part index and its parent -1 or one");
			return true;
		}
		// The runtime reads the table by row and poses a part by its last
		// row (threedi_panm_pose.cpp); every retail table is canonical, row i
		// transforming part i (all 3,250 JO tables).
		const size_t rows = model.lods[r.lod].panm.size();
		if (pa.part != static_cast<long long>(rows)) {
			r.error("panm rows go in part order, one per part: this row must transform part " + std::to_string(rows));
			return true;
		}
		long long flags = 0;
		pa.flags_given = in.more();
		if (pa.flags_given && !in.integer(flags, 0, 0xFFFFFFFFll)) {
			r.error("panm's flags are a 32-bit word (decimal or 0x)");
			return true;
		}
		if (in.more() && !in.integer(pa.matrix, 0, 255)) {
			r.error("panm's matrix is the frame selector byte (1..127 an mtrx frame)");
			return true;
		}
		pa.flags = static_cast<uint32_t>(flags);
		model.lods[r.lod].panm.push_back(pa);
	} else if (key == "track") {
		// track target style REG|-|param rate start end [axis]: styles above
		// 0x70 name a declared register; the others may carry a phase byte.
		ThreediO3dTrack t;
		t.line = r.line;
		std::string target, reg;
		long long style = 0, rate = 0, start = 0, end = 0;
		if (r.lod < 0 || model.lods[r.lod].panm.empty() || !in.name(target) || !in.integer(style) || !in.name(reg) ||
				!in.integer(rate) || !in.integer(start) || !in.integer(end)) {
			r.error("track needs an open panm and target style register|-|param rate start end [axis]");
			return true;
		}
		t.target = threedi_panm_track_index(target);
		if (t.target < 0) {
			r.error("unknown track target '" + target + "'");
			return true;
		}
		if (style < 0 || style > 255 || !fits_s16(rate) || !fits_s16(start) || !fits_s16(end)) {
			r.error("track style is a byte and rate/start/end are int16 (rotations 1/16384 turn, others 8.8)");
			return true;
		}
		long long axis = 0;
		if (in.more() && (t.target != 6 || !in.integer(axis, THREEDI_TRANS_X, THREEDI_TRANS_Z))) {
			r.error("only a trans track takes an axis, 1 (x), 2 (y) or 3 (z)");
			return true;
		}
		if (reg != "-") {
			char *stop = nullptr;
			const long value = std::strtol(reg.c_str(), &stop, 10);
			if (!reg.empty() && stop != nullptr && *stop == '\0') {
				if (value < 0 || value > 255) {
					r.error("track param is a byte");
					return true;
				}
				t.param = value;
			} else {
				t.param = -1;
				for (size_t k = 0; k < model.registers.size(); ++k)
					if (model.registers[k].name == reg) t.param = static_cast<long long>(k);
				if (t.param < 0) {
					r.error("track register '" + reg + "' is not declared with 'register'");
					return true;
				}
				t.names_register = true;
			}
		}
		t.style = static_cast<uint8_t>(style);
		if (!r.declared(threedi_generator_names_register(t.style), t.param, "track")) return true;
		t.rate = static_cast<int16_t>(rate);
		t.start = static_cast<int16_t>(start);
		t.end = static_cast<int16_t>(end);
		t.axis = static_cast<int>(axis);
		model.lods[r.lod].panm.back().tracks.push_back(t);
	} else {
		return false;
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
	return true;
}

// The model's other records: `model`, `tangents`, `skinned`, `uv1`,
// `register`, `mtrx`, `material`, `userpoint`, `light`, `occ`, `cxlt`.
void read_model_record(Reader &r, const std::string &key, SceneLine &in) {
	ThreediO3dModel &model = r.model;
	if (key == "model") {
		if (!in.name(model.name.name) || model.name.name.empty()) {
			r.error("model needs a name");
			return;
		}
		model.name.line = r.line;
	} else if (key == "tangents" || key == "skinned" || key == "uv1") {
		// tangents: VERT carries tangent/bitangent (a TANGENT shader turns
		// it on anyway). skinned: GHDR mesh type 2 (parts are the
		// skeleton's bones, vertices carry influences). uv1: every vertex
		// carries its second UV set (the detail stage of FF_MT shaders).
		long long on = 0;
		if (!in.integer(on, 0, 1)) {
			r.error(key + " takes 0 or 1");
			return;
		}
		if (key != "tangents" && !model.lods.empty()) r.error(key + " must precede the first lod");
		if (key == "tangents") model.tangents_line = on != 0 ? r.line : 0;
		else if (key == "skinned") model.skinned = on != 0;
		else model.uv1 = on != 0;
	} else if (key == "register") {
		ThreediO3dNamed reg;
		reg.line = r.line;
		if (!in.name(reg.name)) {
			r.error("register needs a name");
			return;
		}
		model.registers.push_back(reg);
	} else if (key == "mtrx") {
		// A rotation frame (MTRX row 1, 2, ...; row 0 is the identity) in
		// mission axes, row-major; a PANM row selects it by index.
		ThreediO3dFrameRecord f;
		f.line = r.line;
		if (!in.numbers(f.rotation, 9)) {
			r.error("mtrx needs nine values (a 3x3 rotation, row-major)");
			return;
		}
		model.frames.push_back(f);
	} else if (key == "material") {
		ThreediO3dMaterial m;
		m.line = r.line;
		if (!in.name(m.shader)) {
			r.error("material needs a shader tag");
			return;
		}
		model.materials.push_back(m);
		r.material = static_cast<int>(model.materials.size()) - 1;
	} else if (key == "userpoint") {
		ThreediO3dUserPoint u;
		u.line = r.line;
		long long type = THREEDI_USER_POINT_GAMEPLAY;
		if (!in.name(u.name) || !in.numbers(u.position, 3) || !in.numbers(u.direction, 3) || !in.integer(u.part)) {
			r.error("userpoint needs name x y z dx dy dz part [type]");
			return;
		}
		if (in.more() && !in.integer(type, INT32_MIN, INT32_MAX)) {
			r.error("userpoint's type is a whole number (71 G, 83 S)");
			return;
		}
		u.type = static_cast<int32_t>(type);
		model.user_points.push_back(u);
	} else if (key == "light") {
		// light part x y z atten_start atten_end style rate phase|reg r g b r g b
		//       flags [dx dy dz falloff]
		ThreediO3dLight l;
		l.line = r.line;
		long long style = 0, c[6], flags = 0;
		bool ok = true;
		if (!in.integer(l.part) || l.part < 0 || !in.numbers(l.position, 3) || !in.number(l.atten_start) ||
				!in.number(l.atten_end) || !in.integer(style, 0, 255) || !in.number(l.rate) || !in.number(l.phase)) {
			r.error("light needs part x y z atten_start atten_end style rate phase|reg r g b r g b flags "
					"(a part index and a style byte)");
			return;
		}
		for (long long &v : c)
			if (ok && !in.integer(v, 0, 255)) {
				r.error("light's colours are bytes (0..255)");
				ok = false;
			}
		if (!ok) return;
		if (!in.integer(flags, 0, 255)) {
			r.error("light's flags are a byte");
			return;
		}
		if (in.more()) {
			double spot[4];
			if (!in.numbers(spot, 4)) {
				r.error("a spot light needs dx dy dz falloff");
				return;
			}
			for (int k = 0; k < 3; ++k) l.direction[k] = spot[k];
			l.falloff = spot[3];
		}
		l.style = static_cast<uint8_t>(style);
		// A register-driven light's phase field is its register's index.
		if (threedi_generator_names_register(l.style)) {
			if (l.phase != std::floor(l.phase) || !(std::fabs(l.phase) < 1e6)) {
				r.error("a register-driven light's phase field is its register index");
				return;
			}
			if (!r.declared(true, static_cast<long long>(l.phase), "light")) return;
		}
		for (int k = 0; k < 3; ++k) {
			l.start_rgb[k] = static_cast<int>(c[k]);
			l.end_rgb[k] = static_cast<int>(c[k + 3]);
		}
		l.flags = static_cast<uint8_t>(flags);
		model.lights.push_back(l);
	} else if (key == "occ") {
		ThreediO3dOcclusion o;
		o.line = r.line;
		long long type = 0;
		if (!in.integer(type, 0, 255) || !in.integer(o.section) || o.section < 0 || !in.integer(o.connecting) ||
				o.connecting < 0) {
			r.error("occ needs type (a byte) section connecting");
			return;
		}
		// The record's sphere as stored, when it is not the one build
		// derives from the vertices (206 retail models mirror its centre).
		o.sphere_given = in.more();
		double s[4] = {0.0, 0.0, 0.0, 0.0};
		if (o.sphere_given && !in.numbers(s, 4)) {
			r.error("occ's sphere is cx cy cz r");
			return;
		}
		o.type = static_cast<uint8_t>(type);
		o.sphere = ThreediBuildOccSphere{ThreediBuildVec3{s[0], s[1], s[2]}, s[3]};
		model.occlusion.push_back(o);
		r.occ_open = true;
	} else if (key == "cxlt") {
		// A CXLT row (mission axes, the frame of the section offsets), in
		// order; a bare `cxlt` declares the table empty. Any cxlt record
		// replaces the rows build would derive from the sections.
		model.translations_given = true;
		if (!in.more()) return;
		double p[3];
		if (!in.numbers(p, 3)) {
			r.error("cxlt needs x y z (or nothing: an empty table)");
			return;
		}
		model.translations.push_back(ThreediO3dTranslation{r.line, ThreediBuildVec3{p[0], p[1], p[2]}});
	} else {
		r.error("unknown record '" + key + "'");
		return;
	}
	if (in.more()) r.error("unexpected '" + in.peek() + "' after the record");
}

// What the whole text must hold that no single record sees: every reference
// to a part resolved against its LOD's final part list.
void finish(Reader &r) {
	const ThreediO3dModel &model = r.model;
	if (model.name.line == 0) r.model_error("no 'model' record");
	if (model.lods.empty()) r.model_error("no 'lod' record");
	for (size_t li = 0; li < model.lods.size(); ++li) {
		// A LOD with no parts is legal: retail ships them (Dblkhwk1's LOD 4).
		const ThreediO3dLod &lod = model.lods[li];
		const long long parts = static_cast<long long>(lod.parts.size());
		const std::string held = "lod " + std::to_string(li) + " holds " + std::to_string(parts) + " parts";
		for (const ThreediO3dPart &p : lod.parts) {
			if (p.parent >= parts) r.error_at(p.line, "part's parent " + std::to_string(p.parent) + " is no part: " + held);
			if (p.has_center && !p.meshes.empty())
				r.error_at(p.line, "a part that draws takes its sphere from its vertices (no centre)");
			for (const ThreediO3dMesh &m : p.meshes) {
				int first = 0;
				long long named = 0;
				size_t count = 0;
				for (const ThreediO3dVertex &v : m.vertices)
					for (uint32_t k = 0; k < v.influence_count; ++k)
						if (m.influences[v.first_influence + k].part >= parts) {
							if (count++ == 0) {
								first = v.line;
								named = m.influences[v.first_influence + k].part;
							}
							break;
						}
				if (count > 0)
					r.error_at(first, std::to_string(count) + " vertices of the mesh at line " + std::to_string(m.line) +
							" name parts their LOD lacks (this one part " + std::to_string(named) + "): " + held +
							", the skeleton the influences name");
			}
		}
		for (const ThreediO3dPanm &pa : lod.panm) {
			if (pa.part >= parts) r.error_at(pa.line, "panm names part " + std::to_string(pa.part) + ": " + held);
			// The selector byte sign-extends; 1..127 name an mtrx frame
			// (threedi_panm_frame_row).
			if (static_cast<int8_t>(pa.matrix) > static_cast<int>(model.frames.size()))
				r.error_at(pa.line, "panm selects mtrx frame " + std::to_string(pa.matrix) + ": the model declares " +
						std::to_string(model.frames.size()));
		}
	}
	for (const ThreediO3dLight &l : model.lights)
		if (l.part != 0 && (model.lods.empty() || l.part >= static_cast<long long>(model.lods[0].parts.size())))
			r.error_at(l.line, "light names part " + std::to_string(l.part) + ", which LOD 0 lacks");
}

} // namespace

bool threedi_o3d_read(std::istream &text, ThreediO3dModel &model, std::vector<SceneFinding> &findings) {
	model = ThreediO3dModel{};
	Reader r{model, findings};
	const size_t before = findings.size();
	bool header = false;
	std::string raw;
	while (std::getline(text, raw)) {
		++r.line;
		// A UTF-8 byte order mark ahead of the header is no field.
		if (r.line == 1 && raw.compare(0, 3, "\xEF\xBB\xBF") == 0) raw.erase(0, 3);
		SceneLine in(strip_comment(raw), SceneNumbers::any);
		if (!in.bad.empty()) {
			r.error(in.bad);
			continue;
		}
		if (in.tokens.empty()) continue;
		const std::string key = in.key();
		if (!header) {
			long long version = 0;
			if (key != "o3d" || !in.integer(version) || version != kThreediO3dVersion || in.more()) {
				r.error(key == "o3d" && version == 1
								? "this is version 1 of the scene text (strips, bone tables, four bone slots): export or "
								  "write it again as 'o3d 2'"
								: "expected the header line 'o3d 2'");
				return false;
			}
			header = true;
			continue;
		}
		if (key == "texfile") continue; // import metadata (`scene` writes it); nothing to build
		// Each record closes the containers it is not a record of.
		const bool mesh_record = key == "v" || key == "vt" || key == "t";
		const bool occ_record = key == "ov" || key == "op" || key == "of";
		const bool volume_mesh_record = key == "vv" || key == "vf";
		if (!mesh_record) r.close_mesh();
		if (!occ_record) r.close_occ();
		if (!volume_mesh_record) r.close_mesh_volume();
		if (key != "cp") r.close_plane_volume();
		if (mesh_record) {
			if (r.mesh < 0) r.error("'" + key + "' outside a mesh");
			else read_mesh_record(r, key, in);
		} else if (occ_record) {
			if (!r.occ_open) r.error("'" + key + "' outside an occ record");
			else read_occ_record(r, key, in);
		} else if (volume_mesh_record) {
			if (r.mesh_volume < 0) r.error("'" + key + "' outside a cvmesh volume");
			else read_volume_mesh_record(r, key, in);
		} else if (!read_material_record(r, key, in) && !read_collision_record(r, key, in) &&
				!read_lod_record(r, key, in)) {
			read_model_record(r, key, in);
		}
	}
	r.close_mesh();
	r.close_occ();
	r.close_mesh_volume();
	r.close_plane_volume();
	if (!header) r.model_error("empty scene");
	bool clean = true;
	for (size_t i = before; i < findings.size(); ++i) clean = clean && !findings[i].error;
	if (clean) finish(r);
	for (size_t i = before; i < findings.size(); ++i) clean = clean && !findings[i].error;
	return clean;
}

} // namespace opennova::threedi
