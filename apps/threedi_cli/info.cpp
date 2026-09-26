// opennova-3di info: print what a .3di holds, the facts an author needs to
// match a retail model (LODs, parts, shaders, textures, user points, lights,
// registers, part animations, occlusion, collision). Any .3di reads, retail
// ones included.

#include <cstdio>
#include <cstring>
#include <map>
#include <string>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace threedi_cli {

namespace {

void print_track(const Threedi3di3 &m, int t, const ThreediTransform &tr) {
	if (tr.control == 0 && tr.rate == 0 && tr.start == 0 && tr.end == 0) return;
	const char *style = threedi_panm_control_name(tr.control);
	std::string reg;
	if (threedi_panm_parameter_is_ctrl_reference(tr.control) && tr.control_param < m.ctrl.count)
		reg = m.ctrl.registers[tr.control_param].name;
	std::printf("        %-6s style %3u (%s) param %u%s%s rate %d start %d end %d\n", track_label(t),
			tr.control, style != nullptr ? style : "?", tr.control_param, reg.empty() ? "" : " reg ",
			reg.c_str(), tr.rate, tr.start, tr.end);
}

// Model axes (the OCCL and LGHT frame) -> mission axes (x forward, y left, z up).
void mission_of(const float *model, double out[3]) {
	const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{model[0], model[1], model[2]});
	out[0] = m.x;
	out[1] = m.y;
	out[2] = m.z;
}

void print_lights(const Threedi3di3 &m, int verbose) {
	for (size_t i = 0; i < m.light_count; ++i) {
		const ThreediLight &l = m.lights[i];
		double p[3], d[3];
		mission_of(l.offset, p);
		mission_of(l.rotation, d);
		std::printf("light %zu  style %u  part %u  atten %.2f..%.2f  phase %u rate %u  rgb %u %u %u -> %u %u %u"
				"  flags 0x%x  pos(mission) %.3f %.3f %.3f\n",
				i, l.style, l.subobj_index, l.atten_start, l.atten_end, l.phase, l.rate, l.color_start[2],
				l.color_start[1], l.color_start[0], l.color_end[2], l.color_end[1], l.color_end[0], l.flags, p[0], p[1],
				p[2]);
		if (verbose)
			std::printf("    dir(mission) %.4f %.4f %.4f  cos %.4f  falloff %u  unknown %u  view_proj %g %g %g %g | %g %g %g %g"
					" | %g %g %g %g | %g %g %g %g\n",
					d[0], d[1], d[2], l.rotation[3], l.falloff_byte, l.unknown1, l.view_proj[0], l.view_proj[1],
					l.view_proj[2], l.view_proj[3], l.view_proj[4], l.view_proj[5], l.view_proj[6], l.view_proj[7],
					l.view_proj[8], l.view_proj[9], l.view_proj[10], l.view_proj[11], l.view_proj[12], l.view_proj[13],
					l.view_proj[14], l.view_proj[15]);
	}
}

void print_occlusion(const Threedi3di3 &m, int verbose) {
	size_t v = 0, p = 0, f = 0;
	for (size_t o = 0; o < m.occlusion_object_count; ++o) {
		const ThreediOcclusionObject &ob = m.occlusion_objects[o];
		double c[3];
		mission_of(ob.position, c);
		std::printf("occl %zu  type %u  sections %u -> %u  verts %d planes %d faces %d  centre(mission) %.3f %.3f %.3f"
				"  r %.3f  slot priority %g\n",
				o, ob.type, ob.parent_subobject_index, ob.connecting_subobject, ob.num_vertices, ob.num_planes,
				ob.face_count, c[0], c[1], c[2], ob.radius, ob.slot_priority_scale);
		for (int k = 0; k < ob.num_vertices && v < m.occlusion_vertex_count; ++k, ++v) {
			double q[3];
			mission_of(m.occlusion_vertices[v].position, q);
			if (verbose > 1) std::printf("    ov %d  %.4f %.4f %.4f\n", k, q[0], q[1], q[2]);
		}
		for (int k = 0; k < ob.num_planes && p < m.occlusion_plane_count; ++k, ++p) {
			double n[3];
			mission_of(m.occlusion_planes[p].normal, n);
			if (verbose > 1) std::printf("    op %d  n %.4f %.4f %.4f  d %.4f\n", k, n[0], n[1], n[2], m.occlusion_planes[p].radius);
		}
		for (int k = 0; k < ob.face_count && f < m.occlusion_face_count; ++k, ++f) {
			const ThreediOcclusionFace &fc = m.occlusion_faces[f];
			if (verbose > 1)
				std::printf("    of %d  %u %u %u  plane %u  edges 0x%08x 0x%08x\n", k, fc.raw_indices & 0xFF,
						(fc.raw_indices >> 8) & 0xFF, (fc.raw_indices >> 16) & 0xFF, fc.raw_indices >> 24, fc.edge_data,
						fc.other_edge_data);
		}
	}
}

} // namespace

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
			std::printf("    part %2zu parent %2d  strips %d+%d  abs(model) %.5f %.5f %.5f  radius %.3f\n", p,
					ro.parent_index, ro.num_strips, ro.num_alpha_strips, ro.abs[0], ro.abs[1], ro.abs[2],
					ro.bounding_radius);
		}
		if (verbose > 2) {
			// Every vertex in mission axes with its owning ROBJ (the ROBJ walk
			// assigns strips in order) and, when skinned, its first bone.
			size_t cursor = 0;
			for (size_t p = 0; p < lod.render_object_count; ++p) {
				const ThreediRenderObject &ro = lod.render_objects[p];
				const size_t count = static_cast<size_t>(ro.num_strips + ro.num_alpha_strips);
				for (size_t s = 0; s < count && cursor < lod.strip_count; ++s, ++cursor) {
					const ThreediTriangleStrip &st = lod.strips[cursor];
					for (int i = 0; i < st.num_vertices; ++i) {
						const ThreediVertex &v = lod.vertices.items[st.start_vertex + i];
						const int bone = st.bone_table_length > 0 && v.bone_indices[0] < st.bone_table_length
								? st.bone_table[v.bone_indices[0]] : static_cast<int>(p);
						double q[3];
						mission_of(v.position, q);
						std::printf("vert lod %zu part %zu strip %zu bone %d  %.5f %.5f %.5f\n", li, p, cursor, bone, q[0],
								q[1], q[2]);
					}
				}
			}
		}
		if (verbose > 1) {
			for (size_t s = 0; s < lod.strip_count; ++s) {
				const ThreediTriangleStrip &st = lod.strips[s];
				std::printf("    strip %zu mat %d strip %d idx %d+%u verts %d+%d bones[%d]", s, st.material_index,
						st.is_strip, st.index_offset, st.num_indices, st.start_vertex, st.num_vertices,
						st.bone_table_length);
				for (int b = 0; b < st.bone_table_length && b < 16; ++b) std::printf(" %u", st.bone_table[b]);
				std::printf("\n");
			}
			for (size_t p = 0; p < lod.render_object_count; ++p) {
				const ThreediRenderObject &ro = lod.render_objects[p];
				std::printf("    robj %zu center %.3f %.3f %.3f rel %.3f %.3f %.3f\n", p, ro.bounding_center[0],
						ro.bounding_center[1], ro.bounding_center[2], ro.rel[0], ro.rel[1], ro.rel[2]);
			}
		}
		for (size_t a = 0; a < lod.part_animation_count; ++a) {
			const ThreediPartAnimation &pa = lod.part_animations[a];
			std::printf("    panm part %u parent %u flags 0x%08x  matrix %u offset %u bind %d\n", pa.subobject_index,
					pa.parent_subobject, pa.flags, pa.matrix_index, pa.matrix_offset, pa.bind_matrix_index);
			const auto tracks = panm_tracks(pa);
			for (int t = 0; t < kTrackCount; ++t) print_track(m, t, *tracks[t]);
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
	std::printf("mtrx %u  occl objects %zu  lights %zu\n", m.mtrx.count, m.occlusion_object_count, m.light_count);
	if (verbose > 1)
		for (uint32_t i = 0; i < m.mtrx.count; ++i) {
			const float *r = m.mtrx.matrices[i].m;
			std::printf("    mtrx %u  %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f | %.3f %.3f %.3f %.3f\n", i,
					r[0], r[1], r[2], r[3], r[4], r[5], r[6], r[7], r[8], r[9], r[10], r[11], r[12], r[13], r[14], r[15]);
		}
	for (size_t i = 0; i < m.user_point_count; ++i) {
		const ThreediUserPoint &u = m.user_points[i];
		std::printf("userpoint %-15s  type %d  part %d  pos(mission) %.3f %.3f %.3f  dir %.3f %.3f %.3f\n", u.name,
				u.userpoint_type, u.subobject_index, u.x / 65536.0, u.y / 65536.0, u.z / 65536.0, u.rot_x / 65536.0,
				u.rot_y / 65536.0, u.rot_z / 65536.0);
	}
	print_lights(m, verbose);
	print_occlusion(m, verbose);
	if (m.collision != nullptr) {
		const ThreediCollisionModel &c = *m.collision;
		const float *b = c.model_data.bbox;
		std::printf("collision  objects %zu  verts %zu  faces %zu  volumes %zu  bbox(mission) %.2f %.2f %.2f .. %.2f %.2f %.2f\n",
				c.object_count, c.vertex_count, c.face_count, c.volume_count, b[0], b[1], b[2], b[3], b[4], b[5]);
		for (size_t o = 0; o < c.object_count; ++o) {
			const ThreediCollisionObject &co = c.objects[o];
			std::printf("    cobj %zu parent %d  verts %d faces %d volumes %d", o, co.parent_subobject_index,
					co.num_vertices, co.num_faces, co.num_bounding_volumes);
			if (verbose)
				std::printf("  offset %.3f %.3f %.3f  sphere %.3f %.3f %.3f r %.3f", co.offset[0] / 65536.0,
						co.offset[1] / 65536.0, co.offset[2] / 65536.0, co.med[0] / 65536.0, co.med[1] / 65536.0,
						co.med[2] / 65536.0, co.radius / 65536.0);
			std::printf("\n");
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

} // namespace threedi_cli
