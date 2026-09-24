// opennova-3di compare: tell whether two .3di files hold the same model,
// ignoring what an authoring round trip legitimately changes (strip layout,
// vertex order and sharing, material order, register order, float noise,
// derived bounds) and reporting everything else: LOD types and thresholds,
// part hierarchy and pivots, per-part geometry per material (triangle count,
// area, area-weighted centroid, normal and UVs, so winding and mapping
// count), materials, PANM tracks and frames, user points, lights, occlusion
// records and collision (sections, bullet faces, volumes as the polytopes
// their planes bound). Seam-flag differences are reported as info only.
// Exit 0 when the models match, 1 with a difference list.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <set>
#include <string>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_strip_decode.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace threedi_cli {

namespace {

constexpr double kPosTol = 2e-3;   // metres
constexpr double kDirTol = 2e-3;   // unit vectors
constexpr double kRelTol = 2e-3;   // relative (areas)

struct Diff {
	std::vector<std::string> lines;
	std::vector<std::string> info;
	void add(const std::string &s) { lines.push_back(s); }
};

std::string num(double v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.5g", v);
	return buf;
}

using Vec = std::array<double, 3>;

Vec mission(const float *model) {
	const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{model[0], model[1], model[2]});
	return {m.x, m.y, m.z};
}

// Equal within `tol`; a NaN matches a NaN (retail ships them in normals,
// planes and frames).
bool close(double a, double b, double tol) {
	return (std::isnan(a) && std::isnan(b)) || std::fabs(a - b) <= tol;
}

bool near(const Vec &a, const Vec &b, double tol) {
	for (int k = 0; k < 3; ++k)
		if (!close(a[k], b[k], tol)) return false;
	return true;
}

std::string vs(const Vec &v) { return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")"; }

std::string reg_name(const Threedi3di3 &m, int style, int reg) {
	if (style <= THREEDI_GENERATOR_CTRL_REFERENCE_THRESHOLD) return "-";
	if (reg >= 0 && static_cast<uint32_t>(reg) < m.ctrl.count) return m.ctrl.registers[reg].name;
	return "#" + std::to_string(reg);
}

int byte_of(float unit) { return static_cast<int>(std::lround(unit * 255.0f)); }

// Everything a material means, as one comparable string.
std::string material_key(const Threedi3di3 &m, const ThreediMaterial &mt) {
	std::string k = std::string(mt.shader_name) + " flags " + std::to_string(mt.material_flags);
	if (mt.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) k += " at " + std::to_string(mt.alpha_test_value_byte);
	k += " glass " + std::to_string(mt.is_glass) + " emissive " + std::to_string(mt.emissive_type);
	k += " reflect";
	for (int c = 0; c < 4; ++c) k += " " + std::to_string(byte_of(mt.reflect_color[c]));
	for (uint32_t t = 0; t < mt.texture_count && t < 24; ++t) {
		const ThreediMaterialTexture &x = mt.textures[t];
		k += std::string(" [") + x.name + " " + std::to_string(x.slot) + " " + std::to_string(x.type) + " " +
				std::to_string(x.flags) + " " + std::to_string(x.frame) + "]";
	}
	const ThreediTexAnim &a = mt.animation;
	if (a.num_frames || a.animation_type || a.cycle_frame_time)
		k += " anim " + std::to_string(a.num_frames) + "/" + std::to_string(a.animation_type) + "/" +
				std::to_string(a.cycle_frame_time);
	const ThreediRgbGen &g = mt.rgb_gen;
	if (g.style) {
		k += " rgb " + std::to_string(g.style) + " " + reg_name(m, g.style, g.reg) + " " + num(g.rate) + " " + num(g.phase);
		for (int c = 0; c < 3; ++c) k += " " + std::to_string(byte_of(g.start_color[c]));
		for (int c = 0; c < 3; ++c) k += " " + std::to_string(byte_of(g.end_color[c]));
	}
	const ThreediAlphaGen &ag = mt.alpha_gen;
	if (ag.style)
		k += " alpha " + std::to_string(ag.style) + " " + reg_name(m, ag.style, ag.reg) + " " + num(ag.rate) + " " +
				num(ag.phase) + " " + std::to_string(ag.start) + " " + std::to_string(ag.end);
	const ThreediUvParams *uv[2] = {&mt.u_params, &mt.v_params};
	for (int i = 0; i < 2; ++i)
		if (uv[i]->style)
			k += std::string(i ? " vgen " : " ugen ") + std::to_string(uv[i]->style) + " " +
					reg_name(m, uv[i]->style, uv[i]->reg) + " " + num(uv[i]->gen_rate) + " " + num(uv[i]->phase) + " " +
					num(uv[i]->start) + " " + num(uv[i]->end);
	return k;
}

// A part's geometry under one material: enough to tell geometry, winding and
// mapping apart without matching vertices one to one.
struct Geometry {
	int triangles = 0;
	double area = 0.0;
	Vec centroid{0, 0, 0};  // area-weighted
	Vec normal{0, 0, 0};    // area-weighted face normal (winding)
	Vec vnormal{0, 0, 0};   // area-weighted stored vertex normal
	int agree = 0;          // triangles whose winding agrees with their vertex normals
	double uv0[2] = {0, 0}, uv1[2] = {0, 0};
	Vec mn{1e30, 1e30, 1e30}, mx{-1e30, -1e30, -1e30};
};

// (part, material key, alpha) -> geometry, for one LOD.
std::map<std::string, Geometry> lod_geometry(const Threedi3di3 &m, const ThreediLod &lod) {
	std::map<std::string, Geometry> out;
	size_t cursor = 0;
	for (size_t p = 0; p < lod.render_object_count; ++p) {
		const ThreediRenderObject &ro = lod.render_objects[p];
		for (int s = 0; s < ro.num_strips + ro.num_alpha_strips && cursor < lod.strip_count; ++s, ++cursor) {
			const ThreediTriangleStrip &st = lod.strips[cursor];
			const int mi = threedi_material_array_index_for_id(m, st.material_index);
			const std::string key = "part " + std::to_string(p) + " " +
					(mi >= 0 ? material_key(m, m.materials[mi]) : std::string("?")) + (s >= ro.num_strips ? " alpha" : "");
			Geometry &g = out[key];
			std::vector<uint16_t> tris;
			if (!threedi_decode_strip_indices(lod, st, tris)) continue;
			for (size_t t = 0; t + 2 < tris.size(); t += 3) {
				const ThreediVertex *v[3];
				Vec p3[3];
				for (int k = 0; k < 3; ++k) {
					v[k] = &lod.vertices.items[st.start_vertex + tris[t + k]];
					p3[k] = mission(v[k]->position);
				}
				Vec e{p3[1][0] - p3[0][0], p3[1][1] - p3[0][1], p3[1][2] - p3[0][2]};
				Vec f{p3[2][0] - p3[0][0], p3[2][1] - p3[0][1], p3[2][2] - p3[0][2]};
				// Model axes mirror mission: retail's model-axes counter-clockwise
				// order is clockwise here, so the outward normal is f x e.
				Vec n{f[1] * e[2] - f[2] * e[1], f[2] * e[0] - f[0] * e[2], f[0] * e[1] - f[1] * e[0]};
				const double a = 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
				++g.triangles;
				g.area += a;
				for (int k = 0; k < 3; ++k) {
					g.centroid[k] += a * (p3[0][k] + p3[1][k] + p3[2][k]) / 3.0;
					g.normal[k] += 0.5 * n[k];
					for (int c = 0; c < 3; ++c) {
						g.mn[k] = std::min(g.mn[k], p3[c][k]);
						g.mx[k] = std::max(g.mx[k], p3[c][k]);
					}
				}
				Vec sum{0, 0, 0};
				for (int c = 0; c < 3; ++c) {
					const Vec vn = mission(v[c]->normal);
					for (int k = 0; k < 3; ++k) sum[k] += vn[k];
				}
				if (n[0] * sum[0] + n[1] * sum[1] + n[2] * sum[2] > 0) ++g.agree;
				for (int c = 0; c < 3; ++c) {
					const Vec vn = mission(v[c]->normal);
					for (int k = 0; k < 3; ++k) g.vnormal[k] += a * vn[k] / 3.0;
					for (int k = 0; k < 2; ++k) {
						g.uv0[k] += a * v[c]->uv0[k] / 3.0;
						g.uv1[k] += a * v[c]->uv1[k] / 3.0;
					}
				}
			}
		}
	}
	for (auto &kv : out) {
		Geometry &g = kv.second;
		if (g.area > 0)
			for (int k = 0; k < 3; ++k) {
				g.centroid[k] /= g.area;
				g.vnormal[k] /= g.area;
			}
		if (g.area > 0)
			for (int k = 0; k < 2; ++k) {
				g.uv0[k] /= g.area;
				g.uv1[k] /= g.area;
			}
	}
	return out;
}

void compare_geometry(Diff &d, const std::string &where, const std::map<std::string, Geometry> &a,
		const std::map<std::string, Geometry> &b) {
	for (const auto &kv : a) {
		const auto it = b.find(kv.first);
		const std::string label = where + " " + kv.first.substr(0, kv.first.find(' ', 5)) + " [" +
				kv.first.substr(kv.first.find(' ', 5) + 1, 60) + "]";
		if (it == b.end()) {
			d.add(label + ": missing (" + std::to_string(kv.second.triangles) + " triangles)");
			continue;
		}
		const Geometry &x = kv.second, &y = it->second;
		if (x.triangles != y.triangles)
			d.add(label + ": " + std::to_string(x.triangles) + " vs " + std::to_string(y.triangles) + " triangles");
		const double scale = std::max(1.0, x.area);
		if (std::fabs(x.area - y.area) > kRelTol * scale)
			d.add(label + ": area " + num(x.area) + " vs " + num(y.area));
		if (!near(x.centroid, y.centroid, kPosTol)) d.add(label + ": centroid " + vs(x.centroid) + " vs " + vs(y.centroid));
		if (!near(x.normal, y.normal, kRelTol * scale))
			d.add(label + ": face normal sum " + vs(x.normal) + " vs " + vs(y.normal));
		if (x.agree != y.agree)
			d.add(label + ": " + std::to_string(x.agree) + " vs " + std::to_string(y.agree) +
					" triangles wound with their vertex normals (winding)");
		if (!near(x.vnormal, y.vnormal, 1e-2))
			d.add(label + ": vertex normals " + vs(x.vnormal) + " vs " + vs(y.vnormal));
		if (std::fabs(x.uv0[0] - y.uv0[0]) > 1e-3 || std::fabs(x.uv0[1] - y.uv0[1]) > 1e-3)
			d.add(label + ": uv0 mean (" + num(x.uv0[0]) + " " + num(x.uv0[1]) + ") vs (" + num(y.uv0[0]) + " " +
					num(y.uv0[1]) + ")");
		if (std::fabs(x.uv1[0] - y.uv1[0]) > 1e-3 || std::fabs(x.uv1[1] - y.uv1[1]) > 1e-3)
			d.add(label + ": uv1 mean (" + num(x.uv1[0]) + " " + num(x.uv1[1]) + ") vs (" + num(y.uv1[0]) + " " +
					num(y.uv1[1]) + ")");
		if (x.triangles > 0 && (!near(x.mn, y.mn, kPosTol) || !near(x.mx, y.mx, kPosTol)))
			d.add(label + ": bounds " + vs(x.mn) + ".." + vs(x.mx) + " vs " + vs(y.mn) + ".." + vs(y.mx));
	}
	for (const auto &kv : b)
		if (!a.count(kv.first))
			d.add(where + " " + kv.first.substr(0, 60) + ": extra (" + std::to_string(kv.second.triangles) + " triangles)");
}

std::string track_key(const Threedi3di3 &m, const ThreediTransform &t) {
	if (t.control == 0 && t.control_param == 0 && t.rate == 0 && t.start == 0 && t.end == 0) return "-";
	std::string param = threedi_panm_parameter_is_ctrl_reference(t.control)
			? (t.control_param < m.ctrl.count ? m.ctrl.registers[t.control_param].name : "#" + std::to_string(t.control_param))
			: std::to_string(t.control_param);
	return std::to_string(t.control) + " " + param + " " + std::to_string(t.rate) + " " + std::to_string(t.start) + " " +
			std::to_string(t.end);
}

void compare_panm(Diff &d, const std::string &where, const Threedi3di3 &a, const ThreediLod &la, const Threedi3di3 &b,
		const ThreediLod &lb) {
	std::map<int, const ThreediPartAnimation *> ra, rb;
	for (size_t i = 0; i < la.part_animation_count; ++i) ra[la.part_animations[i].subobject_index] = &la.part_animations[i];
	for (size_t i = 0; i < lb.part_animation_count; ++i) rb[lb.part_animations[i].subobject_index] = &lb.part_animations[i];
	for (const auto &kv : ra) {
		const auto it = rb.find(kv.first);
		const std::string w = where + " panm part " + std::to_string(kv.first);
		if (it == rb.end()) {
			d.add(w + ": missing");
			continue;
		}
		const ThreediPartAnimation &x = *kv.second, &y = *it->second;
		if (x.parent_subobject != y.parent_subobject)
			d.add(w + ": parent " + std::to_string(x.parent_subobject) + " vs " + std::to_string(y.parent_subobject));
		if (x.flags != y.flags) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), ": flags 0x%08x vs 0x%08x", x.flags, y.flags);
			d.add(w + buf);
		}
		const ThreediTransform *tx[] = {&x.rotation_x, &x.rotation_y, &x.rotation_z, &x.scale_x, &x.scale_y, &x.scale_z,
				&x.translation};
		const ThreediTransform *ty[] = {&y.rotation_x, &y.rotation_y, &y.rotation_z, &y.scale_x, &y.scale_y, &y.scale_z,
				&y.translation};
		for (int t = 0; t < kTrackCount; ++t) {
			const std::string kx = track_key(a, *tx[t]), ky = track_key(b, *ty[t]);
			if (kx != ky) d.add(w + " " + track_label(t) + ": " + kx + " vs " + ky);
		}
		// The rotation frame the row selects (a positive matrix_index).
		const auto frame = [](const Threedi3di3 &m, const ThreediPartAnimation &r) {
			std::array<double, 9> f{1, 0, 0, 0, 1, 0, 0, 0, 1};
			const int sel = static_cast<int8_t>(r.matrix_index);
			if (sel > 0 && static_cast<uint32_t>(sel) < m.mtrx.count)
				for (int i = 0; i < 3; ++i)
					for (int j = 0; j < 3; ++j) f[i * 3 + j] = m.mtrx.matrices[sel].m[i * 4 + j];
			return f;
		};
		const std::array<double, 9> fx = frame(a, x), fy = frame(b, y);
		for (int i = 0; i < 9; ++i)
			if (!close(fx[i], fy[i], 2e-3)) {
				d.add(w + ": rotation frame differs");
				break;
			}
	}
	for (const auto &kv : rb)
		if (!ra.count(kv.first)) d.add(where + " panm part " + std::to_string(kv.first) + ": extra");
}

// The corners of the convex polytope a plane list bounds (n . p + d <= 0 inside).
std::vector<Vec> polytope(const ThreediBoundingPlane *planes, int count) {
	std::vector<Vec> out;
	for (int i = 0; i < count; ++i)
		for (int j = i + 1; j < count; ++j)
			for (int k = j + 1; k < count; ++k) {
				const float *a = planes[i].normal, *b = planes[j].normal, *c = planes[k].normal;
				const double det = a[0] * (b[1] * c[2] - b[2] * c[1]) - a[1] * (b[0] * c[2] - b[2] * c[0]) +
						a[2] * (b[0] * c[1] - b[1] * c[0]);
				if (std::fabs(det) < 1e-9) continue;
				const double da = -planes[i].radius, db = -planes[j].radius, dc = -planes[k].radius;
				Vec p{(da * (b[1] * c[2] - b[2] * c[1]) - a[1] * (db * c[2] - b[2] * dc) + a[2] * (db * c[1] - b[1] * dc)) / det,
						(a[0] * (db * c[2] - b[2] * dc) - da * (b[0] * c[2] - b[2] * c[0]) + a[2] * (b[0] * dc - db * c[0])) / det,
						(a[0] * (b[1] * dc - db * c[1]) - a[1] * (b[0] * dc - db * c[0]) + da * (b[0] * c[1] - b[1] * c[0])) / det};
				bool inside = true;
				for (int q = 0; q < count && inside; ++q) {
					const float *n = planes[q].normal;
					inside = n[0] * p[0] + n[1] * p[1] + n[2] * p[2] + planes[q].radius <= 1e-3;
				}
				bool dup = false;
				for (const Vec &o : out) dup = dup || near(o, p, 1e-4);
				if (inside && !dup) out.push_back(p);
			}
	return out;
}

// Every corner lies inside (or within `tol` of) the plane list: two convex
// solids are the same when each one's corners lie inside the other.
bool inside_all(const std::vector<Vec> &corners, const std::vector<ThreediBoundingPlane> &planes, double tol) {
	for (const Vec &p : corners)
		for (const ThreediBoundingPlane &q : planes)
			if (q.normal[0] * p[0] + q.normal[1] * p[1] + q.normal[2] * p[2] + q.radius > tol) return false;
	return true;
}

bool same_points(const std::vector<Vec> &a, const std::vector<Vec> &b, double tol) {
	for (const Vec &p : a) {
		bool found = false;
		for (const Vec &q : b) found = found || near(p, q, tol);
		if (!found) return false;
	}
	for (const Vec &p : b) {
		bool found = false;
		for (const Vec &q : a) found = found || near(p, q, tol);
		if (!found) return false;
	}
	return true;
}

struct Section {
	int parent = 0;
	Vec offset{0, 0, 0};
	bool sphere = false;
	Vec med{0, 0, 0};
	double radius = 0;
	std::map<int, int> poly;
	int faces = 0;
	int ccw = 0;  // faces wound counter-clockwise about their stored normal
	double area = 0;
	Vec normal{0, 0, 0};
	struct Volume {
		int type, flags;
		Vec mn, mx;
		std::vector<Vec> corners;
		std::vector<ThreediBoundingPlane> planes;
		int seams;
	};
	std::vector<Volume> volumes;
};

std::vector<Section> sections(const Threedi3di3 &m) {
	std::vector<Section> out;
	if (m.collision == nullptr) return out;
	const ThreediCollisionModel &c = *m.collision;
	size_t v = 0, f = 0, vol = 0, pl = 0, nrm = 0;
	for (size_t o = 0; o < c.object_count; ++o) {
		const ThreediCollisionObject &co = c.objects[o];
		Section s;
		s.parent = co.parent_subobject_index;
		for (int k = 0; k < 3; ++k) s.offset[k] = co.offset[k] / 65536.0;
		s.sphere = co.num_vertices == 0 && co.num_bounding_volumes == 0 && co.min[0] <= co.max[0];
		for (int k = 0; k < 3; ++k) s.med[k] = co.med[k] / 65536.0;
		s.radius = co.radius / 65536.0;
		s.faces = co.num_faces;
		for (int k = 0; k < co.num_faces && f < c.face_count; ++k, ++f) {
			const ThreediCollisionFace &fc = c.faces[f];
			++s.poly[fc.poly_type];
			const float *p[3];
			for (int i = 0; i < 3; ++i) p[i] = c.vertices[v + fc.vert_index[i]].position;
			const Vec e{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
			const Vec g{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
			const Vec n{e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
			s.area += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			for (int k = 0; k < 3; ++k) s.normal[k] += 0.5 * n[k];
			if (nrm + fc.normal_index < c.normal_count) {
				const float *sn = c.normals[nrm + fc.normal_index].normal;
				if (n[0] * sn[0] + n[1] * sn[1] + n[2] * sn[2] > 0) ++s.ccw;
			}
		}
		v += static_cast<size_t>(co.num_vertices);
		nrm += static_cast<size_t>(std::max(0, co.num_normals));
		for (int k = 0; k < co.num_bounding_volumes && vol < c.volume_count; ++k, ++vol) {
			const ThreediBoundingVolume &bv = c.volumes[vol];
			Section::Volume x;
			x.type = bv.collidable_type;
			x.flags = bv.flags;
			x.mn = {bv.min_x_fp16 / 65536.0, bv.min_y_fp16 / 65536.0, bv.min_z_fp16 / 65536.0};
			x.mx = {bv.max_x_fp16 / 65536.0, bv.max_y_fp16 / 65536.0, bv.max_z_fp16 / 65536.0};
			const int count = static_cast<int>(std::min<size_t>(bv.plane_count, c.plane_count - pl));
			x.corners = polytope(c.planes + pl, count);
			x.planes.assign(c.planes + pl, c.planes + pl + count);
			x.seams = 0;
			for (int q = 0; q < count; ++q) x.seams += c.planes[pl + q].flags != 0;
			pl += static_cast<size_t>(std::max(0, bv.plane_count));
			s.volumes.push_back(x);
		}
		out.push_back(s);
	}
	return out;
}

void compare_collision(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	const std::vector<Section> sa = sections(a), sb = sections(b);
	if (sa.size() != sb.size()) {
		d.add("collision: " + std::to_string(sa.size()) + " vs " + std::to_string(sb.size()) + " sections");
		return;
	}
	for (size_t i = 0; i < sa.size(); ++i) {
		const Section &x = sa[i], &y = sb[i];
		const std::string w = "collision section " + std::to_string(i);
		if (x.parent != y.parent) d.add(w + ": parent " + std::to_string(x.parent) + " vs " + std::to_string(y.parent));
		if (!near(x.offset, y.offset, kPosTol)) d.add(w + ": offset " + vs(x.offset) + " vs " + vs(y.offset));
		if (x.sphere != y.sphere || (x.sphere && (!near(x.med, y.med, 1e-2) || std::fabs(x.radius - y.radius) > 1e-2)))
			d.add(w + ": hit sphere " + (x.sphere ? vs(x.med) + " r " + num(x.radius) : "none") + " vs " +
					(y.sphere ? vs(y.med) + " r " + num(y.radius) : "none"));
		if (x.faces != y.faces) d.add(w + ": " + std::to_string(x.faces) + " vs " + std::to_string(y.faces) + " bullet faces");
		if (x.poly != y.poly) {
			std::string px, py;
			for (const auto &kv : x.poly) px += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
			for (const auto &kv : y.poly) py += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
			d.add(w + ": face surfaces" + px + " vs" + py);
		}
		const double scale = std::max(1.0, x.area);
		if (std::fabs(x.area - y.area) > 1e-2 * scale) d.add(w + ": bullet-face area " + num(x.area) + " vs " + num(y.area));
		if (!near(x.normal, y.normal, 1e-2 * scale))
			d.add(w + ": bullet-face normal sum " + vs(x.normal) + " vs " + vs(y.normal));
		if (x.ccw != y.ccw)
			d.add(w + ": " + std::to_string(x.ccw) + " vs " + std::to_string(y.ccw) +
					" bullet faces wound counter-clockwise about their normal (winding)");
		std::vector<bool> used(y.volumes.size(), false);
		for (const Section::Volume &vx : x.volumes) {
			int match = -1;
			for (size_t j = 0; j < y.volumes.size() && match < 0; ++j) {
				const Section::Volume &vy = y.volumes[j];
				if (used[j] || vx.type != vy.type || vx.flags != vy.flags) continue;
				if (near(vx.mn, vy.mn, 1e-2) && near(vx.mx, vy.mx, 1e-2) && inside_all(vx.corners, vy.planes, 2e-2) &&
						inside_all(vy.corners, vx.planes, 2e-2))
					match = static_cast<int>(j);
			}
			if (match < 0) {
				d.add(w + ": volume type " + std::to_string(vx.type) + " flags " + std::to_string(vx.flags) + " box " +
						vs(vx.mn) + ".." + vs(vx.mx) + " has no match");
				continue;
			}
			used[match] = true;
			if (vx.seams != y.volumes[match].seams)
				d.info.push_back(w + ": volume type " + std::to_string(vx.type) + " seam planes " + std::to_string(vx.seams) +
						" vs " + std::to_string(y.volumes[match].seams));
		}
		for (size_t j = 0; j < y.volumes.size(); ++j)
			if (!used[j]) d.add(w + ": extra volume type " + std::to_string(y.volumes[j].type));
	}
}

void compare_occlusion(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	if (a.occlusion_object_count != b.occlusion_object_count) {
		d.add("occlusion: " + std::to_string(a.occlusion_object_count) + " vs " + std::to_string(b.occlusion_object_count) +
				" records");
		return;
	}
	size_t va = 0, vb = 0, pa = 0, pb = 0;
	for (size_t o = 0; o < a.occlusion_object_count; ++o) {
		const ThreediOcclusionObject &x = a.occlusion_objects[o], &y = b.occlusion_objects[o];
		const std::string w = "occlusion record " + std::to_string(o);
		// The connecting section means something to windows and portals only.
		const bool connects = x.type == 2 || x.type == 3;
		if (x.type != y.type || x.parent_subobject_index != y.parent_subobject_index ||
				(connects && x.connecting_subobject != y.connecting_subobject))
			d.add(w + ": type/sections " + std::to_string(x.type) + " " + std::to_string(x.parent_subobject_index) + "->" +
					std::to_string(x.connecting_subobject) + " vs " + std::to_string(y.type) + " " +
					std::to_string(y.parent_subobject_index) + "->" + std::to_string(y.connecting_subobject));
		if (x.face_count != y.face_count || x.num_vertices != y.num_vertices)
			d.add(w + ": " + std::to_string(x.num_vertices) + "/" + std::to_string(x.face_count) + " vs " +
					std::to_string(y.num_vertices) + "/" + std::to_string(y.face_count) + " vertices/faces");
		std::vector<Vec> px, py;
		for (int k = 0; k < x.num_vertices && va + k < a.occlusion_vertex_count; ++k)
			px.push_back(mission(a.occlusion_vertices[va + k].position));
		for (int k = 0; k < y.num_vertices && vb + k < b.occlusion_vertex_count; ++k)
			py.push_back(mission(b.occlusion_vertices[vb + k].position));
		if (!same_points(px, py, kPosTol)) d.add(w + ": vertices differ");
		// Planes as a set: (normal, d) within tolerance, order-insensitive.
		std::vector<std::array<double, 4>> nx, ny;
		for (int k = 0; k < x.num_planes && pa + k < a.occlusion_plane_count; ++k) {
			const Vec n = mission(a.occlusion_planes[pa + k].normal);
			nx.push_back({n[0], n[1], n[2], a.occlusion_planes[pa + k].radius});
		}
		for (int k = 0; k < y.num_planes && pb + k < b.occlusion_plane_count; ++k) {
			const Vec n = mission(b.occlusion_planes[pb + k].normal);
			ny.push_back({n[0], n[1], n[2], b.occlusion_planes[pb + k].radius});
		}
		const auto covered = [](const std::vector<std::array<double, 4>> &from, const std::vector<std::array<double, 4>> &in) {
			for (const auto &p : from) {
				bool found = false;
				for (const auto &q : in)
					found = found || (close(p[0], q[0], kDirTol) && close(p[1], q[1], kDirTol) &&
											 close(p[2], q[2], kDirTol) && close(p[3], q[3], kPosTol));
				if (!found) return false;
			}
			return true;
		};
		if (x.num_planes != y.num_planes || !covered(nx, ny) || !covered(ny, nx))
			d.add(w + ": planes differ (" + std::to_string(x.num_planes) + " vs " + std::to_string(y.num_planes) + ")");
		va += static_cast<size_t>(std::max(0, x.num_vertices));
		vb += static_cast<size_t>(std::max(0, y.num_vertices));
		pa += static_cast<size_t>(std::max(0, x.num_planes));
		pb += static_cast<size_t>(std::max(0, y.num_planes));
	}
}

void compare_lights(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	if (a.light_count != b.light_count) {
		d.add("lights: " + std::to_string(a.light_count) + " vs " + std::to_string(b.light_count));
		return;
	}
	for (size_t i = 0; i < a.light_count; ++i) {
		const ThreediLight &x = a.lights[i], &y = b.lights[i];
		const std::string w = "light " + std::to_string(i);
		if (x.subobj_index != y.subobj_index || x.style != y.style || x.phase != y.phase || x.rate != y.rate ||
				x.flags != y.flags || x.falloff_byte != y.falloff_byte ||
				std::memcmp(x.color_start, y.color_start, 3) != 0 || std::memcmp(x.color_end, y.color_end, 3) != 0)
			d.add(w + ": part/style/phase/rate/colours/flags differ");
		if (std::fabs(x.atten_start - y.atten_start) > 1e-3 || std::fabs(x.atten_end - y.atten_end) > 1e-3)
			d.add(w + ": attenuation " + num(x.atten_start) + ".." + num(x.atten_end) + " vs " + num(y.atten_start) + ".." +
					num(y.atten_end));
		if (!near(mission(x.offset), mission(y.offset), kPosTol))
			d.add(w + ": position " + vs(mission(x.offset)) + " vs " + vs(mission(y.offset)));
		if (!near(mission(x.rotation), mission(y.rotation), kDirTol) || std::fabs(x.rotation[3] - y.rotation[3]) > kDirTol)
			d.add(w + ": axis/cone differ");
	}
}

} // namespace

int cmd_compare(const char *expected_path, const char *actual_path) {
	Threedi3di3 a{}, b{};
	if (threedi_3di3_read(expected_path, &a) != 0 || threedi_3di3_read(actual_path, &b) != 0) {
		std::fprintf(stderr, "opennova-3di: cannot read %s or %s\n", expected_path, actual_path);
		threedi_3di3_free(&a);
		return 2;
	}
	Diff d;
	if (std::strcmp(a.header.name, b.header.name) != 0)
		d.add(std::string("model name ") + a.header.name + " vs " + b.header.name);
	if (a.header.mesh_type != b.header.mesh_type)
		d.add("mesh type " + std::to_string(a.header.mesh_type) + " vs " + std::to_string(b.header.mesh_type));
	std::set<std::string> ra, rb;
	for (uint32_t i = 0; i < a.ctrl.count; ++i) ra.insert(a.ctrl.registers[i].name);
	for (uint32_t i = 0; i < b.ctrl.count; ++i) rb.insert(b.ctrl.registers[i].name);
	for (const std::string &r : ra)
		if (!rb.count(r)) d.add("register " + r + ": missing");
	for (const std::string &r : rb)
		if (!ra.count(r)) d.add("register " + r + ": extra");
	if (a.lod_count != b.lod_count) d.add("lods " + std::to_string(a.lod_count) + " vs " + std::to_string(b.lod_count));
	for (size_t li = 0; li < std::min(a.lod_count, b.lod_count); ++li) {
		const ThreediLod &x = a.lods[li], &y = b.lods[li];
		const std::string w = "lod " + std::to_string(li);
		if (std::strcmp(x.model_type, y.model_type) != 0 || x.lod_threshold != y.lod_threshold)
			d.add(w + ": " + x.model_type + " " + std::to_string(x.lod_threshold) + " vs " + y.model_type + " " +
					std::to_string(y.lod_threshold));
		if (x.render_object_count != y.render_object_count) {
			d.add(w + ": " + std::to_string(x.render_object_count) + " vs " + std::to_string(y.render_object_count) + " parts");
			continue;
		}
		for (size_t p = 0; p < x.render_object_count; ++p) {
			const ThreediRenderObject &px = x.render_objects[p], &py = y.render_objects[p];
			if (px.parent_index != py.parent_index)
				d.add(w + " part " + std::to_string(p) + ": parent " + std::to_string(px.parent_index) + " vs " +
						std::to_string(py.parent_index));
			if (!near(mission(px.abs), mission(py.abs), kPosTol))
				d.add(w + " part " + std::to_string(p) + ": pivot " + vs(mission(px.abs)) + " vs " + vs(mission(py.abs)));
		}
		compare_geometry(d, w, lod_geometry(a, x), lod_geometry(b, y));
		compare_panm(d, w, a, x, b, y);
	}
	// Materials are compared through the geometry that draws with them;
	// materials no strip draws (the exporter drops them) are not compared.
	if (a.user_point_count != b.user_point_count)
		d.add("user points " + std::to_string(a.user_point_count) + " vs " + std::to_string(b.user_point_count));
	for (size_t i = 0; i < std::min(a.user_point_count, b.user_point_count); ++i) {
		const ThreediUserPoint &x = a.user_points[i], &y = b.user_points[i];
		const Vec px{x.x / 65536.0, x.y / 65536.0, x.z / 65536.0}, py{y.x / 65536.0, y.y / 65536.0, y.z / 65536.0};
		const Vec dx{x.rot_x / 65536.0, x.rot_y / 65536.0, x.rot_z / 65536.0};
		const Vec dy{y.rot_x / 65536.0, y.rot_y / 65536.0, y.rot_z / 65536.0};
		if (std::strcmp(x.name, y.name) != 0 || x.userpoint_type != y.userpoint_type ||
				x.subobject_index != y.subobject_index || !near(px, py, kPosTol) || !near(dx, dy, kDirTol))
			d.add("user point " + std::to_string(i) + ": " + x.name + " " + std::to_string(x.userpoint_type) + " part " +
					std::to_string(x.subobject_index) + " " + vs(px) + " vs " + y.name + " " +
					std::to_string(y.userpoint_type) + " part " + std::to_string(y.subobject_index) + " " + vs(py));
	}
	compare_lights(d, a, b);
	compare_occlusion(d, a, b);
	compare_collision(d, a, b);
	for (const std::string &s : d.info) std::printf("info: %s\n", s.c_str());
	for (const std::string &s : d.lines) std::printf("%s\n", s.c_str());
	std::printf("%s: %s\n", d.lines.empty() ? "same model" : "different", actual_path);
	threedi_3di3_free(&a);
	threedi_3di3_free(&b);
	return d.lines.empty() ? 0 : 1;
}

} // namespace threedi_cli
