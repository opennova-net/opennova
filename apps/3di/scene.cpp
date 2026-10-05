// opennova-3di scene: write a .3di back out as .o3d scene text, the inverse
// of `build`, so a DCC importer (the Blender add-on's importer.py) lays a
// model out by the same naming contract the exporter reads and the model
// exports again. Only records `build` reads are written, in the order it
// requires; every conversion is the exact inverse of build's (model ->
// mission axes, the render and collision winding flips), so
// `build(scene(x))` re-mints a builder-made model byte for byte. What the
// scene text cannot carry is listed as `#` comments and on stderr.
//
// `texfile <name> <type> <path|->` records name the file each texture row,
// by its name and type, loads beside the model: the one file the loader its
// type picks opens (renderer::material_texture_source, the game's and the
// editor's rule); `build` ignores them.

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <map>
#include <set>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include <base/io/strutil.h>
#include <base/resource_index/texture_candidates.h>
#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_strip_decode.h>

#include <formats/threedi/scene_text.h>
#include <runtime/renderer/material_texture.h>
#include "threedi_cli.h"

using namespace opennova::threedi;

namespace opennova::threedi_cli {

namespace {

std::string vec9(const float *model) {
	const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{model[0], model[1], model[2]});
	return f9(m.x) + " " + f9(m.y) + " " + f9(m.z);
}

// A generator's register field: the CTRL index for styles above 0x70, else -1.
int register_field(uint8_t style, int reg) { return threedi_generator_names_register(style) ? reg : -1; }

// The scene text, written out whole once the model has been walked. `note`
// reports what the text cannot carry (`# dropped: ...`); `remark` what it
// carries in a form worth knowing about (`# note: ...`).
struct Writer {
	std::string text;
	std::vector<std::string> notes, remarks;

	void line(const std::string &s) {
		text += s;
		text += '\n';
	}
	void note(const std::string &s) {
		notes.push_back(s);
		line("# dropped: " + s);
	}
	void remark(const std::string &s) {
		remarks.push_back(s);
		line("# note: " + s);
	}
};

// The file a texture row loads beside the model: the one file the loader the row's type picks
// opens (renderer::material_texture_source over the runtime type the loader stores), matched
// in the folder without case. The folder is loose files alone, so no name is a loose-first
// hit. "" when that loader opens no file or the folder lacks it.
std::string row_texture_file(const opennova::TextureFolder &folder, const char *name, uint8_t type) {
	namespace renderer = opennova::renderer;
	const auto held = [&folder](const std::string &file) { return folder.count(opennova::strutil::to_lower(file)) != 0; };
	const renderer::MaterialTextureSource source =
			renderer::material_texture_source(name, renderer::material_texture_runtime_type(type), held);
	if (source.reader == renderer::MaterialTextureReader::None) return std::string();
	const auto it = folder.find(opennova::strutil::to_lower(source.file));
	return it == folder.end() ? std::string() : it->second;
}

void write_materials(Writer &w, const Threedi3di3 &m, const opennova::TextureFolder &folder) {
	// One record per name and type: one name under two types can load two files.
	std::set<std::pair<std::string, uint8_t>> resolved;
	for (uint32_t i = 0; i < m.material_count; ++i) {
		const ThreediMaterial &mt = m.materials[i];
		w.line("material " + name_field(w, mt.shader_name[0] != '\0' ? mt.shader_name : "FF_ST_OP") + "  # " +
				std::to_string(i));
		if (mt.shader_name[0] == '\0') w.note("material " + std::to_string(i) + " has no shader tag");
		for (uint32_t t = 0; t < mt.texture_count && t < 24; ++t) {
			const ThreediMaterialTexture &tx = mt.textures[t];
			w.line("texture " + name_field(w, tx.name) + " " + std::to_string(tx.slot) + " " + std::to_string(tx.type) + " " +
					std::to_string(tx.flags) + " " + std::to_string(tx.frame));
			if (resolved.insert({tx.name, tx.type}).second) {
				const std::string path = row_texture_file(folder, tx.name, tx.type);
				w.line("texfile " + name_field(w, tx.name) + " " + std::to_string(tx.type) + " " + (path.empty() ? "-" : path));
			}
		}
		const ThreediTexAnim &a = mt.animation;
		if (a.num_frames != 0 || a.animation_type != 0 || a.cycle_frame_time != 0)
			w.line("texanim " + std::to_string(a.num_frames) + " " + std::to_string(a.animation_type) + " " +
					std::to_string(a.cycle_frame_time));
		const float *rc = mt.reflect_color;
		if (rc[0] != 0.0f || rc[1] != 0.0f || rc[2] != 0.0f || rc[3] != 0.0f)
			w.line("reflect " + std::to_string(threedi_build_byte_of(rc[0])) + " " + std::to_string(threedi_build_byte_of(rc[1])) + " " +
					std::to_string(threedi_build_byte_of(rc[2])) + " " + std::to_string(threedi_build_byte_of(rc[3])));
		if (mt.material_flags != 0) w.line("matflags " + std::to_string(mt.material_flags));
		if (mt.alpha_test_value_byte != 0) w.line("alphatest " + std::to_string(mt.alpha_test_value_byte));
		if (mt.is_glass != 0) w.line("glass " + std::to_string(mt.is_glass));
		if (mt.emissive_type != 0) w.line("emissive " + std::to_string(mt.emissive_type));
		const ThreediRgbGen &g = mt.rgb_gen;
		if (g.style != 0) {
			std::string s = "rgbgen " + std::to_string(g.style) + " " + std::to_string(register_field(g.style, g.reg)) + " " +
					f9(g.rate);
			for (int k = 0; k < 3; ++k) s += " " + std::to_string(threedi_build_byte_of(g.start_color[k]));
			for (int k = 0; k < 3; ++k) s += " " + std::to_string(threedi_build_byte_of(g.end_color[k]));
			w.line(s + " " + f9(g.phase));
			if (g.start_color[3] != 0.0f || g.end_color[3] != 0.0f)
				w.note("material " + std::to_string(i) + " rgbgen alpha bytes");
		}
		const ThreediAlphaGen &ag = mt.alpha_gen;
		if (ag.style != 0)
			w.line("alphagen " + std::to_string(ag.style) + " " + std::to_string(register_field(ag.style, ag.reg)) + " " +
					f9(ag.rate) + " " + std::to_string(ag.start) + " " + std::to_string(ag.end) + " " + f9(ag.phase));
		const ThreediUvParams *uv[2] = {&mt.u_params, &mt.v_params};
		for (int k = 0; k < 2; ++k)
			if (uv[k]->style != 0)
				w.line(std::string(k == 0 ? "ugen " : "vgen ") + std::to_string(uv[k]->style) + " " +
						std::to_string(register_field(uv[k]->style, uv[k]->reg)) + " " + f9(uv[k]->gen_rate) + " " +
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

// Which strips of a LOD share their vertex window with another strip.
// Retail's exporter pools one window between strips, across parts too
// (Dblkhwk1's rotor strips of parts 3 and 4, Armry01's part 3 and its
// neighbours; 70 JO models).
std::vector<bool> shared_windows(const ThreediLod &lod) {
	std::vector<bool> shared(lod.strip_count, false);
	for (size_t a = 0; a < lod.strip_count; ++a)
		for (size_t b = a + 1; b < lod.strip_count; ++b) {
			const ThreediTriangleStrip &x = lod.strips[a], &y = lod.strips[b];
			if (x.num_vertices > 0 && y.num_vertices > 0 && x.start_vertex < y.start_vertex + y.num_vertices &&
					y.start_vertex < x.start_vertex + x.num_vertices)
				shared[a] = shared[b] = true;
		}
	return shared;
}

// The window vertices a strip is written with, in window order: all of them,
// unless the window is shared, when only those its own triangles use. The rest
// belong to the strips it shares with, and a part's sphere spans the vertices
// its triangles use (Armry01's part 3 and Dblkhwk1's part 4 match retail's
// sphere exactly over those, and match no sphere over the whole window). A
// vertex of an unshared window that no triangle uses is the author's: build
// writes it again.
std::vector<int> strip_vertices(const ThreediLod &lod, const ThreediTriangleStrip &st, bool shared) {
	std::vector<int> out;
	std::vector<uint16_t> tris;
	if (shared && st.num_indices > 0 && threedi_decode_strip_indices(lod, st, tris)) {
		std::vector<bool> used(static_cast<size_t>(st.num_vertices), false);
		for (const uint16_t i : tris) used[i] = true;
		for (int i = 0; i < st.num_vertices; ++i)
			if (used[static_cast<size_t>(i)]) out.push_back(i);
		return out;
	}
	for (int i = 0; i < st.num_vertices; ++i) out.push_back(i);
	return out;
}

void write_strip(Writer &w, const Threedi3di3 &m, const ThreediLod &lod, const ThreediTriangleStrip &st, bool alpha,
		bool uv1, bool skinned, bool shared) {
	const int material = threedi_material_array_index_for_id(m, st.material_index);
	w.line("strip " + std::to_string(material < 0 ? 0 : material) + " " + (alpha ? "1" : "0"));
	if (material < 0) w.note("a strip names material id " + std::to_string(st.material_index) + " the model lacks");
	if (skinned) {
		std::string s = "bones";
		for (int b = 0; b < st.bone_table_length && b < 16; ++b) s += " " + std::to_string(st.bone_table[b]);
		w.line(s);
	}
	// Each written vertex's number in the scene's strip.
	const std::vector<int> vertices = strip_vertices(lod, st, shared);
	std::vector<int> number(static_cast<size_t>(std::max(0, st.num_vertices)), -1);
	for (size_t k = 0; k < vertices.size(); ++k) number[static_cast<size_t>(vertices[k])] = static_cast<int>(k);
	for (const int i : vertices) {
		const ThreediVertex &v = lod.vertices.items[st.start_vertex + i];
		std::string s = "v " + vec9(v.position) + " " + vec9(v.normal) + " " + f9(v.uv0[0]) + " " + f9(v.uv0[1]);
		if (uv1) s += " " + f9(v.uv1[0]) + " " + f9(v.uv1[1]);
		// Skinned: the four slots, then the three weights (slot i3 takes the
		// rest, threedi_skin_influences).
		if (skinned) {
			for (int k = 0; k < 4; ++k) s += " " + std::to_string(v.bone_indices[k]);
			for (int k = 0; k < 3; ++k) s += " " + f9(v.bone_weights[k]);
		}
		w.line(s);
	}
	if (st.num_indices == 0) return; // vertices and no triangle: build writes the same
	std::vector<uint16_t> tris;
	if (!threedi_decode_strip_indices(lod, st, tris)) {
		w.note("a strip whose indices escape its vertex window");
		return;
	}
	// The loader's decode drops a triangle that repeats a corner; so does the
	// scene (build refuses one).
	if (!st.is_strip && tris.size() < static_cast<size_t>(st.num_indices / 3) * 3)
		w.note("a strip's " + std::to_string(st.num_indices / 3 - tris.size() / 3) + " triangles that repeat a corner");
	// Retail winds counter-clockwise in model axes; the scene winds
	// counter-clockwise in mission axes, the mirror of model: swap the second
	// and third corners (the inverse of build's swap).
	for (size_t t = 0; t + 2 < tris.size(); t += 3)
		w.line("t " + std::to_string(number[tris[t]]) + " " + std::to_string(number[tris[t + 2]]) + " " +
				std::to_string(number[tris[t + 1]]));
}

void write_lod(Writer &w, const Threedi3di3 &m, size_t li, bool uv1) {
	const ThreediLod &lod = m.lods[li];
	const bool skinned = m.header.mesh_type == THREEDI_MESH_SKINNED;
	w.line("lod " + std::to_string(lod.lod_threshold) + " " + (lod.model_type[0] != '\0' ? lod.model_type : "gnrc") +
			"  # lod " + std::to_string(li));
	if (lod.model_type[0] == '\0') w.note("lod " + std::to_string(li) + " has no type (written as gnrc)");
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
	const std::vector<bool> shared = shared_windows(lod);
	if (skinned && lod.render_object_count > 0) {
		// The retail skinned layout keeps every strip on the root ROBJ; the
		// scene authors them on the mesh part(s), which the builder moves back.
		// A LOD with no part owns no strip (retail ships empty LODs).
		std::vector<std::pair<size_t, bool>> all;
		for (auto &o : owned) {
			all.insert(all.end(), o.begin(), o.end());
			o.clear();
		}
		// The parts geometry was authored on carry bounds: the mesh parts no
		// bone table names, else (a model authored on its bones, dM1A1's hull)
		// every part with bounds.
		std::vector<int> mesh = skinned_mesh_parts(lod);
		if (mesh.empty())
			for (size_t p = 0; p < lod.render_object_count; ++p)
				if (lod.render_objects[p].bounding_radius > 0.0f) mesh.push_back(static_cast<int>(p));
		if (mesh.empty()) {
			if (!all.empty()) w.remark("lod " + std::to_string(li) + ": no part carries bounds (strips kept on the root)");
			owned[0] = all;
		} else {
			// Each strip goes to the tightest of those parts whose sphere holds
			// every vertex of it (the first when none does).
			int homeless = 0;
			for (const auto &entry : all) {
				const ThreediTriangleStrip &st = lod.strips[entry.first];
				const std::vector<int> vertices = strip_vertices(lod, st, shared[entry.first]);
				int best = mesh.front();
				float best_radius = -1.0f;
				for (int p : mesh) {
					const ThreediRenderObject &ro = lod.render_objects[p];
					bool inside = true;
					for (size_t j = 0; j < vertices.size() && inside; ++j) {
						const float *v = lod.vertices.items[st.start_vertex + vertices[j]].position;
						double d = 0;
						for (int k = 0; k < 3; ++k) d += (v[k] - ro.bounding_center[k]) * (v[k] - ro.bounding_center[k]);
						inside = std::sqrt(d) <= ro.bounding_radius * 1.0001 + 1e-5;
					}
					if (inside && (best_radius < 0.0f || ro.bounding_radius < best_radius)) {
						best = p;
						best_radius = ro.bounding_radius;
					}
				}
				if (best_radius < 0.0f) ++homeless;
				owned[best].push_back(entry);
			}
			if (homeless > 0)
				w.remark("lod " + std::to_string(li) + ": " + std::to_string(homeless) +
						" skinned strips lie in no part's bounds (placed on part " + std::to_string(mesh.front()) + ")");
		}
	}
	for (size_t p = 0; p < lod.render_object_count; ++p) {
		const ThreediRenderObject &ro = lod.render_objects[p];
		// A part that draws nothing keeps the point its sphere sits on.
		const bool seeded = owned[p].empty() && ro.bounding_radius == 0.0f &&
				(ro.bounding_center[0] != 0.0f || ro.bounding_center[1] != 0.0f || ro.bounding_center[2] != 0.0f);
		w.line("part " + std::to_string(ro.parent_index) + " " + vec9(ro.abs) + (seeded ? " " + vec9(ro.bounding_center) : "") +
				"  # part " + std::to_string(p));
		for (const auto &entry : owned[p])
			write_strip(w, m, lod, lod.strips[entry.first], entry.second, uv1, skinned, shared[entry.first]);
	}
	for (size_t a = 0; a < lod.part_animation_count; ++a) {
		const ThreediPartAnimation &pa = lod.part_animations[a];
		const auto tracks = threedi_panm_tracks(pa);
		// The flags word build derives from the tracks; any other is written.
		const uint8_t axis = threedi_panm_translate_type(pa.flags);
		const uint32_t derived =
				threedi_build_panm_flags(pa, axis != 0 ? axis : static_cast<uint8_t>(THREEDI_TRANS_Z));
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
		for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t) {
			const ThreediTransform &tr = *tracks[t];
			if (tr.control == 0 && tr.control_param == 0 && tr.rate == 0 && tr.start == 0 && tr.end == 0) continue;
			std::string reg = tr.control_param != 0 ? std::to_string(tr.control_param) : "-";
			if (threedi_generator_names_register(tr.control)) {
				if (tr.control_param < m.ctrl.count) reg = name_field(w, m.ctrl.registers[tr.control_param].name);
				else w.note("a track names CTRL " + std::to_string(tr.control_param) + " the model lacks");
			}
			std::string line = std::string("track ") + threedi_panm_track_label(t) + " " + std::to_string(tr.control) + " " + reg + " " +
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

// CXLT after the collision records: every row, in order, or a bare `cxlt`
// when the table is empty but build's derived rule would fill it (retail
// Chair03X: seven sections, no row).
void write_translations(Writer &w, const Threedi3di3 &m) {
	if (m.collision == nullptr) return;
	const ThreediCollisionModel &c = *m.collision;
	for (size_t i = 0; i < c.translation_count; ++i) {
		const int32_t *t = c.translations[i].translation;
		w.line("cxlt " + f17(t[0] / 65536.0) + " " + f17(t[1] / 65536.0) + " " + f17(t[2] / 65536.0));
	}
	const bool skinned = m.header.mesh_type == THREEDI_MESH_SKINNED;
	const size_t derived = c.object_count == 0 ? 0 : c.object_count - (skinned ? 0 : 1);
	if (c.translation_count == 0 && derived > 0) w.line("cxlt  # an empty table");
}

void write_occlusion(Writer &w, const Threedi3di3 &m) {
	size_t v = 0, p = 0, f = 0;
	for (size_t o = 0; o < m.occlusion_object_count; ++o) {
		const ThreediOcclusionObject &ob = m.occlusion_objects[o];
		// The record's sphere, only when it is not the one build derives from
		// the vertices: 206 retail models store the centre mirrored across y
		// (Crdrblk2, DRGVLA), which the portal walk reads as it is. Retail's
		// own centres sit within float noise of the derived ones (Armry01's
		// up to 5e-7 m off: OED summed in another precision), so a sphere
		// within the 1e-4 m compare holds a stored position to is the
		// derived one.
		const size_t count = std::min(static_cast<size_t>(std::max(0, ob.num_vertices)), m.occlusion_vertex_count - v);
		float centre[3], radius = 0.0f;
		threedi_build_occ_sphere(count > 0 ? m.occlusion_vertices + v : nullptr, count, centre, radius);
		bool derived = std::memcmp(centre, ob.position, sizeof(centre)) == 0 &&
				std::memcmp(&radius, &ob.radius, sizeof(radius)) == 0;
		if (!derived && std::isfinite(radius) && std::isfinite(ob.radius)) {
			derived = std::fabs(radius - ob.radius) <= 1e-4;
			for (int k = 0; k < 3; ++k)
				derived = derived && std::isfinite(centre[k]) && std::isfinite(ob.position[k]) &&
						std::fabs(centre[k] - ob.position[k]) <= 1e-4;
		}
		const std::string sphere = derived ? "" : " " + vec9(ob.position) + " " + f9(ob.radius);
		w.line("occ " + std::to_string(ob.type) + " " + std::to_string(ob.parent_subobject_index) + " " +
				std::to_string(ob.connecting_subobject) + sphere + "  # record " + std::to_string(o));
		if (ob.slot_priority_scale != 0.0f) w.note("occ record " + std::to_string(o) + " slot_priority_scale");
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
		const std::string phase = threedi_generator_names_register(l.style) ? std::to_string(l.phase)
																			: f9(threedi_build_light_phase_value(l.phase));
		char flags[8];
		std::snprintf(flags, sizeof(flags), "0x%02x", l.flags);
		std::string s = "light " + std::to_string(l.subobj_index) + " " + vec9(l.offset) + " " + f9(l.atten_start) + " " +
				f9(l.atten_end) + " " + std::to_string(l.style) + " " + f9(threedi_build_light_rate_value(l.rate)) + " " + phase;
		for (int k = 2; k >= 0; --k) s += " " + std::to_string(l.color_start[k]);
		for (int k = 2; k >= 0; --k) s += " " + std::to_string(l.color_end[k]);
		s += std::string(" ") + flags;
		// The light's axis and cone, unless they are the omni default.
		const bool omni = l.rotation[0] == 0.0f && l.rotation[1] == -1.0f && l.rotation[2] == 0.0f &&
				l.rotation[3] == 1.0f && l.falloff_byte == 0;
		if (!omni) s += " " + vec9(l.rotation) + " " + f9(threedi_build_light_cone_half_angle(l));
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
	std::error_code ec;
	const opennova::TextureFolder folder = opennova::list_texture_folder(std::filesystem::absolute(std::filesystem::path(model_path), ec).parent_path());
	w.line("o3d 1");
	w.line(std::string("# scene of ") + model_path + " (opennova-3di scene)");
	w.line("model " + name_field(w, m.header.name[0] != '\0' ? m.header.name : "MODEL"));
	if (m.header.name[0] == '\0') w.note("the model has no name (written as MODEL)");
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
	for (uint32_t i = 0; i < m.ctrl.count; ++i) w.line("register " + name_field(w, m.ctrl.registers[i].name));
	// MTRX row 0 is the identity build always writes; rows 1.. are frames.
	for (uint32_t i = 1; i < m.mtrx.count; ++i) {
		const float *r = m.mtrx.matrices[i].m;
		bool finite = true;
		for (int k = 0; k < 16; ++k) finite = finite && std::isfinite(r[k]);
		if (!finite) {
			// Skinned models carry NaN rows no PANM row selects (US01, ArmsG).
			w.line("mtrx 1 0 0 0 1 0 0 0 1  # frame " + std::to_string(i));
			w.note("mtrx " + std::to_string(i) + " is not finite (written as the identity)");
			continue;
		}
		double rotation[9];
		threedi_build_frame_to_mission(m.mtrx.matrices[i], rotation);
		std::string s = "mtrx";
		for (double v : rotation) s += " " + f9(v);
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
	write_materials(w, m, folder);
	for (size_t li = 0; li < m.lod_count; ++li) write_lod(w, m, li, uv1);
	for (size_t i = 0; i < m.user_point_count; ++i) {
		const ThreediUserPoint &u = m.user_points[i];
		w.line("userpoint " + name_field(w, u.name) + " " + f17(u.x / 65536.0) + " " + f17(u.y / 65536.0) + " " + f17(u.z / 65536.0) + " " +
				f17(u.rot_x / 65536.0) + " " + f17(u.rot_y / 65536.0) + " " + f17(u.rot_z / 65536.0) + " " +
				std::to_string(u.subobject_index) + " " + std::to_string(u.userpoint_type));
	}
	write_lights(w, m);
	write_occlusion(w, m);
	write_collision(w, m);
	write_translations(w, m);
	threedi_3di3_free(&m);
	if (!write_output(out_path, w.text.data(), w.text.size())) return 1;
	for (const std::string &n : w.notes) std::fprintf(stderr, "opennova-3di: note: scene drops %s\n", n.c_str());
	for (const std::string &n : w.remarks) std::fprintf(stderr, "opennova-3di: note: %s\n", n.c_str());
	return 0;
}

} // namespace opennova::threedi_cli
