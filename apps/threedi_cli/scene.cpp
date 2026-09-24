// opennova-3di scene: write a .3di back out as .o3d scene text, the inverse
// of `build`, so a DCC importer (the Blender add-on's importer.py) lays a
// model out by the same naming contract the exporter reads and the model
// exports again. Only records `build` reads are written, in the order it
// requires; every conversion is the exact inverse of build's (model ->
// mission axes, the render and collision winding flips), so
// `build(scene(x))` re-mints a builder-made model byte for byte. What the
// scene text cannot carry is listed as `#` comments and on stderr.
//
// `texfile <name> <path|->` records name the file each texture reference
// resolves to beside the model, by the runtime's own candidate order
// (base/resource_index/texture_candidates.h); `build` ignores them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <vector>

#include <base/resource_index/texture_candidates.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_strip_decode.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace threedi_cli {

namespace {

// Floats print with 9 significant digits (a float32 round-trips exactly);
// values that come from fixed-point words print with 17 (exact re-quantization).
std::string f9(double v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.9g", v);
	return std::strcmp(buf, "-0") == 0 ? std::string("0") : std::string(buf);
}

std::string f17(double v) {
	char buf[40];
	std::snprintf(buf, sizeof(buf), "%.17g", v);
	return std::strcmp(buf, "-0") == 0 ? std::string("0") : std::string(buf);
}

std::string vec9(const float *model) {
	const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{model[0], model[1], model[2]});
	return f9(m.x) + " " + f9(m.y) + " " + f9(m.z);
}

int byte_of(float unit) { return static_cast<int>(std::lround(unit * 255.0f)); }

// A name field: bare when it is one plain token, else "quoted" (build reads
// both). A name that holds '"' itself cannot be written and is reported.
std::string name_field(const std::string &name) {
	if (!name.empty() && name.find_first_of(" \t\"") == std::string::npos && name[0] != '#') return name;
	return "\"" + name + "\"";
}

struct Writer {
	FILE *f = nullptr;
	std::vector<std::string> notes;

	void line(const std::string &s) { std::fprintf(f, "%s\n", s.c_str()); }
	void note(const std::string &s) {
		notes.push_back(s);
		line("# dropped: " + s);
	}
};

// Case-insensitive lookup of a texture's candidate names in `dir`.
std::string resolve_texture(const std::filesystem::path &dir, const std::string &name) {
	std::error_code ec;
	std::map<std::string, std::string> listing;
	for (const auto &entry : std::filesystem::directory_iterator(dir, ec)) {
		if (!entry.is_regular_file(ec)) continue;
		std::string file = entry.path().filename().string();
		std::string key = file;
		std::transform(key.begin(), key.end(), key.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		listing.emplace(key, entry.path().string());
	}
	for (std::string candidate : opennova::texture_candidate_filenames(name)) {
		std::transform(candidate.begin(), candidate.end(), candidate.begin(),
				[](unsigned char c) { return static_cast<char>(std::tolower(c)); });
		const auto it = listing.find(candidate);
		if (it != listing.end()) return it->second;
	}
	return std::string();
}

void write_materials(Writer &w, const Threedi3di3 &m, const std::filesystem::path &dir) {
	std::set<std::string> resolved;
	for (uint32_t i = 0; i < m.material_count; ++i) {
		const ThreediMaterial &mt = m.materials[i];
		w.line("material " + name_field(mt.shader_name[0] != '\0' ? mt.shader_name : "FF_ST_OP") + "  # " +
				std::to_string(i));
		if (mt.shader_name[0] == '\0') w.note("material " + std::to_string(i) + " has no shader tag");
		for (uint32_t t = 0; t < mt.texture_count && t < 24; ++t) {
			const ThreediMaterialTexture &tx = mt.textures[t];
			w.line("texture " + name_field(tx.name) + " " + std::to_string(tx.slot) + " " + std::to_string(tx.type) + " " +
					std::to_string(tx.flags) + " " + std::to_string(tx.frame));
			if (resolved.insert(tx.name).second) {
				const std::string path = resolve_texture(dir, tx.name);
				w.line("texfile " + name_field(tx.name) + " " + (path.empty() ? "-" : path));
			}
		}
		const ThreediTexAnim &a = mt.animation;
		if (a.num_frames != 0 || a.animation_type != 0 || a.cycle_frame_time != 0)
			w.line("texanim " + std::to_string(a.num_frames) + " " + std::to_string(a.animation_type) + " " +
					std::to_string(a.cycle_frame_time));
		const float *rc = mt.reflect_color;
		if (rc[0] != 0.0f || rc[1] != 0.0f || rc[2] != 0.0f || rc[3] != 0.0f)
			w.line("reflect " + std::to_string(byte_of(rc[0])) + " " + std::to_string(byte_of(rc[1])) + " " +
					std::to_string(byte_of(rc[2])) + " " + std::to_string(byte_of(rc[3])));
		if (mt.material_flags != 0) w.line("matflags " + std::to_string(mt.material_flags));
		if (mt.alpha_test_value_byte != 0) w.line("alphatest " + std::to_string(mt.alpha_test_value_byte));
		if (mt.is_glass != 0) w.line("glass " + std::to_string(mt.is_glass));
		if (mt.emissive_type != 0) w.line("emissive " + std::to_string(mt.emissive_type));
		const ThreediRgbGen &g = mt.rgb_gen;
		if (g.style != 0) {
			std::string s = "rgbgen " + std::to_string(g.style) + " " + std::to_string(g.style > 112 ? g.reg : -1) + " " +
					f9(g.rate);
			for (int k = 0; k < 3; ++k) s += " " + std::to_string(byte_of(g.start_color[k]));
			for (int k = 0; k < 3; ++k) s += " " + std::to_string(byte_of(g.end_color[k]));
			w.line(s + " " + f9(g.phase));
			if (g.start_color[3] != 0.0f || g.end_color[3] != 0.0f)
				w.note("material " + std::to_string(i) + " rgbgen alpha bytes");
		}
		const ThreediAlphaGen &ag = mt.alpha_gen;
		if (ag.style != 0)
			w.line("alphagen " + std::to_string(ag.style) + " " + std::to_string(ag.style > 112 ? ag.reg : -1) + " " +
					f9(ag.rate) + " " + std::to_string(ag.start) + " " + std::to_string(ag.end) + " " + f9(ag.phase));
		const ThreediUvParams *uv[2] = {&mt.u_params, &mt.v_params};
		for (int k = 0; k < 2; ++k)
			if (uv[k]->style != 0)
				w.line(std::string(k == 0 ? "ugen " : "vgen ") + std::to_string(uv[k]->style) + " " +
						std::to_string(uv[k]->style > 112 ? uv[k]->reg : -1) + " " + f9(uv[k]->gen_rate) + " " +
						f9(uv[k]->start) + " " + f9(uv[k]->end) + " " + f9(uv[k]->phase));
		if (mt.rgb_gen2.style != 0 || mt.emissive_type2 != 0 || mt.glass_type2 != 0 || mt.reflect_color2[0] != 0.0f ||
				mt.reflect_color2[1] != 0.0f || mt.reflect_color2[2] != 0.0f || mt.reflect_color2[3] != 0.0f)
			w.note("material " + std::to_string(i) + " second-channel fields (rgb_gen2, emissive2, glass2, reflect2)");
	}
}

// A skinned LOD's mesh parts: the parts no strip's bone table references
// that carry bounds of their own (FSldr03 part 19, ArmsG part 37).
std::vector<int> skinned_mesh_parts(const ThreediLod &lod) {
	std::set<int> bones;
	for (size_t s = 0; s < lod.strip_count; ++s)
		for (int b = 0; b < lod.strips[s].bone_table_length && b < 16; ++b) bones.insert(lod.strips[s].bone_table[b]);
	std::vector<int> mesh;
	for (size_t p = 0; p < lod.render_object_count; ++p)
		if (!bones.count(static_cast<int>(p)) && lod.render_objects[p].bounding_radius > 0.0f)
			mesh.push_back(static_cast<int>(p));
	return mesh;
}

void write_strip(Writer &w, const Threedi3di3 &m, const ThreediLod &lod, const ThreediTriangleStrip &st, bool alpha,
		bool uv1, bool skinned) {
	const int material = threedi_material_array_index_for_id(m, st.material_index);
	w.line("strip " + std::to_string(material < 0 ? 0 : material) + " " + (alpha ? "1" : "0"));
	if (material < 0) w.note("a strip names material id " + std::to_string(st.material_index) + " the model lacks");
	if (skinned) {
		std::string s = "bones";
		for (int b = 0; b < st.bone_table_length && b < 16; ++b) s += " " + std::to_string(st.bone_table[b]);
		w.line(s);
	}
	for (int i = 0; i < st.num_vertices; ++i) {
		const ThreediVertex &v = lod.vertices.items[st.start_vertex + i];
		std::string s = "v " + vec9(v.position) + " " + vec9(v.normal) + " " + f9(v.uv0[0]) + " " + f9(v.uv0[1]);
		if (uv1) s += " " + f9(v.uv1[0]) + " " + f9(v.uv1[1]);
		if (skinned) {
			for (int k = 0; k < 3; ++k) s += " " + std::to_string(v.bone_indices[k]);
			for (int k = 0; k < 3; ++k) s += " " + f9(v.bone_weights[k]);
		}
		w.line(s);
	}
	std::vector<uint16_t> tris;
	if (!threedi_decode_strip_indices(lod, st, tris)) {
		w.note("a strip whose indices escape its vertex window");
		return;
	}
	// Retail winds counter-clockwise in model axes; the scene winds
	// counter-clockwise in mission axes, the mirror of model: swap the second
	// and third corners (the inverse of build's swap).
	for (size_t t = 0; t + 2 < tris.size(); t += 3)
		w.line("t " + std::to_string(tris[t]) + " " + std::to_string(tris[t + 2]) + " " + std::to_string(tris[t + 1]));
}

void write_lod(Writer &w, const Threedi3di3 &m, size_t li, bool uv1) {
	const ThreediLod &lod = m.lods[li];
	const bool skinned = m.header.mesh_type == THREEDI_MESH_SKINNED;
	w.line("lod " + std::to_string(lod.lod_threshold) + " " + (lod.model_type[0] != '\0' ? lod.model_type : "gnrc") +
			"  # lod " + std::to_string(li));
	// Which strips each part owns: the ROBJ walk (each part's opaque strips,
	// then its alpha strips, in part order).
	std::vector<std::vector<std::pair<size_t, bool>>> owned(lod.render_object_count);
	size_t cursor = 0;
	for (size_t p = 0; p < lod.render_object_count; ++p) {
		const ThreediRenderObject &ro = lod.render_objects[p];
		for (int s = 0; s < ro.num_strips + ro.num_alpha_strips && cursor < lod.strip_count; ++s, ++cursor)
			owned[p].push_back({cursor, s >= ro.num_strips});
	}
	if (cursor != lod.strip_count) w.note("lod " + std::to_string(li) + " strips no ROBJ owns");
	if (skinned) {
		// The retail skinned layout keeps every strip on the root ROBJ; the
		// scene authors them on the mesh part(s), which the builder moves back.
		std::vector<int> mesh = skinned_mesh_parts(lod);
		std::vector<std::pair<size_t, bool>> all;
		for (auto &o : owned) {
			all.insert(all.end(), o.begin(), o.end());
			o.clear();
		}
		if (mesh.empty()) {
			if (!all.empty()) w.note("lod " + std::to_string(li) + ": no mesh part (strips kept on the root)");
			owned[0] = all;
		} else {
			if (mesh.back() != static_cast<int>(lod.render_object_count) - 1 ||
					mesh.front() != static_cast<int>(lod.render_object_count - mesh.size()))
				w.note("lod " + std::to_string(li) + ": mesh parts are not the last parts");
			for (const auto &entry : all) {
				// Several mesh parts: the one whose bounding sphere holds the
				// strip's vertex centroid (the first otherwise).
				const ThreediTriangleStrip &st = lod.strips[entry.first];
				double c[3] = {0, 0, 0};
				for (int i = 0; i < st.num_vertices; ++i)
					for (int k = 0; k < 3; ++k) c[k] += lod.vertices.items[st.start_vertex + i].position[k] / st.num_vertices;
				int best = mesh.front();
				for (int p : mesh) {
					const ThreediRenderObject &ro = lod.render_objects[p];
					double d = 0;
					for (int k = 0; k < 3; ++k) d += (c[k] - ro.bounding_center[k]) * (c[k] - ro.bounding_center[k]);
					if (std::sqrt(d) <= ro.bounding_radius * 1.001 + 1e-4) {
						best = p;
						break;
					}
				}
				owned[best].push_back(entry);
			}
		}
	}
	for (size_t p = 0; p < lod.render_object_count; ++p) {
		const ThreediRenderObject &ro = lod.render_objects[p];
		w.line("part " + std::to_string(ro.parent_index) + " " + vec9(ro.abs) + "  # part " + std::to_string(p));
		for (const auto &entry : owned[p]) write_strip(w, m, lod, lod.strips[entry.first], entry.second, uv1, skinned);
	}
	for (size_t a = 0; a < lod.part_animation_count; ++a) {
		const ThreediPartAnimation &pa = lod.part_animations[a];
		const ThreediTransform *tracks[] = {&pa.rotation_x, &pa.rotation_y, &pa.rotation_z, &pa.scale_x, &pa.scale_y,
				&pa.scale_z, &pa.translation};
		// The flags word build derives from the tracks; any other is written.
		const auto live = [](const ThreediTransform &t) { return t.control != 0; };
		const uint8_t axis = threedi_panm_translate_type(pa.flags);
		const uint32_t derived = threedi_panm_pack_flags(
				live(pa.scale_x) || live(pa.scale_y) || live(pa.scale_z) ? 2 : 0,
				live(pa.rotation_x) || live(pa.rotation_y) || live(pa.rotation_z) ? 2 : 0, 0,
				live(pa.translation) ? (axis != 0 ? axis : static_cast<uint8_t>(THREEDI_TRANS_Z)) : 0);
		std::string s = "panm " + std::to_string(pa.subobject_index) + " " + std::to_string(pa.parent_subobject);
		if (pa.flags != derived || pa.matrix_index != 0) {
			char buf[16];
			std::snprintf(buf, sizeof(buf), "0x%08x", pa.flags);
			s += std::string(" ") + buf;
			if (pa.matrix_index != 0) s += " " + std::to_string(pa.matrix_index);
		}
		w.line(s);
		if (pa.matrix_offset != 0 || pa.bind_matrix_index != 0)
			w.note("panm part " + std::to_string(pa.subobject_index) + " matrix_offset/bind_matrix_index");
		for (int t = 0; t < kTrackCount; ++t) {
			const ThreediTransform &tr = *tracks[t];
			if (tr.control == 0 && tr.control_param == 0 && tr.rate == 0 && tr.start == 0 && tr.end == 0) continue;
			std::string reg = tr.control_param != 0 ? std::to_string(tr.control_param) : "-";
			if (threedi_panm_parameter_is_ctrl_reference(tr.control)) {
				if (tr.control_param < m.ctrl.count) reg = name_field(m.ctrl.registers[tr.control_param].name);
				else w.note("a track names CTRL " + std::to_string(tr.control_param) + " the model lacks");
			}
			std::string line = std::string("track ") + track_label(t) + " " + std::to_string(tr.control) + " " + reg + " " +
					std::to_string(tr.rate) + " " + std::to_string(tr.start) + " " + std::to_string(tr.end);
			if (t == 6 && axis != 0) line += " " + std::to_string(axis);
			w.line(line);
		}
	}
}

void write_collision(Writer &w, const Threedi3di3 &m) {
	if (m.collision == nullptr) return;
	const ThreediCollisionModel &c = *m.collision;
	size_t v = 0, f = 0, vol = 0, pl = 0, normals = 0;
	for (size_t o = 0; o < c.object_count; ++o) {
		const ThreediCollisionObject &co = c.objects[o];
		w.line("cobj " + std::to_string(co.parent_subobject_index) + " " + f17(co.offset[0] / 65536.0) + " " +
				f17(co.offset[1] / 65536.0) + " " + f17(co.offset[2] / 65536.0) + "  # section " + std::to_string(o));
		// A bone section of a person: no geometry, bounds that are a sphere
		// (not the empty-section sentinel, whose min lies above its max).
		if (co.num_vertices == 0 && co.num_bounding_volumes == 0 && co.min[0] <= co.max[0])
			w.line("csphere " + f17(co.med[0] / 65536.0) + " " + f17(co.med[1] / 65536.0) + " " + f17(co.med[2] / 65536.0) +
					" " + f17(co.radius / 65536.0) + " " + f17(co.min[0] / 65536.0) + " " + f17(co.min[1] / 65536.0) + " " +
					f17(co.min[2] / 65536.0) + " " + f17(co.max[0] / 65536.0) + " " + f17(co.max[1] / 65536.0) + " " +
					f17(co.max[2] / 65536.0));
		for (int k = 0; k < co.num_vertices && v < c.vertex_count; ++k, ++v) {
			const float *p = c.vertices[v].position;
			w.line("cv " + f9(p[0]) + " " + f9(p[1]) + " " + f9(p[2]));
		}
		for (int k = 0; k < co.num_faces && f < c.face_count; ++k, ++f) {
			const ThreediCollisionFace &fc = c.faces[f];
			// Retail stores faces counter-clockwise about their normal (mission
			// axes), the scene's order. Always carry the decoded normal:
			// retail derives it before quantizing vertices to 8.8, so even
			// non-collapsed triangles cannot reproduce it from stored corners.
			std::string line = "cf " + std::to_string(fc.vert_index[0]) + " " + std::to_string(fc.vert_index[1]) + " " +
					std::to_string(fc.vert_index[2]) + " " + std::to_string(fc.poly_type) + " " +
					std::to_string(fc.material_flags);
			if (fc.normal_index >= 0 && normals + fc.normal_index < c.normal_count) {
				const float *n = c.normals[normals + fc.normal_index].normal;
				line += " " + f9(n[0]) + " " + f9(n[1]) + " " + f9(n[2]);
			}
			w.line(line);
		}
		for (int k = 0; k < co.num_bounding_volumes && vol < c.volume_count; ++k, ++vol) {
			const ThreediBoundingVolume &bv = c.volumes[vol];
			w.line("cvolume " + std::to_string(bv.collidable_type) + " " + std::to_string(bv.flags) + " " +
					f17(bv.min_x_fp16 / 65536.0) + " " + f17(bv.min_y_fp16 / 65536.0) + " " + f17(bv.min_z_fp16 / 65536.0) +
					" " + f17(bv.max_x_fp16 / 65536.0) + " " + f17(bv.max_y_fp16 / 65536.0) + " " +
					f17(bv.max_z_fp16 / 65536.0));
			for (int q = 0; q < bv.plane_count && pl < c.plane_count; ++q, ++pl) {
				const ThreediBoundingPlane &p = c.planes[pl];
				w.line("cp " + f9(p.normal[0]) + " " + f9(p.normal[1]) + " " + f9(p.normal[2]) + " " + f9(p.radius) + " " +
						std::to_string(p.flags));
			}
		}
		normals += static_cast<size_t>(co.num_normals);
	}
}

void write_occlusion(Writer &w, const Threedi3di3 &m) {
	size_t v = 0, p = 0, f = 0;
	for (size_t o = 0; o < m.occlusion_object_count; ++o) {
		const ThreediOcclusionObject &ob = m.occlusion_objects[o];
		w.line("occ " + std::to_string(ob.type) + " " + std::to_string(ob.parent_subobject_index) + " " +
				std::to_string(ob.connecting_subobject) + "  # record " + std::to_string(o));
		if (ob.glow_scale != 0.0f) w.note("occ record " + std::to_string(o) + " glow_scale");
		for (int k = 0; k < ob.num_vertices && v < m.occlusion_vertex_count; ++k, ++v)
			w.line("ov " + vec9(m.occlusion_vertices[v].position));
		for (int k = 0; k < ob.num_planes && p < m.occlusion_plane_count; ++k, ++p)
			w.line("op " + vec9(m.occlusion_planes[p].normal) + " " + f9(m.occlusion_planes[p].radius));
		// Occlusion faces wind counter-clockwise about their outward normal in
		// mission axes as stored (Armry01), so no swap.
		for (int k = 0; k < ob.face_count && f < m.occlusion_face_count; ++k, ++f) {
			const uint32_t r = m.occlusion_faces[f].raw_indices;
			w.line("of " + std::to_string(r & 0xFF) + " " + std::to_string((r >> 8) & 0xFF) + " " +
					std::to_string((r >> 16) & 0xFF) + " " + std::to_string(r >> 24));
		}
	}
}

void write_lights(Writer &w, const Threedi3di3 &m) {
	for (size_t i = 0; i < m.light_count; ++i) {
		const ThreediLight &l = m.lights[i];
		const std::string phase = l.style > THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD ? std::to_string(l.phase)
																						: f9(l.phase / 256.0);
		char flags[8];
		std::snprintf(flags, sizeof(flags), "0x%02x", l.flags);
		std::string s = "light " + std::to_string(l.subobj_index) + " " + vec9(l.offset) + " " + f9(l.atten_start) + " " +
				f9(l.atten_end) + " " + std::to_string(l.style) + " " + f9(l.rate / 256.0) + " " + phase;
		for (int k = 2; k >= 0; --k) s += " " + std::to_string(l.color_start[k]);
		for (int k = 2; k >= 0; --k) s += " " + std::to_string(l.color_end[k]);
		s += std::string(" ") + flags;
		// The light's axis and cone, unless they are the omni default.
		const bool omni = l.rotation[0] == 0.0f && l.rotation[1] == -1.0f && l.rotation[2] == 0.0f &&
				l.rotation[3] == 1.0f && l.falloff_byte == 0;
		if (!omni) {
			// The cone half-angle: its whole-degree byte when the cosine agrees
			// (build re-derives both from it), else the angle the cosine holds.
			const double cosine = std::max(-1.0, std::min(1.0, static_cast<double>(l.rotation[3])));
			double falloff = std::acos(cosine) * 57.29577951308232;
			if (std::fabs(falloff - l.falloff_byte) < 1e-3) falloff = l.falloff_byte;
			s += " " + vec9(l.rotation) + " " + f9(falloff);
		}
		w.line(s);
		if (l.unknown1 != 0 || l.color_start[3] != 0 || l.color_end[3] != 0)
			w.note("light " + std::to_string(i) + " unknown/pad bytes");
	}
}

} // namespace

int cmd_scene(const char *model_path, const char *out_path) {
	Threedi3di3 m{};
	if (threedi_3di3_read(model_path, &m) != 0) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", model_path);
		return 1;
	}
	Writer w;
	w.f = std::fopen(out_path, "w");
	if (w.f == nullptr) {
		std::fprintf(stderr, "opennova-3di: cannot write %s\n", out_path);
		threedi_3di3_free(&m);
		return 1;
	}
	const std::filesystem::path dir = std::filesystem::absolute(std::filesystem::path(model_path)).parent_path();
	w.line("o3d 1");
	w.line(std::string("# scene of ") + model_path + " (opennova-3di scene)");
	w.line("model " + name_field(m.header.name[0] != '\0' ? m.header.name : "MODEL"));
	const bool skinned = m.header.mesh_type == THREEDI_MESH_SKINNED;
	if (skinned) w.line("skinned 1");
	bool tangents = false, uv1 = false;
	for (size_t li = 0; li < m.lod_count; ++li) {
		const ThreediLod &lod = m.lods[li];
		tangents = tangents || (lod.vertices.flags & THREEDI_VERTEX_FLAG_TANGENTS) == THREEDI_VERTEX_FLAG_TANGENTS;
		for (uint32_t i = 0; i < lod.vertices.count && !uv1; ++i)
			uv1 = lod.vertices.items[i].uv1[0] != lod.vertices.items[i].uv0[0] ||
					lod.vertices.items[i].uv1[1] != lod.vertices.items[i].uv0[1];
	}
	if (tangents) {
		w.line("tangents 1");
		w.note("tangent and bitangent values (the scene keeps only the vertex layout)");
	}
	if (uv1) w.line("uv1 1");
	for (uint32_t i = 0; i < m.ctrl.count; ++i) w.line("register " + name_field(m.ctrl.registers[i].name));
	// MTRX row 0 is the identity build always writes; rows 1.. are frames.
	for (uint32_t i = 1; i < m.mtrx.count; ++i) {
		const float *r = m.mtrx.matrices[i].m;
		// The model-axes frame back in mission axes: R = C M C^T.
		static const double kC[3][3] = {{0, 0, 1}, {-1, 0, 0}, {0, 1, 0}};
		bool finite = true;
		for (int k = 0; k < 16; ++k) finite = finite && std::isfinite(r[k]);
		if (!finite) {
			// Skinned models carry NaN rows no PANM row selects (US01, ArmsG).
			w.line("mtrx 1 0 0 0 1 0 0 0 1  # frame " + std::to_string(i));
			w.note("mtrx " + std::to_string(i) + " is not finite (written as the identity)");
			continue;
		}
		std::string s = "mtrx";
		for (int a = 0; a < 3; ++a)
			for (int b = 0; b < 3; ++b) {
				double sum = 0.0;
				for (int i2 = 0; i2 < 3; ++i2)
					for (int j = 0; j < 3; ++j) sum += kC[a][i2] * r[i2 * 4 + j] * kC[b][j];
				s += " " + f9(sum);
			}
		w.line(s + "  # frame " + std::to_string(i));
		if (r[12] != 0.0f || r[13] != 0.0f || r[14] != 0.0f) w.note("mtrx " + std::to_string(i) + " translation");
	}
	if (m.mtrx.count > 0) {
		ThreediMatrix4x4 identity;
		threedi_mat4_identity(&identity);
		bool same = true;
		for (int k = 0; k < 16; ++k) same = same && identity.m[k] == m.mtrx.matrices[0].m[k];
		if (!same) w.note("mtrx 0 is not the identity");
	}
	write_materials(w, m, dir);
	for (size_t li = 0; li < m.lod_count; ++li) write_lod(w, m, li, uv1);
	for (size_t i = 0; i < m.user_point_count; ++i) {
		const ThreediUserPoint &u = m.user_points[i];
		w.line("userpoint " + name_field(u.name) + " " + f17(u.x / 65536.0) + " " + f17(u.y / 65536.0) + " " + f17(u.z / 65536.0) + " " +
				f17(u.rot_x / 65536.0) + " " + f17(u.rot_y / 65536.0) + " " + f17(u.rot_z / 65536.0) + " " +
				std::to_string(u.subobject_index) + " " + std::to_string(u.userpoint_type));
	}
	write_lights(w, m);
	write_occlusion(w, m);
	write_collision(w, m);
	std::fclose(w.f);
	for (const std::string &n : w.notes) std::fprintf(stderr, "opennova-3di: note: scene drops %s\n", n.c_str());
	threedi_3di3_free(&m);
	return 0;
}

} // namespace threedi_cli
