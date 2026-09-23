// opennova-3di — build a .3di from an authored scene, or inspect one.
//
//   opennova-3di build <scene.o3d> -o <out.3di>
//   opennova-3di info  <model.3di> [--verbose]
//
// `build` reads the .o3d scene text a DCC exporter writes (the Blender add-on
// under tools/blender/opennova_3di is the first one; the grammar is in
// docs/threedi/o3d-scene-format.md) into the engine's construction API
// (formats/threedi/threedi_build.h) and serializes it through the parity
// writer, so a shipped model is produced by the same writer every fixture is
// (ADR 0003). The .o3d carries geometry in MISSION axes (x forward, y left,
// z up); the model-axis conversion is threedi_build's, never the exporter's.
// `info` prints what a .3di holds: the facts an author needs to match a
// retail model (LODs, parts, shaders, textures, user points, registers,
// part animations, collision). Exit codes: 0 ok, 1 build/parse error, 2 usage.

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

using namespace opennova::threedi;

namespace {

int usage(const char *why) {
	if (why != nullptr) std::fprintf(stderr, "opennova-3di: %s\n", why);
	std::fprintf(stderr,
			"usage: opennova-3di build <scene.o3d> -o <out.3di>\n"
			"       opennova-3di info  <model.3di> [--verbose]\n");
	return 2;
}

// --- info ------------------------------------------------------------------

const char *track_label(int t) {
	static const char *const kNames[] = {"rotx", "roty", "rotz", "scalex", "scaley", "scalez", "trans"};
	return kNames[t];
}

void print_track(const Threedi3di3 &m, int t, const ThreediTransform &tr) {
	if (tr.control == 0 && tr.rate == 0 && tr.start == 0 && tr.end == 0) return;
	const char *style = threedi_panm_control_name(tr.control);
	std::string reg;
	if (threedi_panm_control_uses_register(tr.control) && tr.control_param < m.ctrl.count)
		reg = m.ctrl.registers[tr.control_param].name;
	std::printf("        %-6s style %3u (%s) param %u%s%s rate %d start %d end %d\n", track_label(t),
			tr.control, style != nullptr ? style : "?", tr.control_param, reg.empty() ? "" : " reg ",
			reg.c_str(), tr.rate, tr.start, tr.end);
}

int cmd_info(const char *path, int verbose) {
	Threedi3di3 m{};
	if (threedi_3di3_read(path, &m) != 0) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", path);
		return 1;
	}
	std::printf("model %s  mesh_type %d  lods %zu  max_radius %.3f\n", m.header.name, m.header.mesh_type,
			m.lod_count, m.header.max_radius_fp16 / 65536.0);
	for (size_t li = 0; li < m.lod_count; ++li) {
		const ThreediLod &lod = m.lods[li];
		size_t tris = 0;
		for (size_t s = 0; s < lod.strip_count; ++s) tris += lod.strips[s].num_triangles;
		// Winding: the share of list triangles whose model-axis cross(e1, e2)
		// agrees with the authored vertex normals (retail models sit near 100%).
		size_t agree = 0, sampled = 0;
		for (size_t s = 0; s < lod.strip_count; ++s) {
			const ThreediTriangleStrip &st = lod.strips[s];
			if (st.is_strip) continue;
			for (int t = 0; t + 2 < st.num_indices; t += 3) {
				const ThreediVertex *v[3];
				bool ok = true;
				for (int k = 0; k < 3; ++k) {
					const size_t ii = static_cast<size_t>(st.index_offset) + t + k;
					const size_t vi = static_cast<size_t>(st.start_vertex) + (ii < lod.indices.count ? lod.indices.indices[ii] : 0);
					ok = ok && ii < lod.indices.count && vi < lod.vertices.count;
					v[k] = ok ? &lod.vertices.items[vi] : nullptr;
				}
				if (!ok) continue;
				float e1[3], e2[3], n[3] = {0, 0, 0};
				for (int k = 0; k < 3; ++k) {
					e1[k] = v[1]->position[k] - v[0]->position[k];
					e2[k] = v[2]->position[k] - v[0]->position[k];
					n[k] = v[0]->normal[k] + v[1]->normal[k] + v[2]->normal[k];
				}
				const float c[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
				const float d = c[0] * n[0] + c[1] * n[1] + c[2] * n[2];
				if (d == 0.0f) continue;
				++sampled;
				if (d > 0.0f) ++agree;
			}
		}
		std::printf("lod %zu  type %s  threshold %d  parts %zu  strips %zu  verts %u  tris %zu  vflags 0x%x  panm %zu"
				"  ccw-vs-normal %.1f%%\n",
				li, lod.model_type, lod.lod_threshold, lod.render_object_count, lod.strip_count, lod.vertices.count,
				tris, lod.vertices.flags, lod.part_animation_count, sampled ? 100.0 * agree / sampled : 0.0);
		if (!verbose && li > 0) continue;
		for (size_t p = 0; p < lod.render_object_count; ++p) {
			const ThreediRenderObject &ro = lod.render_objects[p];
			std::printf("    part %2zu parent %2d  strips %d+%d  abs(model) %.3f %.3f %.3f  radius %.3f\n", p,
					ro.parent_index, ro.num_strips, ro.num_alpha_strips, ro.abs[0], ro.abs[1], ro.abs[2],
					ro.bounding_radius);
		}
		for (size_t a = 0; a < lod.part_animation_count; ++a) {
			const ThreediPartAnimation &pa = lod.part_animations[a];
			std::printf("    panm part %u parent %u flags 0x%08x\n", pa.subobject_index, pa.parent_subobject, pa.flags);
			const ThreediTransform *tracks[] = {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x,
					&pa.scale_y, &pa.scale_z, &pa.translation};
			for (int t = 0; t < 7; ++t) print_track(m, t, *tracks[t]);
		}
	}
	for (uint32_t i = 0; i < m.material_count; ++i) {
		const ThreediMaterial &mt = m.materials[i];
		std::printf("material %u  shader %s  flags 0x%02x  alpha_test %u  glass %u  emissive %u", i, mt.shader_name,
				mt.material_flags, mt.alpha_test_value_byte, mt.is_glass, mt.emissive_type);
		for (uint32_t t = 0; t < mt.texture_count && t < 24; ++t)
			std::printf("  [%s slot %u type %u flags %u]", mt.textures[t].name, mt.textures[t].slot,
					mt.textures[t].type, mt.textures[t].flags);
		std::printf("\n");
		if (mt.rgb_gen.style != 0)
			std::printf("    rgbgen style %u reg %d rate %.3f\n", mt.rgb_gen.style, mt.rgb_gen.reg, mt.rgb_gen.rate);
		if (mt.alpha_gen.style != 0)
			std::printf("    alphagen style %u reg %d rate %.3f start %d end %d\n", mt.alpha_gen.style,
					mt.alpha_gen.reg, mt.alpha_gen.rate, mt.alpha_gen.start, mt.alpha_gen.end);
		if (mt.u_params.style != 0 || mt.v_params.style != 0)
			std::printf("    uvgen u %u v %u\n", mt.u_params.style, mt.v_params.style);
	}
	for (uint32_t i = 0; i < m.ctrl.count; ++i) std::printf("register %u  %s\n", i, m.ctrl.registers[i].name);
	for (size_t i = 0; i < m.user_point_count; ++i) {
		const ThreediUserPoint &u = m.user_points[i];
		std::printf("userpoint %-15s  type %d  part %d  pos(mission) %.3f %.3f %.3f  dir %.3f %.3f %.3f\n", u.name,
				u.userpoint_type, u.subobject_index, u.x / 65536.0, u.y / 65536.0, u.z / 65536.0, u.rot_x / 65536.0,
				u.rot_y / 65536.0, u.rot_z / 65536.0);
	}
	for (size_t i = 0; i < m.light_count; ++i) {
		const ThreediLight &l = m.lights[i];
		std::printf("light %zu  style %u  part %u  atten %.2f..%.2f\n", i, l.style, l.subobj_index, l.atten_start,
				l.atten_end);
	}
	if (m.collision != nullptr) {
		const ThreediCollisionModel &c = *m.collision;
		const float *b = c.model_data.bbox;
		std::printf("collision  objects %zu  verts %zu  faces %zu  volumes %zu  bbox(mission) %.2f %.2f %.2f .. %.2f %.2f %.2f\n",
				c.object_count, c.vertex_count, c.face_count, c.volume_count, b[0], b[1], b[2], b[3], b[4], b[5]);
		for (size_t o = 0; o < c.object_count; ++o) {
			const ThreediCollisionObject &co = c.objects[o];
			std::printf("    cobj %zu parent %d  verts %d faces %d volumes %d\n", o, co.parent_subobject_index,
					co.num_vertices, co.num_faces, co.num_bounding_volumes);
		}
		if (verbose) {
			size_t v = 0, f = 0, p = 0;
			for (size_t o = 0; o < c.object_count; ++o) {
				const ThreediCollisionObject &co = c.objects[o];
				for (int k = 0; k < co.num_bounding_volumes && v < c.volume_count; ++k, ++v) {
					const ThreediBoundingVolume &bv = c.volumes[v];
					std::printf("    cobj %zu volume %zu type %d flags 0x%x planes %2d  box %.2f %.2f %.2f .. %.2f %.2f %.2f\n", o, v,
							bv.collidable_type, bv.flags, bv.plane_count, bv.min_x_fp16 / 65536.0, bv.min_y_fp16 / 65536.0,
							bv.min_z_fp16 / 65536.0, bv.max_x_fp16 / 65536.0, bv.max_y_fp16 / 65536.0, bv.max_z_fp16 / 65536.0);
					for (int q = 0; q < bv.plane_count && p < c.plane_count; ++q, ++p)
						if (verbose > 1)
							std::printf("        plane flags %d  n %.3f %.3f %.3f  d %.3f\n", c.planes[p].flags,
									c.planes[p].normal[0], c.planes[p].normal[1], c.planes[p].normal[2], c.planes[p].radius);
				}
				std::map<int, int> poly;
				for (int k = 0; k < co.num_faces && f < c.face_count; ++k, ++f) ++poly[c.faces[f].poly_type];
				for (const auto &kv : poly) std::printf("    cobj %zu faces poly_type %d x%d\n", o, kv.first, kv.second);
			}
		}
	}
	threedi_3di3_free(&m);
	return 0;
}

// --- build -----------------------------------------------------------------

struct Parser {
	std::string path;
	int line = 0;
	std::vector<std::string> errors;

	void error(const std::string &what) {
		errors.push_back(path + ":" + std::to_string(line) + ": " + what);
	}
};

bool read_doubles(std::istringstream &in, double *out, int n) {
	for (int i = 0; i < n; ++i)
		if (!(in >> out[i])) return false;
	return true;
}

int track_index(const std::string &name) {
	static const char *const kNames[] = {"rotx", "roty", "rotz", "scalex", "scaley", "scalez", "trans"};
	for (int i = 0; i < 7; ++i)
		if (name == kNames[i]) return i;
	return -1;
}

// The PANM flags word the tracks imply: a rotation type 2 when any rotation
// track animates, scale type 2 for any scale track, and the translate axis.
uint32_t panm_flags_for(const ThreediPartAnimation &pa, int trans_axis) {
	const auto live = [](const ThreediTransform &t) { return t.control != 0; };
	const bool rot = live(pa.rotation_x) || live(pa.rotation_y) || live(pa.rotation_z);
	const bool scale = live(pa.scale_x) || live(pa.scale_y) || live(pa.scale_z);
	return threedi_panm_pack_flags(scale ? 2 : 0, rot ? 2 : 0, 0,
			live(pa.translation) ? static_cast<uint8_t>(trans_axis) : 0);
}

ThreediVertex render_vertex(const double *p, const double *n, const double *uv) {
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
	v.uv1[0] = static_cast<float>(uv[0]);
	v.uv1[1] = static_cast<float>(uv[1]);
	return v;
}

bool parse_scene(Parser &ps, std::istream &file, ThreediBuildModel &model) {
	std::string raw;
	int lod = -1, part = -1, material = -1, cobj = -1, volume_open = -1;
	ThreediBuildStrip *strip = nullptr;
	int strip_lod = -1, strip_part = -1;
	ThreediBuildStrip pending;
	bool have_pending = false;
	std::map<std::pair<int, int>, int> trans_axis; // (lod, panm row) -> axis
	const auto flush_strip = [&]() {
		if (have_pending) {
			model.lods[strip_lod].parts[strip_part].strips.push_back(std::move(pending));
			pending = ThreediBuildStrip{};
			have_pending = false;
		}
		strip = nullptr;
	};
	bool header = false;
	while (std::getline(file, raw)) {
		++ps.line;
		const size_t hash = raw.find('#');
		if (hash != std::string::npos) raw.erase(hash);
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
		if (key == "v" || key == "t") {
			if (strip == nullptr) {
				ps.error("'" + key + "' outside a strip");
				continue;
			}
			if (key == "v") {
				double p[3], n[3], uv[2];
				if (!read_doubles(in, p, 3) || !read_doubles(in, n, 3) || !read_doubles(in, uv, 2)) {
					ps.error("v needs px py pz nx ny nz u v");
					continue;
				}
				if (strip->vertices.size() >= 65535) {
					ps.error("strip exceeds 65535 vertices (u16 indices)");
					continue;
				}
				strip->vertices.push_back(render_vertex(p, n, uv));
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
			long a, b, c;
			int poly = 1;
			unsigned long flags = 0;
			if (cobj < 0 || !(in >> a >> b >> c)) {
				ps.error("cf needs an open cobj and three vertex indices");
				continue;
			}
			in >> poly >> flags;
			const long count = static_cast<long>(model.collision[cobj].vertices.size());
			if (a < 0 || b < 0 || c < 0 || a >= count || b >= count || c >= count) {
				ps.error("cf references a collision vertex not yet declared in this cobj");
				continue;
			}
			// Counter-clockwise from outside in the scene; add_face takes the
			// retail clockwise order (threedi_build add_face_box).
			model.add_face(cobj, static_cast<uint16_t>(a), static_cast<uint16_t>(c), static_cast<uint16_t>(b),
					static_cast<uint8_t>(poly), static_cast<uint32_t>(flags));
			continue;
		}
		flush_strip();
		if (key == "model") {
			std::string name;
			if (!(in >> name) || name.size() > 15) ps.error("model needs a name of at most 15 characters");
			model.name = name;
		} else if (key == "tangents") {
			int on = 0;
			in >> on;
			model.tangents = on != 0;
		} else if (key == "register") {
			std::string name;
			if (!(in >> name) || name.size() > 23) {
				ps.error("register needs a name of at most 23 characters");
				continue;
			}
			if (threedi_ctrl_register_ordinal(name.c_str()) == THREEDI_CTRL_REGISTER_NOT_FOUND)
				ps.error("unknown CTRL register '" + name + "' (see threedi_ctrl_catalog.h)");
			model.add_control_register(name.c_str());
		} else if (key == "material") {
			std::string shader;
			if (!(in >> shader) || shader.size() > 32) {
				ps.error("material needs a shader tag");
				continue;
			}
			material = model.add_material(shader.c_str(), nullptr);
		} else if (key == "texture") {
			std::string name;
			int slot = THREEDI_TEX_SLOT_DIFFUSE, type = THREEDI_TEX_TYPE_DIFFUSE, flags = 0;
			if (material < 0 || !(in >> name)) {
				ps.error("texture needs an open material and a file name");
				continue;
			}
			in >> slot >> type >> flags;
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
			double rate = 0;
			if (material < 0 || !(in >> style >> reg >> rate >> s[0] >> s[1] >> s[2] >> e[0] >> e[1] >> e[2])) {
				ps.error("rgbgen needs style reg rate r g b r g b");
				continue;
			}
			model.set_rgb_gen(material, static_cast<uint8_t>(style), reg, rate, s, e);
		} else if (key == "alphagen" || key == "ugen" || key == "vgen") {
			int style = 0, reg = -1;
			double rate = 0, start = 0, end = 0, phase = 0;
			if (material < 0 || !(in >> style >> reg >> rate >> start >> end)) {
				ps.error(key + " needs style reg rate start end [phase]");
				continue;
			}
			in >> phase;
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
			const int index = static_cast<int>(model.lods[lod].parts.size());
			if (parent < 0 || parent > index) {
				ps.error("part parent must be an earlier part (or itself for the root)");
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
			int p = 0, parent = 0;
			if (lod < 0 || !(in >> p >> parent)) {
				ps.error("panm needs an open lod, a part and its parent");
				continue;
			}
			model.add_panm(lod, p, parent);
		} else if (key == "track") {
			std::string target, reg;
			int style = 0, rate = 0, start = 0, end = 0;
			if (lod < 0 || model.lods[lod].panm.empty() || !(in >> target >> style >> reg >> rate >> start >> end)) {
				ps.error("track needs an open panm and target style register|- rate start end [axis]");
				continue;
			}
			const int t = track_index(target);
			if (t < 0) {
				ps.error("unknown track target '" + target + "'");
				continue;
			}
			int param = 0;
			if (reg != "-") {
				param = -1;
				for (size_t r = 0; r < model.control_registers.size(); ++r)
					if (model.control_registers[r] == reg) param = static_cast<int>(r);
				if (param < 0) {
					ps.error("track register '" + reg + "' is not declared with 'register'");
					continue;
				}
			}
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
			pa.flags = panm_flags_for(pa, trans_axis.count({lod, row}) ? trans_axis[{lod, row}] : 0);
		} else if (key == "userpoint") {
			std::string name;
			double p[3], d[3];
			int sub = 0, type = THREEDI_USER_POINT_GAMEPLAY;
			if (!(in >> name) || !read_doubles(in, p, 3) || !read_doubles(in, d, 3) || !(in >> sub)) {
				ps.error("userpoint needs name x y z dx dy dz part [type]");
				continue;
			}
			in >> type;
			if (name.size() > 15) ps.error("user point name '" + name + "' exceeds 15 characters");
			model.add_user_point(name.c_str(), ThreediBuildVec3{p[0], p[1], p[2]}, ThreediBuildVec3{d[0], d[1], d[2]},
					sub, type);
		} else if (key == "cobj") {
			int parent = 0;
			double o[3] = {0, 0, 0};
			if (!(in >> parent)) {
				ps.error("cobj needs a parent part");
				continue;
			}
			read_doubles(in, o, 3);
			cobj = model.add_cobj(parent, ThreediBuildVec3{o[0], o[1], o[2]});
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
	if (!header) ps.error("empty scene");
	return ps.errors.empty();
}

// Whole-model checks retail imposes that no single record can see.
void validate(Parser &ps, const ThreediBuildModel &m) {
	ps.line = 0;
	if (m.name.empty()) ps.error("no 'model' record");
	if (m.lods.empty()) ps.error("no 'lod' record");
	for (size_t li = 0; li < m.lods.size(); ++li) {
		const ThreediBuildLod &lod = m.lods[li];
		if (lod.parts.empty()) ps.error("lod " + std::to_string(li) + " has no parts");
		if (lod.parts.size() > 255) ps.error("lod " + std::to_string(li) + " has more than 255 parts");
		size_t verts = 0, indices = 0;
		for (const ThreediBuildPart &p : lod.parts)
			for (const ThreediBuildStrip &s : p.strips) {
				verts += s.vertices.size();
				indices += s.indices.size();
				if (s.indices.size() > 65535) ps.error("a strip exceeds 65535 indices (u16 STRP count)");
			}
		for (const ThreediPartAnimation &pa : lod.panm)
			if (pa.subobject_index >= lod.parts.size())
				ps.error("panm in lod " + std::to_string(li) + " names a missing part");
	}
	for (size_t o = 0; o < m.collision.size(); ++o)
		for (const ThreediBoundingVolume &v : m.collision[o].volumes)
			if (v.plane_count < 4) ps.error("cobj " + std::to_string(o) + " has a volume with fewer than 4 planes");
	int seats = 0;
	for (const ThreediUserPoint &u : m.user_points)
		if (strncmp(u.name, "sitex", 5) == 0 || strncmp(u.name, "SITEX", 5) == 0) ++seats;
	if (seats > 8) ps.error("more than 8 sitex seats (retail's scan stops at 8)");
	if (m.user_points.size() > 16)
		std::fprintf(stderr, "opennova-3di: note: %zu user points; the item-effect attach scan only reads the first 16\n",
				m.user_points.size());
}

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

} // namespace

int main(int argc, char **argv) {
	if (argc < 3) return usage(nullptr);
	const std::string cmd = argv[1];
	if (cmd == "info") {
		const int verbose = argc > 3 && std::strcmp(argv[3], "--planes") == 0 ? 2 : (argc > 3 && std::strcmp(argv[3], "--verbose") == 0 ? 1 : 0);
		return cmd_info(argv[2], verbose);
	}
	if (cmd == "build") {
		if (argc != 5 || std::strcmp(argv[3], "-o") != 0) return usage("build needs <scene.o3d> -o <out.3di>");
		return cmd_build(argv[2], argv[4]);
	}
	return usage("unknown command");
}
