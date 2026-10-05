// opennova-3di compare: tell whether two .3di files hold the same model.
//
// The acceptance question (ADR 0047) is the one a modder asks of a retail
// model imported into Blender and exported again: strip layout and precision
// aside, does it mean the same thing to the game? So every check lands in one
// of two tiers, chosen per check below with the reason:
//
//   DIFFERENT (a plain line, exit 1): anything that changes what the model
//   means or how the runtime reads it beyond float and quantization noise.
//   LOD types and thresholds; parts (parent, pivot, rel offset, and the
//   bound sphere of a model without GHDR) and the GHDR radius; per part and
//   material, the triangles as oriented corners (position, normal, UVs, the
//   skin blend: retail's four influences per part, normalized) and the vertex
//   layout; materials; registers; PANM rows in row order (the part,
//   parent, flags, tracks and rotation frame of each); user points; lights;
//   occlusion records (sphere, vertices, faces with their plane, planes);
//   collision: the CMDL, the CXLT rows, and per section its parent, offset,
//   box, midpoint and radius, its bullet faces (corners, stored normal,
//   plane distance, dominant axis, face box, surface, flags) and its volumes
//   (the solid their planes bound, box, type and flags, a ladder's facing).
//
//   DRIFT (a `drift:` line per category with the count and the worst value,
//   exit 0; exit 1 under --strict): a value our builder derives by a
//   heuristic of ours where retail's tool is unwitnessed (tangent and
//   bitangent values, volume seam flags); a weight on a bone slot past its
//   strip's bone table (it names no part); a zero-length vertex normal (it has
//   no direction to keep) given one; the dominant axis of a diagonal bullet
//   face (either axis projects it); a part's bound sphere beside a GHDR
//   radius (nothing reads it); and any value above that moved by more
//   than float noise but by no more than its DIFFERENT tolerance (the storage
//   noise of a Blender round trip; one 8.8, Q14 or 16.16 step after
//   truncation).
//
// Not compared, because an authoring round trip legitimately changes it and
// the runtime reads it through something compared above: strip layout and
// bounds, vertex order and sharing, material, register, bone-table and volume
// order, a triangle's starting corner, the occlusion edge words (indices into
// the record's vertex order), the order of a volume's planes (but a ladder's
// plane 0), and materials no strip draws. Not compared because the JO runtime
// never reads it: LGHT view_proj, PANM matrix_offset and bind_matrix_index,
// and the PANM rows of a LOD none of whose rows animates (the loader keeps no
// table for it: panm_table_kept).
//
// Exit 0 same model (drift notes allowed unless --strict), 1 different, or a
// file that cannot be read or is malformed (an index outside its table).

#include <algorithm>
#include <array>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <unordered_map>
#include <vector>

#include <formats/threedi/threedi_3di3.h>
#include <formats/threedi/threedi_build.h>
#include <formats/threedi/threedi_panm.h>
#include <formats/threedi/threedi_strip_decode.h>

#include "threedi_cli.h"

using namespace opennova::threedi;

namespace opennova::threedi_cli {

namespace {

// Tolerances, calibrated on scene -> build over the 958 JO models (exact for
// everything `scene` carries: it prints floats %.9g and 16.16 values exactly)
// and on Blender 5.1 round trips (import, then export) of 46 retail models
// (Armry01, Dblkhwk1, Dtruck2, US01, ArmsG, Mp5b_1st and 40 more across the
// size range). The DIFFERENT tolerances sit above what that round trip moves
// and below anything a player could see; the noise floors keep plain float
// noise out of DRIFT.
//
// Render and occlusion vertices and UVs are float32 on disk: the round trip
// moves them by at most 1e-5 m and 2e-6.
constexpr double kPosTol = 1e-4, kPosNoise = 1e-5;  // metres
constexpr double kUvTol = 1e-4, kUvNoise = 1e-6;
constexpr double kWeightTol = 1e-4, kWeightNoise = 1e-6;
// Placements Blender composes through object and bone matrices (part pivots
// and rel offsets, user points, lights) and the part spheres derived from
// vertices: the round trip moves them by up to 1.3e-4 m (Mp5b_1st's rig).
constexpr double kPlaceTol = 1e-3, kPlaceNoise = 1e-5;
// Directions and rotation frames (user points, light axes, MTRX rows,
// occlusion planes): up to 5.5e-4 per component.
constexpr double kDirTol = 2e-3, kDirNoise = 1e-5;
// Vertex normals, by angle. Blender's custom-normal storage moves 99.65% of
// 188,000 retail vertex normals by at most 0.05 degrees and the rest by at
// most 1.8, with none between 2 and 5 degrees: past 5 (72 corners) a normal
// was clamped or replaced, which lighting shows. 2 degrees changes diffuse
// light by under 3.5%.
constexpr double kNormalTolDeg = 2.0, kNormalNoiseDeg = 1e-3;
constexpr double kNormalLengthTol = 1e-3;
// A face normal the builder derives from corners (bullet faces, volume and
// occlusion planes) is only as exact as they define it: at least 0.05
// degrees (a few Q14 steps), and for a bullet face one 8.8 step per corner
// across its smallest altitude (Section::faces).
constexpr double kFaceNormalTolDeg = 0.05;
// Tangents and bitangents: a scene carries the stored frames (`vt`), but an
// author's tool derives its own (Blender's MikkTSpace) and a mesh without
// frames takes the OED rule's (docs/threedi/o3d-scene-format.md); retail's
// tool is unwitnessed, so their values are DRIFT only, above what
// renormalizing float noise moves.
constexpr double kTangentNoise = 1e-4;
// Quantized data: the collision block (8.8 CVRT corners, Q14 CNRM and BPLN
// normals, 16.16 planes, boxes, offsets and radii), the CXLT rows and the
// GHDR radius. One step of the grid a value is derived on is storage noise (a
// round trip lands a corner a hair under its grid line and truncation moves
// it a whole step); more is DIFFERENT. Any nonzero difference within that is
// DRIFT.
constexpr double kQ8 = 1.0 / 256.0;
constexpr double kQ14 = 1.0 / 16384.0;
constexpr double kQ16 = 1.0 / 65536.0;
constexpr double kQuantumTol = kQ8 + kQ16;  // one 8.8 step plus the 16.16 truncation
constexpr double kQuantumNoise = 0.5 * kQ16;
// A distance over 8.8 corners (a section radius, the CMDL radii, a face box
// taken from the unquantized corners): each corner may sit one step off.
constexpr double kQuantumDistTol = 2.0 * kQ8 + kQ16;
// A bullet face's plane is compared as the runtime's side test reads it,
// n . p + plane_dist at the face's own corners (collision_query.cpp), within
// the precision of its storage, per face (Section::faces): each side took its
// distance at a corner up to one 8.8 step off the stored one (2 |n|1 / 256
// between them: 7.8e-3 m for an axis-aligned face, 1.35e-2 at a diagonal)
// and with a normal up to one Q14 step per axis off over the corner's lever
// arm (retail took it with the unquantized normal: 8.6e-3 m at 75 m, Odock3).
//
// A volume's solid: each corner of one lies inside the other's planes within
// one step (the round trip measures 3.3e-3 at most).
constexpr double kVolumeTol = kQ8;
// A volume's box: its corners are placements (the importer rebuilds them from
// the stored planes, and Blender composes them through the part's matrix), so
// one truncation step can flip on top of a placement's noise (APLFP1's two CB
// boxes after an import and export: 3.9978e-3 m).
constexpr double kVolumeBoxTol = kQuantumTol + kPlaceTol;

// ---------------------------------------------------------------------------
// The two tiers.

struct Drift {
	long count = 0;
	double worst = 0.0;
	std::string where;  // the owner of the worst value
};

struct Diff {
	std::vector<std::string> lines;      // DIFFERENT
	std::map<std::string, Drift> drift;  // DRIFT, by category
	std::vector<std::string> malformed;  // an index outside its table
	void add(const std::string &s) { lines.push_back(s); }
	void note(const std::string &category, double delta, const std::string &where, long count = 1) {
		Drift &x = drift[category];
		if (x.count == 0 || delta > x.worst) {
			x.worst = delta;
			x.where = where;
		}
		x.count += count;
	}
};

std::string num(double v) {
	char buf[32];
	std::snprintf(buf, sizeof(buf), "%.5g", v == 0.0 ? 0.0 : v);
	return buf;
}

using Vec = std::array<double, 3>;

Vec mission(const float *model) {
	const ThreediBuildVec3 m = threedi_build_to_mission(ThreediBuildVec3{model[0], model[1], model[2]});
	return {m.x, m.y, m.z};
}

// |a - b|, where a NaN matches a NaN (retail ships them in normals, planes and
// frames) and nothing else.
double gap(double a, double b) {
	const bool na = std::isnan(a), nb = std::isnan(b);
	if (na || nb) return na && nb ? 0.0 : std::numeric_limits<double>::infinity();
	return std::fabs(a - b);
}

template <size_t N>
double gap(const std::array<double, N> &a, const std::array<double, N> &b) {
	double worst = 0.0;
	for (size_t k = 0; k < N; ++k) worst = std::max(worst, gap(a[k], b[k]));
	return worst;
}

// One value in two tiers: false (DIFFERENT) beyond `tol`, a DRIFT note under
// `category` when it moved by more than `noise`.
bool within(Diff &d, const std::string &category, const std::string &where, double delta, double tol, double noise) {
	if (!(delta <= tol)) return false;
	if (delta > noise) d.note(category, delta, where);
	return true;
}

std::string vs(const Vec &v) { return "(" + num(v[0]) + " " + num(v[1]) + " " + num(v[2]) + ")"; }

Vec q16v(const int32_t *v) { return {v[0] * kQ16, v[1] * kQ16, v[2] * kQ16}; }

// A register by its index in the model's table: its name, or #index past the table.
std::string register_label(const Threedi3di3 &m, int reg) {
	if (reg >= 0 && static_cast<uint32_t>(reg) < m.ctrl.count) return m.ctrl.registers[reg].name;
	return "#" + std::to_string(reg);
}

std::string reg_name(const Threedi3di3 &m, int style, int reg) {
	return threedi_generator_names_register(style) ? register_label(m, reg) : "-";
}

// Everything a material means, as one comparable string (generator rates and
// phases to five significant digits; colours as the bytes they are authored).
std::string material_key(const Threedi3di3 &m, const ThreediMaterial &mt) {
	std::string k = std::string(mt.shader_name) + " flags " + std::to_string(mt.material_flags);
	if (mt.material_flags & THREEDI_MATERIAL_FLAG_ALPHA_TEST) k += " at " + std::to_string(mt.alpha_test_value_byte);
	k += " glass " + std::to_string(mt.is_glass) + " emissive " + std::to_string(mt.emissive_type);
	k += " reflect";
	for (int c = 0; c < 4; ++c) k += " " + std::to_string(threedi_build_byte_of(mt.reflect_color[c]));
	for (uint32_t t = 0; t < mt.texture_count && t < 24; ++t) {
		const ThreediMaterialTexture &x = mt.textures[t];
		k += std::string(" [") + x.name + " " + std::to_string(x.slot) + " " + std::to_string(x.type) + " " +
				std::to_string(x.flags) + " " + std::to_string(x.frame) + "]";
	}
	const ThreediTexAnim &a = mt.animation;
	if (a.num_frames || a.animation_type || a.cycle_frame_time)
		k += " anim " + std::to_string(a.num_frames) + "/" + std::to_string(a.animation_type) + "/" +
				(threedi_flipbook_reads_register(a) ? register_label(m, a.cycle_frame_time)
												   : std::to_string(a.cycle_frame_time));
	const ThreediRgbGen &g = mt.rgb_gen;
	if (g.style) {
		k += " rgb " + std::to_string(g.style) + " " + reg_name(m, g.style, g.reg) + " " + num(g.rate) + " " + num(g.phase);
		for (int c = 0; c < 3; ++c) k += " " + std::to_string(threedi_build_byte_of(g.start_color[c]));
		for (int c = 0; c < 3; ++c) k += " " + std::to_string(threedi_build_byte_of(g.end_color[c]));
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

// ---------------------------------------------------------------------------
// Faces: render triangles, bullet faces and occlusion faces share one record
// and one matcher.

struct Corner {
	Vec position{}, normal{};
	std::array<double, 4> uv{};
	// The resolved skin blend (skin_blend): part -> weight, sorted by part so
	// the strip bone-table order does not matter (INT_MAX marks an unused
	// entry).
	std::array<int, 4> bone{{INT_MAX, INT_MAX, INT_MAX, INT_MAX}};
	std::array<double, 4> weight{};
	double stray = 0.0;               // the weight on slots past the strip's bone table (DRIFT only)
	std::array<double, 6> tangent{};  // tangent then bitangent (DRIFT only)
	bool tangents = false;
};

struct Face {
	std::array<Corner, 3> corner;
	double plane = 0.0;           // bullet face: the stored plane distance; occlusion face: its plane's d
	int axis = 0;                 // bullet face: the stored normal's dominant axis
	std::array<double, 6> box{};  // bullet face: the stored box (min, max)
	// Bullet face: how far its stored normal (beyond the normal tolerance) and
	// its plane may sit, from the precision its corners give them.
	double normal_slack = 0.0, plane_slack = 0.0;
};

struct Tolerance {
	double pos, normal_deg, uv, weight;
	double plane;           // < 0: the faces carry no plane
	bool zero_normal_free;  // an expected zero-length normal matches any normal
};

// A skinned vertex's blend as the renderer draws it, normalized for
// comparison: its four influences as retail's shader blends them
// (threedi_skin_influences: slot 3 takes 1 - (w0 + w1 + w2)), summed per
// part, zero weights and the hair of negative remainder retail's
// four-decimal weights leave (they sum to 1.0001 in ArmGlovD) left out, and
// divided by the total so the blend sums to 1. Slot order, bone-table order
// and one part's weight split over several slots then no longer matter, only
// each part's share of the vertex. A slot past its strip's bone table names
// no part (retail FSldr03 weights slot 255, which reads whatever the palette
// constant last held): its weight is set apart as `stray` and the parts'
// shares are taken without it, so a scene that cannot name it compares by
// the rest, and the stray weight itself is DRIFT.
void skin_blend(Corner &c, const ThreediVertex &v, const ThreediTriangleStrip &st) {
	ThreediSkinInfluence influences[4];
	threedi_skin_influences(&v, st.bone_table, st.bone_table_length, influences);
	double total = 0.0;
	for (const ThreediSkinInfluence &x : influences) {
		if (std::isnan(x.weight)) {
			// A weight that is no number matches only the same.
			c.bone = {{-1, INT_MAX, INT_MAX, INT_MAX}};
			c.weight = {{std::numeric_limits<double>::quiet_NaN(), 0.0, 0.0, 0.0}};
			return;
		}
		if (!(x.weight > 0.0f)) continue;
		if (x.part < 0) {
			c.stray += x.weight;
			continue;
		}
		const int part = x.part;
		for (int k = 0; k < 4; ++k)
			if (c.bone[k] == part || c.bone[k] == INT_MAX) {
				c.bone[k] = part;
				c.weight[k] += x.weight;
				break;
			}
		total += x.weight;
	}
	if (total > 0.0)
		for (int k = 0; k < 4 && c.bone[k] != INT_MAX; ++k) c.weight[k] /= total;
	for (int i = 1; i < 4; ++i)
		for (int k = i; k > 0 && c.bone[k] < c.bone[k - 1]; --k) {
			std::swap(c.bone[k], c.bone[k - 1]);
			std::swap(c.weight[k], c.weight[k - 1]);
		}
}

// The largest share of the vertex a part takes in one blend and not the
// other, a part a blend lacks weighing 0 there: a sliver of weight an
// importer dropped is a sliver, not another bone set.
double weight_gap(const Corner &a, const Corner &b) {
	double worst = 0.0;
	size_t i = 0, j = 0;
	while (i < 4 || j < 4) {
		const int pa = i < 4 ? a.bone[i] : INT_MAX, pb = j < 4 ? b.bone[j] : INT_MAX;
		if (pa == INT_MAX && pb == INT_MAX) break;
		if (pa == pb)
			worst = std::max(worst, gap(a.weight[i++], b.weight[j++]));
		else if (pa < pb)
			worst = std::max(worst, gap(a.weight[i++], 0.0));
		else
			worst = std::max(worst, gap(0.0, b.weight[j++]));
	}
	return worst;
}

double length(const Vec &v) { return std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]); }

bool has_nan(const Vec &v) { return std::isnan(v[0]) || std::isnan(v[1]) || std::isnan(v[2]); }

bool zero_length(const Vec &v) { return !has_nan(v) && length(v) < 1e-6; }

constexpr double kPi = 3.14159265358979323846;

// The angle between two stored normals in degrees (atan2 keeps small angles
// exact). A NaN matches the same NaN (retail ships them), two zero-length
// normals match, and a zero-length normal is infinitely far from a real one.
double normal_gap(const Vec &a, const Vec &b) {
	const double inf = std::numeric_limits<double>::infinity();
	if (has_nan(a) || has_nan(b)) return gap(a, b) == 0.0 ? 0.0 : inf;
	const double la = length(a), lb = length(b);
	if (la < 1e-6 || lb < 1e-6) return la < 1e-6 && lb < 1e-6 ? 0.0 : inf;
	const Vec c{a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0]};
	return std::atan2(length(c), a[0] * b[0] + a[1] * b[1] + a[2] * b[2]) * 180.0 / kPi;
}

// Direction within the angle and length within kNormalLengthTol (the
// renderer lights with the stored vector as it is).
bool same_normal(const Vec &a, const Vec &b, const Tolerance &t) {
	if (t.zero_normal_free && zero_length(a) && !has_nan(b)) return true;
	if (!(normal_gap(a, b) <= t.normal_deg)) return false;
	return has_nan(a) || gap(length(a), length(b)) <= kNormalLengthTol;
}

bool same_corner(const Corner &a, const Corner &b, const Tolerance &t) {
	return gap(a.position, b.position) <= t.pos && same_normal(a.normal, b.normal, t) && gap(a.uv, b.uv) <= t.uv &&
			weight_gap(a, b) <= t.weight;
}

// How far apart two faces' planes lie at `a`'s corners, n . p + d with each
// face's own normal and distance: the stored distances' difference when the
// normals agree, and what the runtime's side test sees when a re-derived
// normal tilts by a hair.
double plane_gap(const Face &a, const Face &b) {
	const Vec &na = a.corner[0].normal, &nb = b.corner[0].normal;
	double worst = 0.0;
	for (int k = 0; k < 3; ++k) {
		const Vec &p = a.corner[k].position;
		const double sa = na[0] * p[0] + na[1] * p[1] + na[2] * p[2] + a.plane;
		const double sb = nb[0] * p[0] + nb[1] * p[1] + nb[2] * p[2] + b.plane;
		worst = std::max(worst, gap(sa, sb));
	}
	return worst;
}

// CNRM's dominant-axis codes (the builder's WriteCNRM port): 1 z, 2 y, 4 x.
int axis_component(int code) { return code == 1 ? 2 : code == 2 ? 1 : code == 4 ? 0 : -1; }

// The runtime projects a bullet face on its dominant axis for the inside
// test (collision_query.cpp); any axis the face does not stand edge-on to
// gives the same answer, so two codes agree when each normal is diagonal
// between their axes (within the normal tolerance).
bool same_axis(const Face &a, const Face &b, double normal_deg) {
	if (a.axis == b.axis) return true;
	const int ia = axis_component(a.axis), ib = axis_component(b.axis);
	if (ia < 0 || ib < 0) return false;
	const double tie = 2.0 * std::sin(std::min(90.0, normal_deg) * kPi / 180.0);
	const Vec &na = a.corner[0].normal, &nb = b.corner[0].normal;
	return std::fabs(std::fabs(na[ia]) - std::fabs(na[ib])) <= tie && std::fabs(std::fabs(nb[ia]) - std::fabs(nb[ib])) <= tie;
}

// The tolerances for matching expected face `a`: a bullet face's own slack
// on top.
Tolerance face_tolerance(const Tolerance &t, const Face &a) {
	Tolerance u = t;
	u.normal_deg = std::max(t.normal_deg, a.normal_slack);
	if (t.plane >= 0.0) u.plane = t.plane + a.plane_slack;
	return u;
}

// The corner rotation under which `b` matches `a`, or -1. Cyclic rotations
// keep the winding; swapping two corners does not.
int face_shift(const Face &a, const Face &b, const Tolerance &t) {
	const Tolerance u = face_tolerance(t, a);
	if (u.plane >= 0.0 && (!same_axis(a, b, u.normal_deg) || !(plane_gap(a, b) <= u.plane))) return -1;
	for (int shift = 0; shift < 3; ++shift) {
		bool same = true;
		for (int k = 0; k < 3 && same; ++k) same = same_corner(a.corner[k], b.corner[(k + shift) % 3], u);
		if (same) return shift;
	}
	return -1;
}

// How far apart two matching faces are (the corners paired by `shift`): the
// matcher pairs the closest first, so a face finds its exact copy.
double face_cost(const Face &a, const Face &b, int shift, const Tolerance &t) {
	double cost = t.plane >= 0.0 ? plane_gap(a, b) : 0.0;
	for (int k = 0; k < 3; ++k) {
		const Corner &x = a.corner[k], &y = b.corner[(k + shift) % 3];
		cost = std::max(cost, gap(x.position, y.position));
		const double angle = normal_gap(x.normal, y.normal);
		cost += 1e-6 * (std::isinf(angle) ? 180.0 : angle) + gap(x.uv, y.uv) + (std::isinf(weight_gap(x, y)) ? 0.0 : weight_gap(x, y));
	}
	return cost;
}

Vec centre(const Face &f) {
	Vec c{};
	for (int k = 0; k < 3; ++k) c[k] = (f.corner[0].position[k] + f.corner[1].position[k] + f.corner[2].position[k]) / 3.0;
	return c;
}

void put(std::string &s, double v) { s.append(reinterpret_cast<const char *>(&v), sizeof(v)); }

std::string corner_bytes(const Corner &c) {
	std::string s;
	for (double v : c.position) put(s, v);
	for (double v : c.normal) put(s, v);
	for (double v : c.uv) put(s, v);
	for (int k = 0; k < 4; ++k) {
		put(s, c.bone[k]);
		put(s, c.weight[k]);
	}
	for (double v : c.tangent) put(s, v);
	return s;
}

// Exactly equal faces (any starting corner) share a key.
std::string face_key(const Face &f) {
	const std::string c[3] = {corner_bytes(f.corner[0]), corner_bytes(f.corner[1]), corner_bytes(f.corner[2])};
	int first = 0;
	for (int k = 1; k < 3; ++k)
		if (c[k] < c[first]) first = k;
	std::string key = c[first] + c[(first + 1) % 3] + c[(first + 2) % 3];
	put(key, f.plane);
	put(key, f.axis);
	for (double v : f.box) put(key, v);
	return key;
}

// Faces that are exactly equal collapse into one class with a multiplicity.
struct Classes {
	std::vector<size_t> rep;
	std::vector<long> size;
};

Classes classes_of(const std::vector<Face> &faces) {
	Classes c;
	std::unordered_map<std::string, size_t> index;
	for (size_t i = 0; i < faces.size(); ++i) {
		const auto it = index.emplace(face_key(faces[i]), c.rep.size());
		if (it.second) {
			c.rep.push_back(i);
			c.size.push_back(0);
		}
		++c.size[it.first->second];
	}
	return c;
}

using Cell = std::array<int64_t, 3>;

struct CellHash {
	size_t operator()(const Cell &c) const {
		return static_cast<size_t>(static_cast<uint64_t>(c[0]) * 73856093u ^ static_cast<uint64_t>(c[1]) * 19349663u ^
				static_cast<uint64_t>(c[2]) * 83492791u);
	}
};

Cell cell_of(const Vec &p, double size) {
	Cell c{};
	for (int k = 0; k < 3; ++k) {
		const double x = p[k] / size;
		// A NaN centre (a NaN corner) gets a cell of its own kind.
		c[k] = std::isnan(x) ? INT64_MIN / 2 : static_cast<int64_t>(std::floor(std::max(-1e15, std::min(1e15, x))));
	}
	return c;
}

struct Pairing {
	size_t a, b;  // face indices
	int shift;    // b's corner (k + shift) % 3 pairs with a's corner k
	long count;   // exact copies paired this way
};

// Why the nearest face of `b` to `f` is not its match: the attributes past
// their tolerance.
std::string explain(const Face &f, const std::vector<Face> &b, const Tolerance &face_t) {
	const Tolerance t = face_tolerance(face_t, f);
	const Vec c = centre(f);
	size_t best = b.size();
	double best_d = std::numeric_limits<double>::infinity();
	for (size_t j = 0; j < b.size(); ++j) {
		const double d = gap(c, centre(b[j]));
		if (d < best_d) {
			best_d = d;
			best = j;
		}
	}
	std::string s = "e.g. the one centred at " + vs(c);
	if (best == b.size()) return s;
	double worst[5] = {std::numeric_limits<double>::infinity(), 0, 0, 0, 0};
	for (int shift = 0; shift < 3; ++shift) {
		double g[5] = {0, 0, 0, 0, 0};
		for (int k = 0; k < 3; ++k) {
			const Corner &x = f.corner[k], &y = b[best].corner[(k + shift) % 3];
			g[0] = std::max(g[0], gap(x.position, y.position));
			g[1] = std::max(g[1], same_normal(x.normal, y.normal, t) ? 0.0 : normal_gap(x.normal, y.normal));
			g[2] = std::max(g[2], gap(x.uv, y.uv));
			g[3] = std::max(g[3], weight_gap(x, y));
		}
		if (g[0] < worst[0]) std::copy(g, g + 5, worst);
	}
	worst[4] = t.plane >= 0.0 ? plane_gap(f, b[best]) : 0.0;
	const char *names[5] = {"corner position (m)", "normal (degrees)", "UV", "skin weight", "plane (m)"};
	const double tols[5] = {t.pos, t.normal_deg, t.uv, t.weight, std::max(0.0, t.plane)};
	std::string why;
	for (int k = 0; k < 5; ++k)
		if (!(worst[k] <= tols[k])) why += std::string(why.empty() ? "" : ", ") + names[k] + " by " + num(worst[k]);
	if (t.plane >= 0.0 && !same_axis(f, b[best], t.normal_deg)) why += std::string(why.empty() ? "" : ", ") + "dominant axis";
	return s + (why.empty() ? std::string(": its nearest counterpart is taken") : ": the nearest one differs in " + why);
}

// Pair every face of `a` with its own face of `b` under `t`: a perfect
// matching, since nearly coincident faces can match more than one candidate
// and a greedy choice could steal the only candidate of a later face (retail
// fx_med1 otherwise fails even against itself). Exact copies collapse into a
// class with a multiplicity first and candidates come from a grid on the face
// centre, so a flat grid of 20,000 faces or 2,000 stacked copies stay fast;
// the augmenting search is an explicit stack. False with the reason in `why`
// when no perfect matching exists.
bool match_faces(const std::vector<Face> &a, const std::vector<Face> &b, const Tolerance &t,
		std::vector<Pairing> &pairs, std::string &why) {
	pairs.clear();
	if (a.size() != b.size()) {
		why = std::to_string(a.size()) + " vs " + std::to_string(b.size()) + " faces";
		return false;
	}
	const Classes ca = classes_of(a), cb = classes_of(b);
	// A matching centre lies within t.pos per axis: the neighbouring cells hold it.
	const double size = std::max(2.0 * t.pos, 1e-6);
	std::unordered_map<Cell, std::vector<size_t>, CellHash> grid;
	for (size_t j = 0; j < cb.rep.size(); ++j) grid[cell_of(centre(b[cb.rep[j]]), size)].push_back(j);
	struct Edge {
		size_t from, to;
		int shift;
		long flow;
		double cost;
	};
	std::vector<Edge> edges;
	std::vector<std::vector<size_t>> out(ca.rep.size()), in(cb.rep.size());
	long lonely = 0;
	size_t first_lonely = a.size();
	for (size_t i = 0; i < ca.rep.size(); ++i) {
		const Face &f = a[ca.rep[i]];
		const Cell c = cell_of(centre(f), size);
		for (int dx = -1; dx <= 1; ++dx)
			for (int dy = -1; dy <= 1; ++dy)
				for (int dz = -1; dz <= 1; ++dz) {
					const auto it = grid.find(Cell{c[0] + dx, c[1] + dy, c[2] + dz});
					if (it == grid.end()) continue;
					for (size_t j : it->second) {
						const int shift = face_shift(f, b[cb.rep[j]], t);
						if (shift < 0) continue;
						out[i].push_back(edges.size());
						in[j].push_back(edges.size());
						edges.push_back(Edge{i, j, shift, 0, face_cost(f, b[cb.rep[j]], shift, t)});
					}
				}
		if (out[i].empty()) {
			lonely += ca.size[i];
			if (first_lonely == a.size()) first_lonely = ca.rep[i];
		}
	}
	if (lonely > 0) {
		why = std::to_string(lonely) + " of " + std::to_string(a.size()) + " faces have no counterpart, " +
				explain(a[first_lonely], b, t);
		return false;
	}
	// Push one unit per face through class -> class edges (capacity: the
	// target class's size): the closest pairs first, then augmenting along
	// alternating paths (closest candidates first) for what is left.
	std::vector<long> used(cb.rep.size(), 0), left(ca.size);
	std::vector<size_t> order(edges.size());
	for (size_t e = 0; e < edges.size(); ++e) order[e] = e;
	std::stable_sort(order.begin(), order.end(), [&](size_t x, size_t y) { return edges[x].cost < edges[y].cost; });
	for (size_t e : order) {
		Edge &edge = edges[e];
		const long units = std::min(left[edge.from], cb.size[edge.to] - used[edge.to]);
		edge.flow += units;
		left[edge.from] -= units;
		used[edge.to] += units;
	}
	for (auto &list : out)
		std::stable_sort(list.begin(), list.end(), [&](size_t x, size_t y) { return edges[x].cost < edges[y].cost; });
	std::vector<size_t> seen_a(ca.rep.size(), 0), seen_b(cb.rep.size(), 0);
	std::vector<size_t> reached_by(cb.rep.size(), 0), cancelled(ca.rep.size(), 0), stack;
	size_t stamp = 0;
	for (size_t i = 0; i < ca.rep.size(); ++i)
		for (; left[i] > 0; --left[i]) {
			++stamp;
			seen_a[i] = stamp;
			stack.assign(1, i);
			size_t found = cb.rep.size();
			while (!stack.empty() && found == cb.rep.size()) {
				const size_t u = stack.back();
				stack.pop_back();
				for (size_t e : out[u]) {
					const size_t v = edges[e].to;
					if (seen_b[v] == stamp) continue;
					seen_b[v] = stamp;
					reached_by[v] = e;
					if (used[v] < cb.size[v]) {
						found = v;
						break;
					}
					for (size_t r : in[v]) {
						const size_t w = edges[r].from;
						if (edges[r].flow > 0 && seen_a[w] != stamp) {
							seen_a[w] = stamp;
							cancelled[w] = r;
							stack.push_back(w);
						}
					}
				}
			}
			if (found == cb.rep.size()) {
				why = "the faces pair up, but not one to one (copies of a face differ in number)";
				return false;
			}
			++used[found];
			for (size_t v = found;;) {
				const size_t e = reached_by[v];
				++edges[e].flow;
				if (edges[e].from == i) break;
				const size_t r = cancelled[edges[e].from];
				--edges[r].flow;
				v = edges[r].to;
			}
		}
	for (const Edge &e : edges)
		if (e.flow > 0) pairs.push_back(Pairing{ca.rep[e.from], cb.rep[e.to], e.shift, e.flow});
	return true;
}

// ---------------------------------------------------------------------------
// Render geometry.

// A part's geometry under one material. The corner records establish
// equivalence; the aggregates only explain a difference (means alone cannot
// establish one).
struct Geometry {
	std::vector<Face> faces;
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
std::map<std::string, Geometry> lod_geometry(Diff &d, const std::string &where, const Threedi3di3 &m,
		const ThreediLod &lod) {
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
			if (!threedi_decode_strip_indices(lod, st, tris)) {
				d.malformed.push_back(where + " strip " + std::to_string(cursor) + ": indices outside its vertex window");
				continue;
			}
			for (size_t t = 0; t + 2 < tris.size(); t += 3) {
				const ThreediVertex *v[3];
				Vec p3[3];
				Face face;
				for (int k = 0; k < 3; ++k) {
					v[k] = &lod.vertices.items[st.start_vertex + tris[t + k]];
					p3[k] = mission(v[k]->position);
					Corner &c = face.corner[k];
					c.position = p3[k];
					c.normal = mission(v[k]->normal);
					c.uv = {v[k]->uv0[0], v[k]->uv0[1], v[k]->uv1[0], v[k]->uv1[1]};
					c.tangents = v[k]->has_tangents != 0;
					if (c.tangents) {
						const Vec tn = mission(v[k]->tangent), bn = mission(v[k]->bitangent);
						c.tangent = {tn[0], tn[1], tn[2], bn[0], bn[1], bn[2]};
					}
					if (m.header.mesh_type == THREEDI_MESH_SKINNED) skin_blend(c, *v[k], st);
				}
				g.faces.push_back(std::move(face));
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

// The DRIFT of matched render corners: whatever moved within tolerance, and
// the tangent frames, which the builder derives by a heuristic.
void render_drift(Diff &d, const std::string &label, const Geometry &x, const Geometry &y,
		const std::vector<Pairing> &pairs) {
	for (const Pairing &p : pairs)
		for (int k = 0; k < 3; ++k) {
			const Corner &a = x.faces[p.a].corner[k], &b = y.faces[p.b].corner[(k + p.shift) % 3];
			within(d, "render corner positions (m)", label, gap(a.position, b.position), kPosTol, kPosNoise);
			// A zero-length stored normal (retail ships them) has no direction
			// to keep; an importer gives it one (Blender cannot store it).
			if (zero_length(a.normal) && !zero_length(b.normal))
				d.note("zero-length vertex normals given a direction", 1.0, label, p.count);
			else
				within(d, "render vertex normals (degrees)", label, normal_gap(a.normal, b.normal), kNormalTolDeg,
						kNormalNoiseDeg);
			within(d, "render UVs", label, gap(a.uv, b.uv), kUvTol, kUvNoise);
			within(d, "skin weights", label, weight_gap(a, b), kWeightTol, kWeightNoise);
			if (gap(a.stray, b.stray) > kWeightNoise)
				d.note("weights on slots past a strip's bone table (no part to name; compared without them)",
						gap(a.stray, b.stray), label, p.count);
			if (a.tangents && b.tangents) {
				const double g = gap(a.tangent, b.tangent);
				if (g > kTangentNoise)
					d.note("tangent/bitangent values (the builder derives them by the OED rule; retail's tool is unwitnessed)", g,
							label, p.count);
			}
		}
}

void compare_geometry(Diff &d, const std::string &where, const std::map<std::string, Geometry> &a,
		const std::map<std::string, Geometry> &b) {
	const Tolerance t{kPosTol, kNormalTolDeg, kUvTol, kWeightTol, -1.0, true};
	for (const auto &kv : a) {
		const auto it = b.find(kv.first);
		const std::string label = where + " " + kv.first.substr(0, kv.first.find(' ', 5)) + " [" +
				kv.first.substr(kv.first.find(' ', 5) + 1) + "]";
		if (it == b.end()) {
			d.add(label + ": missing (" + std::to_string(kv.second.triangles) + " triangles)");
			continue;
		}
		const Geometry &x = kv.second, &y = it->second;
		std::vector<Pairing> pairs;
		std::string why;
		if (match_faces(x.faces, y.faces, t, pairs, why)) {
			render_drift(d, label, x, y, pairs);
			continue;
		}
		d.add(label + ": triangle corners differ: " + why);
		// What moved, in aggregate.
		const double scale = std::max(1.0, x.area);
		if (std::fabs(x.area - y.area) > kPosTol * scale) d.add(label + ": area " + num(x.area) + " vs " + num(y.area));
		if (gap(x.centroid, y.centroid) > kPosTol) d.add(label + ": centroid " + vs(x.centroid) + " vs " + vs(y.centroid));
		if (gap(x.normal, y.normal) > kPosTol * scale)
			d.add(label + ": face normal sum " + vs(x.normal) + " vs " + vs(y.normal));
		if (x.agree != y.agree)
			d.add(label + ": " + std::to_string(x.agree) + " vs " + std::to_string(y.agree) +
					" triangles wound with their vertex normals (winding)");
		if (gap(x.vnormal, y.vnormal) > kDirTol) d.add(label + ": vertex normals " + vs(x.vnormal) + " vs " + vs(y.vnormal));
		if (gap(x.uv0[0], y.uv0[0]) > kUvTol || gap(x.uv0[1], y.uv0[1]) > kUvTol)
			d.add(label + ": uv0 mean (" + num(x.uv0[0]) + " " + num(x.uv0[1]) + ") vs (" + num(y.uv0[0]) + " " +
					num(y.uv0[1]) + ")");
		if (gap(x.uv1[0], y.uv1[0]) > kUvTol || gap(x.uv1[1], y.uv1[1]) > kUvTol)
			d.add(label + ": uv1 mean (" + num(x.uv1[0]) + " " + num(x.uv1[1]) + ") vs (" + num(y.uv1[0]) + " " +
					num(y.uv1[1]) + ")");
		if (x.triangles > 0 && y.triangles > 0 && (gap(x.mn, y.mn) > kPosTol || gap(x.mx, y.mx) > kPosTol))
			d.add(label + ": bounds " + vs(x.mn) + ".." + vs(x.mx) + " vs " + vs(y.mn) + ".." + vs(y.mx));
	}
	for (const auto &kv : b)
		if (!a.count(kv.first))
			d.add(where + " " + kv.first + ": extra (" + std::to_string(kv.second.triangles) + " triangles)");
}

// Parts: hierarchy, pivot, the rel offset the pose builder reads
// (entity_pose.cpp) and the bound sphere. The runtime reads a part's sphere
// only for a model without GHDR, whose radius the LOD 0 spheres stand in for
// (model_geometry.cpp model_bound_radius_q16_from_3di, which the husk pieces
// read too); with GHDR on both sides (`headed`), nothing reads it, so a
// sphere that moved is DRIFT. The builder derives rel and the sphere;
// retail's rule for the sphere is the farthest vertex from the box centre.
void compare_parts(Diff &d, const std::string &where, const ThreediLod &x, const ThreediLod &y, bool headed) {
	for (size_t p = 0; p < x.render_object_count; ++p) {
		const ThreediRenderObject &px = x.render_objects[p], &py = y.render_objects[p];
		const std::string w = where + " part " + std::to_string(p);
		if (px.parent_index != py.parent_index)
			d.add(w + ": parent " + std::to_string(px.parent_index) + " vs " + std::to_string(py.parent_index));
		if (!within(d, "part pivots (m)", w, gap(mission(px.abs), mission(py.abs)), kPlaceTol, kPlaceNoise))
			d.add(w + ": pivot " + vs(mission(px.abs)) + " vs " + vs(mission(py.abs)));
		if (!within(d, "part rel offsets (m)", w, gap(mission(px.rel), mission(py.rel)), kPlaceTol, kPlaceNoise))
			d.add(w + ": rel offset " + vs(mission(px.rel)) + " vs " + vs(mission(py.rel)));
		const double centre_gap = gap(mission(px.bounding_center), mission(py.bounding_center));
		const double radius_gap = gap(px.bounding_radius, py.bounding_radius);
		if (headed) {
			const double moved = std::max(centre_gap, radius_gap);
			if (moved > kPlaceNoise) d.note("part bound spheres (m; nothing reads them beside a GHDR radius)", moved, w);
			continue;
		}
		const bool centre_same = within(d, "part bound spheres (m)", w, centre_gap, kPlaceTol, kPlaceNoise);
		const bool radius_same = within(d, "part bound spheres (m)", w, radius_gap, kPlaceTol, kPlaceNoise);
		if (!centre_same || !radius_same)
			d.add(w + ": bound sphere " + vs(mission(px.bounding_center)) + " r " + num(px.bounding_radius) + " vs " +
					vs(mission(py.bounding_center)) + " r " + num(py.bounding_radius));
	}
}

// ---------------------------------------------------------------------------
// PANM.

std::string track_key(const Threedi3di3 &m, const ThreediTransform &t) {
	if (t.control == 0 && t.control_param == 0 && t.rate == 0 && t.start == 0 && t.end == 0) return "-";
	std::string param = threedi_generator_names_register(t.control) ? register_label(m, t.control_param)
																	 : std::to_string(t.control_param);
	return std::to_string(t.control) + " " + param + " " + std::to_string(t.rate) + " " + std::to_string(t.start) + " " +
			std::to_string(t.end);
}

// Whether the loader keeps a LOD's PANM table: only when some row sets a
// scale, rotation or translate type; otherwise the render model carries no
// table at all and the part matrices pass through unposed [orig:
// GPM_LoadRenderModel @ 0x5B5450..0x5B5471, the type-byte scan that skips
// the allocation; Model_TransformBoneMatrices @ 0x58E390 tests the table].
bool panm_table_kept(const ThreediLod &l) {
	for (size_t i = 0; i < l.part_animation_count; ++i) {
		const uint32_t f = l.part_animations[i].flags;
		if (threedi_panm_scale_type(f) != 0 || threedi_panm_rotation_type(f) != 0 || threedi_panm_translate_type(f) != 0)
			return true;
	}
	return false;
}

// Rows in row order, never keyed by part: the runtime computes row i's matrix
// for part subobject_index from row i's basis and the row its parent names
// (threedi_panm_matrices.cpp) and poses a part by its LAST row
// (threedi_panm_pose.cpp), so row order, count and duplicates all change
// the pose. Every retail table is canonical (row i transforms part i: all
// 1,916 in the JO corpus). A table no row of which animates is never read
// (panm_table_kept), whatever its rows say: the 251 JOTAC tables that stop
// short of their LOD's parts (DRGVLA's one row for two parts) are all such.
void compare_panm(Diff &d, const std::string &where, const Threedi3di3 &a, const ThreediLod &la, const Threedi3di3 &b,
		const ThreediLod &lb) {
	if (!panm_table_kept(la) && !panm_table_kept(lb)) return;
	if (la.part_animation_count != lb.part_animation_count)
		d.add(where + ": " + std::to_string(la.part_animation_count) + " vs " + std::to_string(lb.part_animation_count) +
				" panm rows");
	for (size_t i = 0; i < std::min(la.part_animation_count, lb.part_animation_count); ++i) {
		const ThreediPartAnimation &x = la.part_animations[i], &y = lb.part_animations[i];
		const std::string w = where + " panm row " + std::to_string(i);
		if (x.subobject_index != y.subobject_index)
			d.add(w + ": transforms part " + std::to_string(x.subobject_index) + " vs " + std::to_string(y.subobject_index));
		if (x.parent_subobject != y.parent_subobject)
			d.add(w + ": parent " + std::to_string(x.parent_subobject) + " vs " + std::to_string(y.parent_subobject));
		if (x.flags != y.flags) {
			char buf[64];
			std::snprintf(buf, sizeof(buf), ": flags 0x%08x vs 0x%08x", x.flags, y.flags);
			d.add(w + buf);
		}
		const auto tx = threedi_panm_tracks(x), ty = threedi_panm_tracks(y);
		for (int t = 0; t < THREEDI_PANM_TRACK_COUNT; ++t) {
			const std::string kx = track_key(a, *tx[t]), ky = track_key(b, *ty[t]);
			if (kx != ky) d.add(w + " " + threedi_panm_track_label(t) + ": " + kx + " vs " + ky);
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
		if (!within(d, "panm rotation frames", w, gap(frame(a, x), frame(b, y)), kDirTol, kDirNoise))
			d.add(w + ": rotation frame differs");
	}
}

// ---------------------------------------------------------------------------
// Collision.

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
				for (const Vec &o : out) dup = dup || gap(o, p) <= 1e-4;
				if (inside && !dup) out.push_back(p);
			}
	return out;
}

// The largest signed distance of `p` outside a plane list.
double outside(const Vec &p, const std::vector<ThreediBoundingPlane> &planes) {
	double worst = -std::numeric_limits<double>::infinity();
	for (const ThreediBoundingPlane &q : planes) {
		const double s = q.normal[0] * p[0] + q.normal[1] * p[1] + q.normal[2] * p[2] + q.radius;
		if (std::isnan(s)) return std::numeric_limits<double>::infinity();
		worst = std::max(worst, s);
	}
	return worst;
}

// How much further the worst corner of solid `x` lies outside `y`'s planes
// than outside its own (polytope keeps corners up to 1e-3 out): two convex
// solids are the same when each one's corners lie inside the other.
double outside(const std::vector<Vec> &corners, const std::vector<ThreediBoundingPlane> &own,
		const std::vector<ThreediBoundingPlane> &other) {
	double worst = 0.0;
	for (const Vec &p : corners) worst = std::max(worst, outside(p, other) - std::max(0.0, outside(p, own)));
	return worst;
}

struct Section {
	std::map<std::pair<int, uint32_t>, std::vector<Face>> faces;  // (surface, flags) -> bullet faces
	int parent = 0;
	Vec offset{0, 0, 0};
	bool sphere = false;
	std::array<double, 6> box{};  // min, max
	Vec med{0, 0, 0};
	double radius = 0;
	int count = 0;
	std::map<int, int> poly;
	int ccw = 0;  // faces wound counter-clockwise about their stored normal
	double area = 0;
	Vec normal{0, 0, 0};
	struct Volume {
		int type, flags;
		std::array<double, 6> box;
		Vec facing;  // plane 0's normal: a ladder's facing
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
	// The caller checked threedi_3di3_collision_is_runtime_safe: every run
	// and local index below stays inside its pool.
	size_t v = 0, f = 0, vol = 0, pl = 0, nrm = 0;
	for (size_t o = 0; o < c.object_count; ++o) {
		const ThreediCollisionObject &co = c.objects[o];
		Section s;
		s.parent = co.parent_subobject_index;
		s.offset = q16v(co.offset);
		s.sphere = co.num_vertices == 0 && co.num_bounding_volumes == 0 && co.min[0] <= co.max[0];
		const Vec mn = q16v(co.min), mx = q16v(co.max);
		s.box = {mn[0], mn[1], mn[2], mx[0], mx[1], mx[2]};
		s.med = q16v(co.med);
		s.radius = co.radius * kQ16;
		s.count = co.num_faces;
		for (int k = 0; k < co.num_faces; ++k, ++f) {
			const ThreediCollisionFace &fc = c.faces[f];
			++s.poly[fc.poly_type];
			const float *p[3];
			for (int i = 0; i < 3; ++i) p[i] = c.vertices[v + fc.vert_index[i]].position;
			const ThreediCollisionNormal *sn = fc.normal_index >= 0 ? &c.normals[nrm + fc.normal_index] : nullptr;
			Face face;
			for (int i = 0; i < 3; ++i) {
				face.corner[i].position = {p[i][0], p[i][1], p[i][2]};
				if (sn != nullptr) face.corner[i].normal = {sn->normal[0], sn->normal[1], sn->normal[2]};
			}
			face.plane = fc.plane_dist_fp16 * kQ16;
			face.axis = sn != nullptr ? sn->dominate_axis : -1;
			// A face normal is only as exact as the corners it was taken from
			// define it: one 8.8 step across the face's smallest altitude (a
			// 2 cm sliver's normal is uncertain by 11 degrees, a metre-wide
			// face's by 0.4), capped so a flipped face never matches.
			double longest = 0.0, lever = 0.0;
			for (int i = 0; i < 3; ++i) {
				const float *q = p[(i + 1) % 3];
				longest = std::max(longest, std::sqrt(double(q[0] - p[i][0]) * (q[0] - p[i][0]) +
													 double(q[1] - p[i][1]) * (q[1] - p[i][1]) +
													 double(q[2] - p[i][2]) * (q[2] - p[i][2])));
				lever = std::max(lever, double(std::fabs(p[i][0])) + std::fabs(p[i][1]) + std::fabs(p[i][2]));
			}
			const Vec e0{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
			const Vec e1{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
			const double twice_area =
					length(Vec{e0[1] * e1[2] - e0[2] * e1[1], e0[2] * e1[0] - e0[0] * e1[2], e0[0] * e1[1] - e0[1] * e1[0]});
			const double altitude = longest > 0.0 ? twice_area / longest : 0.0;
			face.normal_slack = std::min(90.0, std::atan2(2.0 * kQ8, altitude) * 180.0 / kPi);
			const Vec &stored = face.corner[0].normal;
			const double n1 = has_nan(stored) ? 0.0 : std::fabs(stored[0]) + std::fabs(stored[1]) + std::fabs(stored[2]);
			face.plane_slack = 2.0 * kQ8 * n1 + kQ14 * lever + kQ16;
			face.box = {fc.min_x_fp16 * kQ16, fc.min_y_fp16 * kQ16, fc.min_z_fp16 * kQ16,
					fc.max_x_fp16 * kQ16, fc.max_y_fp16 * kQ16, fc.max_z_fp16 * kQ16};
			s.faces[{fc.poly_type, fc.material_flags}].push_back(std::move(face));
			const Vec e{p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
			const Vec g{p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
			const Vec n{e[1] * g[2] - e[2] * g[1], e[2] * g[0] - e[0] * g[2], e[0] * g[1] - e[1] * g[0]};
			s.area += 0.5 * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
			for (int k2 = 0; k2 < 3; ++k2) s.normal[k2] += 0.5 * n[k2];
			if (sn != nullptr && n[0] * sn->normal[0] + n[1] * sn->normal[1] + n[2] * sn->normal[2] > 0) ++s.ccw;
		}
		v += static_cast<size_t>(co.num_vertices);
		nrm += static_cast<size_t>(co.num_normals);
		for (int k = 0; k < co.num_bounding_volumes; ++k, ++vol) {
			const ThreediBoundingVolume &bv = c.volumes[vol];
			Section::Volume x;
			x.type = bv.collidable_type;
			x.flags = bv.flags;
			x.box = {bv.min_x_fp16 * kQ16, bv.min_y_fp16 * kQ16, bv.min_z_fp16 * kQ16,
					bv.max_x_fp16 * kQ16, bv.max_y_fp16 * kQ16, bv.max_z_fp16 * kQ16};
			const int count = bv.plane_count;
			x.corners = polytope(c.planes + pl, count);
			x.facing = {c.planes[pl].normal[0], c.planes[pl].normal[1], c.planes[pl].normal[2]};
			x.planes.assign(c.planes + pl, c.planes + pl + count);
			x.seams = 0;
			for (int q = 0; q < count; ++q) x.seams += c.planes[pl + q].flags != 0;
			pl += static_cast<size_t>(count);
			s.volumes.push_back(x);
		}
		out.push_back(s);
	}
	return out;
}

// The DRIFT of matched bullet faces: corners, normals and plane distances
// within a quantum or two, and face boxes (DIFFERENT only past two steps: the
// box comes from the unquantized corners the 8.8 ones were truncated from).
// Returns the faces whose box moved further, and the worst move.
std::pair<long, double> bullet_drift(Diff &d, const std::string &w, const std::vector<Face> &x,
		const std::vector<Face> &y, const std::vector<Pairing> &pairs) {
	std::pair<long, double> boxes{0, 0.0};
	for (const Pairing &p : pairs) {
		const Face &a = x[p.a], &b = y[p.b];
		for (int k = 0; k < 3; ++k) {
			const Corner &ca = a.corner[k], &cb = b.corner[(k + p.shift) % 3];
			within(d, "bullet-face corners (m, 8.8)", w, gap(ca.position, cb.position), kQuantumTol, kQuantumNoise);
		}
		within(d, "bullet-face normals (degrees)", w, normal_gap(a.corner[0].normal, b.corner[0].normal),
				std::max(kFaceNormalTolDeg, a.normal_slack), 1e-6);
		within(d, "bullet-face planes at the face (m)", w, plane_gap(a, b), a.plane_slack, kQuantumNoise);
		if (a.axis != b.axis) d.note("dominant axes of diagonal bullet faces (either projects the face)", 1.0, w, p.count);
		const double g = gap(a.box, b.box);
		if (!within(d, "bullet-face boxes (m)", w, g, kQuantumDistTol, kQuantumNoise)) {
			boxes.first += p.count;
			boxes.second = std::max(boxes.second, g);
		}
	}
	return boxes;
}

void compare_collision(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	if ((a.collision == nullptr) != (b.collision == nullptr)) {
		d.add("collision block: one model has none");
		return;
	}
	if (a.collision == nullptr) return;
	// CMDL: the box and radii the entity bound reads. The builder derives
	// them from the bullet faces and LOD 0's triangles.
	{
		const ThreediCollisionModelData &x = a.collision->model_data, &y = b.collision->model_data;
		std::array<double, 6> bx{}, by{};
		for (int k = 0; k < 6; ++k) {
			bx[k] = x.bbox[k];
			by[k] = y.bbox[k];
		}
		const Vec rx{x.radii[0], x.radii[1], x.radii[2]}, ry{y.radii[0], y.radii[1], y.radii[2]};
		const bool box_same = within(d, "CMDL box (m)", "CMDL", gap(bx, by), kQuantumTol, kQuantumNoise);
		const bool radii_same = within(d, "CMDL radii (m)", "CMDL", gap(rx, ry), kQuantumDistTol, kQuantumNoise);
		if (!box_same || !radii_same)
			d.add("collision bounds (CMDL) " + vs({bx[0], bx[1], bx[2]}) + ".." + vs({bx[3], bx[4], bx[5]}) + " r " + vs(rx) +
					" vs " + vs({by[0], by[1], by[2]}) + ".." + vs({by[3], by[4], by[5]}) + " r " + vs(ry));
	}
	// CXLT: the section pivots the entity reads (model_pivots_q16,
	// collision_resolve.cpp).
	{
		const ThreediCollisionModel &x = *a.collision, &y = *b.collision;
		if (x.translation_count != y.translation_count)
			d.add("collision translations (CXLT): " + std::to_string(x.translation_count) + " vs " +
					std::to_string(y.translation_count) + " rows");
		for (size_t i = 0; i < std::min(x.translation_count, y.translation_count); ++i) {
			const Vec px = q16v(x.translations[i].translation), py = q16v(y.translations[i].translation);
			const std::string w = "collision translation (CXLT) " + std::to_string(i);
			if (!within(d, "CXLT rows (m)", w, gap(px, py), kQuantumTol, kQuantumNoise))
				d.add(w + ": " + vs(px) + " vs " + vs(py));
		}
	}
	const std::vector<Section> sa = sections(a), sb = sections(b);
	if (sa.size() != sb.size()) {
		d.add("collision: " + std::to_string(sa.size()) + " vs " + std::to_string(sb.size()) + " sections");
		return;
	}
	const Tolerance t{kQuantumTol, kFaceNormalTolDeg, 0.0, 0.0, 0.0, false};
	for (size_t i = 0; i < sa.size(); ++i) {
		const Section &x = sa[i], &y = sb[i];
		const std::string w = "collision section " + std::to_string(i);
		// Bullet faces per (surface, flags), each with its stored normal,
		// plane distance and dominant axis: the runtime tests n . p +
		// plane_dist and projects on the dominant axis (collision_query.cpp).
		bool faces_same = x.faces.size() == y.faces.size();
		std::string why;
		std::pair<long, double> boxes{0, 0.0};
		for (const auto &entry : x.faces) {
			const auto it = y.faces.find(entry.first);
			std::vector<Pairing> pairs;
			std::string reason;
			if (it == y.faces.end()) {
				faces_same = false;
				continue;
			}
			if (match_faces(entry.second, it->second, t, pairs, reason)) {
				const std::pair<long, double> moved = bullet_drift(d, w, entry.second, it->second, pairs);
				boxes.first += moved.first;
				boxes.second = std::max(boxes.second, moved.second);
			} else {
				faces_same = false;
				if (why.empty())
					why = " (surface " + std::to_string(entry.first.first) + " flags " + std::to_string(entry.first.second) + ": " +
							reason + ")";
			}
		}
		// The runtime culls a face by its box (collision_query.cpp).
		if (boxes.first > 0)
			d.add(w + ": " + std::to_string(boxes.first) + " bullet-face boxes moved, up to " + num(boxes.second) + " m");
		if (!faces_same) {
			d.add(w + ": bullet faces differ (corners, stored normals, plane distances, dominant axes, surfaces or flags)" + why);
			if (x.count != y.count)
				d.add(w + ": " + std::to_string(x.count) + " vs " + std::to_string(y.count) + " bullet faces");
			if (x.poly != y.poly) {
				std::string px, py;
				for (const auto &kv : x.poly) px += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
				for (const auto &kv : y.poly) py += " " + std::to_string(kv.first) + "x" + std::to_string(kv.second);
				d.add(w + ": face surfaces" + px + " vs" + py);
			}
			const double scale = std::max(1.0, x.area);
			if (std::fabs(x.area - y.area) > 1e-2 * scale) d.add(w + ": bullet-face area " + num(x.area) + " vs " + num(y.area));
			if (x.ccw != y.ccw)
				d.add(w + ": " + std::to_string(x.ccw) + " vs " + std::to_string(y.ccw) +
						" bullet faces wound counter-clockwise about their normal (winding)");
		}
		if (x.parent != y.parent) d.add(w + ": parent " + std::to_string(x.parent) + " vs " + std::to_string(y.parent));
		if (!within(d, "section offsets (m)", w, gap(x.offset, y.offset), kQuantumTol, kQuantumNoise))
			d.add(w + ": offset " + vs(x.offset) + " vs " + vs(y.offset));
		// The section box, midpoint and radius: the runtime's section cull
		// box and hit sphere (model_geometry.cpp, collision_query.cpp),
		// authored by `csphere` on a bone section.
		if (x.sphere != y.sphere) d.add(w + ": hit sphere " + (x.sphere ? "present" : "none") + " vs " + (y.sphere ? "present" : "none"));
		const bool box_same = within(d, "section boxes and midpoints (m)", w, gap(x.box, y.box), kQuantumTol, kQuantumNoise);
		const bool med_same = within(d, "section boxes and midpoints (m)", w, gap(x.med, y.med), kQuantumTol, kQuantumNoise);
		const bool radius_same = within(d, "section radii (m)", w, gap(x.radius, y.radius), kQuantumDistTol, kQuantumNoise);
		if (!box_same || !med_same || !radius_same)
			d.add(w + ": bounds " + vs({x.box[0], x.box[1], x.box[2]}) + ".." + vs({x.box[3], x.box[4], x.box[5]}) + " mid " +
					vs(x.med) + " r " + num(x.radius) + " vs " + vs({y.box[0], y.box[1], y.box[2]}) + ".." +
					vs({y.box[3], y.box[4], y.box[5]}) + " mid " + vs(y.med) + " r " + num(y.radius));
		// Volumes in any order: the same type and flags, box and solid.
		std::vector<bool> used(y.volumes.size(), false);
		for (const Section::Volume &vx : x.volumes) {
			int match = -1;
			double best = std::numeric_limits<double>::infinity();
			for (size_t j = 0; j < y.volumes.size(); ++j) {
				const Section::Volume &vy = y.volumes[j];
				if (used[j] || vx.type != vy.type || vx.flags != vy.flags || !(gap(vx.box, vy.box) <= kVolumeBoxTol)) continue;
				const double g = std::max(outside(vx.corners, vx.planes, vy.planes), outside(vy.corners, vy.planes, vx.planes));
				if (g <= kVolumeTol && g < best) {
					best = g;
					match = static_cast<int>(j);
				}
			}
			if (match < 0) {
				// The nearest volume of the same type and flags, for the reason.
				double box = std::numeric_limits<double>::infinity(), solid = 0.0;
				for (size_t j = 0; j < y.volumes.size(); ++j) {
					const Section::Volume &vy = y.volumes[j];
					if (used[j] || vx.type != vy.type || vx.flags != vy.flags || !(gap(vx.box, vy.box) < box)) continue;
					box = gap(vx.box, vy.box);
					solid = std::max(outside(vx.corners, vx.planes, vy.planes), outside(vy.corners, vy.planes, vx.planes));
				}
				d.add(w + ": volume type " + std::to_string(vx.type) + " flags " + std::to_string(vx.flags) + " box " +
						vs({vx.box[0], vx.box[1], vx.box[2]}) + ".." + vs({vx.box[3], vx.box[4], vx.box[5]}) + " has no match" +
						(std::isinf(box) ? std::string(" (none of that type and flags left)")
										 : " (the nearest one's box is off by " + num(box) + " m, its solid by " + num(solid) + " m)"));
				continue;
			}
			used[match] = true;
			const Section::Volume &vy = y.volumes[match];
			within(d, "volume boxes (m)", w, gap(vx.box, vy.box), kVolumeBoxTol, kQuantumNoise);
			within(d, "volume solids (m outside the other's planes)", w, best, kVolumeTol, kQuantumNoise);
			// The runtime reads a ladder's plane 0 as its facing.
			if (vx.type == 4 && !within(d, "ladder facings (degrees)", w, normal_gap(vx.facing, vy.facing), kFaceNormalTolDeg, 1e-6))
				d.add(w + ": ladder facing (plane 0) " + vs(vx.facing) + " vs " + vs(vy.facing));
			// Seam flags: our builder derives them by ModSuperOed's rule;
			// retail's tool is unwitnessed (3di-gp-format-re.md), so DRIFT.
			if (vx.seams != vy.seams)
				d.note("volume seam planes (the builder derives them by ModSuperOed's rule; retail's tool is unwitnessed)",
						std::abs(vx.seams - vy.seams), w);
		}
		for (size_t j = 0; j < y.volumes.size(); ++j)
			if (!used[j]) d.add(w + ": extra volume type " + std::to_string(y.volumes[j].type));
	}
}

// ---------------------------------------------------------------------------
// Occlusion.

std::vector<Face> occlusion_faces(const Threedi3di3 &m, size_t vertex, size_t plane, size_t face, int count) {
	std::vector<Face> out;
	for (int i = 0; i < count && face + i < m.occlusion_face_count; ++i) {
		const uint32_t raw = m.occlusion_faces[face + i].raw_indices;
		Face tri;
		const size_t p = plane + (raw >> 24);
		for (int k = 0; k < 3; ++k) {
			const size_t v = vertex + ((raw >> (k * 8)) & 0xFF);
			if (v < m.occlusion_vertex_count) tri.corner[k].position = mission(m.occlusion_vertices[v].position);
			if (p < m.occlusion_plane_count) tri.corner[k].normal = mission(m.occlusion_planes[p].normal);
		}
		// The plane a face takes is its normal and its d: faces on parallel
		// planes must not swap.
		if (p < m.occlusion_plane_count) tri.plane = m.occlusion_planes[p].radius;
		out.push_back(std::move(tri));
	}
	return out;
}

bool same_points(const std::vector<Vec> &a, const std::vector<Vec> &b, double tol) {
	for (const Vec &p : a) {
		bool found = false;
		for (const Vec &q : b) found = found || gap(p, q) <= tol;
		if (!found) return false;
	}
	for (const Vec &p : b) {
		bool found = false;
		for (const Vec &q : a) found = found || gap(p, q) <= tol;
		if (!found) return false;
	}
	return true;
}

void compare_occlusion(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	if (a.occlusion_object_count != b.occlusion_object_count) {
		d.add("occlusion: " + std::to_string(a.occlusion_object_count) + " vs " + std::to_string(b.occlusion_object_count) +
				" records");
		return;
	}
	const Tolerance t{kPosTol, kFaceNormalTolDeg, 0.0, 0.0, kPosTol, false};
	size_t va = 0, vb = 0, pa = 0, pb = 0, fa = 0, fb = 0;
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
		// The record's sphere, which the portal walk reads (model_geometry.cpp);
		// a record without vertices bounds nothing (retail leaves it NaN).
		const bool empty = x.num_vertices <= 0 && y.num_vertices <= 0;
		const bool pos_same = empty || within(d, "occlusion record spheres (m)", w,
				gap(mission(x.position), mission(y.position)), kPosTol, kPosNoise);
		const bool radius_same =
				empty || within(d, "occlusion record spheres (m)", w, gap(x.radius, y.radius), kPosTol, kPosNoise);
		if (!pos_same || !radius_same)
			d.add(w + ": sphere " + vs(mission(x.position)) + " r " + num(x.radius) + " vs " + vs(mission(y.position)) + " r " +
					num(y.radius));
		if (x.face_count != y.face_count || x.num_vertices != y.num_vertices)
			d.add(w + ": " + std::to_string(x.num_vertices) + "/" + std::to_string(x.face_count) + " vs " +
					std::to_string(y.num_vertices) + "/" + std::to_string(y.face_count) + " vertices/faces");
		std::vector<Vec> px, py;
		for (int k = 0; k < x.num_vertices && va + k < a.occlusion_vertex_count; ++k)
			px.push_back(mission(a.occlusion_vertices[va + k].position));
		for (int k = 0; k < y.num_vertices && vb + k < b.occlusion_vertex_count; ++k)
			py.push_back(mission(b.occlusion_vertices[vb + k].position));
		if (!same_points(px, py, kPosTol)) d.add(w + ": vertices differ");
		std::vector<Pairing> pairs;
		std::string why;
		if (!match_faces(occlusion_faces(a, va, pa, fa, x.face_count), occlusion_faces(b, vb, pb, fb, y.face_count), t, pairs,
					why))
			d.add(w + ": face corners, winding or planes differ: " + why);
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
					found = found || (gap(p[0], q[0]) <= kDirTol && gap(p[1], q[1]) <= kDirTol && gap(p[2], q[2]) <= kDirTol &&
											 gap(p[3], q[3]) <= kPosTol);
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
		fa += static_cast<size_t>(std::max(0, x.face_count));
		fb += static_cast<size_t>(std::max(0, y.face_count));
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
		if (!within(d, "light attenuation (m)", w, std::max(gap(x.atten_start, y.atten_start), gap(x.atten_end, y.atten_end)),
					kPlaceTol, kPlaceNoise))
			d.add(w + ": attenuation " + num(x.atten_start) + ".." + num(x.atten_end) + " vs " + num(y.atten_start) + ".." +
					num(y.atten_end));
		if (!within(d, "light positions (m)", w, gap(mission(x.offset), mission(y.offset)), kPlaceTol, kPlaceNoise))
			d.add(w + ": position " + vs(mission(x.offset)) + " vs " + vs(mission(y.offset)));
		if (!within(d, "light axes and cones", w,
					std::max(gap(mission(x.rotation), mission(y.rotation)), gap(x.rotation[3], y.rotation[3])), kDirTol, kDirNoise))
			d.add(w + ": axis/cone differ");
	}
}

void compare_user_points(Diff &d, const Threedi3di3 &a, const Threedi3di3 &b) {
	// In order: the attach scan masks a model's first 16 points by index.
	if (a.user_point_count != b.user_point_count)
		d.add("user points " + std::to_string(a.user_point_count) + " vs " + std::to_string(b.user_point_count));
	for (size_t i = 0; i < std::min(a.user_point_count, b.user_point_count); ++i) {
		const ThreediUserPoint &x = a.user_points[i], &y = b.user_points[i];
		const Vec px{x.x * kQ16, x.y * kQ16, x.z * kQ16}, py{y.x * kQ16, y.y * kQ16, y.z * kQ16};
		const Vec dx{x.rot_x * kQ16, x.rot_y * kQ16, x.rot_z * kQ16}, dy{y.rot_x * kQ16, y.rot_y * kQ16, y.rot_z * kQ16};
		const std::string w = "user point " + std::to_string(i);
		const bool at = within(d, "user point positions (m)", w, gap(px, py), kPlaceTol, kQuantumNoise);
		const bool facing = within(d, "user point directions", w, gap(dx, dy), kDirTol, kQuantumNoise);
		if (std::strcmp(x.name, y.name) != 0 || x.userpoint_type != y.userpoint_type ||
				x.subobject_index != y.subobject_index || !at || !facing)
			d.add(w + ": " + x.name + " " + std::to_string(x.userpoint_type) + " part " + std::to_string(x.subobject_index) +
					" " + vs(px) + " " + vs(dx) + " vs " + y.name + " " + std::to_string(y.userpoint_type) + " part " +
					std::to_string(y.subobject_index) + " " + vs(py) + " " + vs(dy));
	}
}

} // namespace

int cmd_compare(const char *expected_path, const char *actual_path, bool strict) {
	Threedi3di3 a{}, b{};
	if (threedi_3di3_read(expected_path, &a) != 0) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", expected_path);
		return 1;
	}
	if (threedi_3di3_read(actual_path, &b) != 0) {
		std::fprintf(stderr, "opennova-3di: cannot read %s\n", actual_path);
		threedi_3di3_free(&a);
		return 1;
	}
	Diff d;
	// Collision runs and local indices are read unchecked below: refuse a
	// file whose collision block the runtime would refuse too.
	if (a.collision != nullptr && !threedi_3di3_collision_is_runtime_safe(a.collision))
		d.malformed.push_back(std::string(expected_path) + ": collision block indices outside their tables");
	if (b.collision != nullptr && !threedi_3di3_collision_is_runtime_safe(b.collision))
		d.malformed.push_back(std::string(actual_path) + ": collision block indices outside their tables");
	if (std::strcmp(a.header.name, b.header.name) != 0)
		d.add(std::string("model name ") + a.header.name + " vs " + b.header.name);
	if (a.header.mesh_type != b.header.mesh_type)
		d.add("mesh type " + std::to_string(a.header.mesh_type) + " vs " + std::to_string(b.header.mesh_type));
	// GHDR's radius is the entity bound sphere (Entity_InitFromModel reads it;
	// model_geometry.cpp), derived by the builder from every LOD's vertices.
	const double ra_q = static_cast<uint32_t>(a.header.max_radius_fp16) * kQ16;
	const double rb_q = static_cast<uint32_t>(b.header.max_radius_fp16) * kQ16;
	if (!within(d, "GHDR radius (m)", "GHDR", gap(ra_q, rb_q), kQuantumTol, kQuantumNoise))
		d.add("model radius (GHDR) " + num(ra_q) + " vs " + num(rb_q));
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
		// The vertex layout (tangent frames, skin) the renderer binds.
		if (x.vertices.flags != y.vertices.flags || x.vertices.stride != y.vertices.stride) {
			char buf[96];
			std::snprintf(buf, sizeof(buf), ": vertex layout flags 0x%x stride %u vs flags 0x%x stride %u", x.vertices.flags,
					x.vertices.stride, y.vertices.flags, y.vertices.stride);
			d.add(w + buf);
		}
		compare_panm(d, w, a, x, b, y);
		if (x.render_object_count != y.render_object_count) {
			d.add(w + ": " + std::to_string(x.render_object_count) + " vs " + std::to_string(y.render_object_count) + " parts");
			continue;
		}
		compare_parts(d, w, x, y, a.header.has_header != 0 && b.header.has_header != 0);
		compare_geometry(d, w, lod_geometry(d, w, a, x), lod_geometry(d, w, b, y));
	}
	// Materials are compared through the geometry that draws with them;
	// materials no strip draws (the exporter drops them) are not compared.
	compare_user_points(d, a, b);
	compare_lights(d, a, b);
	compare_occlusion(d, a, b);
	if (d.malformed.empty()) compare_collision(d, a, b);
	threedi_3di3_free(&a);
	threedi_3di3_free(&b);
	if (!d.malformed.empty()) {
		for (const std::string &s : d.malformed) std::fprintf(stderr, "opennova-3di: malformed: %s\n", s.c_str());
		return 1;
	}
	for (const auto &kv : d.drift)
		std::printf("drift: %s: %ld values, worst %s (%s)\n", kv.first.c_str(), kv.second.count, num(kv.second.worst).c_str(),
				kv.second.where.c_str());
	for (const std::string &s : d.lines) std::printf("%s\n", s.c_str());
	const bool same = d.lines.empty() && (!strict || d.drift.empty());
	if (!d.lines.empty())
		std::printf("different: %s\n", actual_path);
	else if (!same)
		std::printf("different (--strict: drift): %s\n", actual_path);
	else if (!d.drift.empty())
		std::printf("same model, with drift: %s\n", actual_path);
	else
		std::printf("same model: %s\n", actual_path);
	return same ? 0 : 1;
}

} // namespace opennova::threedi_cli
